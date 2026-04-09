// i_sound_makaos.c — AC97 software mixer for Doom on MakaOS.
//
// Sound effects: up to MIX_CHANNELS simultaneous channels.
//   - WAD samples are 8-bit unsigned mono at varying rates (typically 11025 Hz).
//   - We convert on load to signed 16-bit mono at OUTPUT_RATE, stored in
//     channel_t.samples / nsamples.
//   - Each call to I_UpdateSound() advances all active channels by one tic's
//     worth of samples (OUTPUT_RATE / DOOM_HZ), mixes them with volume/pan,
//     and writes the resulting stereo 16-bit PCM to /dev/dsp.
//
// Music: stubbed (no MIDI synthesiser yet).
//
// Output format: signed 16-bit stereo interleaved, OUTPUT_RATE Hz.

#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/stdint.h"
#include "include/stdbool.h"
#include "include/fcntl.h"
#include "include/unistd.h"
#include "doomgeneric/doomgeneric/doomtype.h"
#include "doomgeneric/doomgeneric/i_sound.h"
#include "doomgeneric/doomgeneric/w_wad.h"
#include "doomgeneric/doomgeneric/z_zone.h"

// ── Output parameters ─────────────────────────────────────────────────────

#define OUTPUT_RATE   48000   // Hz — must match AC97_SAMPLE_RATE in kernel
#define OUTPUT_CH     2       // stereo
#define DOOM_HZ       35      // Doom tics per second

// Samples per tic (per channel), rounded up.
#define TIC_SAMPLES   ((OUTPUT_RATE + DOOM_HZ - 1) / DOOM_HZ)
// Bytes per tic: stereo * 2 bytes/sample.
#define TIC_BYTES     (TIC_SAMPLES * OUTPUT_CH * 2)

// ── Mixer ─────────────────────────────────────────────────────────────────

#define MIX_CHANNELS  8   // simultaneous sound effects

// sep: 0 = full left, 127 = centre, 254 = full right.
// vol: 0–127.
typedef struct {
    int16_t* samples;   // resampled signed 16-bit mono PCM (owned by sfxinfo->driver_data)
    uint32_t nsamples;  // total sample count
    uint32_t pos;       // current playback position (in samples)
    int      vol;       // 0–127
    int      sep;       // 0–254 panning
    boolean  active;
} channel_t;

static channel_t s_channels[MIX_CHANNELS];
static int       s_dsp_fd = -1;

// Mix buffer: one tic of stereo 16-bit samples.
static int16_t s_mix_buf[TIC_SAMPLES * OUTPUT_CH];

// ── WAD sound decoding ────────────────────────────────────────────────────
// WAD DMX format:
//   [0..1]  = 0x03 0x00  (format tag)
//   [2..3]  = sample rate (little-endian 16-bit)
//   [4..7]  = sample count (little-endian 32-bit)
//   [8..]   = 8-bit unsigned mono PCM (first 16 and last 16 bytes are padding)

// Convert a WAD sound lump to signed 16-bit mono at OUTPUT_RATE.
// Allocates memory via Z_Malloc (PU_STATIC) stored in sfxinfo->driver_data.
// Returns 1 on success, 0 on failure.
static boolean decode_sfx(sfxinfo_t* sfx) {
    if (sfx->driver_data) return 1;   // already decoded

    int      lumpnum = sfx->lumpnum;
    byte*    data    = (byte*)W_CacheLumpNum(lumpnum, PU_STATIC);
    uint32_t lumplen = (uint32_t)W_LumpLength(lumpnum);

    // Validate header.
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    uint32_t src_rate = (uint32_t)((data[3] << 8) | data[2]);
    uint32_t src_len  = (uint32_t)(data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24));

    // Sanity checks (mirrors i_sdlsound.c).
    if (src_len > lumplen - 8 || src_len <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    // Skip 16-byte lead-in and 16-byte lead-out padding per DMX convention.
    const byte* src_samples = data + 8 + 16;
    src_len -= 32;

    if (src_rate == 0) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    // Linear resampling: output sample count.
    uint32_t dst_len = (uint32_t)(((uint64_t)src_len * OUTPUT_RATE) / src_rate);
    if (dst_len == 0) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    int16_t* out = (int16_t*)Z_Malloc((int)(dst_len * sizeof(int16_t)), PU_STATIC, NULL);
    if (!out) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    // Linear interpolation resample: 8-bit unsigned → 16-bit signed.
    for (uint32_t i = 0; i < dst_len; i++) {
        // Fixed-point source position.
        uint64_t fp  = ((uint64_t)i * src_rate * 65536ULL) / OUTPUT_RATE;
        uint32_t idx = (uint32_t)(fp >> 16);
        uint32_t frac = (uint32_t)(fp & 0xFFFF);

        if (idx >= src_len - 1) {
            // Clamp to last sample.
            int s = (int)(src_samples[src_len - 1]) - 128;
            out[i] = (int16_t)(s * 256);
        } else {
            int s0 = (int)src_samples[idx]   - 128;
            int s1 = (int)src_samples[idx+1] - 128;
            int s  = s0 + (int)(((s1 - s0) * (int)frac) >> 16);
            out[i] = (int16_t)(s * 256);
        }
    }

    W_ReleaseLumpNum(lumpnum);

    // Store decoded audio alongside the sfxinfo.
    // We pack nsamples into the high 32 bits of a 64-bit pointer-sized value
    // by wrapping it in a small heap struct.
    typedef struct { int16_t* samples; uint32_t nsamples; } sfx_data_t;
    sfx_data_t* d = (sfx_data_t*)Z_Malloc((int)sizeof(sfx_data_t), PU_STATIC, NULL);
    if (!d) return 0;
    d->samples  = out;
    d->nsamples = dst_len;
    sfx->driver_data = d;
    return 1;
}

// ── mix_and_write ─────────────────────────────────────────────────────────
// Called once per tic via sfx_Update().  Mix all active channels into
// s_mix_buf and write to /dev/dsp.

static void mix_and_write(void) {
    if (s_dsp_fd < 0) return;

    // Zero the mix buffer.
    for (int i = 0; i < TIC_SAMPLES * OUTPUT_CH; i++) s_mix_buf[i] = 0;

    for (int c = 0; c < MIX_CHANNELS; c++) {
        channel_t* ch = &s_channels[c];
        if (!ch->active || !ch->samples) continue;

        // Convert sep (0–254) and vol (0–127) to left/right gains (0–256).
        int right_gain = (ch->sep * 256) / 254;       // 0 = all left, 256 = all right
        int left_gain  = 256 - right_gain;
        left_gain  = (left_gain  * ch->vol) / 127;
        right_gain = (right_gain * ch->vol) / 127;

        for (int s = 0; s < TIC_SAMPLES; s++) {
            if (ch->pos >= ch->nsamples) {
                ch->active = 0;
                break;
            }
            int32_t samp = ch->samples[ch->pos++];

            // Mix into stereo output, clamping to int16 range.
            int32_t l = s_mix_buf[s * 2 + 0] + (int32_t)((samp * left_gain)  >> 8);
            int32_t r = s_mix_buf[s * 2 + 1] + (int32_t)((samp * right_gain) >> 8);
            if (l >  32767) l =  32767;
            if (l < -32768) l = -32768;
            if (r >  32767) r =  32767;
            if (r < -32768) r = -32768;
            s_mix_buf[s * 2 + 0] = (int16_t)l;
            s_mix_buf[s * 2 + 1] = (int16_t)r;
        }
    }

    // Write one tic of stereo PCM to the AC97 device.
    write(s_dsp_fd, s_mix_buf, (uint32_t)TIC_BYTES);
}

// ── Sound module interface ────────────────────────────────────────────────

static boolean sfx_Init(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    s_dsp_fd = open("/dev/dsp", O_WRONLY, 0);
    if (s_dsp_fd < 0) return 0;
    for (int i = 0; i < MIX_CHANNELS; i++) s_channels[i].active = 0;
    return 1;
}

static void sfx_Shutdown(void) {
    if (s_dsp_fd >= 0) { close(s_dsp_fd); s_dsp_fd = -1; }
}

static int sfx_GetSfxLumpNum(sfxinfo_t* sfx) {
    char namebuf[16];
    // Doom prepends "DS" to effect names (e.g. "pistol" → "DSPISTOL").
    namebuf[0] = 'D'; namebuf[1] = 'S';
    int i = 2;
    const char* n = sfx->name;
    while (*n && i < 14) namebuf[i++] = *n++;
    namebuf[i] = '\0';
    return W_CheckNumForName(namebuf);
}

static void sfx_Update(void) {
    mix_and_write();
}

static void sfx_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= MIX_CHANNELS) return;
    s_channels[channel].vol = vol;
    s_channels[channel].sep = sep;
}

static int sfx_StartSound(sfxinfo_t* sfx, int channel, int vol, int sep) {
    if (channel < 0 || channel >= MIX_CHANNELS) return -1;
    if (sfx->lumpnum < 0) return -1;
    if (!decode_sfx(sfx)) return -1;

    typedef struct { int16_t* samples; uint32_t nsamples; } sfx_data_t;
    sfx_data_t* d = (sfx_data_t*)sfx->driver_data;
    if (!d) return -1;

    channel_t* ch = &s_channels[channel];
    ch->samples  = d->samples;
    ch->nsamples = d->nsamples;
    ch->pos      = 0;
    ch->vol      = vol;
    ch->sep      = sep;
    ch->active   = 1;
    return channel;
}

static void sfx_StopSound(int channel) {
    if (channel < 0 || channel >= MIX_CHANNELS) return;
    s_channels[channel].active = 0;
}

static boolean sfx_SoundIsPlaying(int channel) {
    if (channel < 0 || channel >= MIX_CHANNELS) return 0;
    return s_channels[channel].active;
}

static void sfx_CacheSounds(sfxinfo_t* sounds, int num_sounds) {
    // Pre-decode all sounds on startup.  Not required but reduces
    // stutter on first play.
    for (int i = 0; i < num_sounds; i++) {
        if (sounds[i].lumpnum >= 0)
            decode_sfx(&sounds[i]);
    }
}

// ── Music stubs ───────────────────────────────────────────────────────────
// Full MIDI synthesis is a separate future project.  The stubs keep Doom
// happy; it degrades gracefully to silence on music channels.

static boolean mus_Init(void) { return 1; }
static void    mus_Shutdown(void) {}
static void    mus_SetVolume(int v) { (void)v; }
static void    mus_Pause(void) {}
static void    mus_Resume(void) {}
static void*   mus_Register(void* d, int l) { (void)d; (void)l; return (void*)0; }
static void    mus_Unregister(void* h) { (void)h; }
static void    mus_Play(void* h, boolean loop) { (void)h; (void)loop; }
static void    mus_Stop(void) {}
static boolean mus_IsPlaying(void) { return 0; }

// ── Module tables ─────────────────────────────────────────────────────────

// We advertise SNDDEVICE_SB so Doom selects us via the default snd_sfxdevice.
static snddevice_t s_sfxdevs[] = { SNDDEVICE_SB };
static snddevice_t s_musdevs[] = { SNDDEVICE_SB };

sound_module_t DG_sound_module = {
    s_sfxdevs,
    1,
    sfx_Init,
    sfx_Shutdown,
    sfx_GetSfxLumpNum,
    sfx_Update,
    sfx_UpdateSoundParams,
    sfx_StartSound,
    sfx_StopSound,
    sfx_SoundIsPlaying,
    sfx_CacheSounds,
};

music_module_t DG_music_module = {
    s_musdevs,
    1,
    mus_Init,
    mus_Shutdown,
    mus_SetVolume,
    mus_Pause,
    mus_Resume,
    mus_Register,
    mus_Unregister,
    mus_Play,
    mus_Stop,
    mus_IsPlaying,
    (void*)0,   // Poll
};

// ── Doom-referenced globals not defined by i_sound.c ─────────────────────
// (snd_sfxdevice, snd_musicdevice, snd_samplerate, snd_cachesize,
//  snd_maxslicetime_ms, snd_musiccmd are defined in i_sound.c)
int   opl_io_port         = 0;
char* timidity_cfg_path   = (char*)"";
int   use_libsamplerate   = 0;
float libsamplerate_scale = 0.65f;

// ── PC speaker and OPL stubs (linked in by d_main.c) ─────────────────────
static boolean noop_sfx_init(boolean p) { (void)p; return 0; }
static boolean noop_mus_init(void) { return 0; }
static boolean noop_mus_playing(void) { return 0; }
static void    noop_mus_pause(void) {}
static void    noop_mus_resume(void) {}
static void    noop_mus_stop(void) {}
static void*   noop_mus_reg(void* d, int l) { (void)d; (void)l; return (void*)0; }
static void    noop_mus_unreg(void* h) { (void)h; }
static void    noop_mus_play(void* h, boolean loop) { (void)h; (void)loop; }

static snddevice_t s_noop_dev[] = { SNDDEVICE_NONE };

sound_module_t sound_pcsound_module = {
    s_noop_dev, 1,
    noop_sfx_init, sfx_Shutdown,
    sfx_GetSfxLumpNum, sfx_Update, sfx_UpdateSoundParams,
    sfx_StartSound, sfx_StopSound, sfx_SoundIsPlaying, sfx_CacheSounds,
};

music_module_t music_opl_module = {
    s_noop_dev, 1,
    noop_mus_init, mus_Shutdown, mus_SetVolume,
    noop_mus_pause, noop_mus_resume,
    noop_mus_reg, noop_mus_unreg,
    noop_mus_play, noop_mus_stop, noop_mus_playing,
    (void*)0,
};

