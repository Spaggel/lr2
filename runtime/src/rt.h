/* Internal runtime interfaces. */
#ifndef RT_H
#define RT_H

#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/* ------------------------------------------------------------ guest layout */
#define GUEST_PEB          0x00010000u
#define GUEST_ENV          0x00020000u    /* env block + command line */
#define GUEST_MAIN_STACK_LO 0x00100000u
#define GUEST_MAIN_STACK_HI 0x00300000u
#define GUEST_IMAGE        0x00400000u
#define GUEST_THUNKS       0x00800000u    /* host-function thunks, 16 bytes each */
#define GUEST_THUNKS_END   0x00900000u
#define GUEST_STACKS       0x01000000u    /* secondary thread stacks */
#define GUEST_STACKS_END   0x04000000u
#define GUEST_VA           0x04000000u    /* VirtualAlloc arena */
#define GUEST_VA_END       0x20000000u
#define GUEST_HEAP         0x20000000u    /* HeapAlloc / runtime allocations */
#define GUEST_HEAP_END     0x70000000u

/* -------------------------------------------------------------- logging */
extern int g_log_level;
extern bool g_windowed;     /* --windowed or LR2_WINDOWED: no full screen */
void rt_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGE(...) rt_log(0, __VA_ARGS__)
#define LOGW(...) rt_log(1, __VA_ARGS__)
#define LOGI(...) rt_log(2, __VA_ARGS__)
#define LOGD(...) rt_log(3, __VA_ARGS__)
_Noreturn void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ------------------------------------------------- import call convention */
static inline const char *gstr(uint32_t a) { return a ? (const char *)(g_mem + a) : NULL; }
/* Runtime implementations of guest-visible functions are called exactly like
   guest code: [esp] = return address, args above it.  They must pop the
   return address and (stdcall) their arguments. */
#define ARG(n)      (*(u32u *)(g_mem + c->esp + 4 + 4 * (n)))
#define ARGP(n)     ((void *)(g_mem + ARG(n)))          /* arg as host pointer */
#define ARGS(n)     gstr(ARG(n))
#define RET(nargs, v) do { c->eax = (uint32_t)(v); c->esp += 4 + 4 * (nargs); return; } while (0)
#define RET_CDECL(v) do { c->eax = (uint32_t)(v); c->esp += 4; return; } while (0)
#define G2H(a)      ((void *)(g_mem + (uint32_t)(a)))
#define H2G(p)      ((uint32_t)((uint8_t *)(p) - g_mem))

/* ------------------------------------------------------------- memory */
void mem_init(void);
uint32_t thunk_register(GuestFn fn, const char *name);
GuestFn thunk_lookup(uint32_t addr);
const char *thunk_name(uint32_t addr);
extern bool g_trace_thunks;

/* Per-frame hooks, run after each Present on the presenting thread
   (savestates, scripted input, debugging). */
typedef void (*FrameHook)(CPU *c, uint32_t frame);
void frame_hook_add(FrameHook h);
void frame_hooks_run(CPU *c, uint32_t frame);
void autoinput_init(void);
void thunk_stats_dump(void);
void debug_backtrace(const char *label);
extern bool g_autoinput_bt;

/* Guest heap: returns guest addresses (0 on failure). */
void heap_init(void);
uint32_t gmalloc(uint32_t size);
uint32_t gcalloc(uint32_t size);
void gfree(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gmsize(uint32_t p);
uint32_t gstrdup(const char *s);
uint32_t heap_top(void);       /* start of the never-allocated tail */

/* VirtualAlloc arena */
uint32_t va_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect);
bool va_free(uint32_t addr, uint32_t size, uint32_t type);
extern uint8_t g_va_commit[];    /* one byte per committed 4 KiB page */

/* ------------------------------------------------------------ PE image */
typedef struct {
    uint32_t base, entry, size;
    uint32_t text_start, text_end;
} PeInfo;
extern PeInfo g_pe;
void pe_load(void);

/* setup.c: copy the player's CD into <prog_dir>/game; false if they quit. */
bool setup_run(const char *src, const char *prog_dir);

/* ----------------------------------------------------------- dispatch */
void dispatch_init(void);
/* Call guest code at addr (stdcall/cdecl) with nargs dword args from the
   host.  Returns eax.  Caller-cleanup is handled: esp is restored. */
uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args);

void install_crash_handlers(void);
_Noreturn void unimplemented_import(CPU *c, const char *name);

/* ------------------------------------------------------------ threads */
CPU *cpu_current(void);
void cpu_set_current(CPU *c);
void teb_init(CPU *c, uint32_t stack_lo, uint32_t stack_hi);
#define TLS_SLOTS 64
#define THREAD_STACK_SIZE 0x100000u
/* Guest thread-local state of the calling thread (savestates). */
void thread_locals_export(uint32_t tls[TLS_SLOTS], uint32_t *tid);
void thread_locals_import(const uint32_t tls[TLS_SLOTS], uint32_t tid);
/* Critical sections the calling thread holds (it must not park while > 0). */
int thread_cs_held(void);
uint32_t stack_slot_count(void);
bool stack_slot_used(uint32_t slot);
GuestFn module_export_fn(const char *name);

/* -------------------------------------------------------------- paths */
extern char g_game_dir[1024];     /* host directory holding the game files */
extern char g_cd_dir[1024];       /* host directory with CD contents */
extern char g_save_dir[1024];     /* host directory for saves/config */
/* Translate a Windows path to a host path (case-insensitive lookup). */
bool path_to_host(const char *win, char *out, size_t outsz);
void mkdir_p(const char *host);

/* HD texture pack (hdtex.c): served from inside the game archive. */
void hd_init(const char *dir);        /* dir NULL: look for "HD TEXTURES" */
int hd_open(const char *host, int flags);                 /* open() */
size_t hd_fill(int fd, uint8_t *buf, size_t n, uint64_t off); /* after a short read */
bool hd_size(int fd, uint64_t *size);

/* Win32 last error (per thread) */
void set_last_error(uint32_t e);
uint32_t get_last_error(void);

#endif
