/* Frame hooks and scripted input for automated testing.
 *
 * LR2_AUTOINPUT="600:Return,700:Down,750:Return,900:shot,2000:quit" presses
 * the named SDL key (held for HOLD frames) when Present has been called that
 * many times; "shot" saves the next frame like LR2_DUMP_FRAMES, "quit" exits,
 * "bt" logs the host call chain at Present and at the next PeekMessage,
 * "save:N" / "load:N" save or load savestate slot N.  "Up/300" holds a key
 * for 300 frames instead of HOLD.  Commands fire once,
 * even when a load rewinds the frame counter past them again.
 *
 * "pad:A", "pad:RT/300", "pad:LX=-0.6/100" drive a virtual SDL gamepad
 * (attached when the script has a pad step): buttons A B X Y LB RB Back
 * Start Guide LS RS Up Down Left Right, axes LX LY RX RY (=-1..1) and
 * triggers LT RT (=0..1, default fully pressed).
 */
#define _GNU_SOURCE   /* dladdr */
#include <SDL3/SDL.h>
#include "../rt.h"
#include "../savestate.h"
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>

#define MAX_HOOKS 8
#define HOLD 4

static FrameHook s_hooks[MAX_HOOKS];
static int s_nhooks;

void frame_hook_add(FrameHook h) {
    if (s_nhooks < MAX_HOOKS)
        s_hooks[s_nhooks++] = h;
}

void frame_hooks_run(CPU *c, uint32_t frame) {
    for (int i = 0; i < s_nhooks; i++)
        s_hooks[i](c, frame);
}

typedef struct {
    uint32_t frame;
    SDL_Scancode sc;   /* SDL_SCANCODE_UNKNOWN for commands */
    uint32_t hold;
    char cmd[8];
    int arg;
    bool done;
    int pad_ctl;       /* pad steps: SDL button, or PAD_AXIS + SDL axis */
    int16_t pad_val;
} Step;

#define PAD_AXIS 100

static Step s_steps[256];
static int s_nsteps;
bool g_autoinput_shot;

static void push_key(SDL_Scancode sc, bool down) {
    SDL_Event e = {0};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    e.key.down = down;
    e.key.windowID = 0;
    SDL_PushEvent(&e);
}

bool g_autoinput_bt;

/* ------------------------------------------------------- virtual gamepad */
static SDL_Joystick *s_vpad;

static void vpad_attach(void) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x045e;    /* looks like an Xbox controller */
    desc.product_id = 0x02ea;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "LR2 virtual gamepad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (!id || !(s_vpad = SDL_OpenJoystick(id)))
        LOGW("autoinput: virtual gamepad: %s", SDL_GetError());
}

static int pad_ctl_from_name(const char *n) {
    static const struct { const char *n; int ctl; } tab[] = {
        {"A", SDL_GAMEPAD_BUTTON_SOUTH}, {"B", SDL_GAMEPAD_BUTTON_EAST},
        {"X", SDL_GAMEPAD_BUTTON_WEST}, {"Y", SDL_GAMEPAD_BUTTON_NORTH},
        {"LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"Back", SDL_GAMEPAD_BUTTON_BACK}, {"Start", SDL_GAMEPAD_BUTTON_START},
        {"Guide", SDL_GAMEPAD_BUTTON_GUIDE}, {"LS", SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {"RS", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"Up", SDL_GAMEPAD_BUTTON_DPAD_UP},
        {"Down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"Left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {"Right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"LX", PAD_AXIS + SDL_GAMEPAD_AXIS_LEFTX}, {"LY", PAD_AXIS + SDL_GAMEPAD_AXIS_LEFTY},
        {"RX", PAD_AXIS + SDL_GAMEPAD_AXIS_RIGHTX}, {"RY", PAD_AXIS + SDL_GAMEPAD_AXIS_RIGHTY},
        {"LT", PAD_AXIS + SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
        {"RT", PAD_AXIS + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    };
    for (size_t i = 0; i < sizeof tab / sizeof *tab; i++)
        if (!strcmp(n, tab[i].n))
            return tab[i].ctl;
    return -1;
}

static void vpad_set(const Step *st, bool on) {
    if (!s_vpad)
        return;
    if (st->pad_ctl >= PAD_AXIS)
        SDL_SetJoystickVirtualAxis(s_vpad, st->pad_ctl - PAD_AXIS, on ? st->pad_val : 0);
    else
        SDL_SetJoystickVirtualButton(s_vpad, st->pad_ctl, on);
}

/* Log the host call chain (recompiled guest functions show as f_XXXXXXXX). */
void debug_backtrace(const char *label) {
    void *pcs[64];
    int n = backtrace(pcs, 64);
    LOGI("backtrace at %s:", label);
    for (int i = 1; i < n; i++) {
        Dl_info di;
        if (dladdr(pcs[i], &di) && di.dli_sname)
            LOGI("  %s+0x%lx", di.dli_sname, (unsigned long)((char *)pcs[i] - (char *)di.dli_saddr));
        else
            LOGI("  %p", pcs[i]);
    }
}

/* Steps count Presents in this process, not the device's frame counter,
   which a savestate load rewinds. */
static void autoinput_frame(CPU *c, uint32_t dev_frame) {
    (void)c;
    (void)dev_frame;
    static uint32_t frame;
    frame++;
    for (int i = 0; i < s_nsteps; i++) {
        Step *st = &s_steps[i];
        if (!strcmp(st->cmd, "pad")) {
            if (frame == st->frame)
                vpad_set(st, true);
            else if (frame == st->frame + st->hold)
                vpad_set(st, false);
        } else if (st->sc != SDL_SCANCODE_UNKNOWN) {
            if (frame == st->frame)
                push_key(st->sc, true);
            else if (frame == st->frame + st->hold)
                push_key(st->sc, false);
        } else if (frame == st->frame && !st->done) {
            st->done = true;
            if (!strcmp(st->cmd, "save"))
                snap_request_save(st->arg);
            else if (!strcmp(st->cmd, "load"))
                snap_request_load(st->arg);
            else if (!strcmp(st->cmd, "shot"))
                g_autoinput_shot = true;
            else if (!strcmp(st->cmd, "bt")) {
                debug_backtrace("Present");
                g_autoinput_bt = true;    /* and at the next PeekMessage */
            }
            else if (!strcmp(st->cmd, "close")) {
                /* As if the user closed the window: exercises the game's
                   own shutdown path. */
                SDL_Event ev = {.type = SDL_EVENT_QUIT};
                SDL_PushEvent(&ev);
            } else if (!strcmp(st->cmd, "quit")) {
                LOGI("autoinput: quit at frame %u", frame);
                snap_wait_writers();
                _exit(0);
            }
        }
    }
}

void autoinput_init(void) {
    const char *spec = getenv("LR2_AUTOINPUT");
    if (!spec)
        return;
    char *dup = strdup(spec), *save = NULL;
    bool want_pad = false;
    for (char *tok = strtok_r(dup, ",", &save); tok && s_nsteps < 256; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon)
            continue;
        *colon = 0;
        Step *st = &s_steps[s_nsteps];
        st->frame = (uint32_t)strtoul(tok, NULL, 10);
        const char *what = colon + 1;
        if (!strncmp(what, "save:", 5) || !strncmp(what, "load:", 5)) {
            st->sc = SDL_SCANCODE_UNKNOWN;
            snprintf(st->cmd, sizeof st->cmd, "%.4s", what);
            st->arg = atoi(what + 5);
        } else if (!strncmp(what, "pad:", 4)) {
            char ctl[32];
            snprintf(ctl, sizeof ctl, "%s", what + 4);
            st->hold = HOLD;
            char *slash = strchr(ctl, '/'), *eq = strchr(ctl, '=');
            if (slash) {
                *slash = 0;
                st->hold = (uint32_t)strtoul(slash + 1, NULL, 10);
            }
            float v = 1.0f;
            if (eq) {
                *eq = 0;
                v = strtof(eq + 1, NULL);
            }
            st->pad_ctl = pad_ctl_from_name(ctl);
            if (st->pad_ctl < 0) {
                LOGW("autoinput: unknown pad control '%s'", what);
                continue;
            }
            v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
            st->pad_val = (int16_t)(v < 0 ? v * 32768.0f : v * 32767.0f);
            st->sc = SDL_SCANCODE_UNKNOWN;
            snprintf(st->cmd, sizeof st->cmd, "pad");
            want_pad = true;
        } else if (!strcmp(what, "shot") || !strcmp(what, "quit") || !strcmp(what, "bt") ||
                   !strcmp(what, "close")) {
            st->sc = SDL_SCANCODE_UNKNOWN;
            snprintf(st->cmd, sizeof st->cmd, "%s", what);
        } else {
            char key[32];
            snprintf(key, sizeof key, "%s", what);
            char *slash = strchr(key, '/');
            st->hold = HOLD;
            if (slash) {
                *slash = 0;
                st->hold = (uint32_t)strtoul(slash + 1, NULL, 10);
            }
            st->sc = SDL_GetScancodeFromName(key);
            if (st->sc == SDL_SCANCODE_UNKNOWN) {
                LOGW("autoinput: unknown key '%s'", what);
                continue;
            }
        }
        s_nsteps++;
    }
    free(dup);
    if (want_pad || getenv("LR2_VIRTUAL_PAD"))
        vpad_attach();
    LOGI("autoinput: %d steps", s_nsteps);
    frame_hook_add(autoinput_frame);
}
