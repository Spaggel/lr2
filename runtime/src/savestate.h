/* Savestates.
 *
 * Guest calls are host calls, so every guest frame on a thread's stack has a
 * matching host C frame.  A snapshot is therefore only taken, and only
 * restored, while each thread sits at a known point:
 *
 *   main thread      PeekMessageA (the game's main loop calls it every frame)
 *   guest threads    inside a wait (wait_handles), which restarts afterwards
 *   timer threads    between callbacks
 *
 * At those points the host frames above the guest state hold nothing but
 * return addresses, so swapping guest memory and CPU registers underneath
 * them is safe as long as the host call chains match; the chains are stored
 * in the snapshot and compared before anything is changed.
 *
 * Host-side state (COM objects, handles, timers, ...) is written by
 * "sections" that subsystems register at startup.
 */
#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <stddef.h>
#include "rt.h"

/* ------------------------------------------------------------ byte buffer */
typedef struct {
    uint8_t *data;
    size_t len, cap, pos;
    bool err;                 /* a read ran past the end */
} SnapBuf;

void sb_put(SnapBuf *b, const void *p, size_t n);
void sb_u32(SnapBuf *b, uint32_t v);
void sb_str(SnapBuf *b, const char *s);          /* NULL allowed */
bool sb_get(SnapBuf *b, void *p, size_t n);
uint32_t sb_get_u32(SnapBuf *b);
char *sb_get_str(SnapBuf *b);                    /* malloc'd, may be NULL */
void sb_free(SnapBuf *b);
#define SB_PUT(b, v) sb_put((b), &(v), sizeof(v))
#define SB_GET(b, v) sb_get((b), &(v), sizeof(v))

/* --------------------------------------------------------------- sections */
#define SNAP_TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

typedef struct {
    uint32_t tag;
    int order;                            /* load order, ascending */
    void (*save)(SnapBuf *b);
    bool (*check)(SnapBuf *b);            /* optional: can this be loaded?  no side effects */
    void (*drop)(void);                   /* optional: release host state before loading */
    bool (*load)(SnapBuf *b);
} SnapSection;

void snap_section(const SnapSection *s);

/* Load order of the built-in sections. */
enum {
    SNAP_ORDER_MEMORY = 10,   /* guest memory, VA tables, thunks */
    SNAP_ORDER_KERNEL = 20,   /* handles, threads, clock */
    SNAP_ORDER_COM = 30,      /* COM objects (all subsystems) */
    SNAP_ORDER_LATE = 40,     /* subsystem globals that refer to COM objects */
};

/* Called with every subsystem lock held by the snapshot code; subsystems
   whose state other host threads touch (the audio mixer) take it here. */
typedef struct {
    void (*lock)(void);
    void (*unlock)(void);
} SnapLock;
void snap_add_lock(const SnapLock *l);

/* ---------------------------------------------------------------- threads */
enum { SNAP_THREAD_MAIN, SNAP_THREAD_GUEST, SNAP_THREAD_TIMER };

/* Threads that run guest code register so the world can be stopped.  key
   identifies the thread across runs (guest threads: their handle). */
void snap_thread_enter(int role, uint32_t key, CPU *c);
void snap_thread_exit(void);

/* Cheap check for worker threads; call snap_park() at a park point when it
   returns true.  snap_park() returns true if the state was replaced while
   parked, in which case the caller must restart the interrupted call. */
extern volatile int g_snap_stop;
static inline bool snap_should_park(void) { return __atomic_load_n(&g_snap_stop, __ATOMIC_ACQUIRE) != 0; }
bool snap_park(void);
bool snap_can_park(void);          /* registered worker (not the main thread) */

/* Main thread safepoint (top of PeekMessageA). */
void snap_safepoint(CPU *c);

/* Wake hooks: called after g_snap_stop is set so blocked threads notice. */
void snap_add_waker(void (*wake)(void));

/* ------------------------------------------------------------------ misc */
void snap_init(void);
void snap_request_save(int slot);
void snap_request_load(int slot);
/* Block until background slot writes are on disk (before _exit). */
void snap_wait_writers(void);

/* Guest clock (ns), shared by every time source so a load can rewind it. */
uint64_t guest_time_ns(void);
void guest_time_set(uint64_t ns);

#endif
