/* IDirect3D8: adapter/mode enumeration, capabilities, device creation. */
#include <SDL3/SDL.h>
#include "d3d8_internal.h"

typedef struct {
    ComObj com;
} D3D8;

/* Modes offered to the game: common 4:3 and widescreen sizes below the
   desktop size, then the desktop's native size, each in 32- and 16-bit. */
static const uint32_t s_sizes[][2] = {
    {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960},
    {1280, 1024}, {1366, 768}, {1600, 900}, {1600, 1200}, {1920, 1080}, {2560, 1440},
};
static uint32_t s_modes[64][2];
static int s_nmodes;

static void init_modes(void) {
    if (s_nmodes)
        return;
    /* In pixels: high-DPI macOS reports desktop modes in points. */
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    uint32_t mw = dm ? (uint32_t)SDL_lroundf((float)dm->w * dm->pixel_density) : 1920;
    uint32_t mh = dm ? (uint32_t)SDL_lroundf((float)dm->h * dm->pixel_density) : 1080;
    if (mw < 1024 || mh < 768)
        mw = 1024, mh = 768;
    for (size_t i = 0; i < sizeof s_sizes / sizeof *s_sizes; i++) {
        if (s_sizes[i][0] <= mw && s_sizes[i][1] <= mh && (s_sizes[i][0] != mw || s_sizes[i][1] != mh)) {
            s_modes[s_nmodes][0] = s_sizes[i][0];
            s_modes[s_nmodes][1] = s_sizes[i][1];
            s_nmodes++;
        }
    }
    s_modes[s_nmodes][0] = mw;
    s_modes[s_nmodes][1] = mh;
    s_nmodes++;
}

/* Called from patched game code (tools/recomp/overrides.py PATCHES). */
uint32_t lr2_native_w(void) {
    init_modes();
    return s_modes[s_nmodes - 1][0];
}

uint32_t lr2_native_h(void) {
    init_modes();
    return s_modes[s_nmodes - 1][1];
}

/* Hor+: keep the 4:3 vertical FOV on wider screens by widening the
   horizontal one (degrees), from the back buffer's aspect ratio. */
double lr2_hor_fov(float deg) {
    double a = g_dev && g_dev->pp[1] ? (double)g_dev->pp[0] / g_dev->pp[1] : 4.0 / 3.0;
    if (a <= 4.0 / 3.0)
        return deg;
    return 2.0 * atan(tan(deg * SDL_PI_D / 360.0) * a * 0.75) * 360.0 / (2.0 * SDL_PI_D);
}

int fmt_bpp(uint32_t f) {
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_D32: case FMT_D24S8: case FMT_D24X8:
    case FMT_D24X4S4: case FMT_INDEX32:
        return 32;
    case FMT_R8G8B8:
        return 24;
    case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_X4R4G4B4:
    case FMT_A8R3G3B2: case FMT_A8L8: case FMT_A8P8: case FMT_V8U8: case FMT_D16: case FMT_D16_LOCKABLE:
    case FMT_D15S1: case FMT_INDEX16:
        return 16;
    case FMT_A8: case FMT_L8: case FMT_P8: case FMT_R3G3B2: case FMT_A4L4:
        return 8;
    case FMT_DXT1:
        return 4;
    case FMT_DXT3: case FMT_DXT5:
        return 8;
    }
    return 0;
}

bool fmt_is_depth(uint32_t f) {
    return f == FMT_D16 || f == FMT_D16_LOCKABLE || f == FMT_D24S8 || f == FMT_D24X8 ||
           f == FMT_D32 || f == FMT_D15S1 || f == FMT_D24X4S4;
}

bool fmt_texture_ok(uint32_t f) {
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5:
    case FMT_A4R4G4B4: case FMT_X4R4G4B4: case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_P8:
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------- methods */
static void D3D8_QueryInterface(CPU *c) {
    put32(ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

static void D3D8_RegisterSoftwareDevice(CPU *c) { RET(2, D3DERR_INVALIDCALL); }
static void D3D8_GetAdapterCount(CPU *c) { RET(1, 1); }

/* D3DADAPTER_IDENTIFIER8: Driver[512], Description[512], DriverVersion,
   VendorId, DeviceId, SubSysId, Revision, DeviceIdentifier, WHQLLevel */
static void D3D8_GetAdapterIdentifier(CPU *c) {
    uint32_t p = ARG(3);
    if (ARG(1) != 0)
        RET(4, D3DERR_INVALIDCALL);
    memset(G2H(p), 0, 1068);
    strcpy((char *)G2H(p), "nv4_disp.dll");
    strcpy((char *)G2H(p + 512), "NVIDIA GeForce4 Ti 4600");
    put32(p + 1024, 0x00010000 | 2960);   /* DriverVersion.LowPart */
    put32(p + 1028, 0x00060000 | 14);     /* HighPart: 6.14 */
    put32(p + 1032, 0x10de);
    put32(p + 1036, 0x0250);
    put32(p + 1040, 0);
    put32(p + 1044, 0xa3);
    put32(p + 1064, 1);
    RET(4, D3D_OK);
}

static void D3D8_GetAdapterModeCount(CPU *c) {
    init_modes();
    RET(2, (uint32_t)s_nmodes * 2);
}

/* D3DDISPLAYMODE: Width, Height, RefreshRate, Format */
static void put_mode(uint32_t p, uint32_t w, uint32_t h, uint32_t fmt) {
    put32(p, w);
    put32(p + 4, h);
    put32(p + 8, 60);
    put32(p + 12, fmt);
}

static void D3D8_EnumAdapterModes(CPU *c) {
    init_modes();
    uint32_t i = ARG(2);
    if (i >= (uint32_t)s_nmodes * 2)
        RET(4, D3DERR_INVALIDCALL);
    uint32_t m = i % (uint32_t)s_nmodes;
    put_mode(ARG(3), s_modes[m][0], s_modes[m][1], i < (uint32_t)s_nmodes ? FMT_X8R8G8B8 : FMT_R5G6B5);
    RET(4, D3D_OK);
}

static void D3D8_GetAdapterDisplayMode(CPU *c) {
    init_modes();
    uint32_t m = (uint32_t)s_nmodes - 1;
    put_mode(ARG(2), s_modes[m][0], s_modes[m][1], FMT_X8R8G8B8);
    RET(3, D3D_OK);
}

/* CheckDeviceType(Adapter, CheckType, DisplayFormat, BackBufferFormat, Windowed) */
static void D3D8_CheckDeviceType(CPU *c) {
    uint32_t df = ARG(3), bf = ARG(4);
    bool ok = (df == FMT_X8R8G8B8 || df == FMT_R5G6B5) &&
              (bf == FMT_X8R8G8B8 || bf == FMT_A8R8G8B8 || bf == FMT_R5G6B5 || bf == FMT_UNKNOWN);
    RET(6, ok ? D3D_OK : D3DERR_NOTAVAILABLE);
}

/* CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat) */
static void D3D8_CheckDeviceFormat(CPU *c) {
    uint32_t usage = ARG(4), rtype = ARG(5), fmt = ARG(6);
    bool ok;
    if (usage & USAGE_DEPTHSTENCIL)
        ok = fmt == FMT_D16 || fmt == FMT_D24S8 || fmt == FMT_D24X8 || fmt == FMT_D32;
    else if (usage & USAGE_RENDERTARGET)
        ok = fmt == FMT_X8R8G8B8 || fmt == FMT_A8R8G8B8 || fmt == FMT_R5G6B5;
    else if (rtype == RT_SURFACE || rtype == RT_TEXTURE || rtype == RT_CUBETEXTURE)
        ok = fmt_texture_ok(fmt) && fmt != FMT_P8;
    else
        ok = false;
    LOGD("CheckDeviceFormat(usage=%x, rtype=%u, fmt=%08x) -> %d", usage, rtype, fmt, ok);
    RET(7, ok ? D3D_OK : D3DERR_NOTAVAILABLE);
}

static void D3D8_CheckDeviceMultiSampleType(CPU *c) {
    RET(6, ARG(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE);
}

static void D3D8_CheckDepthStencilMatch(CPU *c) { RET(6, D3D_OK); }

void d3d8_fill_caps(uint32_t p) {
    memset(G2H(p), 0, 212);
    uint32_t *k = G2H(p);
    float *f = G2H(p);
    k[0] = 1;                  /* DeviceType = HAL */
    k[1] = 0;
    k[2] = 0;                  /* Caps */
    k[3] = 0x00020000 | 0x00080000 | 0x20000000;   /* FULLSCREENGAMMA|CANRENDERWINDOWED|DYNAMICTEXTURES */
    k[4] = 0x20;               /* Caps3 */
    k[5] = 0x80000000u | 1;    /* PresentationIntervals: IMMEDIATE|ONE */
    k[6] = 0;                  /* CursorCaps */
    k[7] = 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x2000 | 0x4000 | 0x8000 |
           0x10000 | 0x80000;  /* DevCaps incl. HWTRANSFORMANDLIGHT|HWRASTERIZATION */
    k[8] = 2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800;   /* PrimitiveMiscCaps */
    k[9] = 0x10 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x10000 | 0x20000 | 0x100000 | 0x200000;
    k[10] = 0xff;              /* ZCmpCaps */
    k[11] = 0x1fff;            /* SrcBlendCaps */
    k[12] = 0x1fff;            /* DestBlendCaps */
    k[13] = 0xff;              /* AlphaCmpCaps */
    k[14] = 0x8 | 0x200 | 0x4000 | 0x80000;        /* ShadeCaps */
    k[15] = 0x1 | 0x4 | 0x400 | 0x800 | 0x4000 | 0x10000;   /* TextureCaps */
    k[16] = 0x100 | 0x200 | 0x400 | 0x10000 | 0x20000 | 0x1000000 | 0x2000000 | 0x4000000;
    k[17] = k[16];             /* CubeTextureFilterCaps */
    k[18] = 0;
    k[19] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20;   /* TextureAddressCaps */
    k[20] = 0;
    k[21] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;          /* LineCaps */
    k[22] = 4096;              /* MaxTextureWidth */
    k[23] = 4096;
    k[24] = 0;
    k[25] = 8192;              /* MaxTextureRepeat */
    k[26] = 0;                 /* MaxTextureAspectRatio */
    k[27] = 16;                /* MaxAnisotropy */
    f[28] = 1e10f;             /* MaxVertexW */
    f[29] = -1e8f;             /* guard band */
    f[30] = -1e8f;
    f[31] = 1e8f;
    f[32] = 1e8f;
    f[33] = 0;                 /* ExtentsAdjust */
    k[34] = 0xff;              /* StencilCaps */
    k[35] = 8 | 0x80000;       /* FVFCaps: 8 texcoords, DONOTSTRIPELEMENTS */
    k[36] = 0x03ffffff & ~(0x200000u | 0x400000u);   /* TextureOpCaps (no bump mapping) */
    k[37] = 8;                 /* MaxTextureBlendStages */
    k[38] = 4;                 /* MaxSimultaneousTextures */
    k[39] = 0x1 | 0x2 | 0x8 | 0x10 | 0x20;           /* VertexProcessingCaps */
    k[40] = 8;                 /* MaxActiveLights */
    k[41] = 6;                 /* MaxUserClipPlanes */
    k[42] = 4;                 /* MaxVertexBlendMatrices */
    k[43] = 0;                 /* MaxVertexBlendMatrixIndex */
    f[44] = 64.0f;             /* MaxPointSize */
    k[45] = 0xfffff;           /* MaxPrimitiveCount */
    k[46] = 0xfffff;           /* MaxVertexIndex */
    k[47] = 1;                 /* MaxStreams */
    k[48] = 256;               /* MaxStreamStride */
    k[49] = 0;                 /* VertexShaderVersion: none */
    k[50] = 0;
    k[51] = 0;                 /* PixelShaderVersion: none */
    f[52] = 1.0f;
}

static void D3D8_GetDeviceCaps(CPU *c) {
    d3d8_fill_caps(ARG(3));
    RET(4, D3D_OK);
}

static void D3D8_GetAdapterMonitor(CPU *c) { RET(2, 0x00a50000u); }

/* CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppDevice) */
static void D3D8_CreateDevice(CPU *c) {
    uint32_t pp[13];
    for (int i = 0; i < 13; i++)
        pp[i] = get32(ARG(5) + 4u * (uint32_t)i);
    uint32_t dev = 0;
    uint32_t hr = device_create(c, ARG(0), ARG(3), ARG(4), pp, &dev);
    if (hr == D3D_OK)
        for (int i = 0; i < 13; i++)
            put32(ARG(5) + 4u * (uint32_t)i, pp[i]);
    put32(ARG(6), dev);
    RET(7, hr);
}

static const GuestFn s_d3d8_vtbl[] = {
    D3D8_QueryInterface, com_AddRef, com_Release, D3D8_RegisterSoftwareDevice,
    D3D8_GetAdapterCount, D3D8_GetAdapterIdentifier, D3D8_GetAdapterModeCount,
    D3D8_EnumAdapterModes, D3D8_GetAdapterDisplayMode, D3D8_CheckDeviceType,
    D3D8_CheckDeviceFormat, D3D8_CheckDeviceMultiSampleType, D3D8_CheckDepthStencilMatch,
    D3D8_GetDeviceCaps, D3D8_GetAdapterMonitor, D3D8_CreateDevice,
};

void imp_D3D8_Direct3DCreate8(CPU *c) {
    LOGI("Direct3DCreate8(%u)", ARG(0));
    D3D8 *d = calloc(1, sizeof *d);
    d->com.type = "IDirect3D8";
    uint32_t vt = com_vtbl("IDirect3D8", s_d3d8_vtbl, (int)(sizeof s_d3d8_vtbl / sizeof *s_d3d8_vtbl));
    RET(1, com_wrap(&d->com, vt, 0));
}

COM_VTABLE("IDirect3D8", s_d3d8_vtbl)

/* No host state beyond the header. */
static void d3d8_snap_save(ComObj *o, SnapBuf *b) { (void)o, (void)b; }
static ComObj *d3d8_snap_load(SnapBuf *b) {
    (void)b;
    return calloc(1, sizeof(D3D8));
}

static const ComSnapClass s_d3d8_class = {"IDirect3D8", d3d8_snap_save, d3d8_snap_load, NULL, NULL};

__attribute__((constructor)) static void d3d8_snap_register(void) { com_snap_class(&s_d3d8_class); }
