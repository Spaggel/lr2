#include "com.h"
#include "../savestate.h"
#include <pthread.h>

#define MAX_OBJS 65536

static ComObj *s_objs[MAX_OBJS];
static uint32_t s_next = 1;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void guid_str(const Guid *g, char out[40]) {
    snprintf(out, 40, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->d1, g->d2, g->d3,
             g->d4[0], g->d4[1], g->d4[2], g->d4[3], g->d4[4], g->d4[5], g->d4[6], g->d4[7]);
}

bool guid_eq(const Guid *a, const Guid *b) { return !memcmp(a, b, sizeof *a); }

bool guid_parse(const char *s, Guid *g) {
    unsigned d1, d2, d3, b[8];
    if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2],
               &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    g->d1 = d1;
    g->d2 = (uint16_t)d2;
    g->d3 = (uint16_t)d3;
    for (int i = 0; i < 8; i++)
        g->d4[i] = (uint8_t)b[i];
    return true;
}

/* Vtable definitions (registered at startup by COM_VTABLE) and the guest
   copies made from them on first use. */
typedef struct {
    const char *name;
    const GuestFn *fns;
    int n;
} VtblDef;
static VtblDef s_defs[64];
static int s_ndefs;

typedef struct {
    const char *name;
    uint32_t addr;
} Vtbl;
static Vtbl s_vtbls[128];
static int s_nvtbls;

void com_vtbl_define(const char *name, const GuestFn *fns, int n) {
    for (int i = 0; i < s_ndefs; i++)
        if (!strcmp(s_defs[i].name, name))
            return;
    if (s_ndefs == 64)
        rt_fatal("too many COM vtables");
    s_defs[s_ndefs++] = (VtblDef){name, fns, n};
}

static const VtblDef *def_find(const char *name, size_t len) {
    for (int i = 0; i < s_ndefs; i++)
        if (strlen(s_defs[i].name) == len && !memcmp(s_defs[i].name, name, len))
            return &s_defs[i];
    return NULL;
}

GuestFn com_vtbl_method(const char *thunk_name) {
    const char *br = strchr(thunk_name, '[');
    if (!br)
        return NULL;
    const VtblDef *d = def_find(thunk_name, (size_t)(br - thunk_name));
    int i = atoi(br + 1);
    return d && i >= 0 && i < d->n ? d->fns[i] : NULL;
}

uint32_t com_vtbl(const char *name, const GuestFn *fns, int n) {
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_nvtbls; i++) {
        if (!strcmp(s_vtbls[i].name, name)) {
            pthread_mutex_unlock(&s_lock);
            return s_vtbls[i].addr;
        }
    }
    if (!def_find(name, strlen(name)))
        rt_fatal("COM vtable %s used without COM_VTABLE", name);
    uint32_t v = gcalloc(4u * (uint32_t)n);
    char tn[128];
    for (int i = 0; i < n; i++) {
        snprintf(tn, sizeof tn, "%s[%d]", name, i);
        *(u32u *)G2H(v + 4u * (uint32_t)i) = fns[i] ? thunk_register(fns[i], strdup(tn)) : 0;
    }
    s_vtbls[s_nvtbls++] = (Vtbl){name, v};
    pthread_mutex_unlock(&s_lock);
    return v;
}

uint32_t com_wrap(ComObj *o, uint32_t vtbl, uint32_t extra) {
    pthread_mutex_lock(&s_lock);
    uint32_t id = 0;
    for (uint32_t k = 0; k < MAX_OBJS - 1; k++) {
        uint32_t i = (s_next + k) % (MAX_OBJS - 1) + 1;
        if (!s_objs[i]) {
            id = i;
            break;
        }
    }
    if (!id)
        rt_fatal("out of COM objects");
    s_next = id + 1;
    s_objs[id] = o;
    pthread_mutex_unlock(&s_lock);
    uint32_t g = gcalloc(8 + extra);
    *(u32u *)G2H(g) = vtbl;
    *(u32u *)G2H(g + 4) = id;
    o->guest = g;
    o->refs = 1;
    return g;
}

ComObj *com_get(uint32_t g) {
    if (g < GUEST_HEAP || g >= GUEST_HEAP_END)
        return NULL;
    uint32_t id = *(u32u *)G2H(g + 4);
    if (!id || id >= MAX_OBJS)
        return NULL;
    ComObj *o = s_objs[id];
    return o && o->guest == g ? o : NULL;
}

void com_addref_obj(ComObj *o) { __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST); }

uint32_t com_release_obj(ComObj *o) {
    int r = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
    if (r == 0) {
        uint32_t g = o->guest;
        uint32_t id = *(u32u *)G2H(g + 4);
        pthread_mutex_lock(&s_lock);
        s_objs[id] = NULL;
        pthread_mutex_unlock(&s_lock);
        *(u32u *)G2H(g + 4) = 0;
        if (o->destroy)
            o->destroy(o);
        gfree(g);
    }
    return r < 0 ? 0 : (uint32_t)r;
}

void com_AddRef(CPU *c) {
    ComObj *o = com_get(ARG(0));
    if (!o)
        RET(1, 1);
    com_addref_obj(o);
    RET(1, o->refs);
}

void com_Release(CPU *c) {
    ComObj *o = com_get(ARG(0));
    RET(1, o ? com_release_obj(o) : 0);
}

typedef struct {
    Guid clsid;
    const char *name;
    ComFactory f;
} Class;
static Class s_classes[32];
static int s_nclasses;

void com_register_class(const char *clsid, const char *name, ComFactory f) {
    Class *k = &s_classes[s_nclasses++];
    guid_parse(clsid, &k->clsid);
    k->name = name;
    k->f = f;
}

uint32_t com_create(CPU *c, const Guid *clsid, const Guid *iid, uint32_t *out) {
    for (int i = 0; i < s_nclasses; i++)
        if (guid_eq(&s_classes[i].clsid, clsid))
            return s_classes[i].f(c, iid, out);
    return REGDB_E_CLASSNOTREG;
}

/* -------------------------------------------------------------- savestates
 * COM_: guest vtable addresses, then every live object as
 *     id, guest, refs, type, payload (written by the type's ComSnapClass).
 * Objects reference each other by guest address in the payload; link()
 * turns those back into pointers once every object exists. */
static ComSnapClass s_snap_classes[32];
static int s_nsnap_classes;

void com_snap_class(const ComSnapClass *k) {
    if (s_nsnap_classes < 32)
        s_snap_classes[s_nsnap_classes++] = *k;
}

static const ComSnapClass *snap_class(const char *type) {
    for (int i = 0; i < s_nsnap_classes; i++)
        if (!strcmp(s_snap_classes[i].type, type))
            return &s_snap_classes[i];
    return NULL;
}

void com_foreach(const char *type, void (*fn)(ComObj *o, void *ud), void *ud) {
    for (uint32_t i = 1; i < MAX_OBJS; i++)
        if (s_objs[i] && (!type || !strcmp(s_objs[i]->type, type)))
            fn(s_objs[i], ud);
}

static void com_save(SnapBuf *b) {
    sb_u32(b, (uint32_t)s_nvtbls);
    for (int i = 0; i < s_nvtbls; i++) {
        sb_str(b, s_vtbls[i].name);
        sb_u32(b, s_vtbls[i].addr);
    }
    sb_u32(b, s_next);
    uint32_t n = 0;
    for (uint32_t i = 1; i < MAX_OBJS; i++)
        n += s_objs[i] != NULL;
    sb_u32(b, n);
    for (uint32_t i = 1; i < MAX_OBJS; i++) {
        ComObj *o = s_objs[i];
        if (!o)
            continue;
        const ComSnapClass *k = snap_class(o->type);
        if (!k)
            rt_fatal("savestate: COM type %s has no serializer", o->type);
        sb_u32(b, i);
        sb_u32(b, o->guest);
        sb_u32(b, (uint32_t)o->refs);
        sb_str(b, o->type);
        SnapBuf p = {0};
        k->save(o, &p);
        sb_u32(b, (uint32_t)p.len);
        sb_put(b, p.data, p.len);
        sb_free(&p);
    }
}

static bool com_check(SnapBuf *b) {
    uint32_t nv = sb_get_u32(b);
    bool ok = nv <= 128;
    for (uint32_t i = 0; ok && i < nv && !b->err; i++) {
        char *name = sb_get_str(b);
        sb_get_u32(b);
        if (!name || !def_find(name, strlen(name))) {
            LOGW("savestate: unknown COM vtable %s", name ? name : "(null)");
            ok = false;
        }
        free(name);
    }
    sb_get_u32(b);
    uint32_t n = sb_get_u32(b);
    for (uint32_t i = 0; ok && i < n && !b->err; i++) {
        uint32_t id = sb_get_u32(b);
        sb_get_u32(b);
        sb_get_u32(b);
        char *type = sb_get_str(b);
        uint32_t len = sb_get_u32(b);
        if (!id || id >= MAX_OBJS || !type || !snap_class(type) || b->pos + len > b->len) {
            LOGW("savestate: bad COM object %u (%s)", id, type ? type : "?");
            ok = false;
        }
        b->pos += len;
        free(type);
    }
    return ok && !b->err;
}

static void com_drop(void) {
    for (uint32_t i = 1; i < MAX_OBJS; i++) {
        ComObj *o = s_objs[i];
        if (!o)
            continue;
        s_objs[i] = NULL;
        const ComSnapClass *k = snap_class(o->type);
        if (k && k->drop)
            k->drop(o);
        else
            free(o);
    }
    s_nvtbls = 0;
}

static bool com_load(SnapBuf *b) {
    uint32_t nv = sb_get_u32(b);
    for (uint32_t i = 0; i < nv && !b->err; i++) {
        char *name = sb_get_str(b);
        uint32_t addr = sb_get_u32(b);
        const VtblDef *d = name ? def_find(name, strlen(name)) : NULL;
        free(name);
        if (!d)
            return false;
        s_vtbls[s_nvtbls++] = (Vtbl){d->name, addr};
    }
    s_next = sb_get_u32(b);
    uint32_t n = sb_get_u32(b);
    for (uint32_t i = 0; i < n && !b->err; i++) {
        uint32_t id = sb_get_u32(b);
        uint32_t guest = sb_get_u32(b);
        int refs = (int)sb_get_u32(b);
        char *type = sb_get_str(b);
        uint32_t len = sb_get_u32(b);
        const ComSnapClass *k = type ? snap_class(type) : NULL;
        free(type);
        if (!k || b->pos + len > b->len)
            return false;
        SnapBuf p = {.data = b->data + b->pos, .len = len, .cap = len};
        b->pos += len;
        ComObj *o = k->load(&p);
        if (!o || p.err)
            return false;
        o->guest = guest;
        o->refs = refs;
        o->type = k->type;
        s_objs[id] = o;
    }
    for (uint32_t i = 1; i < MAX_OBJS; i++) {
        if (!s_objs[i])
            continue;
        const ComSnapClass *k = snap_class(s_objs[i]->type);
        if (k->link)
            k->link(s_objs[i]);
    }
    return !b->err;
}

static const SnapSection s_com_section = {
    SNAP_TAG('C', 'O', 'M', '_'), SNAP_ORDER_COM, com_save, com_check, com_drop, com_load,
};

__attribute__((constructor)) static void com_snap_register(void) { snap_section(&s_com_section); }
