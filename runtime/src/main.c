/* Entry point: sets up the guest machine and runs the game's CRT entry. */
#include "rt.h"
#include "savestate.h"
#include "os.h"
#include "input/lr2pad.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   /* WinMain on Windows */

int g_log_level = 2;
bool g_windowed;

void rt_log(int level, const char *fmt, ...) {
    if (level > g_log_level)
        return;
    static const char *tag[] = {"E", "W", "I", "D", "T"};
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%5lld.%03ld %s] ", (long long)ts.tv_sec % 100000, ts.tv_nsec / 1000000,
            tag[level < 4 ? level : 4]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

_Noreturn void rt_fatal(const char *fmt, ...) {
    char msg[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "fatal: %s\n", msg);
    fflush(stderr);
    /* Started from a file manager there is no terminal to read this in. */
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LEGO Racers 2", msg, NULL);
    exit(1);
}

_Noreturn void unimplemented_import(CPU *c, const char *name) {
    fprintf(stderr, "\n*** unimplemented import %s (called from %08x)\n", name,
            *(u32u *)(g_mem + c->esp));
    TRAP(c, *(u32u *)(g_mem + c->esp), "unimplemented import");
}

void kernel32_init(int argc, char **argv);
void user32_init(void);

/* GAMEDATA.GTC marks a game dir: the CD's install folder. */
static bool has_game_data(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d)))
        found = !strcasecmp(e->d_name, "GAMEDATA.GTC");
    closedir(d);
    return found;
}

/* The folder holding this program; on macOS the one holding the .app. */
static void program_dir(char *out, size_t sz) {
    const char *base = SDL_GetBasePath();
    snprintf(out, sz, "%s", base ? base : ".");
    char *app = strstr(out, ".app/Contents/");
    if (app) {
        *app = 0;
        char *slash = strrchr(out, '/');
        if (slash)
            *slash = 0;
    }
    size_t l = strlen(out);
    while (l > 1 && (out[l - 1] == '/' || out[l - 1] == '\\'))
        out[--l] = 0;
}

/* Without --game, look for the game files in the current directory and
   next to this program, directly or in the layouts this repository and
   the release packages use:
     game/install   CD contents in game/ (release packages, made by setup)
     game/cd/install
   The CD dir is then the parent of install/, and saves go to game/save. */
static bool find_game(const char *prog) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, ".");
    static const struct {
        const char *sub, *cd, *save;
    } layouts[] = {
        {"", "", "save"},
        {"game/install", "game", "game/save"},
        {"game/cd/install", "game/cd", "game/save"},
    };
    const char *roots[] = {cwd, prog};
    char dir[1100];
    for (int r = 0; r < 2; r++) {
        for (size_t i = 0; i < sizeof layouts / sizeof *layouts; i++) {
            snprintf(dir, sizeof dir, "%s%s%s", roots[r], *layouts[i].sub ? "/" : "", layouts[i].sub);
            if (!has_game_data(dir))
                continue;
            snprintf(g_game_dir, sizeof g_game_dir, "%s", dir);
            if (!g_cd_dir[0])
                snprintf(g_cd_dir, sizeof g_cd_dir, "%s%s%s", roots[r], *layouts[i].cd ? "/" : "", layouts[i].cd);
            if (!g_save_dir[0])
                snprintf(g_save_dir, sizeof g_save_dir, "%s/%s", roots[r], layouts[i].save);
            return true;
        }
    }
    return false;
}

static void usage(void) {
    fprintf(stderr,
            "usage: lr2 [options]\n"
            "  --game DIR   directory with the game files (default: found in the current\n"
            "               dir or next to this program, see README.md)\n"
            "  --cd DIR     directory with the CD contents (default: game dir)\n"
            "  --save DIR   directory for saves/settings (default: <game>/save)\n"
            "  --hd DIR     HD texture pack (default: an \"HD TEXTURES\" folder in the\n"
            "               game dir, CD dir or current dir, if there is one)\n"
            "  --no-hd      use the original textures\n"
            "  --windowed   run in a window instead of full screen\n"
            "  --setup [ISO|DIR]  copy the game files from your CD (a disc image, the\n"
            "               CD or a copy of it) into the game folder next to this program\n"
            "  --log N      log level 0-4 (default 2)\n");
    exit(1);
}

int main(int argc, char **argv) {
    os_init();
    const char *hd_dir = NULL, *setup_src = NULL;
    bool no_hd = false, setup = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc)
            snprintf(g_game_dir, sizeof g_game_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--cd") && i + 1 < argc)
            snprintf(g_cd_dir, sizeof g_cd_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc)
            snprintf(g_save_dir, sizeof g_save_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--hd") && i + 1 < argc)
            hd_dir = argv[++i];
        else if (!strcmp(argv[i], "--no-hd"))
            no_hd = true;
        else if (!strcmp(argv[i], "--setup")) {
            setup = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                setup_src = argv[++i];
        } else if (!strcmp(argv[i], "--windowed"))
            g_windowed = true;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            g_log_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help"))
            usage();
    }
    if (getenv("LR2_WINDOWED"))
        g_windowed = true;
    char prog[1024];
    program_dir(prog, sizeof prog);
    if (!g_game_dir[0] && (setup || !find_game(prog))) {
        /* First run of a release package: copy the player's CD into game/. */
        if (!setup_run(setup_src, prog))
            return 0;
        snprintf(g_game_dir, sizeof g_game_dir, "%s/game/install", prog);
        snprintf(g_cd_dir, sizeof g_cd_dir, "%s/game", prog);
        snprintf(g_save_dir, sizeof g_save_dir, "%s/game/save", prog);
    }
    if (!g_cd_dir[0])
        snprintf(g_cd_dir, sizeof g_cd_dir, "%s", g_game_dir);
    if (!g_save_dir[0])
        snprintf(g_save_dir, sizeof g_save_dir, "%s/save", g_game_dir);
    mkdir(g_save_dir, 0755);
    char log_path[1100];
    snprintf(log_path, sizeof log_path, "%s/lr2.log", g_save_dir);
    os_log_file(log_path);

    if (!has_game_data(g_game_dir))
        rt_fatal("Could not find the LEGO Racers 2 game files (GAMEDATA.GTC) in %s.\n\n"
                 "Start the game with --setup to copy them from your CD.", g_game_dir);
    LOGI("game dir: %s", g_game_dir);
    LOGI("cd dir:   %s", g_cd_dir);
    LOGI("save dir: %s", g_save_dir);
    if (!no_hd)
        hd_init(hd_dir);

    install_crash_handlers();
    mem_init();
    heap_init();
    pe_load();
    lr2pad_init();
    dispatch_init();

    static CPU main_cpu;
    CPU *c = &main_cpu;
    cpu_set_current(c);
    teb_init(c, GUEST_MAIN_STACK_LO, GUEST_MAIN_STACK_HI);
    kernel32_init(argc, argv);
    g_trace_thunks = getenv("LR2_TRACE_THUNKS") != NULL;
    user32_init();
    autoinput_init();
    snap_init();
    snap_thread_enter(SNAP_THREAD_MAIN, 0, c);

    LOGI("starting guest at %08x", g_pe.entry);
    guest_call(c, g_pe.entry, 0, NULL);
    LOGI("guest entry returned (eax=%08x)", c->eax);
    SDL_Quit();
    return (int)c->eax;
}
