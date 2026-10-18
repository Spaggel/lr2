/* Host OS services that differ between POSIX systems and Windows.
 *
 * Implemented in os/os_posix.c and os/os_win32.c.  This header must not
 * depend on rt.h: the Windows implementation includes <windows.h>, whose
 * names clash with the runtime's own Win32 definitions.
 */
#ifndef OS_H
#define OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Process setup that has to happen before anything else (binary file
   mode, timer resolution). */
void os_init(void);

/* Send log output (stderr) to path when there is no console to show it,
   as for a Windows GUI program started from Explorer. */
void os_log_file(const char *path);

/* Reserve size bytes of zero-filled address space whose first guard bytes
   fault on access.  Pages are committed on first use. */
void *os_reserve(size_t size, size_t guard);

/* Make [p, p+n) of the reserved space usable before the OS kernel writes
   to it (a read() into a never-touched page fails on Windows instead of
   faulting the page in). */
void os_commit(void *p, size_t n);

/* Call fn on a fatal host fault (segfault, illegal instruction, ...) with
   a description and the faulting data address, if any.  The process dies
   afterwards. */
void os_on_fault(void (*fn)(const char *what, void *addr));

/* Identity of an open file or a path, to recognise the same file opened
   under different names. */
typedef struct {
    uint64_t dev, ino;
} OsFileId;
bool os_fd_id(int fd, OsFileId *id);
bool os_path_id(const char *path, OsFileId *id);

/* An anonymous read/write file, deleted when closed. */
int os_tmpfd(void);

/* rename() that replaces an existing destination. */
int os_replace(const char *from, const char *to);

#endif
