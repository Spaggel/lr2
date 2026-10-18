/* MinGW's <sys/stat.h> with the POSIX two-argument mkdir(). */
#ifndef LR2_COMPAT_SYS_STAT_H
#define LR2_COMPAT_SYS_STAT_H

#include_next <sys/stat.h>
#include <io.h>
#include <direct.h>

#define mkdir(path, mode) _mkdir(path)

#endif
