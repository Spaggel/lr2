/* First-run setup: copy the game files from the player's own CD (a disc
 * image, the disc itself or a copy of its files) into game/ next to the
 * program.
 *
 * The game's assets are copied in the CD's layout; the installer, its
 * redistributables, the manual and every .exe (the game itself is the
 * recompiled build) are left out.  GAMEDATA.GTC is copied last, under a
 * temporary name, so its presence means a complete copy.
 */
#include "rt.h"
#include <SDL3/SDL.h>

typedef struct {
    char *rel;       /* destination path below the CD dir, '/'-separated */
    char *src;       /* source file (folder copies) */
    uint32_t lba;    /* first sector (disc images) */
    uint64_t size;
} Entry;

static Entry *s_ents;
static int s_nents, s_cap;
static uint64_t s_total;

/* Everything but the installer and extras: the game reads install/ (data,
   movies, music), language/ and ld/ from the CD. */
static bool wanted(const char *rel) {
    static const char *skip[] = {"DCOM95/", "eReg/", "Manual/", "install/redist/"};
    for (size_t i = 0; i < SDL_arraysize(skip); i++)
        if (!SDL_strncasecmp(rel, skip[i], SDL_strlen(skip[i])))
            return false;
    size_t n = SDL_strlen(rel);
    return !(n > 4 && !SDL_strcasecmp(rel + n - 4, ".exe"));
}

static void add(const char *rel, const char *src, uint32_t lba, uint64_t size) {
    if (!wanted(rel))
        return;
    if (s_nents == s_cap) {
        s_cap = s_cap ? s_cap * 2 : 256;
        s_ents = SDL_realloc(s_ents, (size_t)s_cap * sizeof *s_ents);
    }
    s_ents[s_nents++] = (Entry){SDL_strdup(rel), src ? SDL_strdup(src) : NULL, lba, size};
    s_total += size;
}

/* ------------------------------------------------------------ disc image */
static SDL_IOStream *s_iso;

static bool iso_read(uint32_t lba, void *buf, size_t n) {
    return SDL_SeekIO(s_iso, (Sint64)lba * 2048, SDL_IO_SEEK_SET) >= 0 && SDL_ReadIO(s_iso, buf, n) == n;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void iso_walk(uint32_t lba, uint32_t size, const char *dir, bool joliet, int depth) {
    uint8_t *d = SDL_malloc(size);
    if (depth > 16 || !d || !iso_read(lba, d, size)) {
        SDL_free(d);
        return;
    }
    for (uint32_t i = 0; i < size;) {
        uint8_t len = d[i];
        if (!len) {             /* records do not cross sector boundaries */
            i = (i / 2048 + 1) * 2048;
            continue;
        }
        const uint8_t *r = d + i;
        i += len;
        if (len < 34 || r[32] + 33u > len || (r[32] == 1 && r[33] <= 1))   /* ".", ".." */
            continue;
        char *name = joliet ? SDL_iconv_string("UTF-8", "UCS-2BE", (const char *)r + 33, r[32])
                            : SDL_strndup((const char *)r + 33, r[32]);
        if (!name)
            continue;
        char *semi = SDL_strchr(name, ';');
        if (semi)
            *semi = 0;
        size_t nl = SDL_strlen(name);
        if (!joliet && nl && name[nl - 1] == '.')
            name[nl - 1] = 0;
        char rel[1024];
        SDL_snprintf(rel, sizeof rel, "%s%s%s", dir, *dir ? "/" : "", name);
        SDL_free(name);
        if (r[25] & 2)
            iso_walk(le32(r + 2), le32(r + 10), rel, joliet, depth + 1);
        else
            add(rel, NULL, le32(r + 2), le32(r + 10));
    }
    SDL_free(d);
}

static bool list_iso(const char *path) {
    s_iso = SDL_IOFromFile(path, "rb");
    if (!s_iso)
        return false;
    /* Prefer the Joliet tree (long names) over the plain ISO9660 one. */
    uint8_t vd[2048], root[34];
    bool found = false, joliet = false;
    for (uint32_t lba = 16; lba < 64 && iso_read(lba, vd, sizeof vd) && vd[0] != 255; lba++) {
        if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && SDL_strchr("@CE", vd[90])) {
            SDL_memcpy(root, vd + 156, 34);
            found = joliet = true;
            break;
        }
        if (vd[0] == 1 && !found) {
            SDL_memcpy(root, vd + 156, 34);
            found = true;
        }
    }
    if (found)
        iso_walk(le32(root + 2), le32(root + 10), "", joliet, 0);
    return found;
}

/* ---------------------------------------------------------------- folder */
typedef struct {
    const char *rel;
    int depth;
} WalkCtx;

static SDL_EnumerationResult SDLCALL dir_entry(void *ud, const char *dir, const char *name) {
    const WalkCtx *w = ud;
    char src[1024], rel[1024];
    SDL_snprintf(src, sizeof src, "%s%s", dir, name);
    SDL_snprintf(rel, sizeof rel, "%s%s%s", w->rel, *w->rel ? "/" : "", name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(src, &info))
        return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY && w->depth < 16) {
        WalkCtx sub = {rel, w->depth + 1};
        SDL_EnumerateDirectory(src, dir_entry, &sub);
    } else if (info.type == SDL_PATHTYPE_FILE) {
        add(rel, src, 0, info.size);
    }
    return SDL_ENUM_CONTINUE;
}

/* The CD's root folder: the one with install/ in it. */
static bool list_folder(const char *path) {
    WalkCtx w = {"", 0};
    return SDL_EnumerateDirectory(path, dir_entry, &w);
}

/* --------------------------------------------------------------- copying */
#define WIN_W 800
#define WIN_H 340

/* What the player starts, for the setup window's text. */
#if defined(_WIN32)
#define PROGRAM_NAME "lr2.exe"
#elif defined(__APPLE__)
#define PROGRAM_NAME "the LEGO Racers 2 app"
#else
#define PROGRAM_NAME "lr2"
#endif
#define LINE_CHARS 48   /* debug font: 8 px, drawn at 2x */

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static bool s_quit;

/* Clears the window and draws text lines (wrapped), centred. */
static void draw_text(const char *text, int y) {
    SDL_SetRenderDrawColor(s_ren, 0x10, 0x30, 0x90, 255);
    SDL_RenderClear(s_ren);
    SDL_SetRenderScale(s_ren, 2, 2);
    for (const char *p = text; *p; y += 12) {
        size_t n = strcspn(p, "\n");
        if (n > LINE_CHARS) {
            n = LINE_CHARS;
            while (n > 1 && p[n] != ' ')
                n--;
        }
        char line[LINE_CHARS + 1];
        SDL_snprintf(line, sizeof line, "%.*s", (int)n, p);
        SDL_SetRenderDrawColor(s_ren, 255, 255, 255, 255);
        SDL_RenderDebugText(s_ren, (WIN_W / 2 - 8.0f * (float)SDL_strlen(line)) / 2, (float)y, line);
        p += n;
        if (*p == '\n' || *p == ' ')
            p++;
    }
    SDL_SetRenderScale(s_ren, 1, 1);
}

/* Draws the progress; false if the player closed the window. */
static bool progress(uint64_t done) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            s_quit = true;
    if (s_quit)
        return false;
    int pct = s_total ? (int)(done * 100 / s_total) : 0;
    char msg[64];
    SDL_snprintf(msg, sizeof msg, "Copying the game files... %d%%", pct);
    draw_text(msg, 40);
    SDL_FRect bar = {100, 160, WIN_W - 200, 30};
    SDL_RenderRect(s_ren, &bar);
    bar.w = (WIN_W - 200) * (float)pct / 100.0f;
    SDL_SetRenderDrawColor(s_ren, 0xff, 0xd0, 0x20, 255);
    SDL_RenderFillRect(s_ren, &bar);
    SDL_RenderPresent(s_ren);
    return true;
}

static bool copy_one(const Entry *e, const char *dest, uint64_t *done) {
    SDL_IOStream *in = e->src ? SDL_IOFromFile(e->src, "rb") : s_iso;
    if (!in || (!e->src && SDL_SeekIO(in, (Sint64)e->lba * 2048, SDL_IO_SEEK_SET) < 0))
        return false;
    SDL_IOStream *out = SDL_IOFromFile(dest, "wb");
    static uint8_t buf[1 << 20];
    bool ok = out != NULL;
    for (uint64_t left = e->size; ok && left;) {
        size_t n = left < sizeof buf ? (size_t)left : sizeof buf;
        ok = SDL_ReadIO(in, buf, n) == n && SDL_WriteIO(out, buf, n) == n && progress(*done += n);
        left -= n;
    }
    if (out && !SDL_CloseIO(out))
        ok = false;
    if (e->src)
        SDL_CloseIO(in);
    return ok;
}

static bool is_gtc(const Entry *e) { return !SDL_strcasecmp(e->rel, "install/GAMEDATA.GTC"); }

static bool copy_all(const char *dest_cd) {
    int gtc = -1;
    for (int i = 0; i < s_nents; i++)
        if (is_gtc(&s_ents[i]))
            gtc = i;
    if (gtc < 0)
        return SDL_SetError("no GAMEDATA.GTC in its install folder");
    Entry last = s_ents[gtc];
    s_ents[gtc] = s_ents[s_nents - 1];
    s_ents[s_nents - 1] = last;
    uint64_t done = 0;
    for (int i = 0; i < s_nents; i++) {
        char dest[1100], tmp[1110];
        SDL_snprintf(dest, sizeof dest, "%s/%s", dest_cd, s_ents[i].rel);
        *SDL_strrchr(dest, '/') = 0;
        SDL_CreateDirectory(dest);
        SDL_snprintf(dest, sizeof dest, "%s/%s", dest_cd, s_ents[i].rel);
        SDL_snprintf(tmp, sizeof tmp, "%s.partial", dest);
        bool last_one = i == s_nents - 1;
        if (!copy_one(&s_ents[i], last_one ? tmp : dest, &done))
            return SDL_SetError("could not copy %s", s_ents[i].rel);
        if (last_one && !SDL_RenamePath(tmp, dest))
            return false;
    }
    return true;
}

static void reset(void) {
    for (int i = 0; i < s_nents; i++) {
        SDL_free(s_ents[i].rel);
        SDL_free(s_ents[i].src);
    }
    SDL_free(s_ents);
    s_ents = NULL;
    s_nents = s_cap = 0;
    s_total = 0;
    if (s_iso)
        SDL_CloseIO(s_iso);
    s_iso = NULL;
}

static bool install_from(const char *src, const char *dest_cd) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(src, &info))
        return false;
    bool listed = info.type == SDL_PATHTYPE_DIRECTORY ? list_folder(src) : list_iso(src);
    bool ok = listed ? copy_all(dest_cd) : SDL_SetError("not a disc image or a folder");
    reset();
    return ok;
}

/* ------------------------------------------------------- the setup window */
static char *s_picked;
static SDL_AtomicInt s_answered;   /* set from the dialog's thread on Windows */

static void SDLCALL picked(void *ud, const char *const *files, int filter) {
    (void)ud;
    (void)filter;
    s_picked = files && files[0] ? SDL_strdup(files[0]) : NULL;
    SDL_SetAtomicInt(&s_answered, 1);
}

static char *pick_iso(void) {
    static const SDL_DialogFileFilter filters[] = {{"Disc images (*.iso)", "iso"}, {"All files", "*"}};
    s_picked = NULL;
    SDL_SetAtomicInt(&s_answered, 0);
    SDL_ShowOpenFileDialog(picked, NULL, s_win, filters, 2, NULL, false);
    /* On Windows the dialog runs on another thread and waits on its owner,
       this window, so its messages must keep flowing: take the events off
       the queue (waiting with a NULL event only peeks, and stops pumping
       once anything is queued). */
    SDL_Event e;
    while (!SDL_GetAtomicInt(&s_answered))
        if (SDL_WaitEventTimeout(&e, 50) && (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED))
            s_quit = true;
    /* The click that closed the dialog must not open it again. */
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_WHEEL);
    return s_picked;
}

static SDL_EnumerationResult SDLCALL iso_entry(void *ud, const char *dir, const char *name) {
    size_t n = SDL_strlen(name);
    if (n > 4 && !SDL_strcasecmp(name + n - 4, ".iso")) {
        SDL_asprintf((char **)ud, "%s%s", dir, name);
        return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

/* A disc image the player put next to the game. */
static char *iso_nearby(const char *dir) {
    char *found = NULL;
    SDL_EnumerateDirectory(dir, iso_entry, &found);
    return found;
}

/* Copies the game files into <prog_dir>/game (the CD layout: game/install/),
   from src, from a disc image next to the program, or from whatever the
   player drops on the setup window.  False if the player quit. */
bool setup_run(const char *src, const char *prog_dir) {
    if (!SDL_Init(SDL_INIT_VIDEO))
        rt_fatal("SDL_Init: %s", SDL_GetError());
    if (!SDL_CreateWindowAndRenderer("LEGO Racers 2 setup", WIN_W, WIN_H, 0, &s_win, &s_ren))
        rt_fatal("setup window: %s", SDL_GetError());
    SDL_SetRenderVSync(s_ren, 1);
    char dest_cd[1100], status[1200] = "";
    SDL_snprintf(dest_cd, sizeof dest_cd, "%s/game", prog_dir);
    /* macOS runs a downloaded app that was not moved from a read-only copy. */
    bool translocated = SDL_strstr(prog_dir, "/AppTranslocation/") != NULL;
    char *path = src ? SDL_strdup(src) : translocated ? NULL : iso_nearby(prog_dir);
    bool ok = false;
    while (!s_quit) {
        if (path) {
            LOGI("setup: copying game files from %s to %s", path, dest_cd);
            ok = install_from(path, dest_cd);
            if (ok || s_quit)
                break;
            SDL_snprintf(status, sizeof status, "That did not work: %s. Try the .iso file, or the folder that contains \"install\".",
                         SDL_GetError());
            LOGE("setup: %s: %s", path, SDL_GetError());
            SDL_free(path);
            path = NULL;
        }
        char text[1600];
        if (translocated)
            SDL_snprintf(text, sizeof text,
                         "macOS keeps LEGO Racers 2 in quarantine, so it cannot save anything.\n\n"
                         "Close this window, open Terminal, type: xattr -cr\n"
                         "and a space, drag the LEGO Racers 2 app into the Terminal window, "
                         "press Enter, and open the game again.");
        else
            SDL_snprintf(text, sizeof text,
                     "LEGO Racers 2 needs the files from your own CD.\n\n"
                     "Drag your disc image (.iso), the CD, or a folder with the CD's files into this window.\n\n"
                     "Or click here to choose the .iso file.\n\n"
                     "Or put the .iso in the same folder as " PROGRAM_NAME ", then start it again.\n\n%s",
                     status);
        draw_text(text, 16);
        SDL_RenderPresent(s_ren);
        SDL_Event e;
        if (!SDL_WaitEvent(&e))
            continue;
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            s_quit = true;
        else if (translocated)
            continue;
        else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data)
            path = SDL_strdup(e.drop.data);
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP)
            path = pick_iso();
    }
    SDL_free(path);
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    s_ren = NULL;
    s_win = NULL;
    return ok;
}
