// i_sound_stub.c — No-op sound/music implementation for MakaOS.
// Doom will function without audio; these stubs satisfy the linker.

#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/stdint.h"
#include "include/stdbool.h"
#include "doomgeneric/doomgeneric/doomtype.h"
#include "doomgeneric/doomgeneric/i_sound.h"

// ── Extern config vars declared in i_sound.h ─────────────────────────────
int   snd_sfxdevice      = 0;
int   snd_musicdevice    = 0;
int   snd_samplerate     = 22050;
int   snd_cachesize      = 64 * 1024 * 1024;
int   snd_maxslicetime_ms = 28;
char* snd_musiccmd       = (char*)"";

// OPL / timidity globals referenced from various doom modules.
int   opl_io_port        = 0;
char* timidity_cfg_path  = (char*)"";

// ── SFX stubs (correct signatures from i_sound.h) ─────────────────────────
void    I_InitSound(boolean use_sfx_prefix) { (void)use_sfx_prefix; }
void    I_ShutdownSound(void) {}
int     I_GetSfxLumpNum(sfxinfo_t* sfx) { (void)sfx; return -1; }
void    I_UpdateSound(void) {}
void    I_UpdateSoundParams(int channel, int vol, int sep) { (void)channel; (void)vol; (void)sep; }
int     I_StartSound(sfxinfo_t* sfx, int channel, int vol, int sep) { (void)sfx; (void)channel; (void)vol; (void)sep; return -1; }
void    I_StopSound(int channel) { (void)channel; }
boolean I_SoundIsPlaying(int channel) { (void)channel; return 0; }
void    I_PrecacheSounds(sfxinfo_t* sounds, int num_sounds) { (void)sounds; (void)num_sounds; }

// ── Music stubs ────────────────────────────────────────────────────────────
void    I_InitMusic(void) {}
void    I_ShutdownMusic(void) {}
void    I_SetMusicVolume(int volume) { (void)volume; }
void    I_PauseSong(void) {}
void    I_ResumeSong(void) {}
void*   I_RegisterSong(void* data, int len) { (void)data; (void)len; return (void*)0; }
void    I_UnRegisterSong(void* handle) { (void)handle; }
void    I_PlaySong(void* handle, boolean looping) { (void)handle; (void)looping; }
void    I_StopSong(void) {}
boolean I_MusicIsPlaying(void) { return 0; }

// ── Binding stub ──────────────────────────────────────────────────────────
void    I_BindSoundVariables(void) {}

// ── Module tables (used when FEATURE_SOUND is defined) ────────────────────
// Doom references DG_sound_module / DG_music_module via #ifdef FEATURE_SOUND.
// If not defined, these are not used; provide them anyway as safeguard.
static boolean sfx_init_stub(boolean p) { (void)p; return 0; }
static boolean mus_init_stub(void) { return 0; }
static boolean mus_playing_stub(void) { return 0; }
static void mus_pause_stub(void) {}
static void mus_resume_stub(void) {}
static void mus_stop_stub(void) {}
static void* mus_reg_stub(void* d, int l) { (void)d; (void)l; return (void*)0; }
static void mus_unreg_stub(void* h) { (void)h; }
static void mus_play_stub(void* h, boolean loop) { (void)h; (void)loop; }

static snddevice_t s_sfxdev[] = { SNDDEVICE_NONE };
static snddevice_t s_musdev[] = { SNDDEVICE_NONE };

sound_module_t DG_sound_module = {
    s_sfxdev, 1,
    sfx_init_stub,
    I_ShutdownSound,
    I_GetSfxLumpNum,
    I_UpdateSound,
    I_UpdateSoundParams,
    I_StartSound,
    I_StopSound,
    I_SoundIsPlaying,
    I_PrecacheSounds,
};

music_module_t DG_music_module = {
    s_musdev, 1,
    mus_init_stub,
    I_ShutdownMusic,
    I_SetMusicVolume,
    mus_pause_stub,
    mus_resume_stub,
    mus_reg_stub,
    mus_unreg_stub,
    mus_play_stub,
    mus_stop_stub,
    mus_playing_stub,
    (void*)0,  // Poll
};

// ── PC speaker module (referenced from d_main.c) ──────────────────────────
sound_module_t sound_pcsound_module = {
    s_sfxdev, 1,
    sfx_init_stub,
    I_ShutdownSound,
    I_GetSfxLumpNum,
    I_UpdateSound,
    I_UpdateSoundParams,
    I_StartSound,
    I_StopSound,
    I_SoundIsPlaying,
    I_PrecacheSounds,
};

// ── OPL music module (referenced from d_main.c) ───────────────────────────
music_module_t music_opl_module = {
    s_musdev, 1,
    mus_init_stub,
    I_ShutdownMusic,
    I_SetMusicVolume,
    mus_pause_stub,
    mus_resume_stub,
    mus_reg_stub,
    mus_unreg_stub,
    mus_play_stub,
    mus_stop_stub,
    mus_playing_stub,
    (void*)0,
};
