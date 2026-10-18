/* Per-thread guest state: CPU, TEB, last-error. */
#include "rt.h"

static __thread CPU *t_cpu;
static __thread uint32_t t_last_error;

CPU *cpu_current(void) { return t_cpu; }
void cpu_set_current(CPU *c) { t_cpu = c; }
void set_last_error(uint32_t e) { t_last_error = e; }
uint32_t get_last_error(void) { return t_last_error; }

/* Minimal TEB: SEH chain, stack bounds, self pointer, PEB. */
void teb_init(CPU *c, uint32_t stack_lo, uint32_t stack_hi) {
    uint32_t teb = gcalloc(0x1000);
    *(u32u *)(g_mem + teb + 0x00) = 0xffffffffu;   /* SEH chain end */
    *(u32u *)(g_mem + teb + 0x04) = stack_hi;
    *(u32u *)(g_mem + teb + 0x08) = stack_lo;
    *(u32u *)(g_mem + teb + 0x18) = teb;
    *(u32u *)(g_mem + teb + 0x30) = GUEST_PEB;
    c->fs_base = teb;
    c->esp = stack_hi - 16;
    fpu_init(c);
}
