/* Button prompts: gamepad glyphs in place of keyboard keys.
 *
 * Menu texts carry icon tokens ("~~UPDOWN Select ~~X Confirm ~~T Back");
 * the icon manager (vtable 0x52f57c) holds one entry per token, each with
 * a texture handle at +0x20 that the text drawer copies every frame.  Its
 * FUN_00433680 loads the ten icons (table at 0x53fcf0) and FUN_00433700
 * releases them.  Both are wrapped (overrides.py WRAPS): after the load,
 * the gamepad versions of the swapped icons are loaded too, from archive
 * names the PC game never uses (the PS2 version's button icons, which the
 * runtime serves from the "PAD GLYPHS" folder, see hdtex.c and
 * tools/make_pad_glyphs.py); before the release, the keyboard handles are
 * put back and the gamepad ones released.  After each frame the entries
 * get the set that matches the last input (dinput8.c), so the prompts
 * change as soon as the player switches between pad and keyboard.
 *
 *   ~~X  Enter -> A          ~~UPDOWN     up/down arrows    -> d-pad up/down
 *   ~~T  Esc   -> B          ~~LEFTRIGHT  left/right arrows -> d-pad left/right
 *   ~~FRENCHT Ech -> B       ~~UDLR       four arrows       -> d-pad
 *
 * ~~BACK (Backspace) and the d-pad direction icons are left alone.
 * LR2_PROMPTS=pad or LR2_PROMPTS=keys fixes the choice.
 */
#include "rt.h"
#include "savestate.h"
#include "input/lr2pad.h"

#define TEXMGR_PTR 0x00549dfcu   /* texture manager */
#define TEX_LOAD   0x004c0b70u   /* thiscall (path, 0, -1, 1, 1) -> handle, -1 if missing */
#define TEX_FREE   0x004c0ee0u   /* thiscall (handle, 1) */
#define NONE       0xffffffffu

static const struct {
    uint32_t icon;
    const char *path;
} s_swaps[] = {
    {0, "Game Data\\Frontend\\Ch Builder Bits\\cross.tga"},
    {1, "Game Data\\Frontend\\Ch Builder Bits\\circle.tga"},
    {2, "Game Data\\Frontend\\Ch Builder Bits\\circle.tga"},
    {4, "Game Data\\Frontend\\Ch Builder Bits\\l1.tga"},
    {5, "Game Data\\Frontend\\Ch Builder Bits\\l2.tga"},
    {6, "Game Data\\Frontend\\Ch Builder Bits\\r1.tga"},
};
#define NSWAPS (sizeof s_swaps / sizeof *s_swaps)

static uint32_t s_mgr;            /* icon manager with our handles loaded, or 0 */
static uint32_t s_key[NSWAPS], s_pad[NSWAPS];
static int s_shown;               /* 1: the entries hold the gamepad set */
static bool s_hooked;

static inline uint32_t rd32(uint32_t a) { return *(u32u *)G2H(a); }
static inline void wr32(uint32_t a, uint32_t v) { *(u32u *)G2H(a) = v; }

/* Address of icon i's texture handle. */
static uint32_t handle_slot(uint32_t mgr, uint32_t i) { return rd32(mgr + 4 + 4 * i) + 0x20; }

static bool want_pad(void) {
    const char *f = getenv("LR2_PROMPTS");
    if (f && !strcmp(f, "pad"))
        return true;
    if (f && !strcmp(f, "keys"))
        return false;
    return di_using_pad();
}

static void show(int pad) {
    for (size_t k = 0; k < NSWAPS; k++) {
        uint32_t h = pad ? s_pad[k] : s_key[k];
        if (h != NONE)
            wr32(handle_slot(s_mgr, s_swaps[k].icon), h);
    }
    s_shown = pad;
}

static void free_pad_set(CPU *c) {
    for (size_t k = 0; k < NSWAPS; k++) {
        if (s_pad[k] == NONE)
            continue;
        uint32_t args[2] = {s_pad[k], 1};
        c->ecx = rd32(TEXMGR_PTR);
        guest_call(c, TEX_FREE, 2, args);
        s_pad[k] = NONE;
    }
}

static void frame_hook(CPU *c, uint32_t frame) {
    (void)c;
    (void)frame;
    if (s_mgr && want_pad() != (s_shown != 0))
        show(!s_shown);
}

/* Wraps FUN_00433680 (thiscall, ecx = icon manager): loads the icons. */
void lr2_glyphs_load(CPU *c, void (*orig)(CPU *restrict c)) {
    uint32_t mgr = c->ecx;
    if (s_mgr) {
        /* Loaded again without a release: the game overwrites its own
           handles; drop the gamepad set so it is not leaked. */
        uint32_t saved = c->ecx;
        if (s_shown)
            show(0);
        free_pad_set(c);
        s_mgr = 0;
        c->ecx = saved;
    }
    orig(c);
    if (!s_hooked) {
        frame_hook_add(frame_hook);
        s_hooked = true;
    }
    int loaded = 0;
    for (size_t k = 0; k < NSWAPS; k++) {
        s_key[k] = rd32(handle_slot(mgr, s_swaps[k].icon));
        uint32_t path = gstrdup(s_swaps[k].path);
        uint32_t args[5] = {path, 0, NONE, 1, 1};
        c->ecx = rd32(TEXMGR_PTR);
        s_pad[k] = guest_call(c, TEX_LOAD, 5, args);
        gfree(path);
        loaded += s_pad[k] != NONE;
    }
    s_mgr = mgr;
    s_shown = 0;
    LOGI("prompts: %d of %zu gamepad glyphs loaded", loaded, NSWAPS);
    if (want_pad())
        show(1);
}

/* Wraps FUN_00433700 (thiscall, ecx = icon manager): releases the icons. */
void lr2_glyphs_free(CPU *c, void (*orig)(CPU *restrict c)) {
    uint32_t mgr = c->ecx;
    if (s_mgr == mgr) {
        if (s_shown)
            show(0);
        free_pad_set(c);
        s_mgr = 0;
    }
    c->ecx = mgr;
    orig(c);
}

/* Savestates: the handles live in guest memory, which a load restores, so
   only this module's view of them is saved. */
static void glyphs_save(SnapBuf *b) {
    sb_u32(b, s_mgr);
    sb_u32(b, (uint32_t)s_shown);
    sb_put(b, s_key, sizeof s_key);
    sb_put(b, s_pad, sizeof s_pad);
}

static bool glyphs_load(SnapBuf *b) {
    s_mgr = sb_get_u32(b);
    s_shown = (int)sb_get_u32(b);
    sb_get(b, s_key, sizeof s_key);
    sb_get(b, s_pad, sizeof s_pad);
    return !b->err;
}

static const SnapSection s_glyphs_section = {
    SNAP_TAG('G', 'L', 'Y', 'P'), SNAP_ORDER_LATE, glyphs_save, NULL, NULL, glyphs_load,
};

__attribute__((constructor)) static void glyphs_snap_register(void) {
    snap_section(&s_glyphs_section);
}
