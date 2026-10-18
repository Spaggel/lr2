/* Indirect-call dispatch, traps, string instructions and x87 helpers. */
#include "rt.h"
#include "savestate.h"
#include "os.h"
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <execinfo.h>
#include <unistd.h>

PeInfo g_pe;
static GuestFn *s_fn_by_addr;      /* indexed by guest address - text_start */

void dispatch_init(void) {
    uint32_t span = g_pe.text_end - g_pe.text_start;
    s_fn_by_addr = calloc(span, sizeof *s_fn_by_addr);
    for (uint32_t i = 0; i < g_func_count; i++) {
        uint32_t a = g_func_table[i].addr;
        if (a >= g_pe.text_start && a < g_pe.text_end)
            s_fn_by_addr[a - g_pe.text_start] = g_func_table[i].fn;
    }
}

static GuestFn lookup(uint32_t t) {
    if (t >= g_pe.text_start && t < g_pe.text_end)
        return s_fn_by_addr[t - g_pe.text_start];
    return thunk_lookup(t);
}

void call_indirect(CPU *c, uint32_t t) {
    GuestFn fn = lookup(t);
    if (!fn) {
        LOGE("call_indirect: no function at %08x (return address %08x)", t, *(u32u *)(g_mem + c->esp));
        TRAP(c, t, "indirect call to unknown address");
    }
    fn(c);
}

uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args) {
    uint32_t saved_esp = c->esp;
    for (int i = nargs - 1; i >= 0; i--) {
        c->esp -= 4;
        *(u32u *)(g_mem + c->esp) = args[i];
    }
    c->esp -= 4;
    *(u32u *)(g_mem + c->esp) = GUEST_THUNKS_END - 16;   /* never executed */
    call_indirect(c, addr);
    c->esp = saved_esp;
    return c->eax;
}

/* ------------------------------------------------------------------- traps */
static void dump_state(CPU *c) {
    fprintf(stderr,
            "  eax=%08x ecx=%08x edx=%08x ebx=%08x\n"
            "  esp=%08x ebp=%08x esi=%08x edi=%08x  ftop=%u fcw=%04x\n",
            c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi,
            c->ftop, c->fcw);
    /* Heuristic guest backtrace: stack words that point just past a call. */
    fprintf(stderr, "  guest stack (likely return addresses):");
    int n = 0;
    for (uint32_t a = c->esp; a < c->esp + 0x2000 && n < 24; a += 4) {
        uint32_t v = *(u32u *)(g_mem + a);
        if (v >= g_pe.text_start + 5 && v < g_pe.text_end) {
            uint8_t *p = g_mem + v;
            if (p[-5] == 0xe8 || (p[-6] == 0xff && (p[-5] & 0x38) == 0x10) ||
                (p[-2] == 0xff && (p[-1] & 0x38) == 0x10) ||
                (p[-3] == 0xff && (p[-2] & 0x38) == 0x10)) {
                fprintf(stderr, "%s %08x", n % 8 ? "" : "\n   ", v);
                n++;
            }
        }
    }
    fprintf(stderr, "\n");
}

/* Terminate after a guest fault.  abort() only when asked to, since core
   dumps of a 4 GiB mapping take ages. */
_Noreturn static void die(void) {
    fflush(stderr);
    if (getenv("LR2_ABORT"))
        abort();
    _exit(3);
}

_Noreturn void TRAP(CPU *c, uint32_t addr, const char *what) {
    fprintf(stderr, "\n*** guest trap at %08x: %s\n", addr, what);
    dump_state(c);
    die();
}

_Noreturn void BADJUMP(CPU *c, uint32_t addr, uint32_t target) {
    fprintf(stderr, "\n*** unresolved jump-table target %08x at %08x\n", target, addr);
    dump_state(c);
    die();
}

_Noreturn void NORETURN(CPU *c, uint32_t addr) {
    fprintf(stderr, "\n*** call at %08x returned from a noreturn function\n", addr);
    dump_state(c);
    die();
}

_Noreturn void DIVZERO(CPU *c, uint32_t addr) {
    fprintf(stderr, "\n*** integer divide by zero at %08x\n", addr);
    dump_state(c);
    die();
}

static void host_fault(const char *what, void *addr) {
    uint8_t *a = addr;
    fprintf(stderr, "\n*** %s", what);
    if (a)
        fprintf(stderr, ", address %p", (void *)a);
    if (a >= g_mem && a < g_mem + (1ull << 32) + 0x10000)
        fprintf(stderr, " (guest address %08x)", (uint32_t)(a - g_mem));
    fprintf(stderr, "\n");
    CPU *c = cpu_current();
    if (c)
        dump_state(c);
}

#ifndef _WIN32
/* SIGUSR1: print where the main thread is (host backtrace names the
   recompiled guest functions, f_XXXXXXXX), for diagnosing hangs. */
static pthread_t s_main_thread;

static void usr1_handler(int sig) {
    if (!pthread_equal(pthread_self(), s_main_thread)) {
        pthread_kill(s_main_thread, sig);
        return;
    }
    fprintf(stderr, "\n*** SIGUSR1: main thread state\n");
    void *bt[48];
    int n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, 2);
    CPU *c = cpu_current();
    if (c)
        dump_state(c);
}
#endif

void install_crash_handlers(void) {
#ifndef _WIN32
    s_main_thread = pthread_self();
    signal(SIGUSR1, usr1_handler);
#endif
    os_on_fault(host_fault);
}

/* ------------------------------------------------------------- cpuid etc. */
void do_cpuid(CPU *c) {
    switch (c->eax) {
    case 0:
        c->eax = 1;
        c->ebx = 0x756e6547;   /* "GenuineIntel" */
        c->edx = 0x49656e69;
        c->ecx = 0x6c65746e;
        break;
    case 1:
        c->eax = 0x00000673;   /* Pentium III */
        c->ebx = 0;
        c->ecx = 0;
        c->edx = 0x00008111;   /* FPU, TSC, CX8, CMOV */
        break;
    default:
        c->eax = c->ebx = c->ecx = c->edx = 0;
    }
}

void do_rdtsc(CPU *c) {
    /* Pretend to be a 1 GHz part. */
    uint64_t t = guest_time_ns();
    c->eax = (uint32_t)t;
    c->edx = (uint32_t)(t >> 32);
}

/* -------------------------------------------------------------- strings */
#define GM8(a)  (*(uint8_t *)(g_mem + (uint32_t)(a)))
#define GM16(a) (*(u16u *)(g_mem + (uint32_t)(a)))
#define GM32(a) (*(u32u *)(g_mem + (uint32_t)(a)))

static inline uint32_t rd(uint32_t a, int size) {
    return size == 1 ? GM8(a) : size == 2 ? GM16(a) : GM32(a);
}

static inline void wr(uint32_t a, int size, uint32_t v) {
    if (size == 1)
        GM8(a) = (uint8_t)v;
    else if (size == 2)
        GM16(a) = (uint16_t)v;
    else
        GM32(a) = v;
}

void str_movs(CPU *c, int size, int rep) {
    int32_t step = c->df ? -size : size;
    if (!rep) {
        wr(c->edi, size, rd(c->esi, size));
        c->esi += step;
        c->edi += step;
        return;
    }
    uint32_t n = c->ecx;
    if (!n)
        return;
    uint32_t bytes = n * size;
    if (!c->df && (c->edi + bytes <= c->esi || c->esi + bytes <= c->edi)) {
        memcpy(g_mem + c->edi, g_mem + c->esi, bytes);
        c->esi += bytes;
        c->edi += bytes;
    } else {
        /* Overlapping or backwards: element-wise, as the hardware does. */
        for (uint32_t i = 0; i < n; i++) {
            wr(c->edi, size, rd(c->esi, size));
            c->esi += step;
            c->edi += step;
        }
    }
    c->ecx = 0;
}

void str_stos(CPU *c, int size, int rep) {
    int32_t step = c->df ? -size : size;
    uint32_t v = size == 1 ? (uint8_t)c->eax : size == 2 ? (uint16_t)c->eax : c->eax;
    if (!rep) {
        wr(c->edi, size, v);
        c->edi += step;
        return;
    }
    uint32_t n = c->ecx;
    if (!c->df && size == 1) {
        memset(g_mem + c->edi, (int)v, n);
        c->edi += n;
    } else if (!c->df && size == 4 && (v == 0 || v == 0xffffffffu)) {
        memset(g_mem + c->edi, (int)(v & 0xff), (size_t)n * 4);
        c->edi += n * 4;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            wr(c->edi, size, v);
            c->edi += step;
        }
    }
    c->ecx = 0;
}

void str_lods(CPU *c, int size, int rep) {
    int32_t step = c->df ? -size : size;
    uint32_t n = rep ? c->ecx : 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = rd(c->esi, size);
        if (size == 1)
            c->eax = (c->eax & 0xffffff00u) | v;
        else if (size == 2)
            c->eax = (c->eax & 0xffff0000u) | v;
        else
            c->eax = v;
        c->esi += step;
    }
    if (rep)
        c->ecx = 0;
}

/* Flags of a - b at the given operand size. */
static void cmp_flags(CPU *c, uint32_t a, uint32_t b, int size) {
    int bits = size * 8;
    uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    uint32_t r = (a - b) & mask;
    c->cf = a < b;
    c->zf = r == 0;
    c->sf = (r >> (bits - 1)) & 1;
    c->of = (((a ^ b) & (a ^ r)) >> (bits - 1)) & 1;
    c->pf = PARITY(r);
}

void str_scas(CPU *c, int size, int rep) {
    int32_t step = c->df ? -size : size;
    uint32_t acc = size == 1 ? (uint8_t)c->eax : size == 2 ? (uint16_t)c->eax : c->eax;
    if (!rep) {
        cmp_flags(c, acc, rd(c->edi, size), size);
        c->edi += step;
        return;
    }
    while (c->ecx) {
        uint32_t v = rd(c->edi, size);
        c->edi += step;
        c->ecx--;
        cmp_flags(c, acc, v, size);
        if (rep == 1 && !c->zf)      /* repe */
            break;
        if (rep == 2 && c->zf)       /* repne */
            break;
    }
}

void str_cmps(CPU *c, int size, int rep) {
    int32_t step = c->df ? -size : size;
    if (!rep) {
        cmp_flags(c, rd(c->esi, size), rd(c->edi, size), size);
        c->esi += step;
        c->edi += step;
        return;
    }
    while (c->ecx) {
        uint32_t a = rd(c->esi, size), b = rd(c->edi, size);
        c->esi += step;
        c->edi += step;
        c->ecx--;
        cmp_flags(c, a, b, size);
        if (rep == 1 && !c->zf)
            break;
        if (rep == 2 && c->zf)
            break;
    }
}

/* ------------------------------------------------------------------- x87 */
double f80_load(uint8_t *m, uint32_t a) {
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, m + a, 8);
    memcpy(&se, m + a + 8, 2);
    int sign = se >> 15;
    int exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0)
        v = 0.0;
    else if (exp == 0x7fff)
        v = (mant << 1) ? NAN : INFINITY;
    else
        v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void f80_store(uint8_t *m, uint32_t a, double v) {
    uint64_t mant = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) {
        se |= 0x7fff;
        mant = 0xc000000000000000ull;
    } else if (isinf(v)) {
        se |= 0x7fff;
        mant = 0x8000000000000000ull;
    } else if (v != 0.0) {
        int e;
        double f = frexp(fabs(v), &e);        /* f in [0.5, 1) */
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(m + a, &mant, 8);
    memcpy(m + a + 8, &se, 2);
}

void fpu_fxam(CPU *c) {
    double v = ST(0);
    uint32_t f = c->fsw & ~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (signbit(v))
        f |= FSW_C1;
    if (isnan(v))
        f |= FSW_C0;
    else if (isinf(v))
        f |= FSW_C0 | FSW_C2;
    else if (v == 0.0)
        f |= FSW_C3;
    else if (fpclassify(v) == FP_SUBNORMAL)
        f |= FSW_C2 | FSW_C3;
    else
        f |= FSW_C2;
    c->fsw = f;
}

/* 28-byte protected-mode environment. */
void fpu_stenv(CPU *c, uint32_t a) {
    memset(g_mem + a, 0, 28);
    GM16(a) = (uint16_t)c->fcw;
    GM16(a + 4) = (uint16_t)((c->fsw & 0xc7ffu) | ((c->ftop & 7) << 11));
    GM16(a + 8) = 0;   /* tag word: all valid */
}

void fpu_ldenv(CPU *c, uint32_t a) {
    c->fcw = GM16(a);
    uint16_t sw = GM16(a + 4);
    c->fsw = sw & 0xc7ffu;
    c->ftop = (sw >> 11) & 7;
}

void fpu_fnsave(CPU *c, uint32_t a) {
    fpu_stenv(c, a);
    for (int i = 0; i < 8; i++)
        f80_store(g_mem, a + 28 + i * 10, ST(i));
    fpu_init(c);
}

void fpu_frstor(CPU *c, uint32_t a) {
    fpu_ldenv(c, a);
    for (int i = 0; i < 8; i++)
        ST(i) = f80_load(g_mem, a + 28 + i * 10);
}
