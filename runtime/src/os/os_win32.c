/* Host OS services: Windows (MinGW-w64 / llvm-mingw, x64 and ARM64).
 *
 * Also implements the POSIX pieces the runtime uses that MinGW lacks; their
 * headers are in runtime/compat/win32.  Do not include rt.h here: its Win32
 * definitions clash with <windows.h>.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include "os.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void os_init(void) {
    /* open() and fopen() without a mode letter must not translate CR/LF. */
    _set_fmode(_O_BINARY);
    /* Sleep(), usleep() and condition-variable timeouts default to 15.6 ms
       ticks, far too coarse for the game's 60 Hz timers. */
    timeBeginPeriod(1);
    /* No "the program stopped working" dialog for errors we report. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
}

void os_log_file(const char *path) {
    /* A GUI-subsystem program started from Explorer has no stderr; one
       started from a console or with redirected output inherits it. */
    int fd = _fileno(stderr);
    intptr_t h = fd >= 0 ? _get_osfhandle(fd) : -1;
    if (h != -1 && h != -2 && h != 0)
        return;
    if (freopen(path, "w", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
}

/* ------------------------------------------------------- guest memory */
/* The guest's 4 GiB space is reserved up front and committed in blocks as
   it is touched, so the process only pays for what the game uses. */
static uint8_t *s_res;
static size_t s_res_size, s_res_guard;

#define COMMIT_BLOCK (1u << 20)

static bool commit(uintptr_t lo, uintptr_t hi) {
    uintptr_t b0 = (uintptr_t)s_res + s_res_guard, b1 = (uintptr_t)s_res + s_res_size;
    if (lo < b0)
        lo = b0;
    if (hi > b1)
        hi = b1;
    if (lo >= hi)
        return false;
    return VirtualAlloc((void *)lo, hi - lo, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static LONG CALLBACK commit_on_fault(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 ||
        er->ExceptionInformation[0] == 8 /* execute */)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = er->ExceptionInformation[1];
    if (a < (uintptr_t)s_res + s_res_guard || a >= (uintptr_t)s_res + s_res_size)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t lo = a & ~(uintptr_t)(COMMIT_BLOCK - 1);
    return commit(lo, lo + COMMIT_BLOCK) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void *os_reserve(size_t size, size_t guard) {
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p)
        return NULL;
    s_res = p;
    s_res_size = size;
    s_res_guard = guard;
    AddVectoredExceptionHandler(1, commit_on_fault);
    return p;
}

void os_commit(void *p, size_t n) {
    if (s_res && n)
        commit((uintptr_t)p, (uintptr_t)p + n);
}

/* ------------------------------------------------------------- faults */
static void (*s_fault_fn)(const char *what, void *addr);

static LONG WINAPI fault_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const char *name;
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: name = "access violation"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: name = "illegal instruction"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: name = "integer divide by zero"; break;
    case EXCEPTION_STACK_OVERFLOW: name = "stack overflow"; break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: name = "misaligned access"; break;
    default: name = "exception"; break;
    }
    char what[96];
    snprintf(what, sizeof what, "host %s (%08lx) at pc %p", name, (unsigned long)er->ExceptionCode,
             er->ExceptionAddress);
    void *addr = er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2
                     ? (void *)er->ExceptionInformation[1]
                     : NULL;
    if (s_fault_fn)
        s_fault_fn(what, addr);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void os_on_fault(void (*fn)(const char *what, void *addr)) {
    s_fault_fn = fn;
    SetUnhandledExceptionFilter(fault_filter);
}

/* -------------------------------------------------------------- files */
static bool handle_id(HANDLE h, OsFileId *id) {
    BY_HANDLE_FILE_INFORMATION fi;
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &fi))
        return false;
    id->dev = fi.dwVolumeSerialNumber;
    id->ino = (uint64_t)fi.nFileIndexHigh << 32 | fi.nFileIndexLow;
    return true;
}

bool os_fd_id(int fd, OsFileId *id) { return handle_id((HANDLE)_get_osfhandle(fd), id); }

bool os_path_id(const char *path, OsFileId *id) {
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    bool ok = handle_id(h, id);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    return ok;
}

int os_tmpfd(void) {
    /* tmpfile() in the UCRT creates its file in the root of the current
       drive, which normal users cannot write to. */
    char dir[MAX_PATH], name[MAX_PATH];
    DWORD n = GetTempPathA(sizeof dir, dir);
    if (!n || n >= sizeof dir || !GetTempFileNameA(dir, "lr2", 0, name))
        return -1;
    HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DeleteFileA(name);
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
    if (fd < 0)
        CloseHandle(h);
    return fd;
}

int os_replace(const char *from, const char *to) {
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING))
        return 0;
    errno = EACCES;
    return -1;
}

/* Positional I/O.  ReadFile with an offset moves the file pointer of a
   synchronous handle, so it is put back to keep POSIX semantics. */
static ssize_t pio(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (n > 0x7fffffff)
        n = 0x7fffffff;
    long long pos = _lseeki64(fd, 0, SEEK_CUR);
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)(uint64_t)off;
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD done = 0;
    BOOL ok = write ? WriteFile(h, buf, (DWORD)n, &done, &ov) : ReadFile(h, buf, (DWORD)n, &done, &ov);
    DWORD err = ok ? 0 : GetLastError();
    if (pos >= 0)
        _lseeki64(fd, pos, SEEK_SET);
    if (!ok) {
        if (err == ERROR_HANDLE_EOF)
            return 0;
        errno = err == ERROR_NOACCESS ? EFAULT : EIO;
        return -1;
    }
    return (ssize_t)done;
}

ssize_t pread(int fd, void *buf, size_t n, off_t off) { return pio(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return pio(fd, (void *)buf, n, off, true); }

/* --------------------------------------------------------- backtraces */
__attribute__((noinline)) int backtrace(void **buffer, int size) {
    /* Skip this function, like glibc's backtrace() whose first entry is
       its caller. */
    return size > 0 ? RtlCaptureStackBackTrace(1, (DWORD)size, buffer, NULL) : 0;
}

/* Exported functions of this executable, sorted by address. */
typedef struct {
    uintptr_t addr;
    const char *name;
} Sym;
static Sym *s_syms;
static size_t s_nsyms;
static HMODULE s_self;
static INIT_ONCE s_syms_once = INIT_ONCE_STATIC_INIT;

static int sym_cmp(const void *a, const void *b) {
    uintptr_t x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return x < y ? -1 : x > y;
}

static BOOL CALLBACK load_syms(INIT_ONCE *once, void *param, void **ctx) {
    (void)once, (void)param, (void)ctx;
    s_self = GetModuleHandleA(NULL);
    uint8_t *base = (uint8_t *)s_self;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress || !dd.Size)
        return TRUE;
    IMAGE_EXPORT_DIRECTORY *ex = (IMAGE_EXPORT_DIRECTORY *)(base + dd.VirtualAddress);
    DWORD *funcs = (DWORD *)(base + ex->AddressOfFunctions);
    DWORD *names = (DWORD *)(base + ex->AddressOfNames);
    WORD *ords = (WORD *)(base + ex->AddressOfNameOrdinals);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int nsec = nt->FileHeader.NumberOfSections;
    s_syms = malloc((ex->NumberOfNames + 1) * sizeof *s_syms);
    for (DWORD i = 0; s_syms && i < ex->NumberOfNames; i++) {
        if (ords[i] >= ex->NumberOfFunctions)
            continue;
        DWORD rva = funcs[ords[i]];
        /* Code only: data exports would otherwise swallow addresses. */
        bool code = false;
        for (int k = 0; k < nsec && !code; k++)
            code = (sec[k].Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= sec[k].VirtualAddress &&
                   rva < sec[k].VirtualAddress + sec[k].Misc.VirtualSize;
        if (code)
            s_syms[s_nsyms++] = (Sym){(uintptr_t)base + rva, (const char *)(base + names[i])};
    }
    if (s_nsyms)
        qsort(s_syms, s_nsyms, sizeof *s_syms, sym_cmp);
    return TRUE;
}

int dladdr(const void *addr, Dl_info *info) {
    InitOnceExecuteOnce(&s_syms_once, load_syms, NULL, NULL);
    HMODULE mod;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (const char *)addr, &mod))
        return 0;
    info->dli_fname = "";
    info->dli_fbase = mod;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    if (mod == s_self && s_nsyms) {
        size_t lo = 0, hi = s_nsyms;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (s_syms[mid].addr <= (uintptr_t)addr)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo) {
            info->dli_sname = s_syms[lo - 1].name;
            info->dli_saddr = (void *)s_syms[lo - 1].addr;
        }
    }
    return 1;
}

void backtrace_symbols_fd(void *const *buffer, int size, int fd) {
    for (int i = 0; i < size; i++) {
        Dl_info di;
        char line[256];
        int n;
        if (dladdr(buffer[i], &di) && di.dli_sname)
            n = snprintf(line, sizeof line, "%s+0x%llx [%p]\n", di.dli_sname,
                         (unsigned long long)((const char *)buffer[i] - (const char *)di.dli_saddr), buffer[i]);
        else
            n = snprintf(line, sizeof line, "[%p]\n", buffer[i]);
        if (n > 0)
            _write(fd, line, (unsigned)(n < (int)sizeof line ? n : (int)sizeof line - 1));
    }
}

/* ------------------------------------------------------------ fnmatch */
static int fold(int c, int flags) { return (flags & FNM_CASEFOLD) ? tolower(c) : c; }

int fnmatch(const char *p, const char *s, int flags) {
    for (;; p++, s++) {
        switch (*p) {
        case 0:
            return *s ? FNM_NOMATCH : 0;
        case '?':
            if (!*s)
                return FNM_NOMATCH;
            break;
        case '*':
            while (p[1] == '*')
                p++;
            if (!p[1])
                return 0;
            for (; *s; s++)
                if (!fnmatch(p + 1, s, flags))
                    return 0;
            return fnmatch(p + 1, s, flags);
        case '[': {
            if (!*s)
                return FNM_NOMATCH;
            const char *q = p + 1;
            bool neg = *q == '!' || *q == '^';
            if (neg)
                q++;
            bool hit = false;
            int c = fold((unsigned char)*s, flags);
            do {
                int lo = (unsigned char)*q, hi = lo;
                if (!lo)
                    return FNM_NOMATCH;   /* unterminated */
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    hi = (unsigned char)q[2];
                    q += 2;
                }
                if (c >= fold(lo, flags) && c <= fold(hi, flags))
                    hit = true;
                q++;
            } while (*q != ']');
            if (hit == neg)
                return FNM_NOMATCH;
            p = q;
            break;
        }
        case '\\':
            if (!(flags & FNM_NOESCAPE) && p[1])
                p++;
            /* fall through */
        default:
            if (fold((unsigned char)*p, flags) != fold((unsigned char)*s, flags))
                return FNM_NOMATCH;
            break;
        }
    }
}
#endif
