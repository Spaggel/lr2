/* ADVAPI32 registry, backed by a small text file in the save directory.
 *
 * Keys are full paths like "HKLM\SOFTWARE\LEGO media\games\LEGO Racers 2";
 * comparisons ignore case.  The file format is one value per line:
 *   <key>|<name>|<type>|<hex data>
 */
#include "../rt.h"
#include "../savestate.h"
#include <pthread.h>
#include <ctype.h>

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_MORE_DATA 234
#define REG_SZ 1
#define REG_DWORD 4

#define MAX_VALUES 256
#define MAX_KEYS 64
#define HKEY_BASE 0x00a00000u

typedef struct {
    char key[260], name[128];
    uint32_t type, size;
    uint8_t *data;
} RegValue;

static RegValue s_vals[MAX_VALUES];
static int s_nvals;
static char s_keys[MAX_KEYS][260];       /* open key handles */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_loaded;

static const char *root_name(uint32_t h) {
    switch (h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    }
    return NULL;
}

/* Resolve an HKEY (root or opened) to its path. */
static const char *key_path(uint32_t h) {
    const char *r = root_name(h);
    if (r)
        return r;
    if (h >= HKEY_BASE && h < HKEY_BASE + MAX_KEYS * 4 && s_keys[(h - HKEY_BASE) / 4][0])
        return s_keys[(h - HKEY_BASE) / 4];
    return NULL;
}

static RegValue *find(const char *key, const char *name) {
    for (int i = 0; i < s_nvals; i++)
        if (!strcasecmp(s_vals[i].key, key) && !strcasecmp(s_vals[i].name, name))
            return &s_vals[i];
    return NULL;
}

static void set(const char *key, const char *name, uint32_t type, const void *data, uint32_t size) {
    RegValue *v = find(key, name);
    if (!v) {
        if (s_nvals == MAX_VALUES)
            return;
        v = &s_vals[s_nvals++];
        snprintf(v->key, sizeof v->key, "%s", key);
        snprintf(v->name, sizeof v->name, "%s", name);
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = malloc(size ? size : 1);
    memcpy(v->data, data, size);
}

static void set_sz(const char *key, const char *name, const char *s) {
    set(key, name, REG_SZ, s, (uint32_t)strlen(s) + 1);
}

static void reg_path(char *out, size_t sz) {
    snprintf(out, sz, "%s/registry.txt", g_save_dir);
}

static void load(void) {
    if (s_loaded)
        return;
    s_loaded = true;
    /* What the installer would have written.  The CD path holds language\;
       movies and music are found relative to the game directory. */
    set_sz("HKLM\\SOFTWARE\\LEGO media\\games\\LEGO Racers 2", "CDpath", "D:\\");
    set_sz("HKLM\\SOFTWARE\\LEGO media\\games\\LEGO Racers 2", "moviespath", "C:\\LR2\\");
    char p[1100];
    reg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char *k = strtok(line, "|"), *n = strtok(NULL, "|"), *t = strtok(NULL, "|"), *d = strtok(NULL, "|\r\n");
        if (!k || !n || !t)
            continue;
        uint8_t buf[512];
        uint32_t len = 0;
        for (; d && d[0] && d[1] && len < sizeof buf; d += 2) {
            unsigned b;
            sscanf(d, "%2x", &b);
            buf[len++] = (uint8_t)b;
        }
        set(k, n, (uint32_t)strtoul(t, NULL, 10), buf, len);
    }
    fclose(f);
}

static void save(void) {
    char p[1100];
    reg_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f)
        return;
    for (int i = 0; i < s_nvals; i++) {
        fprintf(f, "%s|%s|%u|", s_vals[i].key, s_vals[i].name, s_vals[i].type);
        for (uint32_t j = 0; j < s_vals[i].size; j++)
            fprintf(f, "%02x", s_vals[i].data[j]);
        fputc('\n', f);
    }
    fclose(f);
}

/* RegCreateKeyExA(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired,
                   lpSecurityAttributes, phkResult, lpdwDisposition) */
void imp_ADVAPI32_RegCreateKeyExA(CPU *c) {
    pthread_mutex_lock(&s_lock);
    load();
    const char *base = key_path(ARG(0));
    uint32_t res = ERROR_INVALID_HANDLE;
    if (base) {
        char path[260];
        if (ARGS(1) && *ARGS(1))
            snprintf(path, sizeof path, "%s\\%s", base, ARGS(1));
        else
            snprintf(path, sizeof path, "%s", base);
        int i = 0;
        while (i < MAX_KEYS && s_keys[i][0])
            i++;
        if (i < MAX_KEYS) {
            snprintf(s_keys[i], sizeof s_keys[i], "%s", path);
            *(u32u *)G2H(ARG(7)) = HKEY_BASE + (uint32_t)i * 4;
            if (ARG(8))
                *(u32u *)G2H(ARG(8)) = 2;   /* REG_OPENED_EXISTING_KEY */
            res = ERROR_SUCCESS;
            LOGD("RegCreateKeyExA(%s) -> %x", path, HKEY_BASE + i * 4);
        }
    }
    pthread_mutex_unlock(&s_lock);
    RET(9, res);
}

void imp_ADVAPI32_RegCloseKey(CPU *c) {
    uint32_t h = ARG(0);
    pthread_mutex_lock(&s_lock);
    if (h >= HKEY_BASE && h < HKEY_BASE + MAX_KEYS * 4)
        s_keys[(h - HKEY_BASE) / 4][0] = 0;
    pthread_mutex_unlock(&s_lock);
    RET(1, ERROR_SUCCESS);
}

/* RegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData) */
void imp_ADVAPI32_RegQueryValueExA(CPU *c) {
    pthread_mutex_lock(&s_lock);
    load();
    const char *k = key_path(ARG(0));
    const char *name = ARGS(1) ? ARGS(1) : "";
    uint32_t res;
    RegValue *v = k ? find(k, name) : NULL;
    if (!k) {
        res = ERROR_INVALID_HANDLE;
    } else if (!v) {
        res = ERROR_FILE_NOT_FOUND;
    } else {
        if (ARG(3))
            *(u32u *)G2H(ARG(3)) = v->type;
        res = ERROR_SUCCESS;
        if (ARG(5)) {
            uint32_t cap = *(u32u *)G2H(ARG(5));
            if (ARG(4)) {
                if (cap < v->size)
                    res = ERROR_MORE_DATA;
                else
                    memcpy(G2H(ARG(4)), v->data, v->size);
            }
            *(u32u *)G2H(ARG(5)) = v->size;
        }
    }
    LOGD("RegQueryValueExA(%s, %s) -> %u", k ? k : "?", name, res);
    pthread_mutex_unlock(&s_lock);
    RET(6, res);
}

/* RegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData) */
void imp_ADVAPI32_RegSetValueExA(CPU *c) {
    pthread_mutex_lock(&s_lock);
    load();
    const char *k = key_path(ARG(0));
    uint32_t res = ERROR_INVALID_HANDLE;
    if (k) {
        set(k, ARGS(1) ? ARGS(1) : "", ARG(3), ARG(4) ? G2H(ARG(4)) : "", ARG(4) ? ARG(5) : 0);
        save();
        res = ERROR_SUCCESS;
        LOGD("RegSetValueExA(%s, %s)", k, ARGS(1) ? ARGS(1) : "");
    }
    pthread_mutex_unlock(&s_lock);
    RET(6, res);
}

/* -------------------------------------------------------------- savestates
 * Values live in the registry file and are not rewound; only the open key
 * handles are part of the state. */
static void reg_save(SnapBuf *b) {
    sb_u32(b, MAX_KEYS);
    for (int i = 0; i < MAX_KEYS; i++)
        sb_str(b, s_keys[i][0] ? s_keys[i] : NULL);
}

static bool reg_load(SnapBuf *b) {
    if (sb_get_u32(b) != MAX_KEYS)
        return false;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_KEYS; i++) {
        char *k = sb_get_str(b);
        snprintf(s_keys[i], sizeof s_keys[i], "%s", k ? k : "");
        free(k);
    }
    pthread_mutex_unlock(&s_lock);
    return !b->err;
}

static const SnapSection s_reg_section = {
    SNAP_TAG('R', 'E', 'G', '_'), SNAP_ORDER_KERNEL, reg_save, NULL, NULL, reg_load,
};

__attribute__((constructor)) static void reg_snap_register(void) { snap_section(&s_reg_section); }
