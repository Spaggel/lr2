/* WINMM: time functions, multimedia timers, and empty mixer/MCI devices. */
#include "../rt.h"
#include "handles.h"
#include "../savestate.h"
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define MMSYSERR_NOERROR 0
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_INVALPARAM 11
#define TIME_PERIODIC 0x0001
#define TIME_CALLBACK_EVENT_SET 0x0010
#define TIME_CALLBACK_EVENT_PULSE 0x0020

void imp_WINMM_timeGetTime(CPU *c) { RET(0, win_tick_ms()); }
void imp_WINMM_timeBeginPeriod(CPU *c) { RET(1, MMSYSERR_NOERROR); }
void imp_WINMM_timeEndPeriod(CPU *c) { RET(1, MMSYSERR_NOERROR); }

void imp_WINMM_timeGetDevCaps(CPU *c) {
    if (ARG(1) >= 8) {
        *(u32u *)G2H(ARG(0)) = 1;          /* wPeriodMin */
        *(u32u *)G2H(ARG(0) + 4) = 1000000; /* wPeriodMax */
    }
    RET(2, MMSYSERR_NOERROR);
}

/* ---------------------------------------------------------------- timers */
#define MAX_TIMERS 16

typedef struct {
    bool used, stop;
    uint32_t id, delay, proc, user, flags;
    pthread_t thread;
} Timer;

static Timer s_timers[MAX_TIMERS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static uint32_t s_next_id = 1;
static __thread uint32_t t_timer_id;   /* id of the timer running on this thread */

static void timer_thread(CPU *c, void *arg) {
    Timer *t = arg;
    t_timer_id = t->id;
    snap_thread_enter(SNAP_THREAD_TIMER, t->id, c);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    pthread_mutex_lock(&s_lock);
    for (;;) {
        next.tv_nsec += (long)t->delay * 1000000L;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        /* A callback that overran by more than a period skips the missed
           ticks rather than firing them back to back. */
        struct timespec now0;
        clock_gettime(CLOCK_MONOTONIC, &now0);
        if ((now0.tv_sec - next.tv_sec) * 1000000000LL + (now0.tv_nsec - next.tv_nsec) >
            (long long)t->delay * 1000000LL)
            next = now0;
        while (!t->stop) {
            /* condvar is realtime-clocked; convert the monotonic deadline */
            struct timespec now, rt;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long left = (next.tv_sec - now.tv_sec) * 1000000000LL + (next.tv_nsec - now.tv_nsec);
            if (left <= 0)
                break;
            clock_gettime(CLOCK_REALTIME, &rt);
            rt.tv_sec += left / 1000000000LL;
            rt.tv_nsec += left % 1000000000LL;
            if (rt.tv_nsec >= 1000000000L) {
                rt.tv_nsec -= 1000000000L;
                rt.tv_sec++;
            }
            pthread_cond_timedwait(&s_cond, &s_lock, &rt);
        }
        if (t->stop)
            break;
        if (snap_should_park()) {
            /* Between callbacks this thread holds no guest state; a load
               may change the timer's settings. */
            pthread_mutex_unlock(&s_lock);
            snap_park();
            pthread_mutex_lock(&s_lock);
            if (t->stop)
                break;
        }
        uint32_t flags = t->flags, proc = t->proc, user = t->user, id = t->id;
        pthread_mutex_unlock(&s_lock);
        if (flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE))
            handle_event_set(proc, true);   /* pulse approximated as set */
        else {
            uint32_t args[5] = {id, 0, user, 0, 0};
            guest_call(c, proc, 5, args);
        }
        pthread_mutex_lock(&s_lock);
        if (!(flags & TIME_PERIODIC))
            break;
    }
    t->used = false;
    pthread_mutex_unlock(&s_lock);
    snap_thread_exit();
}

/* timeSetEvent(uDelay, uResolution, lpTimeProc, dwUser, fuEvent) */
void imp_WINMM_timeSetEvent(CPU *c) {
    pthread_mutex_lock(&s_lock);
    Timer *t = NULL;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!s_timers[i].used) {
            t = &s_timers[i];
            break;
        }
    }
    if (!t) {
        pthread_mutex_unlock(&s_lock);
        RET(5, 0);
    }
    *t = (Timer){.used = true, .id = s_next_id++, .delay = ARG(0) ? ARG(0) : 1,
                 .proc = ARG(2), .user = ARG(3), .flags = ARG(4)};
    uint32_t id = t->id;
    pthread_mutex_unlock(&s_lock);
    LOGI("timeSetEvent(%u ms, proc=%08x, flags=%x) -> %u", ARG(0), ARG(2), ARG(4), id);
    host_thread_start(timer_thread, t, NULL);
    RET(5, id);
}

void imp_WINMM_timeKillEvent(CPU *c) {
    uint32_t id = ARG(0), r = 97;   /* TIMERR_NOCANDO */
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &s_timers[i];
        if (t->used && t->id == id && !t->stop) {
            t->stop = true;
            pthread_cond_broadcast(&s_cond);
            /* Like Windows, wait for a running callback to finish unless we
               are that callback. */
            while (t->used && t_timer_id != id) {
                pthread_mutex_unlock(&s_lock);
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
                pthread_mutex_lock(&s_lock);
            }
            r = 0;
        }
    }
    pthread_mutex_unlock(&s_lock);
    RET(1, r);
}

/* -------------------------------------------------------------- savestates
 * TIMR: timer settings.  The timer threads themselves must already match
 * (the THRD check compares them by id), so loading only updates settings. */
static void timers_wake(void) {
    pthread_mutex_lock(&s_lock);
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
}

static void timers_save(SnapBuf *b) {
    pthread_mutex_lock(&s_lock);
    sb_u32(b, s_next_id);
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &s_timers[i];
        uint32_t live = t->used && !t->stop;
        sb_u32(b, live);
        if (live) {
            sb_u32(b, t->id);
            sb_u32(b, t->delay);
            sb_u32(b, t->proc);
            sb_u32(b, t->user);
            sb_u32(b, t->flags);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

static bool timers_load(SnapBuf *b) {
    pthread_mutex_lock(&s_lock);
    s_next_id = sb_get_u32(b);
    for (int i = 0; i < MAX_TIMERS && !b->err; i++) {
        if (!sb_get_u32(b))
            continue;
        uint32_t id = sb_get_u32(b);
        for (int k = 0; k < MAX_TIMERS; k++) {
            Timer *t = &s_timers[k];
            if (t->used && t->id == id) {
                t->delay = sb_get_u32(b);
                t->proc = sb_get_u32(b);
                t->user = sb_get_u32(b);
                t->flags = sb_get_u32(b);
                id = 0;
                break;
            }
        }
        if (id)
            b->err = true;   /* the thread check guarantees a match */
    }
    pthread_mutex_unlock(&s_lock);
    return !b->err;
}

static const SnapSection s_timers_section = {
    SNAP_TAG('T', 'I', 'M', 'R'), SNAP_ORDER_KERNEL, timers_save, NULL, NULL, timers_load,
};

__attribute__((constructor)) static void winmm_snap_register(void) {
    snap_section(&s_timers_section);
    snap_add_waker(timers_wake);
}

/* ----------------------------------------------------------------- mixer */
/* No mixer devices: the game falls back to its own volume handling. */
void imp_WINMM_mixerGetNumDevs(CPU *c) { RET(0, 0); }
void imp_WINMM_mixerOpen(CPU *c) { RET(5, MMSYSERR_BADDEVICEID); }
void imp_WINMM_mixerClose(CPU *c) { RET(1, MMSYSERR_NOERROR); }
void imp_WINMM_mixerGetDevCapsA(CPU *c) { RET(3, MMSYSERR_BADDEVICEID); }
void imp_WINMM_mixerGetLineInfoA(CPU *c) { RET(3, MMSYSERR_BADDEVICEID); }
void imp_WINMM_mixerGetLineControlsA(CPU *c) { RET(3, MMSYSERR_BADDEVICEID); }
void imp_WINMM_mixerGetControlDetailsA(CPU *c) { RET(3, MMSYSERR_BADDEVICEID); }
void imp_WINMM_mixerSetControlDetails(CPU *c) { RET(3, MMSYSERR_BADDEVICEID); }

/* mciSendCommandA(IDDevice, uMsg, fdwCommand, dwParam) */
void imp_WINMM_mciSendCommandA(CPU *c) {
    LOGD("mciSendCommandA(dev=%u, msg=%#x, flags=%#x)", ARG(0), ARG(1), ARG(2));
    RET(4, 263);   /* MCIERR_INVALID_DEVICE_NAME: no CD audio device */
}
