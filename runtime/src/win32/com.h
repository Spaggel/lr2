/* Minimal COM: guest-visible objects whose vtables point at host thunks.
 *
 * A guest object is { vtbl, id } in guest memory; id indexes a host-side
 * table holding the ComObj header that starts every host object.  Interface
 * methods are ordinary imports: ARG(0) is `this`, RET(n, hr) pops this+args.
 */
#ifndef COM_H
#define COM_H

#include "../rt.h"
#include "../savestate.h"

typedef struct {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} Guid;

#define S_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_FAIL 0x80004005u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000eu
#define CLASS_E_CLASSNOTAVAILABLE 0x80040111u
#define REGDB_E_CLASSNOTREG 0x80040154u

typedef struct ComObj ComObj;
struct ComObj {
    uint32_t guest;           /* guest address of the object */
    int refs;
    const char *type;
    void (*destroy)(ComObj *o);
};

void guid_str(const Guid *g, char out[40]);
bool guid_eq(const Guid *a, const Guid *b);
bool guid_parse(const char *s, Guid *out);   /* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */

/* Build (once per name) a guest vtable from host methods.  Every vtable
   must also be declared with COM_VTABLE so savestates can map thunk names
   ("IFoo[3]") back to host functions before the vtable is first used. */
uint32_t com_vtbl(const char *name, const GuestFn *fns, int n);
void com_vtbl_define(const char *name, const GuestFn *fns, int n);
GuestFn com_vtbl_method(const char *thunk_name);
#define COM_VTABLE(name, arr)                                                     \
    __attribute__((constructor)) static void vtdef_##arr(void) {                 \
        com_vtbl_define(name, arr, (int)(sizeof arr / sizeof *arr));              \
    }
/* Create a guest object for host object o (refs=1).  extra = additional
   guest bytes after the {vtbl,id} header (some interfaces expose data). */
uint32_t com_wrap(ComObj *o, uint32_t vtbl, uint32_t extra);
/* Host object behind a guest pointer (NULL if not ours). */
ComObj *com_get(uint32_t guest);
#define COM_THIS(T) ((T *)com_get(ARG(0)))

/* Generic IUnknown::AddRef/Release for objects created with com_wrap. */
void com_AddRef(CPU *c);
void com_Release(CPU *c);
uint32_t com_release_obj(ComObj *o);
void com_addref_obj(ComObj *o);

/* CoCreateInstance support: class factories registered by subsystems. */
typedef uint32_t (*ComFactory)(CPU *c, const Guid *iid, uint32_t *out);
void com_register_class(const char *clsid, const char *name, ComFactory f);
uint32_t com_create(CPU *c, const Guid *clsid, const Guid *iid, uint32_t *out);

/* Savestate support: how to write and recreate the host side of one COM
   type.  load() returns a new object (guest/refs/type are filled in by the
   caller); pointers to other objects are read as guest addresses and fixed
   up in link().  drop() frees host resources only: guest memory is about to
   be replaced, so it must not call gfree or release other objects. */

typedef struct {
    const char *type;
    void (*save)(ComObj *o, SnapBuf *b);
    ComObj *(*load)(SnapBuf *b);
    void (*link)(ComObj *o);
    void (*drop)(ComObj *o);
} ComSnapClass;
void com_snap_class(const ComSnapClass *k);
/* Visit live objects (all if type is NULL). */
void com_foreach(const char *type, void (*fn)(ComObj *o, void *ud), void *ud);
/* guest address -> pointer, for link() */
#define COM_LINK(field, T) ((field) = (T *)com_get((uint32_t)(uintptr_t)(field)))

/* Stub method returning E_NOTIMPL that logs which slot was hit. */
#define COM_STUB(iface, method, nargs)                                            \
    static void iface##_##method(CPU *c) {                                        \
        LOGW(#iface "::" #method " not implemented");                             \
        RET((nargs) + 1, E_NOTIMPL);                                              \
    }

#endif
