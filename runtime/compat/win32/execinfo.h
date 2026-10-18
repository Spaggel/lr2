/* Windows stand-in for glibc's <execinfo.h> (runtime/src/os/os_win32.c). */
#ifndef LR2_COMPAT_EXECINFO_H
#define LR2_COMPAT_EXECINFO_H

int backtrace(void **buffer, int size);
void backtrace_symbols_fd(void *const *buffer, int size, int fd);

#endif
