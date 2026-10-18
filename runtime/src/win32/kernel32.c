#define _GNU_SOURCE
/* KERNEL32 subset used by the game and its statically linked MSVC CRT. */
#include "../rt.h"
#include "handles.h"
#include "../savestate.h"
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <fnmatch.h>
#include "os.h"
#include <ctype.h>

#define INVALID_HANDLE_VALUE 0xffffffffu
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102
#define WAIT_FAILED 0xffffffffu
#define INFINITE 0xffffffffu

void path_set_cwd(const char *win);
void path_get_cwd(char *out, size_t sz);

static uint32_t s_cmdline, s_env_a, s_env_w;

void kernel32_init(int argc, char **argv) {
    /* Command line: exe path plus any pass-through game switches (e.g. /nomusic). */
    char cmd[1024] = "\"C:\\LR2\\LEGO Racers 2.exe\"";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-' && i + 1 < argc && strcmp(argv[i], "--no-hd") &&
            strcmp(argv[i], "--windowed")) {
            i++;                /* runtime option and its value */
            continue;
        }
        if (argv[i][0] == '/') {
            strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
            strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
        }
    }
    s_cmdline = gstrdup(cmd);
    s_env_a = gcalloc(16);
    s_env_w = gcalloc(16);
    handles_init();
}

/* ------------------------------------------------------------ misc/process */
void imp_KERNEL32_GetCommandLineA(CPU *c) { RET(0, s_cmdline); }
void imp_KERNEL32_GetEnvironmentStrings(CPU *c) { RET(0, s_env_a); }
void imp_KERNEL32_GetEnvironmentStringsW(CPU *c) { RET(0, s_env_w); }
void imp_KERNEL32_FreeEnvironmentStringsA(CPU *c) { RET(1, 1); }
void imp_KERNEL32_FreeEnvironmentStringsW(CPU *c) { RET(1, 1); }

void imp_KERNEL32_GetEnvironmentVariableA(CPU *c) {
    LOGD("GetEnvironmentVariableA(%s)", ARGS(0));
    set_last_error(203);   /* ERROR_ENVVAR_NOT_FOUND */
    RET(3, 0);
}

void imp_KERNEL32_GetVersion(CPU *c) {
    /* Windows XP: major 5, minor 1, build 2600, NT platform. */
    RET(0, 0x0a280105u);
}

void imp_KERNEL32_GetVersionExA(CPU *c) {
    uint32_t p = ARG(0);
    *(u32u *)G2H(p + 4) = 5;       /* major */
    *(u32u *)G2H(p + 8) = 1;       /* minor */
    *(u32u *)G2H(p + 12) = 2600;   /* build */
    *(u32u *)G2H(p + 16) = 2;      /* VER_PLATFORM_WIN32_NT */
    memset(G2H(p + 20), 0, 128);
    RET(1, 1);
}

void imp_KERNEL32_GetStartupInfoA(CPU *c) {
    uint32_t p = ARG(0);
    memset(G2H(p), 0, 68);
    *(u32u *)G2H(p) = 68;
    RET(1, 0);
}

void imp_KERNEL32_GetModuleHandleA(CPU *c) {
    const char *name = ARGS(0);
    if (!name)
        RET(1, GUEST_IMAGE);
    LOGD("GetModuleHandleA(%s)", name);
    RET(1, module_handle_for(name));
}

void imp_KERNEL32_GetModuleFileNameA(CPU *c) {
    const char *path = "C:\\LR2\\LEGO Racers 2.exe";
    uint32_t n = ARG(2);
    uint32_t len = (uint32_t)strlen(path);
    if (n == 0)
        RET(3, 0);
    if (len >= n)
        len = n - 1;
    memcpy(G2H(ARG(1)), path, len);
    ((char *)G2H(ARG(1)))[len] = 0;
    RET(3, len);
}

void imp_KERNEL32_LoadLibraryA(CPU *c) {
    const char *name = ARGS(0);
    uint32_t h = module_handle_for(name);
    LOGI("LoadLibraryA(%s) -> %08x", name, h);
    if (!h)
        set_last_error(126);
    RET(1, h);
}

void imp_KERNEL32_FreeLibrary(CPU *c) { RET(1, 1); }

void imp_KERNEL32_GetProcAddress(CPU *c) {
    uint32_t mod = ARG(0);
    uint32_t nameptr = ARG(1);
    char ord[16];
    const char *name = nameptr < 0x10000 ? (snprintf(ord, sizeof ord, "#%u", nameptr), ord) : ARGS(1);
    uint32_t a = module_proc(mod, name);
    LOGI("GetProcAddress(%08x, %s) -> %08x", mod, name, a);
    if (!a)
        set_last_error(127);
    RET(2, a);
}

void imp_KERNEL32_ExitProcess(CPU *c) {
    LOGI("ExitProcess(%u)", ARG(0));
    exit((int)ARG(0));
}

void imp_KERNEL32_TerminateProcess(CPU *c) {
    LOGI("TerminateProcess(%u)", ARG(1));
    exit((int)ARG(1));
}

void imp_KERNEL32_GetCurrentProcess(CPU *c) { RET(0, 0xffffffffu); }
void imp_KERNEL32_GetCurrentThread(CPU *c) { RET(0, 0xfffffffeu); }
void imp_KERNEL32_GetCurrentThreadId(CPU *c) { RET(0, thread_current_id()); }
void imp_KERNEL32_SetPriorityClass(CPU *c) { RET(2, 1); }
void imp_KERNEL32_SetThreadPriority(CPU *c) { RET(2, 1); }
void imp_KERNEL32_GetThreadPriority(CPU *c) { RET(1, 0); }
void imp_KERNEL32_SetUnhandledExceptionFilter(CPU *c) { RET(1, 0); }
void imp_KERNEL32_UnhandledExceptionFilter(CPU *c) { RET(1, 0); /* EXCEPTION_CONTINUE_SEARCH */ }
void imp_KERNEL32_GetLastError(CPU *c) { RET(0, get_last_error()); }
void imp_KERNEL32_SetLastError(CPU *c) { set_last_error(ARG(0)); RET(1, 0); }
void imp_KERNEL32_SetHandleCount(CPU *c) { RET(1, ARG(0)); }

void imp_KERNEL32_GetStdHandle(CPU *c) {
    int32_t n = (int32_t)ARG(0);
    RET(1, n == -10 ? 0x10u : n == -11 ? 0x14u : 0x18u);
}

void imp_KERNEL32_SetStdHandle(CPU *c) { RET(2, 1); }

void imp_KERNEL32_GetFileType(CPU *c) {
    uint32_t h = ARG(0);
    if (h == 0x10 || h == 0x14 || h == 0x18)
        RET(1, 2);              /* FILE_TYPE_CHAR */
    RET(1, handle_type(h) == H_FILE ? 1 : 0);
}

void imp_KERNEL32_RaiseException(CPU *c) {
    LOGE("RaiseException(code=%08x, flags=%x, nargs=%u)", ARG(0), ARG(1), ARG(2));
    if (ARG(0) == 0xe06d7363u)
        LOGE("  (C++ exception thrown; exception object %08x, type info %08x)",
             ARG(3) ? *(u32u *)G2H(ARG(3) + 4) : 0, ARG(3) ? *(u32u *)G2H(ARG(3) + 8) : 0);
    TRAP(c, *(u32u *)G2H(c->esp), "RaiseException");
}

void imp_KERNEL32_RtlUnwind(CPU *c) {
    TRAP(c, *(u32u *)G2H(c->esp), "RtlUnwind (SEH unwinding is not supported)");
}

void imp_KERNEL32_MulDiv(CPU *c) {
    int32_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), d = (int32_t)ARG(2);
    if (!d)
        RET(3, 0xffffffffu);
    int64_t r = (int64_t)a * b;
    /* Round half away from zero, like Windows. */
    int64_t q = (r + ((r < 0) ^ (d < 0) ? -(int64_t)llabs(d) / 2 : llabs(d) / 2)) / d;
    RET(3, (uint32_t)(int32_t)q);
}

/* ---------------------------------------------------------------- memory */
#define HEAP_ZERO_MEMORY 0x8u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u

void imp_KERNEL32_HeapCreate(CPU *c) { RET(3, 0x00150000u); }
void imp_KERNEL32_HeapDestroy(CPU *c) { RET(1, 1); }

void imp_KERNEL32_HeapAlloc(CPU *c) {
    uint32_t p = (ARG(1) & HEAP_ZERO_MEMORY) ? gcalloc(ARG(2)) : gmalloc(ARG(2));
    RET(3, p);
}

void imp_KERNEL32_HeapFree(CPU *c) {
    gfree(ARG(2));
    RET(3, 1);
}

void imp_KERNEL32_HeapReAlloc(CPU *c) {
    uint32_t flags = ARG(1), p = ARG(2), n = ARG(3);
    uint32_t old = gmsize(p);
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        /* We never grow in place; the CRT copes by allocating anew. */
        if (n <= old)
            RET(4, p);
        RET(4, 0);
    }
    uint32_t q = grealloc(p, n);
    if (q && (flags & HEAP_ZERO_MEMORY) && n > old)
        memset(G2H(q + old), 0, n - old);
    RET(4, q);
}

void imp_KERNEL32_HeapSize(CPU *c) { RET(3, gmsize(ARG(2))); }

void imp_KERNEL32_VirtualAlloc(CPU *c) {
    RET(4, va_alloc(ARG(0), ARG(1), ARG(2), ARG(3)));
}

void imp_KERNEL32_VirtualFree(CPU *c) {
    RET(3, va_free(ARG(0), ARG(1), ARG(2)) ? 1 : 0);
}

void imp_KERNEL32_GlobalAlloc(CPU *c) {
    uint32_t p = (ARG(0) & 0x40) ? gcalloc(ARG(1)) : gmalloc(ARG(1));
    RET(2, p);
}

void imp_KERNEL32_GlobalFree(CPU *c) {
    gfree(ARG(0));
    RET(1, 0);
}

void imp_KERNEL32_GlobalMemoryStatus(CPU *c) {
    uint32_t p = ARG(0);
    uint32_t *s = G2H(p);
    s[0] = 32;
    s[1] = 20;                     /* memory load % */
    s[2] = 512u << 20;             /* total phys */
    s[3] = 384u << 20;             /* avail phys */
    s[4] = 1024u << 20;            /* total page file */
    s[5] = 900u << 20;
    s[6] = 2047u << 20;            /* total virtual */
    s[7] = 1500u << 20;
    RET(1, 0);
}

void imp_KERNEL32_IsBadReadPtr(CPU *c) { RET(2, ARG(0) < 0x10000 ? 1 : 0); }
void imp_KERNEL32_IsBadWritePtr(CPU *c) { RET(2, ARG(0) < 0x10000 ? 1 : 0); }
void imp_KERNEL32_IsBadCodePtr(CPU *c) { RET(1, ARG(0) < 0x10000 ? 1 : 0); }

/* ---------------------------------------------------------------- atomics */
void imp_KERNEL32_InterlockedIncrement(CPU *c) {
    RET(1, __atomic_add_fetch((uint32_t *)G2H(ARG(0)), 1, __ATOMIC_SEQ_CST));
}

void imp_KERNEL32_InterlockedDecrement(CPU *c) {
    RET(1, __atomic_sub_fetch((uint32_t *)G2H(ARG(0)), 1, __ATOMIC_SEQ_CST));
}

void imp_KERNEL32_InterlockedExchange(CPU *c) {
    RET(2, __atomic_exchange_n((uint32_t *)G2H(ARG(0)), ARG(1), __ATOMIC_SEQ_CST));
}

/* ------------------------------------------------------------------ time */
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Every guest-visible clock derives from this; loading a savestate moves
   it back to the saved time so the game sees no jump. */
static int64_t s_clock_offset;

uint64_t guest_time_ns(void) { return mono_ns() + (uint64_t)__atomic_load_n(&s_clock_offset, __ATOMIC_RELAXED); }

void guest_time_set(uint64_t ns) { __atomic_store_n(&s_clock_offset, (int64_t)(ns - mono_ns()), __ATOMIC_RELAXED); }

uint32_t win_tick_ms(void) {
    return (uint32_t)(guest_time_ns() / 1000000ull);
}

void imp_KERNEL32_GetTickCount(CPU *c) { RET(0, win_tick_ms()); }

void imp_KERNEL32_QueryPerformanceFrequency(CPU *c) {
    *(u64u *)G2H(ARG(0)) = 1000000000ull;
    RET(1, 1);
}

void imp_KERNEL32_QueryPerformanceCounter(CPU *c) {
    *(u64u *)G2H(ARG(0)) = guest_time_ns();
    RET(1, 1);
}

void imp_KERNEL32_Sleep(CPU *c) {
    /* A sleeping worker is at a safe point; after a load, start over. */
    while (snap_should_park() && snap_can_park())
        snap_park();
    uint32_t ms = ARG(0);
    c->esp += 8;
    if (ms == 0)
        sched_yield();
    else
        usleep(ms * 1000u);
}

/* ------------------------------------------------------------- strings */
void imp_KERNEL32_GetACP(CPU *c) { RET(0, 1252); }
void imp_KERNEL32_GetOEMCP(CPU *c) { RET(0, 437); }

void imp_KERNEL32_GetCPInfo(CPU *c) {
    uint8_t *p = G2H(ARG(1));
    memset(p, 0, 20);
    *(u32u *)p = 1;            /* MaxCharSize */
    p[4] = '?';
    RET(2, 1);
}

/* Code page 1252 <-> UTF-16 treated as Latin-1 (close enough for this game). */
void imp_KERNEL32_MultiByteToWideChar(CPU *c) {
    const uint8_t *src = G2H(ARG(2));
    int32_t n = (int32_t)ARG(3);
    uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0)
        n = (int32_t)strlen((const char *)src) + 1;
    if (cap == 0)
        RET(6, n);
    int32_t k = n < (int32_t)cap ? n : (int32_t)cap;
    for (int32_t i = 0; i < k; i++)
        *(u16u *)G2H(dst + 2 * i) = src[i];
    RET(6, k);
}

void imp_KERNEL32_WideCharToMultiByte(CPU *c) {
    uint32_t src = ARG(2);
    int32_t n = (int32_t)ARG(3);
    uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) {
        n = 0;
        while (*(u16u *)G2H(src + 2 * n))
            n++;
        n++;
    }
    if (cap == 0)
        RET(8, n);
    int32_t k = n < (int32_t)cap ? n : (int32_t)cap;
    for (int32_t i = 0; i < k; i++) {
        uint16_t ch = *(u16u *)G2H(src + 2 * i);
        ((uint8_t *)G2H(dst))[i] = ch < 256 ? (uint8_t)ch : '?';
    }
    RET(8, k);
}

static uint16_t ctype_of(unsigned ch) {
    uint16_t t = 0;
    if (ch < 128) {
        if (isupper(ch)) t |= 0x001;
        if (islower(ch)) t |= 0x002;
        if (isdigit(ch)) t |= 0x004;
        if (isspace(ch)) t |= 0x008;
        if (ispunct(ch)) t |= 0x010;
        if (iscntrl(ch)) t |= 0x020;
        if (ch == ' ' || ch == '\t') t |= 0x040;
        if (isxdigit(ch)) t |= 0x080;
        if (isalpha(ch)) t |= 0x100;
    } else if (ch >= 0xc0 && ch != 0xd7 && ch != 0xf7 && ch < 0x100) {
        t |= 0x100 | (ch < 0xe0 ? 0x001 : 0x002);
    }
    return t;
}

void imp_KERNEL32_GetStringTypeA(CPU *c) {
    const uint8_t *s = G2H(ARG(2));
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
        n = (int32_t)strlen((const char *)s) + 1;
    for (int32_t i = 0; i < n; i++)
        *(u16u *)G2H(ARG(4) + 2 * i) = ctype_of(s[i]);
    RET(5, 1);
}

void imp_KERNEL32_GetStringTypeW(CPU *c) {
    uint32_t s = ARG(1);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) {
        n = 0;
        while (*(u16u *)G2H(s + 2 * n))
            n++;
        n++;
    }
    for (int32_t i = 0; i < n; i++)
        *(u16u *)G2H(ARG(3) + 2 * i) = ctype_of(*(u16u *)G2H(s + 2 * i));
    RET(4, 1);
}

#define LCMAP_LOWERCASE 0x100
#define LCMAP_UPPERCASE 0x200

static unsigned lcmap_char(unsigned ch, uint32_t flags) {
    if (flags & LCMAP_LOWERCASE)
        return (ch < 128) ? (unsigned)tolower(ch) : (ch >= 0xc0 && ch < 0xdf && ch != 0xd7) ? ch + 32 : ch;
    if (flags & LCMAP_UPPERCASE)
        return (ch < 128) ? (unsigned)toupper(ch) : (ch >= 0xe0 && ch < 0xff && ch != 0xf7) ? ch - 32 : ch;
    return ch;
}

void imp_KERNEL32_LCMapStringA(CPU *c) {
    uint32_t flags = ARG(1);
    const uint8_t *s = G2H(ARG(2));
    int32_t n = (int32_t)ARG(3);
    uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0)
        n = (int32_t)strlen((const char *)s) + 1;
    if (!cap)
        RET(6, n);
    int32_t k = n < (int32_t)cap ? n : (int32_t)cap;
    for (int32_t i = 0; i < k; i++)
        ((uint8_t *)G2H(dst))[i] = (uint8_t)lcmap_char(s[i], flags);
    RET(6, k);
}

void imp_KERNEL32_LCMapStringW(CPU *c) {
    uint32_t flags = ARG(1), s = ARG(2);
    int32_t n = (int32_t)ARG(3);
    uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) {
        n = 0;
        while (*(u16u *)G2H(s + 2 * n))
            n++;
        n++;
    }
    if (!cap)
        RET(6, n);
    int32_t k = n < (int32_t)cap ? n : (int32_t)cap;
    for (int32_t i = 0; i < k; i++)
        *(u16u *)G2H(dst + 2 * i) = (uint16_t)lcmap_char(*(u16u *)G2H(s + 2 * i), flags);
    RET(6, k);
}

/* ------------------------------------------------------------------ files */
#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5

void imp_KERNEL32_CreateFileA(CPU *c) {
    const char *name = ARGS(0);
    uint32_t access = ARG(1), disp = ARG(4);
    char host[1200];
    if (!path_to_host(name, host, sizeof host)) {
        set_last_error(ERROR_PATH_NOT_FOUND);
        RET(7, INVALID_HANDLE_VALUE);
    }
    int fl = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case CREATE_NEW: fl |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: fl |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: fl |= O_CREAT; break;
    case TRUNCATE_EXISTING: fl |= O_TRUNC; break;
    default: break;
    }
    struct stat st;
    bool existed = stat(host, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        set_last_error(ERROR_ACCESS_DENIED);
        RET(7, INVALID_HANDLE_VALUE);
    }
    int fd = hd_open(host, fl);
    if (fd < 0) {
        LOGD("CreateFileA(%s) -> %s: %s", name, host, strerror(errno));
        set_last_error(errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
        RET(7, INVALID_HANDLE_VALUE);
    }
    LOGD("CreateFileA(%s, %08x, %u) -> fd %d", name, access, disp, fd);
    uint32_t h = handle_new_file(fd, fl & (O_ACCMODE | O_APPEND), name);
    set_last_error((existed && (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS)) ? ERROR_ALREADY_EXISTS : 0);
    RET(7, h);
}

void imp_KERNEL32_ReadFile(CPU *c) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pread_out = ARG(3), ov = ARG(4);
    int fd = handle_fd(h);
    if (fd < 0) {
        set_last_error(ERROR_INVALID_HANDLE);
        RET(5, 0);
    }
    os_commit(G2H(buf), n);
    ssize_t r;
    if (ov) {
        /* Overlapped read: performed synchronously at the given offset. */
        uint64_t off = *(u32u *)G2H(ov + 8) | ((uint64_t)*(u32u *)G2H(ov + 12) << 32);
        r = pread(fd, G2H(buf), n, (off_t)off);
        if (r >= 0 && (uint32_t)r < n)
            r += (ssize_t)hd_fill(fd, G2H(buf + (uint32_t)r), n - (uint32_t)r, off + (uint64_t)r);
        *(u32u *)G2H(ov) = 0;                       /* Internal: status */
        *(u32u *)G2H(ov + 4) = r < 0 ? 0 : (uint32_t)r;
        if (*(u32u *)G2H(ov + 16))
            handle_event_set(*(u32u *)G2H(ov + 16), true);
    } else {
        r = read(fd, G2H(buf), n);
        if (r >= 0 && (uint32_t)r < n) {
            off_t pos = lseek(fd, 0, SEEK_CUR);
            size_t more = hd_fill(fd, G2H(buf + (uint32_t)r), n - (uint32_t)r, (uint64_t)pos);
            if (more) {
                lseek(fd, (off_t)more, SEEK_CUR);
                r += (ssize_t)more;
            }
        }
    }
    if (pread_out)
        *(u32u *)G2H(pread_out) = r < 0 ? 0 : (uint32_t)r;
    RET(5, r < 0 ? 0 : 1);
}

void imp_KERNEL32_GetOverlappedResult(CPU *c) {
    uint32_t ov = ARG(1);
    *(u32u *)G2H(ARG(2)) = *(u32u *)G2H(ov + 4);
    RET(4, 1);
}

void imp_KERNEL32_WriteFile(CPU *c) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (h == 0x14 || h == 0x18) {
        fwrite(G2H(buf), 1, n, stderr);
        if (pw)
            *(u32u *)G2H(pw) = n;
        RET(5, 1);
    }
    int fd = handle_fd(h);
    if (fd < 0) {
        set_last_error(ERROR_INVALID_HANDLE);
        RET(5, 0);
    }
    os_commit(G2H(buf), n);
    ssize_t r = write(fd, G2H(buf), n);
    if (pw)
        *(u32u *)G2H(pw) = r < 0 ? 0 : (uint32_t)r;
    RET(5, r < 0 ? 0 : 1);
}

void imp_KERNEL32_SetFilePointer(CPU *c) {
    int fd = handle_fd(ARG(0));
    int64_t dist = (int32_t)ARG(1);
    if (ARG(2))
        dist = (int64_t)(((uint64_t)*(u32u *)G2H(ARG(2)) << 32) | ARG(1));
    int whence = ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END;
    off_t r = fd < 0 ? -1 : lseek(fd, (off_t)dist, whence);
    if (r < 0) {
        set_last_error(ERROR_INVALID_HANDLE);
        RET(4, 0xffffffffu);
    }
    if (ARG(2))
        *(u32u *)G2H(ARG(2)) = (uint32_t)((uint64_t)r >> 32);
    set_last_error(0);
    RET(4, (uint32_t)r);
}

void imp_KERNEL32_GetFileSize(CPU *c) {
    int fd = handle_fd(ARG(0));
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0)
        RET(2, 0xffffffffu);
    uint64_t hd;
    if (hd_size(fd, &hd))
        st.st_size = (off_t)hd;
    if (ARG(1))
        *(u32u *)G2H(ARG(1)) = (uint32_t)((uint64_t)st.st_size >> 32);
    RET(2, (uint32_t)st.st_size);
}

void imp_KERNEL32_FlushFileBuffers(CPU *c) { RET(1, 1); }

void imp_KERNEL32_CloseHandle(CPU *c) { RET(1, handle_close(ARG(0)) ? 1 : 0); }

void imp_KERNEL32_CreateDirectoryA(CPU *c) {
    char host[1200];
    path_to_host(ARGS(0), host, sizeof host);
    if (mkdir(host, 0755) < 0) {
        set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_PATH_NOT_FOUND);
        RET(2, 0);
    }
    RET(2, 1);
}

void imp_KERNEL32_SetCurrentDirectoryA(CPU *c) {
    LOGD("SetCurrentDirectoryA(%s)", ARGS(0));
    path_set_cwd(ARGS(0));
    RET(1, 1);
}

void imp_KERNEL32_GetDriveTypeA(CPU *c) {
    const char *root = ARGS(0);
    char d = root ? (char)toupper((unsigned char)root[0]) : 'C';
    uint32_t t = d == 'C' ? 3 : d == 'D' ? 5 : 1;   /* FIXED / CDROM / NO_ROOT_DIR */
    LOGD("GetDriveTypeA(%s) -> %u", root ? root : "(null)", t);
    RET(1, t);
}

/* WIN32_FIND_DATAA */
static void fill_find_data(uint32_t p, const char *dir, const char *name) {
    uint8_t *f = G2H(p);
    memset(f, 0, 320);
    char full[1400];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    struct stat st;
    if (stat(full, &st) == 0) {
        *(u32u *)(f + 0) = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
        *(u32u *)(f + 28) = (uint32_t)((uint64_t)st.st_size >> 32);
        *(u32u *)(f + 32) = (uint32_t)st.st_size;
    }
    snprintf((char *)f + 44, 260, "%s", name);
}

void imp_KERNEL32_FindFirstFileA(CPU *c) {
    const char *pattern = ARGS(0);
    char host[1200];
    path_to_host(pattern, host, sizeof host);
    char *slash = strrchr(host, '/');
    if (!slash) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        RET(2, INVALID_HANDLE_VALUE);
    }
    *slash = 0;
    const char *pat = slash + 1;
    DIR *d = opendir(host);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!fnmatch(pat, e->d_name, FNM_CASEFOLD)) {
                fill_find_data(ARG(1), host, e->d_name);
                closedir(d);
                LOGD("FindFirstFileA(%s) -> %s", pattern, e->d_name);
                RET(2, handle_new(H_FIND));
            }
        }
        closedir(d);
    }
    LOGD("FindFirstFileA(%s) -> not found", pattern);
    set_last_error(ERROR_FILE_NOT_FOUND);
    RET(2, INVALID_HANDLE_VALUE);
}

void imp_KERNEL32_FindClose(CPU *c) {
    handle_close(ARG(0));
    RET(1, 1);
}

/* -------------------------------------------------------------- savestates
 * K32_: guest clock, process strings and the current directory. */
static void k32_save(SnapBuf *b) {
    uint64_t now = guest_time_ns();
    SB_PUT(b, now);
    sb_u32(b, s_cmdline);
    sb_u32(b, s_env_a);
    sb_u32(b, s_env_w);
    char cwd[260];
    path_get_cwd(cwd, sizeof cwd);
    sb_str(b, cwd);
}

static bool k32_load(SnapBuf *b) {
    uint64_t now;
    SB_GET(b, now);
    guest_time_set(now);
    s_cmdline = sb_get_u32(b);
    s_env_a = sb_get_u32(b);
    s_env_w = sb_get_u32(b);
    char *cwd = sb_get_str(b);
    if (cwd)
        path_set_cwd(cwd);
    free(cwd);
    return !b->err;
}

static const SnapSection s_k32_section = {
    SNAP_TAG('K', '3', '2', '_'), SNAP_ORDER_KERNEL, k32_save, NULL, NULL, k32_load,
};

__attribute__((constructor)) static void k32_snap_register(void) { snap_section(&s_k32_section); }
