/* Shared definitions for recompiled guest code and the runtime. */
#ifndef RECOMP_H
#define RECOMP_H

#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    /* Flags are function-local in generated code; these slots carry them
       across tail calls and out of string-compare helpers. */
    uint32_t cf, zf, sf, of, pf;
    uint32_t df;
    uint32_t eflags_hi;          /* bits written by popfd (ID flag etc.) */
    /* x87 */
    double st[8];
    uint32_t ftop;
    uint32_t fcw;
    uint32_t fsw;                /* condition bits C0..C3 (TOP kept in ftop) */
    uint32_t fs_base;            /* guest address of this thread's TEB */
    void *thread;                /* runtime per-thread data */
} CPU;

/* Base of the 4 GiB guest address space. */
extern uint8_t *g_mem;

typedef uint16_t u16u __attribute__((aligned(1), may_alias));
typedef uint32_t u32u __attribute__((aligned(1), may_alias));
typedef uint64_t u64u __attribute__((aligned(1), may_alias));
typedef float f32u __attribute__((aligned(1), may_alias));
typedef double f64u __attribute__((aligned(1), may_alias));

/* Runtime values used by single-instruction patches (overrides.py PATCHES). */
uint32_t lr2_native_w(void);
uint32_t lr2_native_h(void);
double lr2_hor_fov(float deg);

/* Generated functions keep a local copy `m` of g_mem. */
#define M8(a)   (*(uint8_t *)(m + (uint32_t)(a)))
#define M16(a)  (*(u16u *)(m + (uint32_t)(a)))
#define M32(a)  (*(u32u *)(m + (uint32_t)(a)))
#define M64(a)  (*(u64u *)(m + (uint32_t)(a)))
#define MF32(a) (*(f32u *)(m + (uint32_t)(a)))
#define MF64(a) (*(f64u *)(m + (uint32_t)(a)))

/* Host-side pointer to guest memory, for runtime code. */
#define GPTR(a) ((void *)(g_mem + (uint32_t)(a)))

#define PUSH32(v) do { uint32_t _v = (v); c->esp -= 4; M32(c->esp) = _v; } while (0)
#define PARITY(r) ((uint32_t)!__builtin_parity((uint32_t)(r) & 0xffu))

#define MAKE_EFLAGS(c, cf, pf, zf, sf, of) \
    (((c)->eflags_hi & ~0xcd5u) | 0x202u | (cf) | ((pf) << 2) | ((zf) << 6) | ((sf) << 7) | \
     ((c)->df << 10) | ((of) << 11))

typedef void (*GuestFn)(CPU *c);

/* Runtime services used by generated code. */
void call_indirect(CPU *c, uint32_t target);
_Noreturn void TRAP(CPU *c, uint32_t addr, const char *what);
_Noreturn void BADJUMP(CPU *c, uint32_t addr, uint32_t target);
_Noreturn void NORETURN(CPU *c, uint32_t addr);
_Noreturn void DIVZERO(CPU *c, uint32_t addr);
void do_cpuid(CPU *c);
void do_rdtsc(CPU *c);

/* ---------------------------------------------------------------- strings */
void str_movs(CPU *c, int size, int rep);
void str_stos(CPU *c, int size, int rep);
void str_lods(CPU *c, int size, int rep);
void str_scas(CPU *c, int size, int rep);
void str_cmps(CPU *c, int size, int rep);
#define STR_MOVS(c, s, r) str_movs(c, s, r)
#define STR_STOS(c, s, r) str_stos(c, s, r)
#define STR_LODS(c, s, r) str_lods(c, s, r)
#define STR_SCAS(c, s, r) str_scas(c, s, r)
#define STR_CMPS(c, s, r) str_cmps(c, s, r)

/* ------------------------------------------------------------------- x87 */
#define ST(i) (c->st[(c->ftop + (i)) & 7])
#define FPUSH(v) do { double _v = (v); c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = _v; } while (0)
#define FPOP() (c->ftop = (c->ftop + 1) & 7)
#define FSW_C0 0x0100u
#define FSW_C1 0x0200u
#define FSW_C2 0x0400u
#define FSW_C3 0x4000u
#define FCOM(a, b) fpu_compare(c, (a), (b))
#define FSTSW() ((uint16_t)((c->fsw & 0xc7ffu) | ((c->ftop & 7) << 11)))

static inline void fpu_compare(CPU *c, double a, double b) {
    uint32_t f = c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3);
    if (a > b) {
    } else if (a < b) {
        f |= FSW_C0;
    } else if (a == b) {
        f |= FSW_C3;
    } else {
        f |= FSW_C0 | FSW_C2 | FSW_C3;
    }
    c->fsw = f;
}

/* Round according to the control word's RC field. */
static inline double fpu_round(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);    /* host default mode is round-to-nearest-even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static inline int64_t fpu_toint64(CPU *c, double v) {
    double r = fpu_round(c, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return INT64_MIN;
    return (int64_t)r;
}

static inline int32_t fpu_toint32(CPU *c, double v) {
    double r = fpu_round(c, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return INT32_MIN;
    return (int32_t)r;
}

static inline int16_t fpu_toint16(CPU *c, double v) {
    double r = fpu_round(c, v);
    if (!(r >= -32768.0 && r <= 32767.0))
        return INT16_MIN;
    return (int16_t)r;
}

static inline void fpu_init(CPU *c) {
    c->fcw = 0x37f;
    c->fsw = 0;
    c->ftop = 0;
}

double f80_load(uint8_t *m, uint32_t a);
void f80_store(uint8_t *m, uint32_t a, double v);
void fpu_fxam(CPU *c);
void fpu_ldenv(CPU *c, uint32_t a);
void fpu_stenv(CPU *c, uint32_t a);
void fpu_fnsave(CPU *c, uint32_t a);
void fpu_frstor(CPU *c, uint32_t a);

/* -------------------------------------------------------- function table */
typedef struct FuncEntry {
    uint32_t addr;
    GuestFn fn;
} FuncEntry;

extern const FuncEntry g_func_table[];
extern const uint32_t g_func_count;

typedef struct ImportEntry {
    uint32_t iat;               /* guest address of the IAT slot */
    const char *dll;
    const char *name;
    GuestFn fn;
} ImportEntry;

extern const ImportEntry g_import_table[];
/* The original executable's bytes (headers and data sections), see recomp.py. */
extern const uint8_t g_image[];
extern const uint32_t g_image_size;
extern const uint32_t g_import_count;

#endif
