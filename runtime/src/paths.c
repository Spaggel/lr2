/* Windows path -> host path translation.
 *
 * Virtual drives:
 *   C:\LR2\...   the game directory (where the original exe lives)
 *   C:\...       anything else on C: goes under the save directory
 *   D:\...       the CD contents
 * Lookups are case-insensitive, component by component, because the game
 * mixes "Game Data" and "game data" and host filesystems may be
 * case-sensitive.
 */
#include "rt.h"
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <pthread.h>

char g_game_dir[1024];
char g_cd_dir[1024];
char g_save_dir[1024];
char g_cwd_win[260] = "C:\\LR2";
static pthread_mutex_t s_cwd_lock = PTHREAD_MUTEX_INITIALIZER;

#define GAME_PREFIX "C:\\LR2"

/* Make an absolute, normalized Windows path ("X:\a\b"). */
static void win_normalize(const char *in, char *out, size_t outsz) {
    char tmp[1024];
    if (isalpha((unsigned char)in[0]) && in[1] == ':') {
        if (in[2] == '\\' || in[2] == '/')
            snprintf(tmp, sizeof tmp, "%s", in);
        else
            snprintf(tmp, sizeof tmp, "%c:\\%s", in[0], in + 2);
    } else if (in[0] == '\\' || in[0] == '/') {
        pthread_mutex_lock(&s_cwd_lock);
        snprintf(tmp, sizeof tmp, "%c:%s", g_cwd_win[0], in);
        pthread_mutex_unlock(&s_cwd_lock);
    } else {
        pthread_mutex_lock(&s_cwd_lock);
        snprintf(tmp, sizeof tmp, "%s\\%s", g_cwd_win, in);
        pthread_mutex_unlock(&s_cwd_lock);
    }
    /* Split into components, resolving . and .. */
    char *comps[128];
    int n = 0;
    char drive = (char)toupper((unsigned char)tmp[0]);
    for (char *tok = strtok(tmp + 2, "\\/"); tok; tok = strtok(NULL, "\\/")) {
        if (!strcmp(tok, ".") || !*tok)
            continue;
        if (!strcmp(tok, "..")) {
            if (n)
                n--;
            continue;
        }
        if (n < 128)
            comps[n++] = tok;
    }
    size_t o = (size_t)snprintf(out, outsz, "%c:", drive);
    for (int i = 0; i < n && o < outsz; i++)
        o += (size_t)snprintf(out + o, outsz - o, "\\%s", comps[i]);
    if (n == 0 && o < outsz)
        snprintf(out + o, outsz - o, "\\");
}

/* Append component to host dir, matching an existing entry ignoring case. */
static void append_ci(char *host, size_t sz, const char *comp) {
    DIR *d = opendir(host);
    const char *use = comp;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcasecmp(e->d_name, comp)) {
                use = e->d_name;
                break;
            }
        }
        size_t l = strlen(host);
        snprintf(host + l, sz - l, "/%s", use);
        closedir(d);
        return;
    }
    size_t l = strlen(host);
    snprintf(host + l, sz - l, "/%s", comp);
}

void mkdir_p(const char *host) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", host);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(tmp, 0755);
            *s = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void map_under(const char *root, const char *rest, char *out, size_t outsz) {
    snprintf(out, outsz, "%s", root);
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", rest);
    for (char *tok = strtok(buf, "\\"); tok; tok = strtok(NULL, "\\"))
        append_ci(out, outsz, tok);
}

static bool host_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

bool path_to_host(const char *win, char *out, size_t outsz) {
    if (!win || !*win)
        return false;
    char norm[1024];
    win_normalize(win, norm, sizeof norm);
    /* Files the original install kept in different places: the game dir
       and the CD's install\ folder hold the same game data, so a path that
       is missing in one is looked up in the other. */
    bool game = !strncasecmp(norm, GAME_PREFIX, strlen(GAME_PREFIX)) &&
                (norm[strlen(GAME_PREFIX)] == '\\' || !norm[strlen(GAME_PREFIX)]);
    if (game || norm[0] == 'D') {
        const char *rest = game ? norm + strlen(GAME_PREFIX) : norm + 2;
        char cd_install[1100];
        snprintf(cd_install, sizeof cd_install, "%s/install", g_cd_dir);
        const char *roots[3] = {game ? g_game_dir : g_cd_dir, game ? cd_install : g_game_dir,
                                game ? g_cd_dir : cd_install};
        for (int i = 0; i < 3; i++) {
            map_under(roots[i], rest, out, outsz);
            if (host_exists(out))
                return true;
        }
        map_under(roots[0], rest, out, outsz);
        return true;
    }
    char root[1100];
    snprintf(root, sizeof root, "%s/%c", g_save_dir, norm[0]);
    mkdir(root, 0755);
    map_under(root, norm + 2, out, outsz);
    return true;
}

void path_set_cwd(const char *win) {
    char norm[260];
    win_normalize(win, norm, sizeof norm);
    pthread_mutex_lock(&s_cwd_lock);
    snprintf(g_cwd_win, sizeof g_cwd_win, "%s", norm);
    pthread_mutex_unlock(&s_cwd_lock);
}

void path_get_cwd(char *out, size_t sz) {
    pthread_mutex_lock(&s_cwd_lock);
    snprintf(out, sz, "%s", g_cwd_win);
    pthread_mutex_unlock(&s_cwd_lock);
}
