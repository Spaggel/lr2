/* Handle table, synchronization objects, threads, TLS, critical sections.
 *
 * All waitable objects share one mutex/condition variable: signalling any
 * object broadcasts, and waiters re-check their own condition.  Simple and
 * plenty fast for a handful of game threads.
 */
#include "handles.h"
#include "../savestate.h"
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <ctype.h>

#define MAX_HANDLES 4096
#define HBASE 0x100u

typedef struct {
    int type;
    int fd;                 /* H_FILE */
    int oflags;             /* H_FILE: open() access flags, to reopen it */
    bool manual, signaled;  /* H_EVENT; H_THREAD signaled = exited */
    uint32_t owner, count;  /* H_MUTEX */
    uint32_t exit_code;     /* H_THREAD */
    uint32_t tid;
    char *path;
    char *name;             /* named mutex/event */
    int refs;               /* handles sharing this object (OpenMutex etc.) */
} HObj;

static HObj s_h[MAX_HANDLES];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;

void handles_init(void) {}

static HObj *get(uint32_t h) {
    if (h < HBASE || (h - HBASE) % 4)
        return NULL;
    uint32_t i = (h - HBASE) / 4;
    if (i >= MAX_HANDLES || s_h[i].type == H_FREE)
        return NULL;
    return &s_h[i];
}

static uint32_t alloc_locked(int type) {
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        if (s_h[i].type == H_FREE) {
            memset(&s_h[i], 0, sizeof s_h[i]);
            s_h[i].type = type;
            s_h[i].fd = -1;
            s_h[i].refs = 1;
            return HBASE + i * 4;
        }
    }
    rt_fatal("out of handles");
}

uint32_t handle_new(int type) {
    pthread_mutex_lock(&s_lock);
    uint32_t h = alloc_locked(type);
    pthread_mutex_unlock(&s_lock);
    return h;
}

int handle_type(uint32_t h) {
    HObj *o = get(h);
    return o ? o->type : H_FREE;
}

bool handle_close(uint32_t h) {
    pthread_mutex_lock(&s_lock);
    HObj *o = get(h);
    if (!o) {
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    if (--o->refs > 0) {
        pthread_mutex_unlock(&s_lock);
        return true;
    }
    if (o->type == H_FILE && o->fd >= 0)
        close(o->fd);
    free(o->path);
    free(o->name);
    o->type = H_FREE;
    pthread_mutex_unlock(&s_lock);
    return true;
}

uint32_t handle_new_file(int fd, int oflags, const char *winpath) {
    pthread_mutex_lock(&s_lock);
    uint32_t h = alloc_locked(H_FILE);
    get(h)->fd = fd;
    get(h)->oflags = oflags;
    get(h)->path = strdup(winpath);
    pthread_mutex_unlock(&s_lock);
    return h;
}

int handle_fd(uint32_t h) {
    HObj *o = get(h);
    return o && o->type == H_FILE ? o->fd : -1;
}

uint32_t handle_new_event(bool manual, bool initial) {
    pthread_mutex_lock(&s_lock);
    uint32_t h = alloc_locked(H_EVENT);
    get(h)->manual = manual;
    get(h)->signaled = initial;
    pthread_mutex_unlock(&s_lock);
    return h;
}

void handle_event_set(uint32_t h, bool sig) {
    pthread_mutex_lock(&s_lock);
    HObj *o = get(h);
    if (o)
        o->signaled = sig;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
}

/* ----------------------------------------------------------------- waits */
static __thread uint32_t t_tid;
static uint32_t s_next_tid = 1000;

uint32_t thread_current_id(void) {
    if (!t_tid)
        t_tid = __atomic_add_fetch(&s_next_tid, 4, __ATOMIC_SEQ_CST);
    return t_tid;
}

/* Is the object signaled for this thread?  If so, consume it (auto-reset
   events, mutex ownership). */
static bool try_acquire(HObj *o) {
    switch (o->type) {
    case H_EVENT:
        if (!o->signaled)
            return false;
        if (!o->manual)
            o->signaled = false;
        return true;
    case H_THREAD:
        return o->signaled;
    case H_MUTEX:
        if (o->count && o->owner != thread_current_id())
            return false;
        o->owner = thread_current_id();
        o->count++;
        return true;
    default:
        return true;
    }
}

static void deadline_from_ms(struct timespec *ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* Wait for one (all=false) or all handles.  Returns WAIT_OBJECT_0+i,
   WAIT_TIMEOUT (0x102) or WAIT_FAILED.  Also called by the message-wait
   functions in user32 with a "wake" predicate. */
uint32_t wait_handles(const uint32_t *hs, uint32_t n, bool all, uint32_t ms,
                      bool (*wake)(void)) {
    struct timespec dl;
    if (ms != 0xffffffffu)
        deadline_from_ms(&dl, ms);
    pthread_mutex_lock(&s_lock);
    for (;;) {
        if (snap_should_park() && snap_can_park()) {
            pthread_mutex_unlock(&s_lock);
            if (snap_park())
                return WAIT_RESTART;
            pthread_mutex_lock(&s_lock);
            continue;
        }
        if (all) {
            bool ok = true;
            for (uint32_t i = 0; i < n && ok; i++) {
                HObj *o = get(hs[i]);
                ok = o && (o->type != H_EVENT || o->signaled) &&
                     (o->type != H_THREAD || o->signaled) &&
                     (o->type != H_MUTEX || !o->count || o->owner == thread_current_id());
            }
            if (ok) {
                for (uint32_t i = 0; i < n; i++)
                    try_acquire(get(hs[i]));
                pthread_mutex_unlock(&s_lock);
                return 0;
            }
        } else {
            for (uint32_t i = 0; i < n; i++) {
                HObj *o = get(hs[i]);
                if (!o) {
                    pthread_mutex_unlock(&s_lock);
                    LOGW("wait on invalid handle %08x", hs[i]);
                    return 0xffffffffu;
                }
                if (try_acquire(o)) {
                    pthread_mutex_unlock(&s_lock);
                    return i;
                }
            }
        }
        if (wake && wake()) {
            pthread_mutex_unlock(&s_lock);
            return n;          /* WAIT_OBJECT_0 + n: input available */
        }
        if (ms == 0) {
            pthread_mutex_unlock(&s_lock);
            return 0x102;
        }
        if (wake) {
            /* Poll so window events are noticed promptly. */
            struct timespec shortdl;
            deadline_from_ms(&shortdl, 2);
            if (ms != 0xffffffffu &&
                (shortdl.tv_sec > dl.tv_sec || (shortdl.tv_sec == dl.tv_sec && shortdl.tv_nsec > dl.tv_nsec)))
                shortdl = dl;
            int r = pthread_cond_timedwait(&s_cond, &s_lock, &shortdl);
            if (r == ETIMEDOUT && ms != 0xffffffffu) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                if (now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec)) {
                    pthread_mutex_unlock(&s_lock);
                    return 0x102;
                }
            }
        } else if (ms == 0xffffffffu) {
            pthread_cond_wait(&s_cond, &s_lock);
        } else if (pthread_cond_timedwait(&s_cond, &s_lock, &dl) == ETIMEDOUT) {
            pthread_mutex_unlock(&s_lock);
            return 0x102;
        }
    }
}

/* Waits keep their arguments on the guest stack until they return: if a
   savestate is loaded while the thread is parked inside, wait_handles
   returns WAIT_RESTART and the call starts over with the restored stack. */
void imp_KERNEL32_WaitForSingleObject(CPU *c) {
    uint32_t r;
    do {
        uint32_t h = ARG(0);
        r = wait_handles(&h, 1, false, ARG(1), NULL);
    } while (r == WAIT_RESTART);
    RET(2, r);
}

void imp_KERNEL32_WaitForMultipleObjects(CPU *c) {
    uint32_t r;
    do {
        uint32_t n = ARG(0), arr = ARG(1);
        uint32_t hs[64];
        for (uint32_t i = 0; i < n && i < 64; i++)
            hs[i] = *(u32u *)G2H(arr + 4 * i);
        r = wait_handles(hs, n, ARG(2) != 0, ARG(3), NULL);
    } while (r == WAIT_RESTART);
    RET(4, r);
}

/* Named objects: opening an existing name hands out the same handle value
   with one more reference, which CloseHandle drops. */
static uint32_t find_named_locked(int type, const char *name) {
    if (!name || !*name)
        return 0;
    for (uint32_t i = 1; i < MAX_HANDLES; i++)
        if (s_h[i].type == type && s_h[i].name && !strcmp(s_h[i].name, name)) {
            s_h[i].refs++;
            return HBASE + i * 4;
        }
    return 0;
}

void imp_KERNEL32_CreateEventA(CPU *c) {
    const char *name = ARGS(3);
    pthread_mutex_lock(&s_lock);
    uint32_t h = find_named_locked(H_EVENT, name);
    pthread_mutex_unlock(&s_lock);
    if (h) {
        set_last_error(ERROR_ALREADY_EXISTS);
        RET(4, h);
    }
    h = handle_new_event(ARG(1) != 0, ARG(2) != 0);
    if (name && *name)
        get(h)->name = strdup(name);
    LOGD("CreateEventA(%s) -> %x", name ? name : "", h);
    set_last_error(0);
    RET(4, h);
}

void imp_KERNEL32_SetEvent(CPU *c) {
    handle_event_set(ARG(0), true);
    RET(1, 1);
}

void imp_KERNEL32_ResetEvent(CPU *c) {
    handle_event_set(ARG(0), false);
    RET(1, 1);
}

void imp_KERNEL32_CreateMutexA(CPU *c) {
    const char *name = ARGS(2);
    pthread_mutex_lock(&s_lock);
    uint32_t h = find_named_locked(H_MUTEX, name);
    if (h) {
        pthread_mutex_unlock(&s_lock);
        set_last_error(ERROR_ALREADY_EXISTS);
        RET(3, h);
    }
    h = alloc_locked(H_MUTEX);
    if (ARG(1)) {
        get(h)->owner = thread_current_id();
        get(h)->count = 1;
    }
    if (name && *name)
        get(h)->name = strdup(name);
    pthread_mutex_unlock(&s_lock);
    LOGD("CreateMutexA(%s) -> %x", name ? name : "", h);
    set_last_error(0);
    RET(3, h);
}

/* OpenMutexA(dwDesiredAccess, bInheritHandle, lpName) */
void imp_KERNEL32_OpenMutexA(CPU *c) {
    pthread_mutex_lock(&s_lock);
    uint32_t h = find_named_locked(H_MUTEX, ARGS(2));
    pthread_mutex_unlock(&s_lock);
    set_last_error(h ? 0 : ERROR_FILE_NOT_FOUND);
    RET(3, h);
}

void imp_KERNEL32_ReleaseMutex(CPU *c) {
    pthread_mutex_lock(&s_lock);
    HObj *o = get(ARG(0));
    bool ok = o && o->type == H_MUTEX && o->count && o->owner == thread_current_id();
    if (ok && --o->count == 0)
        o->owner = 0;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    RET(1, ok ? 1 : 0);
}

/* --------------------------------------------------------------- threads */
static uint8_t s_stack_used[(GUEST_STACKS_END - GUEST_STACKS) / THREAD_STACK_SIZE];

typedef struct {
    CPU cpu;
    uint32_t handle, start, param;
    uint32_t stack_lo;
    uint32_t tid;
} ThreadStart;

static void *thread_main(void *arg) {
    ThreadStart *ts = arg;
    CPU *c = &ts->cpu;
    cpu_set_current(c);
    t_tid = ts->tid;
    teb_init(c, ts->stack_lo, ts->stack_lo + THREAD_STACK_SIZE);
    snap_thread_enter(SNAP_THREAD_GUEST, ts->handle, c);
    uint32_t code = guest_call(c, ts->start, 1, &ts->param);
    snap_thread_exit();
    LOGD("thread %x exited with %u", ts->handle, code);
    pthread_mutex_lock(&s_lock);
    HObj *o = get(ts->handle);
    if (o) {
        o->exit_code = code;
        o->signaled = true;
    }
    s_stack_used[(ts->stack_lo - GUEST_STACKS) / THREAD_STACK_SIZE] = 0;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

void imp_KERNEL32_CreateThread(CPU *c) {
    uint32_t start = ARG(2), param = ARG(3), flags = ARG(4), ptid = ARG(5);
    if (flags & 4)
        LOGW("CreateThread: CREATE_SUSPENDED not supported, starting immediately");
    ThreadStart *ts = calloc(1, sizeof *ts);
    pthread_mutex_lock(&s_lock);
    uint32_t slot = 0;
    while (slot < sizeof s_stack_used && s_stack_used[slot])
        slot++;
    if (slot == sizeof s_stack_used)
        rt_fatal("out of guest thread stacks");
    s_stack_used[slot] = 1;
    ts->handle = alloc_locked(H_THREAD);
    /* The id is known before the thread runs: the creator may post to its
       message queue right away (PostThreadMessage), and the game stops
       its worker thread with a WM_QUIT sent that way. */
    ts->tid = __atomic_add_fetch(&s_next_tid, 4, __ATOMIC_SEQ_CST);
    get(ts->handle)->tid = ts->tid;
    pthread_mutex_unlock(&s_lock);
    ts->stack_lo = GUEST_STACKS + slot * THREAD_STACK_SIZE;
    ts->start = start;
    ts->param = param;
    uint32_t h = ts->handle;
    LOGI("CreateThread(start=%08x, param=%08x) -> %x", start, param, h);
    pthread_t pt;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);   /* host stack for deep guest recursion */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&pt, &attr, thread_main, ts);
    pthread_attr_destroy(&attr);
    if (ptid)
        *(u32u *)G2H(ptid) = ts->tid;
    RET(6, h);
}

/* ------------------------------------------------------------------- TLS */
static uint8_t s_tls_used[TLS_SLOTS];
static __thread uint32_t t_tls[TLS_SLOTS];

void imp_KERNEL32_TlsAlloc(CPU *c) {
    pthread_mutex_lock(&s_lock);
    uint32_t i = 0;
    while (i < TLS_SLOTS && s_tls_used[i])
        i++;
    if (i < TLS_SLOTS)
        s_tls_used[i] = 1;
    pthread_mutex_unlock(&s_lock);
    RET(0, i < TLS_SLOTS ? i : 0xffffffffu);
}

void imp_KERNEL32_TlsSetValue(CPU *c) {
    if (ARG(0) >= TLS_SLOTS)
        RET(2, 0);
    t_tls[ARG(0)] = ARG(1);
    RET(2, 1);
}

void imp_KERNEL32_TlsGetValue(CPU *c) {
    set_last_error(0);
    RET(1, ARG(0) < TLS_SLOTS ? t_tls[ARG(0)] : 0);
}

void thread_locals_export(uint32_t tls[TLS_SLOTS], uint32_t *tid) {
    memcpy(tls, t_tls, sizeof t_tls);
    *tid = thread_current_id();
}

void thread_locals_import(const uint32_t tls[TLS_SLOTS], uint32_t tid) {
    memcpy(t_tls, tls, sizeof t_tls);
    t_tid = tid;
}

uint32_t stack_slot_count(void) { return sizeof s_stack_used; }
bool stack_slot_used(uint32_t slot) { return slot < sizeof s_stack_used && s_stack_used[slot]; }

/* ------------------------------------------------------ critical sections
 * Host recursive mutexes, found by the guest CRITICAL_SECTION address. */
typedef struct {
    uint32_t addr;
    pthread_mutex_t m;
} CritSec;

#define MAX_CS 1024
static CritSec s_cs[MAX_CS];
static uint32_t s_cs_count;
/* Held by this thread.  A savestate is only taken while every thread holds
   none, so the mutexes themselves never need saving. */
static __thread int t_cs_held;

int thread_cs_held(void) { return t_cs_held; }

static pthread_mutex_t *cs_find(uint32_t addr, bool create) {
    pthread_mutex_lock(&s_lock);
    for (uint32_t i = 0; i < s_cs_count; i++) {
        if (s_cs[i].addr == addr) {
            pthread_mutex_unlock(&s_lock);
            return &s_cs[i].m;
        }
    }
    if (!create) {
        pthread_mutex_unlock(&s_lock);
        return NULL;
    }
    if (s_cs_count == MAX_CS)
        rt_fatal("too many critical sections");
    CritSec *cs = &s_cs[s_cs_count++];
    cs->addr = addr;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a);
    pthread_mutexattr_destroy(&a);
    pthread_mutex_unlock(&s_lock);
    return &cs->m;
}

void imp_KERNEL32_InitializeCriticalSection(CPU *c) {
    memset(G2H(ARG(0)), 0, 24);
    cs_find(ARG(0), true);
    RET(1, 0);
}

void imp_KERNEL32_DeleteCriticalSection(CPU *c) { RET(1, 0); }

void imp_KERNEL32_EnterCriticalSection(CPU *c) {
    pthread_mutex_t *m = cs_find(ARG(0), true);
    c->esp += 8;
    pthread_mutex_lock(m);
    t_cs_held++;
}

void imp_KERNEL32_LeaveCriticalSection(CPU *c) {
    pthread_mutex_t *m = cs_find(ARG(0), false);
    if (m && pthread_mutex_unlock(m) == 0)
        t_cs_held--;
    RET(1, 0);
}

/* --------------------------------------------------------------- modules */
typedef struct {
    const char *dll, *name;
    GuestFn fn;
} Export;

static Export s_exports[256];
static int s_nexports;
static const char *s_known_dlls[] = {
    "KERNEL32", "USER32", "GDI32", "ADVAPI32", "OLE32", "OLEAUT32", "WINMM",
    "SHFOLDER", "D3D8", "DINPUT8", "DSOUND", "DDRAW", "DINPUT",
};

static void dll_base(const char *name, char *out, size_t sz) {
    const char *b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    size_t i = 0;
    for (; b[i] && b[i] != '.' && i + 1 < sz; i++)
        out[i] = (char)toupper((unsigned char)b[i]);
    out[i] = 0;
}

uint32_t module_handle_for(const char *name) {
    char base[64];
    dll_base(name, base, sizeof base);
    for (size_t i = 0; i < sizeof s_known_dlls / sizeof *s_known_dlls; i++)
        if (!strcmp(base, s_known_dlls[i]))
            return 0x70000000u + (uint32_t)i * 0x10000u;
    return 0;
}

void module_register_export(const char *dll, const char *name, GuestFn fn) {
    if (s_nexports < 256)
        s_exports[s_nexports++] = (Export){dll, name, fn};
}

GuestFn module_export_fn(const char *name) {
    for (int i = 0; i < s_nexports; i++)
        if (!strcmp(s_exports[i].name, name))
            return s_exports[i].fn;
    return NULL;
}

uint32_t module_proc(uint32_t module, const char *name) {
    uint32_t idx = (module - 0x70000000u) / 0x10000u;
    if (module < 0x70000000u || idx >= sizeof s_known_dlls / sizeof *s_known_dlls)
        return 0;
    const char *dll = s_known_dlls[idx];
    for (int i = 0; i < s_nexports; i++)
        if (!strcasecmp(s_exports[i].dll, dll) && !strcmp(s_exports[i].name, name))
            return thunk_register(s_exports[i].fn, s_exports[i].name);
    /* Fall back to the game's own import table. */
    for (uint32_t i = 0; i < g_import_count; i++) {
        char b[64];
        dll_base(g_import_table[i].dll, b, sizeof b);
        if (!strcmp(b, dll) && !strcmp(g_import_table[i].name, name))
            return *(u32u *)G2H(g_import_table[i].iat);
    }
    return 0;
}

/* ------------------------------------------------- host-owned guest threads */
/* Host threads that call back into guest code (multimedia timers, sound
   notifications) need their own guest stack and TEB. */
typedef struct {
    void (*fn)(CPU *c, void *arg);
    void *arg;
    uint32_t stack_lo;
} HostStart;

static void *host_thread_main(void *p) {
    HostStart hs = *(HostStart *)p;
    free(p);
    CPU *c = calloc(1, sizeof *c);
    cpu_set_current(c);
    teb_init(c, hs.stack_lo, hs.stack_lo + THREAD_STACK_SIZE);
    hs.fn(c, hs.arg);
    pthread_mutex_lock(&s_lock);
    s_stack_used[(hs.stack_lo - GUEST_STACKS) / THREAD_STACK_SIZE] = 0;
    pthread_mutex_unlock(&s_lock);
    free(c);
    return NULL;
}

bool host_thread_start(void (*fn)(CPU *c, void *arg), void *arg, pthread_t *out) {
    HostStart *hs = malloc(sizeof *hs);
    hs->fn = fn;
    hs->arg = arg;
    pthread_mutex_lock(&s_lock);
    uint32_t slot = 0;
    while (slot < sizeof s_stack_used && s_stack_used[slot])
        slot++;
    if (slot == sizeof s_stack_used)
        rt_fatal("out of guest thread stacks");
    s_stack_used[slot] = 1;
    pthread_mutex_unlock(&s_lock);
    hs->stack_lo = GUEST_STACKS + slot * THREAD_STACK_SIZE;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t pt;
    bool ok = pthread_create(&pt, &attr, host_thread_main, hs) == 0;
    pthread_attr_destroy(&attr);
    if (out)
        *out = pt;
    else if (ok)
        pthread_detach(pt);
    return ok;
}

/* -------------------------------------------------------------- savestates
 * HNDL: the handle table, TLS slot and stack slot allocation.  Files are
 * reopened by path at their saved offset; critical sections are host
 * mutexes that nobody holds at a safe point, so they need nothing. */
static void handles_wake(void) {
    pthread_mutex_lock(&s_lock);
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
}

static void handles_save(SnapBuf *b) {
    pthread_mutex_lock(&s_lock);
    uint32_t n = 0;
    for (uint32_t i = 1; i < MAX_HANDLES; i++)
        n += s_h[i].type != H_FREE;
    sb_u32(b, n);
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        HObj *o = &s_h[i];
        if (o->type == H_FREE)
            continue;
        sb_u32(b, i);
        sb_u32(b, (uint32_t)o->type);
        uint8_t fl[2] = {o->manual, o->signaled};
        sb_put(b, fl, 2);
        sb_u32(b, o->owner);
        sb_u32(b, o->count);
        sb_u32(b, o->exit_code);
        sb_u32(b, o->tid);
        sb_u32(b, (uint32_t)o->refs);
        sb_str(b, o->path);
        sb_str(b, o->name);
        int64_t ofs = -1;
        uint32_t flags = 0;
        if (o->type == H_FILE && o->fd >= 0) {
            ofs = lseek(o->fd, 0, SEEK_CUR);
            flags = (uint32_t)o->oflags;
        }
        SB_PUT(b, ofs);
        sb_u32(b, flags);
    }
    sb_put(b, s_tls_used, sizeof s_tls_used);
    sb_put(b, s_stack_used, sizeof s_stack_used);
    sb_u32(b, s_next_tid);
    pthread_mutex_unlock(&s_lock);
}

static void handles_drop(void) {
    pthread_mutex_lock(&s_lock);
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        HObj *o = &s_h[i];
        if (o->type == H_FILE && o->fd >= 0)
            close(o->fd);
        free(o->path);
        free(o->name);
        memset(o, 0, sizeof *o);
    }
    pthread_mutex_unlock(&s_lock);
}

static bool handles_load(SnapBuf *b) {
    pthread_mutex_lock(&s_lock);
    uint32_t n = sb_get_u32(b);
    for (uint32_t k = 0; k < n && !b->err; k++) {
        uint32_t i = sb_get_u32(b);
        if (!i || i >= MAX_HANDLES) {
            b->err = true;
            break;
        }
        HObj *o = &s_h[i];
        o->type = (int)sb_get_u32(b);
        uint8_t fl[2];
        sb_get(b, fl, 2);
        o->manual = fl[0];
        o->signaled = fl[1];
        o->owner = sb_get_u32(b);
        o->count = sb_get_u32(b);
        o->exit_code = sb_get_u32(b);
        o->tid = sb_get_u32(b);
        o->refs = (int)sb_get_u32(b);
        o->path = sb_get_str(b);
        o->name = sb_get_str(b);
        int64_t ofs;
        SB_GET(b, ofs);
        uint32_t flags = sb_get_u32(b);
        o->fd = -1;
        o->oflags = (int)(flags & (O_ACCMODE | O_APPEND));
        if (o->type == H_FILE && o->path && ofs >= 0) {
            char host[1200];
            if (path_to_host(o->path, host, sizeof host))
                o->fd = hd_open(host, o->oflags);
            if (o->fd >= 0)
                lseek(o->fd, (off_t)ofs, SEEK_SET);
            else
                LOGW("savestate: cannot reopen %s", o->path);
        }
    }
    sb_get(b, s_tls_used, sizeof s_tls_used);
    sb_get(b, s_stack_used, sizeof s_stack_used);
    uint32_t next = sb_get_u32(b);
    if (next > s_next_tid)
        s_next_tid = next;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    return !b->err;
}

static const SnapSection s_handles_section = {
    SNAP_TAG('H', 'N', 'D', 'L'), SNAP_ORDER_KERNEL, handles_save, NULL, handles_drop, handles_load,
};

__attribute__((constructor)) static void sync_snap_register(void) {
    snap_section(&s_handles_section);
    snap_add_waker(handles_wake);
}
