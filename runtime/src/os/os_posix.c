/* Host OS services: Linux and macOS. */
#ifndef _WIN32
#include "os.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void os_init(void) {}

void os_log_file(const char *path) { (void)path; }

void *os_reserve(size_t size, size_t guard) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    if (guard)
        mprotect(p, guard, PROT_NONE);
    return p;
}

void os_commit(void *p, size_t n) {
    (void)p;
    (void)n;
}

static void (*s_fault_fn)(const char *what, void *addr);

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char what[64];
    snprintf(what, sizeof what, "host signal %d (%s)", sig, strsignal(sig));
    s_fault_fn(what, si->si_addr);
    signal(sig, SIG_DFL);
    raise(sig);
}

void os_on_fault(void (*fn)(const char *what, void *addr)) {
    s_fault_fn = fn;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

static void id_from(const struct stat *st, OsFileId *id) {
    id->dev = (uint64_t)st->st_dev;
    id->ino = (uint64_t)st->st_ino;
}

bool os_fd_id(int fd, OsFileId *id) {
    struct stat st;
    if (fstat(fd, &st) < 0)
        return false;
    id_from(&st, id);
    return true;
}

bool os_path_id(const char *path, OsFileId *id) {
    struct stat st;
    if (stat(path, &st) < 0)
        return false;
    id_from(&st, id);
    return true;
}

int os_tmpfd(void) {
    FILE *t = tmpfile();
    if (!t)
        return -1;
    int fd = dup(fileno(t));
    fclose(t);
    return fd;
}

int os_replace(const char *from, const char *to) { return rename(from, to); }
#endif
