/* Savestates: world stop, snapshot format, guest memory, threads, slots.
 * See savestate.h for the overall approach.
 *
 * File format (little endian):
 *     "LR2STATE" u32 version u32 build
 *     { u32 tag, u32 len, data[len] } ...      one record per section
 */
#define _GNU_SOURCE   /* dladdr */
#include "savestate.h"
#include "win32/user32.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <zlib.h>
#include <signal.h>
#include "os.h"

#define SNAP_VERSION 1
#define NUM_SLOTS 10

/* ------------------------------------------------------------ byte buffer */
void sb_put(SnapBuf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n)
            cap *= 2;
        b->data = realloc(b->data, cap);
        if (!b->data)
            rt_fatal("savestate: out of memory");
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void sb_u32(SnapBuf *b, uint32_t v) { sb_put(b, &v, 4); }

void sb_str(SnapBuf *b, const char *s) {
    if (!s) {
        sb_u32(b, 0xffffffffu);
        return;
    }
    uint32_t n = (uint32_t)strlen(s);
    sb_u32(b, n);
    sb_put(b, s, n);
}

bool sb_get(SnapBuf *b, void *p, size_t n) {
    if (b->err || b->pos + n > b->len) {
        b->err = true;
        memset(p, 0, n);
        return false;
    }
    memcpy(p, b->data + b->pos, n);
    b->pos += n;
    return true;
}

uint32_t sb_get_u32(SnapBuf *b) {
    uint32_t v;
    sb_get(b, &v, 4);
    return v;
}

char *sb_get_str(SnapBuf *b) {
    uint32_t n = sb_get_u32(b);
    if (n == 0xffffffffu || b->err)
        return NULL;
    if (b->pos + n > b->len) {
        b->err = true;
        return NULL;
    }
    char *s = malloc(n + 1);
    memcpy(s, b->data + b->pos, n);
    s[n] = 0;
    b->pos += n;
    return s;
}

void sb_free(SnapBuf *b) {
    free(b->data);
    memset(b, 0, sizeof *b);
}

/* --------------------------------------------------------------- sections */
#define MAX_SECTIONS 32
static SnapSection s_sections[MAX_SECTIONS];
static int s_nsections;

void snap_section(const SnapSection *s) {
    if (s_nsections == MAX_SECTIONS)
        rt_fatal("savestate: too many sections");
    int i = s_nsections++;
    while (i > 0 && s_sections[i - 1].order > s->order) {
        s_sections[i] = s_sections[i - 1];
        i--;
    }
    s_sections[i] = *s;
}

#define MAX_LOCKS 8
static SnapLock s_locks[MAX_LOCKS];
static int s_nlocks;

void snap_add_lock(const SnapLock *l) {
    if (s_nlocks < MAX_LOCKS)
        s_locks[s_nlocks++] = *l;
}

static void (*s_wakers[8])(void);
static int s_nwakers;

void snap_add_waker(void (*wake)(void)) {
    if (s_nwakers < 8)
        s_wakers[s_nwakers++] = wake;
}

/* ---------------------------------------------------------- call chains */
#define MAX_CHAIN 48

typedef struct {
    char *frames[MAX_CHAIN];
    int n;
} Chain;

static void chain_free(Chain *ch) {
    for (int i = 0; i < ch->n; i++)
        free(ch->frames[i]);
    ch->n = 0;
}

/* Host frames inside our executable, outermost last.  Recompiled functions
   keep their offset (it identifies the guest call site); runtime functions
   are named only, so runtime-only changes keep old states loadable. */
static void chain_capture(Chain *ch) {
    void *pcs[MAX_CHAIN + 8];
    int n = backtrace(pcs, MAX_CHAIN + 8);
    Dl_info self;
    dladdr((void *)chain_capture, &self);
    ch->n = 0;
    for (int i = 1; i < n && ch->n < MAX_CHAIN; i++) {
        Dl_info di;
        if (!dladdr(pcs[i], &di) || di.dli_fbase != self.dli_fbase)
            continue;
        char buf[160];
        const char *name = di.dli_sname ? di.dli_sname : "?";
        if (name[0] == 'f' && name[1] == '_' && di.dli_saddr)
            snprintf(buf, sizeof buf, "%s+%lx", name,
                     (unsigned long)((char *)pcs[i] - (char *)di.dli_saddr));
        else
            snprintf(buf, sizeof buf, "%s", name);
        ch->frames[ch->n++] = strdup(buf);
    }
}

static void chain_save(SnapBuf *b, const Chain *ch) {
    sb_u32(b, (uint32_t)ch->n);
    for (int i = 0; i < ch->n; i++)
        sb_str(b, ch->frames[i]);
}

static void chain_load(SnapBuf *b, Chain *ch) {
    uint32_t n = sb_get_u32(b);
    ch->n = 0;
    for (uint32_t i = 0; i < n && !b->err; i++) {
        char *s = sb_get_str(b);
        if (ch->n < MAX_CHAIN)
            ch->frames[ch->n++] = s;
        else
            free(s);
    }
}

static bool chain_eq(const Chain *a, const Chain *b) {
    if (a->n != b->n)
        return false;
    for (int i = 0; i < a->n; i++)
        if (strcmp(a->frames[i], b->frames[i]))
            return false;
    return true;
}

static void chain_log(const char *what, const Chain *ch) {
    LOGW("  %s:", what);
    for (int i = 0; i < ch->n; i++)
        LOGW("    %s", ch->frames[i]);
}

/* ---------------------------------------------------------------- threads */
#define MAX_THREADS 16

typedef struct {
    bool used, parked, loaded;
    int role;
    uint32_t key;
    CPU *c;
    pthread_t self;
    Chain chain;
    /* thread-local guest state, exported when parking */
    uint32_t tls[TLS_SLOTS];
    uint32_t tid, last_error;
} SnapThread;

static SnapThread s_threads[MAX_THREADS];
static __thread SnapThread *t_self;
static pthread_mutex_t s_tlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_tcond = PTHREAD_COND_INITIALIZER;
volatile int g_snap_stop;
static uint32_t s_generation;     /* bumped on every resume */

void snap_thread_enter(int role, uint32_t key, CPU *c) {
    pthread_mutex_lock(&s_tlock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!s_threads[i].used) {
            s_threads[i] = (SnapThread){.used = true, .role = role, .key = key, .c = c,
                                        .self = pthread_self()};
            t_self = &s_threads[i];
            break;
        }
    }
    pthread_mutex_unlock(&s_tlock);
    if (!t_self)
        LOGW("savestate: too many threads; savestates will not work");
}

void snap_thread_exit(void) {
    if (!t_self)
        return;
    pthread_mutex_lock(&s_tlock);
    chain_free(&t_self->chain);
    t_self->used = false;
    t_self = NULL;
    pthread_cond_broadcast(&s_tcond);
    pthread_mutex_unlock(&s_tlock);
}

static void export_locals(SnapThread *t) {
    thread_locals_export(t->tls, &t->tid);
    t->last_error = get_last_error();
}

static void import_locals(SnapThread *t) {
    thread_locals_import(t->tls, t->tid);
    set_last_error(t->last_error);
}

bool snap_can_park(void) { return t_self && t_self->role != SNAP_THREAD_MAIN && !thread_cs_held(); }

bool snap_park(void) {
    SnapThread *t = t_self;
    if (!t)
        return false;
    chain_capture(&t->chain);
    export_locals(t);
    pthread_mutex_lock(&s_tlock);
    t->parked = true;
    t->loaded = false;
    pthread_cond_broadcast(&s_tcond);
    uint32_t gen = s_generation;
    while (s_generation == gen)
        pthread_cond_wait(&s_tcond, &s_tlock);
    t->parked = false;
    bool loaded = t->loaded;
    pthread_mutex_unlock(&s_tlock);
    chain_free(&t->chain);
    if (loaded)
        import_locals(t);
    return loaded;
}

#ifndef _WIN32
/* Diagnostics: a thread that does not park is asked to log where it is. */
static void stuck_handler(int sig) {
    (void)sig;
    LOGW("savestate: backtrace of the thread that did not park follows");
    debug_backtrace("thread that did not park");
}
#endif

/* Stop every other registered thread at its park point.  report: log (and
   backtrace) the threads that did not make it. */
static bool world_stop(int timeout_ms, bool report) {
    __atomic_store_n(&g_snap_stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < s_nwakers; i++)
        s_wakers[i]();
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += (long)timeout_ms * 1000000L;
    dl.tv_sec += dl.tv_nsec / 1000000000L;
    dl.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&s_tlock);
    for (;;) {
        bool all = true;
        for (int i = 0; i < MAX_THREADS; i++)
            if (s_threads[i].used && &s_threads[i] != t_self && !s_threads[i].parked)
                all = false;
        if (all)
            break;
        if (pthread_cond_timedwait(&s_tcond, &s_tlock, &dl) == ETIMEDOUT) {
            if (!report) {
                pthread_mutex_unlock(&s_tlock);
                return false;
            }
#ifndef _WIN32
            signal(SIGUSR2, stuck_handler);
#endif
            for (int i = 0; i < MAX_THREADS; i++) {
                if (s_threads[i].used && &s_threads[i] != t_self && !s_threads[i].parked) {
                    LOGW("savestate: thread (role %d, key %x) did not reach a safe point",
                         s_threads[i].role, s_threads[i].key);
#ifndef _WIN32
                    pthread_kill(s_threads[i].self, SIGUSR2);
#endif
                }
            }
            pthread_mutex_unlock(&s_tlock);
            SDL_Delay(100);
            return false;
        }
    }
    pthread_mutex_unlock(&s_tlock);
    return true;
}

static void world_resume(bool loaded) {
    pthread_mutex_lock(&s_tlock);
    for (int i = 0; i < MAX_THREADS; i++)
        if (s_threads[i].used && s_threads[i].parked)
            s_threads[i].loaded = loaded;
    __atomic_store_n(&g_snap_stop, 0, __ATOMIC_RELEASE);
    s_generation++;
    pthread_cond_broadcast(&s_tcond);
    pthread_mutex_unlock(&s_tlock);
    for (int i = 0; i < s_nwakers; i++)
        s_wakers[i]();
}

/* THRD: CPU registers, thread locals and call chains of every thread.  The
   timer threads' guest state is idle between callbacks; only their TEB has
   to line up. */
static void threads_save(SnapBuf *b) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_THREADS; i++)
        n += s_threads[i].used;
    sb_u32(b, n);
    for (int i = 0; i < MAX_THREADS; i++) {
        SnapThread *t = &s_threads[i];
        if (!t->used)
            continue;
        sb_u32(b, (uint32_t)t->role);
        sb_u32(b, t->key);
        sb_put(b, t->c, offsetof(CPU, thread));
        sb_put(b, t->tls, sizeof t->tls);
        sb_u32(b, t->tid);
        sb_u32(b, t->last_error);
        chain_save(b, &t->chain);
    }
}

typedef struct {
    int role;
    uint32_t key;
    CPU cpu;
    uint32_t tls[TLS_SLOTS];
    uint32_t tid, last_error;
    Chain chain;
} SavedThread;

static int threads_read(SnapBuf *b, SavedThread *out) {
    uint32_t n = sb_get_u32(b);
    if (n > MAX_THREADS)
        return -1;
    for (uint32_t i = 0; i < n; i++) {
        SavedThread *s = &out[i];
        memset(s, 0, sizeof *s);
        s->role = (int)sb_get_u32(b);
        s->key = sb_get_u32(b);
        sb_get(b, &s->cpu, offsetof(CPU, thread));
        sb_get(b, s->tls, sizeof s->tls);
        s->tid = sb_get_u32(b);
        s->last_error = sb_get_u32(b);
        chain_load(b, &s->chain);
    }
    return b->err ? -1 : (int)n;
}

static SnapThread *thread_find(int role, uint32_t key) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (s_threads[i].used && s_threads[i].role == role && s_threads[i].key == key)
            return &s_threads[i];
    return NULL;
}

static bool threads_check(SnapBuf *b) {
    SavedThread saved[MAX_THREADS];
    int n = threads_read(b, saved);
    bool ok = n >= 0;
    int live = 0;
    for (int i = 0; i < MAX_THREADS; i++)
        live += s_threads[i].used;
    if (ok && n != live) {
        LOGW("savestate: state has %d threads, the game is running %d", n, live);
        ok = false;
    }
    for (int i = 0; ok && i < n; i++) {
        SavedThread *s = &saved[i];
        SnapThread *t = thread_find(s->role, s->key);
        if (!t) {
            LOGW("savestate: no running thread matches role %d key %x", s->role, s->key);
            ok = false;
        } else if (s->role == SNAP_THREAD_TIMER) {
            if (t->c->fs_base != s->cpu.fs_base) {
                LOGW("savestate: timer thread %x has a different TEB", s->key);
                ok = false;
            }
        } else if (!chain_eq(&t->chain, &s->chain)) {
            LOGW("savestate: thread (role %d, key %x) is somewhere else in the game", s->role, s->key);
            chain_log("state", &s->chain);
            chain_log("now", &t->chain);
            ok = false;
        }
    }
    for (int i = 0; i < (n > 0 ? n : 0); i++)
        chain_free(&saved[i].chain);
    return ok;
}

static bool threads_load(SnapBuf *b) {
    SavedThread saved[MAX_THREADS];
    int n = threads_read(b, saved);
    for (int i = 0; i < n; i++) {
        SavedThread *s = &saved[i];
        SnapThread *t = thread_find(s->role, s->key);
        if (t && s->role != SNAP_THREAD_TIMER) {
            memcpy(t->c, &s->cpu, offsetof(CPU, thread));
            memcpy(t->tls, s->tls, sizeof t->tls);
            t->tid = s->tid;
            t->last_error = s->last_error;
        }
        chain_free(&s->chain);
    }
    return n >= 0;
}

/* ------------------------------------------------------------ guest memory
 * MEMR: a list of page ranges; each page is stored only if non-zero. */
#define PAGE 0x1000u

typedef struct {
    uint32_t lo, hi;
} Range;

static int mem_ranges(Range *r, int max) {
    int n = 0;
#define ADD(a, b) do { if (n < max && (b) > (a)) r[n++] = (Range){(a) & ~(PAGE - 1), ((b) + PAGE - 1) & ~(PAGE - 1)}; } while (0)
    ADD(GUEST_PEB, GUEST_MAIN_STACK_LO);
    ADD(GUEST_MAIN_STACK_LO, GUEST_MAIN_STACK_HI);
    ADD(g_pe.base, g_pe.base + g_pe.size);
    for (uint32_t s = 0; s < stack_slot_count(); s++)
        if (stack_slot_used(s))
            ADD(GUEST_STACKS + s * THREAD_STACK_SIZE, GUEST_STACKS + (s + 1) * THREAD_STACK_SIZE);
    /* committed VirtualAlloc pages, as runs */
    uint32_t pages = (GUEST_VA_END - GUEST_VA) / PAGE;
    for (uint32_t p = 0; p < pages;) {
        if (!g_va_commit[p]) {
            p++;
            continue;
        }
        uint32_t q = p;
        while (q < pages && g_va_commit[q])
            q++;
        ADD(GUEST_VA + p * PAGE, GUEST_VA + q * PAGE);
        p = q;
    }
    ADD(GUEST_HEAP, heap_top() + 64);
#undef ADD
    return n;
}

static bool page_zero(const uint8_t *p) {
    const uint64_t *q = (const uint64_t *)p;
    for (uint32_t i = 0; i < PAGE / 8; i++)
        if (q[i])
            return false;
    return true;
}

static void mem_save(SnapBuf *b) {
    static Range r[4096];
    int n = mem_ranges(r, 4096);
    sb_u32(b, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        sb_u32(b, r[i].lo);
        sb_u32(b, r[i].hi);
        for (uint32_t a = r[i].lo; a < r[i].hi; a += PAGE) {
            const uint8_t *p = g_mem + a;
            uint8_t nz = !page_zero(p);
            sb_put(b, &nz, 1);
            if (nz)
                sb_put(b, p, PAGE);
        }
    }
}

static bool mem_load(SnapBuf *b) {
    uint32_t n = sb_get_u32(b);
    for (uint32_t i = 0; i < n && !b->err; i++) {
        uint32_t lo = sb_get_u32(b), hi = sb_get_u32(b);
        if (lo < GUEST_PEB || hi > GUEST_HEAP_END || lo > hi) {
            b->err = true;
            break;
        }
        for (uint32_t a = lo; a < hi && !b->err; a += PAGE) {
            uint8_t nz = 0;
            sb_get(b, &nz, 1);
            if (nz)
                sb_get(b, g_mem + a, PAGE);
            else
                memset(g_mem + a, 0, PAGE);
        }
    }
    return !b->err;
}

/* -------------------------------------------------------------- snapshots */
static uint32_t build_id(void) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < g_func_count; i++)
        h = (h ^ g_func_table[i].addr) * 16777619u;
    return h ^ g_import_count;
}

static void lock_all(void) {
    for (int i = 0; i < s_nlocks; i++)
        s_locks[i].lock();
}

static void unlock_all(void) {
    for (int i = s_nlocks - 1; i >= 0; i--)
        s_locks[i].unlock();
}

static const SnapSection s_builtin[] = {
    {SNAP_TAG('M', 'E', 'M', 'R'), SNAP_ORDER_MEMORY, mem_save, NULL, NULL, mem_load},
    {SNAP_TAG('T', 'H', 'R', 'D'), SNAP_ORDER_KERNEL, threads_save, threads_check, NULL, threads_load},
};

static void snapshot_build(SnapBuf *out) {
    sb_put(out, "LR2STATE", 8);
    sb_u32(out, SNAP_VERSION);
    sb_u32(out, build_id());
    for (int i = 0; i < s_nsections; i++) {
        SnapBuf sec = {0};
        s_sections[i].save(&sec);
        sb_u32(out, s_sections[i].tag);
        sb_u32(out, (uint32_t)sec.len);
        sb_put(out, sec.data, sec.len);
        sb_free(&sec);
    }
}

/* Find a section's payload inside a snapshot. */
static bool snapshot_section(const SnapBuf *snap, uint32_t tag, SnapBuf *out) {
    size_t pos = 16;
    while (pos + 8 <= snap->len) {
        uint32_t t, len;
        memcpy(&t, snap->data + pos, 4);
        memcpy(&len, snap->data + pos + 4, 4);
        pos += 8;
        if (pos + len > snap->len)
            return false;
        if (t == tag) {
            *out = (SnapBuf){.data = snap->data + pos, .len = len, .cap = len};
            return true;
        }
        pos += len;
    }
    return false;
}

static bool snapshot_header_ok(const SnapBuf *snap) {
    uint32_t ver, build;
    if (snap->len < 16 || memcmp(snap->data, "LR2STATE", 8))
        return false;
    memcpy(&ver, snap->data + 8, 4);
    memcpy(&build, snap->data + 12, 4);
    if (ver != SNAP_VERSION) {
        LOGW("savestate: version %u, expected %u", ver, SNAP_VERSION);
        return false;
    }
    if (build != build_id()) {
        LOGW("savestate: made with a different recompiled executable");
        return false;
    }
    return true;
}

static bool snapshot_apply(const SnapBuf *snap) {
    if (!snapshot_header_ok(snap))
        return false;
    for (int i = 0; i < s_nsections; i++) {
        SnapBuf sec;
        if (!snapshot_section(snap, s_sections[i].tag, &sec)) {
            LOGW("savestate: missing section %.4s", (const char *)&s_sections[i].tag);
            return false;
        }
        if (s_sections[i].check && !s_sections[i].check(&sec)) {
            LOGW("savestate: section %.4s cannot be loaded here", (const char *)&s_sections[i].tag);
            return false;
        }
    }
    /* Point of no return. */
    for (int i = s_nsections - 1; i >= 0; i--)
        if (s_sections[i].drop)
            s_sections[i].drop();
    for (int i = 0; i < s_nsections; i++) {
        SnapBuf sec;
        snapshot_section(snap, s_sections[i].tag, &sec);
        if (!s_sections[i].load(&sec) || sec.err)
            rt_fatal("savestate: section %.4s failed to load; game state is inconsistent",
                     (const char *)&s_sections[i].tag);
    }
    return true;
}

/* ------------------------------------------------------------------ slots */
static SnapBuf s_slots[NUM_SLOTS];
static int s_slot;
static volatile int s_request;     /* 0, 'S' or 'L' */
static int s_request_slot;

static void slot_path(int slot, char *out, size_t sz) {
    snprintf(out, sz, "%s/states/slot%d.lr2s", g_save_dir, slot);
}

/* Slot files: FILE_MAGIC, raw length (u64), then a zlib stream of the
   snapshot.  Compression runs on a writer thread so saving does not stall
   the game; the slot's mutex keeps the next save to that slot from
   replacing the buffer while it is still being written. */
#define FILE_MAGIC SNAP_TAG('L', 'R', '2', 'Z')
static pthread_mutex_t s_slot_mu[NUM_SLOTS];

static bool slot_read(int slot, SnapBuf *out) {
    if (s_slots[slot].len) {
        *out = s_slots[slot];
        return true;
    }
    char path[1200];
    slot_path(slot, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint32_t magic = 0;
    uint64_t raw = 0;
    SnapBuf b = {0};
    if (fread(&magic, 4, 1, f) == 1 && magic == FILE_MAGIC && fread(&raw, 8, 1, f) == 1 &&
        raw && raw < ((uint64_t)1 << 32)) {
        b.data = malloc((size_t)raw);
        b.len = b.cap = (size_t)raw;
        z_stream z = {0};
        inflateInit(&z);
        z.next_out = b.data;
        z.avail_out = (uInt)raw;
        static uint8_t in[1 << 20];
        int r = Z_OK;
        while (r == Z_OK) {
            z.avail_in = (uInt)fread(in, 1, sizeof in, f);
            z.next_in = in;
            if (!z.avail_in)
                break;
            r = inflate(&z, Z_NO_FLUSH);
        }
        if (r != Z_STREAM_END || z.total_out != raw)
            sb_free(&b);
        inflateEnd(&z);
    }
    fclose(f);
    if (!b.len) {
        LOGW("savestate: %s is not a valid state file", path);
        return false;
    }
    s_slots[slot] = b;
    *out = b;
    return true;
}

static void *slot_writer(void *arg) {
    int slot = (int)(intptr_t)arg;
    char dir[1100], path[1200], tmp[1210];
    snprintf(dir, sizeof dir, "%s/states", g_save_dir);
    mkdir(dir, 0755);
    slot_path(slot, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    uint64_t t0 = SDL_GetTicks();
    const SnapBuf *b = &s_slots[slot];
    FILE *f = fopen(tmp, "wb");
    uint32_t magic = FILE_MAGIC;
    uint64_t raw = b->len;
    bool ok = f && fwrite(&magic, 4, 1, f) == 1 && fwrite(&raw, 8, 1, f) == 1;
    z_stream z = {0};
    deflateInit(&z, 1);
    z.next_in = b->data;
    z.avail_in = (uInt)b->len;
    enum { OUT = 1 << 20 };          /* writers for different slots can overlap */
    uint8_t *out = malloc(OUT);
    int r = Z_OK;
    while (ok && r == Z_OK) {
        z.next_out = out;
        z.avail_out = OUT;
        r = deflate(&z, Z_FINISH);
        size_t n = OUT - z.avail_out;
        ok = fwrite(out, 1, n, f) == n;
    }
    ok = ok && r == Z_STREAM_END;
    uint64_t packed = z.total_out;
    deflateEnd(&z);
    free(out);
    if (f && fclose(f))
        ok = false;
    if (ok && !os_replace(tmp, path))
        LOGI("savestate: wrote %s (%.1f MiB, %llu ms)", path, packed / 1048576.0,
             (unsigned long long)(SDL_GetTicks() - t0));
    else
        LOGW("savestate: cannot write %s", tmp);
    pthread_mutex_unlock(&s_slot_mu[slot]);
    return NULL;
}

/* Called with the slot's mutex held; the writer releases it when the file
   is complete. */
static void slot_write(int slot) {
    pthread_t t;
    if (pthread_create(&t, NULL, slot_writer, (void *)(intptr_t)slot)) {
        pthread_mutex_unlock(&s_slot_mu[slot]);
        LOGW("savestate: cannot start the writer thread");
        return;
    }
    pthread_detach(t);
}

static uint64_t now_ms(void) { return SDL_GetTicks(); }

/* Threads usually park within a frame; one that is briefly busy (a timer
   callback waiting on the main thread, say) is given a few more frames:
   the world is resumed and the request retried at the next safepoint. */
#define STOP_TRIES 8
#define STOP_WAIT_MS 250

/* Returns false if the request should be retried next frame. */
static bool do_save(int slot, bool last_try) {
    uint64_t t0 = now_ms();
    if (!world_stop(STOP_WAIT_MS, last_try)) {
        world_resume(false);
        if (last_try)
            LOGW("savestate: could not stop the game threads; not saved");
        return last_try;
    }
    chain_capture(&t_self->chain);
    export_locals(t_self);
    lock_all();
    SnapBuf b = {0};
    snapshot_build(&b);
    unlock_all();
    chain_free(&t_self->chain);
    world_resume(false);
    pthread_mutex_lock(&s_slot_mu[slot]);
    sb_free(&s_slots[slot]);
    s_slots[slot] = b;
    slot_write(slot);
    LOGI("savestate: saved slot %d (%.1f MiB, %llu ms)", slot, b.len / 1048576.0,
         (unsigned long long)(now_ms() - t0));
    return true;
}

static bool do_load(int slot, bool last_try) {
    SnapBuf b;
    uint64_t t0 = now_ms();
    if (!slot_read(slot, &b)) {
        LOGI("savestate: slot %d is empty", slot);
        return true;
    }
    if (!world_stop(STOP_WAIT_MS, last_try)) {
        world_resume(false);
        if (last_try)
            LOGW("savestate: could not stop the game threads; not loaded");
        return last_try;
    }
    chain_capture(&t_self->chain);
    export_locals(t_self);
    lock_all();
    bool ok = snapshot_apply(&b);
    unlock_all();
    chain_free(&t_self->chain);
    if (ok)
        import_locals(t_self);
    world_resume(ok);
    if (ok)
        LOGI("savestate: loaded slot %d (%llu ms)", slot, (unsigned long long)(now_ms() - t0));
    else
        LOGW("savestate: slot %d cannot be loaded at this point", slot);
    return true;
}

void snap_request_save(int slot) {
    s_request_slot = slot;
    __atomic_store_n(&s_request, 'S', __ATOMIC_RELEASE);
}

void snap_request_load(int slot) {
    s_request_slot = slot;
    __atomic_store_n(&s_request, 'L', __ATOMIC_RELEASE);
}

void snap_safepoint(CPU *c) {
    (void)c;
    if (!t_self || t_self->role != SNAP_THREAD_MAIN || thread_cs_held())
        return;   /* a request waits for a frame where no lock is held */
    static int tries;
    int r = __atomic_exchange_n(&s_request, 0, __ATOMIC_ACQ_REL);
    if (!r)
        return;
    bool last = ++tries >= STOP_TRIES;
    bool done = r == 'S' ? do_save(s_request_slot, last) : do_load(s_request_slot, last);
    if (done) {
        if (tries > 1)
            LOGI("savestate: needed %d attempts", tries);
        tries = 0;
    } else {
        /* retry unless a newer request arrived meanwhile */
        int expected = 0;
        if (!__atomic_compare_exchange_n(&s_request, &expected, r, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            tries = 0;
    }
}

/* Gamepad: hold Back and press LB to save, RB to load.  The shoulder
   button events are not passed on, but the game's polled pad state (and
   Back itself) still sees them. */
static bool snap_pad(const SDL_Event *e) {
    if (e->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->type != SDL_EVENT_GAMEPAD_BUTTON_UP)
        return false;
    uint8_t b = e->gbutton.button;
    if (b != SDL_GAMEPAD_BUTTON_LEFT_SHOULDER && b != SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)
        return false;
    static bool eaten[2];
    int i = b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        bool was = eaten[i];
        eaten[i] = false;
        return was;
    }
    SDL_Gamepad *gp = SDL_GetGamepadFromID(e->gbutton.which);
    if (!gp || !SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK))
        return false;
    eaten[i] = true;
    if (i)
        snap_request_load(s_slot);
    else
        snap_request_save(s_slot);
    return true;
}

/* F5 save, F9 load, F6/F7 pick the slot.  The keys never reach the game. */
static bool snap_keys(const SDL_Event *e) {
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP)
        return snap_pad(e);
    SDL_Scancode sc = e->key.scancode;
    if (sc != SDL_SCANCODE_F5 && sc != SDL_SCANCODE_F6 && sc != SDL_SCANCODE_F7 && sc != SDL_SCANCODE_F9)
        return false;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        if (sc == SDL_SCANCODE_F5)
            snap_request_save(s_slot);
        else if (sc == SDL_SCANCODE_F9)
            snap_request_load(s_slot);
        else {
            s_slot = (s_slot + (sc == SDL_SCANCODE_F7 ? 1 : NUM_SLOTS - 1)) % NUM_SLOTS;
            LOGI("savestate: slot %d", s_slot);
        }
    }
    return true;
}

/* Let state files that are still being written finish. */
void snap_wait_writers(void) {
    for (int i = 0; i < NUM_SLOTS; i++)
        pthread_mutex_lock(&s_slot_mu[i]);
}

void snap_init(void) {
    for (int i = 0; i < NUM_SLOTS; i++)
        pthread_mutex_init(&s_slot_mu[i], NULL);
    atexit(snap_wait_writers);
    for (size_t i = 0; i < sizeof s_builtin / sizeof *s_builtin; i++)
        snap_section(&s_builtin[i]);
    user32_add_event_hook(snap_keys);
}
