/* Textures, surfaces, vertex and index buffers. */
#include "d3d8_internal.h"

/* Vtables are looked up by name on each creation: a savestate load can
   move them, so their guest addresses are never cached here. */
static uint32_t vt_tex(void), vt_cube(void), vt_surf(void), vt_vb(void), vt_ib(void);

static uint32_t pitch_for(uint32_t fmt, uint32_t w) {
    if (fmt == FMT_DXT1)
        return ((w + 3) / 4) * 8;
    if (fmt == FMT_DXT3 || fmt == FMT_DXT5)
        return ((w + 3) / 4) * 16;
    return ((w * (uint32_t)fmt_bpp(fmt) / 8) + 3) & ~3u;
}

static uint32_t rows_for(uint32_t fmt, uint32_t h) {
    return fmt == FMT_DXT1 || fmt == FMT_DXT3 || fmt == FMT_DXT5 ? (h + 3) / 4 : h;
}

/* ----------------------------------------------------------- GL formats */
typedef struct {
    GLint internal;
    GLenum format, type;
    GLint swz[4];
} GlFmt;

static bool gl_format(uint32_t fmt, GlFmt *g) {
    GLint rgba[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    memcpy(g->swz, rgba, sizeof rgba);
    switch (fmt) {
    case FMT_A8R8G8B8:
        *g = (GlFmt){GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA}};
        return true;
    case FMT_X8R8G8B8:
        *g = (GlFmt){GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, {GL_RED, GL_GREEN, GL_BLUE, GL_ONE}};
        return true;
    case FMT_R5G6B5:
        *g = (GlFmt){GL_RGB8, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, {GL_RED, GL_GREEN, GL_BLUE, GL_ONE}};
        return true;
    case FMT_A1R5G5B5:
        *g = (GlFmt){GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA}};
        return true;
    case FMT_X1R5G5B5:
        *g = (GlFmt){GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, {GL_RED, GL_GREEN, GL_BLUE, GL_ONE}};
        return true;
    case FMT_A4R4G4B4:
        *g = (GlFmt){GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA}};
        return true;
    case FMT_X4R4G4B4:
        *g = (GlFmt){GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, {GL_RED, GL_GREEN, GL_BLUE, GL_ONE}};
        return true;
    case FMT_A8:
        *g = (GlFmt){GL_R8, GL_RED, GL_UNSIGNED_BYTE, {GL_ZERO, GL_ZERO, GL_ZERO, GL_RED}};
        return true;
    case FMT_L8:
        *g = (GlFmt){GL_R8, GL_RED, GL_UNSIGNED_BYTE, {GL_RED, GL_RED, GL_RED, GL_ONE}};
        return true;
    case FMT_A8L8:
        *g = (GlFmt){GL_RG8, GL_RG, GL_UNSIGNED_BYTE, {GL_RED, GL_RED, GL_RED, GL_GREEN}};
        return true;
    }
    return false;
}

/* -------------------------------------------------------------- common */
static void Res_QueryInterface(CPU *c) {
    put32(ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

static void Res_GetDevice(CPU *c) {
    com_addref_obj(&g_dev->com);
    put32(ARG(1), g_dev->com.guest);
    RET(2, D3D_OK);
}

static void Res_SetPrivateData(CPU *c) { RET(5, D3D_OK); }
static void Res_GetPrivateData(CPU *c) { RET(4, D3DERR_NOTFOUND); }
static void Res_FreePrivateData(CPU *c) { RET(2, D3D_OK); }
static void Res_SetPriority(CPU *c) { RET(2, 0); }
static void Res_GetPriority(CPU *c) { RET(1, 0); }
static void Res_PreLoad(CPU *c) { RET(1, 0); }

/* ------------------------------------------------------------- surfaces */
static void surf_destroy(ComObj *o) {
    Surface *s = (Surface *)o;
    if (s->mem)
        gfree(s->mem);
    if (s->gl_tex)
        glDeleteTextures(1, &s->gl_tex);
    if (s->gl_rb)
        glDeleteRenderbuffers(1, &s->gl_rb);
    free(s);
}

Surface *surf_create(Device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
                     bool with_mem) {
    Surface *s = calloc(1, sizeof *s);
    s->com.type = "IDirect3DSurface8";
    s->com.destroy = surf_destroy;
    s->dev = d;
    s->w = w;
    s->h = h;
    s->fmt = fmt;
    s->usage = usage;
    s->pool = pool;
    s->pitch = pitch_for(fmt, w);
    if (with_mem && !fmt_is_depth(fmt)) {
        s->mem = gcalloc(s->pitch * rows_for(fmt, h));
        if (!s->mem) {
            free(s);
            return NULL;
        }
    }
    com_wrap(&s->com, vt_surf(), 0);
    return s;
}

/* Surfaces of a texture share its reference count. */
static void Surf_AddRef(CPU *c) {
    Surface *s = COM_THIS(Surface);
    if (!s)
        RET(1, 0);
    ComObj *o = s->container ? &s->container->com : &s->com;
    com_addref_obj(o);
    RET(1, o->refs);
}

static void Surf_Release(CPU *c) {
    Surface *s = COM_THIS(Surface);
    if (!s)
        RET(1, 0);
    RET(1, com_release_obj(s->container ? &s->container->com : &s->com));
}

static void Surf_GetContainer(CPU *c) {
    Surface *s = COM_THIS(Surface);
    if (!s->container)
        RET(3, E_NOINTERFACE);
    com_addref_obj(&s->container->com);
    put32(ARG(2), s->container->com.guest);
    RET(3, D3D_OK);
}

static void put_desc(uint32_t p, Surface *s) {
    put32(p, s->fmt);
    put32(p + 4, RT_SURFACE);
    put32(p + 8, s->usage);
    put32(p + 12, s->pool);
    put32(p + 16, s->pitch * rows_for(s->fmt, s->h));
    put32(p + 20, 0);
    put32(p + 24, s->w);
    put32(p + 28, s->h);
}

static void Surf_GetDesc(CPU *c) {
    put_desc(ARG(1), COM_THIS(Surface));
    RET(2, D3D_OK);
}

static uint32_t surf_lock(Surface *s, uint32_t plr, uint32_t prect, uint32_t flags) {
    if (!s->mem)
        return D3DERR_INVALIDCALL;
    if ((s->usage & USAGE_RENDERTARGET) || s->is_backbuffer)
        surf_read_back(s);
    uint32_t off = 0;
    if (prect) {
        uint32_t x = get32(prect), y = get32(prect + 4);
        if (s->fmt == FMT_DXT1 || s->fmt == FMT_DXT3 || s->fmt == FMT_DXT5)
            off = (y / 4) * s->pitch + (x / 4) * (s->fmt == FMT_DXT1 ? 8 : 16);
        else
            off = y * s->pitch + x * (uint32_t)fmt_bpp(s->fmt) / 8;
    }
    put32(plr, s->pitch);
    put32(plr + 4, s->mem + off);
    s->locked = true;
    (void)flags;
    return D3D_OK;
}

static void surf_unlock(Surface *s) {
    s->locked = false;
    if (s->container)
        s->container->dirty = true;
}

/* LockRect(pLockedRect, pRect, Flags) */
static void Surf_LockRect(CPU *c) {
    RET(4, surf_lock(COM_THIS(Surface), ARG(1), ARG(2), ARG(3)));
}

static void Surf_UnlockRect(CPU *c) {
    surf_unlock(COM_THIS(Surface));
    RET(1, D3D_OK);
}

void surf_ensure_gl(Surface *s) {
    if (fmt_is_depth(s->fmt)) {
        if (!s->gl_rb) {
            glGenRenderbuffers(1, &s->gl_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, s->gl_rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)s->w, (GLsizei)s->h);
        }
        return;
    }
    if (s->container) {
        tex_bind(s->dev, 0, s->container);
        return;
    }
    if (!s->gl_tex) {
        glGenTextures(1, &s->gl_tex);
        glBindTexture(GL_TEXTURE_2D, s->gl_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)s->w, (GLsizei)s->h, 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
}

/* Copy a render target's pixels into its guest memory (for LockRect). */
void surf_read_back(Surface *s) {
    if (!s->mem || fmt_is_depth(s->fmt))
        return;
    GLuint tex = s->container ? s->container->gl : s->gl_tex;
    if (!tex)
        return;
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, s->level);
    uint32_t *tmp = malloc((size_t)s->w * s->h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)s->w, (GLsizei)s->h, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    g_dev->dirty |= DIRTY_FBO;
    for (uint32_t y = 0; y < s->h; y++) {
        uint8_t *row = G2H(s->mem + y * s->pitch);
        const uint32_t *src = tmp + (size_t)y * s->w;
        for (uint32_t x = 0; x < s->w; x++) {
            uint32_t p = src[x];
            switch (s->fmt) {
            case FMT_A8R8G8B8: case FMT_X8R8G8B8:
                memcpy(row + 4 * x, &p, 4);
                break;
            case FMT_R5G6B5: {
                uint16_t v = (uint16_t)((p >> 8 & 0xf800) | (p >> 5 & 0x07e0) | (p >> 3 & 0x001f));
                memcpy(row + 2 * x, &v, 2);
                break;
            }
            case FMT_X1R5G5B5: case FMT_A1R5G5B5: {
                uint16_t v = (uint16_t)((p >> 16 & 0x8000) | (p >> 9 & 0x7c00) | (p >> 6 & 0x03e0) | (p >> 3 & 0x001f));
                memcpy(row + 2 * x, &v, 2);
                break;
            }
            }
        }
    }
    free(tmp);
}

static const GuestFn s_surf_vtbl[] = {
    Res_QueryInterface, Surf_AddRef, Surf_Release, Res_GetDevice, Res_SetPrivateData,
    Res_GetPrivateData, Res_FreePrivateData, Surf_GetContainer, Surf_GetDesc, Surf_LockRect,
    Surf_UnlockRect,
};

/* ------------------------------------------------------------- textures */
static void tex_destroy(ComObj *o) {
    Texture *t = (Texture *)o;
    int faces = t->type == RT_CUBETEXTURE ? 6 : 1;
    for (int f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < t->levels; l++) {
            Surface *s = t->surf[f][l];
            if (s) {
                s->container = NULL;
                s->com.refs = 1;
                com_release_obj(&s->com);
            }
        }
    }
    if (t->gl)
        glDeleteTextures(1, &t->gl);
    free(t);
}

uint32_t tex_create(Device *d, int type, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage,
                    uint32_t fmt, uint32_t pool) {
    if (!fmt_bpp(fmt) || fmt_is_depth(fmt) || w == 0 || h == 0) {
        LOGW("CreateTexture: unsupported format %08x (%ux%u)", fmt, w, h);
        return 0;
    }
    uint32_t max_levels = 1;
    for (uint32_t s = w > h ? w : h; s > 1; s >>= 1)
        max_levels++;
    if (levels == 0 || levels > max_levels)
        levels = max_levels;
    if (levels > 16)
        levels = 16;
    Texture *t = calloc(1, sizeof *t);
    t->com.type = type == RT_CUBETEXTURE ? "IDirect3DCubeTexture8" : "IDirect3DTexture8";
    t->com.destroy = tex_destroy;
    t->dev = d;
    t->type = type;
    t->fmt = fmt;
    t->usage = usage;
    t->pool = pool;
    t->w = w;
    t->h = h;
    t->levels = levels;
    t->dirty = true;
    int faces = type == RT_CUBETEXTURE ? 6 : 1;
    for (int f = 0; f < faces; f++) {
        uint32_t lw = w, lh = h;
        for (uint32_t l = 0; l < levels; l++) {
            Surface *s = surf_create(d, lw, lh, fmt, usage, pool, true);
            if (!s) {
                t->levels = l;
                tex_destroy(&t->com);
                return 0;
            }
            s->container = t;
            s->level = (int)l;
            s->face = f;
            t->surf[f][l] = s;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
    }
    return com_wrap(&t->com, type == RT_CUBETEXTURE ? vt_cube() : vt_tex(), 0);
}

GLuint tex_gl_target(Texture *t) { return t->type == RT_CUBETEXTURE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D; }

/* Expand an unsupported-by-GL format to BGRA8. */
static void *convert_level(Surface *s, GlFmt *g) {
    (void)s;
    (void)g;
    return NULL;
}

static void tex_upload(Texture *t) {
    GLenum target = tex_gl_target(t);
    GlFmt g;
    bool known = gl_format(t->fmt, &g);
    bool fresh = !t->gl;
    if (fresh) {
        glGenTextures(1, &t->gl);
        glBindTexture(target, t->gl);
        glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
        if (known) {
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, g.swz[0]);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, g.swz[1]);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, g.swz[2]);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, g.swz[3]);
        }
    } else {
        glBindTexture(target, t->gl);
    }
    if (t->usage & USAGE_RENDERTARGET) {
        if (fresh) {
            uint32_t lw = t->w, lh = t->h;
            for (uint32_t l = 0; l < t->levels; l++) {
                glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)lw, (GLsizei)lh, 0, GL_BGRA,
                             GL_UNSIGNED_BYTE, NULL);
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
            }
        }
        t->dirty = false;
        return;
    }
    int faces = t->type == RT_CUBETEXTURE ? 6 : 1;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (int f = 0; f < faces; f++) {
        GLenum ft = t->type == RT_CUBETEXTURE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f : GL_TEXTURE_2D;
        for (uint32_t l = 0; l < t->levels; l++) {
            Surface *s = t->surf[f][l];
            if (t->fmt == FMT_DXT1 || t->fmt == FMT_DXT3 || t->fmt == FMT_DXT5) {
                GLenum cf = t->fmt == FMT_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                          : t->fmt == FMT_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                                               : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                glCompressedTexImage2D(ft, (GLint)l, cf, (GLsizei)s->w, (GLsizei)s->h, 0,
                                       (GLsizei)(s->pitch * rows_for(t->fmt, s->h)), G2H(s->mem));
            } else if (known) {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(s->pitch * 8 / (uint32_t)fmt_bpp(t->fmt)));
                glTexImage2D(ft, (GLint)l, g.internal, (GLsizei)s->w, (GLsizei)s->h, 0, g.format,
                             g.type, G2H(s->mem));
            } else {
                void *px = convert_level(s, &g);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexImage2D(ft, (GLint)l, GL_RGBA8, (GLsizei)s->w, (GLsizei)s->h, 0, GL_BGRA,
                             GL_UNSIGNED_BYTE, px);
                free(px);
            }
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    t->dirty = false;
}

void tex_bind(Device *d, int unit, Texture *t) {
    (void)d;
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    if (!t->gl || t->dirty)
        tex_upload(t);
    else
        glBindTexture(tex_gl_target(t), t->gl);
}

static void Tex_GetType(CPU *c) { RET(1, COM_THIS(Texture)->type); }

static void Tex_SetLOD(CPU *c) {
    Texture *t = COM_THIS(Texture);
    uint32_t old = t->lod;
    t->lod = ARG(1);
    RET(2, old);
}

static void Tex_GetLOD(CPU *c) { RET(1, COM_THIS(Texture)->lod); }
static void Tex_GetLevelCount(CPU *c) { RET(1, COM_THIS(Texture)->levels); }

/* GetLevelDesc(Level, pDesc) */
static void Tex_GetLevelDesc(CPU *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) >= t->levels)
        RET(3, D3DERR_INVALIDCALL);
    put_desc(ARG(2), t->surf[0][ARG(1)]);
    RET(3, D3D_OK);
}

/* GetSurfaceLevel(Level, ppSurface) */
static void Tex_GetSurfaceLevel(CPU *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) >= t->levels)
        RET(3, D3DERR_INVALIDCALL);
    com_addref_obj(&t->com);
    put32(ARG(2), t->surf[0][ARG(1)]->com.guest);
    RET(3, D3D_OK);
}

/* LockRect(Level, pLockedRect, pRect, Flags) */
static void Tex_LockRect(CPU *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) >= t->levels)
        RET(5, D3DERR_INVALIDCALL);
    RET(5, surf_lock(t->surf[0][ARG(1)], ARG(2), ARG(3), ARG(4)));
}

static void Tex_UnlockRect(CPU *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) < t->levels)
        surf_unlock(t->surf[0][ARG(1)]);
    RET(2, D3D_OK);
}

static void Tex_AddDirtyRect(CPU *c) {
    COM_THIS(Texture)->dirty = true;
    RET(2, D3D_OK);
}

/* Cube: GetCubeMapSurface(FaceType, Level, ppSurface) */
static void Cube_GetCubeMapSurface(CPU *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) >= 6 || ARG(2) >= t->levels)
        RET(4, D3DERR_INVALIDCALL);
    com_addref_obj(&t->com);
    put32(ARG(3), t->surf[ARG(1)][ARG(2)]->com.guest);
    RET(4, D3D_OK);
}

/* LockRect(FaceType, Level, pLockedRect, pRect, Flags) */
static void Cube_LockRect(CPU *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) >= 6 || ARG(2) >= t->levels)
        RET(6, D3DERR_INVALIDCALL);
    RET(6, surf_lock(t->surf[ARG(1)][ARG(2)], ARG(3), ARG(4), ARG(5)));
}

static void Cube_UnlockRect(CPU *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) < 6 && ARG(2) < t->levels)
        surf_unlock(t->surf[ARG(1)][ARG(2)]);
    RET(3, D3D_OK);
}

static void Cube_AddDirtyRect(CPU *c) {
    COM_THIS(Texture)->dirty = true;
    RET(3, D3D_OK);
}

static const GuestFn s_tex_vtbl[] = {
    Res_QueryInterface, com_AddRef, com_Release, Res_GetDevice, Res_SetPrivateData,
    Res_GetPrivateData, Res_FreePrivateData, Res_SetPriority, Res_GetPriority, Res_PreLoad,
    Tex_GetType, Tex_SetLOD, Tex_GetLOD, Tex_GetLevelCount, Tex_GetLevelDesc, Tex_GetSurfaceLevel,
    Tex_LockRect, Tex_UnlockRect, Tex_AddDirtyRect,
};

static const GuestFn s_cube_vtbl[] = {
    Res_QueryInterface, com_AddRef, com_Release, Res_GetDevice, Res_SetPrivateData,
    Res_GetPrivateData, Res_FreePrivateData, Res_SetPriority, Res_GetPriority, Res_PreLoad,
    Tex_GetType, Tex_SetLOD, Tex_GetLOD, Tex_GetLevelCount, Tex_GetLevelDesc,
    Cube_GetCubeMapSurface, Cube_LockRect, Cube_UnlockRect, Cube_AddDirtyRect,
};

/* -------------------------------------------------------------- buffers */
static void buf_destroy(ComObj *o) {
    Buffer *b = (Buffer *)o;
    if (b->mem)
        gfree(b->mem);
    if (b->gl)
        glDeleteBuffers(1, &b->gl);
    free(b);
}

uint32_t buf_create(Device *d, int type, uint32_t size, uint32_t usage, uint32_t fvf_or_fmt,
                    uint32_t pool) {
    Buffer *b = calloc(1, sizeof *b);
    b->com.type = type == RT_VERTEXBUFFER ? "IDirect3DVertexBuffer8" : "IDirect3DIndexBuffer8";
    b->com.destroy = buf_destroy;
    b->dev = d;
    b->type = type;
    b->size = size;
    b->usage = usage;
    b->pool = pool;
    if (type == RT_VERTEXBUFFER)
        b->fvf = fvf_or_fmt;
    else
        b->fmt = fvf_or_fmt;
    b->mem = gcalloc(size ? size : 4);
    if (!b->mem) {
        free(b);
        return 0;
    }
    b->dirty = true;
    b->dirty_lo = 0;
    b->dirty_hi = size;
    return com_wrap(&b->com, type == RT_VERTEXBUFFER ? vt_vb() : vt_ib(), 0);
}

void buf_sync(Buffer *b) {
    GLenum target = b->type == RT_VERTEXBUFFER ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
    if (!b->gl) {
        glGenBuffers(1, &b->gl);
        glBindBuffer(target, b->gl);
        glBufferData(target, b->size, G2H(b->mem), (b->usage & USAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        b->dirty = false;
        return;
    }
    glBindBuffer(target, b->gl);
    if (b->dirty) {
        if (b->dirty_lo == 0 && b->dirty_hi >= b->size)
            glBufferData(target, b->size, G2H(b->mem), (b->usage & USAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        else
            glBufferSubData(target, b->dirty_lo, b->dirty_hi - b->dirty_lo, G2H(b->mem + b->dirty_lo));
        b->dirty = false;
    }
}

static void Buf_GetType(CPU *c) { RET(1, COM_THIS(Buffer)->type); }

/* Lock(OffsetToLock, SizeToLock, ppbData, Flags) */
static void Buf_Lock(CPU *c) {
    Buffer *b = COM_THIS(Buffer);
    uint32_t off = ARG(1), size = ARG(2);
    if (off > b->size)
        RET(5, D3DERR_INVALIDCALL);
    if (size == 0 || off + size > b->size)
        size = b->size - off;
    put32(ARG(3), b->mem + off);
    if (!(ARG(4) & 0x10)) {   /* not D3DLOCK_READONLY */
        if (!b->dirty) {
            b->dirty_lo = off;
            b->dirty_hi = off + size;
        } else {
            if (off < b->dirty_lo)
                b->dirty_lo = off;
            if (off + size > b->dirty_hi)
                b->dirty_hi = off + size;
        }
        b->dirty = true;
    }
    RET(5, D3D_OK);
}

static void Buf_Unlock(CPU *c) { RET(1, D3D_OK); }

static void VB_GetDesc(CPU *c) {
    Buffer *b = COM_THIS(Buffer);
    uint32_t p = ARG(1);
    put32(p, 100);   /* D3DFMT_VERTEXDATA */
    put32(p + 4, RT_VERTEXBUFFER);
    put32(p + 8, b->usage);
    put32(p + 12, b->pool);
    put32(p + 16, b->size);
    put32(p + 20, b->fvf);
    RET(2, D3D_OK);
}

static void IB_GetDesc(CPU *c) {
    Buffer *b = COM_THIS(Buffer);
    uint32_t p = ARG(1);
    put32(p, b->fmt);
    put32(p + 4, RT_INDEXBUFFER);
    put32(p + 8, b->usage);
    put32(p + 12, b->pool);
    put32(p + 16, b->size);
    RET(2, D3D_OK);
}

static const GuestFn s_vb_vtbl[] = {
    Res_QueryInterface, com_AddRef, com_Release, Res_GetDevice, Res_SetPrivateData,
    Res_GetPrivateData, Res_FreePrivateData, Res_SetPriority, Res_GetPriority, Res_PreLoad,
    Buf_GetType, Buf_Lock, Buf_Unlock, VB_GetDesc,
};

static const GuestFn s_ib_vtbl[] = {
    Res_QueryInterface, com_AddRef, com_Release, Res_GetDevice, Res_SetPrivateData,
    Res_GetPrivateData, Res_FreePrivateData, Res_SetPriority, Res_GetPriority, Res_PreLoad,
    Buf_GetType, Buf_Lock, Buf_Unlock, IB_GetDesc,
};

#define VT(name, arr) com_vtbl(name, arr, (int)(sizeof arr / sizeof *arr))

COM_VTABLE("IDirect3DTexture8", s_tex_vtbl)
COM_VTABLE("IDirect3DCubeTexture8", s_cube_vtbl)
COM_VTABLE("IDirect3DSurface8", s_surf_vtbl)
COM_VTABLE("IDirect3DVertexBuffer8", s_vb_vtbl)
COM_VTABLE("IDirect3DIndexBuffer8", s_ib_vtbl)

static uint32_t vt_tex(void) { return VT("IDirect3DTexture8", s_tex_vtbl); }
static uint32_t vt_cube(void) { return VT("IDirect3DCubeTexture8", s_cube_vtbl); }
static uint32_t vt_surf(void) { return VT("IDirect3DSurface8", s_surf_vtbl); }
static uint32_t vt_vb(void) { return VT("IDirect3DVertexBuffer8", s_vb_vtbl); }
static uint32_t vt_ib(void) { return VT("IDirect3DIndexBuffer8", s_ib_vtbl); }

/* -------------------------------------------------------------- savestates
 * Contents live in guest memory, so only descriptions are written; GL
 * objects are recreated (and uploaded) on first use.  Render target
 * contents come back with the next frame. */
static uint32_t guest_of(const void *o) { return o ? ((const ComObj *)o)->guest : 0; }
#define LINK_PTR(v) ((void *)(uintptr_t)(v))

static void surf_snap_save(ComObj *o, SnapBuf *b) {
    Surface *s = (Surface *)o;
    sb_u32(b, guest_of(s->dev));
    sb_u32(b, guest_of(s->container));
    uint32_t v[] = {(uint32_t)s->level, (uint32_t)s->face, s->fmt, s->usage, s->pool, s->w, s->h,
                    s->pitch, s->mem, s->locked, s->is_backbuffer};
    sb_put(b, v, sizeof v);
}

static ComObj *surf_snap_load(SnapBuf *b) {
    Surface *s = calloc(1, sizeof *s);
    s->com.destroy = surf_destroy;
    s->dev = LINK_PTR(sb_get_u32(b));
    s->container = LINK_PTR(sb_get_u32(b));
    uint32_t v[11];
    sb_get(b, v, sizeof v);
    s->level = (int)v[0];
    s->face = (int)v[1];
    s->fmt = v[2];
    s->usage = v[3];
    s->pool = v[4];
    s->w = v[5];
    s->h = v[6];
    s->pitch = v[7];
    s->mem = v[8];
    s->locked = v[9] != 0;
    s->is_backbuffer = v[10] != 0;
    return &s->com;
}

static void surf_snap_link(ComObj *o) {
    Surface *s = (Surface *)o;
    COM_LINK(s->dev, Device);
    COM_LINK(s->container, Texture);
}

static void surf_snap_drop(ComObj *o) {
    Surface *s = (Surface *)o;
    if (s->gl_tex)
        glDeleteTextures(1, &s->gl_tex);
    if (s->gl_rb)
        glDeleteRenderbuffers(1, &s->gl_rb);
    free(s);
}

static void tex_snap_save(ComObj *o, SnapBuf *b) {
    Texture *t = (Texture *)o;
    sb_u32(b, guest_of(t->dev));
    uint32_t v[] = {(uint32_t)t->type, t->fmt, t->usage, t->pool, t->w, t->h, t->levels, t->lod};
    sb_put(b, v, sizeof v);
    for (int f = 0; f < 6; f++)
        for (int l = 0; l < 16; l++)
            sb_u32(b, guest_of(t->surf[f][l]));
}

static ComObj *tex_snap_load(SnapBuf *b) {
    Texture *t = calloc(1, sizeof *t);
    t->com.destroy = tex_destroy;
    t->dev = LINK_PTR(sb_get_u32(b));
    uint32_t v[8];
    sb_get(b, v, sizeof v);
    t->type = (int)v[0];
    t->fmt = v[1];
    t->usage = v[2];
    t->pool = v[3];
    t->w = v[4];
    t->h = v[5];
    t->levels = v[6] > 16 ? 16 : v[6];
    t->lod = v[7];
    for (int f = 0; f < 6; f++)
        for (int l = 0; l < 16; l++)
            t->surf[f][l] = LINK_PTR(sb_get_u32(b));
    t->dirty = true;
    return &t->com;
}

static void tex_snap_link(ComObj *o) {
    Texture *t = (Texture *)o;
    COM_LINK(t->dev, Device);
    for (int f = 0; f < 6; f++)
        for (int l = 0; l < 16; l++)
            COM_LINK(t->surf[f][l], Surface);
}

static void tex_snap_drop(ComObj *o) {
    Texture *t = (Texture *)o;
    if (t->gl)
        glDeleteTextures(1, &t->gl);
    free(t);
}

static void buf_snap_save(ComObj *o, SnapBuf *b) {
    Buffer *u = (Buffer *)o;
    sb_u32(b, guest_of(u->dev));
    uint32_t v[] = {(uint32_t)u->type, u->size, u->usage, u->pool, u->fvf, u->fmt, u->mem};
    sb_put(b, v, sizeof v);
}

static ComObj *buf_snap_load(SnapBuf *b) {
    Buffer *u = calloc(1, sizeof *u);
    u->com.destroy = buf_destroy;
    u->dev = LINK_PTR(sb_get_u32(b));
    uint32_t v[7];
    sb_get(b, v, sizeof v);
    u->type = (int)v[0];
    u->size = v[1];
    u->usage = v[2];
    u->pool = v[3];
    u->fvf = v[4];
    u->fmt = v[5];
    u->mem = v[6];
    u->dirty = true;
    u->dirty_lo = 0;
    u->dirty_hi = u->size;
    return &u->com;
}

static void buf_snap_link(ComObj *o) { COM_LINK(((Buffer *)o)->dev, Device); }

static void buf_snap_drop(ComObj *o) {
    Buffer *u = (Buffer *)o;
    if (u->gl)
        glDeleteBuffers(1, &u->gl);
    free(u);
}

static const ComSnapClass s_res_classes[] = {
    {"IDirect3DSurface8", surf_snap_save, surf_snap_load, surf_snap_link, surf_snap_drop},
    {"IDirect3DTexture8", tex_snap_save, tex_snap_load, tex_snap_link, tex_snap_drop},
    {"IDirect3DCubeTexture8", tex_snap_save, tex_snap_load, tex_snap_link, tex_snap_drop},
    {"IDirect3DVertexBuffer8", buf_snap_save, buf_snap_load, buf_snap_link, buf_snap_drop},
    {"IDirect3DIndexBuffer8", buf_snap_save, buf_snap_load, buf_snap_link, buf_snap_drop},
};

__attribute__((constructor)) static void res_snap_register(void) {
    for (size_t i = 0; i < sizeof s_res_classes / sizeof *s_res_classes; i++)
        com_snap_class(&s_res_classes[i]);
}
