/* DirectSound 8 on SDL3: a software mixer with DirectSound3D positioning.
 *
 * Secondary buffers keep their sample data in guest memory (Lock hands the
 * guest pointers straight into it, and savestates capture it).  The mixer
 * runs in the SDL audio callback; every buffer's play cursor advances in
 * fixed point as it is mixed, so GetCurrentPosition reflects real progress.
 *
 * DS3D: inverse-distance rolloff between min and max distance, simple
 * linear panning from the listener-relative direction.  Doppler is ignored.
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include "../win32/com.h"
#include "../win32/handles.h"

#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_CONTROLUNAVAIL 0x8878001eu
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_BADFORMAT 0x88780064u
#define DSERR_NODRIVER 0x88780078u
#define DSERR_UNSUPPORTED 0x80004001u

#define DSBCAPS_PRIMARYBUFFER 0x1u
#define DSBCAPS_STATIC 0x2u
#define DSBCAPS_LOCSOFTWARE 0x8u
#define DSBCAPS_CTRL3D 0x10u
#define DSBCAPS_CTRLFREQUENCY 0x20u
#define DSBCAPS_CTRLPAN 0x40u
#define DSBCAPS_CTRLVOLUME 0x80u
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x100u
#define DSBCAPS_MUTE3DATMAXDISTANCE 0x20000u
#define DSBPLAY_LOOPING 1u
#define DSBSTATUS_PLAYING 1u
#define DSBSTATUS_LOOPING 4u
#define DSBLOCK_FROMWRITECURSOR 1u
#define DSBLOCK_ENTIREBUFFER 2u
#define DSBPN_OFFSETSTOP 0xffffffffu
#define DS3DMODE_NORMAL 0u
#define DS3DMODE_HEADRELATIVE 1u
#define DS3DMODE_DISABLE 2u

#define OUT_RATE 44100
#define MAX_NOTIFY 64

static const Guid IID_IDirectSound3DBuffer = {0x279afa86, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const Guid IID_IDirectSound3DListener = {0x279afa84, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const Guid IID_IDirectSoundNotify = {0xb0210783, 0x89cd, 0x11d0, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}};
static const Guid IID_IDirectSoundBuffer8 = {0x6825a449, 0x7524, 0x4d82, {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}};
static const Guid IID_IDirectSoundBuffer = {0x279afa85, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};

typedef struct { float x, y, z; } V3;

/* Sample memory shared between a buffer and its duplicates. */
typedef struct {
    uint32_t mem, size;
    int refs;
} SoundData;

typedef struct SoundBuffer SoundBuffer;

/* Secondary interfaces (3D buffer, notify, listener) are separate guest
   objects that point back at their buffer. */
typedef struct {
    ComObj com;
    SoundBuffer *buf;
} SubIface;

typedef struct {
    float min_dist, max_dist;
    V3 pos, vel, cone_dir;
    uint32_t cone_in, cone_out;
    int32_t cone_out_vol;
    uint32_t mode;
} Params3D;

struct SoundBuffer {
    ComObj com;
    uint32_t flags;
    bool primary;
    SoundData *data;
    /* format */
    uint32_t rate, channels, bits, block;
    /* playback */
    bool playing, looping;
    uint64_t pos;              /* play cursor in frames, 32.32 fixed point */
    uint32_t freq;             /* 0 = format rate */
    int32_t volume, pan;       /* hundredths of a dB */
    Params3D p3d, p3d_deferred;
    /* notifications */
    struct { uint32_t ofs, event; } notify[MAX_NOTIFY];
    int nnotify;
    SubIface *i3d, *inotify;
    double energy;             /* LR2_AUDIO_STATS: sum of squared output */
    uint32_t energy_frames;
};

typedef struct {
    ComObj com;
} DSound;

typedef struct {
    float dist_factor, rolloff, doppler;
    V3 pos, vel, front, top;
} Listener;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static SoundBuffer *s_bufs[512];
static int s_nbufs;
static SDL_AudioStream *s_stream;
static Listener s_listener = {1, 1, 1, {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}};
static Listener s_listener_deferred;
static int32_t s_master_volume;

static inline void put32(uint32_t a, uint32_t v) { *(u32u *)G2H(a) = v; }
static inline uint32_t get32(uint32_t a) { return *(u32u *)G2H(a); }
static inline float getf(uint32_t a) { float f; memcpy(&f, G2H(a), 4); return f; }
static inline void putf(uint32_t a, float f) { memcpy(G2H(a), &f, 4); }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* -------------------------------------------------------------- mixer */
static float db_gain(int32_t mb) {
    if (mb <= -10000)
        return 0.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

static V3 v3sub(V3 a, V3 b) { return (V3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static float v3dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 v3cross(V3 a, V3 b) {
    return (V3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static V3 v3norm(V3 a) {
    float l = sqrtf(v3dot(a, a));
    return l > 1e-6f ? (V3){a.x / l, a.y / l, a.z / l} : (V3){0, 0, 0};
}

/* Per-channel gains for a buffer, including 3D placement. */
static void buffer_gains(const SoundBuffer *b, float *gl, float *gr) {
    float g = db_gain(b->volume) * db_gain(s_master_volume);
    float l = 1.0f, r = 1.0f;
    if ((b->flags & DSBCAPS_CTRL3D) && b->p3d.mode != DS3DMODE_DISABLE) {
        V3 rel = b->p3d.mode == DS3DMODE_HEADRELATIVE ? b->p3d.pos : v3sub(b->p3d.pos, s_listener.pos);
        float d = sqrtf(v3dot(rel, rel)) * s_listener.dist_factor;
        float mn = b->p3d.min_dist * s_listener.dist_factor, mx = b->p3d.max_dist * s_listener.dist_factor;
        if (d > mx && (b->flags & DSBCAPS_MUTE3DATMAXDISTANCE)) {
            g = 0;
        } else {
            float dc = d < mn ? mn : d > mx ? mx : d;
            if (mn > 0 && dc > mn)
                g *= mn / (mn + s_listener.rolloff * (dc - mn));
        }
        /* Pan from the direction in listener space (left-handed). */
        V3 front = v3norm(s_listener.front), top = v3norm(s_listener.top);
        V3 right = v3norm(v3cross(top, front));
        V3 dir = b->p3d.mode == DS3DMODE_HEADRELATIVE ? v3norm(rel)
                                                      : v3norm(rel);
        float x = b->p3d.mode == DS3DMODE_HEADRELATIVE ? dir.x : v3dot(dir, right);
        if (x > 0)
            l = 1.0f - 0.7f * x;
        else
            r = 1.0f + 0.7f * x;
    } else if (b->pan) {
        float pg = db_gain(-abs(b->pan));
        if (b->pan > 0)
            l = pg;
        else
            r = pg;
    }
    *gl = g * l;
    *gr = g * r;
}

static inline void sample_frame(const SoundBuffer *b, const uint8_t *m, uint32_t f, float *sl, float *sr) {
    if (b->bits == 16) {
        const int16_t *p = (const int16_t *)(m + f * b->block);
        int16_t a, c;
        memcpy(&a, p, 2);
        if (b->channels > 1)
            memcpy(&c, p + 1, 2);
        else
            c = a;
        *sl = a / 32768.0f;
        *sr = c / 32768.0f;
    } else {
        const uint8_t *p = m + f * b->block;
        *sl = (p[0] - 128) / 128.0f;
        *sr = ((b->channels > 1 ? p[1] : p[0]) - 128) / 128.0f;
    }
}

static void fire_notifications(SoundBuffer *b, uint32_t from_byte, uint32_t to_byte, bool wrapped, bool stopped) {
    for (int i = 0; i < b->nnotify; i++) {
        uint32_t o = b->notify[i].ofs;
        bool hit;
        if (o == DSBPN_OFFSETSTOP)
            hit = stopped;
        else if (!wrapped)
            hit = o >= from_byte && o < to_byte;
        else
            hit = o >= from_byte || o < to_byte;
        if (hit)
            handle_event_set(b->notify[i].event, true);
    }
}

/* Mix one buffer into out (stereo float, n frames). */
static void mix_buffer(SoundBuffer *b, float *out, int n) {
    uint32_t frames = b->data->size / b->block;
    if (!frames) {
        b->playing = false;
        return;
    }
    const uint8_t *m = G2H(b->data->mem);
    uint32_t rate = b->freq ? b->freq : b->rate;
    uint64_t step = ((uint64_t)rate << 32) / OUT_RATE;
    uint64_t end = (uint64_t)frames << 32;
    float gl, gr;
    buffer_gains(b, &gl, &gr);
    uint32_t start_byte = (uint32_t)(b->pos >> 32) * b->block;
    bool wrapped = false, stopped = false;
    for (int i = 0; i < n; i++) {
        uint32_t f = (uint32_t)(b->pos >> 32);
        float frac = (float)(uint32_t)b->pos / 4294967296.0f;
        uint32_t f2 = f + 1;
        if (f2 >= frames)
            f2 = b->looping ? 0 : f;
        float l0, r0, l1, r1;
        sample_frame(b, m, f, &l0, &r0);
        sample_frame(b, m, f2, &l1, &r1);
        out[2 * i] += (l0 + (l1 - l0) * frac) * gl;
        out[2 * i + 1] += (r0 + (r1 - r0) * frac) * gr;
        b->pos += step;
        if (b->pos >= end) {
            if (b->looping) {
                b->pos -= end;
                wrapped = true;
            } else {
                b->pos = 0;
                b->playing = false;
                stopped = true;
                break;
            }
        }
    }
    if (b->nnotify) {
        uint32_t end_byte = stopped ? b->data->size : (uint32_t)(b->pos >> 32) * b->block;
        fire_notifications(b, start_byte, end_byte, wrapped, stopped);
    }
}

/* Master peak limiter. Identical engine loops started together add up in
   phase at race start and would otherwise hard-clip; this pulls the bus
   down instantly on peaks and recovers over ~100 ms. */
static void limit(float *mix, int n) {
    static float gain = 1.0f;
    const float ceiling = 0.95f, release = 1.0f / (0.1f * OUT_RATE);
    for (int i = 0; i < n; i++) {
        float l = mix[2 * i], r = mix[2 * i + 1];
        float peak = fmaxf(fabsf(l), fabsf(r));
        if (peak * gain > ceiling)
            gain = ceiling / peak;
        else
            gain += (1.0f - gain) * release;
        mix[2 * i] = l * gain;
        mix[2 * i + 1] = r * gain;
    }
}

/* Debug: LR2_AUDIO_DUMP=file records the mix as raw stereo float32 at OUT_RATE. */
static FILE *s_dump;

static void SDLCALL audio_cb(void *ud, SDL_AudioStream *stream, int additional, int total) {
    (void)ud;
    (void)total;
    static float mix[4096 * 2];
    while (additional > 0) {
        int n = additional / 8;
        if (n > 4096)
            n = 4096;
        if (n <= 0)
            break;
        memset(mix, 0, (size_t)n * 8);
        static int stats = -1;
        static uint64_t mixed;
        static float solo[4096 * 2];
        static double total_energy;
        static uint32_t clipped;
        if (stats < 0)
            stats = getenv("LR2_AUDIO_STATS") != NULL;
        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < s_nbufs; i++) {
            SoundBuffer *b = s_bufs[i];
            if (!b->playing || b->primary)
                continue;
            if (!stats) {
                mix_buffer(b, mix, n);
                continue;
            }
            memset(solo, 0, (size_t)n * 8);
            mix_buffer(b, solo, n);
            for (int k = 0; k < 2 * n; k++) {
                mix[k] += solo[k];
                b->energy += (double)solo[k] * solo[k];
            }
            b->energy_frames += (uint32_t)n;
        }
        if (stats)
            for (int k = 0; k < 2 * n; k++) {
                total_energy += (double)mix[k] * mix[k];
                clipped += mix[k] > 1.0f || mix[k] < -1.0f;
            }
        if (stats && (mixed += (uint64_t)n) >= 2 * OUT_RATE) {
            mixed = 0;
            LOGI("audio: mix rms %.3f clipped %u; listener pos (%.1f %.1f %.1f) front (%.2f %.2f %.2f) rolloff %.2f dist %.2f",
                 sqrt(total_energy / (2.0 * OUT_RATE * 2)), clipped, s_listener.pos.x, s_listener.pos.y, s_listener.pos.z, s_listener.front.x,
                 s_listener.front.y, s_listener.front.z, s_listener.rolloff, s_listener.dist_factor);
            for (int i = 0; i < s_nbufs; i++) {
                SoundBuffer *b = s_bufs[i];
                if (b->primary || !b->energy_frames)
                    continue;
                float gl, gr;
                buffer_gains(b, &gl, &gr);
                LOGI("  buf %08x %s%s %uHz/%uch vol %d freq %u 3d mode %u pos (%.1f %.1f %.1f) min %.1f max %.1f -> %.3f/%.3f rms %.3f",
                     b->com.guest, b->looping ? "loop " : "", (b->flags & DSBCAPS_CTRL3D) ? "3d" : "2d",
                     b->rate, b->channels, b->volume, b->freq, b->p3d.mode, b->p3d.pos.x, b->p3d.pos.y,
                     b->p3d.pos.z, b->p3d.min_dist, b->p3d.max_dist, gl, gr,
                     sqrt(b->energy / (2.0 * OUT_RATE * 2)));
                b->energy = 0;
                b->energy_frames = 0;
            }
            total_energy = 0;
            clipped = 0;
        }
        pthread_mutex_unlock(&s_lock);
        limit(mix, n);
        SDL_PutAudioStreamData(stream, mix, n * 8);
        if (s_dump)
            fwrite(mix, 8, (size_t)n, s_dump);
        additional -= n * 8;
    }
}

static bool audio_open(void) {
    if (s_stream)
        return true;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, OUT_RATE};
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!s_stream) {
        LOGW("DirectSound: cannot open audio device: %s", SDL_GetError());
        return false;
    }
    const char *dump = getenv("LR2_AUDIO_DUMP");
    if (dump && !(s_dump = fopen(dump, "wb")))
        LOGW("cannot open %s", dump);
    SDL_ResumeAudioStreamDevice(s_stream);
    LOGI("DirectSound: mixing at %d Hz on %s", OUT_RATE, SDL_GetCurrentAudioDriver());
    return true;
}

/* ------------------------------------------------------------ buffers */
static const GuestFn s_buf_vtbl[24];
static const GuestFn s_3d_vtbl[21];
static const GuestFn s_notify_vtbl[4];
static const GuestFn s_listener_vtbl[18];

static void params3d_default(Params3D *p) {
    memset(p, 0, sizeof *p);
    p->min_dist = 1.0f;
    p->max_dist = 1e9f;
    p->cone_in = p->cone_out = 360;
    p->cone_dir = (V3){0, 0, 1};
}

static void sub_destroy(ComObj *o) { free(o); }

static void buf_destroy(ComObj *o) {
    SoundBuffer *b = (SoundBuffer *)o;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_nbufs; i++) {
        if (s_bufs[i] == b) {
            s_bufs[i] = s_bufs[--s_nbufs];
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
    if (b->data && --b->data->refs == 0) {
        gfree(b->data->mem);
        free(b->data);
    }
    /* Sub-interfaces forward their references to the buffer, so nothing
       can still be using them. */
    if (b->i3d)
        com_release_obj(&b->i3d->com);
    if (b->inotify)
        com_release_obj(&b->inotify->com);
    free(b);
}

static uint32_t buf_new(SoundBuffer *b) {
    b->com.type = "IDirectSoundBuffer8";
    b->com.destroy = buf_destroy;
    uint32_t vt = com_vtbl("IDirectSoundBuffer8", s_buf_vtbl, 24);
    uint32_t g = com_wrap(&b->com, vt, 0);
    pthread_mutex_lock(&s_lock);
    if (s_nbufs < (int)(sizeof s_bufs / sizeof *s_bufs))
        s_bufs[s_nbufs++] = b;
    pthread_mutex_unlock(&s_lock);
    return g;
}

#define BUF ((SoundBuffer *)com_get(ARG(0)))

/* Sub-interfaces are created once, owned by their buffer (their own count
   stays 1), and forward AddRef/Release to the buffer. */
static uint32_t make_sub(SoundBuffer *b, SubIface **slot, const char *name, const GuestFn *vt, int n) {
    if (!*slot) {
        SubIface *s = calloc(1, sizeof *s);
        s->com.type = name;
        s->com.destroy = sub_destroy;
        s->buf = b;
        com_wrap(&s->com, com_vtbl(name, vt, n), 0);
        *slot = s;
    }
    com_addref_obj(&b->com);
    return (*slot)->com.guest;
}

static void Buf_QueryInterface(CPU *c) {
    SoundBuffer *b = BUF;
    Guid iid;
    memcpy(&iid, G2H(ARG(1)), 16);
    uint32_t out = 0;
    if (guid_eq(&iid, &IID_IDirectSound3DBuffer) && (b->flags & DSBCAPS_CTRL3D) && !b->primary)
        out = make_sub(b, &b->i3d, "IDirectSound3DBuffer8", s_3d_vtbl, 21);
    else if (guid_eq(&iid, &IID_IDirectSound3DListener) && b->primary)
        out = make_sub(b, &b->i3d, "IDirectSound3DListener8", s_listener_vtbl, 18);
    else if (guid_eq(&iid, &IID_IDirectSoundNotify) && !b->primary)
        out = make_sub(b, &b->inotify, "IDirectSoundNotify", s_notify_vtbl, 4);
    else if (guid_eq(&iid, &IID_IDirectSoundBuffer8) || guid_eq(&iid, &IID_IDirectSoundBuffer)) {
        com_addref_obj(&b->com);
        out = ARG(0);
    }
    if (!out) {
        char gs[40];
        guid_str(&iid, gs);
        LOGW("DirectSoundBuffer QueryInterface(%s): no interface", gs);
        put32(ARG(2), 0);
        RET(3, E_NOINTERFACE);
    }
    put32(ARG(2), out);
    RET(3, S_OK);
}

/* GetCaps(DSBCAPS: dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead) */
static void Buf_GetCaps(CPU *c) {
    SoundBuffer *b = BUF;
    uint32_t p = ARG(1);
    put32(p + 4, b->flags | DSBCAPS_LOCSOFTWARE);
    put32(p + 8, b->data ? b->data->size : 0);
    put32(p + 12, 0);
    put32(p + 16, 0);
    RET(2, DS_OK);
}

/* GetCurrentPosition(pdwCurrentPlayCursor, pdwCurrentWriteCursor) */
static void Buf_GetCurrentPosition(CPU *c) {
    SoundBuffer *b = BUF;
    uint32_t play = 0, write = 0;
    if (b->data) {
        pthread_mutex_lock(&s_lock);
        play = (uint32_t)(b->pos >> 32) * b->block;
        pthread_mutex_unlock(&s_lock);
        /* Write cursor: ~10 ms ahead of play when playing. */
        uint32_t rate = b->freq ? b->freq : b->rate;
        write = b->playing ? play + (rate / 100) * b->block : play;
        if (b->data->size)
            write %= b->data->size;
    }
    if (ARG(1))
        put32(ARG(1), play);
    if (ARG(2))
        put32(ARG(2), write);
    RET(3, DS_OK);
}

static void write_wfx(uint32_t p, uint32_t rate, uint32_t ch, uint32_t bits) {
    uint8_t *h = G2H(p);
    uint16_t v[2];
    v[0] = 1, v[1] = (uint16_t)ch;
    memcpy(h, v, 4);
    put32(p + 4, rate);
    put32(p + 8, rate * ch * bits / 8);
    v[0] = (uint16_t)(ch * bits / 8), v[1] = (uint16_t)bits;
    memcpy(h + 12, v, 4);
}

/* GetFormat(pwfxFormat, dwSizeAllocated, pdwSizeWritten) */
static void Buf_GetFormat(CPU *c) {
    SoundBuffer *b = BUF;
    uint32_t p = ARG(1), sz = ARG(2), written = ARG(3);
    if (p && sz >= 16) {
        write_wfx(p, b->rate, b->channels, b->bits);
        if (sz >= 18)
            memset(G2H(p + 16), 0, 2);
    }
    if (written)
        put32(written, 18);
    RET(4, DS_OK);
}

static void Buf_GetVolume(CPU *c) { put32(ARG(1), (uint32_t)(BUF->primary ? s_master_volume : BUF->volume)); RET(2, DS_OK); }
static void Buf_GetPan(CPU *c) { put32(ARG(1), (uint32_t)BUF->pan); RET(2, DS_OK); }
static void Buf_GetFrequency(CPU *c) {
    SoundBuffer *b = BUF;
    put32(ARG(1), b->freq ? b->freq : b->rate);
    RET(2, DS_OK);
}
static void Buf_GetStatus(CPU *c) {
    SoundBuffer *b = BUF;
    put32(ARG(1), (b->playing ? DSBSTATUS_PLAYING : 0) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0));
    RET(2, DS_OK);
}
static void Buf_Initialize(CPU *c) { RET(3, DSERR_INVALIDCALL); }

/* Lock(dwOffset, dwBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2, pdwAudioBytes2, dwFlags) */
static void Buf_Lock(CPU *c) {
    SoundBuffer *b = BUF;
    uint32_t ofs = ARG(1), bytes = ARG(2), flags = ARG(7);
    if (b->primary || !b->data)
        RET(8, DSERR_INVALIDCALL);
    uint32_t size = b->data->size;
    if (flags & DSBLOCK_FROMWRITECURSOR) {
        uint32_t play = (uint32_t)(b->pos >> 32) * b->block;
        ofs = play;
    }
    if (flags & DSBLOCK_ENTIREBUFFER)
        bytes = size;
    if (ofs >= size || bytes > size || !bytes)
        RET(8, DSERR_INVALIDPARAM);
    uint32_t n1 = bytes, n2 = 0;
    if (ofs + bytes > size) {
        n1 = size - ofs;
        n2 = bytes - n1;
    }
    put32(ARG(3), b->data->mem + ofs);
    put32(ARG(4), n1);
    if (ARG(5))
        put32(ARG(5), n2 ? b->data->mem : 0);
    if (ARG(6))
        put32(ARG(6), n2);
    RET(8, DS_OK);
}

/* Play(dwReserved1, dwPriority, dwFlags) */
static void Buf_Play(CPU *c) {
    SoundBuffer *b = BUF;
    if (!b->primary) {
        pthread_mutex_lock(&s_lock);
        b->playing = true;
        b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
        pthread_mutex_unlock(&s_lock);
    }
    RET(4, DS_OK);
}

static void Buf_SetCurrentPosition(CPU *c) {
    SoundBuffer *b = BUF;
    if (b->data && b->block) {
        pthread_mutex_lock(&s_lock);
        b->pos = (uint64_t)((ARG(1) % b->data->size) / b->block) << 32;
        pthread_mutex_unlock(&s_lock);
    }
    RET(2, DS_OK);
}

static void Buf_SetFormat(CPU *c) {
    SoundBuffer *b = BUF;
    if (!b->primary)
        RET(2, DSERR_INVALIDCALL);
    uint32_t w = ARG(1);
    LOGD("DirectSound primary format: %u Hz, %u ch, %u bit", get32(w + 4),
         *(uint16_t *)G2H(w + 2), *(uint16_t *)G2H(w + 14));
    RET(2, DS_OK);
}

static void Buf_SetVolume(CPU *c) {
    SoundBuffer *b = BUF;
    int32_t v = (int32_t)ARG(1);
    if (v > 0 || v < -10000)
        RET(2, DSERR_INVALIDPARAM);
    if (b->primary)
        s_master_volume = v;
    else
        b->volume = v;
    RET(2, DS_OK);
}

static void Buf_SetPan(CPU *c) {
    int32_t v = (int32_t)ARG(1);
    if (v > 10000 || v < -10000)
        RET(2, DSERR_INVALIDPARAM);
    BUF->pan = v;
    RET(2, DS_OK);
}

static void Buf_SetFrequency(CPU *c) {
    uint32_t f = ARG(1);
    if (f && (f < 100 || f > 200000))
        RET(2, DSERR_INVALIDPARAM);
    BUF->freq = f;
    RET(2, DS_OK);
}

static void Buf_Stop(CPU *c) {
    SoundBuffer *b = BUF;
    pthread_mutex_lock(&s_lock);
    bool was = b->playing;
    b->playing = false;
    if (was && b->nnotify)
        fire_notifications(b, 0, 0, false, true);
    pthread_mutex_unlock(&s_lock);
    RET(1, DS_OK);
}

static void Buf_Unlock(CPU *c) { RET(5, DS_OK); }
static void Buf_Restore(CPU *c) { RET(1, DS_OK); }
static void Buf_SetFX(CPU *c) { RET(4, DSERR_CONTROLUNAVAIL); }
static void Buf_AcquireResources(CPU *c) { RET(4, DS_OK); }
static void Buf_GetObjectInPath(CPU *c) { RET(5, DSERR_CONTROLUNAVAIL); }

static const GuestFn s_buf_vtbl[24] = {
    Buf_QueryInterface, com_AddRef, com_Release, Buf_GetCaps, Buf_GetCurrentPosition,
    Buf_GetFormat, Buf_GetVolume, Buf_GetPan, Buf_GetFrequency, Buf_GetStatus, Buf_Initialize,
    Buf_Lock, Buf_Play, Buf_SetCurrentPosition, Buf_SetFormat, Buf_SetVolume, Buf_SetPan,
    Buf_SetFrequency, Buf_Stop, Buf_Unlock, Buf_Restore, Buf_SetFX, Buf_AcquireResources,
    Buf_GetObjectInPath,
};

/* ------------------------------------------------------- 3D buffer */
#define SUB ((SubIface *)com_get(ARG(0)))
#define B3D (&SUB->buf->p3d)

static void Sub_QueryInterface(CPU *c) {
    /* Same answers as the owning buffer. */
    ARG(0) = SUB->buf->com.guest;
    Buf_QueryInterface(c);
}

static void Sub_AddRef(CPU *c) {
    SoundBuffer *b = SUB->buf;
    com_addref_obj(&b->com);
    RET(1, (uint32_t)b->com.refs);
}

static void Sub_Release(CPU *c) { RET(1, com_release_obj(&SUB->buf->com)); }

/* DS3DBUFFER: dwSize, vPosition, vVelocity, dwInsideConeAngle, dwOutsideConeAngle,
   vConeOrientation, lConeOutsideVolume, flMinDistance, flMaxDistance, dwMode (64 bytes) */
static void B3D_GetAllParameters(CPU *c) {
    Params3D *p = B3D;
    uint32_t a = ARG(1);
    memcpy(G2H(a + 4), &p->pos, 12);
    memcpy(G2H(a + 16), &p->vel, 12);
    put32(a + 28, p->cone_in);
    put32(a + 32, p->cone_out);
    memcpy(G2H(a + 36), &p->cone_dir, 12);
    put32(a + 48, (uint32_t)p->cone_out_vol);
    putf(a + 52, p->min_dist);
    putf(a + 56, p->max_dist);
    put32(a + 60, p->mode);
    RET(2, DS_OK);
}

static void apply3d(SoundBuffer *b, uint32_t apply) {
    if (apply == 0) {   /* DS3D_IMMEDIATE */
        pthread_mutex_lock(&s_lock);
        b->p3d = b->p3d_deferred;
        pthread_mutex_unlock(&s_lock);
    }
}

#define B3D_SET_BEGIN SoundBuffer *b = SUB->buf; Params3D *p = &b->p3d_deferred;

static void B3D_SetAllParameters(CPU *c) {
    B3D_SET_BEGIN
    uint32_t a = ARG(1);
    memcpy(&p->pos, G2H(a + 4), 12);
    memcpy(&p->vel, G2H(a + 16), 12);
    p->cone_in = get32(a + 28);
    p->cone_out = get32(a + 32);
    memcpy(&p->cone_dir, G2H(a + 36), 12);
    p->cone_out_vol = (int32_t)get32(a + 48);
    p->min_dist = getf(a + 52);
    p->max_dist = getf(a + 56);
    p->mode = get32(a + 60);
    apply3d(b, ARG(2));
    RET(3, DS_OK);
}

static void B3D_GetConeAngles(CPU *c) { put32(ARG(1), B3D->cone_in); put32(ARG(2), B3D->cone_out); RET(3, DS_OK); }
static void B3D_GetConeOrientation(CPU *c) { memcpy(G2H(ARG(1)), &B3D->cone_dir, 12); RET(2, DS_OK); }
static void B3D_GetConeOutsideVolume(CPU *c) { put32(ARG(1), (uint32_t)B3D->cone_out_vol); RET(2, DS_OK); }
static void B3D_GetMaxDistance(CPU *c) { putf(ARG(1), B3D->max_dist); RET(2, DS_OK); }
static void B3D_GetMinDistance(CPU *c) { putf(ARG(1), B3D->min_dist); RET(2, DS_OK); }
static void B3D_GetMode(CPU *c) { put32(ARG(1), B3D->mode); RET(2, DS_OK); }
static void B3D_GetPosition(CPU *c) { memcpy(G2H(ARG(1)), &B3D->pos, 12); RET(2, DS_OK); }
static void B3D_GetVelocity(CPU *c) { memcpy(G2H(ARG(1)), &B3D->vel, 12); RET(2, DS_OK); }
static void B3D_SetConeAngles(CPU *c) { B3D_SET_BEGIN p->cone_in = ARG(1); p->cone_out = ARG(2); apply3d(b, ARG(3)); RET(4, DS_OK); }
static void B3D_SetConeOrientation(CPU *c) {
    B3D_SET_BEGIN p->cone_dir = (V3){u2f(ARG(1)), u2f(ARG(2)), u2f(ARG(3))};
    apply3d(b, ARG(4));
    RET(5, DS_OK);
}
static void B3D_SetConeOutsideVolume(CPU *c) { B3D_SET_BEGIN p->cone_out_vol = (int32_t)ARG(1); apply3d(b, ARG(2)); RET(3, DS_OK); }
static void B3D_SetMaxDistance(CPU *c) { B3D_SET_BEGIN p->max_dist = u2f(ARG(1)); apply3d(b, ARG(2)); RET(3, DS_OK); }
static void B3D_SetMinDistance(CPU *c) { B3D_SET_BEGIN p->min_dist = u2f(ARG(1)); apply3d(b, ARG(2)); RET(3, DS_OK); }
static void B3D_SetMode(CPU *c) { B3D_SET_BEGIN p->mode = ARG(1); apply3d(b, ARG(2)); RET(3, DS_OK); }
static void B3D_SetPosition(CPU *c) {
    B3D_SET_BEGIN p->pos = (V3){u2f(ARG(1)), u2f(ARG(2)), u2f(ARG(3))};
    apply3d(b, ARG(4));
    RET(5, DS_OK);
}
static void B3D_SetVelocity(CPU *c) {
    B3D_SET_BEGIN p->vel = (V3){u2f(ARG(1)), u2f(ARG(2)), u2f(ARG(3))};
    apply3d(b, ARG(4));
    RET(5, DS_OK);
}

static const GuestFn s_3d_vtbl[21] = {
    Sub_QueryInterface, Sub_AddRef, Sub_Release, B3D_GetAllParameters, B3D_GetConeAngles,
    B3D_GetConeOrientation, B3D_GetConeOutsideVolume, B3D_GetMaxDistance, B3D_GetMinDistance,
    B3D_GetMode, B3D_GetPosition, B3D_GetVelocity, B3D_SetAllParameters, B3D_SetConeAngles,
    B3D_SetConeOrientation, B3D_SetConeOutsideVolume, B3D_SetMaxDistance, B3D_SetMinDistance,
    B3D_SetMode, B3D_SetPosition, B3D_SetVelocity,
};

/* ------------------------------------------------------------ notify */
/* SetNotificationPositions(dwPositionNotifies, pcPositionNotifies) */
static void Notify_SetNotificationPositions(CPU *c) {
    SoundBuffer *b = SUB->buf;
    uint32_t n = ARG(1), arr = ARG(2);
    if (n > MAX_NOTIFY)
        RET(3, DSERR_INVALIDPARAM);
    pthread_mutex_lock(&s_lock);
    b->nnotify = (int)n;
    for (uint32_t i = 0; i < n; i++) {
        b->notify[i].ofs = get32(arr + 8 * i);
        b->notify[i].event = get32(arr + 8 * i + 4);
    }
    pthread_mutex_unlock(&s_lock);
    RET(3, DS_OK);
}

static const GuestFn s_notify_vtbl[4] = {
    Sub_QueryInterface, Sub_AddRef, Sub_Release, Notify_SetNotificationPositions,
};

/* ----------------------------------------------------------- listener */
static Listener *LST(void) { return &s_listener_deferred; }

static void lst_apply(uint32_t apply) {
    if (apply == 0) {
        pthread_mutex_lock(&s_lock);
        s_listener = s_listener_deferred;
        pthread_mutex_unlock(&s_lock);
    }
}

/* DS3DLISTENER: dwSize, vPosition, vVelocity, vOrientFront, vOrientTop,
   flDistanceFactor, flRolloffFactor, flDopplerFactor (64 bytes) */
static void L_GetAllParameters(CPU *c) {
    uint32_t a = ARG(1);
    memcpy(G2H(a + 4), &s_listener.pos, 12);
    memcpy(G2H(a + 16), &s_listener.vel, 12);
    memcpy(G2H(a + 28), &s_listener.front, 12);
    memcpy(G2H(a + 40), &s_listener.top, 12);
    putf(a + 52, s_listener.dist_factor);
    putf(a + 56, s_listener.rolloff);
    putf(a + 60, s_listener.doppler);
    RET(2, DS_OK);
}
static void L_GetDistanceFactor(CPU *c) { putf(ARG(1), s_listener.dist_factor); RET(2, DS_OK); }
static void L_GetDopplerFactor(CPU *c) { putf(ARG(1), s_listener.doppler); RET(2, DS_OK); }
static void L_GetOrientation(CPU *c) {
    memcpy(G2H(ARG(1)), &s_listener.front, 12);
    memcpy(G2H(ARG(2)), &s_listener.top, 12);
    RET(3, DS_OK);
}
static void L_GetPosition(CPU *c) { memcpy(G2H(ARG(1)), &s_listener.pos, 12); RET(2, DS_OK); }
static void L_GetRolloffFactor(CPU *c) { putf(ARG(1), s_listener.rolloff); RET(2, DS_OK); }
static void L_GetVelocity(CPU *c) { memcpy(G2H(ARG(1)), &s_listener.vel, 12); RET(2, DS_OK); }
static void L_SetAllParameters(CPU *c) {
    uint32_t a = ARG(1);
    Listener *l = LST();
    memcpy(&l->pos, G2H(a + 4), 12);
    memcpy(&l->vel, G2H(a + 16), 12);
    memcpy(&l->front, G2H(a + 28), 12);
    memcpy(&l->top, G2H(a + 40), 12);
    l->dist_factor = getf(a + 52);
    l->rolloff = getf(a + 56);
    l->doppler = getf(a + 60);
    lst_apply(ARG(2));
    RET(3, DS_OK);
}
static void L_SetDistanceFactor(CPU *c) { LST()->dist_factor = u2f(ARG(1)); lst_apply(ARG(2)); RET(3, DS_OK); }
static void L_SetDopplerFactor(CPU *c) { LST()->doppler = u2f(ARG(1)); lst_apply(ARG(2)); RET(3, DS_OK); }
static void L_SetOrientation(CPU *c) {
    LST()->front = (V3){u2f(ARG(1)), u2f(ARG(2)), u2f(ARG(3))};
    LST()->top = (V3){u2f(ARG(4)), u2f(ARG(5)), u2f(ARG(6))};
    lst_apply(ARG(7));
    RET(8, DS_OK);
}
static void L_SetPosition(CPU *c) {
    LST()->pos = (V3){u2f(ARG(1)), u2f(ARG(2)), u2f(ARG(3))};
    lst_apply(ARG(4));
    RET(5, DS_OK);
}
static void L_SetRolloffFactor(CPU *c) { LST()->rolloff = u2f(ARG(1)); lst_apply(ARG(2)); RET(3, DS_OK); }
static void L_SetVelocity(CPU *c) {
    LST()->vel = (V3){u2f(ARG(1)), u2f(ARG(2)), u2f(ARG(3))};
    lst_apply(ARG(4));
    RET(5, DS_OK);
}
static void L_CommitDeferredSettings(CPU *c) {
    pthread_mutex_lock(&s_lock);
    s_listener = s_listener_deferred;
    for (int i = 0; i < s_nbufs; i++)
        s_bufs[i]->p3d = s_bufs[i]->p3d_deferred;
    pthread_mutex_unlock(&s_lock);
    RET(1, DS_OK);
}

static const GuestFn s_listener_vtbl[18] = {
    Sub_QueryInterface, Sub_AddRef, Sub_Release, L_GetAllParameters, L_GetDistanceFactor,
    L_GetDopplerFactor, L_GetOrientation, L_GetPosition, L_GetRolloffFactor, L_GetVelocity,
    L_SetAllParameters, L_SetDistanceFactor, L_SetDopplerFactor, L_SetOrientation, L_SetPosition,
    L_SetRolloffFactor, L_SetVelocity, L_CommitDeferredSettings,
};

/* ------------------------------------------------------- IDirectSound8 */
static void DS_QueryInterface(CPU *c) {
    com_addref_obj(com_get(ARG(0)));
    put32(ARG(2), ARG(0));
    RET(3, S_OK);
}

/* CreateSoundBuffer(pcDSBufferDesc, ppDSBuffer, pUnkOuter)
   DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, dwReserved, lpwfxFormat, guid3DAlgorithm */
static void DS_CreateSoundBuffer(CPU *c) {
    uint32_t desc = ARG(1);
    uint32_t flags = get32(desc + 4), bytes = get32(desc + 8), wfx = get32(desc + 16);
    SoundBuffer *b = calloc(1, sizeof *b);
    b->flags = flags;
    params3d_default(&b->p3d);
    b->p3d_deferred = b->p3d;
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        b->primary = true;
        b->rate = OUT_RATE;
        b->channels = 2;
        b->bits = 16;
        b->block = 4;
    } else {
        if (!wfx)
            goto bad;
        uint16_t tag = *(uint16_t *)G2H(wfx), ch = *(uint16_t *)G2H(wfx + 2);
        uint16_t bits = *(uint16_t *)G2H(wfx + 14);
        b->rate = get32(wfx + 4);
        b->channels = ch;
        b->bits = bits;
        b->block = ch * bits / 8u;
        if (tag != 1 || (bits != 8 && bits != 16) || ch < 1 || ch > 2 || !b->rate) {
            LOGW("DirectSound: unsupported buffer format tag=%u %u ch %u bit %u Hz", tag, ch, bits, b->rate);
            free(b);
            put32(ARG(2), 0);
            RET(4, DSERR_BADFORMAT);
        }
        if (bytes < 4 || bytes > 0x4000000)
            goto bad;
        bytes -= bytes % b->block;
        b->data = calloc(1, sizeof *b->data);
        b->data->mem = gcalloc(bytes);
        b->data->size = bytes;
        b->data->refs = 1;
        /* 8-bit silence is 0x80 */
        if (bits == 8)
            memset(G2H(b->data->mem), 0x80, bytes);
    }
    uint32_t g = buf_new(b);
    LOGD("CreateSoundBuffer(flags=%x, %u bytes, %u Hz %u ch %u bit) -> %08x", flags, bytes, b->rate,
         b->channels, b->bits, g);
    put32(ARG(2), g);
    RET(4, DS_OK);
bad:
    free(b);
    put32(ARG(2), 0);
    RET(4, DSERR_INVALIDPARAM);
}

/* DSCAPS (96 bytes) */
static void DS_GetCaps(CPU *c) {
    uint32_t p = ARG(1);
    memset(G2H(p + 4), 0, 92);
    put32(p + 4, 0x0f00u | 0x1fu);       /* DSCAPS_SECONDARY*, PRIMARY*, CONTINUOUSRATE */
    put32(p + 8, 100);                   /* dwMinSecondarySampleRate */
    put32(p + 12, 200000);               /* dwMaxSecondarySampleRate */
    put32(p + 16, 1);                    /* dwPrimaryBuffers */
    for (int i = 0; i < 9; i++)          /* max/free hw mixing and 3D buffer counts */
        put32(p + 20 + 4 * (uint32_t)i, 64);
    RET(2, DS_OK);
}

/* DuplicateSoundBuffer(pDSBufferOriginal, ppDSBufferDuplicate) */
static void DS_DuplicateSoundBuffer(CPU *c) {
    SoundBuffer *o = (SoundBuffer *)com_get(ARG(1));
    if (!o || o->primary)
        RET(3, DSERR_INVALIDCALL);
    SoundBuffer *b = calloc(1, sizeof *b);
    b->flags = o->flags;
    b->data = o->data;
    b->data->refs++;
    b->rate = o->rate;
    b->channels = o->channels;
    b->bits = o->bits;
    b->block = o->block;
    b->freq = o->freq;
    b->volume = o->volume;
    b->pan = o->pan;
    b->p3d = o->p3d;
    b->p3d_deferred = o->p3d_deferred;
    put32(ARG(2), buf_new(b));
    RET(3, DS_OK);
}

static void DS_SetCooperativeLevel(CPU *c) { RET(3, DS_OK); }
static void DS_Compact(CPU *c) { RET(1, DS_OK); }
static void DS_GetSpeakerConfig(CPU *c) { put32(ARG(1), 4u /* DSSPEAKER_STEREO */); RET(2, DS_OK); }
static void DS_SetSpeakerConfig(CPU *c) { RET(2, DS_OK); }
static void DS_Initialize(CPU *c) { RET(2, DS_OK); }
static void DS_VerifyCertification(CPU *c) { put32(ARG(1), 0); RET(2, DS_OK); }

static const GuestFn s_ds_vtbl[] = {
    DS_QueryInterface, com_AddRef, com_Release, DS_CreateSoundBuffer, DS_GetCaps,
    DS_DuplicateSoundBuffer, DS_SetCooperativeLevel, DS_Compact, DS_GetSpeakerConfig,
    DS_SetSpeakerConfig, DS_Initialize, DS_VerifyCertification,
};

/* DirectSoundCreate8(lpcGuidDevice, ppDS8, pUnkOuter) */
void imp_DSOUND_ord11(CPU *c) {
    if (!audio_open()) {
        put32(ARG(1), 0);
        RET(3, DSERR_NODRIVER);
    }
    s_listener_deferred = s_listener;
    DSound *ds = calloc(1, sizeof *ds);
    ds->com.type = "IDirectSound8";
    uint32_t vt = com_vtbl("IDirectSound8", s_ds_vtbl, (int)(sizeof s_ds_vtbl / sizeof *s_ds_vtbl));
    put32(ARG(1), com_wrap(&ds->com, vt, 0));
    LOGI("DirectSoundCreate8");
    RET(3, DS_OK);
}

COM_VTABLE("IDirectSound8", s_ds_vtbl)
COM_VTABLE("IDirectSoundBuffer8", s_buf_vtbl)
COM_VTABLE("IDirectSound3DBuffer8", s_3d_vtbl)
COM_VTABLE("IDirectSoundNotify", s_notify_vtbl)
COM_VTABLE("IDirectSound3DListener8", s_listener_vtbl)

/* -------------------------------------------------------------- savestates
 * Sample data is in guest memory; duplicates share a SoundData, found again
 * on load by its guest address.  The snapshot code holds s_lock (SnapLock)
 * throughout, so nothing here locks it. */
static void ds_snap_save(ComObj *o, SnapBuf *b) { (void)o, (void)b; }
static ComObj *ds_snap_load(SnapBuf *b) {
    (void)b;
    if (!audio_open())
        LOGW("savestate: no audio device, sound stays silent");
    return calloc(1, sizeof(DSound));
}

static void sbuf_snap_save(ComObj *o, SnapBuf *b) {
    SoundBuffer *s = (SoundBuffer *)o;
    sb_u32(b, s->flags);
    sb_u32(b, s->primary);
    sb_u32(b, s->data ? s->data->mem : 0);
    sb_u32(b, s->data ? s->data->size : 0);
    uint32_t v[] = {s->rate, s->channels, s->bits, s->block, s->playing, s->looping, s->freq,
                    (uint32_t)s->volume, (uint32_t)s->pan};
    sb_put(b, v, sizeof v);
    SB_PUT(b, s->pos);
    SB_PUT(b, s->p3d);
    SB_PUT(b, s->p3d_deferred);
    sb_u32(b, (uint32_t)s->nnotify);
    sb_put(b, s->notify, (size_t)s->nnotify * sizeof *s->notify);
    sb_u32(b, s->i3d ? s->i3d->com.guest : 0);
    sb_u32(b, s->inotify ? s->inotify->com.guest : 0);
}

static ComObj *sbuf_snap_load(SnapBuf *b) {
    SoundBuffer *s = calloc(1, sizeof *s);
    s->com.destroy = buf_destroy;
    s->flags = sb_get_u32(b);
    s->primary = sb_get_u32(b) != 0;
    uint32_t mem = sb_get_u32(b), size = sb_get_u32(b);
    uint32_t v[9];
    sb_get(b, v, sizeof v);
    s->rate = v[0];
    s->channels = v[1];
    s->bits = v[2];
    s->block = v[3];
    s->playing = v[4] != 0;
    s->looping = v[5] != 0;
    s->freq = v[6];
    s->volume = (int32_t)v[7];
    s->pan = (int32_t)v[8];
    SB_GET(b, s->pos);
    SB_GET(b, s->p3d);
    SB_GET(b, s->p3d_deferred);
    s->nnotify = (int)sb_get_u32(b);
    if (s->nnotify < 0 || s->nnotify > MAX_NOTIFY) {
        free(s);
        return NULL;
    }
    sb_get(b, s->notify, (size_t)s->nnotify * sizeof *s->notify);
    s->i3d = (SubIface *)(uintptr_t)sb_get_u32(b);
    s->inotify = (SubIface *)(uintptr_t)sb_get_u32(b);
    if (mem) {
        for (int i = 0; i < s_nbufs && !s->data; i++)
            if (s_bufs[i]->data && s_bufs[i]->data->mem == mem)
                s->data = s_bufs[i]->data;
        if (!s->data) {
            s->data = calloc(1, sizeof *s->data);
            s->data->mem = mem;
            s->data->size = size;
        }
        s->data->refs++;
    }
    if (s_nbufs < (int)(sizeof s_bufs / sizeof *s_bufs))
        s_bufs[s_nbufs++] = s;
    return &s->com;
}

static void sbuf_snap_link(ComObj *o) {
    SoundBuffer *s = (SoundBuffer *)o;
    COM_LINK(s->i3d, SubIface);
    COM_LINK(s->inotify, SubIface);
}

static void sbuf_snap_drop(ComObj *o) {
    SoundBuffer *s = (SoundBuffer *)o;
    if (s->data && --s->data->refs == 0)
        free(s->data);
    free(s);
}

static void sub_snap_save(ComObj *o, SnapBuf *b) { sb_u32(b, ((SubIface *)o)->buf->com.guest); }
static ComObj *sub_snap_load(SnapBuf *b) {
    SubIface *s = calloc(1, sizeof *s);
    s->com.destroy = sub_destroy;
    s->buf = (SoundBuffer *)(uintptr_t)sb_get_u32(b);
    return &s->com;
}
static void sub_snap_link(ComObj *o) { COM_LINK(((SubIface *)o)->buf, SoundBuffer); }

static const ComSnapClass s_ds_classes[] = {
    {"IDirectSound8", ds_snap_save, ds_snap_load, NULL, NULL},
    {"IDirectSoundBuffer8", sbuf_snap_save, sbuf_snap_load, sbuf_snap_link, sbuf_snap_drop},
    {"IDirectSound3DBuffer8", sub_snap_save, sub_snap_load, sub_snap_link, NULL},
    {"IDirectSoundNotify", sub_snap_save, sub_snap_load, sub_snap_link, NULL},
    {"IDirectSound3DListener8", sub_snap_save, sub_snap_load, sub_snap_link, NULL},
};

/* DSND: listener and master volume.  Its drop runs before the COM drop
   (drops go in reverse order), so the mixer list is empty by the time
   buffers are freed and rebuilt. */
static void dsnd_save(SnapBuf *b) {
    SB_PUT(b, s_listener);
    SB_PUT(b, s_listener_deferred);
    SB_PUT(b, s_master_volume);
}

static void dsnd_drop(void) { s_nbufs = 0; }

static bool dsnd_load(SnapBuf *b) {
    SB_GET(b, s_listener);
    SB_GET(b, s_listener_deferred);
    SB_GET(b, s_master_volume);
    return !b->err;
}

static const SnapSection s_dsnd_section = {
    SNAP_TAG('D', 'S', 'N', 'D'), SNAP_ORDER_LATE, dsnd_save, NULL, dsnd_drop, dsnd_load,
};

static void dsnd_lock(void) { pthread_mutex_lock(&s_lock); }
static void dsnd_unlock(void) { pthread_mutex_unlock(&s_lock); }
static const SnapLock s_dsnd_lock = {dsnd_lock, dsnd_unlock};

__attribute__((constructor)) static void dsound_snap_register(void) {
    for (size_t i = 0; i < sizeof s_ds_classes / sizeof *s_ds_classes; i++)
        com_snap_class(&s_ds_classes[i]);
    snap_section(&s_dsnd_section);
    snap_add_lock(&s_dsnd_lock);
}
