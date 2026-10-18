/* Windows stand-in for <dlfcn.h>: only dladdr(), which names functions of
   this executable from its export table (link with --export-all-symbols). */
#ifndef LR2_COMPAT_DLFCN_H
#define LR2_COMPAT_DLFCN_H

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

int dladdr(const void *addr, Dl_info *info);

#endif
