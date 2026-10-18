/* Small DLLs: SHFOLDER, GDI32, OLE32 (COM startup and class factory). */
#include "../rt.h"
#include "com.h"

/* ------------------------------------------------------------- SHFOLDER */
/* SHGetFolderPathA(hwnd, csidl, hToken, dwFlags, pszPath) */
void imp_SHFOLDER_SHGetFolderPathA(CPU *c) {
    uint32_t csidl = ARG(1) & 0xff;
    const char *p;
    switch (csidl) {
    case 0x05: p = "C:\\My Documents"; break;                    /* CSIDL_PERSONAL */
    case 0x1a: p = "C:\\Application Data"; break;                /* CSIDL_APPDATA */
    case 0x1c: p = "C:\\Local Settings\\Application Data"; break; /* CSIDL_LOCAL_APPDATA */
    case 0x23: p = "C:\\Common Application Data"; break;         /* CSIDL_COMMON_APPDATA */
    case 0x2e: p = "C:\\Common Documents"; break;                 /* CSIDL_COMMON_DOCUMENTS */
    default: p = "C:\\Folders\\Other"; break;
    }
    LOGD("SHGetFolderPathA(%#x) -> %s", ARG(1), p);
    strcpy((char *)G2H(ARG(4)), p);
    /* Make sure it exists on the host so the game can create subfolders. */
    char host[1100];
    path_to_host(p, host, sizeof host);
    mkdir_p(host);
    RET(5, 0);
}

/* ---------------------------------------------------------------- GDI32 */
void imp_GDI32_GetStockObject(CPU *c) {
    RET(1, 0x00a10000u + ARG(0));   /* opaque brush/font handles */
}

/* ---------------------------------------------------------------- OLE32 */
void imp_OLE32_CoInitialize(CPU *c) { RET(1, 0); }
void imp_OLE32_CoInitializeEx(CPU *c) { RET(2, 0); }
void imp_OLE32_CoUninitialize(CPU *c) { RET(0, 0); }
void imp_OLE32_CoFreeUnusedLibraries(CPU *c) { RET(0, 0); }
void imp_OLE32_CoTaskMemAlloc(CPU *c) { RET(1, gmalloc(ARG(0))); }

void imp_OLE32_CoTaskMemFree(CPU *c) {
    if (ARG(0))
        gfree(ARG(0));
    RET(1, 0);
}

/* CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv) */
void imp_OLE32_CoCreateInstance(CPU *c) {
    const Guid *clsid = G2H(ARG(0)), *iid = G2H(ARG(3));
    uint32_t ppv = ARG(4);
    char cs[40], is[40];
    guid_str(clsid, cs);
    guid_str(iid, is);
    uint32_t obj = 0;
    uint32_t hr = com_create(c, clsid, iid, &obj);
    LOGI("CoCreateInstance(%s, %s) -> %08x", cs, is, hr);
    *(u32u *)G2H(ppv) = hr ? 0 : obj;
    RET(5, hr);
}
