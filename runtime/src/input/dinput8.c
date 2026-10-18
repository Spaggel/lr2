/* DirectInput 8 on SDL3: system keyboard, system mouse and SDL gamepads.
 *
 * Each device has a fixed list of "native" objects (keys, axes, buttons,
 * POV hats).  SetDataFormat matches the guest's DIDATAFORMAT against that
 * list the way DirectInput does, giving every object an offset in the
 * guest's state block.  GetDeviceState samples the live SDL state;
 * GetDeviceData drains a per-device event buffer filled by an SDL event hook.
 *
 * Gamepads are exposed as HID gamepads with an Xbox-style layout:
 *   X/Y = left stick, Rx/Ry = right stick, Z = left trigger, Rz = right trigger,
 *   POV 0 = d-pad, buttons 0..9 = A B X Y LB RB Back Start LS RS,
 *   buttons 10/11 = LT/RT as digital buttons, button 12 = Guide.
 */
#include <SDL3/SDL.h>
#include <pthread.h>
#include "../win32/com.h"
#include "../win32/user32.h"
#include "../win32/handles.h"
#include "keymap.h"
#include "lr2pad.h"

#define DI_OK 0u
#define DI_NOEFFECT 1u
#define DI_BUFFEROVERFLOW 1u
#define DIERR_INPUTLOST 0x8007001eu
#define DIERR_NOTACQUIRED 0x8007000cu
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_OBJECTNOTFOUND 0x80070002u
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_NOTBUFFERED 0x80040207u
#define DIERR_ACQUIRED 0x800700aau

/* DIDFT_* object type bits */
#define DIDFT_RELAXIS 0x01u
#define DIDFT_ABSAXIS 0x02u
#define DIDFT_AXIS 0x03u
#define DIDFT_PSHBUTTON 0x04u
#define DIDFT_BUTTON 0x0cu
#define DIDFT_POV 0x10u
#define DIDFT_TYPEMASK 0xffu
#define DIDFT_ANYINSTANCE 0xffffu
#define DIDFT_INSTANCE(t) (((t) >> 8) & 0xffffu)
#define DIDFT_OPTIONAL 0x80000000u
#define DIDF_ABSAXIS 1u
#define DIDF_RELAXIS 2u

enum { DIPH_DEVICE = 0, DIPH_BYOFFSET = 1, DIPH_BYID = 2, DIPH_BYUSAGE = 3 };
enum { DIPROP_BUFFERSIZE = 1, DIPROP_AXISMODE = 2, DIPROP_GRANULARITY = 3, DIPROP_RANGE = 4,
       DIPROP_DEADZONE = 5, DIPROP_SATURATION = 6, DIPROP_FFGAIN = 7, DIPROP_AUTOCENTER = 9,
       DIPROP_CALIBRATIONMODE = 10, DIPROP_INSTANCENAME = 13, DIPROP_PRODUCTNAME = 14,
       DIPROP_JOYSTICKID = 15, DIPROP_KEYNAME = 20, DIPROP_SCANCODE = 22, DIPROP_VIDPID = 24 };

#define DI8DEVCLASS_ALL 0
#define DI8DEVCLASS_DEVICE 1
#define DI8DEVCLASS_POINTER 2
#define DI8DEVCLASS_KEYBOARD 3
#define DI8DEVCLASS_GAMECTRL 4
#define DI8DEVTYPE_MOUSE 0x12u
#define DI8DEVTYPE_KEYBOARD 0x13u
#define DI8DEVTYPE_GAMEPAD 0x15u
#define DIDEVTYPE_HID 0x10000u

/* ------------------------------------------------------------- GUIDs */
static const Guid GUID_XAxis = {0xa36d02e0, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_YAxis = {0xa36d02e1, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_ZAxis = {0xa36d02e2, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_RxAxis = {0xa36d02f4, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_RyAxis = {0xa36d02f5, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_RzAxis = {0xa36d02e3, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_Button = {0xa36d02f0, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_Key = {0x55728220, 0xd33c, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_POV = {0xa36d02f2, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_SysMouse = {0x6f1d2b60, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
static const Guid GUID_SysKeyboard = {0x6f1d2b61, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
/* Gamepad instance GUIDs: {4c523250-<slot>-0000-...} ("LR2P"), stable per slot. */
#define PAD_GUID_D1 0x4c523250u

/* ------------------------------------------------------------- devices */
enum { DEV_KEYBOARD, DEV_MOUSE, DEV_PAD };
enum { SRC_KEY, SRC_MAXIS, SRC_MBUTTON, SRC_PAXIS, SRC_PTRIGGER, SRC_PBUTTON, SRC_PTRIGBTN, SRC_PPOV,
       SRC_PVIRT /* lr2pad.h button */, SRC_PSTEER };

typedef struct {
    const Guid *guid;
    uint32_t type;            /* DIDFT_* type bits | instance << 8 */
    uint32_t native_ofs;      /* offset in the standard (c_dfDI*) layout */
    int src, idx;             /* where the value comes from */
    char name[32];
    /* per-object properties */
    int32_t min, max;
    uint32_t deadzone, saturation;   /* 0..10000 */
    /* assigned by SetDataFormat */
    int32_t fmt_ofs;          /* -1 if not in the data format */
} Obj;

#define MAX_OBJS 300
#define MAX_PADS 4

typedef struct {
    uint32_t ofs, data, time, seq;
} DiEvent;

typedef struct {
    ComObj com;
    int kind, pad_slot;
    uint32_t create_seq;      /* creation order, for the game's joystick numbering */
    Obj objs[MAX_OBJS];
    int nobjs;
    bool have_format, acquired, abs_mode;
    uint32_t data_size;
    uint32_t buf_size;        /* DIPROP_BUFFERSIZE */
    DiEvent *buf;
    uint32_t buf_head, buf_count;
    bool overflow;
    uint32_t event_handle;
    int32_t mouse_acc[3];     /* relative mouse motion since last GetDeviceState */
    int32_t mouse_abs[3];
} DiDevice;

typedef struct {
    SDL_Gamepad *gp;
    SDL_JoystickID id;
    char name[128];
    uint16_t vid, pid;
} Pad;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static Pad s_pads[MAX_PADS];
static DiDevice *s_devs[32];
static int s_ndevs;
static uint32_t s_seq, s_create_seq;
static bool s_hooked;
/* Keyboard state by DIK code, fed from key events (so injected events
   count too) and cleared when the window loses focus. */
static uint8_t s_keys[256];

/* Gamepad buttons in DirectInput order (see lr2pad.h: Start is button 8,
   where the game expects its pause button), then the triggers as buttons,
   Guide, and the virtual buttons lr2pad.c computes. */
static const int s_pad_buttons[] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
};
static const char *s_pad_button_names[PADB_COUNT] = {
    "A", "B", "X", "Y", "Left Bumper", "Right Bumper", "Back", "Left Stick", "Start",
    "Right Stick", "Left Trigger", "Right Trigger", "Guide",
    "Right Stick Down", "Right Stick Up", "Right Stick Left", "Right Stick Right",
    "Accelerate", "Brake", "Reverse", "Fire Weapon", "Brick Boost", "Talk", "Look Behind",
    "Next Target", "Previous Target", "Pause",
};
_Static_assert(PADB_LT == 10 && PADB_RT == 11 && PADB_GUIDE == 12, "button order");

static inline void put32(uint32_t a, uint32_t v) { *(u32u *)G2H(a) = v; }
static inline uint32_t get32(uint32_t a) { return *(u32u *)G2H(a); }

static void pad_guid(int slot, Guid *g) {
    memset(g, 0, sizeof *g);
    g->d1 = PAD_GUID_D1;
    g->d2 = (uint16_t)slot;
    memcpy(g->d4, "LR2JOYST", 8);
}

static void pad_product_guid(int slot, Guid *g) {
    /* DirectInput's {PIDVID-0000-0000-0000-504944564944} form */
    memset(g, 0, sizeof *g);
    g->d1 = (uint32_t)s_pads[slot].pid << 16 | s_pads[slot].vid;
    memcpy(g->d4, "\0\0PIDVID", 8);
}

/* ------------------------------------------------------- object tables */
static Obj *add_obj(DiDevice *d, const Guid *g, uint32_t type_bits, int instance, uint32_t ofs,
                    int src, int idx, const char *name) {
    Obj *o = &d->objs[d->nobjs++];
    memset(o, 0, sizeof *o);
    o->guid = g;
    o->type = type_bits | (uint32_t)instance << 8;
    o->native_ofs = ofs;
    o->src = src;
    o->idx = idx;
    snprintf(o->name, sizeof o->name, "%s", name);
    o->min = 0;
    o->max = 65535;
    o->saturation = 10000;
    o->fmt_ofs = -1;
    return o;
}

static void build_objects(DiDevice *d) {
    d->nobjs = 0;
    if (d->kind == DEV_KEYBOARD) {
        for (int k = 1; k < 256; k++) {
            const char *n = key_dik_name((uint8_t)k);
            if (n && *n)
                add_obj(d, &GUID_Key, DIDFT_PSHBUTTON, k, (uint32_t)k, SRC_KEY, k, n);
        }
    } else if (d->kind == DEV_MOUSE) {
        add_obj(d, &GUID_XAxis, DIDFT_RELAXIS, 0, 0, SRC_MAXIS, 0, "X-axis");
        add_obj(d, &GUID_YAxis, DIDFT_RELAXIS, 1, 4, SRC_MAXIS, 1, "Y-axis");
        add_obj(d, &GUID_ZAxis, DIDFT_RELAXIS, 2, 8, SRC_MAXIS, 2, "Wheel");
        static const char *bn[] = {"Left Button", "Right Button", "Middle Button", "Button 4", "Button 5"};
        for (int i = 0; i < 5; i++)
            add_obj(d, &GUID_Button, DIDFT_PSHBUTTON, 3 + i, 12 + (uint32_t)i, SRC_MBUTTON, i, bn[i]);
    } else {
        static const struct { const Guid *g; int axis; uint32_t ofs; const char *n; int src; } ax[] = {
            {&GUID_XAxis, SDL_GAMEPAD_AXIS_LEFTX, 0, "X Axis", SRC_PAXIS},
            {&GUID_YAxis, SDL_GAMEPAD_AXIS_LEFTY, 4, "Y Axis", SRC_PAXIS},
            {&GUID_ZAxis, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 8, "Left Trigger", SRC_PTRIGGER},
            {&GUID_RxAxis, SDL_GAMEPAD_AXIS_RIGHTX, 12, "X Rotation", SRC_PAXIS},
            /* The game reads only these 6 axes; the race steering axis
               (left stick + d-pad + keyboard) takes the Ry slot. */
            {&GUID_RyAxis, 0, 16, "Steering", SRC_PSTEER},
            {&GUID_RzAxis, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 20, "Right Trigger", SRC_PTRIGGER},
        };
        int inst = 0;
        for (int i = 0; i < 6; i++)
            add_obj(d, ax[i].g, DIDFT_ABSAXIS, inst++, ax[i].ofs, ax[i].src, ax[i].axis, ax[i].n);
        add_obj(d, &GUID_POV, DIDFT_POV, 0, 32, SRC_PPOV, 0, "Hat Switch");
        for (int i = 0; i < PADB_COUNT; i++) {
            int src = i < 10 ? SRC_PBUTTON : i < 12 ? SRC_PTRIGBTN : i == 12 ? SRC_PBUTTON : SRC_PVIRT;
            int idx = i < 10 ? s_pad_buttons[i]
                    : i == 10 ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                    : i == 11 ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                    : i == 12 ? SDL_GAMEPAD_BUTTON_GUIDE
                              : i;
            add_obj(d, &GUID_Button, DIDFT_PSHBUTTON, i, 48 + (uint32_t)i, src, idx,
                    s_pad_button_names[i]);
        }
    }
}

static int count_type(DiDevice *d, uint32_t mask) {
    int n = 0;
    for (int i = 0; i < d->nobjs; i++)
        n += (d->objs[i].type & mask) != 0;
    return n;
}

/* ------------------------------------------------------- live sampling */
static SDL_Gamepad *dev_pad(DiDevice *d) {
    return d->kind == DEV_PAD ? s_pads[d->pad_slot].gp : NULL;
}

static int32_t scale_axis(const Obj *o, int v /* -32768..32767 */) {
    float f = v < 0 ? v / 32768.0f : v / 32767.0f;
    float a = SDL_fabsf(f), dz = o->deadzone / 10000.0f, sat = o->saturation / 10000.0f;
    if (sat <= dz)
        sat = dz + 0.0001f;
    a = a <= dz ? 0.0f : a >= sat ? 1.0f : (a - dz) / (sat - dz);
    f = f < 0 ? -a : a;
    double mid = ((double)o->min + o->max) / 2.0, half = ((double)o->max - o->min) / 2.0;
    return (int32_t)SDL_lround(mid + f * half);
}

static int32_t scale_trigger(const Obj *o, int v /* 0..32767 */) {
    float f = v / 32767.0f, dz = o->deadzone / 10000.0f, sat = o->saturation / 10000.0f;
    if (sat <= dz)
        sat = dz + 0.0001f;
    f = f <= dz ? 0.0f : f >= sat ? 1.0f : (f - dz) / (sat - dz);
    return (int32_t)SDL_lround(o->min + f * ((double)o->max - o->min));
}

static uint32_t pov_value(SDL_Gamepad *gp) {
    bool u = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP);
    bool dn = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    bool l = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    bool r = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    int x = r - l, y = dn - u;
    static const uint32_t tab[3][3] = {
        {31500, 0, 4500},          /* y = -1 (up):   x = -1, 0, 1 */
        {27000, 0xffffffffu, 9000},
        {22500, 18000, 13500},
    };
    return tab[y + 1][x + 1];
}

bool di_key_down(uint8_t dik) { return s_keys[dik] != 0; }

/* Left stick with the d-pad on top, so the d-pad moves through menus. */
static int stick_with_dpad(SDL_Gamepad *gp, int axis) {
    int v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)axis);
    SDL_GamepadButton neg = SDL_GAMEPAD_BUTTON_DPAD_LEFT, pos = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    if (axis == SDL_GAMEPAD_AXIS_LEFTY)
        neg = SDL_GAMEPAD_BUTTON_DPAD_UP, pos = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    else if (axis != SDL_GAMEPAD_AXIS_LEFTX)
        return v;
    int d = SDL_GetGamepadButton(gp, pos) - SDL_GetGamepadButton(gp, neg);
    return d < 0 ? -32768 : d > 0 ? 32767 : v;
}

/* Current value of a native object, in DirectInput units. */
static uint32_t obj_value(DiDevice *d, Obj *o) {
    SDL_Gamepad *gp = dev_pad(d);
    switch (o->src) {
    case SRC_KEY:
        return s_keys[o->idx];
    case SRC_MBUTTON: {
        SDL_MouseButtonFlags b = SDL_GetMouseState(NULL, NULL);
        return b & SDL_BUTTON_MASK(o->idx + 1) ? 0x80 : 0;
    }
    case SRC_MAXIS:
        return (uint32_t)(d->abs_mode ? d->mouse_abs[o->idx] : d->mouse_acc[o->idx]);
    case SRC_PAXIS:
        return (uint32_t)scale_axis(o, gp ? stick_with_dpad(gp, o->idx) : 0);
    case SRC_PSTEER: {
        float f = gp ? lr2pad_steer(gp, d->pad_slot) : 0.0f;
        double mid = ((double)o->min + o->max) / 2.0, half = ((double)o->max - o->min) / 2.0;
        return (uint32_t)(int32_t)SDL_lround(mid + f * half);
    }
    case SRC_PVIRT:
        return gp && lr2pad_button(gp, d->pad_slot, o->idx) ? 0x80 : 0;
    case SRC_PTRIGGER:
        return (uint32_t)scale_trigger(o, gp ? SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)o->idx) : 0);
    case SRC_PBUTTON:
        return gp && SDL_GetGamepadButton(gp, (SDL_GamepadButton)o->idx) ? 0x80 : 0;
    case SRC_PTRIGBTN:
        return gp && SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)o->idx) > PAD_TRIGGER_THRESHOLD ? 0x80 : 0;
    case SRC_PPOV:
        return gp ? pov_value(gp) : 0xffffffffu;
    }
    return 0;
}

static bool obj_is_byte(const Obj *o) { return (o->type & DIDFT_BUTTON) != 0; }

/* ------------------------------------------------------- event buffer */
static void push_event(DiDevice *d, Obj *o, uint32_t value) {
    if (!d->buf_size || !d->acquired || o->fmt_ofs < 0)
        return;
    if (d->buf_count == d->buf_size) {
        d->overflow = true;
        d->buf_head = (d->buf_head + 1) % d->buf_size;
        d->buf_count--;
    }
    DiEvent *e = &d->buf[(d->buf_head + d->buf_count) % d->buf_size];
    e->ofs = (uint32_t)o->fmt_ofs;
    e->data = value;
    e->time = win_tick_ms();
    e->seq = ++s_seq;
    d->buf_count++;
}

static Obj *find_src(DiDevice *d, int src, int idx) {
    for (int i = 0; i < d->nobjs; i++)
        if (d->objs[i].src == src && d->objs[i].idx == idx)
            return &d->objs[i];
    return NULL;
}

static int pad_slot_of(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++)
        if (s_pads[i].gp && s_pads[i].id == id)
            return i;
    return -1;
}

static void pad_open(SDL_JoystickID id) {
    if (pad_slot_of(id) >= 0)
        return;
    for (int i = 0; i < MAX_PADS; i++) {
        if (s_pads[i].gp)
            continue;
        SDL_Gamepad *gp = SDL_OpenGamepad(id);
        if (!gp) {
            LOGW("cannot open gamepad %u: %s", (unsigned)id, SDL_GetError());
            return;
        }
        s_pads[i].gp = gp;
        s_pads[i].id = id;
        const char *n = SDL_GetGamepadName(gp);
        snprintf(s_pads[i].name, sizeof s_pads[i].name, "%s", n ? n : "Gamepad");
        s_pads[i].vid = SDL_GetGamepadVendor(gp);
        s_pads[i].pid = SDL_GetGamepadProduct(gp);
        LOGI("gamepad %d: %s (%04x:%04x)", i, s_pads[i].name, s_pads[i].vid, s_pads[i].pid);
        return;
    }
}

static void pad_close(SDL_JoystickID id) {
    int i = pad_slot_of(id);
    if (i < 0)
        return;
    LOGI("gamepad %d disconnected", i);
    SDL_CloseGamepad(s_pads[i].gp);
    s_pads[i].gp = NULL;
}

/* Previous digital/analog values for gamepad buffered events. */
static void pad_event(int slot, int src, int idx) {
    for (int k = 0; k < s_ndevs; k++) {
        DiDevice *d = s_devs[k];
        if (d->kind != DEV_PAD || d->pad_slot != slot)
            continue;
        Obj *o = find_src(d, src, idx);
        if (o)
            push_event(d, o, obj_value(d, o));
        if (src == SRC_PTRIGGER && (o = find_src(d, SRC_PTRIGBTN, idx)))
            push_event(d, o, obj_value(d, o));
    }
}

static bool di_event_hook(const SDL_Event *e) {
    pthread_mutex_lock(&s_lock);
    switch (e->type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        memset(s_keys, 0, sizeof s_keys);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        pad_open(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_close(e->gdevice.which);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (e->key.repeat)
            break;
        uint8_t dik = key_sdl_to_dik(e->key.scancode);
        if (dik)
            s_keys[dik] = e->key.down ? 0x80 : 0;
        for (int k = 0; k < s_ndevs; k++) {
            Obj *o;
            if (s_devs[k]->kind == DEV_KEYBOARD && dik && (o = find_src(s_devs[k], SRC_KEY, dik)))
                push_event(s_devs[k], o, e->key.down ? 0x80 : 0);
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
        for (int k = 0; k < s_ndevs; k++) {
            DiDevice *d = s_devs[k];
            if (d->kind != DEV_MOUSE)
                continue;
            int32_t dv[3] = {0, 0, 0};
            if (e->type == SDL_EVENT_MOUSE_MOTION) {
                dv[0] = (int32_t)SDL_lroundf(e->motion.xrel);
                dv[1] = (int32_t)SDL_lroundf(e->motion.yrel);
            } else {
                dv[2] = (int32_t)SDL_lroundf(e->wheel.y * 120.0f);
            }
            for (int a = 0; a < 3; a++) {
                if (!dv[a])
                    continue;
                d->mouse_acc[a] += dv[a];
                d->mouse_abs[a] += dv[a];
                Obj *o = find_src(d, SRC_MAXIS, a);
                if (o)
                    push_event(d, o, (uint32_t)(d->abs_mode ? d->mouse_abs[a] : dv[a]));
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        for (int k = 0; k < s_ndevs; k++) {
            Obj *o;
            if (s_devs[k]->kind == DEV_MOUSE && (o = find_src(s_devs[k], SRC_MBUTTON, e->button.button - 1)))
                push_event(s_devs[k], o, e->button.down ? 0x80 : 0);
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int slot = pad_slot_of(e->gaxis.which);
        if (slot < 0)
            break;
        int a = e->gaxis.axis;
        bool trig = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        pad_event(slot, trig ? SRC_PTRIGGER : SRC_PAXIS, a);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int slot = pad_slot_of(e->gbutton.which);
        if (slot < 0)
            break;
        int b = e->gbutton.button;
        if (b >= SDL_GAMEPAD_BUTTON_DPAD_UP && b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
            pad_event(slot, SRC_PPOV, 0);
            pad_event(slot, SRC_PAXIS, b <= SDL_GAMEPAD_BUTTON_DPAD_DOWN ? SDL_GAMEPAD_AXIS_LEFTY
                                                                        : SDL_GAMEPAD_AXIS_LEFTX);
        }
        else
            pad_event(slot, SRC_PBUTTON, b);
        break;
    }
    }
    pthread_mutex_unlock(&s_lock);
    return false;
}

static void ensure_hook(void) {
    if (s_hooked)
        return;
    s_hooked = true;
    user32_add_event_hook(di_event_hook);
    /* Gamepads already connected at startup. */
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++)
        pad_open(ids[i]);
    SDL_free(ids);
}

/* ------------------------------------------------- device descriptions */
/* DIDEVICEINSTANCEA (580 bytes): dwSize, guidInstance, guidProduct, dwDevType,
   tszInstanceName[260], tszProductName[260], guidFFDriver, wUsagePage, wUsage */
static void fill_devinst(uint32_t p, int kind, int slot) {
    uint8_t *h = G2H(p);
    memset(h + 4, 0, 576);
    Guid inst, prod;
    const char *name;
    uint32_t type;
    uint16_t page = 1, usage;
    if (kind == DEV_KEYBOARD) {
        inst = prod = GUID_SysKeyboard;
        name = "Keyboard";
        type = DI8DEVTYPE_KEYBOARD | 1u << 8;
        usage = 6;
    } else if (kind == DEV_MOUSE) {
        inst = prod = GUID_SysMouse;
        name = "Mouse";
        type = DI8DEVTYPE_MOUSE | 1u << 8;
        usage = 2;
    } else {
        pad_guid(slot, &inst);
        pad_product_guid(slot, &prod);
        name = s_pads[slot].name;
        type = DI8DEVTYPE_GAMEPAD | 2u << 8 | DIDEVTYPE_HID;
        usage = 5;
    }
    put32(p, 580);
    memcpy(h + 4, &inst, 16);
    memcpy(h + 20, &prod, 16);
    put32(p + 36, type);
    snprintf((char *)h + 40, 260, "%s", name);
    snprintf((char *)h + 300, 260, "%s", name);
    memcpy(h + 576, &page, 2);
    memcpy(h + 578, &usage, 2);
}

/* ----------------------------------------------------------- IDirectInputDevice8A */
static DiDevice *DEVTHIS(CPU *c) { return (DiDevice *)com_get(ARG(0)); }

static void Dev_QueryInterface(CPU *c) {
    /* The game only uses the interface it created; hand back the same object. */
    com_addref_obj(&DEVTHIS(c)->com);
    put32(ARG(2), ARG(0));
    RET(3, S_OK);
}

/* DIDEVCAPS: dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, FF fields... */
static void Dev_GetCapabilities(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    uint32_t p = ARG(1);
    uint32_t size = get32(p);
    if (size != 44 && size != 24)
        RET(2, DIERR_INVALIDPARAM);
    memset(G2H(p + 4), 0, size - 4);
    uint32_t flags = 1;   /* DIDC_ATTACHED */
    uint32_t type = d->kind == DEV_KEYBOARD ? DI8DEVTYPE_KEYBOARD | 1u << 8
                  : d->kind == DEV_MOUSE    ? DI8DEVTYPE_MOUSE | 1u << 8
                                            : DI8DEVTYPE_GAMEPAD | 2u << 8 | DIDEVTYPE_HID;
    if (d->kind == DEV_PAD && !dev_pad(d))
        flags = 0;
    put32(p + 4, flags);
    put32(p + 8, type);
    put32(p + 12, (uint32_t)count_type(d, DIDFT_AXIS));
    put32(p + 16, (uint32_t)count_type(d, DIDFT_BUTTON));
    put32(p + 20, (uint32_t)count_type(d, DIDFT_POV));
    RET(2, DI_OK);
}

static bool obj_matches_flags(const Obj *o, uint32_t flags) {
    /* EnumObjects dwFlags: DIDFT_ALL (0) or a mask of type bits. */
    uint32_t m = flags & DIDFT_TYPEMASK & 0x1f;
    return !m || (o->type & m);
}

/* EnumObjects(lpCallback, pvRef, dwFlags) */
static void Dev_EnumObjects(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3);
    uint32_t oi = gcalloc(316);
    for (int i = 0; i < d->nobjs; i++) {
        Obj *o = &d->objs[i];
        if (!obj_matches_flags(o, flags))
            continue;
        uint8_t *h = G2H(oi);
        memset(h, 0, 316);
        put32(oi, 316);
        memcpy(h + 4, o->guid, 16);
        put32(oi + 20, d->have_format && o->fmt_ofs >= 0 ? (uint32_t)o->fmt_ofs : o->native_ofs);
        put32(oi + 24, o->type);
        put32(oi + 28, (o->type & DIDFT_AXIS) ? 0x100u : 0);   /* DIDOI_ASPECTPOSITION */
        snprintf((char *)h + 32, 260, "%s", o->name);
        uint32_t args[2] = {oi, ref};
        if (guest_call(c, cb, 2, args) == 0)   /* DIENUM_STOP */
            break;
    }
    gfree(oi);
    RET(4, DI_OK);
}

static Obj *find_obj(DiDevice *d, uint32_t obj, uint32_t how) {
    for (int i = 0; i < d->nobjs; i++) {
        Obj *o = &d->objs[i];
        if (how == DIPH_BYOFFSET && o->fmt_ofs >= 0 && (uint32_t)o->fmt_ofs == obj)
            return o;
        if (how == DIPH_BYID && (o->type & 0xffffffu) == (obj & 0xffffffu))
            return o;
    }
    return NULL;
}

/* GetProperty(rguidProp, pdiph) / SetProperty(rguidProp, pdiph)
   DIPROPHEADER: dwSize, dwHeaderSize, dwObj, dwHow; data follows. */
static uint32_t prop(DiDevice *d, uint32_t guid, uint32_t ph, bool set) {
    uint32_t obj = get32(ph + 8), how = get32(ph + 12);
    uint32_t data = ph + get32(ph + 4);
    Obj *o = NULL;
    if (how != DIPH_DEVICE) {
        o = find_obj(d, obj, how);
        if (!o)
            return DIERR_OBJECTNOTFOUND;
    }
    /* Apply to one object or to all axes. */
    switch (guid) {
    case DIPROP_BUFFERSIZE:
        if (!set) {
            put32(data, d->buf_size);
            return DI_OK;
        }
        if (d->acquired)
            return DIERR_ACQUIRED;
        free(d->buf);
        d->buf_size = get32(data);
        if (d->buf_size > 4096)
            d->buf_size = 4096;
        d->buf = d->buf_size ? calloc(d->buf_size, sizeof *d->buf) : NULL;
        d->buf_head = d->buf_count = 0;
        return DI_OK;
    case DIPROP_AXISMODE:
        if (set)
            d->abs_mode = get32(data) == 1;
        else
            put32(data, d->abs_mode ? 1 : 0);
        return DI_OK;
    case DIPROP_GRANULARITY:
        if (!set)
            put32(data, o && o->src == SRC_MAXIS && o->idx == 2 ? 120 : 1);
        return DI_OK;
    case DIPROP_RANGE:
    case DIPROP_DEADZONE:
    case DIPROP_SATURATION: {
        bool any = false;
        for (int i = 0; i < d->nobjs; i++) {
            Obj *x = &d->objs[i];
            if ((o && x != o) || !(x->type & DIDFT_ABSAXIS))
                continue;
            any = true;
            if (guid == DIPROP_RANGE) {
                if (set) {
                    x->min = (int32_t)get32(data);
                    x->max = (int32_t)get32(data + 4);
                } else {
                    put32(data, (uint32_t)x->min);
                    put32(data + 4, (uint32_t)x->max);
                    return DI_OK;
                }
            } else {
                uint32_t *f = guid == DIPROP_DEADZONE ? &x->deadzone : &x->saturation;
                if (set)
                    *f = get32(data) > 10000 ? 10000 : get32(data);
                else {
                    put32(data, *f);
                    return DI_OK;
                }
            }
        }
        return any ? DI_OK : DIERR_UNSUPPORTED;
    }
    case DIPROP_CALIBRATIONMODE:
    case DIPROP_AUTOCENTER:
    case DIPROP_FFGAIN:
        if (!set)
            put32(data, 0);
        return DI_OK;
    case DIPROP_JOYSTICKID:
        if (!set)
            put32(data, d->kind == DEV_PAD ? (uint32_t)d->pad_slot : 0);
        return set ? DIERR_UNSUPPORTED : DI_OK;
    case DIPROP_VIDPID:
        if (!set)
            put32(data, d->kind == DEV_PAD ? (uint32_t)s_pads[d->pad_slot].pid << 16 | s_pads[d->pad_slot].vid : 0);
        return set ? DIERR_UNSUPPORTED : DI_OK;
    case DIPROP_INSTANCENAME:
    case DIPROP_PRODUCTNAME:
    case DIPROP_KEYNAME: {
        if (set)
            return DIERR_UNSUPPORTED;
        /* DIPROPSTRING: WCHAR wsz[260] */
        const char *s = o ? o->name : d->kind == DEV_PAD ? s_pads[d->pad_slot].name
                          : d->kind == DEV_MOUSE ? "Mouse" : "Keyboard";
        uint8_t *w = G2H(data);
        int i = 0;
        for (; s[i] && i < 259; i++)
            w[2 * i] = (uint8_t)s[i], w[2 * i + 1] = 0;
        w[2 * i] = w[2 * i + 1] = 0;
        return DI_OK;
    }
    case DIPROP_SCANCODE:
        if (!set && o && o->src == SRC_KEY) {
            put32(data, (uint32_t)o->idx);
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }
    LOGW("DirectInput: %s property %u not supported", set ? "set" : "get", guid);
    return DIERR_UNSUPPORTED;
}

static void Dev_GetProperty(CPU *c) { RET(3, prop(DEVTHIS(c), ARG(1), ARG(2), false)); }
static void Dev_SetProperty(CPU *c) {
    pthread_mutex_lock(&s_lock);
    uint32_t hr = prop(DEVTHIS(c), ARG(1), ARG(2), true);
    pthread_mutex_unlock(&s_lock);
    RET(3, hr);
}

static void Dev_Acquire(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    if (!d->have_format)
        RET(1, DIERR_INVALIDPARAM);
    if (d->acquired)
        RET(1, S_FALSE);
    pthread_mutex_lock(&s_lock);
    d->acquired = true;
    d->buf_head = d->buf_count = 0;
    memset(d->mouse_acc, 0, sizeof d->mouse_acc);
    pthread_mutex_unlock(&s_lock);
    RET(1, DI_OK);
}

static void Dev_Unacquire(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    bool was = d->acquired;
    d->acquired = false;
    RET(1, was ? DI_OK : DI_NOEFFECT);
}

/* GetDeviceState(cbData, lpvData) */
static void Dev_GetDeviceState(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    uint32_t size = ARG(1), p = ARG(2);
    if (!d->acquired)
        RET(3, DIERR_NOTACQUIRED);
    if (size != d->data_size)
        RET(3, DIERR_INVALIDPARAM);
    if (d->kind == DEV_PAD && !dev_pad(d)) {
        memset(G2H(p), 0, size);
        RET(3, DIERR_INPUTLOST);
    }
    pthread_mutex_lock(&s_lock);
    memset(G2H(p), 0, size);
    for (int i = 0; i < d->nobjs; i++) {
        Obj *o = &d->objs[i];
        if (o->fmt_ofs < 0)
            continue;
        uint32_t v = obj_value(d, o);
        if (obj_is_byte(o))
            *(uint8_t *)G2H(p + (uint32_t)o->fmt_ofs) = (uint8_t)v;
        else
            put32(p + (uint32_t)o->fmt_ofs, v);
    }
    memset(d->mouse_acc, 0, sizeof d->mouse_acc);
    pthread_mutex_unlock(&s_lock);
    RET(3, DI_OK);
}

/* GetDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags)
   DIDEVICEOBJECTDATA (DI8): dwOfs, dwData, dwTimeStamp, dwSequence, uAppData */
static void Dev_GetDeviceData(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    uint32_t osize = ARG(1), arr = ARG(2), pinout = ARG(3), flags = ARG(4);
    if (!d->acquired)
        RET(5, DIERR_NOTACQUIRED);
    if (!d->buf_size)
        RET(5, DIERR_NOTBUFFERED);
    if (osize != 20 && osize != 16)
        RET(5, DIERR_INVALIDPARAM);
    pthread_mutex_lock(&s_lock);
    uint32_t want = get32(pinout), n = 0;
    bool peek = flags & 1;   /* DIGDD_PEEK */
    uint32_t head = d->buf_head, count = d->buf_count;
    while (n < want && count) {
        DiEvent *e = &d->buf[head];
        if (arr) {
            uint32_t q = arr + n * osize;
            put32(q, e->ofs);
            put32(q + 4, e->data);
            put32(q + 8, e->time);
            put32(q + 12, e->seq);
            if (osize == 20)
                put32(q + 16, 0);
        }
        head = (head + 1) % d->buf_size;
        count--;
        n++;
    }
    if (!peek) {
        d->buf_head = head;
        d->buf_count = count;
    }
    bool ovf = d->overflow;
    if (!peek)
        d->overflow = false;
    pthread_mutex_unlock(&s_lock);
    put32(pinout, n);
    RET(5, ovf ? DI_BUFFEROVERFLOW : DI_OK);
}

/* SetDataFormat(lpdf): DIDATAFORMAT { dwSize, dwObjSize, dwFlags, dwDataSize,
   dwNumObjs, rgodf }, DIOBJECTDATAFORMAT { pguid, dwOfs, dwType, dwFlags } */
static void Dev_SetDataFormat(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    uint32_t df = ARG(1);
    if (d->acquired)
        RET(2, DIERR_ACQUIRED);
    if (get32(df) != 24 || get32(df + 4) != 16)
        RET(2, DIERR_INVALIDPARAM);
    uint32_t flags = get32(df + 8), size = get32(df + 12), n = get32(df + 16), rg = get32(df + 20);
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < d->nobjs; i++)
        d->objs[i].fmt_ofs = -1;
    uint32_t hr = DI_OK;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t od = rg + 16 * k;
        uint32_t pg = get32(od), ofs = get32(od + 4), type = get32(od + 8);
        Guid g;
        if (pg)
            memcpy(&g, G2H(pg), 16);
        uint32_t inst = DIDFT_INSTANCE(type);
        Obj *hit = NULL;
        for (int i = 0; i < d->nobjs && !hit; i++) {
            Obj *o = &d->objs[i];
            if (o->fmt_ofs >= 0)
                continue;
            if (pg && !guid_eq(&g, o->guid))
                continue;
            if (!(type & o->type & 0x1f))
                continue;
            if (inst != DIDFT_ANYINSTANCE && inst != DIDFT_INSTANCE(o->type))
                continue;
            hit = o;
        }
        if (hit) {
            hit->fmt_ofs = (int32_t)ofs;
        } else if (!(type & DIDFT_OPTIONAL)) {
            LOGW("DirectInput: required format object %u (type %08x) not found", k, type);
            hr = DIERR_INVALIDPARAM;
        }
    }
    d->data_size = size;
    d->have_format = hr == DI_OK;
    if (d->kind == DEV_MOUSE)
        d->abs_mode = (flags & DIDF_ABSAXIS) != 0;
    pthread_mutex_unlock(&s_lock);
    RET(2, hr);
}

static void Dev_SetEventNotification(CPU *c) {
    DEVTHIS(c)->event_handle = ARG(1);
    RET(2, DI_OK);
}
static void Dev_SetCooperativeLevel(CPU *c) { RET(3, DI_OK); }

/* GetObjectInfo(pdidoi, dwObj, dwHow) */
static void Dev_GetObjectInfo(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    uint32_t p = ARG(1);
    Obj *o = find_obj(d, ARG(2), ARG(3));
    if (!o)
        RET(4, DIERR_OBJECTNOTFOUND);
    uint8_t *h = G2H(p);
    uint32_t size = get32(p);
    memset(h + 4, 0, size > 4 ? size - 4 : 0);
    memcpy(h + 4, o->guid, 16);
    put32(p + 20, o->fmt_ofs >= 0 ? (uint32_t)o->fmt_ofs : o->native_ofs);
    put32(p + 24, o->type);
    snprintf((char *)h + 32, 260, "%s", o->name);
    RET(4, DI_OK);
}

static void Dev_GetDeviceInfo(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    fill_devinst(ARG(1), d->kind, d->pad_slot);
    RET(2, DI_OK);
}

static void Dev_Poll(CPU *c) {
    DiDevice *d = DEVTHIS(c);
    if (!d->acquired)
        RET(1, DIERR_NOTACQUIRED);
    if (d->kind == DEV_PAD && !dev_pad(d))
        RET(1, DIERR_INPUTLOST);
    RET(1, DI_NOEFFECT);
}

static void Dev_RunControlPanel(CPU *c) { RET(3, DI_OK); }
static void Dev_Initialize(CPU *c) { RET(4, DI_OK); }
static void Dev_EnumEffects(CPU *c) { RET(4, DI_OK); }
static void Dev_GetForceFeedbackState(CPU *c) { RET(2, DIERR_UNSUPPORTED); }
static void Dev_SendForceFeedbackCommand(CPU *c) { RET(2, DIERR_UNSUPPORTED); }
static void Dev_EnumCreatedEffectObjects(CPU *c) { RET(4, DI_OK); }
static void Dev_Escape(CPU *c) { RET(2, DIERR_UNSUPPORTED); }
static void Dev_SendDeviceData(CPU *c) { RET(5, DIERR_UNSUPPORTED); }
static void Dev_EnumEffectsInFile(CPU *c) { RET(5, DI_OK); }
static void Dev_WriteEffectToFile(CPU *c) { RET(5, DIERR_UNSUPPORTED); }
static void Dev_BuildActionMap(CPU *c) { RET(4, DIERR_UNSUPPORTED); }
static void Dev_SetActionMap(CPU *c) { RET(4, DIERR_UNSUPPORTED); }
static void Dev_GetImageInfo(CPU *c) { RET(2, DIERR_UNSUPPORTED); }
COM_STUB(Dev, CreateEffect, 4)
COM_STUB(Dev, GetEffectInfo, 2)

static void dev_destroy(ComObj *o) {
    DiDevice *d = (DiDevice *)o;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_ndevs; i++) {
        if (s_devs[i] == d) {
            s_devs[i] = s_devs[--s_ndevs];
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
    free(d->buf);
    free(d);
}

static const GuestFn s_dev_vtbl[] = {
    Dev_QueryInterface, com_AddRef, com_Release, Dev_GetCapabilities, Dev_EnumObjects,
    Dev_GetProperty, Dev_SetProperty, Dev_Acquire, Dev_Unacquire, Dev_GetDeviceState,
    Dev_GetDeviceData, Dev_SetDataFormat, Dev_SetEventNotification, Dev_SetCooperativeLevel,
    Dev_GetObjectInfo, Dev_GetDeviceInfo, Dev_RunControlPanel, Dev_Initialize, Dev_CreateEffect,
    Dev_EnumEffects, Dev_GetEffectInfo, Dev_GetForceFeedbackState, Dev_SendForceFeedbackCommand,
    Dev_EnumCreatedEffectObjects, Dev_Escape, Dev_Poll, Dev_SendDeviceData, Dev_EnumEffectsInFile,
    Dev_WriteEffectToFile, Dev_BuildActionMap, Dev_SetActionMap, Dev_GetImageInfo,
};
_Static_assert(sizeof s_dev_vtbl / sizeof *s_dev_vtbl == 32, "IDirectInputDevice8A has 32 methods");

/* ------------------------------------------------------------ IDirectInput8A */
typedef struct {
    ComObj com;
} DInput;

static void DI_QueryInterface(CPU *c) {
    com_addref_obj(com_get(ARG(0)));
    put32(ARG(2), ARG(0));
    RET(3, S_OK);
}

static int pad_slot_from_guid(const Guid *g) {
    Guid t;
    for (int i = 0; i < MAX_PADS; i++) {
        pad_guid(i, &t);
        if (guid_eq(&t, g))
            return i;
    }
    return -1;
}

/* CreateDevice(rguid, lplpDirectInputDevice, pUnkOuter) */
static void DI_CreateDevice(CPU *c) {
    Guid g;
    memcpy(&g, G2H(ARG(1)), 16);
    char gs[40];
    guid_str(&g, gs);
    int kind, slot = 0;
    pthread_mutex_lock(&s_lock);
    if (guid_eq(&g, &GUID_SysKeyboard))
        kind = DEV_KEYBOARD;
    else if (guid_eq(&g, &GUID_SysMouse))
        kind = DEV_MOUSE;
    else if ((slot = pad_slot_from_guid(&g)) >= 0 && s_pads[slot].gp)
        kind = DEV_PAD;
    else {
        pthread_mutex_unlock(&s_lock);
        LOGW("DirectInput CreateDevice(%s): no such device", gs);
        put32(ARG(2), 0);
        RET(4, DIERR_DEVICENOTREG);
    }
    DiDevice *d = calloc(1, sizeof *d);
    d->com.type = "IDirectInputDevice8A";
    d->com.destroy = dev_destroy;
    d->kind = kind;
    d->pad_slot = slot;
    d->create_seq = s_create_seq++;
    build_objects(d);
    if (s_ndevs < (int)(sizeof s_devs / sizeof *s_devs))
        s_devs[s_ndevs++] = d;
    pthread_mutex_unlock(&s_lock);
    LOGI("DirectInput CreateDevice(%s) -> %s", gs,
         kind == DEV_KEYBOARD ? "keyboard" : kind == DEV_MOUSE ? "mouse" : s_pads[slot].name);
    uint32_t vt = com_vtbl("IDirectInputDevice8A", s_dev_vtbl, 32);
    put32(ARG(2), com_wrap(&d->com, vt, 0));
    RET(4, DI_OK);
}

/* The game numbers its joysticks in the order it created them (it creates
   one per device EnumDevices lists, at startup).  Returns the number of the
   joystick for gamepad slot, or -1 if the game has none for it or the pad
   is gone. */
int di_pad_game_index(int slot) {
    int idx = -1;
    pthread_mutex_lock(&s_lock);
    DiDevice *mine = NULL;
    for (int i = 0; i < s_ndevs; i++)
        if (s_devs[i]->kind == DEV_PAD && s_devs[i]->pad_slot == slot && s_pads[slot].gp)
            mine = s_devs[i];
    if (mine) {
        idx = 0;
        for (int i = 0; i < s_ndevs; i++)
            idx += s_devs[i]->kind == DEV_PAD && s_devs[i]->create_seq < mine->create_seq;
    }
    pthread_mutex_unlock(&s_lock);
    return idx;
}

static bool class_matches(uint32_t cls, int kind) {
    uint32_t type = cls & 0xff;
    switch (type) {
    case DI8DEVCLASS_ALL: return true;
    case DI8DEVCLASS_DEVICE: return false;
    case DI8DEVCLASS_POINTER: case DI8DEVTYPE_MOUSE: return kind == DEV_MOUSE;
    case DI8DEVCLASS_KEYBOARD: case DI8DEVTYPE_KEYBOARD: return kind == DEV_KEYBOARD;
    case DI8DEVCLASS_GAMECTRL: case DI8DEVTYPE_GAMEPAD: case 0x14: /* JOYSTICK */
        return kind == DEV_PAD;
    }
    return false;
}

/* EnumDevices(dwDevType, lpCallback, pvRef, dwFlags) */
static void DI_EnumDevices(CPU *c) {
    uint32_t cls = ARG(1), cb = ARG(2), ref = ARG(3);
    uint32_t di = gcalloc(580);
    struct { int kind, slot; } list[2 + MAX_PADS];
    int n = 0;
    pthread_mutex_lock(&s_lock);
    for (int k = DEV_KEYBOARD; k <= DEV_MOUSE; k++)
        if (class_matches(cls, k))
            list[n].kind = k, list[n].slot = 0, n++;
    if (class_matches(cls, DEV_PAD))
        for (int i = 0; i < MAX_PADS; i++)
            if (s_pads[i].gp)
                list[n].kind = DEV_PAD, list[n].slot = i, n++;
    pthread_mutex_unlock(&s_lock);
    LOGI("DirectInput EnumDevices(class %x): %d device(s)", cls, n);
    for (int i = 0; i < n; i++) {
        fill_devinst(di, list[i].kind, list[i].slot);
        uint32_t args[2] = {di, ref};
        if (guest_call(c, cb, 2, args) == 0)
            break;
    }
    gfree(di);
    RET(5, DI_OK);
}

/* GetDeviceStatus(rguidInstance) */
static void DI_GetDeviceStatus(CPU *c) {
    Guid g;
    memcpy(&g, G2H(ARG(1)), 16);
    if (guid_eq(&g, &GUID_SysKeyboard) || guid_eq(&g, &GUID_SysMouse))
        RET(2, DI_OK);
    int slot = pad_slot_from_guid(&g);
    RET(2, slot >= 0 && s_pads[slot].gp ? DI_OK : 1u /* DI_NOTATTACHED */);
}

static void DI_RunControlPanel(CPU *c) { RET(3, DI_OK); }
static void DI_Initialize(CPU *c) { RET(3, DI_OK); }
static void DI_FindDevice(CPU *c) { RET(4, DIERR_DEVICENOTREG); }
static void DI_EnumDevicesBySemantics(CPU *c) { RET(6, DI_OK); }
static void DI_ConfigureDevices(CPU *c) { RET(5, DIERR_UNSUPPORTED); }

static const GuestFn s_di_vtbl[] = {
    DI_QueryInterface, com_AddRef, com_Release, DI_CreateDevice, DI_EnumDevices,
    DI_GetDeviceStatus, DI_RunControlPanel, DI_Initialize, DI_FindDevice,
    DI_EnumDevicesBySemantics, DI_ConfigureDevices,
};

/* DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter) */
void imp_DINPUT8_DirectInput8Create(CPU *c) {
    LOGI("DirectInput8Create(version %x)", ARG(1));
    ensure_hook();
    DInput *di = calloc(1, sizeof *di);
    di->com.type = "IDirectInput8A";
    uint32_t vt = com_vtbl("IDirectInput8A", s_di_vtbl, (int)(sizeof s_di_vtbl / sizeof *s_di_vtbl));
    put32(ARG(3), com_wrap(&di->com, vt, 0));
    RET(5, DI_OK);
}

COM_VTABLE("IDirectInputDevice8A", s_dev_vtbl)
COM_VTABLE("IDirectInput8A", s_di_vtbl)

/* -------------------------------------------------------------- savestates
 * Object tables are rebuilt from the device kind; only their per-object
 * settings are stored.  Input is live, so buffered events and pending mouse
 * motion are dropped on load rather than replayed. */
typedef struct {
    int32_t min, max;
    uint32_t deadzone, saturation;
    int32_t fmt_ofs;
} ObjSnap;

static void di_snap_save(ComObj *o, SnapBuf *b) { (void)o, (void)b; }
static ComObj *di_snap_load(SnapBuf *b) {
    (void)b;
    ensure_hook();
    return calloc(1, sizeof(DInput));
}

static void dev_snap_save(ComObj *o, SnapBuf *b) {
    DiDevice *d = (DiDevice *)o;
    uint32_t v[] = {(uint32_t)d->kind, (uint32_t)d->pad_slot, d->have_format, d->acquired,
                    d->abs_mode, d->data_size, d->buf_size, d->event_handle};
    sb_put(b, v, sizeof v);
    sb_put(b, d->mouse_abs, sizeof d->mouse_abs);
    sb_u32(b, (uint32_t)d->nobjs);
    for (int i = 0; i < d->nobjs; i++) {
        const Obj *x = &d->objs[i];
        ObjSnap s = {x->min, x->max, x->deadzone, x->saturation, x->fmt_ofs};
        SB_PUT(b, s);
    }
}

static ComObj *dev_snap_load(SnapBuf *b) {
    DiDevice *d = calloc(1, sizeof *d);
    d->com.destroy = dev_destroy;
    uint32_t v[8];
    sb_get(b, v, sizeof v);
    d->kind = (int)v[0];
    d->pad_slot = (int)v[1];
    d->have_format = v[2] != 0;
    d->acquired = v[3] != 0;
    d->abs_mode = v[4] != 0;
    d->data_size = v[5];
    d->buf_size = v[6];
    d->event_handle = v[7];
    sb_get(b, d->mouse_abs, sizeof d->mouse_abs);
    if (d->kind < DEV_KEYBOARD || d->kind > DEV_PAD || d->pad_slot < 0 || d->pad_slot >= MAX_PADS ||
        d->buf_size > 0x100000) {
        free(d);
        return NULL;
    }
    build_objects(d);
    d->create_seq = s_create_seq++;
    /* Gamepads gained virtual buttons after the first savestates were made;
       those older states have the same objects up to them, and the new ones
       go where the game's standard joystick format puts buttons. */
    int n = (int)sb_get_u32(b);
    if (n != d->nobjs && !(d->kind == DEV_PAD && n < d->nobjs)) {
        LOGW("savestate: DirectInput object table changed");
        free(d);
        return NULL;
    }
    for (int i = n; i < d->nobjs; i++)
        d->objs[i].fmt_ofs = d->have_format ? (int32_t)d->objs[i].native_ofs : -1;
    for (int i = 0; i < n; i++) {
        ObjSnap s;
        SB_GET(b, s);
        Obj *x = &d->objs[i];
        x->min = s.min;
        x->max = s.max;
        x->deadzone = s.deadzone;
        x->saturation = s.saturation;
        x->fmt_ofs = s.fmt_ofs;
    }
    d->buf = d->buf_size ? calloc(d->buf_size, sizeof *d->buf) : NULL;
    if (s_ndevs < (int)(sizeof s_devs / sizeof *s_devs))
        s_devs[s_ndevs++] = d;
    return &d->com;
}

static void dev_snap_drop(ComObj *o) {
    DiDevice *d = (DiDevice *)o;
    free(d->buf);
    free(d);
}

static const ComSnapClass s_di_classes[] = {
    {"IDirectInput8A", di_snap_save, di_snap_load, NULL, NULL},
    {"IDirectInputDevice8A", dev_snap_save, dev_snap_load, NULL, dev_snap_drop},
};

/* DINP: nothing to store; its drop empties the device list before the COM
   drop frees the devices (drops run in reverse order). */
static void dinp_save(SnapBuf *b) { (void)b; }
static void dinp_drop(void) { s_ndevs = 0; }
static bool dinp_load(SnapBuf *b) { (void)b; return true; }

static const SnapSection s_dinp_section = {
    SNAP_TAG('D', 'I', 'N', 'P'), SNAP_ORDER_LATE, dinp_save, NULL, dinp_drop, dinp_load,
};

static void dinp_lock(void) { pthread_mutex_lock(&s_lock); }
static void dinp_unlock(void) { pthread_mutex_unlock(&s_lock); }
static const SnapLock s_dinp_lock = {dinp_lock, dinp_unlock};

__attribute__((constructor)) static void dinput_snap_register(void) {
    for (size_t i = 0; i < sizeof s_di_classes / sizeof *s_di_classes; i++)
        com_snap_class(&s_di_classes[i]);
    snap_section(&s_dinp_section);
    snap_add_lock(&s_dinp_lock);
}
