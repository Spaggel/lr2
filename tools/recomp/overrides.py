"""Hand-written replacements for guest functions (by entry address).

Used for CRT routines whose machine code is awkward to recompile (computed
jumps with negative table indices) or that are hot enough to deserve a host
implementation.  Each value maps a C function name to its full definition.

WRAPS keeps the recompiled function (renamed to <name>_orig) and routes
calls through a runtime function instead:
    void host(CPU *c, void (*orig)(CPU *restrict c))
which either calls orig(c) or does the work itself, popping the return
address and stack arguments like the original.
"""


def _memmove(name):
    # cdecl void *memmove(void *dst, const void *src, size_t n)
    return f'''void {name}(CPU *restrict c) {{
    uint8_t *const m = g_mem;
    uint32_t d = M32(c->esp + 4), s = M32(c->esp + 8), n = M32(c->esp + 12);
    memmove(m + d, m + s, n);
    c->eax = d;
    c->esp += 4;
}}
'''


OVERRIDES = {
    0x0051ae40: _memmove,   # memcpy (MSVC's handles overlap like memmove)
    0x00521290: _memmove,   # memmove
}


WRAPS = {
    0x0040ad60: 'lr2pad_race_bindings',   # builds a racer's input bindings
}


# Single instructions replaced by C statements (by instruction address).
# Native resolution and Hor+ widescreen, after alphayellow1's
# LegoRacers2WidescreenFix; the lr2_* functions live in runtime/src/d3d8/d3d8.c.
PATCHES = {
    # Display mode used until the player picks one (saved in LR2.cfg):
    # the desktop's native size, 32-bit instead of 640x480x16.
    0x0049b5d6: 'c->eax = lr2_native_w();',
    0x0049b5db: 'c->edx = lr2_native_h();',
    0x0049b5eb: 'c->esi = 32;',
    # The engine only accepts a few 4:3 modes; its 800x600 entry becomes native.
    0x0042a911: 'zf = (c->eax == lr2_native_w());',
    0x0042a918: 'zf = (c->ebp == lr2_native_h());',
    # Camera horizontal FOV (degrees) when width > height: widen for Hor+.
    0x004ba425: 'ST(0) = ST(0) * lr2_hor_fov(MF32((uint32_t)(c->esi + 0x1a4u)));',
}
