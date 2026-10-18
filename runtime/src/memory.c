/* Guest address space, host-function thunks, guest heap and VirtualAlloc.
 *
 * Every piece of state the guest can observe lives inside guest memory and
 * refers to other guest memory by 32-bit guest address, never by host
 * pointer.  That keeps a snapshot of guest memory self-contained, which is
 * what savestates rely on.
 */
#include "rt.h"
#include "savestate.h"
#include "win32/com.h"
#include "os.h"
#include <pthread.h>

uint8_t *g_mem;

#define GUEST_SPACE (1ull << 32)
#define GUEST_SLACK (1u << 16)   /* lets unaligned accesses at 0xffffffff land */

void mem_init(void) {
    /* The first 64 KiB stay inaccessible to catch guest NULL-pointer
       accesses. */
    g_mem = os_reserve(GUEST_SPACE + GUEST_SLACK, 0x10000);
    if (!g_mem)
        rt_fatal("cannot reserve 4 GiB guest address space");
}

/* ------------------------------------------------------------------ thunks */
#define MAX_THUNKS ((GUEST_THUNKS_END - GUEST_THUNKS) / 16)
static GuestFn s_thunks[MAX_THUNKS];
static const char *s_thunk_names[MAX_THUNKS];
static uint32_t s_thunk_count;
static pthread_mutex_t s_thunk_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t thunk_register(GuestFn fn, const char *name) {
    pthread_mutex_lock(&s_thunk_lock);
    /* Reuse an existing thunk for the same host function. */
    for (uint32_t i = 0; i < s_thunk_count; i++) {
        if (s_thunks[i] == fn) {
            pthread_mutex_unlock(&s_thunk_lock);
            return GUEST_THUNKS + i * 16;
        }
    }
    if (s_thunk_count >= MAX_THUNKS)
        rt_fatal("out of host thunks");
    uint32_t i = s_thunk_count++;
    s_thunks[i] = fn;
    s_thunk_names[i] = name;
    uint32_t a = GUEST_THUNKS + i * 16;
    /* Guest-visible bytes: "ret; int3 ..." so code-pointer sanity checks pass. */
    memset(g_mem + a, 0xcc, 16);
    g_mem[a] = 0xc3;
    pthread_mutex_unlock(&s_thunk_lock);
    return a;
}

/* Debug: LR2_TRACE_THUNKS=1 counts host-function calls; thunk_stats_dump
 * prints and resets the counts. */
bool g_trace_thunks;
static uint32_t s_thunk_calls[MAX_THUNKS];

GuestFn thunk_lookup(uint32_t a) {
    if (a < GUEST_THUNKS || a >= GUEST_THUNKS_END || (a & 15))
        return NULL;
    uint32_t i = (a - GUEST_THUNKS) / 16;
    if (i >= s_thunk_count)
        return NULL;
    if (g_trace_thunks)
        __atomic_fetch_add(&s_thunk_calls[i], 1, __ATOMIC_RELAXED);
    return s_thunks[i];
}

void thunk_stats_dump(void) {
    for (uint32_t i = 0; i < s_thunk_count; i++) {
        uint32_t n = __atomic_exchange_n(&s_thunk_calls[i], 0, __ATOMIC_RELAXED);
        if (n)
            LOGI("  %8u %s", n, s_thunk_names[i]);
    }
}

const char *thunk_name(uint32_t a) {
    if (!thunk_lookup(a))
        return NULL;
    return s_thunk_names[(a - GUEST_THUNKS) / 16];
}

/* -------------------------------------------------------------- guest heap
 * Boundary-tag allocator with segregated free lists.  Chunk layout (guest
 * addresses, all fields uint32):
 *     chunk+0: prev_size   (valid when previous chunk is free)
 *     chunk+4: size | INUSE | PREV_INUSE
 *     chunk+8: payload (16-byte aligned) / free: next, prev links
 * Chunks start at 8 mod 16 so payloads are 16-byte aligned.
 */
#define INUSE      1u
#define PREV_INUSE 2u
#define FLAGS      15u
#define NBINS      160
#define MINCHUNK   32u

typedef struct {
    uint32_t bins[NBINS];
    uint64_t bitmap[3];
    uint32_t top;          /* start of the wilderness chunk */
    uint32_t end;
} HeapMeta;

#define HMETA ((HeapMeta *)(g_mem + GUEST_HEAP))
static pthread_mutex_t s_heap_lock = PTHREAD_MUTEX_INITIALIZER;

#define W(a) (*(u32u *)(g_mem + (uint32_t)(a)))
#define CSIZE(ch) (W((ch) + 4) & ~FLAGS)

static unsigned bin_of(uint32_t sz) {
    if (sz < 1024)
        return sz >> 4;
    unsigned lg = 31 - __builtin_clz(sz);
    unsigned b = 64 + (lg - 10) * 4 + ((sz >> (lg - 2)) & 3);
    return b < NBINS ? b : NBINS - 1;
}

static void bin_insert(uint32_t ch) {
    HeapMeta *h = HMETA;
    unsigned b = bin_of(CSIZE(ch));
    uint32_t head = h->bins[b];
    W(ch + 8) = head;
    W(ch + 12) = 0;
    if (head)
        W(head + 12) = ch;
    h->bins[b] = ch;
    h->bitmap[b >> 6] |= 1ull << (b & 63);
}

static void bin_remove(uint32_t ch) {
    HeapMeta *h = HMETA;
    unsigned b = bin_of(CSIZE(ch));
    uint32_t nx = W(ch + 8), pv = W(ch + 12);
    if (pv)
        W(pv + 8) = nx;
    else
        h->bins[b] = nx;
    if (nx)
        W(nx + 12) = pv;
    if (!h->bins[b])
        h->bitmap[b >> 6] &= ~(1ull << (b & 63));
}

void heap_init(void) {
    HeapMeta *h = HMETA;
    memset(h, 0, sizeof *h);
    uint32_t start = (GUEST_HEAP + sizeof(HeapMeta) + 31) & ~15u;
    h->top = start + 8;          /* chunk at 8 mod 16 */
    h->end = GUEST_HEAP_END - 64;
    W(h->top + 4) = PREV_INUSE;  /* wilderness: size implicit */
}

static uint32_t req_size(uint32_t n) {
    uint32_t s = (n + 8 + 15) & ~15u;
    return s < MINCHUNK ? MINCHUNK : s;
}

/* Mark free chunk ch (already unlinked) used, splitting off the remainder. */
static uint32_t take_chunk(uint32_t ch, uint32_t need) {
    uint32_t sz = CSIZE(ch);
    if (sz - need >= MINCHUNK) {
        uint32_t rest = ch + need;
        W(ch + 4) = need | INUSE | (W(ch + 4) & PREV_INUSE);
        W(rest + 4) = (sz - need) | PREV_INUSE;
        W(rest + sz - need) = sz - need;               /* footer */
        W(rest + sz - need + 4) &= ~PREV_INUSE;
        bin_insert(rest);
    } else {
        W(ch + 4) |= INUSE;
        W(ch + sz + 4) |= PREV_INUSE;
    }
    return ch + 8;
}

static uint32_t heap_alloc_locked(uint32_t n) {
    HeapMeta *h = HMETA;
    if (n > 0x40000000u)
        return 0;
    uint32_t need = req_size(n);
    unsigned b = bin_of(need);
    /* First fit within the exact bin. */
    for (uint32_t ch = h->bins[b]; ch; ch = W(ch + 8)) {
        if (CSIZE(ch) >= need) {
            bin_remove(ch);
            return take_chunk(ch, need);
        }
    }
    /* Any chunk from a larger non-empty bin fits. */
    for (unsigned w = (b + 1) >> 6; w < 3; w++) {
        uint64_t bits = h->bitmap[w];
        if (w == (b + 1) >> 6)
            bits &= ~0ull << ((b + 1) & 63);
        if (bits) {
            uint32_t ch = h->bins[w * 64 + __builtin_ctzll(bits)];
            bin_remove(ch);
            return take_chunk(ch, need);
        }
    }
    /* Carve from the wilderness. */
    uint32_t ch = h->top;
    if (h->end - ch < need + MINCHUNK)
        return 0;
    W(ch + 4) = need | INUSE | (W(ch + 4) & PREV_INUSE);
    h->top = ch + need;
    W(h->top + 4) = PREV_INUSE;
    return ch + 8;
}

static void heap_free_locked(uint32_t p) {
    HeapMeta *h = HMETA;
    uint32_t ch = p - 8;
    uint32_t sf = W(ch + 4);
    if (!(sf & INUSE)) {
        LOGW("heap: double free of %08x", p);
        return;
    }
    uint32_t sz = sf & ~FLAGS;
    /* Coalesce with previous free chunk. */
    if (!(sf & PREV_INUSE)) {
        uint32_t psz = W(ch);
        uint32_t prev = ch - psz;
        bin_remove(prev);
        ch = prev;
        sz += psz;
        sf = W(ch + 4);
    }
    uint32_t next = ch + sz;
    if (next == h->top) {
        /* Merge into the wilderness. */
        h->top = ch;
        W(ch + 4) = sf & PREV_INUSE;
        return;
    }
    uint32_t nsf = W(next + 4);
    if (!(nsf & INUSE)) {
        bin_remove(next);
        sz += nsf & ~FLAGS;
    }
    W(ch + 4) = sz | (sf & PREV_INUSE);
    W(ch + sz) = sz;
    W(ch + sz + 4) &= ~PREV_INUSE;
    bin_insert(ch);
}

uint32_t gmalloc(uint32_t n) {
    pthread_mutex_lock(&s_heap_lock);
    uint32_t p = heap_alloc_locked(n);
    pthread_mutex_unlock(&s_heap_lock);
    if (!p)
        LOGE("heap: out of memory allocating %u bytes", n);
    return p;
}

uint32_t gcalloc(uint32_t n) {
    uint32_t p = gmalloc(n);
    if (p)
        memset(g_mem + p, 0, n);
    return p;
}

void gfree(uint32_t p) {
    if (!p)
        return;
    pthread_mutex_lock(&s_heap_lock);
    heap_free_locked(p);
    pthread_mutex_unlock(&s_heap_lock);
}

uint32_t heap_top(void) { return HMETA->top; }

uint32_t gmsize(uint32_t p) {
    return CSIZE(p - 8) - 8;
}

uint32_t grealloc(uint32_t p, uint32_t n) {
    if (!p)
        return gmalloc(n);
    uint32_t old = gmsize(p);
    if (n <= old)
        return p;
    uint32_t q = gmalloc(n);
    if (q) {
        memcpy(g_mem + q, g_mem + p, old);
        gfree(p);
    }
    return q;
}

uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = gmalloc(n);
    if (p)
        memcpy(g_mem + p, s, n);
    return p;
}

/* ----------------------------------------------------------- VirtualAlloc
 * 64 KiB allocation granularity, 4 KiB commit pages.  Book-keeping is host
 * side but is part of savestates (see savestate.c). */
#define VA_UNIT  0x10000u
#define VA_PAGE  0x1000u
#define VA_UNITS ((GUEST_VA_END - GUEST_VA) / VA_UNIT)
#define VA_PAGES ((GUEST_VA_END - GUEST_VA) / VA_PAGE)

uint32_t g_va_resv[VA_UNITS];          /* units in reservation starting here, else 0 */
uint8_t g_va_used[VA_UNITS];           /* unit belongs to some reservation */
uint8_t g_va_commit[VA_PAGES];
static pthread_mutex_t s_va_lock = PTHREAD_MUTEX_INITIALIZER;

#define MEM_COMMIT   0x1000u
#define MEM_RESERVE  0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE  0x8000u

static void va_commit_range(uint32_t a, uint32_t size) {
    uint32_t p0 = (a - GUEST_VA) / VA_PAGE;
    uint32_t p1 = (a + size - 1 - GUEST_VA) / VA_PAGE;
    for (uint32_t p = p0; p <= p1; p++) {
        if (!g_va_commit[p]) {
            g_va_commit[p] = 1;
            memset(g_mem + GUEST_VA + p * VA_PAGE, 0, VA_PAGE);
        }
    }
}

uint32_t va_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect) {
    (void)protect;
    if (!size)
        return 0;
    pthread_mutex_lock(&s_va_lock);
    uint32_t result = 0;
    if (!addr || (type & MEM_RESERVE)) {
        uint32_t units = (size + VA_UNIT - 1) / VA_UNIT;
        uint32_t start = UINT32_MAX;
        if (addr) {
            uint32_t u = (addr - GUEST_VA) / VA_UNIT;
            if (addr >= GUEST_VA && u + units <= VA_UNITS) {
                start = u;
                for (uint32_t k = 0; k < units; k++)
                    if (g_va_used[u + k])
                        start = UINT32_MAX;
            }
        } else {
            for (uint32_t u = 0, run = 0; u < VA_UNITS; u++) {
                run = g_va_used[u] ? 0 : run + 1;
                if (run == units) {
                    start = u + 1 - units;
                    break;
                }
            }
        }
        if (start != UINT32_MAX) {
            for (uint32_t k = 0; k < units; k++)
                g_va_used[start + k] = 1;
            g_va_resv[start] = units;
            result = GUEST_VA + start * VA_UNIT;
            if (type & MEM_COMMIT)
                va_commit_range(result, size);
        }
    } else if (type & MEM_COMMIT) {
        if (addr >= GUEST_VA && addr + size <= GUEST_VA_END &&
            g_va_used[(addr - GUEST_VA) / VA_UNIT]) {
            va_commit_range(addr, size);
            result = addr & ~(VA_PAGE - 1);
        }
    }
    pthread_mutex_unlock(&s_va_lock);
    if (!result)
        LOGW("VirtualAlloc(%08x, %x, %x) failed", addr, size, type);
    return result;
}

bool va_free(uint32_t addr, uint32_t size, uint32_t type) {
    if (addr < GUEST_VA || addr >= GUEST_VA_END)
        return false;
    pthread_mutex_lock(&s_va_lock);
    bool ok = false;
    if (type & MEM_RELEASE) {
        uint32_t u = (addr - GUEST_VA) / VA_UNIT;
        uint32_t units = g_va_resv[u];
        if (units) {
            g_va_resv[u] = 0;
            for (uint32_t k = 0; k < units; k++)
                g_va_used[u + k] = 0;
            uint32_t p0 = u * (VA_UNIT / VA_PAGE);
            memset(g_va_commit + p0, 0, units * (VA_UNIT / VA_PAGE));
            ok = true;
        }
    } else if (type & MEM_DECOMMIT) {
        uint32_t p0 = (addr - GUEST_VA) / VA_PAGE;
        uint32_t p1 = (addr + size - 1 - GUEST_VA) / VA_PAGE;
        for (uint32_t p = p0; p <= p1 && p < VA_PAGES; p++)
            g_va_commit[p] = 0;
        ok = true;
    }
    pthread_mutex_unlock(&s_va_lock);
    return ok;
}

/* -------------------------------------------------------------- savestates
 * VMEM: VirtualAlloc book-keeping.  THNK: the thunk table by name; thunks
 * are handed out lazily, so the order can differ between runs and is
 * rebuilt from the names. */
static void vmem_save(SnapBuf *b) {
    sb_put(b, g_va_resv, sizeof g_va_resv);
    sb_put(b, g_va_used, sizeof g_va_used);
    sb_put(b, g_va_commit, sizeof g_va_commit);
}

static bool vmem_load(SnapBuf *b) {
    sb_get(b, g_va_resv, sizeof g_va_resv);
    sb_get(b, g_va_used, sizeof g_va_used);
    sb_get(b, g_va_commit, sizeof g_va_commit);
    return !b->err;
}

static GuestFn thunk_fn_by_name(const char *name) {
    for (uint32_t i = 0; i < s_thunk_count; i++)
        if (!strcmp(s_thunk_names[i], name))
            return s_thunks[i];
    GuestFn fn = com_vtbl_method(name);
    return fn ? fn : module_export_fn(name);
}

static void thunks_save(SnapBuf *b) {
    sb_u32(b, s_thunk_count);
    for (uint32_t i = 0; i < s_thunk_count; i++)
        sb_str(b, s_thunk_names[i]);
}

static bool thunks_check(SnapBuf *b) {
    uint32_t n = sb_get_u32(b);
    bool ok = n <= MAX_THUNKS;
    for (uint32_t i = 0; ok && i < n && !b->err; i++) {
        char *name = sb_get_str(b);
        if (!name || !thunk_fn_by_name(name)) {
            LOGW("savestate: unknown host function %s", name ? name : "(null)");
            ok = false;
        }
        free(name);
    }
    return ok && !b->err;
}

static bool thunks_load(SnapBuf *b) {
    uint32_t n = sb_get_u32(b);
    static GuestFn fns[MAX_THUNKS];
    static const char *names[MAX_THUNKS];
    for (uint32_t i = 0; i < n && !b->err; i++) {
        char *name = sb_get_str(b);
        fns[i] = name ? thunk_fn_by_name(name) : NULL;
        names[i] = name;
    }
    if (b->err)
        return false;
    pthread_mutex_lock(&s_thunk_lock);
    for (uint32_t i = 0; i < n; i++) {
        s_thunks[i] = fns[i];
        s_thunk_names[i] = names[i];   /* the old names may be static; keep them */
        uint32_t a = GUEST_THUNKS + i * 16;
        memset(g_mem + a, 0xcc, 16);
        g_mem[a] = 0xc3;
    }
    s_thunk_count = n;
    pthread_mutex_unlock(&s_thunk_lock);
    return true;
}

static const SnapSection s_mem_sections[] = {
    {SNAP_TAG('V', 'M', 'E', 'M'), SNAP_ORDER_MEMORY, vmem_save, NULL, NULL, vmem_load},
    {SNAP_TAG('T', 'H', 'N', 'K'), SNAP_ORDER_MEMORY, thunks_save, thunks_check, NULL, thunks_load},
};

__attribute__((constructor)) static void mem_snap_register(void) {
    for (size_t i = 0; i < sizeof s_mem_sections / sizeof *s_mem_sections; i++)
        snap_section(&s_mem_sections[i]);
}
