#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define OUT_RATE 44100
#define MAX_SOURCES 256
#define MAX_BUFFERS 4096
#define MAX_QUEUE 64

enum {
    AL_SOURCE_RELATIVE = 0x202, AL_PITCH = 0x1003, AL_POSITION = 0x1004, AL_DIRECTION = 0x1005, AL_VELOCITY = 0x1006,
    AL_LOOPING = 0x1007, AL_BUFFER = 0x1009, AL_GAIN = 0x100A, AL_MIN_GAIN = 0x100D, AL_MAX_GAIN = 0x100E,
    AL_ORIENTATION = 0x100F, AL_SOURCE_STATE = 0x1010, AL_INITIAL = 0x1011, AL_PLAYING = 0x1012, AL_PAUSED = 0x1013,
    AL_STOPPED = 0x1014, AL_BUFFERS_QUEUED = 0x1015, AL_BUFFERS_PROCESSED = 0x1016, AL_REFERENCE_DISTANCE = 0x1020,
    AL_ROLLOFF_FACTOR = 0x1021, AL_MAX_DISTANCE = 0x1023, AL_SEC_OFFSET = 0x1024, AL_SAMPLE_OFFSET = 0x1025,
    AL_SOURCE_TYPE = 0x1027, AL_FORMAT_MONO8 = 0x1100, AL_FORMAT_MONO16 = 0x1101, AL_FORMAT_STEREO8 = 0x1102,
    AL_FORMAT_STEREO16 = 0x1103, AL_INVERSE_DISTANCE = 0xD001, AL_INVERSE_DISTANCE_CLAMPED = 0xD002,
    AL_LINEAR_DISTANCE = 0xD003, AL_LINEAR_DISTANCE_CLAMPED = 0xD004, AL_EXPONENT_DISTANCE = 0xD005,
    AL_EXPONENT_DISTANCE_CLAMPED = 0xD006, AL_STATIC = 0x1028, AL_STREAMING = 0x1029, AL_UNDETERMINED = 0x1030
};

typedef struct { int used, chans, rate; u32 frames; s16 *pcm; } Buffer;
typedef struct {
    int used, state, looping, relative;
    float gain, pitch, pos[3], ref, max, rolloff, min_gain, max_gain;
    u32 queue[MAX_QUEUE];
    int nq, cur, processed;
    double at;
} Source;

static Buffer buffers[MAX_BUFFERS];
static Source sources[MAX_SOURCES];
static float lis_pos[3], lis_at[3] = {0, 0, -1}, lis_up[3] = {0, 1, 0}, lis_gain = 1;
static u32 dist_model = AL_INVERSE_DISTANCE_CLAMPED;
static SDL_Mutex *mx;
static SDL_AudioStream *stream;
static int suspended;
static u32 device_cell = 0xA1D0, context_cell = 0xA1C0, current_ctx;

#define LOCK() SDL_LockMutex(mx)
#define UNLOCK() SDL_UnlockMutex(mx)

static Source *src(u32 id) { return id && id <= MAX_SOURCES && sources[id - 1].used ? &sources[id - 1] : NULL; }
static Buffer *buf(u32 id) { return id && id <= MAX_BUFFERS && buffers[id - 1].used ? &buffers[id - 1] : NULL; }

static float attenuation(Source *s, float *pan)
{
    *pan = 0;
    float d[3];
    for (int i = 0; i < 3; i++)
        d[i] = s->relative ? s->pos[i] : s->pos[i] - lis_pos[i];
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist > 1e-4f) {
        float r[3] = {lis_at[1] * lis_up[2] - lis_at[2] * lis_up[1], lis_at[2] * lis_up[0] - lis_at[0] * lis_up[2],
                      lis_at[0] * lis_up[1] - lis_at[1] * lis_up[0]};
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (rl > 1e-6f && !s->relative)
            *pan = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (rl * dist);
        else if (s->relative)
            *pan = d[0] / dist;
    }
    float ref = s->ref, mx_ = s->max, ro = s->rolloff, g = 1;
    switch (dist_model) {
    case AL_INVERSE_DISTANCE_CLAMPED:
        if (dist < ref) dist = ref;
        if (dist > mx_) dist = mx_;
        /* fall through */
    case AL_INVERSE_DISTANCE:
        g = ref + ro * (dist - ref) > 0 ? ref / (ref + ro * (dist - ref)) : 1;
        break;
    case AL_LINEAR_DISTANCE_CLAMPED:
        if (dist < ref) dist = ref;
        if (dist > mx_) dist = mx_;
        /* fall through */
    case AL_LINEAR_DISTANCE:
        g = mx_ > ref ? 1 - ro * (dist - ref) / (mx_ - ref) : 1;
        break;
    case AL_EXPONENT_DISTANCE_CLAMPED:
        if (dist < ref) dist = ref;
        if (dist > mx_) dist = mx_;
        /* fall through */
    case AL_EXPONENT_DISTANCE:
        g = dist > 0 && ref > 0 ? powf(dist / ref, -ro) : 1;
        break;
    default:
        g = 1;
    }
    if (g < s->min_gain) g = s->min_gain;
    if (g > s->max_gain) g = s->max_gain;
    return g < 0 ? 0 : g;
}

static void mix_source(Source *s, float *out, int n)
{
    float pan, g = s->gain * lis_gain;
    float att = attenuation(s, &pan);
    for (int i = 0; i < n;) {
        if (s->cur >= s->nq) {
            s->state = AL_STOPPED;
            return;
        }
        Buffer *b = buf(s->queue[s->cur]);
        if (!b || !b->frames) {
            s->cur++;
            s->processed++;
            s->at = 0;
            continue;
        }
        double step = (double)b->rate * (s->pitch > 0 ? s->pitch : 1) / OUT_RATE;
        float gl, gr;
        if (b->chans == 1) {
            float a = g * att;
            gl = a * sqrtf(0.5f * (1 - pan));
            gr = a * sqrtf(0.5f * (1 + pan));
            gl *= 1.41421356f;
            gr *= 1.41421356f;
            if (gl > a) gl = a;
            if (gr > a) gr = a;
        } else {
            gl = gr = g;
        }
        const s16 *p = b->pcm;
        u32 nf = b->frames;
        for (; i < n; i++) {
            u32 k = (u32)s->at;
            if (k >= nf)
                break;
            float f = (float)(s->at - k);
            u32 k1 = k + 1 < nf ? k + 1 : (s->looping && s->nq == 1 ? 0 : k);
            if (b->chans == 1) {
                float v = (p[k] + (p[k1] - p[k]) * f) * (1 / 32768.0f);
                out[2 * i] += v * gl;
                out[2 * i + 1] += v * gr;
            } else {
                float l = (p[2 * k] + (p[2 * k1] - p[2 * k]) * f) * (1 / 32768.0f);
                float r = (p[2 * k + 1] + (p[2 * k1 + 1] - p[2 * k + 1]) * f) * (1 / 32768.0f);
                out[2 * i] += l * gl;
                out[2 * i + 1] += r * gr;
            }
            s->at += step;
        }
        if ((u32)s->at >= nf) {
            s->at -= nf;
            if (s->looping && s->nq == 1)
                continue;
            s->cur++;
            s->processed++;
            if (s->cur >= s->nq) {
                if (s->looping) {
                    s->cur = 0;
                    s->processed = 0;
                } else {
                    s->state = AL_STOPPED;
                    s->at = 0;
                    return;
                }
            }
        }
    }
}

#define MIX_FRAMES 4096
static void SDLCALL feed(void *user, SDL_AudioStream *st, int additional, int total)
{
    (void)user;
    (void)total;
    static float mixbuf[MIX_FRAMES * 2];
    while (additional > 0) {
        int n = additional / 8;
        if (n > MIX_FRAMES)
            n = MIX_FRAMES;
        if (n <= 0)
            break;
        memset(mixbuf, 0, (size_t)n * 8);
        LOCK();
        if (!suspended)
            for (int i = 0; i < MAX_SOURCES; i++)
                if (sources[i].used && sources[i].state == AL_PLAYING)
                    mix_source(&sources[i], mixbuf, n);
        UNLOCK();
        for (int i = 0; i < 2 * n; i++)
            mixbuf[i] = mixbuf[i] > 1 ? 1 : mixbuf[i] < -1 ? -1 : mixbuf[i];
        SDL_PutAudioStreamData(st, mixbuf, n * 8);
        static FILE *wav;
        static u32 wav_frames;
        static int wav_tried;
        if (!wav_tried) {
            wav_tried = 1;
            const char *p = dev_getenv("GT_WAVDUMP");
            if (p && (wav = fopen(p, "wb")))
                fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data\0\0\0\0", 1, 44, wav);
        }
        if (wav && wav_frames < OUT_RATE * 60) {
            s16 pcm[MIX_FRAMES * 2];
            for (int i = 0; i < 2 * n; i++)
                pcm[i] = (s16)(mixbuf[i] * 32767);
            fwrite(pcm, 4, (size_t)n, wav);
            wav_frames += (u32)n;
            u32 data = wav_frames * 4, riff = data + 36;
            long at = ftell(wav);
            fseek(wav, 4, SEEK_SET);
            fwrite(&riff, 4, 1, wav);
            fseek(wav, 40, SEEK_SET);
            fwrite(&data, 4, 1, wav);
            fseek(wav, at, SEEK_SET);
            fflush(wav);
        }
        additional -= n * 8;
    }
}

static void start_audio(void)
{
    if (stream)
        return;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, OUT_RATE};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!stream) {
        rt_log("[al] no audio device: %s\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream);
    rt_log("[al] audio out: %d Hz stereo\n", OUT_RATE);
}

void al_stop_all(void)
{
    if (stream)
        SDL_PauseAudioStreamDevice(stream);
}

u32 h_alcOpenDevice(u32 name)
{
    (void)name;
    start_audio();
    return GA(&device_cell);
}
u32 h_alcCloseDevice(u32 dev) { (void)dev; return 1; }
u32 h_alcCreateContext(u32 dev, u32 attrs) { (void)dev, (void)attrs; return GA(&context_cell); }
void h_alcDestroyContext(u32 ctx) { (void)ctx; }
u32 h_alcMakeContextCurrent(u32 ctx) { current_ctx = ctx; return 1; }
u32 h_alcGetCurrentContext(void) { return current_ctx; }
u32 h_alcGetContextsDevice(u32 ctx) { (void)ctx; return GA(&device_cell); }
void h_alcProcessContext(u32 ctx) { (void)ctx; suspended = 0; }
void h_alcSuspendContext(u32 ctx) { (void)ctx; suspended = 1; }

void h_alBufferData(u32 id, u32 format, u32 data, u32 size, u32 freq);
static void I_alBufferDataStatic(CPU *c)
{
    h_alBufferData(c->r[0], c->r[1], c->r[2], c->r[3], arg_word(c, 4));
}
u32 h_alcGetProcAddress(u32 dev, u32 name)
{
    (void)dev;
    const char *n = GP(name);
    if (!strcmp(n, "alBufferDataStatic"))
        return rt_host_addr(I_alBufferDataStatic, "alBufferDataStatic");
    rt_log("[al] alcGetProcAddress(%s): none\n", n);
    return 0;
}

void h_alGenBuffers(u32 n, u32 out)
{
    LOCK();
    for (u32 k = 0, i = 0; k < n; k++) {
        while (i < MAX_BUFFERS && buffers[i].used)
            i++;
        if (i == MAX_BUFFERS)
            rt_fatal("OpenAL: more than %d buffers", MAX_BUFFERS);
        memset(&buffers[i], 0, sizeof buffers[i]);
        buffers[i].used = 1;
        wr32(out + 4 * k, i + 1);
    }
    UNLOCK();
}
void h_alDeleteBuffers(u32 n, u32 ids)
{
    LOCK();
    for (u32 k = 0; k < n; k++) {
        Buffer *b = buf(rd32(ids + 4 * k));
        if (b) {
            free(b->pcm);
            memset(b, 0, sizeof *b);
        }
    }
    UNLOCK();
}
void h_alBufferData(u32 id, u32 format, u32 data, u32 size, u32 freq)
{
    int chans = format == AL_FORMAT_STEREO8 || format == AL_FORMAT_STEREO16 ? 2 : 1;
    int bits = format == AL_FORMAT_MONO8 || format == AL_FORMAT_STEREO8 ? 8 : 16;
    u32 frames = size / (u32)(chans * bits / 8);
    static u32 seen[16];
    static int nseen;
    u32 key = freq << 2 | (u32)chans << 1 | (bits == 16);
    int known = 0;
    for (int i = 0; i < nseen; i++)
        known |= seen[i] == key;
    if (!known && nseen < 16) {
        seen[nseen++] = key;
        rt_log("[al] buffers at %u Hz, %d channel(s), %d bit\n", freq, chans, bits);
    }
    s16 *pcm = malloc((size_t)frames * chans * 2 + 4);
    if (bits == 16)
        memcpy(pcm, GP(data), (size_t)frames * chans * 2);
    else
        for (u32 i = 0; i < frames * (u32)chans; i++)
            pcm[i] = (s16)((((const u8 *)GP(data))[i] - 128) << 8);
    LOCK();
    Buffer *b = buf(id);
    if (b) {
        free(b->pcm);
        b->pcm = pcm;
        b->chans = chans;
        b->rate = (int)freq;
        b->frames = frames;
    } else {
        free(pcm);
    }
    UNLOCK();
}

void h_alGenSources(u32 n, u32 out)
{
    LOCK();
    for (u32 k = 0, i = 0; k < n; k++) {
        while (i < MAX_SOURCES && sources[i].used)
            i++;
        if (i == MAX_SOURCES)
            rt_fatal("OpenAL: more than %d sources", MAX_SOURCES);
        Source *s = &sources[i];
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->state = AL_INITIAL;
        s->gain = s->pitch = s->ref = s->rolloff = s->max_gain = 1;
        s->max = 3.4e38f;
        wr32(out + 4 * k, i + 1);
    }
    UNLOCK();
}
void h_alDeleteSources(u32 n, u32 ids)
{
    LOCK();
    for (u32 k = 0; k < n; k++) {
        Source *s = src(rd32(ids + 4 * k));
        if (s)
            memset(s, 0, sizeof *s);
    }
    UNLOCK();
}
static void rewind_src(Source *s)
{
    s->cur = 0;
    s->processed = 0;
    s->at = 0;
}
void h_alSourcePlay(u32 id)
{
    static int first = 1;
    if (first) {
        first = 0;
        Source *s = src(id);
        Buffer *b = s && s->nq ? buf(s->queue[0]) : NULL;
        rt_log("[al] first sound: source %u, %d queued, %s %d Hz %u frames\n", id, s ? s->nq : 0,
               b ? (b->chans == 2 ? "stereo" : "mono") : "-", b ? b->rate : 0, b ? b->frames : 0);
    }
    LOCK();
    Source *s = src(id);
    if (s) {
        if (s->state != AL_PAUSED)
            rewind_src(s);
        s->state = s->nq ? AL_PLAYING : AL_STOPPED;
    }
    UNLOCK();
}
void h_alSourcePause(u32 id)
{
    LOCK();
    Source *s = src(id);
    if (s && s->state == AL_PLAYING)
        s->state = AL_PAUSED;
    UNLOCK();
}
void h_alSourceStop(u32 id)
{
    LOCK();
    Source *s = src(id);
    if (s && s->state != AL_INITIAL) {
        s->state = AL_STOPPED;
        s->cur = s->nq;
        s->processed = s->nq;
        s->at = 0;
    }
    UNLOCK();
}
void h_alSourceQueueBuffers(u32 id, u32 n, u32 ids)
{
    LOCK();
    Source *s = src(id);
    for (u32 k = 0; s && k < n && s->nq < MAX_QUEUE; k++)
        s->queue[s->nq++] = rd32(ids + 4 * k);
    UNLOCK();
}
void h_alSourceUnqueueBuffers(u32 id, u32 n, u32 ids)
{
    LOCK();
    Source *s = src(id);
    if (s) {
        if ((int)n > s->processed)
            n = (u32)s->processed;
        for (u32 k = 0; k < n; k++)
            wr32(ids + 4 * k, s->queue[k]);
        memmove(s->queue, s->queue + n, (size_t)(s->nq - (int)n) * 4);
        s->nq -= (int)n;
        s->cur -= (int)n;
        s->processed -= (int)n;
        if (s->cur < 0)
            s->cur = 0;
    }
    UNLOCK();
}
void h_alSourcei(u32 id, u32 param, u32 v)
{
    LOCK();
    Source *s = src(id);
    if (s)
        switch (param) {
        case AL_BUFFER:
            s->nq = 0;
            if (v)
                s->queue[s->nq++] = v;
            rewind_src(s);
            s->state = AL_INITIAL;
            break;
        case AL_LOOPING:
            s->looping = v != 0;
            break;
        case AL_SOURCE_RELATIVE:
            s->relative = v != 0;
            break;
        case AL_SAMPLE_OFFSET:
            s->at = v;
            break;
        case AL_REFERENCE_DISTANCE:
            s->ref = (float)v;
            break;
        case AL_MAX_DISTANCE:
            s->max = (float)v;
            break;
        case AL_ROLLOFF_FACTOR:
            s->rolloff = (float)v;
            break;
        default:
            break;
        }
    UNLOCK();
}
void h_alSourcef(u32 id, u32 param, float v)
{
    LOCK();
    Source *s = src(id);
    if (s)
        switch (param) {
        case AL_GAIN: s->gain = v; break;
        case AL_PITCH: s->pitch = v; break;
        case AL_REFERENCE_DISTANCE: s->ref = v; break;
        case AL_MAX_DISTANCE: s->max = v; break;
        case AL_ROLLOFF_FACTOR: s->rolloff = v; break;
        case AL_MIN_GAIN: s->min_gain = v; break;
        case AL_MAX_GAIN: s->max_gain = v; break;
        case AL_SEC_OFFSET: {
            Buffer *b = s->nq ? buf(s->queue[s->cur < s->nq ? s->cur : 0]) : NULL;
            s->at = b ? v * b->rate : 0;
            break;
        }
        default: break;
        }
    UNLOCK();
}
void h_alSource3f(u32 id, u32 param, float x, float y, float z)
{
    LOCK();
    Source *s = src(id);
    if (s && param == AL_POSITION) {
        s->pos[0] = x;
        s->pos[1] = y;
        s->pos[2] = z;
    }
    UNLOCK();
}
void h_alSourcefv(u32 id, u32 param, u32 v)
{
    const float *f = GP(v);
    if (param == AL_POSITION || param == AL_VELOCITY || param == AL_DIRECTION)
        h_alSource3f(id, param, f[0], f[1], f[2]);
    else
        h_alSourcef(id, param, f[0]);
}
void h_alGetSourcei(u32 id, u32 param, u32 out)
{
    LOCK();
    Source *s = src(id);
    u32 v = 0;
    if (s)
        switch (param) {
        case AL_SOURCE_STATE: v = (u32)s->state; break;
        case AL_BUFFERS_QUEUED: v = (u32)s->nq; break;
        case AL_BUFFERS_PROCESSED: v = s->state == AL_INITIAL ? 0 : (u32)s->processed; break;
        case AL_BUFFER: v = s->nq ? s->queue[s->cur < s->nq ? s->cur : s->nq - 1] : 0; break;
        case AL_LOOPING: v = (u32)s->looping; break;
        case AL_SOURCE_RELATIVE: v = (u32)s->relative; break;
        case AL_SAMPLE_OFFSET: v = (u32)s->at; break;
        case AL_SOURCE_TYPE: v = s->nq > 1 ? AL_STREAMING : s->nq ? AL_STATIC : AL_UNDETERMINED; break;
        default: break;
        }
    UNLOCK();
    if (out)
        wr32(out, v);
}

void h_alListener3f(u32 param, float x, float y, float z)
{
    if (param == AL_POSITION) {
        LOCK();
        lis_pos[0] = x, lis_pos[1] = y, lis_pos[2] = z;
        UNLOCK();
    }
}
void h_alListenerfv(u32 param, u32 v)
{
    const float *f = GP(v);
    LOCK();
    if (param == AL_ORIENTATION) {
        memcpy(lis_at, f, 12);
        memcpy(lis_up, f + 3, 12);
    } else if (param == AL_POSITION) {
        memcpy(lis_pos, f, 12);
    } else if (param == AL_GAIN) {
        lis_gain = f[0];
    }
    UNLOCK();
}
void h_alDistanceModel(u32 model) { dist_model = model; }

u32 h_AudioSessionInitialize(u32 a, u32 b, u32 listener, u32 user) { (void)a, (void)b, (void)listener, (void)user; return 0; }
u32 h_AudioSessionSetActive(u32 on) { (void)on; return 0; }
u32 h_AudioSessionSetProperty(u32 id, u32 size, u32 data) { (void)id, (void)size, (void)data; return 0; }
u32 h_AudioSessionGetProperty(u32 id, u32 size, u32 data)
{
#define FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))
    u32 n = size ? rd32(size) : 0;
    if (data && n)
        memset(GP(data), 0, n);
    if (id == FOURCC('c', 'h', 's', 'r') && data && n == 8) {
        double r = OUT_RATE;
        memcpy(GP(data), &r, 8);
    }
    return 0;
}
u32 h_AudioSessionAddPropertyListener(u32 id, u32 fn, u32 user) { (void)id, (void)fn, (void)user; return 0; }
u32 h_AudioSessionRemovePropertyListenerWithUserData(u32 id, u32 fn, u32 user) { (void)id, (void)fn, (void)user; return 0; }
void h_AudioServicesPlaySystemSound(u32 id) { (void)id; }

void al_init(void)
{
    mx = SDL_CreateMutex();
}
