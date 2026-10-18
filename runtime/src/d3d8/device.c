/* IDirect3DDevice8. */
#include "d3d8_internal.h"
#include "../win32/user32.h"
#include <math.h>

Device *g_dev;
void d3d8_fill_caps(uint32_t p);

static const GuestFn s_dev_vtbl[97];
static uint32_t dev_vtbl(void);

/* --------------------------------------------------------------- helpers */
static Texture *tex_of(uint32_t g) { return g ? (Texture *)com_get(g) : NULL; }
static Buffer *buf_of(uint32_t g) { return g ? (Buffer *)com_get(g) : NULL; }
static Surface *surf_of(uint32_t g) { return g ? (Surface *)com_get(g) : NULL; }

static void ref(uint32_t g) {
    ComObj *o = g ? com_get(g) : NULL;
    if (o)
        com_addref_obj(o);
}

static void unref(uint32_t g) {
    ComObj *o = g ? com_get(g) : NULL;
    if (o)
        com_release_obj(o);
}

/* Surfaces hold their container's reference. */
static void surf_ref(Surface *s) {
    if (s)
        com_addref_obj(s->container ? &s->container->com : &s->com);
}

static void surf_unref(Surface *s) {
    if (s)
        com_release_obj(s->container ? &s->container->com : &s->com);
}

static const Mat4 s_identity = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};

static void state_defaults(Device *d) {
    State *s = &d->st;
    memset(s->rs, 0, sizeof s->rs);
    uint32_t *rs = s->rs;
    rs[7] = d->pp[8] ? 1 : 0;   /* ZENABLE */
    rs[8] = 3;                  /* FILLMODE solid */
    rs[9] = 2;                  /* SHADEMODE gouraud */
    rs[14] = 1;                 /* ZWRITEENABLE */
    rs[16] = 1;                 /* LASTPIXEL */
    rs[19] = 2;                 /* SRCBLEND one */
    rs[20] = 1;                 /* DESTBLEND zero */
    rs[22] = 3;                 /* CULLMODE ccw */
    rs[23] = 4;                 /* ZFUNC lessequal */
    rs[25] = 8;                 /* ALPHAFUNC always */
    rs[36] = f2u(0.0f);         /* FOGSTART */
    rs[37] = f2u(1.0f);         /* FOGEND */
    rs[38] = f2u(1.0f);         /* FOGDENSITY */
    rs[53] = rs[54] = rs[55] = 1;   /* stencil ops keep */
    rs[56] = 8;                 /* STENCILFUNC always */
    rs[58] = rs[59] = 0xffffffffu;
    rs[60] = 0xffffffffu;       /* TEXTUREFACTOR */
    rs[136] = 1;                /* CLIPPING */
    rs[137] = 1;                /* LIGHTING */
    rs[141] = 1;                /* COLORVERTEX */
    rs[142] = 1;                /* LOCALVIEWER */
    rs[145] = 1;                /* DIFFUSEMATERIALSOURCE color1 */
    rs[146] = 2;                /* SPECULARMATERIALSOURCE color2 */
    rs[154] = f2u(1.0f);        /* POINTSIZE */
    rs[155] = f2u(1.0f);        /* POINTSIZE_MIN */
    rs[158] = f2u(1.0f);        /* POINTSCALE_A */
    rs[161] = 1;                /* MULTISAMPLEANTIALIAS */
    rs[162] = 0xffffffffu;
    rs[166] = f2u(64.0f);       /* POINTSIZE_MAX */
    rs[168] = 0xf;              /* COLORWRITEENABLE */
    rs[171] = 1;                /* BLENDOP add */
    for (int i = 0; i < MAX_STAGES; i++) {
        uint32_t *t = s->tss[i];
        memset(t, 0, sizeof s->tss[i]);
        t[1] = i == 0 ? 4 : 1;  /* COLOROP modulate / disable */
        t[2] = 2;               /* COLORARG1 texture */
        t[3] = 1;               /* COLORARG2 current */
        t[4] = i == 0 ? 2 : 1;  /* ALPHAOP selectarg1 / disable */
        t[5] = 2;
        t[6] = 1;
        t[11] = (uint32_t)i;    /* TEXCOORDINDEX */
        t[13] = t[14] = t[25] = 1;   /* ADDRESS wrap */
        t[16] = t[17] = 1;      /* MAG/MINFILTER point */
        t[18] = 0;              /* MIPFILTER none */
        t[21] = 1;              /* MAXANISOTROPY */
        t[26] = t[27] = t[28] = 1;   /* ARG0s, RESULTARG current */
        s->tex[i] = s_identity;
    }
    for (int i = 0; i < 4; i++)
        s->world[i] = s_identity;
    s->view = s_identity;
    s->proj = s_identity;
    memset(&s->mat, 0, sizeof s->mat);
    memset(s->light_set, 0, sizeof s->light_set);
    memset(s->light_on, 0, sizeof s->light_on);
    memset(s->clip, 0, sizeof s->clip);
    uint32_t w = d->backbuffer ? d->backbuffer->w : 640, h = d->backbuffer ? d->backbuffer->h : 480;
    s->vp = (Viewport){0, 0, w, h, 0.0f, 1.0f};
    s->fvf = 0;
    d->dirty = ~0u;
}

/* -------------------------------------------------------- GL state sync */
static const GLenum s_cmp[] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                               GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};

static GLenum gl_cmp(uint32_t f) { return f <= 8 ? s_cmp[f] : GL_ALWAYS; }

static GLenum gl_blend(uint32_t b, bool src) {
    switch (b) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    case 12: return src ? GL_SRC_ALPHA : GL_ONE_MINUS_SRC_ALPHA;   /* BOTHSRCALPHA */
    case 13: return src ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;   /* BOTHINVSRCALPHA */
    }
    return GL_ONE;
}

static GLenum gl_stencil_op(uint32_t o) {
    switch (o) {
    case 2: return GL_ZERO;
    case 3: return GL_REPLACE;
    case 4: return GL_INCR;
    case 5: return GL_DECR;
    case 6: return GL_INVERT;
    case 7: return GL_INCR_WRAP;
    case 8: return GL_DECR_WRAP;
    }
    return GL_KEEP;
}

static void bind_fbo(Device *d) {
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    Surface *rt = d->rt, *ds = d->ds;
    surf_ensure_gl(rt);
    if (rt->container)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               rt->container->type == RT_CUBETEXTURE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)rt->face : GL_TEXTURE_2D,
                               rt->container->gl, rt->level);
    else
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->gl_tex, 0);
    if (ds) {
        surf_ensure_gl(ds);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds->gl_rb);
    } else {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        LOGW("framebuffer incomplete: %#x", st);
}

static void sync_state(Device *d) {
    const uint32_t *rs = d->st.rs;
    if (d->dirty & DIRTY_FBO)
        bind_fbo(d);
    if (d->dirty & DIRTY_VIEWPORT) {
        const Viewport *v = &d->st.vp;
        glViewport((GLint)v->x, (GLint)v->y, (GLsizei)v->w, (GLsizei)v->h);
        glDepthRange(v->minz, v->maxz);
    }
    if (d->dirty & DIRTY_RASTER) {
        if (rs[7] && d->ds)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        glDepthFunc(gl_cmp(rs[23]));
        glDepthMask(rs[14] && d->ds ? GL_TRUE : GL_FALSE);
        if (rs[27]) {
            glEnable(GL_BLEND);
            glBlendFunc(gl_blend(rs[19], true), gl_blend(rs[20], false));
            switch (rs[171]) {
            case 2: glBlendEquation(GL_FUNC_SUBTRACT); break;
            case 3: glBlendEquation(GL_FUNC_REVERSE_SUBTRACT); break;
            case 4: glBlendEquation(GL_MIN); break;
            case 5: glBlendEquation(GL_MAX); break;
            default: glBlendEquation(GL_FUNC_ADD); break;
            }
        } else {
            glDisable(GL_BLEND);
        }
        /* Culling: see the winding note in d3d8_internal.h. */
        if (rs[22] == 1) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glFrontFace(GL_CCW);
            glCullFace(rs[22] == 3 ? GL_BACK : GL_FRONT);
        }
        glPolygonMode(GL_FRONT_AND_BACK, rs[8] == 1 ? GL_POINT : rs[8] == 2 ? GL_LINE : GL_FILL);
        if (rs[47]) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glEnable(GL_POLYGON_OFFSET_LINE);
            glPolygonOffset(0.0f, -(float)rs[47]);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
            glDisable(GL_POLYGON_OFFSET_LINE);
        }
        uint32_t cw = rs[168];
        glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
        if (rs[52] && d->ds) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(gl_cmp(rs[56]), (GLint)rs[57], rs[58]);
            glStencilOp(gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
            glStencilMask(rs[59]);
        } else {
            glDisable(GL_STENCIL_TEST);
        }
    }
    d->dirty = 0;
}

/* ------------------------------------------------------------- creation */
static void make_backbuffer(Device *d) {
    uint32_t w = d->pp[0], h = d->pp[1];
    if (d->backbuffer) {
        d->backbuffer->is_backbuffer = false;
        surf_unref(d->backbuffer);
    }
    if (d->auto_depth)
        surf_unref(d->auto_depth);
    d->backbuffer = surf_create(d, w, h, d->pp[2], USAGE_RENDERTARGET, POOL_DEFAULT, true);
    d->backbuffer->is_backbuffer = true;
    surf_ensure_gl(d->backbuffer);
    d->auto_depth = NULL;
    if (d->pp[8]) {
        d->auto_depth = surf_create(d, w, h, d->pp[9], USAGE_DEPTHSTENCIL, POOL_DEFAULT, false);
        surf_ensure_gl(d->auto_depth);
    }
    if (d->rt)
        surf_unref(d->rt);
    if (d->ds)
        surf_unref(d->ds);
    d->rt = d->backbuffer;
    surf_ref(d->rt);
    d->ds = d->auto_depth;
    surf_ref(d->ds);
    d->dirty |= DIRTY_FBO;
}

static void apply_pp(Device *d, uint32_t *pp) {
    int cw, ch;
    user32_client_size(d->hwnd, &cw, &ch);
    if (!pp[0] || !pp[1]) {
        pp[0] = (uint32_t)cw;
        pp[1] = (uint32_t)ch;
    }
    if (pp[2] == FMT_UNKNOWN)
        pp[2] = FMT_X8R8G8B8;
    if (!pp[3])
        pp[3] = 1;
    memcpy(d->pp, pp, sizeof d->pp);
    bool fullscreen = !pp[7] && !g_windowed;
    user32_set_client_size(d->hwnd, (int)pp[0], (int)pp[1], fullscreen);
    SDL_GL_SetSwapInterval(pp[12] == 0x80000000u ? 0 : 1);
    LOGI("device: %ux%u fmt=%u %s depth=%u(%u)", pp[0], pp[1], pp[2], pp[7] ? "windowed" : "fullscreen",
         pp[8], pp[9]);
}

static void dev_destroy(ComObj *o) {
    Device *d = (Device *)o;
    LOGI("device destroyed");
    if (g_dev == d)
        g_dev = NULL;
}

/* The GL context outlives devices (and savestate loads). */
static SDL_GLContext s_ctx;

/* Context plus the device's own GL objects. */
static bool device_gl_setup(Device *d, SDL_Window *win) {
    if (!s_ctx) {
        s_ctx = SDL_GL_CreateContext(win);
        if (!s_ctx) {
            LOGE("SDL_GL_CreateContext: %s", SDL_GetError());
            return false;
        }
        if (!gl_load())
            return false;
        LOGI("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        ffp_init();
    }
    SDL_GL_MakeCurrent(win, s_ctx);
    d->ctx = s_ctx;
    glGenFramebuffers(1, &d->fbo);
    glGenVertexArrays(1, &d->vao);
    glBindVertexArray(d->vao);
    glGenBuffers(1, &d->stream_vbo);
    glGenBuffers(1, &d->stream_ibo);
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    return true;
}

static void device_gl_free(Device *d) {
    glDeleteFramebuffers(1, &d->fbo);
    glDeleteVertexArrays(1, &d->vao);
    glDeleteBuffers(1, &d->stream_vbo);
    glDeleteBuffers(1, &d->stream_ibo);
}

uint32_t device_create(CPU *c, uint32_t d3d, uint32_t hwnd, uint32_t behavior, const uint32_t *ppin,
                       uint32_t *out) {
    (void)c;
    (void)behavior;
    uint32_t pp[13];
    memcpy(pp, ppin, sizeof pp);
    if (pp[6])
        hwnd = pp[6];
    SDL_Window *win = user32_sdl_window(hwnd);
    if (!win) {
        LOGE("CreateDevice: no window for hwnd %x", hwnd);
        return D3DERR_INVALIDCALL;
    }
    Device *d = calloc(1, sizeof *d);
    if (!device_gl_setup(d, win)) {
        free(d);
        return D3DERR_NOTAVAILABLE;
    }
    d->com.type = "IDirect3DDevice8";
    d->com.destroy = dev_destroy;
    d->d3d = d3d;
    d->hwnd = hwnd;
    g_dev = d;
    apply_pp(d, pp);
    memcpy((uint32_t *)ppin, pp, sizeof pp);
    make_backbuffer(d);
    state_defaults(d);
    ref(d3d);
    *out = com_wrap(&d->com, dev_vtbl(), 0);
    return D3D_OK;
}

/* ---------------------------------------------------------------- misc */
#define DEV Device *d = COM_THIS(Device)

static void Dev_QueryInterface(CPU *c) {
    put32(ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

static void Dev_TestCooperativeLevel(CPU *c) { RET(1, D3D_OK); }
static void Dev_GetAvailableTextureMem(CPU *c) { RET(1, 256u << 20); }
static void Dev_ResourceManagerDiscardBytes(CPU *c) { RET(2, D3D_OK); }

static void Dev_GetDirect3D(CPU *c) {
    DEV;
    ref(d->d3d);
    put32(ARG(1), d->d3d);
    RET(2, D3D_OK);
}

static void Dev_GetDeviceCaps(CPU *c) {
    d3d8_fill_caps(ARG(1));
    RET(2, D3D_OK);
}

static void Dev_GetDisplayMode(CPU *c) {
    DEV;
    uint32_t p = ARG(1);
    put32(p, d->pp[0]);
    put32(p + 4, d->pp[1]);
    put32(p + 8, 60);
    put32(p + 12, d->pp[2]);
    RET(2, D3D_OK);
}

static void Dev_GetCreationParameters(CPU *c) {
    DEV;
    uint32_t p = ARG(1);
    put32(p, 0);
    put32(p + 4, 1);
    put32(p + 8, d->hwnd);
    put32(p + 12, 0x40);   /* HARDWARE_VERTEXPROCESSING */
    RET(2, D3D_OK);
}

static void Dev_SetCursorProperties(CPU *c) { RET(4, D3D_OK); }
static void Dev_SetCursorPosition(CPU *c) { RET(4, 0); }

static void Dev_ShowCursor(CPU *c) {
    static bool shown;
    bool old = shown;
    shown = ARG(1) != 0;
    RET(2, old);
}

static void Dev_CreateAdditionalSwapChain(CPU *c) { RET(3, D3DERR_NOTAVAILABLE); }

static void Dev_Reset(CPU *c) {
    DEV;
    uint32_t pp[13];
    for (int i = 0; i < 13; i++)
        pp[i] = get32(ARG(1) + 4u * (uint32_t)i);
    LOGI("Reset");
    apply_pp(d, pp);
    for (int i = 0; i < 13; i++)
        put32(ARG(1) + 4u * (uint32_t)i, pp[i]);
    for (int i = 0; i < MAX_STAGES; i++) {
        unref(d->st.tex_obj[i]);
        d->st.tex_obj[i] = 0;
    }
    unref(d->st.vb);
    unref(d->st.ib);
    d->st.vb = d->st.ib = 0;
    make_backbuffer(d);
    state_defaults(d);
    RET(2, D3D_OK);
}

/* Debug: LR2_DUMP_FRAMES=N writes every Nth frame to <save>/frames/. */
static void dump_frame(Device *d) {
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("LR2_DUMP_FRAMES");
        every = e ? atoi(e) : 0;
    }
    extern bool g_autoinput_shot;
    bool shot = g_autoinput_shot;
    g_autoinput_shot = false;
    if (!shot && (!every || d->frame % (uint32_t)every))
        return;
    Surface *bb = d->backbuffer;
    SDL_Surface *img = SDL_CreateSurface((int)bb->w, (int)bb->h, SDL_PIXELFORMAT_ARGB8888);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bb->gl_tex, 0);
    glPixelStorei(GL_PACK_ROW_LENGTH, img->pitch / 4);
    glReadPixels(0, 0, (GLsizei)bb->w, (GLsizei)bb->h, GL_BGRA, GL_UNSIGNED_BYTE, img->pixels);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    char path[1200];
    snprintf(path, sizeof path, "%s/frames", g_save_dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/frames/frame%06u.bmp", g_save_dir, d->frame);
    SDL_SaveBMP(img, path);
    SDL_DestroySurface(img);
    LOGI("saved %s", path);
}

/* Draw the back buffer into the window, scaled with the aspect kept. */
static void present(Device *d) {
    dump_frame(d);
    int pw, ph;
    SDL_Window *win = user32_sdl_window(d->hwnd);
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    Surface *bb = d->backbuffer;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint rfbo;
    glGenFramebuffers(1, &rfbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rfbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bb->gl_tex, 0);
    float s = SDL_min((float)pw / (float)bb->w, (float)ph / (float)bb->h);
    int dw = (int)((float)bb->w * s), dh = (int)((float)bb->h * s);
    int dx = (pw - dw) / 2, dy = (ph - dh) / 2;
    /* Rows are stored top-down; the window's origin is bottom-left. */
    glBlitFramebuffer(0, 0, (GLint)bb->w, (GLint)bb->h, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &rfbo);
    SDL_GL_SwapWindow(win);
    d->dirty |= DIRTY_FBO | DIRTY_RASTER;
    d->frame++;
}

static void Dev_Present(CPU *c) {
    DEV;
    present(d);
    c->esp += 24;           /* this + 4 args + return address */
    c->eax = D3D_OK;
    user32_pump();
    if (g_trace_thunks && d->frame % 300 == 0) {
        LOGI("host calls, frames %u..%u:", d->frame - 300, d->frame);
        thunk_stats_dump();
    }
    frame_hooks_run(c, d->frame);
}

/* GetBackBuffer(BackBuffer, Type, ppBackBuffer) */
static void Dev_GetBackBuffer(CPU *c) {
    DEV;
    surf_ref(d->backbuffer);
    put32(ARG(3), d->backbuffer->com.guest);
    RET(4, D3D_OK);
}

static void Dev_GetRasterStatus(CPU *c) {
    put32(ARG(1), 0);
    put32(ARG(1) + 4, 0);
    RET(2, D3D_OK);
}

static void Dev_SetGammaRamp(CPU *c) { RET(3, 0); }

static void Dev_GetGammaRamp(CPU *c) {
    uint16_t *r = G2H(ARG(1));
    for (int ch = 0; ch < 3; ch++)
        for (int i = 0; i < 256; i++)
            r[ch * 256 + i] = (uint16_t)(i * 257);
    RET(2, 0);
}

/* ------------------------------------------------------------- creation */
/* CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture) */
static void Dev_CreateTexture(CPU *c) {
    DEV;
    uint32_t t = tex_create(d, RT_TEXTURE, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6));
    LOGD("CreateTexture(%ux%u, levels=%u, usage=%x, fmt=%u, pool=%u) -> %08x", ARG(1), ARG(2), ARG(3),
         ARG(4), ARG(5), ARG(6), t);
    put32(ARG(7), t);
    RET(8, t ? D3D_OK : D3DERR_INVALIDCALL);
}

static void Dev_CreateVolumeTexture(CPU *c) {
    put32(ARG(8), 0);
    RET(9, D3DERR_NOTAVAILABLE);
}

/* CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture) */
static void Dev_CreateCubeTexture(CPU *c) {
    DEV;
    uint32_t t = tex_create(d, RT_CUBETEXTURE, ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5));
    put32(ARG(6), t);
    RET(7, t ? D3D_OK : D3DERR_INVALIDCALL);
}

/* CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer) */
static void Dev_CreateVertexBuffer(CPU *c) {
    DEV;
    uint32_t b = buf_create(d, RT_VERTEXBUFFER, ARG(1), ARG(2), ARG(3), ARG(4));
    put32(ARG(5), b);
    RET(6, b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY);
}

/* CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer) */
static void Dev_CreateIndexBuffer(CPU *c) {
    DEV;
    uint32_t b = buf_create(d, RT_INDEXBUFFER, ARG(1), ARG(2), ARG(3), ARG(4));
    put32(ARG(5), b);
    RET(6, b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY);
}

/* CreateRenderTarget(Width, Height, Format, MultiSample, Lockable, ppSurface) */
static void Dev_CreateRenderTarget(CPU *c) {
    DEV;
    Surface *s = surf_create(d, ARG(1), ARG(2), ARG(3), USAGE_RENDERTARGET, POOL_DEFAULT, true);
    put32(ARG(6), s ? s->com.guest : 0);
    RET(7, s ? D3D_OK : D3DERR_INVALIDCALL);
}

/* CreateDepthStencilSurface(Width, Height, Format, MultiSample, ppSurface) */
static void Dev_CreateDepthStencilSurface(CPU *c) {
    DEV;
    Surface *s = surf_create(d, ARG(1), ARG(2), ARG(3), USAGE_DEPTHSTENCIL, POOL_DEFAULT, false);
    put32(ARG(5), s ? s->com.guest : 0);
    RET(6, s ? D3D_OK : D3DERR_INVALIDCALL);
}

/* CreateImageSurface(Width, Height, Format, ppSurface) */
static void Dev_CreateImageSurface(CPU *c) {
    DEV;
    Surface *s = surf_create(d, ARG(1), ARG(2), ARG(3), 0, POOL_SYSTEMMEM, true);
    put32(ARG(4), s ? s->com.guest : 0);
    RET(5, s ? D3D_OK : D3DERR_INVALIDCALL);
}

/* Upload a memory surface region into a GL-backed surface. */
static void upload_to_gl_surface(Surface *dst, Surface *src, uint32_t sx, uint32_t sy, uint32_t w,
                                 uint32_t h, uint32_t dx, uint32_t dy) {
    if (src->fmt != FMT_A8R8G8B8 && src->fmt != FMT_X8R8G8B8 && src->fmt != FMT_R5G6B5) {
        LOGW("CopyRects to render target from format %u not supported", src->fmt);
        return;
    }
    surf_ensure_gl(dst);
    GLuint tex = dst->container ? dst->container->gl : dst->gl_tex;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    int bpp = fmt_bpp(src->fmt) / 8;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(src->pitch / (uint32_t)bpp));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const void *px = G2H(src->mem + sy * src->pitch + sx * (uint32_t)bpp);
    if (src->fmt == FMT_R5G6B5)
        glTexSubImage2D(GL_TEXTURE_2D, dst->level, (GLint)dx, (GLint)dy, (GLsizei)w, (GLsizei)h, GL_RGB,
                        GL_UNSIGNED_SHORT_5_6_5, px);
    else
        glTexSubImage2D(GL_TEXTURE_2D, dst->level, (GLint)dx, (GLint)dy, (GLsizei)w, (GLsizei)h, GL_BGRA,
                        GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

/* CopyRects(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface, pDestPointsArray) */
static void Dev_CopyRects(CPU *c) {
    Surface *src = surf_of(ARG(1)), *dst = surf_of(ARG(4));
    uint32_t rects = ARG(2), n = ARG(3), pts = ARG(5);
    if (!src || !dst || src->fmt != dst->fmt || !src->mem)
        RET(6, D3DERR_INVALIDCALL);
    if (!rects)
        n = 1;
    if ((src->usage & USAGE_RENDERTARGET) || src->is_backbuffer)
        surf_read_back(src);
    int bpp = fmt_bpp(src->fmt) / 8;
    bool gl_dst = (dst->usage & USAGE_RENDERTARGET) || dst->is_backbuffer;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = 0, t = 0, r = src->w, b = src->h;
        if (rects) {
            l = get32(rects + 16 * i);
            t = get32(rects + 16 * i + 4);
            r = get32(rects + 16 * i + 8);
            b = get32(rects + 16 * i + 12);
        }
        uint32_t dx = pts ? get32(pts + 8 * i) : l, dy = pts ? get32(pts + 8 * i + 4) : t;
        if (r > src->w || b > src->h || dx + (r - l) > dst->w || dy + (b - t) > dst->h)
            continue;
        if (gl_dst) {
            upload_to_gl_surface(dst, src, l, t, r - l, b - t, dx, dy);
            continue;
        }
        for (uint32_t y = t; y < b; y++)
            memcpy(G2H(dst->mem + (dy + y - t) * dst->pitch + dx * (uint32_t)bpp),
                   G2H(src->mem + y * src->pitch + l * (uint32_t)bpp), (r - l) * (uint32_t)bpp);
    }
    if (dst->container)
        dst->container->dirty = true;
    RET(6, D3D_OK);
}

/* UpdateTexture(pSourceTexture, pDestinationTexture) */
static void Dev_UpdateTexture(CPU *c) {
    Texture *s = tex_of(ARG(1)), *t = tex_of(ARG(2));
    if (!s || !t || s->fmt != t->fmt || s->type != t->type)
        RET(3, D3DERR_INVALIDCALL);
    int faces = s->type == RT_CUBETEXTURE ? 6 : 1;
    uint32_t n = s->levels < t->levels ? s->levels : t->levels;
    for (int f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < n; l++) {
            Surface *a = s->surf[f][l], *b = t->surf[f][l];
            uint32_t sz = a->pitch * a->h;
            if (sz <= b->pitch * b->h)
                memcpy(G2H(b->mem), G2H(a->mem), sz);
        }
    }
    t->dirty = true;
    RET(3, D3D_OK);
}

static void Dev_GetFrontBuffer(CPU *c) { RET(2, D3D_OK); }

/* SetRenderTarget(pRenderTarget, pNewZStencil) */
static void Dev_SetRenderTarget(CPU *c) {
    DEV;
    Surface *rt = surf_of(ARG(1)), *ds = surf_of(ARG(2));
    if (rt) {
        surf_ref(rt);
        surf_unref(d->rt);
        d->rt = rt;
    }
    surf_ref(ds);
    surf_unref(d->ds);
    d->ds = ds;
    /* Setting a render target resets the viewport to cover it. */
    d->st.vp = (Viewport){0, 0, d->rt->w, d->rt->h, 0.0f, 1.0f};
    d->dirty |= DIRTY_FBO | DIRTY_VIEWPORT | DIRTY_RASTER;
    RET(3, D3D_OK);
}

static void Dev_GetRenderTarget(CPU *c) {
    DEV;
    surf_ref(d->rt);
    put32(ARG(1), d->rt->com.guest);
    RET(2, D3D_OK);
}

static void Dev_GetDepthStencilSurface(CPU *c) {
    DEV;
    if (!d->ds) {
        put32(ARG(1), 0);
        RET(2, D3DERR_NOTFOUND);
    }
    surf_ref(d->ds);
    put32(ARG(1), d->ds->com.guest);
    RET(2, D3D_OK);
}

static void Dev_BeginScene(CPU *c) {
    COM_THIS(Device)->in_scene = true;
    RET(1, D3D_OK);
}

static void Dev_EndScene(CPU *c) {
    COM_THIS(Device)->in_scene = false;
    RET(1, D3D_OK);
}

/* Clear(Count, pRects, Flags, Color, Z, Stencil) */
static void Dev_Clear(CPU *c) {
    DEV;
    uint32_t n = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4);
    float z = u2f(ARG(5));
    uint32_t stencil = ARG(6);
    sync_state(d);
    GLbitfield mask = 0;
    if (flags & 1) {
        mask |= GL_COLOR_BUFFER_BIT;
        glColorMask(1, 1, 1, 1);
        glClearColor((float)(color >> 16 & 0xff) / 255.0f, (float)(color >> 8 & 0xff) / 255.0f,
                     (float)(color & 0xff) / 255.0f, (float)(color >> 24) / 255.0f);
    }
    if ((flags & 2) && d->ds) {
        mask |= GL_DEPTH_BUFFER_BIT;
        glDepthMask(GL_TRUE);
        glClearDepth(z);
    }
    if ((flags & 4) && d->ds) {
        mask |= GL_STENCIL_BUFFER_BIT;
        glStencilMask(0xffffffffu);
        glClearStencil((GLint)stencil);
    }
    /* Clears are limited to the viewport, and to the rects if given. */
    const Viewport *v = &d->st.vp;
    glEnable(GL_SCISSOR_TEST);
    if (n && rects) {
        for (uint32_t i = 0; i < n; i++) {
            int32_t l = (int32_t)get32(rects + 16 * i), t = (int32_t)get32(rects + 16 * i + 4);
            int32_t r = (int32_t)get32(rects + 16 * i + 8), b = (int32_t)get32(rects + 16 * i + 12);
            int32_t vl = SDL_max(l, (int32_t)v->x), vt = SDL_max(t, (int32_t)v->y);
            int32_t vr = SDL_min(r, (int32_t)(v->x + v->w)), vb = SDL_min(b, (int32_t)(v->y + v->h));
            if (vr <= vl || vb <= vt)
                continue;
            glScissor(vl, vt, vr - vl, vb - vt);
            glClear(mask);
        }
    } else {
        glScissor((GLint)v->x, (GLint)v->y, (GLsizei)v->w, (GLsizei)v->h);
        glClear(mask);
    }
    glDisable(GL_SCISSOR_TEST);
    d->dirty |= DIRTY_RASTER;
    RET(7, D3D_OK);
}

/* ----------------------------------------------------------- transforms */
static Mat4 *transform_slot(State *s, uint32_t t) {
    if (t == 2)
        return &s->view;
    if (t == 3)
        return &s->proj;
    if (t >= 16 && t < 24)
        return &s->tex[t - 16];
    if (t >= 256 && t < 260)
        return &s->world[t - 256];
    return NULL;
}

static void record_transform(Device *d, uint32_t t) {
    StateBlock *b = d->recording;
    if (t == 2)
        b->view = true;
    else if (t == 3)
        b->proj = true;
    else if (t >= 16 && t < 24)
        b->tex[t - 16] = true;
    else if (t >= 256 && t < 260)
        b->world[t - 256] = true;
}

static void Dev_SetTransform(CPU *c) {
    DEV;
    State *s = d->recording ? &d->recording->s : &d->st;
    Mat4 *m = transform_slot(s, ARG(1));
    if (m) {
        memcpy(m->m, G2H(ARG(2)), 64);
        if (d->recording)
            record_transform(d, ARG(1));
    }
    RET(3, D3D_OK);
}

static void Dev_GetTransform(CPU *c) {
    DEV;
    Mat4 *m = transform_slot(&d->st, ARG(1));
    memcpy(G2H(ARG(2)), m ? m->m : s_identity.m, 64);
    RET(3, D3D_OK);
}

static void mat_mul(Mat4 *out, const Mat4 *a, const Mat4 *b) {
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i * 4 + j] = a->m[i * 4] * b->m[j] + a->m[i * 4 + 1] * b->m[4 + j] +
                             a->m[i * 4 + 2] * b->m[8 + j] + a->m[i * 4 + 3] * b->m[12 + j];
    *out = r;
}

static void Dev_MultiplyTransform(CPU *c) {
    DEV;
    Mat4 *m = transform_slot(&d->st, ARG(1));
    if (m) {
        Mat4 b;
        memcpy(b.m, G2H(ARG(2)), 64);
        mat_mul(m, m, &b);
    }
    RET(3, D3D_OK);
}

static void Dev_SetViewport(CPU *c) {
    DEV;
    uint32_t p = ARG(1);
    Viewport v = {get32(p), get32(p + 4), get32(p + 8), get32(p + 12), getf(p + 16), getf(p + 20)};
    if (d->recording) {
        d->recording->s.vp = v;
        d->recording->vp = true;
    } else {
        d->st.vp = v;
        d->dirty |= DIRTY_VIEWPORT;
    }
    RET(2, D3D_OK);
}

static void Dev_GetViewport(CPU *c) {
    DEV;
    uint32_t p = ARG(1);
    const Viewport *v = &d->st.vp;
    put32(p, v->x);
    put32(p + 4, v->y);
    put32(p + 8, v->w);
    put32(p + 12, v->h);
    put32(p + 16, f2u(v->minz));
    put32(p + 20, f2u(v->maxz));
    RET(2, D3D_OK);
}

static void Dev_SetMaterial(CPU *c) {
    DEV;
    if (d->recording) {
        memcpy(&d->recording->s.mat, G2H(ARG(1)), sizeof(Material));
        d->recording->mat = true;
    } else {
        memcpy(&d->st.mat, G2H(ARG(1)), sizeof(Material));
    }
    RET(2, D3D_OK);
}

static void Dev_GetMaterial(CPU *c) {
    memcpy(G2H(ARG(1)), &COM_THIS(Device)->st.mat, sizeof(Material));
    RET(2, D3D_OK);
}

static void Dev_SetLight(CPU *c) {
    DEV;
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS) {
        LOGW("SetLight(%u): index too large", i);
        RET(3, D3DERR_INVALIDCALL);
    }
    State *s = d->recording ? &d->recording->s : &d->st;
    memcpy(&s->lights[i], G2H(ARG(2)), sizeof(Light));
    s->light_set[i] = true;
    if (d->recording)
        d->recording->lights[i] = true;
    RET(3, D3D_OK);
}

static void Dev_GetLight(CPU *c) {
    DEV;
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS || !d->st.light_set[i])
        RET(3, D3DERR_INVALIDCALL);
    memcpy(G2H(ARG(2)), &d->st.lights[i], sizeof(Light));
    RET(3, D3D_OK);
}

static const Light s_default_light = {3, {1, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0}, {0, 0, 1},
                                      0, 0, 0, 0, 0, 0, 0};

static void Dev_LightEnable(CPU *c) {
    DEV;
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS)
        RET(3, D3DERR_INVALIDCALL);
    State *s = d->recording ? &d->recording->s : &d->st;
    if (!s->light_set[i]) {
        s->lights[i] = s_default_light;
        s->light_set[i] = true;
    }
    s->light_on[i] = ARG(2) != 0;
    if (d->recording)
        d->recording->lights[i] = true;
    RET(3, D3D_OK);
}

static void Dev_GetLightEnable(CPU *c) {
    DEV;
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS)
        RET(3, D3DERR_INVALIDCALL);
    put32(ARG(2), d->st.light_on[i]);
    RET(3, D3D_OK);
}

static void Dev_SetClipPlane(CPU *c) {
    DEV;
    if (ARG(1) >= 6)
        RET(3, D3DERR_INVALIDCALL);
    State *s = d->recording ? &d->recording->s : &d->st;
    memcpy(s->clip[ARG(1)], G2H(ARG(2)), 16);
    if (d->recording)
        d->recording->clip[ARG(1)] = true;
    RET(3, D3D_OK);
}

static void Dev_GetClipPlane(CPU *c) {
    DEV;
    if (ARG(1) >= 6)
        RET(3, D3DERR_INVALIDCALL);
    memcpy(G2H(ARG(2)), d->st.clip[ARG(1)], 16);
    RET(3, D3D_OK);
}

static void Dev_SetRenderState(CPU *c) {
    DEV;
    uint32_t s = ARG(1), v = ARG(2);
    if (s >= MAX_RS)
        RET(3, D3DERR_INVALIDCALL);
    if (d->recording) {
        d->recording->s.rs[s] = v;
        d->recording->rs[s] = 1;
    } else {
        d->st.rs[s] = v;
        d->dirty |= DIRTY_RASTER;
    }
    RET(3, D3D_OK);
}

static void Dev_GetRenderState(CPU *c) {
    DEV;
    uint32_t s = ARG(1);
    put32(ARG(2), s < MAX_RS ? d->st.rs[s] : 0);
    RET(3, s < MAX_RS ? D3D_OK : D3DERR_INVALIDCALL);
}

/* ----------------------------------------------------------- state blocks */
#define MAX_BLOCKS 1024
static StateBlock *s_blocks[MAX_BLOCKS];

static uint32_t block_alloc(StateBlock *b) {
    for (uint32_t i = 1; i < MAX_BLOCKS; i++) {
        if (!s_blocks[i]) {
            s_blocks[i] = b;
            return i;
        }
    }
    return 0;
}

static StateBlock *block_get(uint32_t h) { return h && h < MAX_BLOCKS ? s_blocks[h] : NULL; }

/* Copy the captured parts of `from` into `to`; refs adjusted for objects. */
static void block_copy(Device *d, StateBlock *b, State *to, const State *from, bool to_device) {
    for (int i = 0; i < MAX_RS; i++)
        if (b->rs[i])
            to->rs[i] = from->rs[i];
    for (int s = 0; s < MAX_STAGES; s++) {
        for (int i = 0; i < MAX_TSS; i++)
            if (b->tss[s][i])
                to->tss[s][i] = from->tss[s][i];
        if (b->tex[s])
            to->tex[s] = from->tex[s];
        if (b->tex_obj[s] && to->tex_obj[s] != from->tex_obj[s]) {
            if (to_device) {
                ref(from->tex_obj[s]);
                unref(to->tex_obj[s]);
            }
            to->tex_obj[s] = from->tex_obj[s];
        }
    }
    for (int i = 0; i < 4; i++)
        if (b->world[i])
            to->world[i] = from->world[i];
    if (b->view)
        to->view = from->view;
    if (b->proj)
        to->proj = from->proj;
    if (b->vp)
        to->vp = from->vp;
    if (b->mat)
        to->mat = from->mat;
    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (b->lights[i]) {
            to->lights[i] = from->lights[i];
            to->light_set[i] = from->light_set[i];
            to->light_on[i] = from->light_on[i];
        }
    }
    for (int i = 0; i < 6; i++)
        if (b->clip[i])
            memcpy(to->clip[i], from->clip[i], 16);
    if (b->fvf)
        to->fvf = from->fvf;
    if (b->vb && to->vb != from->vb) {
        if (to_device) {
            ref(from->vb);
            unref(to->vb);
        }
        to->vb = from->vb;
        to->stride = from->stride;
    }
    if (b->ib && to->ib != from->ib) {
        if (to_device) {
            ref(from->ib);
            unref(to->ib);
        }
        to->ib = from->ib;
        to->base_vertex = from->base_vertex;
    }
    if (to_device)
        d->dirty = ~0u;
}

static void Dev_BeginStateBlock(CPU *c) {
    DEV;
    if (d->recording)
        RET(1, D3DERR_INVALIDCALL);
    d->recording = calloc(1, sizeof(StateBlock));
    d->recording->s = d->st;
    RET(1, D3D_OK);
}

static void Dev_EndStateBlock(CPU *c) {
    DEV;
    if (!d->recording)
        RET(2, D3DERR_INVALIDCALL);
    uint32_t h = block_alloc(d->recording);
    d->recording = NULL;
    put32(ARG(1), h);
    RET(2, h ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY);
}

static void Dev_ApplyStateBlock(CPU *c) {
    DEV;
    StateBlock *b = block_get(ARG(1));
    if (!b)
        RET(2, D3DERR_INVALIDCALL);
    block_copy(d, b, &d->st, &b->s, true);
    RET(2, D3D_OK);
}

static void Dev_CaptureStateBlock(CPU *c) {
    DEV;
    StateBlock *b = block_get(ARG(1));
    if (!b)
        RET(2, D3DERR_INVALIDCALL);
    block_copy(d, b, &b->s, &d->st, false);
    RET(2, D3D_OK);
}

static void Dev_DeleteStateBlock(CPU *c) {
    uint32_t h = ARG(1);
    if (block_get(h)) {
        free(s_blocks[h]);
        s_blocks[h] = NULL;
    }
    RET(2, D3D_OK);
}

/* CreateStateBlock(Type, pToken): 1 = all, 2 = pixel, 3 = vertex state */
static void Dev_CreateStateBlock(CPU *c) {
    DEV;
    uint32_t type = ARG(1);
    StateBlock *b = calloc(1, sizeof *b);
    bool all = type == 1, px = all || type == 2, vx = all || type == 3;
    static const uint8_t pixel_rs[] = {7, 8, 9, 10, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 29,
                                       35, 47, 52, 53, 54, 55, 56, 57, 58, 59, 60, 128, 129, 130, 131,
                                       132, 133, 134, 135, 168, 171};
    static const uint8_t vertex_rs[] = {9, 28, 34, 35, 36, 37, 38, 48, 136, 137, 139, 140, 141, 142,
                                        143, 145, 146, 147, 148, 151, 152, 153, 154, 155, 156, 157,
                                        158, 159, 160, 161, 162, 163, 164, 166, 167, 170};
    if (all)
        memset(b->rs, 1, sizeof b->rs);
    if (px && !all)
        for (size_t i = 0; i < sizeof pixel_rs; i++)
            b->rs[pixel_rs[i]] = 1;
    if (vx && !all)
        for (size_t i = 0; i < sizeof vertex_rs; i++)
            b->rs[vertex_rs[i]] = 1;
    for (int s = 0; s < MAX_STAGES; s++) {
        for (int i = 0; i < MAX_TSS; i++) {
            bool vertex_tss = i == 11 || i == 24;
            if (all || (px && !vertex_tss) || (vx && vertex_tss))
                b->tss[s][i] = 1;
        }
        b->tex_obj[s] = all;
        b->tex[s] = all;
    }
    for (int i = 0; i < 4; i++)
        b->world[i] = all;
    b->view = b->proj = b->vp = b->mat = all;
    for (int i = 0; i < MAX_LIGHTS; i++)
        b->lights[i] = vx;
    for (int i = 0; i < 6; i++)
        b->clip[i] = all;
    b->fvf = vx;
    b->vb = b->ib = all;
    block_copy(d, b, &b->s, &d->st, false);
    uint32_t h = block_alloc(b);
    put32(ARG(2), h);
    RET(3, h ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY);
}

static void Dev_SetClipStatus(CPU *c) { RET(2, D3D_OK); }

static void Dev_GetClipStatus(CPU *c) {
    put32(ARG(1), 0);
    put32(ARG(1) + 4, 0);
    RET(2, D3D_OK);
}

/* -------------------------------------------------------------- textures */
static void Dev_GetTexture(CPU *c) {
    DEV;
    uint32_t s = ARG(1);
    uint32_t t = s < MAX_STAGES ? d->st.tex_obj[s] : 0;
    ref(t);
    put32(ARG(2), t);
    RET(3, D3D_OK);
}

static void Dev_SetTexture(CPU *c) {
    DEV;
    uint32_t s = ARG(1), t = ARG(2);
    if (s >= MAX_STAGES)
        RET(3, D3DERR_INVALIDCALL);
    if (d->recording) {
        d->recording->s.tex_obj[s] = t;
        d->recording->tex_obj[s] = true;
        RET(3, D3D_OK);
    }
    if (d->st.tex_obj[s] != t) {
        ref(t);
        unref(d->st.tex_obj[s]);
        d->st.tex_obj[s] = t;
    }
    RET(3, D3D_OK);
}

static void Dev_GetTextureStageState(CPU *c) {
    DEV;
    uint32_t s = ARG(1), t = ARG(2);
    put32(ARG(3), s < MAX_STAGES && t < MAX_TSS ? d->st.tss[s][t] : 0);
    RET(4, D3D_OK);
}

static void Dev_SetTextureStageState(CPU *c) {
    DEV;
    uint32_t s = ARG(1), t = ARG(2), v = ARG(3);
    if (s >= MAX_STAGES || t >= MAX_TSS)
        RET(4, D3DERR_INVALIDCALL);
    if (d->recording) {
        d->recording->s.tss[s][t] = v;
        d->recording->tss[s][t] = 1;
    } else {
        d->st.tss[s][t] = v;
    }
    RET(4, D3D_OK);
}

static void Dev_ValidateDevice(CPU *c) {
    put32(ARG(1), 1);
    RET(2, D3D_OK);
}

static void Dev_GetInfo(CPU *c) { RET(4, S_FALSE); }

static uint8_t s_palettes[256][1024];
static uint32_t s_cur_palette;

static void Dev_SetPaletteEntries(CPU *c) {
    if (ARG(1) < 256)
        memcpy(s_palettes[ARG(1)], G2H(ARG(2)), 1024);
    RET(3, D3D_OK);
}

static void Dev_GetPaletteEntries(CPU *c) {
    if (ARG(1) < 256)
        memcpy(G2H(ARG(2)), s_palettes[ARG(1)], 1024);
    RET(3, D3D_OK);
}

static void Dev_SetCurrentTexturePalette(CPU *c) {
    s_cur_palette = ARG(1);
    RET(2, D3D_OK);
}

static void Dev_GetCurrentTexturePalette(CPU *c) {
    put32(ARG(1), s_cur_palette);
    RET(2, D3D_OK);
}

/* ---------------------------------------------------------------- drawing */
static uint32_t fvf_stride(uint32_t fvf) {
    uint32_t n = 0;
    switch (fvf & 0xe) {
    case 0x2: n = 12; break;
    case 0x4: n = 16; break;
    case 0x6: n = 16; break;
    case 0x8: n = 20; break;
    case 0xa: n = 24; break;
    case 0xc: n = 28; break;
    case 0xe: n = 32; break;
    }
    if (fvf & 0x10) n += 12;
    if (fvf & 0x20) n += 4;
    if (fvf & 0x40) n += 4;
    if (fvf & 0x80) n += 4;
    uint32_t ntex = (fvf >> 8) & 0xf;
    for (uint32_t i = 0; i < ntex; i++) {
        static const uint32_t sz[4] = {8, 12, 16, 4};
        n += sz[(fvf >> (16 + 2 * i)) & 3];
    }
    return n;
}

/* Point the vertex attributes at `base` (offset into the bound VBO). */
static void setup_attribs(uint32_t fvf, uint32_t stride, uintptr_t base) {
    uintptr_t off = base;
    for (GLuint i = 0; i < 14; i++)
        glDisableVertexAttribArray(i);
    uint32_t pos = fvf & 0xe;
    if (pos == 0x4) {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void *)off);
        off += 16;
    } else if (pos) {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void *)off);
        off += 12;
        int nb = pos == 0x2 ? 0 : (int)(pos - 0x4) / 2;   /* XYZB1 = 0x6 -> 1 */
        if (nb > 0) {
            /* Weights (the last beta may be UBYTE4 indices; unused here). */
            int nw = (fvf & 0x1000) ? nb - 1 : nb;
            if (nw > 0) {
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, nw > 4 ? 4 : nw, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void *)off);
            }
            off += 4u * (uint32_t)nb;
        }
    }
    if (fvf & 0x10) {
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void *)off);
        off += 12;
    }
    if (fvf & 0x20) {
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void *)off);
        off += 4;
    }
    if (fvf & 0x40) {
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (void *)off);
        off += 4;
    } else {
        glVertexAttrib4f(4, 1, 1, 1, 1);
    }
    if (fvf & 0x80) {
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (void *)off);
        off += 4;
    } else {
        glVertexAttrib4f(5, 0, 0, 0, 0);
    }
    uint32_t ntex = (fvf >> 8) & 0xf;
    for (uint32_t i = 0; i < 8; i++) {
        if (i < ntex) {
            static const int cnt[4] = {2, 3, 4, 1};
            int n = cnt[(fvf >> (16 + 2 * i)) & 3];
            glEnableVertexAttribArray(6 + i);
            glVertexAttribPointer(6 + i, n, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void *)off);
            off += 4u * (uint32_t)n;
        } else {
            glVertexAttrib4f(6 + i, 0, 0, 0, 1);
        }
    }
}

static GLenum prim_mode(uint32_t type, uint32_t prims, uint32_t *count) {
    switch (type) {
    case 1: *count = prims; return GL_POINTS;
    case 2: *count = prims * 2; return GL_LINES;
    case 3: *count = prims + 1; return GL_LINE_STRIP;
    case 4: *count = prims * 3; return GL_TRIANGLES;
    case 5: *count = prims + 2; return GL_TRIANGLE_STRIP;
    case 6: *count = prims + 2; return GL_TRIANGLE_FAN;
    }
    *count = 0;
    return GL_POINTS;
}

/* Debug: LR2_TRACE_DRAWS=N logs every draw of frame N. */
static void trace_draw(Device *d, const char *what, GLenum mode, uint32_t count, uint32_t stride,
                       const uint8_t *v) {
    static int frame = -2;
    if (frame == -2) {
        const char *e = getenv("LR2_TRACE_DRAWS");
        frame = e ? atoi(e) : -1;
    }
    if ((int)d->frame != frame)
        return;
    float f[8] = {0};
    if (v)
        memcpy(f, v, sizeof f);
    const uint32_t *rs = d->st.rs;
    LOGI("%s mode=%x n=%u fvf=%x stride=%u v0=(%g %g %g %g | %08x) z=%u zw=%u ab=%u src=%u dst=%u at=%u "
         "cull=%u light=%u fog=%u tex0=%08x op0=%u/%u vp=%ux%u+%u+%u err=%x",
         what, mode, count, d->st.fvf, stride, f[0], f[1], f[2], f[3], ((uint32_t *)f)[4], rs[7], rs[14],
         rs[27], rs[19], rs[20], rs[15], rs[22], rs[137], rs[28], d->st.tex_obj[0], d->st.tss[0][1],
         d->st.tss[0][4], d->st.vp.w, d->st.vp.h, d->st.vp.x, d->st.vp.y, glGetError());
    Texture *t = tex_of(d->st.tex_obj[0]);
    if (t) {
        Surface *s0 = t->surf[0][0];
        uint32_t nz = 0, n = s0->pitch * s0->h;
        for (uint32_t i = 0; i < n; i++)
            nz += ((uint8_t *)G2H(s0->mem))[i] != 0;
        LOGI("   tex fmt=%u pool=%u %ux%u lv=%u gl=%u dirty=%d nonzero=%u/%u first=%08x", t->fmt, t->pool,
             t->w, t->h, t->levels, t->gl, t->dirty, nz, n, get32(s0->mem));
    }
}

static bool draw_prepare(Device *d) {
    if (!d->st.fvf) {
        static bool warned;
        if (!warned)
            LOGW("draw with no FVF set");
        warned = true;
        return false;
    }
    sync_state(d);
    bool ok = true;
    ffp_draw_setup(d, d->st.fvf, &ok);
    return ok;
}

/* DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount) */
static void Dev_DrawPrimitive(CPU *c) {
    DEV;
    uint32_t count;
    GLenum mode = prim_mode(ARG(1), ARG(3), &count);
    Buffer *vb = buf_of(d->st.vb);
    if (count && vb && draw_prepare(d)) {
        buf_sync(vb);
        setup_attribs(d->st.fvf, d->st.stride, 0);
        glDrawArrays(mode, (GLint)ARG(2), (GLsizei)count);
    }
    RET(4, D3D_OK);
}

/* DrawIndexedPrimitive(Type, MinIndex, NumVertices, StartIndex, PrimitiveCount) */
static void Dev_DrawIndexedPrimitive(CPU *c) {
    DEV;
    uint32_t count;
    GLenum mode = prim_mode(ARG(1), ARG(5), &count);
    Buffer *vb = buf_of(d->st.vb), *ib = buf_of(d->st.ib);
    if (count && vb && ib && draw_prepare(d)) {
        buf_sync(vb);
        setup_attribs(d->st.fvf, d->st.stride, 0);
        buf_sync(ib);
        bool i32 = ib->fmt == FMT_INDEX32;
        glDrawElementsBaseVertex(mode, (GLsizei)count, i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                                 (void *)(uintptr_t)(ARG(4) * (i32 ? 4u : 2u)), (GLint)d->st.base_vertex);
    }
    RET(6, D3D_OK);
}

/* After *UP draws D3D8 resets stream 0 and the indices. */
static void up_reset(Device *d) {
    unref(d->st.vb);
    d->st.vb = 0;
    d->st.stride = 0;
}

/* DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, Stride) */
static void Dev_DrawPrimitiveUP(CPU *c) {
    DEV;
    uint32_t count;
    GLenum mode = prim_mode(ARG(1), ARG(2), &count);
    uint32_t stride = ARG(4);
    if (count && draw_prepare(d)) {
        glBindBuffer(GL_ARRAY_BUFFER, d->stream_vbo);
        glBufferData(GL_ARRAY_BUFFER, count * stride, G2H(ARG(3)), GL_STREAM_DRAW);
        setup_attribs(d->st.fvf, stride, 0);
        glDrawArrays(mode, 0, (GLsizei)count);
        trace_draw(d, "DPUP", mode, count, stride, G2H(ARG(3)));
    }
    up_reset(d);
    RET(5, D3D_OK);
}

/* DrawIndexedPrimitiveUP(Type, MinVertexIndex, NumVertexIndices, PrimitiveCount,
                          pIndexData, IndexDataFormat, pVertexStreamZeroData, Stride) */
static void Dev_DrawIndexedPrimitiveUP(CPU *c) {
    DEV;
    uint32_t count;
    GLenum mode = prim_mode(ARG(1), ARG(4), &count);
    uint32_t minv = ARG(2), nv = ARG(3), stride = ARG(8);
    bool i32 = ARG(6) == FMT_INDEX32;
    if (count && draw_prepare(d)) {
        glBindBuffer(GL_ARRAY_BUFFER, d->stream_vbo);
        glBufferData(GL_ARRAY_BUFFER, (minv + nv) * stride, G2H(ARG(7)), GL_STREAM_DRAW);
        setup_attribs(d->st.fvf, stride, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d->stream_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * (i32 ? 4u : 2u), G2H(ARG(5)), GL_STREAM_DRAW);
        glDrawElementsBaseVertex(mode, (GLsizei)count, i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, 0, 0);
    }
    up_reset(d);
    unref(d->st.ib);
    d->st.ib = 0;
    RET(9, D3D_OK);
}

static void Dev_ProcessVertices(CPU *c) {
    LOGW("ProcessVertices not implemented");
    RET(6, D3DERR_INVALIDCALL);
}

/* ------------------------------------------------------- shaders, streams */
static void Dev_CreateVertexShader(CPU *c) {
    LOGW("CreateVertexShader: programmable shaders not supported");
    RET(5, D3DERR_INVALIDCALL);
}

static void Dev_SetVertexShader(CPU *c) {
    DEV;
    uint32_t h = ARG(1);
    if (h & 1)
        LOGW("SetVertexShader(%x): not an FVF", h);
    if (d->recording) {
        d->recording->s.fvf = h;
        d->recording->fvf = true;
    } else {
        d->st.fvf = h;
    }
    RET(2, D3D_OK);
}

static void Dev_GetVertexShader(CPU *c) {
    put32(ARG(1), COM_THIS(Device)->st.fvf);
    RET(2, D3D_OK);
}

static void Dev_DeleteVertexShader(CPU *c) { RET(2, D3D_OK); }
static void Dev_SetVertexShaderConstant(CPU *c) { RET(4, D3D_OK); }
static void Dev_GetVertexShaderConstant(CPU *c) { RET(4, D3D_OK); }
static void Dev_GetVertexShaderDeclaration(CPU *c) { RET(4, D3DERR_INVALIDCALL); }
static void Dev_GetVertexShaderFunction(CPU *c) { RET(4, D3DERR_INVALIDCALL); }

/* SetStreamSource(StreamNumber, pStreamData, Stride) */
static void Dev_SetStreamSource(CPU *c) {
    DEV;
    if (ARG(1) != 0)
        RET(4, ARG(2) ? D3DERR_INVALIDCALL : D3D_OK);
    uint32_t vb = ARG(2);
    if (d->recording) {
        d->recording->s.vb = vb;
        d->recording->s.stride = ARG(3);
        d->recording->vb = true;
        RET(4, D3D_OK);
    }
    if (d->st.vb != vb) {
        ref(vb);
        unref(d->st.vb);
        d->st.vb = vb;
    }
    d->st.stride = ARG(3);
    RET(4, D3D_OK);
}

static void Dev_GetStreamSource(CPU *c) {
    DEV;
    uint32_t vb = ARG(1) == 0 ? d->st.vb : 0;
    ref(vb);
    put32(ARG(2), vb);
    put32(ARG(3), ARG(1) == 0 ? d->st.stride : 0);
    RET(4, D3D_OK);
}

/* SetIndices(pIndexData, BaseVertexIndex) */
static void Dev_SetIndices(CPU *c) {
    DEV;
    uint32_t ib = ARG(1);
    if (d->recording) {
        d->recording->s.ib = ib;
        d->recording->s.base_vertex = ARG(2);
        d->recording->ib = true;
        RET(3, D3D_OK);
    }
    if (d->st.ib != ib) {
        ref(ib);
        unref(d->st.ib);
        d->st.ib = ib;
    }
    d->st.base_vertex = ARG(2);
    RET(3, D3D_OK);
}

static void Dev_GetIndices(CPU *c) {
    DEV;
    ref(d->st.ib);
    put32(ARG(1), d->st.ib);
    put32(ARG(2), d->st.base_vertex);
    RET(3, D3D_OK);
}

static void Dev_CreatePixelShader(CPU *c) {
    LOGW("CreatePixelShader: programmable shaders not supported");
    RET(3, D3DERR_INVALIDCALL);
}

static void Dev_SetPixelShader(CPU *c) { RET(2, ARG(1) ? D3DERR_INVALIDCALL : D3D_OK); }

static void Dev_GetPixelShader(CPU *c) {
    put32(ARG(1), 0);
    RET(2, D3D_OK);
}

static void Dev_DeletePixelShader(CPU *c) { RET(2, D3D_OK); }
static void Dev_SetPixelShaderConstant(CPU *c) { RET(4, D3D_OK); }
static void Dev_GetPixelShaderConstant(CPU *c) { RET(4, D3D_OK); }
static void Dev_GetPixelShaderFunction(CPU *c) { RET(4, D3DERR_INVALIDCALL); }
static void Dev_DrawRectPatch(CPU *c) { RET(4, D3DERR_INVALIDCALL); }
static void Dev_DrawTriPatch(CPU *c) { RET(4, D3DERR_INVALIDCALL); }
static void Dev_DeletePatch(CPU *c) { RET(2, D3D_OK); }

static const GuestFn s_dev_vtbl[] = {
    Dev_QueryInterface, com_AddRef, com_Release, Dev_TestCooperativeLevel,
    Dev_GetAvailableTextureMem, Dev_ResourceManagerDiscardBytes, Dev_GetDirect3D,
    Dev_GetDeviceCaps, Dev_GetDisplayMode, Dev_GetCreationParameters, Dev_SetCursorProperties,
    Dev_SetCursorPosition, Dev_ShowCursor, Dev_CreateAdditionalSwapChain, Dev_Reset, Dev_Present,
    Dev_GetBackBuffer, Dev_GetRasterStatus, Dev_SetGammaRamp, Dev_GetGammaRamp, Dev_CreateTexture,
    Dev_CreateVolumeTexture, Dev_CreateCubeTexture, Dev_CreateVertexBuffer, Dev_CreateIndexBuffer,
    Dev_CreateRenderTarget, Dev_CreateDepthStencilSurface, Dev_CreateImageSurface, Dev_CopyRects,
    Dev_UpdateTexture, Dev_GetFrontBuffer, Dev_SetRenderTarget, Dev_GetRenderTarget,
    Dev_GetDepthStencilSurface, Dev_BeginScene, Dev_EndScene, Dev_Clear, Dev_SetTransform,
    Dev_GetTransform, Dev_MultiplyTransform, Dev_SetViewport, Dev_GetViewport, Dev_SetMaterial,
    Dev_GetMaterial, Dev_SetLight, Dev_GetLight, Dev_LightEnable, Dev_GetLightEnable,
    Dev_SetClipPlane, Dev_GetClipPlane, Dev_SetRenderState, Dev_GetRenderState,
    Dev_BeginStateBlock, Dev_EndStateBlock, Dev_ApplyStateBlock, Dev_CaptureStateBlock,
    Dev_DeleteStateBlock, Dev_CreateStateBlock, Dev_SetClipStatus, Dev_GetClipStatus,
    Dev_GetTexture, Dev_SetTexture, Dev_GetTextureStageState, Dev_SetTextureStageState,
    Dev_ValidateDevice, Dev_GetInfo, Dev_SetPaletteEntries, Dev_GetPaletteEntries,
    Dev_SetCurrentTexturePalette, Dev_GetCurrentTexturePalette, Dev_DrawPrimitive,
    Dev_DrawIndexedPrimitive, Dev_DrawPrimitiveUP, Dev_DrawIndexedPrimitiveUP,
    Dev_ProcessVertices, Dev_CreateVertexShader, Dev_SetVertexShader, Dev_GetVertexShader,
    Dev_DeleteVertexShader, Dev_SetVertexShaderConstant, Dev_GetVertexShaderConstant,
    Dev_GetVertexShaderDeclaration, Dev_GetVertexShaderFunction, Dev_SetStreamSource,
    Dev_GetStreamSource, Dev_SetIndices, Dev_GetIndices, Dev_CreatePixelShader,
    Dev_SetPixelShader, Dev_GetPixelShader, Dev_DeletePixelShader, Dev_SetPixelShaderConstant,
    Dev_GetPixelShaderConstant, Dev_GetPixelShaderFunction, Dev_DrawRectPatch, Dev_DrawTriPatch,
    Dev_DeletePatch,
};

_Static_assert(sizeof s_dev_vtbl / sizeof *s_dev_vtbl == 97, "IDirect3DDevice8 has 97 methods");
COM_VTABLE("IDirect3DDevice8", s_dev_vtbl)

static uint32_t dev_vtbl(void) { return com_vtbl("IDirect3DDevice8", s_dev_vtbl, 97); }

/* -------------------------------------------------------------- savestates
 * The device's State is plain data (objects in it are guest pointers).
 * GL objects are recreated; render target contents come back with the next
 * frame.  D3DS holds the state blocks and palettes. */
static void dev_snap_save(ComObj *o, SnapBuf *b) {
    Device *d = (Device *)o;
    sb_u32(b, d->d3d);
    sb_u32(b, d->hwnd);
    sb_put(b, d->pp, sizeof d->pp);
    sb_put(b, &d->st, sizeof d->st);
    sb_u32(b, d->backbuffer ? d->backbuffer->com.guest : 0);
    sb_u32(b, d->auto_depth ? d->auto_depth->com.guest : 0);
    sb_u32(b, d->rt ? d->rt->com.guest : 0);
    sb_u32(b, d->ds ? d->ds->com.guest : 0);
    sb_u32(b, d->in_scene);
    sb_u32(b, d->frame);
}

static ComObj *dev_snap_load(SnapBuf *b) {
    Device *d = calloc(1, sizeof *d);
    d->com.destroy = dev_destroy;
    d->d3d = sb_get_u32(b);
    d->hwnd = sb_get_u32(b);
    sb_get(b, d->pp, sizeof d->pp);
    sb_get(b, &d->st, sizeof d->st);
    d->backbuffer = (Surface *)(uintptr_t)sb_get_u32(b);
    d->auto_depth = (Surface *)(uintptr_t)sb_get_u32(b);
    d->rt = (Surface *)(uintptr_t)sb_get_u32(b);
    d->ds = (Surface *)(uintptr_t)sb_get_u32(b);
    d->in_scene = sb_get_u32(b) != 0;
    d->frame = sb_get_u32(b);
    SDL_Window *win = user32_sdl_window(d->hwnd);
    if (!win || !device_gl_setup(d, win)) {
        free(d);
        return NULL;
    }
    int cw, ch;
    user32_client_size(d->hwnd, &cw, &ch);
    if ((uint32_t)cw != d->pp[0] || (uint32_t)ch != d->pp[1])
        apply_pp(d, d->pp);
    d->dirty = ~0u;
    g_dev = d;
    return &d->com;
}

static void dev_snap_link(ComObj *o) {
    Device *d = (Device *)o;
    COM_LINK(d->backbuffer, Surface);
    COM_LINK(d->auto_depth, Surface);
    COM_LINK(d->rt, Surface);
    COM_LINK(d->ds, Surface);
}

static void dev_snap_drop(ComObj *o) {
    Device *d = (Device *)o;
    device_gl_free(d);
    free(d->recording);
    if (g_dev == d)
        g_dev = NULL;
    free(d);
}

static const ComSnapClass s_dev_class = {
    "IDirect3DDevice8", dev_snap_save, dev_snap_load, dev_snap_link, dev_snap_drop,
};

#define BLOCK_DATA(b) ((uint8_t *)&(b)->s)
#define BLOCK_SIZE (sizeof(StateBlock) - offsetof(StateBlock, s))

static void d3ds_save(SnapBuf *b) {
    for (uint32_t i = 1; i < MAX_BLOCKS; i++) {
        if (s_blocks[i]) {
            sb_u32(b, i);
            sb_put(b, BLOCK_DATA(s_blocks[i]), BLOCK_SIZE);
        }
    }
    sb_u32(b, 0);
    StateBlock *rec = g_dev ? g_dev->recording : NULL;
    sb_u32(b, rec != NULL);
    if (rec)
        sb_put(b, BLOCK_DATA(rec), BLOCK_SIZE);
    sb_put(b, s_palettes, sizeof s_palettes);
    sb_u32(b, s_cur_palette);
}

static void d3ds_drop(void) {
    for (uint32_t i = 1; i < MAX_BLOCKS; i++) {
        free(s_blocks[i]);
        s_blocks[i] = NULL;
    }
}

static bool d3ds_load(SnapBuf *b) {
    for (;;) {
        uint32_t i = sb_get_u32(b);
        if (!i || i >= MAX_BLOCKS || b->err)
            break;
        s_blocks[i] = calloc(1, sizeof(StateBlock));
        sb_get(b, BLOCK_DATA(s_blocks[i]), BLOCK_SIZE);
    }
    if (sb_get_u32(b)) {
        StateBlock *rec = calloc(1, sizeof *rec);
        sb_get(b, BLOCK_DATA(rec), BLOCK_SIZE);
        if (g_dev)
            g_dev->recording = rec;
        else
            free(rec);
    }
    sb_get(b, s_palettes, sizeof s_palettes);
    s_cur_palette = sb_get_u32(b);
    return !b->err;
}

static const SnapSection s_d3ds_section = {
    SNAP_TAG('D', '3', 'D', 'S'), SNAP_ORDER_LATE, d3ds_save, NULL, d3ds_drop, d3ds_load,
};

__attribute__((constructor)) static void device_snap_register(void) {
    com_snap_class(&s_dev_class);
    snap_section(&s_d3ds_section);
}
