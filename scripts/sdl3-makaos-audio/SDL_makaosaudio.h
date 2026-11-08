// MakaOS native SDL3 audio backend -- private per-device state.
//
// Installed into SDL's tree at src/audio/makaos/ by scripts/port-sdl3.sh.

#include "SDL_internal.h"

#ifndef SDL_makaosaudio_h_
#define SDL_makaosaudio_h_

#include "../SDL_sysaudio.h"

struct SDL_PrivateAudioData
{
    int audio_fd;   // /dev/dsp descriptor (-1 until opened)
    Uint8 *mixbuf;  // SDL mixes into this; PlayDevice write()s it to the device
};

#endif // SDL_makaosaudio_h_
