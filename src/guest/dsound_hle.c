/*
 * DirectSound, replaced at its API.
 *
 * First written for JSRF (XDK 4134); addresses are Orta's (XDK 4928),
 * mapped by XbSymbolDatabase name, each `ret N` checked in Orta's code.
 *
 * The title links the XDK's DirectSound library, which drives the Xbox's
 * MCPX audio processor directly; with no such hardware its DirectSoundCreate
 * fails with DSERR_NODRIVER and the title quits. Every DirectSound entry
 * point the game itself calls is replaced here and routed to our mixer
 * (src/audio/mixer.c). The library's internals are then never reached.
 *
 * Conventions: each function is __stdcall. At entry g_esp points at the
 * return address and argument n is at g_esp + 4 + 4n. Each one pops the
 * return address and exactly the argument bytes the original's `ret N` did
 * (measured from the binary), and returns its HRESULT in g_eax.
 *
 * Objects handed to the game are small blocks of guest memory holding a tag
 * and our ids; the game treats them as opaque interface pointers.
 *
 * Not implemented yet, accepted and recorded: envelopes (SetEG), LFOs,
 * filters, I3DL2 source HF levels, obstruction and occlusion, the APU's
 * HRTF filters (3D voices are panned instead), and the DSP image's own
 * effects (its I3DL2 reverb is ours, in src/audio/reverb.c).
 *
 * PDO_DSOUND_TRACE=1 logs every call. Listening tests, off unless set:
 * PDO_TEST_NEWEST=1 (src/audio/mixer.c); PDO_MOVIE51=0 keeps movies stereo.
 */

#include "recomp_types.h"

#include "audio/aix.h"
#include "audio/mixer.h"
#include "config.h"
#include "guest/guest.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ARG(n)      MEM32(g_esp + 4 + 4 * (n))
#define RETURN(argbytes, hr) do { g_eax = (uint32_t)(hr); g_esp += 4 + (argbytes); return; } while (0)

#define DS_OK               0x00000000u
#define DSERR_INVALIDCALL   0x88780032u
#define DSERR_OUTOFMEMORY   0x8007000Eu

#define TAG_DSOUND  0x534F4450u     /* "PDOS" */
#define TAG_BUFFER  0x464F4450u     /* "PDOF" */

/* Object layout in guest memory. */
#define OBJ_TAG     0
#define OBJ_VOICE   4
#define OBJ_REFS    8
#define OBJ_DATA    12   /* buffer memory we allocated, if any */
#define OBJ_BYTES   16
#define OBJ_SIZE    32

static uint32_t the_dsound;

/* 3D state of DSBCAPS_CTRL3D buffers, by voice id (see "3D sound"). */
#define DS3DMODE_HEADRELATIVE 1
#define DS3DMODE_DISABLE      2
#define MAX_3D                513     /* voice ids are 1..512 */

static struct {
    float pos[3], front[3], top[3], rolloff;
} listener = { {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1.0f };

static struct {
    bool  used;
    float pos[3], min_dist, max_dist;
    uint32_t mode;
} src3d[MAX_3D];

static int trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("PDO_DSOUND_TRACE");
        on = v && *v && *v != '0';
    }
    return on;
}

#define TRACE(...) do { if (trace_on()) SDL_Log("dsound: " __VA_ARGS__); } while (0)

static void note_once(const char *what)
{
    /* Each unimplemented feature is reported the first time it is used. */
    static const char *seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == what)
            return;
        if (!seen[i]) {
            seen[i] = what;
            SDL_Log("dsound: %s used by the game (accepted, not implemented yet)", what);
            return;
        }
    }
}

static int voice_of(uint32_t obj)
{
    if (!obj || MEM32(obj + OBJ_TAG) != TAG_BUFFER)
        return 0;
    return (int)MEM32(obj + OBJ_VOICE);
}

static uint32_t guest_alloc(uint32_t bytes)
{
    uint32_t va = xbox_HeapAlloc(bytes ? bytes : 4, 32);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, bytes);
    return va;
}

/* ── IDirectSound ─────────────────────────────────────────────────────── */

/* DirectSoundCreate(pguid, ppDS, pUnkOuter) */
void sub_002C49ED(void)
{
    uint32_t pp = ARG(1);

    if (!the_dsound) {
        pdo_mixer_init();
        the_dsound = guest_alloc(OBJ_SIZE);
        if (!the_dsound)
            RETURN(12, DSERR_OUTOFMEMORY);
        MEM32(the_dsound + OBJ_TAG) = TAG_DSOUND;
    }
    MEM32(the_dsound + OBJ_REFS) += 1;
    if (pp)
        MEM32(pp) = the_dsound;
    TRACE("DirectSoundCreate -> 0x%08x", the_dsound);
    RETURN(12, DS_OK);
}

/* IDirectSound_AddRef(pDS) */
void sub_002C127D(void)
{
    uint32_t ds = ARG(0);
    RETURN(4, ds ? (MEM32(ds + OBJ_REFS) += 1) : 0);
}

/* IDirectSound_Release(pDS). The device lives for the whole run. */
void sub_002C1293(void)
{
    uint32_t ds = ARG(0), refs = 0;
    if (ds && MEM32(ds + OBJ_REFS))
        refs = (MEM32(ds + OBJ_REFS) -= 1);
    RETURN(4, refs);
}

/* IDirectSound_GetCaps(pDS, LPDSCAPS) */
void sub_002C240E(void)
{
    uint32_t caps = ARG(1);
    if (caps) {
        MEM32(caps + 0) = 192;      /* dwFree2DBuffers */
        MEM32(caps + 4) = 64;       /* dwFree3DBuffers */
        MEM32(caps + 8) = 2047;     /* dwFreeBufferSGEs */
        MEM32(caps + 12) = 0;       /* dwMemoryAllocated */
    }
    RETURN(8, DS_OK);
}

/* IDirectSound_CreateSoundBuffer(pDS, pdsbd, ppBuffer, pUnkOuter) */
void sub_002C4833(void)
{
    uint32_t desc = ARG(1), pp = ARG(2);
    uint32_t flags, bytes, wfx, mixbins, obj, data = 0;
    pdo_voice_desc vd;
    int id;

    if (!desc || !pp)
        RETURN(16, DSERR_INVALIDCALL);
    flags = MEM32(desc + 4);
    bytes = MEM32(desc + 8);
    wfx = MEM32(desc + 12);
    mixbins = MEM32(desc + 16);

    memset(&vd, 0, sizeof vd);
    vd.format = PDO_FMT_PCM16;
    vd.channels = 1;
    vd.sample_rate = 48000;
    vd.positional = (flags & 0x10) != 0;    /* DSBCAPS_CTRL3D */
    if (wfx) {
        uint16_t tag = MEM16(wfx), bits = MEM16(wfx + 14);
        vd.channels = MEM16(wfx + 2) == 2 ? 2 : 1;
        vd.sample_rate = MEM32(wfx + 4);
        if (tag == 0x0069) {
            vd.format = PDO_FMT_XADPCM;
            vd.samples_per_block = MEM16(wfx + 16) >= 2 ? MEM16(wfx + 18) : 64;
        } else {
            vd.format = bits == 8 ? PDO_FMT_PCM8 : PDO_FMT_PCM16;
            if (tag != 1)
                SDL_Log("dsound: unknown wave format 0x%04x, treating as PCM", tag);
        }
    }

    if (mixbins && MEM32(mixbins)) {
        uint32_t n = MEM32(mixbins), pairs = MEM32(mixbins + 4);
        for (uint32_t i = 0; i < n && i < PDO_MAX_VOICE_BINS && pairs; i++) {
            vd.bins[vd.bin_count].bin = MEM32(pairs + i * 8);
            vd.bins[vd.bin_count].volume = (int32_t)MEM32(pairs + i * 8 + 4);
            vd.bin_count++;
        }
    } else {
        /* The default routing: a mono buffer to both front speakers, a stereo
         * one left to left and right to right. */
        vd.bins[0].bin = PDO_BIN_FRONT_LEFT;
        vd.bins[1].bin = PDO_BIN_FRONT_RIGHT;
        vd.bin_count = 2;
    }

    if (bytes) {
        data = guest_alloc(bytes);
        if (!data)
            RETURN(16, DSERR_OUTOFMEMORY);
        vd.data = data;
        vd.bytes = bytes;
    }

    id = pdo_voice_create(&vd);
    obj = guest_alloc(OBJ_SIZE);
    if (!id || !obj)
        RETURN(16, DSERR_OUTOFMEMORY);
    MEM32(obj + OBJ_TAG) = TAG_BUFFER;
    if (id < MAX_3D) {
        memset(&src3d[id], 0, sizeof src3d[id]);
        src3d[id].used = vd.positional;
    }
    MEM32(obj + OBJ_VOICE) = (uint32_t)id;
    MEM32(obj + OBJ_REFS) = 1;
    MEM32(obj + OBJ_DATA) = data;
    MEM32(obj + OBJ_BYTES) = bytes;
    MEM32(pp) = obj;

    TRACE("CreateSoundBuffer voice %d", id);
    TRACE("CreateSoundBuffer flags 0x%x, %u bytes, %s %u ch %u Hz, %d bins -> 0x%08x",
          flags, bytes, vd.format == PDO_FMT_XADPCM ? "adpcm" : vd.format == PDO_FMT_PCM8 ? "pcm8" : "pcm16",
          vd.channels, vd.sample_rate, vd.bin_count, obj);
    for (int i = 0; i < vd.bin_count; i++)
        TRACE("  0x%08x bin %u volume %d", obj, vd.bins[i].bin, vd.bins[i].volume);
    RETURN(16, DS_OK);
}

/* DirectSound HRTF setup (called once during sound init). The Xbox's
 * tables are not used: with the 3D audio setting on Headphones, our mixer
 * runs its own model head (src/audio/mixer.c). Takes no arguments. */
void sub_002C12DA(void)
{
    note_once("HRTF");
    g_esp += 4;
}

/* DirectSoundDoWork(): the mixer runs on its own thread. */
void sub_002C26A5(void)
{
    g_esp += 4;
}

/* IDirectSound_CommitDeferredSettings(pDS) */
void sub_002C379E(void) { RETURN(4, DS_OK); }

/* ── DSP effects image ─────────────────────────────────────────────────
 * The title loads a DSP program (reverb and the like) and then reads and
 * writes its parameters by effect index. We return a descriptor whose state
 * segments are plain memory, so parameter traffic works, and record it. */

#define FX_COUNT       32
#define FX_STATE_BYTES 4096
static uint32_t fx_desc;

/* IDirectSound_DownloadEffectsImage(pDS, pvImage, dwSize, pImageLoc, ppDesc) */
void sub_002C2446(void)
{
    uint32_t pp = ARG(4);

    note_once("DSP effects image (reverb)");
    TRACE("DownloadEffectsImage %u bytes at 0x%08x, image loc 0x%08x", ARG(2), ARG(1), ARG(3));
    if (!fx_desc) {
        fx_desc = guest_alloc(8 + FX_COUNT * 32);
        if (!fx_desc)
            RETURN(20, DSERR_OUTOFMEMORY);
        MEM32(fx_desc) = FX_COUNT;
        for (int i = 0; i < FX_COUNT; i++) {
            uint32_t map = fx_desc + 8 + (uint32_t)i * 32;
            MEM32(map + 8) = guest_alloc(FX_STATE_BYTES);    /* lpvStateSegment */
            MEM32(map + 12) = FX_STATE_BYTES;
        }
    }
    if (pp)
        MEM32(pp) = fx_desc;
    RETURN(20, DS_OK);
}

static uint32_t fx_state(uint32_t index, uint32_t offset, uint32_t size)
{
    uint32_t map;
    if (!fx_desc || index >= FX_COUNT || offset > FX_STATE_BYTES || size > FX_STATE_BYTES - offset)
        return 0;
    map = fx_desc + 8 + index * 32;
    return MEM32(map + 8) + offset;
}

/* IDirectSound_GetEffectData(pDS, index, offset, pvData, size) */
void sub_002C246D(void)
{
    uint32_t src = fx_state(ARG(1), ARG(2), ARG(4)), dst = ARG(3), n = ARG(4);
    if (!src || !dst)
        RETURN(20, DSERR_INVALIDCALL);
    for (uint32_t i = 0; i < n; i++)
        MEM8(dst + i) = MEM8(src + i);
    RETURN(20, DS_OK);
}

/* IDirectSound_SetEffectData(pDS, index, offset, pvData, size, dwApply) */
void sub_002C2494(void)
{
    uint32_t dst = fx_state(ARG(1), ARG(2), ARG(4)), src = ARG(3), n = ARG(4);
    TRACE("SetEffectData effect %u offset %u, %u bytes", ARG(1), ARG(2), n);
    if (!dst || !src)
        RETURN(24, DSERR_INVALIDCALL);
    for (uint32_t i = 0; i < n; i++)
        MEM8(dst + i) = MEM8(src + i);
    RETURN(24, DS_OK);
}

/* ── 3D sound ─────────────────────────────────────────────────────────
 * DSBCAPS_CTRL3D buffers are placed relative to the listener: DirectSound's
 * distance model (full volume up to the minimum distance, then
 * min / (min + rolloff * (d - min)), held from the maximum distance on) and
 * an equal-power left/right pan from the listener's orientation, or on
 * headphones (the output setting) a model head, or on 5.1 a pan around the
 * four corner speakers. No cones or Doppler (Orta sets cones of 360 degrees
 * and no velocity). Deferred settings apply at once. Positions are in the
 * world, the listener moving with the player; enemy gunfire is played at
 * the shooter, within the minimum distance of the player, so at full
 * volume. On the Xbox the left/right placement is the APU's per-voice HRTF
 * filter instead of a pan (docs/orta-facts.md, "DirectSound's 3D voice
 * volumes"). */

static float fget(uint32_t va)
{
    uint32_t v = MEM32(va);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float fval(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void apply_3d(int id)
{
    float rel[3], right[3], d, g, x = 0.0f, l, r;
    const float *f = listener.front, *t = listener.top;

    if (id <= 0 || id >= MAX_3D || !src3d[id].used)
        return;
    if (src3d[id].mode == DS3DMODE_DISABLE) {
        pdo_voice_set_3d_distance(id, 1.0f);
        pdo_voice_set_3d(id, 1.0f, 1.0f);
        return;
    }
    for (int k = 0; k < 3; k++)
        rel[k] = src3d[id].pos[k] - (src3d[id].mode == DS3DMODE_HEADRELATIVE ? 0.0f : listener.pos[k]);
    d = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    {
        float mn = src3d[id].min_dist > 0 ? src3d[id].min_dist : 1.0f;
        float mx = src3d[id].max_dist > mn ? src3d[id].max_dist : 1e9f;
        float dc = d < mn ? mn : d > mx ? mx : d;
        g = mn / (mn + listener.rolloff * (dc - mn));
    }
    pdo_voice_set_3d_distance(id, g);
    /* Right = top x front (D3D is left-handed); head-relative uses the
     * listener's own axes, which are the identity there. */
    if (src3d[id].mode == DS3DMODE_HEADRELATIVE) {
        right[0] = 1; right[1] = 0; right[2] = 0;
    } else {
        right[0] = t[1] * f[2] - t[2] * f[1];
        right[1] = t[2] * f[0] - t[0] * f[2];
        right[2] = t[0] * f[1] - t[1] * f[0];
    }
    if (d > 1e-6f) {
        float n = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (n > 1e-6f)
            x = (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / (d * n);
    }
    /* Headphones (a model head) and 5.1 (around four speakers), the output
     * setting: both need ahead or behind too. */
    if (pdo_settings_get()->audio_out != PDO_AUDIO_STEREO) {
        float fz = 0.0f;
        const float fr[3] = { 0.0f, 0.0f, 1.0f };
        const float *ff = src3d[id].mode == DS3DMODE_HEADRELATIVE ? fr : f;
        float n = sqrtf(ff[0] * ff[0] + ff[1] * ff[1] + ff[2] * ff[2]);
        if (d > 1e-6f && n > 1e-6f)
            fz = (rel[0] * ff[0] + rel[1] * ff[1] + rel[2] * ff[2]) / (d * n);
        if (pdo_settings_get()->audio_out == PDO_AUDIO_SURROUND)
            pdo_voice_set_3d_surround(id, g, x, fz);
        else
            pdo_voice_set_3d_ears(id, g, x, fz);
        return;
    }
    /* Equal-power pan: a source straight ahead is -3 dB in each ear. */
    l = sqrtf(0.5f * (1.0f - x));
    r = sqrtf(0.5f * (1.0f + x));
    pdo_voice_set_3d(id, g * l, g * r);
}

static void apply_3d_all(void)
{
    for (int id = 1; id < MAX_3D; id++)
        if (src3d[id].used)
            apply_3d(id);
}

/* IDirectSound_SetAllParameters(pDS, pds3dl, dwApply). DS3DLISTENER:
 * dwSize, vPosition, vVelocity, vOrientFront, vOrientTop,
 * flDistanceFactor, flRolloffFactor, flDopplerFactor. */
void sub_002C43BD(void)
{
    uint32_t l = ARG(1);
    if (l) {
        for (int k = 0; k < 3; k++) {
            listener.pos[k] = fget(l + 4 + 4 * k);
            listener.front[k] = fget(l + 28 + 4 * k);
            listener.top[k] = fget(l + 40 + 4 * k);
        }
        listener.rolloff = fget(l + 56);
        TRACE("listener pos %g %g %g front %g %g %g rolloff %g", listener.pos[0], listener.pos[1],
              listener.pos[2], listener.front[0], listener.front[1], listener.front[2], listener.rolloff);
        apply_3d_all();
    }
    RETURN(12, DS_OK);
}
void sub_002C43DD(void) { RETURN(12, DS_OK); }  /* SetDistanceFactor(pDS, f, apply): Doppler only */
void sub_002C4401(void) { RETURN(12, DS_OK); }  /* SetDopplerFactor */

/* IDirectSound_SetOrientation(pDS, xFront, yFront, zFront, xTop, yTop, zTop, apply) */
void sub_002C4425(void)
{
    for (int k = 0; k < 3; k++) {
        listener.front[k] = fval(ARG(1 + k));
        listener.top[k] = fval(ARG(4 + k));
    }
    apply_3d_all();
    RETURN(32, DS_OK);
}

/* IDirectSound_SetPosition(pDS, x, y, z, apply) */
void sub_002C446F(void)
{
    for (int k = 0; k < 3; k++)
        listener.pos[k] = fval(ARG(1 + k));
    TRACE("listener SetPosition %g %g %g", listener.pos[0], listener.pos[1], listener.pos[2]);
    apply_3d_all();
    RETURN(20, DS_OK);
}

/* IDirectSound_SetRolloffFactor(pDS, f, apply) */
void sub_002C44A4(void)
{
    listener.rolloff = fval(ARG(1));
    apply_3d_all();
    RETURN(12, DS_OK);
}
void sub_002C44C8(void) { RETURN(20, DS_OK); }  /* SetVelocity */
/* IDirectSound_SetI3DL2Listener(pDS, pds3dl, dwApply). DSI3DL2LISTENER:
 * lRoom, lRoomHF, flRoomRolloffFactor, flDecayTime, flDecayHFRatio,
 * lReflections, flReflectionsDelay, lReverb, flReverbDelay, flDiffusion,
 * flDensity, flHFReference. */
void sub_002C44FD(void)
{
    uint32_t p = ARG(1);

    if (p) {
        pdo_i3dl2 env = {
            .room = (int32_t)MEM32(p), .room_hf = (int32_t)MEM32(p + 4),
            .room_rolloff = fval(MEM32(p + 8)), .decay_time = fval(MEM32(p + 12)),
            .decay_hf_ratio = fval(MEM32(p + 16)), .reflections = (int32_t)MEM32(p + 20),
            .reflections_delay = fval(MEM32(p + 24)), .reverb = (int32_t)MEM32(p + 28),
            .reverb_delay = fval(MEM32(p + 32)), .diffusion = fval(MEM32(p + 36)),
            .density = fval(MEM32(p + 40)), .hf_reference = fval(MEM32(p + 44)),
        };
        pdo_mixer_set_i3dl2(&env);
    }
    if (p)
        TRACE("SetI3DL2Listener room %d hf %d rolloff %.2f decay %.2f s hf ratio %.2f "
              "reflections %d delay %.3f reverb %d delay %.3f diffusion %.0f density %.0f "
              "hf ref %.0f", (int32_t)MEM32(p), (int32_t)MEM32(p + 4), fval(MEM32(p + 8)),
              fval(MEM32(p + 12)), fval(MEM32(p + 16)), (int32_t)MEM32(p + 20),
              fval(MEM32(p + 24)), (int32_t)MEM32(p + 28), fval(MEM32(p + 32)),
              fval(MEM32(p + 36)), fval(MEM32(p + 40)), fval(MEM32(p + 44)));
    RETURN(12, DS_OK);
}

/* ── IDirectSoundBuffer ───────────────────────────────────────────────── */

/* IDirectSoundBuffer_AddRef(pBuf) */
void sub_002C12AE(void)
{
    uint32_t b = ARG(0);
    RETURN(4, voice_of(b) ? (MEM32(b + OBJ_REFS) += 1) : 0);
}

/* ── 5.1 movie soundtracks (Output: 5.1; PDO_MOVIE51=0 turns them off) ───
 * With a surround console reported, a movie that has an .aix (opening_bs,
 * ins1_bs, ins2_bs) plays its 5.1 soundtrack: CRI makes a stereo buffer per
 * speaker pair at 48 kHz and routes them front (mixbins 0/1), back (4/5) and
 * centre/LFE (2/3), the .aix's streams 0, 1 and 2 in that order. CRI's own
 * decoding under us fills only some of the six channels, so we decode the
 * .aix (src/audio/aix.c) and overwrite each chunk CRI unlocks with the same
 * stretch of the soundtrack: CRI still paces the writes to the picture. A
 * chunk at the offset CRI wrote last is a rewrite of it (CRI locks each
 * chunk twice); any other is the next stretch. */
static pdo_aix movie_aix;
static struct { uint32_t buf; int stream; uint32_t last_off, last_pos, last_frames; bool any; } movie_out[4];

static bool test_ac3(void)
{
    const char *t = getenv("PDO_MOVIE51");
    return pdo_settings_get()->audio_out == PDO_AUDIO_SURROUND && !(t && *t == '0');
}

static void movie51_start(uint32_t name_va)
{
    char name[128], path[PDO_PATH_MAX];
    const char *base;
    size_t n = 0;

    pdo_aix_free(&movie_aix);
    memset(movie_out, 0, sizeof movie_out);
    if (!name_va)
        return;
    while (n + 1 < sizeof name && MEM8(name_va + n))
        name[n] = (char)MEM8(name_va + n), n++;
    name[n] = 0;
    base = name;
    for (const char *c = name; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':')
            base = c + 1;
    static const char *dirs[] = { "Movie", "movie", "MOVIE" };
    for (size_t i = 0; i < SDL_arraysize(dirs); i++) {
        snprintf(path, sizeof path, "%s/%s/%s", pdo_guest_game_dir(), dirs[i], base);
        if (pdo_aix_load(path, &movie_aix)) {
            SDL_Log("dsound: 5.1 movie soundtrack %s, %d streams, %.1f s", base, movie_aix.streams,
                    movie_aix.rate[0] ? movie_aix.frames[0] / (double)movie_aix.rate[0] : 0.0);
            return;
        }
    }
    SDL_Log("dsound: 5.1 movie soundtrack %s (%s) not found or not decodable", base, name);
}

/* SetMixBins on a movie buffer: which .aix stream it plays. */
static void movie51_route(uint32_t buf, uint32_t first_bin)
{
    int stream = first_bin == 0 ? 0 : first_bin == 4 ? 1 : first_bin == 2 ? 2 : -1;
    int slot = -1;

    if (!movie_aix.streams || stream < 0 || stream >= movie_aix.streams || movie_aix.channels[stream] != 2)
        return;
    for (int i = 0; i < 4 && slot < 0; i++)
        if (movie_out[i].buf == buf)
            slot = i;
    for (int i = 0; i < 4 && slot < 0; i++)
        if (!movie_out[i].buf)
            slot = i;
    if (slot < 0)
        return;
    if (movie_out[slot].buf != buf || movie_out[slot].stream != stream)
        SDL_Log("dsound: 5.1 movie buffer 0x%08x plays soundtrack stream %d", buf, stream);
    else
        return;                     /* the same routing again: keep the position */
    memset(&movie_out[slot], 0, sizeof movie_out[slot]);
    movie_out[slot].buf = buf;
    movie_out[slot].stream = stream;
}

static void movie51_forget(uint32_t buf)
{
    for (int i = 0; i < 4; i++)
        if (movie_out[i].buf == buf)
            memset(&movie_out[i], 0, sizeof movie_out[i]);
}

/* Unlock of a chunk CRI wrote: the soundtrack over it. */
static void movie51_fill(uint32_t buf, uint32_t p1, uint32_t n1, uint32_t p2, uint32_t n2)
{
    for (int i = 0; i < 4; i++) {
        if (!buf || movie_out[i].buf != buf)
            continue;
        uint32_t data = MEM32(buf + OBJ_DATA), off = p1 - data, frames = (n1 + n2) / 4;
        const int16_t *pcm = movie_aix.pcm[movie_out[i].stream];
        uint32_t total = movie_aix.frames[movie_out[i].stream], pos;

        if (!data || !p1)
            return;
        pos = movie_out[i].any && off != movie_out[i].last_off ? movie_out[i].last_pos + movie_out[i].last_frames
                                                               : movie_out[i].last_pos;
        if (!movie_out[i].any)
            SDL_Log("dsound: 5.1 movie buffer 0x%08x: soundtrack written from frame %u", buf, pos);
        movie_out[i].any = true;
        movie_out[i].last_off = off;
        movie_out[i].last_pos = pos;
        movie_out[i].last_frames = frames;
        for (uint32_t f = 0; f < frames; f++) {
            uint32_t at = f < n1 / 4 ? p1 + f * 4 : p2 + (f - n1 / 4) * 4;
            int16_t l = 0, r = 0;
            if (pos + f < total) {
                l = pcm[(size_t)(pos + f) * 2];
                r = pcm[(size_t)(pos + f) * 2 + 1];
            }
            MEM16(at) = (uint16_t)l;
            MEM16(at + 2) = (uint16_t)r;
        }
        return;
    }
}

/* IDirectSoundBuffer_Release(pBuf) */
void sub_002C12C4(void)
{
    uint32_t b = ARG(0), refs = 0;
    int id = voice_of(b);

    if (id && MEM32(b + OBJ_REFS)) {
        refs = (MEM32(b + OBJ_REFS) -= 1);
        if (!refs) {
            movie51_forget(b);
            if (id < MAX_3D)
                src3d[id].used = false;
            pdo_voice_destroy(id);
            MEM32(b + OBJ_TAG) = 0;
            if (MEM32(b + OBJ_DATA))
                xbox_HeapFree(MEM32(b + OBJ_DATA));
            xbox_HeapFree(b);
        }
    }
    TRACE("Release 0x%08x (voice %d) -> %u", b, id, refs);
    RETURN(4, refs);
}

/* IDirectSoundBuffer_SetBufferData(pBuf, pvData, dwBytes): play the game's
 * own memory. */
void sub_002C451D(void)
{
    int id = voice_of(ARG(0));
    if (!id)
        RETURN(12, DSERR_INVALIDCALL);
    pdo_voice_set_data(id, ARG(1), ARG(2));
    TRACE("SetBufferData 0x%08x: 0x%08x, %u bytes", ARG(0), ARG(1), ARG(2));
    RETURN(12, DS_OK);
}

/* IDirectSoundBuffer_Lock(pBuf, offset, bytes, ppv1, pdw1, ppv2, pdw2, flags) */
void sub_002C2652(void)
{
    uint32_t b = ARG(0), off = ARG(1), n = ARG(2), flags = ARG(7);
    uint32_t data = MEM32(b + OBJ_DATA), size = MEM32(b + OBJ_BYTES);
    uint32_t n1, n2;
    int id = voice_of(b);

    if (!id || !data || !size)
        RETURN(32, DSERR_INVALIDCALL);
    if (flags & 2)              /* DSBLOCK_ENTIREBUFFER */
        off = 0, n = size;
    else if (flags & 1)         /* DSBLOCK_FROMWRITECURSOR */
        off = pdo_voice_position(id);
    off %= size;
    if (n > size)
        n = size;
    n1 = size - off < n ? size - off : n;
    n2 = n - n1;
    TRACE("Lock 0x%08x offset %u, %u bytes, flags 0x%x", b, off, n, flags);
    if (ARG(3)) MEM32(ARG(3)) = data + off;
    if (ARG(4)) MEM32(ARG(4)) = n1;
    if (ARG(5)) MEM32(ARG(5)) = n2 ? data : 0;
    if (ARG(6)) MEM32(ARG(6)) = n2;
    RETURN(32, DS_OK);
}

/* IDirectSoundBuffer_Unlock(pBuf, pv1, dw1, pv2, dw2): the voice reads the
 * memory; a 5.1 movie buffer gets its soundtrack written over (above). */
void sub_002C12A9(void)
{
    if (movie_aix.streams)
        movie51_fill(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4));
    RETURN(20, DS_OK);
}

/* IDirectSoundBuffer_Play(pBuf, reserved1, reserved2, flags) */
void sub_002C2582(void)
{
    int id = voice_of(ARG(0));
    if (!id)
        RETURN(16, DSERR_INVALIDCALL);
    if (ARG(3) & 2)             /* DSBPLAY_FROMSTART */
        pdo_voice_set_position(id, 0);
    pdo_voice_play(id, (ARG(3) & 1) != 0);
    TRACE("Play 0x%08x flags 0x%x", ARG(0), ARG(3));
    RETURN(16, DS_OK);
}

/* IDirectSoundBuffer_Stop(pBuf) */
void sub_002C25A6(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_stop(id);
    TRACE("Stop 0x%08x", ARG(0));
    RETURN(4, DS_OK);
}

/* IDirectSoundBuffer_GetStatus(pBuf, pdwStatus) */
void sub_002C25FA(void)
{
    int id = voice_of(ARG(0));
    bool looping = false, playing = id && pdo_voice_playing(id, &looping);
    if (ARG(1))
        MEM32(ARG(1)) = (playing ? 1u : 0u) | (playing && looping ? 4u : 0u);
    TRACE("GetStatus 0x%08x -> %s", ARG(0), playing ? "playing" : "stopped");
    RETURN(8, id ? DS_OK : DSERR_INVALIDCALL);
}

/* IDirectSoundBuffer_GetCurrentPosition(pBuf, pdwPlay, pdwWrite) */
void sub_002C2616(void)
{
    int id = voice_of(ARG(0));
    uint32_t pos = id ? pdo_voice_position(id) : 0;
    TRACE("GetCurrentPosition 0x%08x -> %u", ARG(0), pos);
    if (ARG(1)) MEM32(ARG(1)) = pos;
    if (ARG(2)) MEM32(ARG(2)) = pos;
    RETURN(12, id ? DS_OK : DSERR_INVALIDCALL);
}

/* IDirectSoundBuffer_SetCurrentPosition(pBuf, dwOffset) */
void sub_002C2636(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_set_position(id, ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetLoopRegion(pBuf, dwStart, dwLength) */
void sub_002C25DA(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_set_loop_region(id, ARG(1), ARG(2));
    TRACE("SetLoopRegion 0x%08x %u, %u bytes", ARG(0), ARG(1), ARG(2));
    RETURN(12, DS_OK);
}

/* IDirectSoundBuffer_SetVolume(pBuf, lVolume) */
void sub_002C24BE(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_set_volume(id, (int32_t)ARG(1));
    TRACE("SetVolume 0x%08x %d", ARG(0), (int32_t)ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetHeadroom(pBuf, dwHeadroom) */
void sub_002C252E(void)
{
    int id = voice_of(ARG(0));
    if (id)
        pdo_voice_set_headroom(id, ARG(1));
    TRACE("SetHeadroom 0x%08x %u", ARG(0), ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetFrequency(pBuf, dwFrequency) */
void sub_002C37B6(void)
{
    int id = voice_of(ARG(0));
    TRACE("SetFrequency 0x%08x %u Hz", ARG(0), ARG(1));
    if (id)
        pdo_voice_set_frequency(id, ARG(1));
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetMixBins(pBuf, pMixBins) */
void sub_002C254A(void)
{
    int id = voice_of(ARG(0));
    uint32_t mb = ARG(1);
    pdo_voice_bin bins[PDO_MAX_VOICE_BINS];
    int n = 0;

    if (id && mb) {
        uint32_t count = MEM32(mb), pairs = MEM32(mb + 4);
        for (uint32_t i = 0; i < count && n < PDO_MAX_VOICE_BINS && pairs; i++, n++) {
            bins[n].bin = MEM32(pairs + i * 8);
            bins[n].volume = (int32_t)MEM32(pairs + i * 8 + 4);
        }
        pdo_voice_set_bins(id, bins, n);
        if (n && test_ac3())
            movie51_route(ARG(0), bins[0].bin);
    }
    for (int i = 0; i < n; i++)
        TRACE("  0x%08x bin %u volume %d", ARG(0), bins[i].bin, bins[i].volume);
    RETURN(8, DS_OK);
}

void sub_002C24F6(void) { note_once("envelopes (SetEG)"); RETURN(8, DS_OK); }
void sub_002C24DA(void) { note_once("LFOs (SetLFO)"); RETURN(8, DS_OK); }
void sub_002C2512(void) { note_once("filters (SetFilter)"); RETURN(8, DS_OK); }

/* Per-buffer 3D state (see "3D sound" above). */
static int src_of(uint32_t buf)
{
    int id = voice_of(buf);
    return id > 0 && id < MAX_3D && src3d[id].used ? id : 0;
}

/* IDirectSoundBuffer_SetAllParameters(pBuf, pds3db, apply). DS3DBUFFER:
 * dwSize, vPosition, vVelocity, dwInsideConeAngle, dwOutsideConeAngle,
 * vConeOrientation, lConeOutsideVolume, flMinDistance, flMaxDistance,
 * dwMode. */
void sub_002C37D2(void)
{
    int id = src_of(ARG(0));
    uint32_t p = ARG(1);
    if (id && p) {
        for (int k = 0; k < 3; k++)
            src3d[id].pos[k] = fget(p + 4 + 4 * k);
        src3d[id].min_dist = fget(p + 52);
        src3d[id].max_dist = fget(p + 56);
        src3d[id].mode = MEM32(p + 60);
        TRACE("3D 0x%08x pos %g %g %g dist %g-%g mode %u", ARG(0), src3d[id].pos[0], src3d[id].pos[1],
              src3d[id].pos[2], src3d[id].min_dist, src3d[id].max_dist, src3d[id].mode);
        apply_3d(id);
    }
    RETURN(12, DS_OK);
}
void sub_002C37F2(void) { RETURN(16, DS_OK); }  /* SetConeAngles */
void sub_002C3816(void) { RETURN(20, DS_OK); }  /* SetConeOrientation */
void sub_002C384B(void) { RETURN(12, DS_OK); }  /* SetConeOutsideVolume */

/* IDirectSoundBuffer_SetMaxDistance(pBuf, f, apply) */
void sub_002C386B(void)
{
    int id = src_of(ARG(0));
    if (id) { src3d[id].max_dist = fval(ARG(1)); apply_3d(id); }
    RETURN(12, DS_OK);
}

/* IDirectSoundBuffer_SetMinDistance(pBuf, f, apply) */
void sub_002C388F(void)
{
    int id = src_of(ARG(0));
    if (id) { src3d[id].min_dist = fval(ARG(1)); apply_3d(id); }
    RETURN(12, DS_OK);
}

/* IDirectSoundBuffer_SetMode(pBuf, dwMode, apply) */
void sub_002C38B3(void)
{
    int id = src_of(ARG(0));
    if (id) { src3d[id].mode = ARG(1); apply_3d(id); }
    RETURN(12, DS_OK);
}

/* IDirectSoundBuffer_SetPosition(pBuf, x, y, z, apply) */
void sub_002C38D3(void)
{
    int id = src_of(ARG(0));
    if (id) {
        for (int k = 0; k < 3; k++)
            src3d[id].pos[k] = fval(ARG(1 + k));
        TRACE("3D 0x%08x SetPosition %g %g %g apply %u", ARG(0), src3d[id].pos[0], src3d[id].pos[1],
              src3d[id].pos[2], ARG(4));
        apply_3d(id);
    }
    RETURN(20, DS_OK);
}
void sub_002C3908(void) { RETURN(20, DS_OK); }  /* SetVelocity */
void sub_002C393D(void) { RETURN(12, DS_OK); }  /* SetDistanceFactor */
void sub_002C3961(void) { RETURN(12, DS_OK); }  /* SetDopplerFactor */
void sub_002C3985(void) { RETURN(12, DS_OK); }  /* SetRolloffFactor */
/* SetI3DL2Source(pBuf, pds3db, dwApply). DSI3DL2BUFFER: lDirect, lDirectHF,
 * lRoom, lRoomHF, flRoomRolloffFactor, Obstruction {lHFLevel, flLFRatio},
 * Occlusion {lHFLevel, flLFRatio}. */
static void trace_i3dl2_source(const char *what, uint32_t obj, uint32_t p)
{
    if (p)
        TRACE("%s 0x%08x direct %d hf %d room %d hf %d rolloff %.2f obstruction %d %.2f "
              "occlusion %d %.2f", what, obj, (int32_t)MEM32(p), (int32_t)MEM32(p + 4),
              (int32_t)MEM32(p + 8), (int32_t)MEM32(p + 12), fval(MEM32(p + 16)),
              (int32_t)MEM32(p + 20), fval(MEM32(p + 24)), (int32_t)MEM32(p + 28),
              fval(MEM32(p + 32)));
}
/* IDirectSoundBuffer_SetI3DL2Source */
void sub_002C39A9(void)
{
    trace_i3dl2_source("buffer SetI3DL2Source", ARG(0), ARG(1));
    if (ARG(1))
        pdo_voice_set_i3dl2(voice_of(ARG(0)), (int32_t)MEM32(ARG(1)), (int32_t)MEM32(ARG(1) + 8));
    RETURN(12, DS_OK);
}

/* ── Calls Orta makes that JSRF did not ───────────────────────────────── */

/* IDirectSound_GetSpeakerConfig(pDS, pdwSpeakerConfig). Stereo, or with
 * Output on 5.1 a surround console with Dolby Digital (DSSPEAKER_SURROUND |
 * DSSPEAKER_ENABLE_AC3): the movie setup (0x000687B0, its only caller)
 * then keeps the AC3 bit and plays a movie's 5.1 .aix soundtrack
 * (opening_bs, ins1_bs, ins2_bs) over the .sfd's stereo; see "5.1 movie
 * soundtracks". PDO_MOVIE51=0 keeps them stereo. */
void sub_002C242A(void)
{
    uint32_t cfg = test_ac3() ? 0x00010004u : 0x00000002u;    /* or DSSPEAKER_STEREO */

    /* From the movie setup (call at 0x000687E4): ebx holds the .aix name,
     * or 0 for a movie with only the .sfd's stereo. */
    if (test_ac3() && MEM32(g_esp) == 0x000687E9u)
        movie51_start(g_ebx);
    TRACE("GetSpeakerConfig -> 0x%08x", cfg);
    if (ARG(1))
        MEM32(ARG(1)) = cfg;
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_SetMixBinVolumes(pBuf, pMixBins): new volumes for the
 * listed bins. Our voices hold only the bins they route to, so this sets
 * the same list (the game passes the whole set). */
void sub_002C2566(void)
{
    int id = voice_of(ARG(0));
    uint32_t mb = ARG(1);
    pdo_voice_bin bins[PDO_MAX_VOICE_BINS];
    int n = 0;

    if (id && mb) {
        uint32_t count = MEM32(mb), pairs = MEM32(mb + 4);
        for (uint32_t i = 0; i < count && n < PDO_MAX_VOICE_BINS && pairs; i++, n++) {
            bins[n].bin = MEM32(pairs + i * 8);
            bins[n].volume = (int32_t)MEM32(pairs + i * 8 + 4);
        }
        pdo_voice_set_bins(id, bins, n);
        if (n && test_ac3())
            movie51_route(ARG(0), bins[0].bin);
    }
    for (int i = 0; i < n; i++)
        TRACE("  0x%08x bin %u volume %d", ARG(0), bins[i].bin, bins[i].volume);
    RETURN(8, DS_OK);
}

/* IDirectSoundBuffer_Pause(pBuf, dwPause): DSBPAUSE_RESUME 0, DSBPAUSE_PAUSE
 * 1, DSBPAUSE_SYNCHPAUSE 2. Pausing keeps the position; resuming restarts
 * only a paused buffer. Restarting any stopped one brought back looping
 * gunshots the game had stopped, every time its sound code resumed its
 * buffers, until they piled up. */
static void pause_voice(int id, uint32_t how)
{
    if (id)
        pdo_voice_pause(id, how != 0);
}

void sub_002C25BE(void)
{
    pause_voice(voice_of(ARG(0)), ARG(1));
    TRACE("Pause 0x%08x %u", ARG(0), ARG(1));
    RETURN(8, DS_OK);
}

/* ── IDirectSoundStream ───────────────────────────────────────────────────
 * A stream is an XMediaObject: the game hands it packets with Process and
 * polls their status. Our stream objects carry the library's own vtable
 * (CDirectSoundStream, 0x0032DD9C), whose methods are replaced below, so
 * calls through the vtable and through the C wrappers both land here.
 *
 * Packets play back to back on a mixer voice (pdo_voice_queue). The
 * descriptor the game passes may be on its stack, so each packet's
 * pdwCompletedSize, pdwStatus and completion event are copied when it is
 * queued, and written from the mixer thread when it has played. */

#define TAG_STREAM          0x53524450u     /* "PDRS" */
#define STREAM_VTABLE       0x0032DD9Cu

#define STR_VTBL    0
#define STR_TAG     4
#define STR_VOICE   8
#define STR_REFS    12
#define STR_INDEX   16
#define STR_SIZE    32

#define XMEDIAPACKET_STATUS_SUCCESS 0x00000000u
#define XMEDIAPACKET_STATUS_PENDING 0x8000000Au
#define XMEDIAPACKET_STATUS_FLUSHED 0x80004004u
#define XMO_STATUSF_ACCEPT_INPUT_DATA 0x00000001u

#define MAX_STREAMS     32
#define STREAM_PACKETS  64

typedef struct stream_packet {
    uint32_t completed_size, status, event, bytes;
} stream_packet;

typedef struct stream {
    uint32_t      obj;
    int           voice;
    uint32_t      max_packets;
    uint32_t      callback, context;
    uint32_t      block_align;
    uint32_t      next;                 /* next packet slot */
    stream_packet packets[STREAM_PACKETS];
} stream;

static stream streams[MAX_STREAMS];

static stream *stream_of(uint32_t obj)
{
    uint32_t i;

    if (!obj || MEM32(obj + STR_TAG) != TAG_STREAM)
        return NULL;
    i = MEM32(obj + STR_INDEX);
    return i < MAX_STREAMS && streams[i].obj == obj ? &streams[i] : NULL;
}

/* Mixer thread: a packet has been played, or flushed. */
static void packet_done(void *user, uint32_t tag, bool flushed)
{
    stream *s = user;
    stream_packet *p = &s->packets[tag % STREAM_PACKETS];

    if (p->completed_size)
        MEM32(p->completed_size) = flushed ? 0 : p->bytes;
    if (p->status)
        MEM32(p->status) = flushed ? XMEDIAPACKET_STATUS_FLUSHED : XMEDIAPACKET_STATUS_SUCCESS;
}

/* DirectSoundCreateStream(pdssd, ppStream). DSSTREAMDESC: dwFlags,
 * dwMaxAttachedPackets, lpwfxFormat, lpfnCallback, lpvContext, lpMixBins. */
void sub_002C4A34(void)
{
    uint32_t desc = ARG(0), pp = ARG(1), wfx, mixbins, obj;
    pdo_voice_desc vd;
    stream *s = NULL;
    int i;

    if (!desc || !pp)
        RETURN(8, DSERR_INVALIDCALL);
    for (i = 0; i < MAX_STREAMS && !s; i++)
        if (!streams[i].obj)
            s = &streams[i];
    if (!s)
        RETURN(8, DSERR_OUTOFMEMORY);
    if (!the_dsound)
        pdo_mixer_init();

    wfx = MEM32(desc + 8);
    mixbins = MEM32(desc + 20);
    memset(&vd, 0, sizeof vd);
    vd.format = PDO_FMT_PCM16;
    vd.channels = 2;
    vd.sample_rate = 48000;
    if (wfx) {
        uint16_t tag = MEM16(wfx), bits = MEM16(wfx + 14);
        vd.channels = MEM16(wfx + 2) == 1 ? 1 : 2;
        vd.sample_rate = MEM32(wfx + 4);
        if (tag == 0x0069) {
            vd.format = PDO_FMT_XADPCM;
            vd.samples_per_block = MEM16(wfx + 16) >= 2 ? MEM16(wfx + 18) : 64;
        } else {
            vd.format = bits == 8 ? PDO_FMT_PCM8 : PDO_FMT_PCM16;
            if (tag != 1)
                SDL_Log("dsound: stream format 0x%04x, treating as PCM", tag);
        }
    }
    if (mixbins && MEM32(mixbins)) {
        uint32_t n = MEM32(mixbins), pairs = MEM32(mixbins + 4);
        for (uint32_t k = 0; k < n && k < PDO_MAX_VOICE_BINS && pairs; k++) {
            vd.bins[vd.bin_count].bin = MEM32(pairs + k * 8);
            vd.bins[vd.bin_count].volume = (int32_t)MEM32(pairs + k * 8 + 4);
            vd.bin_count++;
        }
    } else {
        vd.bins[0].bin = PDO_BIN_FRONT_LEFT;
        vd.bins[1].bin = PDO_BIN_FRONT_RIGHT;
        vd.bin_count = 2;
    }

    memset(s, 0, sizeof *s);
    s->voice = pdo_voice_create(&vd);
    obj = guest_alloc(STR_SIZE);
    if (!s->voice || !obj)
        RETURN(8, DSERR_OUTOFMEMORY);
    s->obj = obj;
    s->max_packets = MEM32(desc + 4) ? MEM32(desc + 4) : 1;
    if (s->max_packets > STREAM_PACKETS)
        s->max_packets = STREAM_PACKETS;
    s->callback = MEM32(desc + 12);
    s->context = MEM32(desc + 16);
    s->block_align = pdo_voice_block_align(s->voice);
    MEM32(obj + STR_VTBL) = STREAM_VTABLE;
    MEM32(obj + STR_TAG) = TAG_STREAM;
    MEM32(obj + STR_VOICE) = (uint32_t)s->voice;
    MEM32(obj + STR_REFS) = 1;
    MEM32(obj + STR_INDEX) = (uint32_t)(s - streams);
    pdo_voice_set_packet_done(s->voice, packet_done, s);
    pdo_voice_play(s->voice, false);    /* plays packets as they arrive */
    if (s->callback)
        note_once("stream packet callbacks");
    MEM32(pp) = obj;
    SDL_Log("dsound: stream 0x%08x: %s %u ch %u Hz, %u packets", obj,
            vd.format == PDO_FMT_XADPCM ? "adpcm" : vd.format == PDO_FMT_PCM8 ? "pcm8" : "pcm16",
            vd.channels, vd.sample_rate, s->max_packets);
    RETURN(8, DS_OK);
}

/* CDirectSoundStream::AddRef(this) */
void sub_002C1D21(void)
{
    uint32_t o = ARG(0);
    RETURN(4, stream_of(o) ? (MEM32(o + STR_REFS) += 1) : 0);
}

/* CDirectSoundStream::Release(this) */
void sub_002C1D68(void)
{
    uint32_t o = ARG(0), refs = 0;
    stream *s = stream_of(o);

    if (s && MEM32(o + STR_REFS)) {
        refs = (MEM32(o + STR_REFS) -= 1);
        if (!refs) {
            pdo_voice_flush(s->voice);
            pdo_voice_destroy(s->voice);
            MEM32(o + STR_TAG) = 0;
            s->obj = 0;
            xbox_HeapFree(o);
        }
    }
    RETURN(4, refs);
}

/* CDirectSoundStream::GetInfo(this, pInfo): XMEDIAINFO { dwFlags,
 * dwInputSize, dwOutputSize, dwMaxLookahead }. */
void sub_002C1DB6(void)
{
    stream *s = stream_of(ARG(0));
    uint32_t info = ARG(1);

    if (!s || !info)
        RETURN(8, DSERR_INVALIDCALL);
    MEM32(info + 0) = 0x00000005u;      /* XMO_STREAMF_FIXED_SAMPLE_SIZE | INPUT_ASYNC */
    MEM32(info + 4) = s->block_align;
    MEM32(info + 8) = 0;
    MEM32(info + 12) = 0;
    RETURN(8, DS_OK);
}

/* CDirectSoundStream::GetStatus(this, pdwStatus) */
void sub_002C1EB7(void)
{
    stream *s = stream_of(ARG(0));

    if (!s)
        RETURN(8, DSERR_INVALIDCALL);
    if (ARG(1))
        MEM32(ARG(1)) = (uint32_t)pdo_voice_queued(s->voice) < s->max_packets
                      ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0;
    RETURN(8, DS_OK);
}

/* CDirectSoundStream::Process(this, pInputPacket, pOutputPacket).
 * XMEDIAPACKET: pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus,
 * hCompletionEvent (pContext with a callback), prtTimestamp. */
void sub_002C1F08(void)
{
    stream *s = stream_of(ARG(0));
    uint32_t in = ARG(1), slot;
    stream_packet *p;

    if (!s || !in)
        RETURN(12, DSERR_INVALIDCALL);
    if ((uint32_t)pdo_voice_queued(s->voice) >= s->max_packets)
        RETURN(12, DSERR_INVALIDCALL);
    slot = s->next++ % STREAM_PACKETS;
    p = &s->packets[slot];
    p->bytes = MEM32(in + 4);
    p->completed_size = MEM32(in + 8);
    p->status = MEM32(in + 12);
    p->event = MEM32(in + 16);
    if (p->event && !s->callback)
        note_once("stream packet completion events");
    if (p->completed_size)
        MEM32(p->completed_size) = 0;
    if (p->status)
        MEM32(p->status) = XMEDIAPACKET_STATUS_PENDING;
    if (!pdo_voice_queue(s->voice, MEM32(in), p->bytes, slot)) {
        if (p->status)
            MEM32(p->status) = XMEDIAPACKET_STATUS_FLUSHED;
        RETURN(12, DSERR_INVALIDCALL);
    }
    TRACE("stream 0x%08x Process 0x%08x, %u bytes", ARG(0), MEM32(in), p->bytes);
    RETURN(12, DS_OK);
}

/* CDirectSoundStream::Discontinuity(this): the end of the data for now;
 * what is queued still plays. */
void sub_002C1E1D(void)
{
    RETURN(4, stream_of(ARG(0)) ? DS_OK : DSERR_INVALIDCALL);
}

/* CDirectSoundStream::Flush(this) */
void sub_002C1E6A(void)
{
    stream *s = stream_of(ARG(0));
    if (s)
        pdo_voice_flush(s->voice);
    RETURN(4, s ? DS_OK : DSERR_INVALIDCALL);
}

static int stream_voice(uint32_t obj)
{
    stream *s = stream_of(obj);
    return s ? s->voice : 0;
}

/* IDirectSoundStream_* setters (5-byte thunks into CDirectSoundStream, or
 * small wrappers), same arguments as the buffer ones. */
/* SetVolume(pStream, lVolume) */
void sub_002C2682(void)
{
    int id = stream_voice(ARG(0));
    if (id)
        pdo_voice_set_volume(id, (int32_t)ARG(1));
    TRACE("stream SetVolume 0x%08x %d", ARG(0), (int32_t)ARG(1));
    RETURN(8, DS_OK);
}

/* SetHeadroom(pStream, dwHeadroom) */
void sub_002C2696(void)
{
    int id = stream_voice(ARG(0));
    if (id)
        pdo_voice_set_headroom(id, ARG(1));
    TRACE("stream SetHeadroom 0x%08x %u", ARG(0), ARG(1));
    RETURN(8, DS_OK);
}

/* SetFrequency(pStream, dwFrequency) */
void sub_002C39C9(void)
{
    int id = stream_voice(ARG(0));
    if (id)
        pdo_voice_set_frequency(id, ARG(1));
    RETURN(8, DS_OK);
}

/* SetMixBins(pStream, pMixBins) */
void sub_002C269B(void)
{
    int id = stream_voice(ARG(0));
    uint32_t mb = ARG(1);
    pdo_voice_bin bins[PDO_MAX_VOICE_BINS];
    int n = 0;

    if (id && mb) {
        uint32_t count = MEM32(mb), pairs = MEM32(mb + 4);
        for (uint32_t i = 0; i < count && n < PDO_MAX_VOICE_BINS && pairs; i++, n++) {
            bins[n].bin = MEM32(pairs + i * 8);
            bins[n].volume = (int32_t)MEM32(pairs + i * 8 + 4);
        }
        pdo_voice_set_bins(id, bins, n);
    }
    for (int i = 0; i < n; i++)
        TRACE("  0x%08x bin %u volume %d", ARG(0), bins[i].bin, bins[i].volume);
    RETURN(8, DS_OK);
}

/* Pause(pStream, dwPause) */
void sub_002C26A0(void)
{
    pause_voice(stream_voice(ARG(0)), ARG(1));
    RETURN(8, DS_OK);
}

void sub_002C2687(void) { note_once("stream LFOs (SetLFO)"); RETURN(8, DS_OK); }
void sub_002C268C(void) { note_once("stream envelopes (SetEG)"); RETURN(8, DS_OK); }
void sub_002C2691(void) { note_once("stream filters (SetFilter)"); RETURN(8, DS_OK); }

/* Stream 3D state, recorded only for now. */
void sub_002C39CE(void) { note_once("3D streams"); RETURN(12, DS_OK); } /* SetAllParameters */
void sub_002C39D3(void) { RETURN(16, DS_OK); }  /* SetConeAngles */
void sub_002C39D8(void) { RETURN(20, DS_OK); }  /* SetConeOrientation */
void sub_002C3A01(void) { RETURN(12, DS_OK); }  /* SetConeOutsideVolume */
void sub_002C3A06(void) { RETURN(12, DS_OK); }  /* SetMaxDistance */
void sub_002C3A1E(void) { RETURN(12, DS_OK); }  /* SetMinDistance */
void sub_002C3A36(void) { RETURN(12, DS_OK); }  /* SetMode */
void sub_002C3A3B(void) { RETURN(20, DS_OK); }  /* SetPosition */
void sub_002C3A64(void) { RETURN(20, DS_OK); }  /* SetVelocity */
void sub_002C3A8D(void) { RETURN(12, DS_OK); }  /* SetDistanceFactor */
void sub_002C3AA5(void) { RETURN(12, DS_OK); }  /* SetDopplerFactor */
void sub_002C3ABD(void) { RETURN(12, DS_OK); }  /* SetRolloffFactor */
/* IDirectSoundStream_SetI3DL2Source */
void sub_002C3AD5(void)
{
    trace_i3dl2_source("stream SetI3DL2Source", ARG(0), ARG(1));
    if (ARG(1) && stream_of(ARG(0)))
        pdo_voice_set_i3dl2(stream_of(ARG(0))->voice, (int32_t)MEM32(ARG(1)),
                            (int32_t)MEM32(ARG(1) + 8));
    RETURN(12, DS_OK);
}
