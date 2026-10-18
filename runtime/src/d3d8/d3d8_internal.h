/* Direct3D 8 on OpenGL 3.3 core: shared definitions.
 *
 * Lockable resource contents live in guest memory (so savestates capture
 * them); GL objects are caches rebuilt from that memory when dirty.  The
 * render target is an FBO stored top-down (row 0 = top of the screen),
 * matching D3D, and Present blits it flipped to the window.
 */
#ifndef D3D8_INTERNAL_H
#define D3D8_INTERNAL_H

#include <SDL3/SDL.h>
#include "../win32/com.h"
#include "gl.h"

/* ---------------------------------------------------------------- errors */
#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086cu
#define D3DERR_NOTAVAILABLE 0x8876086au
#define D3DERR_NOTFOUND 0x88760866u
#define D3DERR_DEVICELOST 0x88760868u
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017cu

/* --------------------------------------------------------------- formats */
enum {
    FMT_UNKNOWN = 0, FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23,
    FMT_X1R5G5B5 = 24, FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_R3G3B2 = 27, FMT_A8 = 28,
    FMT_A8R3G3B2 = 29, FMT_X4R4G4B4 = 30, FMT_A8P8 = 40, FMT_P8 = 41, FMT_L8 = 50,
    FMT_A8L8 = 51, FMT_A4L4 = 52, FMT_V8U8 = 60, FMT_D16_LOCKABLE = 70, FMT_D32 = 71,
    FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D16 = 80, FMT_D24X8 = 77, FMT_D24X4S4 = 79,
    FMT_INDEX16 = 101, FMT_INDEX32 = 102,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')

int fmt_bpp(uint32_t fmt);           /* bits per pixel, 0 if unsupported */
bool fmt_is_depth(uint32_t fmt);
bool fmt_texture_ok(uint32_t fmt);   /* usable as a texture */

/* ------------------------------------------------------------- resources */
enum { RT_SURFACE = 1, RT_TEXTURE = 3, RT_CUBETEXTURE = 5, RT_VERTEXBUFFER = 6, RT_INDEXBUFFER = 7 };
enum { POOL_DEFAULT = 0, POOL_MANAGED = 1, POOL_SYSTEMMEM = 2 };
#define USAGE_RENDERTARGET 1u
#define USAGE_DEPTHSTENCIL 2u
#define USAGE_DYNAMIC 0x200u

typedef struct Device Device;
typedef struct Texture Texture;

typedef struct Surface {
    ComObj com;
    Device *dev;
    Texture *container;       /* owning texture, or NULL for standalone */
    int level, face;
    uint32_t fmt, usage, pool, w, h, pitch;
    uint32_t mem;             /* guest memory (0 for GL-only surfaces) */
    bool locked;
    /* GL backing for standalone render targets / depth buffers */
    GLuint gl_tex, gl_rb;
    bool is_backbuffer;
} Surface;

struct Texture {
    ComObj com;
    Device *dev;
    int type;                 /* RT_TEXTURE or RT_CUBETEXTURE */
    uint32_t fmt, usage, pool, w, h, levels;
    Surface *surf[6][16];     /* [face][level], created eagerly */
    GLuint gl;
    bool dirty;               /* guest memory changed since last upload */
    uint32_t lod;
};

typedef struct {
    ComObj com;
    Device *dev;
    int type;                 /* RT_VERTEXBUFFER / RT_INDEXBUFFER */
    uint32_t size, usage, pool, fvf, fmt;
    uint32_t mem;
    GLuint gl;
    bool dirty;
    uint32_t dirty_lo, dirty_hi;
} Buffer;

/* ----------------------------------------------------------------- state */
typedef struct { float r, g, b, a; } Color4;
typedef struct { float m[16]; } Mat4;

typedef struct {
    Color4 diffuse, ambient, specular, emissive;
    float power;
} Material;

typedef struct {
    uint32_t type;            /* 1 point, 2 spot, 3 directional */
    Color4 diffuse, specular, ambient;
    float pos[3], dir[3];
    float range, falloff, att0, att1, att2, theta, phi;
} Light;

#define MAX_LIGHTS 64
#define MAX_STAGES 8
#define MAX_RS 256
#define MAX_TSS 32

typedef struct {
    uint32_t x, y, w, h;
    float minz, maxz;
} Viewport;

typedef struct {
    uint32_t rs[MAX_RS];
    uint32_t tss[MAX_STAGES][MAX_TSS];
    Mat4 world[4], view, proj, tex[MAX_STAGES];
    Viewport vp;
    Material mat;
    Light lights[MAX_LIGHTS];
    bool light_set[MAX_LIGHTS], light_on[MAX_LIGHTS];
    float clip[6][4];
    uint32_t tex_obj[MAX_STAGES];      /* guest pointers to bound textures */
    uint32_t vb, stride, ib, base_vertex;
    uint32_t fvf;
} State;

/* State block: which parts of State it captured. */
typedef struct {
    ComObj com;               /* not guest visible; handle = index */
    State s;
    uint8_t rs[MAX_RS], tss[MAX_STAGES][MAX_TSS];
    bool world[4], view, proj, tex[MAX_STAGES], vp, mat, lights[MAX_LIGHTS], clip[6];
    bool tex_obj[MAX_STAGES], vb, ib, fvf;
} StateBlock;

struct Device {
    ComObj com;
    uint32_t d3d;             /* guest IDirect3D8 */
    uint32_t hwnd;
    uint32_t pp[13];          /* presentation parameters */
    SDL_GLContext ctx;
    State st;
    StateBlock *recording;
    Surface *backbuffer, *auto_depth, *rt, *ds;
    GLuint fbo, vao, stream_vbo, stream_ibo;
    bool in_scene;
    uint32_t frame;
    uint32_t dirty;           /* DIRTY_* bits for GL state sync */
};

#define DIRTY_RASTER 1u
#define DIRTY_FBO 2u
#define DIRTY_VIEWPORT 4u

extern Device *g_dev;

/* resources.c */
uint32_t tex_create(Device *d, int type, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage,
                    uint32_t fmt, uint32_t pool);
Surface *surf_create(Device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
                     bool with_mem);
uint32_t buf_create(Device *d, int type, uint32_t size, uint32_t usage, uint32_t fvf_or_fmt,
                    uint32_t pool);
void tex_bind(Device *d, int unit, Texture *t);     /* uploads if dirty */
GLuint tex_gl_target(Texture *t);
void buf_sync(Buffer *b);                           /* uploads if dirty */
void surf_ensure_gl(Surface *s);                    /* RT/DS backing */
void surf_read_back(Surface *s);                    /* RT contents -> guest memory */

/* ffp.c: fixed-function pipeline emulation */
void ffp_draw_setup(Device *d, uint32_t fvf, bool *ok);
void ffp_init(void);

/* device.c */
uint32_t device_create(CPU *c, uint32_t d3d, uint32_t hwnd, uint32_t behavior, const uint32_t *pp,
                       uint32_t *out);

/* Helpers for writing guest structs */
static inline void put32(uint32_t a, uint32_t v) { *(u32u *)G2H(a) = v; }
static inline uint32_t get32(uint32_t a) { return *(u32u *)G2H(a); }
static inline float getf(uint32_t a) { return *(float *)G2H(a); }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

#endif
