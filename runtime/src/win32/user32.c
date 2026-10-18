/* USER32: windows backed by one SDL window, a message queue fed from SDL
 * events, and the handful of dialog/cursor/metrics calls the game makes.
 *
 * The guest sees the client area as the whole window (no borders), at the
 * logical size it asked for; the host window may be any size and the
 * renderer scales to it.
 */
#include "user32.h"
#include "../savestate.h"
#include "handles.h"
#include "../input/keymap.h"
#include <pthread.h>

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000f
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001c
#define WM_SETCURSOR 0x0020
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCOMMAND 0x0112
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020a

#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)

#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define CW_USEDEFAULT 0x80000000u

/* --------------------------------------------------------------- classes */
typedef struct {
    char name[64];
    uint32_t atom, style, wndproc, cls_extra, wnd_extra, hinst, icon, cursor, brush;
} WndClass;

#define MAX_CLASSES 32
static WndClass s_classes[MAX_CLASSES];
static int s_nclasses;

static WndClass *class_find(uint32_t name_or_atom) {
    for (int i = 0; i < s_nclasses; i++) {
        if (name_or_atom < 0x10000 ? s_classes[i].atom == name_or_atom
                                   : !strcasecmp(s_classes[i].name, (const char *)G2H(name_or_atom)))
            return &s_classes[i];
    }
    return NULL;
}

/* --------------------------------------------------------------- windows */
typedef struct {
    bool used;
    uint32_t hwnd, parent, wndproc, style, exstyle, id, hinst, userdata, tid;
    int x, y, w, h;
    char title[128];
    WndClass *cls;
    SDL_Window *sdl;
    bool visible, minimized, fullscreen;
} Wnd;

#define MAX_WINDOWS 32
#define HWND_BASE 0x00010100u
static Wnd s_wnds[MAX_WINDOWS];
static uint32_t s_main_hwnd;
static uint32_t s_main_tid;

static Wnd *wnd_get(uint32_t hwnd) {
    if (hwnd < HWND_BASE || (hwnd - HWND_BASE) % 4)
        return NULL;
    uint32_t i = (hwnd - HWND_BASE) / 4;
    return i < MAX_WINDOWS && s_wnds[i].used ? &s_wnds[i] : NULL;
}

SDL_Window *user32_sdl_window(uint32_t hwnd) {
    Wnd *w = wnd_get(hwnd);
    while (w && !w->sdl && w->parent)
        w = wnd_get(w->parent);
    return w ? w->sdl : NULL;
}

uint32_t user32_main_hwnd(void) { return s_main_hwnd; }

void user32_client_size(uint32_t hwnd, int *pw, int *ph) {
    Wnd *w = wnd_get(hwnd);
    *pw = w ? w->w : 640;
    *ph = w ? w->h : 480;
}

/* ------------------------------------------------------------- messages */
typedef struct {
    uint32_t hwnd, msg, wp, lp, time;
    int32_t ptx, pty;
} Msg;

#define QSIZE 1024
static Msg s_q[QSIZE];
static uint32_t s_qtid[QSIZE];
static int s_qhead, s_qcount;
static pthread_mutex_t s_qlock = PTHREAD_MUTEX_INITIALIZER;
static int32_t s_mouse_x, s_mouse_y;

/* WM_QUIT is delivered when the queue is otherwise empty. */
static __thread bool t_quit;
static __thread uint32_t t_quit_code;

static void post(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&s_qlock);
    if (s_qcount == QSIZE) {
        pthread_mutex_unlock(&s_qlock);
        LOGW("message queue full, dropping %#x", msg);
        return;
    }
    int i = (s_qhead + s_qcount++) % QSIZE;
    s_q[i] = (Msg){hwnd, msg, wp, lp, win_tick_ms(), s_mouse_x, s_mouse_y};
    s_qtid[i] = tid;
    pthread_mutex_unlock(&s_qlock);
}

static bool filter_ok(const Msg *m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
    if (hwnd && m->hwnd != hwnd)
        return false;
    if (lo || hi)
        return m->msg >= lo && m->msg <= hi;
    return true;
}

/* Take (or peek) the first matching message for this thread. */
static bool take(Msg *out, uint32_t hwnd, uint32_t lo, uint32_t hi, bool remove) {
    uint32_t tid = thread_current_id();
    pthread_mutex_lock(&s_qlock);
    for (int k = 0; k < s_qcount; k++) {
        int i = (s_qhead + k) % QSIZE;
        if (s_qtid[i] != tid || !filter_ok(&s_q[i], hwnd, lo, hi))
            continue;
        *out = s_q[i];
        if (remove) {
            for (int j = k; j > 0; j--) {
                int a = (s_qhead + j) % QSIZE, b = (s_qhead + j - 1) % QSIZE;
                s_q[a] = s_q[b];
                s_qtid[a] = s_qtid[b];
            }
            s_qhead = (s_qhead + 1) % QSIZE;
            s_qcount--;
        }
        pthread_mutex_unlock(&s_qlock);
        return true;
    }
    pthread_mutex_unlock(&s_qlock);
    if (t_quit && (!lo || (WM_QUIT >= lo && WM_QUIT <= hi))) {
        *out = (Msg){0, WM_QUIT, t_quit_code, 0, win_tick_ms(), s_mouse_x, s_mouse_y};
        if (remove)
            t_quit = false;
        return true;
    }
    return false;
}

static bool queue_nonempty(void) {
    uint32_t tid = thread_current_id();
    pthread_mutex_lock(&s_qlock);
    bool any = false;
    for (int k = 0; k < s_qcount && !any; k++)
        any = s_qtid[(s_qhead + k) % QSIZE] == tid;
    pthread_mutex_unlock(&s_qlock);
    return any || t_quit;
}

static void write_msg(uint32_t p, const Msg *m) {
    u32u *o = G2H(p);
    o[0] = m->hwnd;
    o[1] = m->msg;
    o[2] = m->wp;
    o[3] = m->lp;
    o[4] = m->time;
    o[5] = (uint32_t)m->ptx;
    o[6] = (uint32_t)m->pty;
}

uint32_t user32_send(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Wnd *w = wnd_get(hwnd);
    if (!w || !w->wndproc)
        return 0;
    uint32_t args[4] = {hwnd, msg, wp, lp};
    return guest_call(c, w->wndproc, 4, args);
}

/* ------------------------------------------------------------ SDL events */
#define MAX_HOOKS 8
static EventHook s_hooks[MAX_HOOKS];
static int s_nhooks;

void user32_add_event_hook(EventHook h) {
    if (s_nhooks < MAX_HOOKS)
        s_hooks[s_nhooks++] = h;
}

static bool s_focused;

static void set_focus(bool on) {
    if (on == s_focused || !s_main_hwnd)
        return;
    s_focused = on;
    post(s_main_tid, s_main_hwnd, WM_ACTIVATEAPP, on, 0);
    post(s_main_tid, s_main_hwnd, WM_ACTIVATE, on ? 1 : 0, 0);
    post(s_main_tid, s_main_hwnd, on ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

/* Map window pixels to the guest's logical client coordinates. */
static void to_client(Wnd *w, float x, float y, int32_t *ox, int32_t *oy) {
    int pw, ph;
    SDL_GetWindowSize(w->sdl, &pw, &ph);
    if (pw <= 0 || ph <= 0) {
        *ox = (int32_t)x;
        *oy = (int32_t)y;
        return;
    }
    /* The renderer letterboxes to keep the aspect ratio. */
    float s = SDL_min((float)pw / (float)w->w, (float)ph / (float)w->h);
    float offx = ((float)pw - (float)w->w * s) / 2, offy = ((float)ph - (float)w->h * s) / 2;
    *ox = (int32_t)((x - offx) / s);
    *oy = (int32_t)((y - offy) / s);
}

static uint8_t s_vk_down[256];

static uint32_t key_lparam(SDL_Scancode sc, bool down, bool repeat) {
    uint8_t dik = key_sdl_to_dik(sc);
    uint32_t lp = 1 | (uint32_t)(dik & 0x7f) << 16;
    if (dik & 0x80)
        lp |= 1u << 24;
    if (!down)
        lp |= 3u << 30;
    else if (repeat)
        lp |= 1u << 30;
    return lp;
}

static void translate(const SDL_Event *e) {
    Wnd *w = wnd_get(s_main_hwnd);
    if (!w)
        return;
    uint32_t tid = w->tid, h = w->hwnd;
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        post(tid, h, WM_CLOSE, 0, 0);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        set_focus(true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        set_focus(false);
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        w->minimized = true;
        post(tid, h, WM_SIZE, 1, 0);   /* SIZE_MINIMIZED */
        break;
    case SDL_EVENT_WINDOW_RESTORED:
        if (w->minimized) {
            w->minimized = false;
            post(tid, h, WM_SIZE, 0, (uint32_t)w->w | (uint32_t)w->h << 16);
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        uint8_t vk = key_sdl_to_vk(e->key.scancode);
        if (!vk)
            break;
        s_vk_down[vk] = down;
        bool alt = (e->key.mod & SDL_KMOD_ALT) != 0;
        uint32_t lp = key_lparam(e->key.scancode, down, e->key.repeat);
        if (alt)
            lp |= 1u << 29;
        uint32_t msg = alt || vk == 0x12 ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
                                          : (down ? WM_KEYDOWN : WM_KEYUP);
        post(tid, h, msg, vk, lp);
        if (down && !alt) {
            /* WM_CHAR from the layout's character for this key. */
            SDL_Keycode k = SDL_GetKeyFromScancode(e->key.scancode, e->key.mod, false);
            uint32_t ch = 0;
            if (k >= 32 && k < 127)
                ch = (uint32_t)k;
            else if (k >= 0xa0 && k < 0x100)
                ch = (uint32_t)k;   /* Latin-1 */
            else if (vk == 0x08 || vk == 0x0d || vk == 0x1b || vk == 0x09)
                ch = vk;
            if ((e->key.mod & SDL_KMOD_CTRL) && ch >= 'a' && ch <= 'z')
                ch -= 'a' - 1;
            if (ch)
                post(tid, h, WM_CHAR, ch, lp);
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        to_client(w, e->motion.x, e->motion.y, &s_mouse_x, &s_mouse_y);
        post(tid, h, WM_MOUSEMOVE, 0, (uint32_t)(uint16_t)s_mouse_x | (uint32_t)(uint16_t)s_mouse_y << 16);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        to_client(w, e->button.x, e->button.y, &s_mouse_x, &s_mouse_y);
        uint32_t msg;
        switch (e->button.button) {
        case SDL_BUTTON_LEFT: msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; s_vk_down[1] = down; break;
        case SDL_BUTTON_RIGHT: msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; s_vk_down[2] = down; break;
        case SDL_BUTTON_MIDDLE: msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; s_vk_down[4] = down; break;
        default: return;
        }
        post(tid, h, msg, 0, (uint32_t)(uint16_t)s_mouse_x | (uint32_t)(uint16_t)s_mouse_y << 16);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        post(tid, h, WM_MOUSEWHEEL, (uint32_t)(int16_t)(e->wheel.y * 120) << 16,
             (uint32_t)(uint16_t)s_mouse_x | (uint32_t)(uint16_t)s_mouse_y << 16);
        break;
    }
}

void user32_pump(void) {
    if (thread_current_id() != s_main_tid)
        return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        bool eaten = false;
        for (int i = 0; i < s_nhooks && !eaten; i++)
            eaten = s_hooks[i](&e);
        if (!eaten)
            translate(&e);
    }
}

void user32_init(void) {
    s_main_tid = thread_current_id();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        rt_fatal("SDL_Init: %s", SDL_GetError());
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGW("no audio device (%s); running silent", SDL_GetError());
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
            LOGW("dummy audio failed too: %s", SDL_GetError());
    }
}

/* --------------------------------------------------------------- imports */
void imp_USER32_RegisterClassExA(CPU *c) {
    uint32_t p = ARG(0);
    if (s_nclasses == MAX_CLASSES)
        RET(1, 0);
    WndClass *k = &s_classes[s_nclasses++];
    u32u *wc = G2H(p);
    k->style = wc[1];
    k->wndproc = wc[2];
    k->cls_extra = wc[3];
    k->wnd_extra = wc[4];
    k->hinst = wc[5];
    k->icon = wc[6];
    k->cursor = wc[7];
    k->brush = wc[8];
    uint32_t name = wc[10];
    if (name < 0x10000)
        snprintf(k->name, sizeof k->name, "#%u", name);
    else
        snprintf(k->name, sizeof k->name, "%s", (const char *)G2H(name));
    k->atom = 0xc000u + (uint32_t)s_nclasses;
    LOGI("RegisterClassExA(%s, wndproc=%08x)", k->name, k->wndproc);
    RET(1, k->atom);
}

/* Create the SDL window for a top-level guest window. */
static void make_sdl_window(Wnd *w) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    int pw = w->w, ph = w->h;
    /* Small modes are unpleasant on modern screens: start at 2x. */
    while (pw < 1000 && ph < 700) {
        pw *= 2;
        ph *= 2;
    }
    w->sdl = SDL_CreateWindow(w->title[0] ? w->title : "LEGO Racers 2", pw, ph,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!w->sdl)
        rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
}

/* CreateWindowExA(exstyle, class, name, style, x, y, w, h, parent, menu, hinst, param) */
void imp_USER32_CreateWindowExA(CPU *c) {
    uint32_t exstyle = ARG(0), cls = ARG(1), style = ARG(3);
    int32_t x = (int32_t)ARG(4), y = (int32_t)ARG(5), cw = (int32_t)ARG(6), ch = (int32_t)ARG(7);
    uint32_t parent = ARG(8), menu = ARG(9), hinst = ARG(10), param = ARG(11);
    const char *title = ARGS(2);
    WndClass *k = class_find(cls);
    if (!k) {
        LOGW("CreateWindowExA: unknown class");
        RET(12, 0);
    }
    int i = 0;
    while (i < MAX_WINDOWS && s_wnds[i].used)
        i++;
    if (i == MAX_WINDOWS)
        RET(12, 0);
    Wnd *w = &s_wnds[i];
    memset(w, 0, sizeof *w);
    w->used = true;
    w->hwnd = HWND_BASE + 4u * (uint32_t)i;
    w->parent = parent;
    w->wndproc = k->wndproc;
    w->cls = k;
    w->style = style;
    w->exstyle = exstyle;
    w->id = menu;
    w->hinst = hinst;
    w->tid = thread_current_id();
    w->x = x == (int32_t)CW_USEDEFAULT ? 0 : x;
    w->y = y == (int32_t)CW_USEDEFAULT ? 0 : y;
    w->w = cw == (int32_t)CW_USEDEFAULT || cw <= 0 ? 640 : cw;
    w->h = ch == (int32_t)CW_USEDEFAULT || ch <= 0 ? 480 : ch;
    snprintf(w->title, sizeof w->title, "%s", title ? title : "");
    LOGI("CreateWindowExA(%s, \"%s\", style=%08x, %dx%d) -> %x", k->name, w->title, style, w->w,
         w->h, w->hwnd);
    if (!(style & WS_CHILD)) {
        make_sdl_window(w);
        if (!s_main_hwnd) {
            s_main_hwnd = w->hwnd;
            s_main_tid = w->tid;
        }
    }
    /* WM_NCCREATE / WM_CREATE get a CREATESTRUCTA. */
    uint32_t cs = gcalloc(48);
    u32u *p = G2H(cs);
    p[0] = param;
    p[1] = hinst;
    p[2] = menu;
    p[3] = parent;
    p[4] = (uint32_t)w->h;
    p[5] = (uint32_t)w->w;
    p[6] = (uint32_t)w->y;
    p[7] = (uint32_t)w->x;
    p[8] = style;
    p[9] = ARG(2);
    p[10] = cls;
    p[11] = exstyle;
    uint32_t hwnd = w->hwnd;
    bool ok = user32_send(c, hwnd, WM_NCCREATE, 0, cs) != 0;
    if (ok && (int32_t)user32_send(c, hwnd, WM_CREATE, 0, cs) == -1)
        ok = false;
    gfree(cs);
    if (!ok) {
        LOGW("CreateWindowExA: window procedure refused creation");
        if (w->sdl)
            SDL_DestroyWindow(w->sdl);
        w->used = false;
        if (s_main_hwnd == hwnd)
            s_main_hwnd = 0;
        RET(12, 0);
    }
    user32_send(c, hwnd, WM_SIZE, 0, (uint32_t)w->w | (uint32_t)w->h << 16);
    user32_send(c, hwnd, WM_MOVE, 0, (uint32_t)(uint16_t)w->x | (uint32_t)(uint16_t)w->y << 16);
    if (style & WS_VISIBLE) {
        w->visible = true;
        if (w->sdl)
            SDL_ShowWindow(w->sdl);
    }
    RET(12, hwnd);
}

static bool destroy_window(CPU *c, uint32_t hwnd) {
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return false;
    user32_send(c, hwnd, WM_DESTROY, 0, 0);
    user32_send(c, hwnd, WM_NCDESTROY, 0, 0);
    w = wnd_get(hwnd);
    if (w) {
        if (w->sdl)
            SDL_DestroyWindow(w->sdl);
        w->used = false;
    }
    if (s_main_hwnd == hwnd)
        s_main_hwnd = 0;
    return true;
}

void imp_USER32_DestroyWindow(CPU *c) {
    uint32_t hwnd = ARG(0);
    RET(1, destroy_window(c, hwnd));
}

void imp_USER32_DefWindowProcA(CPU *c) {
    uint32_t hwnd = ARG(0), msg = ARG(1), wp = ARG(2);
    uint32_t r = 0;
    switch (msg) {
    case WM_NCCREATE:
    case WM_ERASEBKGND:
        r = 1;
        break;
    case WM_CLOSE:
        destroy_window(c, hwnd);
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == 0xf060)   /* SC_CLOSE */
            post(thread_current_id(), hwnd, WM_CLOSE, 0, 0);
        break;
    }
    RET(4, r);
}

void imp_USER32_ShowWindow(CPU *c) {
    uint32_t hwnd = ARG(0), cmd = ARG(1);
    Wnd *w = wnd_get(hwnd);
    if (!w)
        RET(2, 0);
    bool was = w->visible;
    w->visible = cmd != 0;   /* SW_HIDE = 0 */
    if (w->sdl) {
        if (w->visible) {
            SDL_ShowWindow(w->sdl);
            SDL_RaiseWindow(w->sdl);
            set_focus(true);
        } else {
            SDL_HideWindow(w->sdl);
        }
    }
    if (was != w->visible)
        user32_send(c, hwnd, WM_SHOWWINDOW, w->visible, 0);
    RET(2, was);
}

void user32_set_client_size(uint32_t hwnd, int cw, int ch, bool fullscreen) {
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return;
    bool changed = w->w != cw || w->h != ch;
    w->w = cw;
    w->h = ch;
    w->fullscreen = fullscreen;
    if (w->sdl && changed && !fullscreen) {
        int pw = cw, ph = ch;
        while (pw < 1000 && ph < 700) {
            pw *= 2;
            ph *= 2;
        }
        /* Native-size modes would not fit on screen as a window. */
        SDL_Rect r;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(w->sdl), &r)) {
            float f = SDL_min(0.9f * (float)r.w / (float)pw, 0.9f * (float)r.h / (float)ph);
            if (f < 1.0f) {
                pw = (int)((float)pw * f);
                ph = (int)((float)ph * f);
            }
        }
        SDL_SetWindowSize(w->sdl, pw, ph);
    }
    if (w->sdl)
        SDL_SetWindowFullscreen(w->sdl, fullscreen);
}

void imp_USER32_MoveWindow(CPU *c) {
    uint32_t hwnd = ARG(0);
    Wnd *w = wnd_get(hwnd);
    if (!w)
        RET(6, 0);
    w->x = (int32_t)ARG(1);
    w->y = (int32_t)ARG(2);
    int nw = (int32_t)ARG(3), nh = (int32_t)ARG(4);
    LOGD("MoveWindow(%x, %d, %d, %d, %d)", hwnd, w->x, w->y, nw, nh);
    if (nw > 0 && nh > 0 && (nw != w->w || nh != w->h)) {
        user32_set_client_size(hwnd, nw, nh, w->fullscreen);
        user32_send(c, hwnd, WM_SIZE, 0, (uint32_t)nw | (uint32_t)nh << 16);
    }
    RET(6, 1);
}

void imp_USER32_GetClientRect(CPU *c) {
    Wnd *w = wnd_get(ARG(0));
    u32u *r = G2H(ARG(1));
    r[0] = r[1] = 0;
    r[2] = w ? (uint32_t)w->w : 640;
    r[3] = w ? (uint32_t)w->h : 480;
    RET(2, w != NULL);
}

void imp_USER32_GetWindowRect(CPU *c) {
    Wnd *w = wnd_get(ARG(0));
    u32u *r = G2H(ARG(1));
    int x = w ? w->x : 0, y = w ? w->y : 0;
    r[0] = (uint32_t)x;
    r[1] = (uint32_t)y;
    r[2] = (uint32_t)(x + (w ? w->w : 640));
    r[3] = (uint32_t)(y + (w ? w->h : 480));
    RET(2, w != NULL);
}

/* The guest never sees borders, so the window rect equals the client rect. */
void imp_USER32_AdjustWindowRectEx(CPU *c) { RET(4, 1); }

void imp_USER32_ScreenToClient(CPU *c) {
    Wnd *w = wnd_get(ARG(0));
    int32_t *p = G2H(ARG(1));
    if (w) {
        p[0] -= w->x;
        p[1] -= w->y;
    }
    RET(2, w != NULL);
}

void imp_USER32_GetWindowLongA(CPU *c) {
    Wnd *w = wnd_get(ARG(0));
    int32_t idx = (int32_t)ARG(1);
    uint32_t r = 0;
    if (w) {
        switch (idx) {
        case GWL_WNDPROC: r = w->wndproc; break;
        case GWL_HINSTANCE: r = w->hinst; break;
        case GWL_HWNDPARENT: r = w->parent; break;
        case GWL_ID: r = w->id; break;
        case GWL_STYLE: r = w->style | (w->visible ? WS_VISIBLE : 0); break;
        case GWL_EXSTYLE: r = w->exstyle; break;
        case GWL_USERDATA: r = w->userdata; break;
        default: LOGW("GetWindowLongA(%d)", idx);
        }
    }
    RET(2, r);
}

void imp_USER32_IsIconic(CPU *c) {
    Wnd *w = wnd_get(ARG(0));
    RET(1, w && w->minimized);
}

void imp_USER32_GetParent(CPU *c) {
    Wnd *w = wnd_get(ARG(0));
    RET(1, w ? w->parent : 0);
}

void imp_USER32_GetActiveWindow(CPU *c) { RET(0, s_focused ? s_main_hwnd : 0); }
void imp_USER32_SetActiveWindow(CPU *c) { RET(1, s_main_hwnd); }
void imp_USER32_SetFocus(CPU *c) { RET(1, s_main_hwnd); }

void imp_USER32_SetForegroundWindow(CPU *c) {
    Wnd *w = wnd_get(ARG(0));
    if (w && w->sdl)
        SDL_RaiseWindow(w->sdl);
    RET(1, w != NULL);
}

void imp_USER32_GetMenu(CPU *c) { RET(1, 0); }

void imp_USER32_FindWindowA(CPU *c) {
    const char *cls = ARG(0) >= 0x10000 ? ARGS(0) : NULL, *title = ARGS(1);
    uint32_t r = 0;
    for (int i = 0; i < MAX_WINDOWS && !r; i++) {
        Wnd *w = &s_wnds[i];
        if (w->used && (!cls || !strcasecmp(w->cls->name, cls)) && (!title || !strcmp(w->title, title)))
            r = w->hwnd;
    }
    LOGD("FindWindowA(%s, %s) -> %x", cls ? cls : "-", title ? title : "-", r);
    RET(2, r);
}

/* EnumChildWindows(hwndParent, lpEnumFunc, lParam) */
void imp_USER32_EnumChildWindows(CPU *c) {
    uint32_t parent = ARG(0), fn = ARG(1), lp = ARG(2);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (s_wnds[i].used && s_wnds[i].parent == parent) {
            uint32_t args[2] = {s_wnds[i].hwnd, lp};
            if (!guest_call(c, fn, 2, args))
                break;
        }
    }
    RET(3, 1);
}

/* -------------------------------------------------------- message loop */
void imp_USER32_PeekMessageA(CPU *c) {
    /* The game's main loop calls this every frame with nothing of its own
       on the host stack: the savestate safepoint. */
    snap_safepoint(c);
    uint32_t pmsg = ARG(0), hwnd = ARG(1), lo = ARG(2), hi = ARG(3), flags = ARG(4);
    if (g_autoinput_bt) {
        g_autoinput_bt = false;
        debug_backtrace("PeekMessageA");
    }
    user32_pump();
    Msg m;
    bool got = take(&m, hwnd, lo, hi, flags & 1);
    if (got)
        write_msg(pmsg, &m);
    RET(5, got);
}

static bool wake_on_input(void) {
    user32_pump();
    return queue_nonempty();
}

void imp_USER32_GetMessageA(CPU *c) {
    uint32_t pmsg = ARG(0), hwnd = ARG(1), lo = ARG(2), hi = ARG(3);
    Msg m;
    for (;;) {
        user32_pump();
        if (take(&m, hwnd, lo, hi, true))
            break;
        wait_handles(NULL, 0, false, 10, wake_on_input);
    }
    write_msg(pmsg, &m);
    RET(4, m.msg != WM_QUIT);
}

void imp_USER32_TranslateMessage(CPU *c) { RET(1, 0); }

void imp_USER32_DispatchMessageA(CPU *c) {
    u32u *m = G2H(ARG(0));
    uint32_t hwnd = m[0], msg = m[1], wp = m[2], lp = m[3];
    uint32_t r = hwnd ? user32_send(c, hwnd, msg, wp, lp) : 0;
    RET(1, r);
}

void imp_USER32_PostQuitMessage(CPU *c) {
    t_quit = true;
    t_quit_code = ARG(0);
    RET(1, 0);
}

void imp_USER32_PostThreadMessageA(CPU *c) {
    post(ARG(0), 0, ARG(1), ARG(2), ARG(3));
    RET(4, 1);
}

void imp_USER32_GetQueueStatus(CPU *c) {
    user32_pump();
    RET(1, queue_nonempty() ? (ARG(0) & 0xffff) << 16 | (ARG(0) & 0xffff) : 0);
}

/* MsgWaitForMultipleObjects(nCount, pHandles, fWaitAll, dwMilliseconds, dwWakeMask) */
void imp_USER32_MsgWaitForMultipleObjects(CPU *c) {
    uint32_t r;
    do {
        uint32_t n = ARG(0), arr = ARG(1);
        uint32_t hs[64];
        for (uint32_t i = 0; i < n && i < 64; i++)
            hs[i] = *(u32u *)G2H(arr + 4 * i);
        user32_pump();
        if (queue_nonempty())
            RET(5, n);
        r = wait_handles(hs, n, ARG(2) != 0, ARG(3), wake_on_input);
    } while (r == WAIT_RESTART);
    RET(5, r);
}

void imp_USER32_RegisterWindowMessageA(CPU *c) {
    static char names[64][64];
    static int n;
    const char *s = ARGS(0);
    for (int i = 0; i < n; i++)
        if (!strcasecmp(names[i], s))
            RET(1, 0xc100u + (uint32_t)i);
    if (n == 64)
        RET(1, 0);
    snprintf(names[n], sizeof names[n], "%s", s);
    RET(1, 0xc100u + (uint32_t)n++);
}

/* ------------------------------------------------------- input, cursor */
void imp_USER32_GetAsyncKeyState(CPU *c) {
    uint8_t vk = (uint8_t)ARG(0);
    bool down = false;
    if (vk >= 1 && vk <= 6) {
        down = s_vk_down[vk];
    } else {
        const bool *ks = SDL_GetKeyboardState(NULL);
        SDL_Scancode sc = key_vk_to_sdl(vk);
        if (vk == 0x10)
            down = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
        else if (vk == 0x11)
            down = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL];
        else if (vk == 0x12)
            down = ks[SDL_SCANCODE_LALT] || ks[SDL_SCANCODE_RALT];
        else if (sc != SDL_SCANCODE_UNKNOWN)
            down = ks[sc];
    }
    RET(1, down ? 0x8000 : 0);
}

void imp_USER32_GetKeyboardLayoutNameA(CPU *c) {
    strcpy((char *)G2H(ARG(0)), "00000409");
    RET(1, 1);
}

void imp_USER32_ShowCursor(CPU *c) {
    static int count = 0;
    count += ARG(0) ? 1 : -1;
    if (count >= 0)
        SDL_ShowCursor();
    else
        SDL_HideCursor();
    RET(1, (uint32_t)count);
}

void imp_USER32_LoadCursorA(CPU *c) { RET(2, 0x00a20000u + (ARG(1) & 0xffff)); }
void imp_USER32_LoadIconA(CPU *c) { RET(2, 0x00a30000u + (ARG(1) & 0xffff)); }
void imp_USER32_LoadAcceleratorsA(CPU *c) { RET(2, 0x00a40000u + (ARG(1) & 0xffff)); }

void imp_USER32_GetSystemMetrics(CPU *c) {
    int idx = (int)ARG(0);
    uint32_t r = 0;
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    switch (idx) {
    case 0: r = dm ? (uint32_t)dm->w : 1024; break;   /* SM_CXSCREEN */
    case 1: r = dm ? (uint32_t)dm->h : 768; break;    /* SM_CYSCREEN */
    case 19: r = 1; break;                            /* SM_MOUSEPRESENT */
    case 16: r = dm ? (uint32_t)dm->w : 1024; break;  /* SM_CXFULLSCREEN */
    case 17: r = dm ? (uint32_t)dm->h : 768; break;
    case 80: r = 1; break;                            /* SM_CMONITORS */
    default: r = 0; break;                            /* borders, captions: none */
    }
    RET(1, r);
}

/* --------------------------------------------------------------- dialogs */
void imp_USER32_MessageBoxA(CPU *c) {
    const char *text = ARGS(1), *cap = ARGS(2);
    uint32_t type = ARG(3);
    LOGW("MessageBoxA(\"%s\", \"%s\", %#x)", cap ? cap : "", text ? text : "", type);
    if (type & 0x10)   /* MB_ICONERROR */
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, cap ? cap : "LEGO Racers 2", text ? text : "",
                                 user32_sdl_window(s_main_hwnd));
    uint32_t r;
    switch (type & 0xf) {
    case 3: case 4: r = 6; break;   /* YESNO(CANCEL) -> IDYES */
    case 5: r = 4; break;           /* RETRYCANCEL -> IDRETRY */
    default: r = 1; break;          /* IDOK */
    }
    RET(4, r);
}

/* The only dialog is the "Rendering Options" box; act as if cancelled. */
void imp_USER32_DialogBoxParamA(CPU *c) {
    LOGI("DialogBoxParamA(%u): cancelled", ARG(1));
    RET(5, 2);
}

void imp_USER32_EndDialog(CPU *c) { RET(2, 1); }
void imp_USER32_SendDlgItemMessageA(CPU *c) { RET(5, 0); }
