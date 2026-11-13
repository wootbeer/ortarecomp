/*
 * Software voice mixer. See mixer.h.
 *
 * Output: stereo (the speaker bins folded to two), or 5.1, the bins as they
 * are: SDL's 5.1 order (FL, FR, FC, LFE, BL, BR) is the Xbox's bin order.
 *
 * Debugging switches (environment):
 *   PDO_AUDIO_DUMP=file.wav   record exactly what is mixed (float, the output's channels)
 *   PDO_AUDIO_STATS=1         log peak level and clipped samples per second
 */

#include "audio/mixer.h"
#include "config.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VOICES     512
#define BLOCK_FRAMES   256
#define XADPCM_BLOCK   36      /* bytes per channel per block */
#define MAX_PACKETS    64
/* Mixing columns: the speaker bins, then the reverb send. */
#define SEND           PDO_BIN_SPEAKERS
#define COLUMNS        (PDO_BIN_SPEAKERS + 1)

typedef struct packet {
    uint32_t addr, bytes, tag;
} packet;

extern ptrdiff_t g_xbox_mem_offset;

typedef struct voice {
    bool           used;
    pdo_voice_desc desc;
    bool           playing, looping, paused;
    bool           test_muted;          /* PDO_TEST_NEWEST only */
    double         pos;                 /* in frames */
    uint32_t       loop_start, loop_len;/* frames; len 0 = whole buffer */
    float          volume, headroom;    /* linear */
    float          g3d[2];              /* 3D left/right gains */
    /* 5.1 (pdo_voice_set_3d_surround): the 3D gain of each speaker bin
     * instead, the voice's front speakers' level spread over the corners. */
    bool           pan6;
    float          g6[PDO_BIN_SPEAKERS];
    /* Headphones (pdo_voice_set_3d_ears): a mono 3D voice heard through a
     * head -- each ear's delay, head-shadow filter and a cue for behind. */
    bool           ears;
    float          ear_delay[2], ear_delay_now[2];   /* samples */
    float          ear_b0[2], ear_b1[2], ear_a1[2];  /* head shadow, per ear */
    float          ear_x1[2], ear_y1[2], ear_lp[2];
    float          ear_rear;                         /* 0..0.5: behind */
    float          ear_line[64];                     /* the voice's last samples */
    uint32_t       ear_pos;
    /* The Xbox's HRTF (pdo_voice_set_3d_hrtf): each ear's FIR in place of
     * the model head's filter, fed from that ear's delayed samples. */
    bool           hrtf;
    float          hrtf_h[2][PDO_HRTF_TAPS];
    float          hrtf_x[2][PDO_HRTF_TAPS];         /* each ear's input, newest first */
    float          direct, room;        /* I3DL2 source levels, linear */
    float          dist;                /* 3D distance gain, linear */
    double         step;                /* frames per output frame */
    float          gain[2][COLUMNS];
    /* ADPCM: the last decoded block, both channels */
    int32_t        cached_block;
    int16_t        cache[2][72];
    /* Packet queue (streams): the head packet is desc.data/desc.bytes. */
    bool           queued;
    packet         q[MAX_PACKETS];
    int            q_head, q_count;
    pdo_packet_done_fn done;
    void          *done_user;
} voice;

static voice voices[MAX_VOICES];
static SDL_Mutex *lock;
static SDL_AudioStream *stream;
static int out_channels = 2;        /* 2, or 6 for 5.1 */
static SDL_Thread *pacer;
static SDL_AtomicInt pacer_quit;

static SDL_IOStream *dump;
static Uint64 dump_frames;
static bool stats;
static float stat_peak;
static Uint64 stat_clipped, stat_frames;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static float db100_to_gain(int32_t v)
{
    if (v <= -10000)
        return 0.0f;
    return powf(10.0f, (float)v / 2000.0f);
}

static voice *get(int id)
{
    if (id <= 0 || id > MAX_VOICES || !voices[id - 1].used)
        return NULL;
    return &voices[id - 1];
}

static const uint8_t *guest(uint32_t va)
{
    return (const uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
}

static uint32_t spb(const voice *v)
{
    return v->desc.samples_per_block ? v->desc.samples_per_block : 64;
}

static uint32_t frame_bytes(const voice *v)
{
    return (v->desc.format == PDO_FMT_PCM8 ? 1 : 2) * (uint32_t)v->desc.channels;
}

static uint32_t total_frames(const voice *v)
{
    if (!v->desc.data)
        return 0;
    if (v->desc.format == PDO_FMT_XADPCM)
        return v->desc.bytes / (XADPCM_BLOCK * (uint32_t)v->desc.channels) * spb(v);
    return v->desc.bytes / frame_bytes(v);
}

static uint32_t bytes_to_frames(const voice *v, uint32_t bytes)
{
    if (v->desc.format == PDO_FMT_XADPCM)
        return bytes / (XADPCM_BLOCK * (uint32_t)v->desc.channels) * spb(v);
    return bytes / frame_bytes(v);
}

static uint32_t frames_to_bytes(const voice *v, uint32_t frames)
{
    if (v->desc.format == PDO_FMT_XADPCM)
        return frames / spb(v) * XADPCM_BLOCK * (uint32_t)v->desc.channels;
    return frames * frame_bytes(v);
}

static void update_step(voice *v, uint32_t hz)
{
    v->step = (double)(hz ? hz : v->desc.sample_rate) / PDO_MIX_RATE;
}

/* ── Headphones: a spherical head ─────────────────────────────────────
 * The textbook structural model of a head (a sphere of radius 8.75 cm,
 * ears at its sides): the far ear hears a sound later, by Woodworth's
 * (a/c)(phi + sin phi) for a source at lateral angle phi, and the head
 * shadows it -- a one-pole, one-zero filter whose high frequencies rise to
 * +6 dB at the ear facing the sound and fall to -20 dB at 150 degrees from
 * it. Our own code from the model's formulas; no measured data. Front and
 * back sound alike to a sphere, so sounds behind lose some top end (a
 * pinna's shadow, roughly). Mono 3D voices only. */
#define HEAD_RADIUS 0.0875f
#define SOUND_SPEED 343.0f

static void ear_filter(voice *v, int ear, float cos_to_ear)
{
    /* alpha(theta) with alpha_min 0.1 at theta_min 150 degrees */
    const float amin = 0.1f, tmin = 150.0f;
    float th = acosf(cos_to_ear < -1.0f ? -1.0f : cos_to_ear > 1.0f ? 1.0f : cos_to_ear) * 57.2957795f;
    float alpha = (1.0f + amin * 0.5f) + (1.0f - amin * 0.5f) * cosf(th / tmin * 3.14159265f);
    float beta = 2.0f * SOUND_SPEED / HEAD_RADIUS;   /* 2 w0 */
    float k = 2.0f * PDO_MIX_RATE / beta;            /* bilinear transform */
    float a0 = 1.0f + k;
    v->ear_b0[ear] = (1.0f + alpha * k) / a0;
    v->ear_b1[ear] = (1.0f - alpha * k) / a0;
    v->ear_a1[ear] = (1.0f - k) / a0;
}

/* One sample through both ears: e[0] left, e[1] right. */
static void ears_process(voice *v, float s, float e[2])
{
    v->ear_line[v->ear_pos & 63] = s;
    for (int ear = 0; ear < 2; ear++) {
        float d = v->ear_delay_now[ear], t = v->ear_delay[ear], x, y;
        uint32_t i;
        float f;
        /* Glide to a new delay (a jump clicks): under 0.1 sample a sample. */
        d += t - d > 0.05f ? 0.05f : t - d < -0.05f ? -0.05f : t - d;
        v->ear_delay_now[ear] = d;
        i = (uint32_t)d;
        f = d - (float)i;
        x = v->ear_line[(v->ear_pos - i) & 63];
        x += (v->ear_line[(v->ear_pos - i - 1) & 63] - x) * f;
        if (v->hrtf) {
            float *h = v->hrtf_h[ear], *in = v->hrtf_x[ear], acc = 0.0f;
            memmove(in + 1, in, sizeof(float) * (PDO_HRTF_TAPS - 1));
            in[0] = x;
            for (int k = 0; k < PDO_HRTF_TAPS; k++)
                acc += h[k] * in[k];
            e[ear] = acc;
            continue;
        }
        y = v->ear_b0[ear] * x + v->ear_b1[ear] * v->ear_x1[ear] - v->ear_a1[ear] * v->ear_y1[ear];
        v->ear_x1[ear] = x;
        v->ear_y1[ear] = y;
        /* behind: some of a ~4 kHz low-pass */
        v->ear_lp[ear] += 0.41f * (y - v->ear_lp[ear]);
        e[ear] = y + v->ear_rear * (v->ear_lp[ear] - y);
    }
    v->ear_pos++;
}

/* What a bin takes of the two ears: left bins the left, right bins the
 * right, the centre and LFE both. */
static float ear_mix(uint32_t bin, const float e[2])
{
    if (bin == PDO_BIN_FRONT_LEFT || bin == PDO_BIN_BACK_LEFT)
        return e[0];
    if (bin == PDO_BIN_FRONT_RIGHT || bin == PDO_BIN_BACK_RIGHT)
        return e[1];
    return 0.5f * (e[0] + e[1]);
}

/* Which channel of the buffer a mixbin pair feeds. One pair per channel in
 * order for multichannel buffers; every pair takes the mono channel. */
/* The 3D gain a speaker bin takes: left or right, both for the others. */
static float side_gain(const voice *v, uint32_t bin)
{
    if (bin == PDO_BIN_FRONT_LEFT || bin == PDO_BIN_BACK_LEFT)
        return v->g3d[0];
    if (bin == PDO_BIN_FRONT_RIGHT || bin == PDO_BIN_BACK_RIGHT)
        return v->g3d[1];
    return 0.5f * (v->g3d[0] + v->g3d[1]);
}

/* The reverb send of a 3D voice takes its distance gain but not its pan:
 * DirectSound's per-bin volumes (0x002C6E40) give the I3DL2 bin the voice's
 * distance attenuation plus its I3DL2 room level. (The room path's own
 * extra rolloff, flRoomRolloffFactor, is 0 wherever Orta sets it.) */
static void update_gains(voice *v)
{
    bool sends = false;
    float dist = v->desc.positional ? v->dist : 1.0f;

    memset(v->gain, 0, sizeof v->gain);
    for (int i = 0; i < v->desc.bin_count; i++) {
        int ch = v->desc.channels == 1 ? 0 : i % v->desc.channels;
        uint32_t bin = v->desc.bins[i].bin;
        if (bin < PDO_BIN_SPEAKERS && v->pan6) {
            /* 5.1: the loudest speaker bin's level, spread over the
             * corners by where the voice is (a stereo voice's two channels
             * both, halved). */
            float level = db100_to_gain(v->desc.bins[i].volume) * v->direct /
                          (v->desc.channels == 1 ? 1.0f : (float)v->desc.channels);
            for (int b = 0; b < PDO_BIN_SPEAKERS; b++)
                if (level * v->g6[b] > v->gain[ch][b])
                    v->gain[ch][b] = level * v->g6[b];
        } else if (bin < PDO_BIN_SPEAKERS)
            v->gain[ch][bin] = db100_to_gain(v->desc.bins[i].volume) * side_gain(v, bin) * v->direct;
        else if (bin == PDO_BIN_I3DL2) {
            v->gain[ch][SEND] = db100_to_gain(v->desc.bins[i].volume) * v->room * dist;
            sends = true;
        } else {
            static uint32_t seen;
            if (bin < 32 && !(seen & (1u << bin))) {
                seen |= 1u << bin;
                SDL_Log("audio: mixbin %u (effect send) not mixed yet", bin);
            }
        }
    }
    /* A 3D voice given no mixbins of its own feeds the reverb too. */
    if (v->desc.positional && !sends)
        for (int ch = 0; ch < v->desc.channels; ch++)
            v->gain[ch][SEND] = v->room * dist / (float)v->desc.channels;
}

/* ── Xbox ADPCM ───────────────────────────────────────────────────────────
 * IMA ADPCM in 36-byte blocks per channel: a 4-byte header (int16 sample,
 * uint8 step index, reserved) then 32 bytes of nibbles, low nibble first.
 * Multichannel blocks interleave the channels four bytes at a time. A block
 * yields 64 samples: the header sample, then 63 nibbles (the last nibble of
 * a block is not played). */

static const int16_t ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
static const int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static void decode_block(voice *v, uint32_t block)
{
    int chs = v->desc.channels;
    const uint8_t *b = guest(v->desc.data + block * XADPCM_BLOCK * (uint32_t)chs);
    uint32_t n = spb(v);

    for (int ch = 0; ch < chs; ch++) {
        const uint8_t *h = b + ch * 4;
        int pred = (int16_t)(h[0] | h[1] << 8);
        int idx = h[2] > 88 ? 88 : h[2];
        uint32_t out = 0;

        v->cache[ch][out++] = (int16_t)pred;
        for (uint32_t word = 0; word < 8 && out < n; word++) {
            const uint8_t *d = b + 4 * chs + (word * chs + ch) * 4;
            for (int byte = 0; byte < 4 && out < n; byte++) {
                for (int half = 0; half < 2 && out < n; half++) {
                    int nib = (d[byte] >> (half * 4)) & 15;
                    int step = ima_steps[idx];
                    int diff = step >> 3;
                    if (nib & 1) diff += step >> 2;
                    if (nib & 2) diff += step >> 1;
                    if (nib & 4) diff += step;
                    pred += (nib & 8) ? -diff : diff;
                    if (pred > 32767) pred = 32767;
                    if (pred < -32768) pred = -32768;
                    idx += ima_index[nib];
                    if (idx < 0) idx = 0;
                    if (idx > 88) idx = 88;
                    v->cache[ch][out++] = (int16_t)pred;
                }
            }
        }
    }
    v->cached_block = (int32_t)block;
}

static float sample_at(voice *v, int ch, uint32_t frame)
{
    switch (v->desc.format) {
    case PDO_FMT_PCM8:
        return ((float)guest(v->desc.data)[frame * v->desc.channels + ch] - 128.0f) / 128.0f;
    case PDO_FMT_PCM16: {
        const uint8_t *p = guest(v->desc.data) + (frame * v->desc.channels + ch) * 2;
        return (float)(int16_t)(p[0] | p[1] << 8) / 32768.0f;
    }
    case PDO_FMT_XADPCM: {
        uint32_t block = frame / spb(v);
        if ((int32_t)block != v->cached_block)
            decode_block(v, block);
        return (float)v->cache[ch][frame % spb(v)] / 32768.0f;
    }
    }
    return 0.0f;
}

/* ── Mixing ───────────────────────────────────────────────────────────── */

/* Make the head of the queue the voice's data (or none). */
static void load_head(voice *v)
{
    if (v->q_count) {
        v->desc.data = v->q[v->q_head].addr;
        v->desc.bytes = v->q[v->q_head].bytes;
    } else {
        v->desc.data = 0;
        v->desc.bytes = 0;
    }
    v->cached_block = -1;
}

static void pop_head(voice *v, bool flushed)
{
    packet p = v->q[v->q_head];

    v->q_head = (v->q_head + 1) % MAX_PACKETS;
    v->q_count--;
    load_head(v);
    if (v->done)
        v->done(v->done_user, p.tag, flushed);
}

/* A packet-queue voice: play the head packet, moving to the next one at its
 * end without a gap (the fraction of a frame carries over). Interpolation
 * does not look across a packet boundary. */
static void mix_queue(voice *v, float bins[][COLUMNS], int frames)
{
    float g = v->volume * v->headroom;

    for (int i = 0; i < frames; i++) {
        uint32_t total, f0, f1;
        float t;

        while (v->q_count && v->pos >= (total = total_frames(v))) {
            v->pos -= total;
            pop_head(v, false);
        }
        if (!v->q_count) {
            v->pos = 0;
            return;
        }
        total = total_frames(v);
        f0 = (uint32_t)v->pos;
        f1 = f0 + 1 < total ? f0 + 1 : f0;
        t = (float)(v->pos - f0);
        for (int ch = 0; ch < v->desc.channels; ch++) {
            float s = sample_at(v, ch, f0);
            s += (sample_at(v, ch, f1) - s) * t;
            s *= g;
            if (v->ears) {          /* headphones: through the head; the reverb send as is */
                float e[2];
                ears_process(v, s, e);
                for (int b = 0; b < SEND; b++)
                    bins[i][b] += ear_mix((uint32_t)b, e) * v->gain[ch][b];
                bins[i][SEND] += s * v->gain[ch][SEND];
                continue;
            }
            for (int b = 0; b < COLUMNS; b++)
                bins[i][b] += s * v->gain[ch][b];
        }
        v->pos += v->step;
    }
}

static void mix_voice(voice *v, float bins[][COLUMNS], int frames)
{
    uint32_t total = total_frames(v);
    uint32_t ls, le;
    float g = v->volume * v->headroom;

    if (!total)
        return;
    ls = v->looping ? v->loop_start : 0;
    le = (v->looping && v->loop_len) ? v->loop_start + v->loop_len : total;
    if (le > total) le = total;
    if (ls >= le) ls = 0;

    for (int i = 0; i < frames; i++) {
        uint32_t f0, f1;
        float t;

        /* Wrap before taking this frame's sample, not instead of it:
         * skipping the frame left one silent output sample per lap of a
         * looping buffer, a click ~2.7 times a second under streamed music
         * (64 KB ring at 44.1 kHz), and lost a frame of time per lap. */
        if (v->pos >= le) {
            if (!v->looping) {
                v->playing = false;
                return;
            }
            v->pos -= (le - ls);
            if (v->pos < ls) v->pos = ls;
        }
        f0 = (uint32_t)v->pos;
        f1 = f0 + 1;
        t = (float)(v->pos - f0);
        if (f1 >= le)
            f1 = v->looping ? ls : f0;

        for (int ch = 0; ch < v->desc.channels; ch++) {
            float s = sample_at(v, ch, f0);
            s += (sample_at(v, ch, f1) - s) * t;
            s *= g;
            if (v->ears) {          /* headphones: through the head; the reverb send as is */
                float e[2];
                ears_process(v, s, e);
                for (int b = 0; b < SEND; b++)
                    bins[i][b] += ear_mix((uint32_t)b, e) * v->gain[ch][b];
                bins[i][SEND] += s * v->gain[ch][SEND];
                continue;
            }
            for (int b = 0; b < COLUMNS; b++)
                bins[i][b] += s * v->gain[ch][b];
        }
        v->pos += v->step;
    }
}

static void write_dump_header(void)
{
    /* RIFF/WAVE, IEEE float, the output's channels, sizes patched as the
     * file grows. */
    uint32_t fb = 4u * (uint32_t)out_channels;
    uint32_t data = (uint32_t)(dump_frames * fb);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    *(uint32_t *)(h + 4) = 36 + data;
    memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16;
    *(uint16_t *)(h + 20) = 3;
    *(uint16_t *)(h + 22) = (uint16_t)out_channels;
    *(uint32_t *)(h + 24) = PDO_MIX_RATE;
    *(uint32_t *)(h + 28) = PDO_MIX_RATE * fb;
    *(uint16_t *)(h + 32) = (uint16_t)fb;
    *(uint16_t *)(h + 34) = 32;
    memcpy(h + 36, "data", 4);
    *(uint32_t *)(h + 40) = data;
    SDL_SeekIO(dump, 0, SDL_IO_SEEK_SET);
    SDL_WriteIO(dump, h, sizeof h);
    SDL_SeekIO(dump, 0, SDL_IO_SEEK_END);
}

/* frames of chs floats (2 or 6). */
static void mix_block(float *out, int frames, int chs)
{
    static float bins[BLOCK_FRAMES][COLUMNS];
    static float send[BLOCK_FRAMES];
    static float room[BLOCK_FRAMES * 2];
    const float k = 0.70710678f;

    memset(bins, 0, sizeof(bins[0]) * (size_t)frames);
    static long solo = -1;
    if (solo < 0)   /* PDO_AUDIO_SOLO=bytes: mix only buffers of that size */
        solo = getenv("PDO_AUDIO_SOLO") ? atol(getenv("PDO_AUDIO_SOLO")) : 0;
    SDL_LockMutex(lock);
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && voices[i].playing && !voices[i].test_muted &&
            (!solo || voices[i].desc.bytes == (uint32_t)solo)) {
            if (voices[i].queued)
                mix_queue(&voices[i], bins, frames);
            else
                mix_voice(&voices[i], bins, frames);
        }

    /* The mixbins' headroom. DirectSound's start-up (0x002C3B2A fills the
     * table, 0x002C67C6 writes it to the APU, 0x002C6304 per bin) gives
     * mixbins 0-30 a headroom of 1, a shift right by one: every bin we mix
     * (speakers 0-5, the I3DL2 send 10) is halved, -6.02 dB, before the
     * reverb and the output. Orta never changes it. Missing, the whole mix
     * was 6 dB hot and dense gunfire clipped hard. */
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < COLUMNS; c++)
            bins[i][c] *= 0.5f;

    if (chs == 6) {
        /* 5.1: the speaker bins as they are, the reverb's two channels
         * around the room (front and back, each -3 dB). */
        memset(room, 0, sizeof(float) * 2 * (size_t)frames);
        for (int i = 0; i < frames; i++) {
            memcpy(out + i * 6, bins[i], 6 * sizeof(float));
            send[i] = bins[i][SEND];
        }
        pdo_reverb_process(send, room, frames, PDO_MIX_RATE);
        for (int i = 0; i < frames; i++) {
            out[i * 6 + PDO_BIN_FRONT_LEFT] += k * room[i * 2];
            out[i * 6 + PDO_BIN_FRONT_RIGHT] += k * room[i * 2 + 1];
            out[i * 6 + PDO_BIN_BACK_LEFT] += k * room[i * 2];
            out[i * 6 + PDO_BIN_BACK_RIGHT] += k * room[i * 2 + 1];
        }
    } else {
        /* Fold the speaker bins to stereo. LFE is not folded in, as a
         * stereo receiver would drop it too. Then the reverb on the send. */
        for (int i = 0; i < frames; i++) {
            float *b = bins[i];
            out[i * 2] = b[PDO_BIN_FRONT_LEFT] + k * b[PDO_BIN_FRONT_CENTER] + k * b[PDO_BIN_BACK_LEFT];
            out[i * 2 + 1] = b[PDO_BIN_FRONT_RIGHT] + k * b[PDO_BIN_FRONT_CENTER] + k * b[PDO_BIN_BACK_RIGHT];
            send[i] = b[SEND];
        }
        pdo_reverb_process(send, out, frames, PDO_MIX_RATE);
    }
    SDL_UnlockMutex(lock);

    for (int i = 0; i < frames * chs; i++) {
        float x = out[i];

        if (stats) {
            float m = fabsf(x);
            if (m > stat_peak) stat_peak = m;
            if (m > 1.0f) stat_clipped++;
        }
        out[i] = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
    }

    if (dump) {
        SDL_WriteIO(dump, out, (size_t)frames * 4 * (size_t)chs);
        dump_frames += (Uint64)frames;
        if (dump_frames % PDO_MIX_RATE < (Uint64)frames)
            write_dump_header();
    }
    if (stats) {
        stat_frames += (Uint64)frames;
        if (stat_frames >= PDO_MIX_RATE) {
            int playing = 0, looping = 0;

            SDL_LockMutex(lock);
            for (int i = 0; i < MAX_VOICES; i++)
                if (voices[i].used && voices[i].playing && !voices[i].queued) {
                    playing++;
                    looping += voices[i].looping;
                }
            SDL_UnlockMutex(lock);
            SDL_Log("audio: peak %.2f dBFS, %llu clipped frames in the last second, %d voices playing (%d looping)",
                    stat_peak > 0 ? 20.0 * log10(stat_peak) : -120.0,
                    (unsigned long long)stat_clipped, playing, looping);
            if (getenv("PDO_AUDIO_VOICES")) {
                SDL_LockMutex(lock);
                for (int i = 0; i < MAX_VOICES; i++)
                    if (voices[i].used && voices[i].playing && !voices[i].queued)
                        SDL_Log("audio:   voice %d data 0x%08x %u bytes%s", i + 1, voices[i].desc.data,
                                voices[i].desc.bytes, voices[i].looping ? " looping" : "");
                SDL_UnlockMutex(lock);
            }
            stat_frames = 0;
            stat_peak = 0;
            stat_clipped = 0;
        }
    }
}

/* While the options overlay is open the voices hold still: silence out, and
 * no play cursor moves. */
static SDL_AtomicInt paused;

void pdo_mixer_set_paused(bool on)
{
    SDL_SetAtomicInt(&paused, on);
}

static void SDLCALL stream_cb(void *userdata, SDL_AudioStream *s, int additional, int total)
{
    float buf[BLOCK_FRAMES * 6];
    /* The overlay's master volume, squared so the slider feels even. */
    float g = (float)pdo_settings_get()->master_volume / 100.0f;
    int chs = (int)(intptr_t)userdata, fb = 4 * chs;

    g *= g;
    if (chs != out_channels)        /* a stream being replaced (pdo_mixer_apply_output) */
        return;
    while (additional > 0) {
        int frames = additional / fb;
        if (frames > BLOCK_FRAMES) frames = BLOCK_FRAMES;
        if (frames <= 0) frames = 1;
        if (SDL_GetAtomicInt(&paused))
            memset(buf, 0, sizeof(float) * (size_t)chs * (size_t)frames);
        else {
            mix_block(buf, frames, chs);
            if (g != 1.0f)
                for (int i = 0; i < frames * chs; i++)
                    buf[i] *= g;
        }
        SDL_PutAudioStreamData(s, buf, frames * fb);
        additional -= frames * fb;
    }
}

/* Without an audio device the game still needs its play cursors to move at
 * the real rate, or streaming code waits forever. */
static int SDLCALL pacer_main(void *unused)
{
    float buf[BLOCK_FRAMES * 6];
    Uint64 start = SDL_GetTicksNS(), mixed = 0;

    while (!SDL_GetAtomicInt(&pacer_quit)) {
        Uint64 due = (SDL_GetTicksNS() - start) * PDO_MIX_RATE / SDL_NS_PER_SECOND;
        while (mixed + BLOCK_FRAMES <= due) {
            if (!SDL_GetAtomicInt(&paused))
                mix_block(buf, BLOCK_FRAMES, out_channels);
            mixed += BLOCK_FRAMES;
        }
        SDL_Delay(2);
    }
    return 0;
}

/* ── API ──────────────────────────────────────────────────────────────── */

/* The device with as many channels as the setting wants: 6 for 5.1 (SDL
 * folds them down itself if the device has fewer), else 2. */
static SDL_AudioStream *open_output(void)
{
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, PDO_MIX_RATE };
    SDL_AudioStream *s;

    spec.channels = out_channels;
    s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, stream_cb,
                                  (void *)(intptr_t)out_channels);
    if (s) {
        SDL_ResumeAudioStreamDevice(s);
        SDL_Log("audio: %s, 48 kHz %s", SDL_GetCurrentAudioDriver(), out_channels == 6 ? "5.1" : "stereo");
    }
    return s;
}

void pdo_mixer_apply_output(void)
{
    int want = pdo_settings_get()->audio_out == PDO_AUDIO_SURROUND ? 6 : 2;

    if (want == out_channels)
        return;
    /* The old stream's callback mixes no more once out_channels moves on;
     * the pacer (no device) just mixes in the new layout. */
    SDL_LockMutex(lock);
    out_channels = want;
    SDL_UnlockMutex(lock);
    if (!stream)
        return;
    SDL_DestroyAudioStream(stream);
    stream = open_output();
    if (!stream)
        SDL_Log("audio: no output after the change (%s)", SDL_GetError());
}

bool pdo_mixer_init(void)
{
    const char *path = getenv("PDO_AUDIO_DUMP");
    const char *st = getenv("PDO_AUDIO_STATS");

    if (lock)
        return true;
    lock = SDL_CreateMutex();
    stats = st && *st && *st != '0';
    out_channels = pdo_settings_get()->audio_out == PDO_AUDIO_SURROUND ? 6 : 2;
    if (path && *path) {
        dump = SDL_IOFromFile(path, "wb");
        if (dump)
            write_dump_header();
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        stream = open_output();
    if (!stream) {
        SDL_Log("audio: no output device (%s); mixing silently in real time", SDL_GetError());
        pacer = SDL_CreateThread(pacer_main, "pdo_audio", NULL);
    }
    return true;
}

void pdo_mixer_shutdown(void)
{
    if (stream) {
        SDL_DestroyAudioStream(stream);
        stream = NULL;
    }
    if (pacer) {
        SDL_SetAtomicInt(&pacer_quit, 1);
        SDL_WaitThread(pacer, NULL);
        pacer = NULL;
    }
    if (dump) {
        write_dump_header();
        SDL_CloseIO(dump);
        dump = NULL;
    }
}

int pdo_voice_create(const pdo_voice_desc *desc)
{
    int id = 0;

    SDL_LockMutex(lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].used) {
            voice *v = &voices[i];
            memset(v, 0, sizeof *v);
            v->used = true;
            v->desc = *desc;
            v->volume = 1.0f;
            v->headroom = 1.0f;
            v->g3d[0] = v->g3d[1] = 1.0f;
            v->direct = v->room = 1.0f;
            v->dist = 1.0f;
            v->cached_block = -1;
            update_step(v, 0);
            update_gains(v);
            id = i + 1;
            break;
        }
    }
    SDL_UnlockMutex(lock);
    if (!id)
        SDL_Log("audio: out of voices");
    return id;
}

void pdo_voice_destroy(int id)
{
    SDL_LockMutex(lock);
    if (get(id))
        voices[id - 1].used = false;
    SDL_UnlockMutex(lock);
}

#define WITH_VOICE(id, body) do {               \
        voice *v;                               \
        SDL_LockMutex(lock);                    \
        if ((v = get(id)) != NULL) { body; }    \
        SDL_UnlockMutex(lock);                  \
    } while (0)

void pdo_voice_set_data(int id, uint32_t addr, uint32_t bytes)
{
    WITH_VOICE(id, v->desc.data = addr; v->desc.bytes = bytes; v->pos = 0; v->cached_block = -1);
}

/* PDO_TEST_NEWEST=1, a listening test, not a setting: a 3D voice that
 * starts mutes the other 3D voices playing the same sample, so only the
 * newest copy of each sound is heard. */
static void test_newest(voice *v)
{
    static int on = -1;
    if (on < 0)
        on = getenv("PDO_TEST_NEWEST") && *getenv("PDO_TEST_NEWEST") == '1';
    v->test_muted = false;
    if (!on || !v->desc.positional)
        return;
    for (int i = 0; i < MAX_VOICES; i++)
        if (&voices[i] != v && voices[i].used && voices[i].playing && voices[i].desc.positional &&
            voices[i].desc.data == v->desc.data && voices[i].desc.bytes == v->desc.bytes)
            voices[i].test_muted = true;
}

void pdo_voice_play(int id, bool looping)
{
    WITH_VOICE(id, v->playing = true; v->looping = looping; v->paused = false; test_newest(v));
}

void pdo_voice_stop(int id)
{
    WITH_VOICE(id, v->playing = false; v->paused = false);
}

void pdo_voice_pause(int id, bool pause)
{
    WITH_VOICE(id,
        if (pause && v->playing) { v->playing = false; v->paused = true; }
        else if (!pause && v->paused) { v->playing = true; v->paused = false; });
}

bool pdo_voice_playing(int id, bool *looping)
{
    bool playing = false;
    WITH_VOICE(id, playing = v->playing; if (looping) *looping = v->looping);
    return playing;
}

void pdo_voice_set_position(int id, uint32_t byte_offset)
{
    WITH_VOICE(id, v->pos = bytes_to_frames(v, byte_offset));
}

uint32_t pdo_voice_position(int id)
{
    uint32_t pos = 0;
    WITH_VOICE(id, pos = frames_to_bytes(v, (uint32_t)v->pos));
    return pos;
}

void pdo_voice_set_loop_region(int id, uint32_t start, uint32_t length)
{
    WITH_VOICE(id, v->loop_start = bytes_to_frames(v, start); v->loop_len = bytes_to_frames(v, length));
}

void pdo_voice_set_3d_distance(int id, float gain)
{
    WITH_VOICE(id, v->dist = gain; update_gains(v));
}

void pdo_voice_set_3d(int id, float left, float right)
{
    WITH_VOICE(id, v->ears = false; v->hrtf = false; v->pan6 = false; v->g3d[0] = left; v->g3d[1] = right; update_gains(v));
}

static void set_ears(voice *v, float gain, float lateral, float front)
{
    float lat = lateral < -1.0f ? -1.0f : lateral > 1.0f ? 1.0f : lateral;
    float phi = asinf(lat < 0 ? -lat : lat);
    float itd = HEAD_RADIUS / SOUND_SPEED * (phi + sinf(phi)) * PDO_MIX_RATE;   /* samples */
    v->pan6 = false;
    if (v->desc.channels != 1) {    /* stereo 3D buffers: the pan */
        float l = sqrtf(0.5f * (1.0f - lat)), r = sqrtf(0.5f * (1.0f + lat));
        v->ears = false;
        v->g3d[0] = gain * l;
        v->g3d[1] = gain * r;
        update_gains(v);
        return;
    }
    if (!v->ears) {                 /* start at the delays, not gliding from 0 */
        v->ear_delay_now[0] = lat > 0 ? itd : 0.0f;
        v->ear_delay_now[1] = lat < 0 ? itd : 0.0f;
    }
    v->ears = true;
    v->hrtf = false;
    v->ear_delay[0] = lat > 0 ? itd : 0.0f;     /* the far ear hears it later */
    v->ear_delay[1] = lat < 0 ? itd : 0.0f;
    ear_filter(v, 0, -lat);
    ear_filter(v, 1, lat);
    v->ear_rear = front < 0 ? -0.5f * front : 0.0f;
    /* -3 dB each ear, as the pan has straight ahead */
    v->g3d[0] = v->g3d[1] = gain * 0.70710678f;
    update_gains(v);
}

void pdo_voice_set_3d_ears(int id, float gain, float lateral, float front)
{
    WITH_VOICE(id, set_ears(v, gain, lateral, front));
}

static void set_hrtf(voice *v, float gain, float h[2][PDO_HRTF_TAPS], const float delay[2])
{
    v->pan6 = false;
    if (v->desc.channels != 1) {    /* stereo 3D buffers: no HRTF (the APU's is for mono voices) */
        v->ears = v->hrtf = false;
        v->g3d[0] = v->g3d[1] = gain * 0.70710678f;
        update_gains(v);
        return;
    }
    if (!v->ears || !v->hrtf) {     /* start at the delays, not gliding from 0 */
        v->ear_delay_now[0] = delay[0];
        v->ear_delay_now[1] = delay[1];
    }
    v->ears = v->hrtf = true;
    v->ear_delay[0] = delay[0];
    v->ear_delay[1] = delay[1];
    memcpy(v->hrtf_h, h, sizeof v->hrtf_h);
    v->g3d[0] = v->g3d[1] = gain;   /* the filters carry the level */
    update_gains(v);
}

void pdo_voice_set_3d_hrtf(int id, float gain, float h[2][PDO_HRTF_TAPS], const float delay[2])
{
    WITH_VOICE(id, set_hrtf(v, gain, h, delay));
}

/* Pairwise constant-power panning around the four corner speakers, by the
 * source's angle from straight ahead (positive to the right). */
static void set_surround(voice *v, float gain, float lateral, float front)
{
    static const struct { int bin; float deg; } ring[] = {
        { PDO_BIN_BACK_LEFT, -110.0f }, { PDO_BIN_FRONT_LEFT, -30.0f }, { PDO_BIN_FRONT_RIGHT, 30.0f },
        { PDO_BIN_BACK_RIGHT, 110.0f }, { PDO_BIN_BACK_LEFT, 250.0f },
    };
    float az = (lateral == 0.0f && front == 0.0f) ? 0.0f : atan2f(lateral, front) * 57.29578f;

    if (az < -110.0f)
        az += 360.0f;                   /* behind on the left: between BR and BL */
    memset(v->g6, 0, sizeof v->g6);
    for (int i = 0; i < 4; i++)
        if (az >= ring[i].deg && az <= ring[i + 1].deg) {
            float p = (az - ring[i].deg) / (ring[i + 1].deg - ring[i].deg) * 1.5707963f;
            v->g6[ring[i].bin] += gain * cosf(p);
            v->g6[ring[i + 1].bin] += gain * sinf(p);
            break;
        }
    v->ears = v->hrtf = false;
    v->pan6 = true;
    update_gains(v);
}

void pdo_voice_set_3d_surround(int id, float gain, float lateral, float front)
{
    WITH_VOICE(id, set_surround(v, gain, lateral, front));
}

void pdo_voice_set_i3dl2(int id, int32_t direct, int32_t room)
{
    WITH_VOICE(id, v->direct = db100_to_gain(direct); v->room = db100_to_gain(room); update_gains(v));
}

void pdo_mixer_set_i3dl2(const pdo_i3dl2 *env)
{
    SDL_LockMutex(lock);
    pdo_reverb_set(env);
    SDL_UnlockMutex(lock);
}

void pdo_voice_set_volume(int id, int32_t hundredths_db)
{
    WITH_VOICE(id, v->volume = db100_to_gain(hundredths_db));
}

void pdo_voice_set_headroom(int id, uint32_t hundredths_db)
{
    WITH_VOICE(id, v->headroom = db100_to_gain(-(int32_t)hundredths_db));
}

void pdo_voice_set_frequency(int id, uint32_t hz)
{
    WITH_VOICE(id, update_step(v, hz));
}

void pdo_voice_set_bins(int id, const pdo_voice_bin *bins, int count)
{
    if (count > PDO_MAX_VOICE_BINS)
        count = PDO_MAX_VOICE_BINS;
    WITH_VOICE(id, memcpy(v->desc.bins, bins, sizeof(*bins) * (size_t)count);
               v->desc.bin_count = count; update_gains(v));
}

uint32_t pdo_voice_block_align(int id)
{
    uint32_t a = 1;
    WITH_VOICE(id, a = v->desc.format == PDO_FMT_XADPCM
                   ? XADPCM_BLOCK * (uint32_t)v->desc.channels : frame_bytes(v));
    return a;
}

void pdo_voice_set_packet_done(int id, pdo_packet_done_fn done, void *user)
{
    WITH_VOICE(id, v->done = done; v->done_user = user);
}

bool pdo_voice_queue(int id, uint32_t addr, uint32_t bytes, uint32_t tag)
{
    bool ok = false;
    voice *v;

    SDL_LockMutex(lock);
    if ((v = get(id)) != NULL && v->q_count < MAX_PACKETS) {
        packet *p = &v->q[(v->q_head + v->q_count) % MAX_PACKETS];
        p->addr = addr;
        p->bytes = bytes;
        p->tag = tag;
        v->queued = true;
        if (v->q_count++ == 0) {
            v->pos = 0;
            load_head(v);
        }
        ok = true;
    }
    SDL_UnlockMutex(lock);
    return ok;
}

int pdo_voice_queued(int id)
{
    int n = 0;
    WITH_VOICE(id, n = v->q_count);
    return n;
}

void pdo_voice_flush(int id)
{
    WITH_VOICE(id, while (v->q_count) pop_head(v, true); v->pos = 0);
}
