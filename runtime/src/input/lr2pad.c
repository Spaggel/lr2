/* Modern gamepad controls for LEGO Racers 2.

   The game binds input per player from a control config that defaults to
   the keyboard, so out of the box a gamepad only worked in the menus.  When
   player N (N < 2) has a keyboard config and gamepad N is connected, the
   race bindings are replaced (lr2pad_race_bindings, which wraps the game's
   binding builder at 0x40ad60) with ones that read the gamepad's virtual
   steering axis and race-action buttons.  Those merge the gamepad with the
   player's own keys, so the keyboard keeps working:

     steer          left stick, d-pad left/right, the player's steer keys
     accelerate     RT                  brake           B
     reverse        LT                  fire weapon     A
     brick boost    X                   talk            Y
     look behind    LB, RS click        next target     RB, d-pad down
     prev. target   d-pad up            pause           Start

   Players with a gamepad config chosen in the game's options keep the
   game's own bindings.  In the menus A confirms, B goes back, Start pauses
   and the d-pad navigates (dinput8.c merges it into the left stick); the
   menu button table at 0x53060c is patched for that. */
#include "../rt.h"
#include "lr2pad.h"

#define OPTIONS_PTR     0x0053e3bcu   /* game options; player configs at +0xb4 */
#define CFG_FIRST       0xb4u
#define CFG_SIZE        0xacu         /* 43 dwords */
#define CFG_KEYBOARD    1             /* cfg[1], the device kind */
#define RACE_ACTIONS    10            /* 0-8 from the config, 9 = pause */
#define DIK_ESCAPE      0x01

#define MENU_PAD_BUTTONS 0x0053060cu  /* joystick button of menu actions 0-8 */
#define INPUT_MGR_PTR   0x00549e80u   /* input manager; vtable +0x10 = device(n, kind) */
#define JOY_NBUTTONS    0xa0u         /* game joystick: DIDEVCAPS.dwButtons */

static inline uint32_t rd32(uint32_t a) { return *(u32u *)G2H(a); }
static inline void wr32(uint32_t a, uint32_t v) { *(u32u *)G2H(a) = v; }

/* Guest address of player's control config, or 0. */
static uint32_t player_cfg(int player) {
    if (player < 0 || player > 1)
        return 0;
    uint32_t opt = rd32(OPTIONS_PTR);
    if (opt < GUEST_IMAGE || opt >= GUEST_HEAP_END - 0x1000)
        return 0;
    return opt + CFG_FIRST + (uint32_t)player * CFG_SIZE;
}

/* DirectInput key code of a config (kind, code) pair; 0 if it is not a
   key (kinds 1 and 2 are axis halves). */
static uint8_t cfg_key(uint32_t cfg, int pair) {
    uint32_t kind = rd32(cfg + 4u * (uint32_t)pair), code = rd32(cfg + 4u * (uint32_t)pair + 4);
    return kind != 1 && kind != 2 && code < 256 ? (uint8_t)code : 0;
}

static bool player_key(int player, int action) {
    uint32_t cfg = player_cfg(player);
    if (!cfg || rd32(cfg + 4) != CFG_KEYBOARD)
        return false;
    uint8_t k = action == 9 ? DIK_ESCAPE : cfg_key(cfg, 7 + 2 * action);
    return k && di_key_down(k);
}

static bool player_steer_key(int player, int right) {
    uint32_t cfg = player_cfg(player);
    if (!cfg || rd32(cfg + 4) != CFG_KEYBOARD)
        return false;
    uint8_t k = cfg_key(cfg, right ? 5 : 3);
    return k && di_key_down(k);
}

static bool btn(SDL_Gamepad *gp, SDL_GamepadButton b) { return SDL_GetGamepadButton(gp, b); }
static bool trig(SDL_Gamepad *gp, SDL_GamepadAxis a) {
    return SDL_GetGamepadAxis(gp, a) > PAD_TRIGGER_THRESHOLD;
}

static bool race_pad(SDL_Gamepad *gp, int action) {
    switch (action) {
    case 0: return trig(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    case 1: return btn(gp, SDL_GAMEPAD_BUTTON_EAST);
    case 2: return trig(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    case 3: return btn(gp, SDL_GAMEPAD_BUTTON_SOUTH);
    case 4: return btn(gp, SDL_GAMEPAD_BUTTON_WEST);
    case 5: return btn(gp, SDL_GAMEPAD_BUTTON_NORTH);
    case 6: return btn(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || btn(gp, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    case 7: return btn(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || btn(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    case 8: return btn(gp, SDL_GAMEPAD_BUTTON_DPAD_UP);
    case 9: return btn(gp, SDL_GAMEPAD_BUTTON_START);
    }
    return false;
}

#define RSTICK_THRESHOLD 16000

bool lr2pad_button(SDL_Gamepad *gp, int slot, int button) {
    int rx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX);
    int ry = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY);
    switch (button) {
    case PADB_RS_DOWN: return ry > RSTICK_THRESHOLD;
    case PADB_RS_UP: return ry < -RSTICK_THRESHOLD;
    case PADB_RS_LEFT: return rx < -RSTICK_THRESHOLD;
    case PADB_RS_RIGHT: return rx > RSTICK_THRESHOLD;
    }
    int a = button - PADB_RACE;
    if (a < 0 || a >= RACE_ACTIONS)
        return false;
    return race_pad(gp, a) || player_key(slot, a);
}

/* Steering: the left stick (with a dead zone) plus digital steering from
   the d-pad and the player's keys, which eases in and out like the game's
   own keyboard steering. */
#define STEER_DEADZONE 0.15f
#define STEER_RISE 6.0f     /* digital steering: full lock in 1/6 s */
#define STEER_FALL 8.0f     /* back to centre in 1/8 s */

static struct {
    float digital;
    uint64_t last_ns;
} s_steer[4];

float lr2pad_steer(SDL_Gamepad *gp, int slot) {
    if (slot < 0 || slot >= (int)(sizeof s_steer / sizeof *s_steer))
        return 0.0f;
    float x = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    float ax = SDL_fabsf(x);
    ax = ax <= STEER_DEADZONE ? 0.0f : SDL_min((ax - STEER_DEADZONE) / (1.0f - STEER_DEADZONE), 1.0f);
    x = x < 0 ? -ax : ax;

    int dir = (btn(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || player_steer_key(slot, 1)) -
              (btn(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || player_steer_key(slot, 0));
    uint64_t now = SDL_GetTicksNS();
    float dt = s_steer[slot].last_ns ? (float)(now - s_steer[slot].last_ns) / 1e9f : 0.0f;
    s_steer[slot].last_ns = now;
    dt = SDL_min(dt, 0.1f);
    float d = s_steer[slot].digital;
    if (dir)
        d = SDL_clamp(d + (float)dir * STEER_RISE * dt, -1.0f, 1.0f);
    else
        d = d > 0 ? SDL_max(d - STEER_FALL * dt, 0.0f) : SDL_min(d + STEER_FALL * dt, 0.0f);
    s_steer[slot].digital = d;
    return SDL_clamp(x + d, -1.0f, 1.0f);
}

/* Binding (FUN_004b7da0's argument): type (0 axis, 3 action), index,
   device kind (3 joystick), device number, input kind (3 analog axis,
   6 button), code.  The controller holds up to 14 from +0x18; +0x14 is
   the count. */
static void add_binding(uint32_t ctrl, uint32_t type, uint32_t idx, uint32_t dev, uint32_t input,
                        uint32_t code) {
    uint32_t n = rd32(ctrl + 0x14), b = ctrl + 0x18 + n * 24;
    uint32_t v[6] = {type, idx, 3, dev, input, code};
    for (int i = 0; i < 6; i++)
        wr32(b + 4u * (uint32_t)i, v[i]);
    wr32(ctrl + 0x14, n + 1);
}

/* The game's object for joystick n (thiscall manager->device(n, 3)), or 0. */
static uint32_t game_joystick(CPU *c, int n) {
    uint32_t mgr = rd32(INPUT_MGR_PTR);
    if (!mgr)
        return 0;
    uint32_t args[2] = {(uint32_t)n, 3};
    c->ecx = mgr;
    return guest_call(c, rd32(rd32(mgr) + 0x10), 2, args);
}

/* Wraps the game's FUN_0040ad60 (thiscall, ecx = player config, arg =
   controller, ret 4), which binds a racer's controls from its config. */
void lr2pad_race_bindings(CPU *c, void (*orig)(CPU *restrict c)) {
    uint32_t cfg = c->ecx, ctrl = ARG(0);
    int player = -1;
    for (int p = 0; p < 2; p++)
        if (cfg && cfg == player_cfg(p))
            player = p;
    int joy = player >= 0 && rd32(cfg + 4) == CFG_KEYBOARD ? di_pad_game_index(player) : -1;
    if (joy >= 0) {
        /* A savestate from before the virtual buttons existed restores a
           joystick that has fewer. */
        uint32_t dev = game_joystick(c, joy);
        if (!dev || rd32(dev + JOY_NBUTTONS) < PADB_COUNT)
            joy = -1;
        c->ecx = cfg;
    }
    if (joy < 0) {
        orig(c);
        return;
    }
    LOGI("player %d: gamepad %d and keyboard controls", player + 1, joy);
    wr32(ctrl + 0x14, 0);
    add_binding(ctrl, 0, 0, (uint32_t)joy, 3, PAD_STEER_AXIS);
    for (uint32_t a = 0; a < RACE_ACTIONS; a++)
        add_binding(ctrl, 3, a, (uint32_t)joy, 6, PADB_RACE + a);
    c->esp += 8;
}

/* Menu actions: 0 confirm, 1 back, 2-5 the keyboard's numpad 2/8/4/6
   (here the right stick), 6 the pause/escape button (the game also shows
   it as such), 7 and 8 joystick only. */
void lr2pad_init(void) {
    static const uint32_t orig[9] = {0, 4, 5, 2, 6, 7, 8, 1, 3};
    static const uint32_t mine[9] = {PADB_A, PADB_B, PADB_RS_DOWN, PADB_RS_UP, PADB_RS_LEFT,
                                     PADB_RS_RIGHT, PADB_START, PADB_X, PADB_Y};
    for (int i = 0; i < 9; i++) {
        if (rd32(MENU_PAD_BUTTONS + 4u * (uint32_t)i) != orig[i]) {
            LOGW("menu gamepad table not found; menu buttons unchanged");
            return;
        }
    }
    for (int i = 0; i < 9; i++)
        wr32(MENU_PAD_BUTTONS + 4u * (uint32_t)i, mine[i]);
}
