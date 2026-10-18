/* Windows stand-in for <fnmatch.h> (runtime/src/os/os_win32.c). */
#ifndef LR2_COMPAT_FNMATCH_H
#define LR2_COMPAT_FNMATCH_H

#define FNM_NOMATCH 1
#define FNM_NOESCAPE 0x01
#define FNM_CASEFOLD 0x10

int fnmatch(const char *pattern, const char *string, int flags);

#endif
