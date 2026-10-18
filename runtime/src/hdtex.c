/* HD texture pack: replacement files served from inside the game archive.
 *
 * The game reads all of its data from GameData.gtc.  FileList.Inf lists
 * each file's offset and size in an uncompressed address space that is cut
 * into 128 KiB chunks; Compress.Inf has one 20-byte record per chunk
 * {gtc offset, packed size, allocated size, method, files in chunk}, and a
 * chunk whose allocated size is 0x20000 is read as is (stored).  The game
 * never looks for loose files, so replacements cannot simply be dropped
 * next to it.
 *
 * Instead, when a pack directory is present (a "game data" tree like the
 * game's own), the runtime appends the pack's files to the address space as
 * stored chunks:
 *   - FileList.Inf and Compress.Inf are served from patched copies in which
 *     replaced entries point at the new chunks;
 *   - reads of GameData.gtc past its real end are filled from the pack
 *     files (ReadFile calls hd_fill when a read comes up short).
 * The originals on disk are never modified.
 */
#define _GNU_SOURCE
#include "rt.h"
#include "savestate.h"
#include "os.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <SDL3/SDL.h>

#define CHUNK 0x20000u
#define ALIGN 2048u
#define LIST_REC 136u
#define CHUNK_REC 20u

typedef struct {
    char *host;               /* pack file */
    uint32_t size;
    uint32_t voff;            /* offset in the appended region */
} HdFile;

static HdFile *s_files;       /* sorted by voff */
static uint32_t s_nfiles;
static uint64_t s_gtc_base;   /* real GameData.gtc size, rounded up to ALIGN */
static uint64_t s_gtc_size;   /* with the appended chunks */
static uint8_t *s_list, *s_chunks;
static size_t s_list_len, s_chunks_len;
static OsFileId s_id_gtc, s_id_list, s_id_chunks;
static uint64_t s_sig;        /* identifies the pack contents for savestates */

/* ------------------------------------------------------------- scanning */
typedef struct {
    char *name;               /* "GAME DATA\..." upper case */
    char *host;
    uint32_t size;
} PackEntry;

static PackEntry *s_scan;
static size_t s_nscan, s_capscan;

static void scan_dir(const char *host, const char *win) {
    DIR *d = opendir(host);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char hp[2048], wp[1024];
        snprintf(hp, sizeof hp, "%s/%s", host, e->d_name);
        snprintf(wp, sizeof wp, "%s%s%s", win, *win ? "\\" : "", e->d_name);
        struct stat st;
        if (stat(hp, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(hp, wp);
        } else if (S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 0x10000000) {
            if (s_nscan == s_capscan) {
                s_capscan = s_capscan ? s_capscan * 2 : 1024;
                s_scan = realloc(s_scan, s_capscan * sizeof *s_scan);
            }
            for (char *p = wp; *p; p++)
                *p = (char)toupper((unsigned char)*p);
            s_scan[s_nscan++] = (PackEntry){strdup(wp), strdup(hp), (uint32_t)st.st_size};
        }
    }
    closedir(d);
}

static int cmp_entry(const void *a, const void *b) {
    return strcmp(((const PackEntry *)a)->name, ((const PackEntry *)b)->name);
}

/* --------------------------------------------------------------- helpers */
static bool read_all(const char *host, uint8_t **out, size_t *len) {
    int fd = open(host, O_RDONLY);
    if (fd < 0)
        return false;
    struct stat st;
    fstat(fd, &st);
    *len = (size_t)st.st_size;
    *out = malloc(*len ? *len : 1);
    bool ok = pread(fd, *out, *len, 0) == (ssize_t)*len;
    close(fd);
    return ok;
}

static bool same_file(const OsFileId *a, const OsFileId *id) {
    return id->ino && a->dev == id->dev && a->ino == id->ino;
}

static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ------------------------------------------------------------------ init */
static bool find_pack(const char *opt, char *out, size_t sz) {
    if (opt) {
        snprintf(out, sz, "%s", opt);
    } else {
        /* Default: an "HD TEXTURES" folder next to the game files, on the
           CD, in the current directory or next to this program. */
        char base[1024];
        snprintf(base, sizeof base, "%s", SDL_GetBasePath() ? SDL_GetBasePath() : ".");
        size_t bl = strlen(base);
        if (bl > 1 && (base[bl - 1] == '/' || base[bl - 1] == '\\'))
            base[bl - 1] = 0;
        const char *roots[] = {g_game_dir, g_cd_dir, ".", base};
        bool found = false;
        for (int i = 0; i < 4 && !found; i++) {
            DIR *d = opendir(roots[i]);
            struct dirent *e;
            while (d && (e = readdir(d))) {
                if (!strcasecmp(e->d_name, "HD TEXTURES")) {
                    snprintf(out, sz, "%s/%s", roots[i], e->d_name);
                    found = true;
                    break;
                }
            }
            if (d)
                closedir(d);
        }
        if (!found)
            return false;
    }
    struct stat st;
    if (stat(out, &st) < 0 || !S_ISDIR(st.st_mode)) {
        if (opt)
            LOGW("hd: %s is not a directory", out);
        return false;
    }
    return true;
}

void hd_init(const char *opt_dir) {
    char dir[1100];
    if (!find_pack(opt_dir, dir, sizeof dir))
        return;
    char list_host[1200], chunks_host[1200], gtc_host[1200];
    path_to_host("C:\\LR2\\FileList.Inf", list_host, sizeof list_host);
    path_to_host("C:\\LR2\\Compress.Inf", chunks_host, sizeof chunks_host);
    path_to_host("C:\\LR2\\GameData.gtc", gtc_host, sizeof gtc_host);
    struct stat gst;
    if (!read_all(list_host, &s_list, &s_list_len) || !read_all(chunks_host, &s_chunks, &s_chunks_len) ||
        stat(gtc_host, &gst) < 0 || s_list_len % LIST_REC || s_chunks_len % CHUNK_REC) {
        LOGW("hd: game archive not found or not understood; HD textures disabled");
        goto fail;
    }

    scan_dir(dir, "");
    qsort(s_scan, s_nscan, sizeof *s_scan, cmp_entry);

    /* The appended region starts after the last original chunk, both in the
       address space and in the archive file. */
    uint32_t nlist = (uint32_t)(s_list_len / LIST_REC), nchunks = (uint32_t)(s_chunks_len / CHUNK_REC);
    uint64_t vbase = (uint64_t)nchunks * CHUNK;
    s_gtc_base = ((uint64_t)gst.st_size + ALIGN - 1) / ALIGN * ALIGN;
    s_files = calloc(s_nscan ? s_nscan : 1, sizeof *s_files);
    uint64_t v = 0;
    s_sig = 1469598103934665603ull;
    for (uint32_t i = 0; i < nlist; i++) {
        uint8_t *rec = s_list + i * LIST_REC;
        char name[129];
        memcpy(name, rec, 128);
        name[128] = 0;
        for (char *p = name; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        PackEntry key = {name, NULL, 0};
        PackEntry *e = bsearch(&key, s_scan, s_nscan, sizeof *s_scan, cmp_entry);
        if (!e || !e->host)
            continue;
        if (vbase + v + e->size >= 0x7fff0000u) {
            LOGW("hd: pack too large, ignoring the rest from %s", name);
            break;
        }
        s_files[s_nfiles++] = (HdFile){e->host, e->size, (uint32_t)v};
        e->host = NULL;                       /* owned by s_files now */
        wr32(rec + 128, (uint32_t)(vbase + v));
        wr32(rec + 132, e->size);
        s_sig = (s_sig ^ e->size ^ (uint64_t)i << 32) * 1099511628211ull;
        v = (v + e->size + ALIGN - 1) / ALIGN * ALIGN;
    }
    size_t unused = s_nscan - s_nfiles;
    for (size_t i = 0; i < s_nscan; i++) {
        free(s_scan[i].name);
        free(s_scan[i].host);
    }
    free(s_scan);
    s_scan = NULL;
    if (!s_nfiles) {
        LOGW("hd: no files in %s match the game's file list", dir);
        goto fail;
    }

    /* One stored chunk per 128 KiB of the appended region. */
    uint32_t nnew = (uint32_t)((v + CHUNK - 1) / CHUNK);
    s_chunks = realloc(s_chunks, s_chunks_len + (size_t)nnew * CHUNK_REC);
    uint32_t f = 0;
    for (uint32_t k = 0; k < nnew; k++) {
        uint8_t *rec = s_chunks + s_chunks_len + (size_t)k * CHUNK_REC;
        uint32_t lo = k * CHUNK, hi = lo + CHUNK, touching = 0;
        while (f < s_nfiles && s_files[f].voff + s_files[f].size <= lo)
            f++;
        for (uint32_t j = f; j < s_nfiles && s_files[j].voff < hi; j++)
            touching++;
        wr32(rec, (uint32_t)(s_gtc_base + lo));
        wr32(rec + 4, CHUNK);
        wr32(rec + 8, CHUNK);
        wr32(rec + 12, 0);
        wr32(rec + 16, touching);
    }
    s_chunks_len += (size_t)nnew * CHUNK_REC;
    s_gtc_size = s_gtc_base + (uint64_t)nnew * CHUNK;
    os_path_id(list_host, &s_id_list);
    os_path_id(chunks_host, &s_id_chunks);
    os_path_id(gtc_host, &s_id_gtc);
    LOGI("hd: %u replacement files from %s (%.0f MiB)", s_nfiles, dir, v / 1048576.0);
    if (unused)
        LOGI("hd: %zu files in the pack are not used by the game", unused);
    return;
fail:
    free(s_list);
    free(s_chunks);
    s_list = s_chunks = NULL;
    s_nfiles = 0;
}

/* ----------------------------------------------------------------- files */
int hd_open(const char *host, int flags) {
    if (s_nfiles && (flags & O_ACCMODE) == O_RDONLY) {
        OsFileId id;
        if (os_path_id(host, &id)) {
            const uint8_t *data = NULL;
            size_t len = 0;
            if (same_file(&id, &s_id_list))
                data = s_list, len = s_list_len;
            else if (same_file(&id, &s_id_chunks))
                data = s_chunks, len = s_chunks_len;
            if (data) {
                int fd = os_tmpfd();
                if (fd >= 0 && pwrite(fd, data, len, 0) == (ssize_t)len) {
                    lseek(fd, 0, SEEK_SET);
                    return fd;
                }
                if (fd >= 0)
                    close(fd);
                LOGW("hd: cannot serve %s: %s", host, strerror(errno));
            }
        }
    }
    return open(host, flags, 0644);
}

static bool is_gtc(int fd) {
    OsFileId id;
    return s_nfiles && os_fd_id(fd, &id) && same_file(&id, &s_id_gtc);
}

size_t hd_fill(int fd, uint8_t *buf, size_t n, uint64_t off) {
    if (!n || off + n <= s_gtc_base || off >= s_gtc_size || !is_gtc(fd))
        return 0;
    if (off < s_gtc_base) {
        /* Padding between the real end and the first appended chunk. */
        size_t pad = (size_t)(s_gtc_base - off);
        memset(buf, 0, pad);
        return pad + hd_fill(fd, buf + pad, n - pad, s_gtc_base);
    }
    if (off + n > s_gtc_size)
        n = (size_t)(s_gtc_size - off);
    memset(buf, 0, n);
    uint64_t v0 = off - s_gtc_base, v1 = v0 + n;
    /* First file that ends after v0. */
    uint32_t lo = 0, hi = s_nfiles;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if ((uint64_t)s_files[mid].voff + s_files[mid].size <= v0)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (uint32_t i = lo; i < s_nfiles && s_files[i].voff < v1; i++) {
        const HdFile *f = &s_files[i];
        uint64_t a = v0 > f->voff ? v0 : f->voff, b = v1 < (uint64_t)f->voff + f->size ? v1 : (uint64_t)f->voff + f->size;
        int pfd = open(f->host, O_RDONLY);
        if (pfd < 0 || pread(pfd, buf + (a - v0), (size_t)(b - a), (off_t)(a - f->voff)) != (ssize_t)(b - a))
            LOGW("hd: cannot read %s", f->host);
        if (pfd >= 0)
            close(pfd);
    }
    return n;
}

bool hd_size(int fd, uint64_t *size) {
    if (!is_gtc(fd))
        return false;
    *size = s_gtc_size;
    return true;
}

/* ------------------------------------------------------------- savestate */
/* The game keeps the file list in guest memory, so a state saved with one
   pack only makes sense with the same pack. */
static void hd_save(SnapBuf *b) {
    sb_u32(b, s_nfiles);
    SB_PUT(b, s_sig);
}

static bool hd_check(SnapBuf *b) {
    uint32_t n = sb_get_u32(b);
    uint64_t sig = 0;
    SB_GET(b, sig);
    if (b->err || n != s_nfiles || (n && sig != s_sig)) {
        LOGW("savestate: the state was saved %s", !s_nfiles ? "with HD textures, which are not enabled now"
                                                   : !n ? "without HD textures (load it with --no-hd)"
                                                        : "with a different HD texture pack");
        return false;
    }
    return true;
}

static bool hd_load(SnapBuf *b) { return hd_check(b); }

__attribute__((constructor)) static void hd_register(void) {
    static const SnapSection sec = {SNAP_TAG('H', 'D', 'T', 'X'), SNAP_ORDER_KERNEL, hd_save, hd_check, NULL, hd_load};
    snap_section(&sec);
}
