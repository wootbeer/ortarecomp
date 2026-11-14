#ifndef PDO_MIXER_H
#define PDO_MIXER_H

/*
 * Software voice mixer: what the Xbox APU did for DirectSound, on SDL3 audio.
 *
 * Voices read their sample data straight out of guest memory (the game owns
 * the buffers and may rewrite them while they play, as streamed music does).
 * Everything is mixed in float at 48 kHz into the Xbox's speaker mixbins and
 * then folded to the output layout, so channel routing follows what the game
 * asked for rather than what a stereo buffer "usually" means.
 */

#include "audio/reverb.h"

#include <stdbool.h>
#include <stdint.h>

#define PDO_MIX_RATE 48000

/* Xbox DSMIXBIN_* speaker bins. Higher bins are effect sends. */
enum {
    PDO_BIN_FRONT_LEFT = 0,
    PDO_BIN_FRONT_RIGHT = 1,
    PDO_BIN_FRONT_CENTER = 2,
    PDO_BIN_LOW_FREQUENCY = 3,
    PDO_BIN_BACK_LEFT = 4,
    PDO_BIN_BACK_RIGHT = 5,
    PDO_BIN_SPEAKERS = 6,
    PDO_BIN_I3DL2 = 10,         /* the I3DL2 reverb send */
};

typedef enum pdo_format {
    PDO_FMT_PCM8,
    PDO_FMT_PCM16,
    PDO_FMT_XADPCM,     /* WAVE_FORMAT_XBOX_ADPCM (0x0069) */
} pdo_format;

#define PDO_MAX_VOICE_BINS 8

typedef struct pdo_voice_bin {
    uint32_t bin;
    int32_t  volume;    /* hundredths of a dB, <= 0 */
} pdo_voice_bin;

typedef struct pdo_voice_desc {
    pdo_format format;
    int        channels;            /* 1 or 2 */
    uint32_t   sample_rate;
    uint32_t   samples_per_block;   /* ADPCM only */
    uint32_t   data;                /* guest address, 0 until known */
    uint32_t   bytes;
    int        bin_count;
    pdo_voice_bin bins[PDO_MAX_VOICE_BINS];
    bool       positional;          /* DSBCAPS_CTRL3D */
} pdo_voice_desc;

bool pdo_mixer_init(void);
void pdo_mixer_shutdown(void);
/* Hold every voice (silence, no cursor moves) while the options overlay is open. */
void pdo_mixer_set_paused(bool on);
/* The output setting changed (Stereo, Headphones, 5.1): reopens the device
 * with two or six channels as needed. */
void pdo_mixer_apply_output(void);

int  pdo_voice_create(const pdo_voice_desc *desc);  /* returns id > 0, or 0 */
void pdo_voice_destroy(int id);

/* All byte offsets and sizes are in the buffer's own format, as the game
 * sees them. */
void pdo_voice_set_data(int id, uint32_t guest_addr, uint32_t bytes);
void pdo_voice_play(int id, bool looping);
void pdo_voice_stop(int id);
/* Pause keeps the position; resume restarts only a voice paused this way
 * (a stopped or finished one stays stopped). */
void pdo_voice_pause(int id, bool pause);
bool pdo_voice_playing(int id, bool *looping);
void pdo_voice_set_position(int id, uint32_t byte_offset);
uint32_t pdo_voice_position(int id);
void pdo_voice_set_loop_region(int id, uint32_t start, uint32_t length);
void pdo_voice_set_volume(int id, int32_t hundredths_db);
void pdo_voice_set_headroom(int id, uint32_t hundredths_db);
void pdo_voice_set_frequency(int id, uint32_t hz);   /* 0 = the format's rate */
void pdo_voice_set_bins(int id, const pdo_voice_bin *bins, int count);
/* Extra gains from 3D positioning (DSBCAPS_CTRL3D voices), left and right;
 * 1, 1 means none. Applied on top of the mixbin volumes. */
void pdo_voice_set_3d(int id, float left, float right);
/* A 3D voice's distance gain alone (1 = none): DirectSound attenuates the
 * reverb send by distance too, not only the direct path. */
void pdo_voice_set_3d_distance(int id, float gain);
/* Headphones: a mono 3D voice heard through a model head instead -- the
 * distance gain, then where it is: lateral -1 (left) .. 1 (right), front
 * -1 (behind) .. 1 (ahead), both as cosines from the listener's axes. */
void pdo_voice_set_3d_ears(int id, float gain, float lateral, float front);
/* The Xbox's HRTF on a mono 3D voice: the distance gain, each ear's
 * 31-tap FIR at 48 kHz (index 0 the newest sample; 0 left, 1 right) and
 * each ear's delay in samples; front and back, the gains of the front and
 * back speaker pairs on 5.1 (each ear feeds its side of both; 1 and 0 on
 * stereo). Stereo voices are panned to the middle. */
#define PDO_HRTF_TAPS 31
void pdo_voice_set_3d_hrtf(int id, float gain, float h[2][PDO_HRTF_TAPS], const float delay[2], float front,
                           float back);
/* 5.1: a 3D voice panned between the two nearest of the four corner
 * speakers (front +-30, back +-110 degrees), as the Xbox's 3D mixing did
 * on surround; same arguments as the ears. */
void pdo_voice_set_3d_surround(int id, float gain, float lateral, float front);

/* I3DL2 source levels, hundredths of a dB (SetI3DL2Source): the direct
 * path's and the reverb send's. A 3D voice sends to the reverb at `room`
 * unless its mixbins say otherwise, as on the Xbox. */
void pdo_voice_set_i3dl2(int id, int32_t direct, int32_t room);
/* The listener's environment (SetI3DL2Listener). */
void pdo_mixer_set_i3dl2(const pdo_i3dl2 *env);

uint32_t pdo_voice_block_align(int id);

/* ── Packet queues (DirectSound streams) ─────────────────────────────────
 * A voice can instead play a queue of packets back to back, as an
 * IDirectSoundStream does: each is a span of guest memory in the voice's
 * format. When one has been played (or flushed), done() is called with its
 * tag, on the mixer thread with the mixer locked: it must not call back
 * into the mixer. A voice with an empty queue plays silence and picks up
 * the next packet as soon as it is queued. */
typedef void (*pdo_packet_done_fn)(void *user, uint32_t tag, bool flushed);

void pdo_voice_set_packet_done(int id, pdo_packet_done_fn done, void *user);
bool pdo_voice_queue(int id, uint32_t guest_addr, uint32_t bytes, uint32_t tag);
int  pdo_voice_queued(int id);          /* packets queued, the playing one included */
void pdo_voice_flush(int id);           /* drop every queued packet, reporting each */

#endif
