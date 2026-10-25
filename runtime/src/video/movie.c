/* Movies: the intro splashes and the world movies of adventure mode.
 *
 * The game plays them with DirectShow (FUN_00499500, cdecl (path, skippable)):
 * a hand-built graph of the MPEG splitter and its own texture and audio
 * renderer filters, polled until EC_COMPLETE or, when skippable, Esc.  This
 * runtime has no DirectShow, so the function is wrapped (overrides.py WRAPS)
 * and the movie is played here instead: pl_mpeg decodes the MPEG-1 file, the
 * frames are blitted into the window with the aspect kept (as present() does
 * for the back buffer) and the MP2 audio goes to an SDL audio stream.
 *
 * Any movie ends on Esc, Enter or Space, or A, B or Start on a pad (the game
 * itself lets only some be skipped, and only with Esc); keys and buttons
 * already held when the movie starts count only after they are released.
 * --no-intro (or LR2_NO_INTRO) leaves out the three splash movies at start,
 * but not the world movies of adventure mode.
 */
#include "../rt.h"
#include "../d3d8/d3d8_internal.h"
#include "../d3d8/gl.h"
#include "../win32/user32.h"
#include <SDL3/SDL.h>
#include <dirent.h>
#include <sys/stat.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct {
    GLuint tex;
    int w, h;
    uint8_t *rgb;
    bool fresh;               /* a frame was decoded since the last draw */
    SDL_AudioStream *audio;
} Movie;

static void on_video(plm_t *plm, plm_frame_t *f, void *user) {
    (void)plm;
    Movie *mv = user;
    plm_frame_to_rgb(f, mv->rgb, mv->w * 3);
    mv->fresh = true;
}

static void on_audio(plm_t *plm, plm_samples_t *s, void *user) {
    (void)plm;
    Movie *mv = user;
    if (mv->audio)
        SDL_PutAudioStreamData(mv->audio, s->interleaved, (int)(s->count * 2 * sizeof(float)));
}

/* The file the guest names, or else the file of that name (any case) in
   the game's Game Data\Movies folder. */
static bool find_movie(const char *guest_path, char *out, size_t outsz) {
    struct stat st;
    if (path_to_host(guest_path, out, outsz) && stat(out, &st) == 0)
        return true;
    const char *base = guest_path;
    for (const char *p = guest_path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/Game Data/Movies", g_game_dir);
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcasecmp(e->d_name, base)) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Skip keys and buttons: bit 0-2 keyboard, 3-5 pad. */
static unsigned skip_inputs(void) {
    unsigned bits = 0;
    const bool *k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_ESCAPE])
        bits |= 1;
    if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER])
        bits |= 2;
    if (k[SDL_SCANCODE_SPACE])
        bits |= 4;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        SDL_Gamepad *gp = SDL_GetGamepadFromID(ids[i]);
        if (!gp)
            continue;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH))
            bits |= 8;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST))
            bits |= 16;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START))
            bits |= 32;
    }
    SDL_free(ids);
    return bits;
}

static void draw(Movie *mv, SDL_Window *win) {
    int pw, ph;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    glBindTexture(GL_TEXTURE_2D, mv->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mv->w, mv->h, GL_RGB, GL_UNSIGNED_BYTE, mv->rgb);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint rfbo;
    glGenFramebuffers(1, &rfbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rfbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mv->tex, 0);
    /* The movies are 4:3 with square pixels. */
    float s = SDL_min((float)pw / (float)mv->w, (float)ph / (float)mv->h);
    int dw = (int)((float)mv->w * s), dh = (int)((float)mv->h * s);
    int dx = (pw - dw) / 2, dy = (ph - dh) / 2;
    /* Rows are top-down; the window's origin is bottom-left. */
    glBlitFramebuffer(0, 0, mv->w, mv->h, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &rfbo);
    SDL_GL_SwapWindow(win);
}

bool g_no_intro;

static void play(const char *host) {
    plm_t *plm = plm_create_with_filename(host);
    if (!plm) {
        LOGW("movie: cannot open %s", host);
        return;
    }
    Movie mv = {0};
    mv.w = plm_get_width(plm);
    mv.h = plm_get_height(plm);
    mv.rgb = calloc((size_t)mv.w * (size_t)mv.h, 3);
    plm_set_loop(plm, 0);
    plm_set_video_decode_callback(plm, on_video, &mv);
    plm_set_audio_decode_callback(plm, on_audio, &mv);
    if (plm_get_num_audio_streams(plm) > 0) {
        SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, plm_get_samplerate(plm)};
        mv.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (mv.audio) {
            plm_set_audio_enabled(plm, 1);
            plm_set_audio_stream(plm, 0);
            /* Decode audio this far ahead of the picture, so the stream never runs dry. */
            plm_set_audio_lead_time(plm, 4096.0 / spec.freq);
            SDL_ResumeAudioStreamDevice(mv.audio);
        } else {
            LOGW("movie: no audio: %s", SDL_GetError());
            plm_set_audio_enabled(plm, 0);
        }
    }
    glGenTextures(1, &mv.tex);
    glBindTexture(GL_TEXTURE_2D, mv.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, mv.w, mv.h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    SDL_Window *win = user32_sdl_window(g_dev->hwnd);
    LOGI("movie: %s (%dx%d, %.0f fps, %s)", host, mv.w, mv.h, plm_get_framerate(plm),
         mv.audio ? "sound" : "silent");
    unsigned held = skip_inputs();   /* pressed before the movie: wait for release */
    bool skipped = false;
    uint64_t last = SDL_GetTicksNS();
    while (!plm_has_ended(plm)) {
        user32_pump();
        unsigned now_in = skip_inputs();
        held &= now_in;
        if (now_in & ~held) {
            skipped = true;
            break;
        }
        uint64_t t = SDL_GetTicksNS();
        double dt = (double)(t - last) / 1e9;
        last = t;
        plm_decode(plm, SDL_min(dt, 1.0 / 20));
        if (mv.fresh) {
            mv.fresh = false;
            draw(&mv, win);   /* swap waits for vsync */
        } else {
            SDL_Delay(1);
        }
    }
    /* Let the last of the sound play out unless the movie was skipped.  The
       stream keeps back its resampler's last samples until it is flushed. */
    if (!skipped && mv.audio) {
        SDL_FlushAudioStream(mv.audio);
        uint64_t until = SDL_GetTicksNS() + SDL_NS_PER_SECOND;
        while (SDL_GetAudioStreamQueued(mv.audio) > 0 && SDL_GetTicksNS() < until) {
            user32_pump();
            SDL_Delay(5);
        }
    }
    LOGI("movie: %s", skipped ? "skipped" : "finished");
    if (mv.audio)
        SDL_DestroyAudioStream(mv.audio);
    glDeleteTextures(1, &mv.tex);
    free(mv.rgb);
    plm_destroy(plm);
    /* GL state the device caches (bindings, FBOs, unpack state) changed. */
    g_dev->dirty = ~0u;
}

static bool is_intro(const char *path) {
    for (const char *p = path; *p; p++)
        if (!strncasecmp(p, "splash", 6))
            return true;
    return false;
}

/* Wraps FUN_00499500 (cdecl: const char *path, int skippable; returns 1). */
void lr2_movie_play(CPU *c, void (*orig)(CPU *restrict c)) {
    (void)orig;
    const char *guest = (const char *)G2H(ARG(0));
    char host[1200];
    if (!g_dev)
        LOGW("movie: %s: no device yet", guest);
    else if (getenv("LR2_NO_MOVIES"))
        LOGI("movie: %s: LR2_NO_MOVIES", guest);
    else if ((g_no_intro || getenv("LR2_NO_INTRO")) && is_intro(guest))
        LOGI("movie: %s: left out (--no-intro)", guest);
    else if (!find_movie(guest, host, sizeof host))
        LOGW("movie: %s not found", guest);
    else
        play(host);
    c->eax = 1;
    c->esp += 4;   /* cdecl: the caller pops the arguments */
}
