/* Maps the game image into guest memory, as the Windows loader would.  The
   recompiled code needs its data sections at runtime; the recompiler puts
   the image's bytes into build/gen (g_image), so no .exe is read. */
#include "rt.h"

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

void pe_load(void) {
    const uint8_t *buf = g_image;
    uint32_t pe = rd32(buf + 0x3c);
    if (memcmp(buf + pe, "PE\0\0", 4))
        rt_fatal("built-in game image is not a PE executable");
    uint16_t nsec = rd16(buf + pe + 6);
    uint16_t optsz = rd16(buf + pe + 20);
    const uint8_t *opt = buf + pe + 24;
    g_pe.base = rd32(opt + 28);
    g_pe.entry = g_pe.base + rd32(opt + 16);
    g_pe.size = rd32(opt + 56);
    uint32_t hdrsz = rd32(opt + 60);
    if (g_pe.base != GUEST_IMAGE)
        rt_fatal("unexpected image base %08x", g_pe.base);

    memcpy(g_mem + g_pe.base, buf, hdrsz);
    const uint8_t *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        char name[9] = {0};
        memcpy(name, sec, 8);
        uint32_t vsize = rd32(sec + 8), va = rd32(sec + 12);
        uint32_t rawsz = rd32(sec + 16), rawoff = rd32(sec + 20);
        uint32_t n = rawsz < vsize || !vsize ? rawsz : vsize;
        memcpy(g_mem + g_pe.base + va, buf + rawoff, n);
        if (!strcmp(name, ".text")) {
            g_pe.text_start = g_pe.base + va;
            g_pe.text_end = g_pe.base + va + (vsize > rawsz ? vsize : rawsz);
        }
        LOGD("section %-8s %08x..%08x", name, g_pe.base + va, g_pe.base + va + vsize);
    }

    /* Point every IAT slot at a host thunk. */
    for (uint32_t i = 0; i < g_import_count; i++) {
        const ImportEntry *ie = &g_import_table[i];
        char *nm = malloc(strlen(ie->dll) + strlen(ie->name) + 2);
        sprintf(nm, "%s!%s", ie->dll, ie->name);
        *(u32u *)(g_mem + ie->iat) = thunk_register(ie->fn, nm);
    }
}
