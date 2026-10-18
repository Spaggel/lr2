/* MinGW's <unistd.h> plus pread()/pwrite() (runtime/src/os/os_win32.c). */
#ifndef LR2_COMPAT_UNISTD_H
#define LR2_COMPAT_UNISTD_H

#include_next <unistd.h>
#include <sys/types.h>

ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);

#endif
