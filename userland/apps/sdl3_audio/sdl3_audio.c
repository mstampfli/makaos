/*
 * sdl3_audio -- end-to-end proof that the SDL3 audio port drives real sound
 * on MakaOS.  Opens the default playback device through SDL, then synthesises
 * and plays a 440 Hz (A4) sine for ~3 seconds.
 *
 * What it proves when it runs:
 *   - "audio driver: makaos" on the log => SDL selected the native /dev/dsp
 *     backend (not dummy/disk).
 *   - The tone reaches the emulated HDA hardware => the WaitDevice(poll) /
 *     PlayDevice(write) path works with no glitching or lock stalls.
 *
 * Under QEMU with `-audiodev wav,...,path=out.wav` the guest's output is
 * captured to a WAV on the host, so the sine can be inspected offline.
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <math.h>

#define RATE        48000
#define CH          2
#define FREQ_MAKAOS 440   /* tone when SDL selected the native makaos backend */
#define FREQ_OTHER  880   /* tone for any other backend -- makes the WAV itself
                             a direct readout of SDL_GetCurrentAudioDriver() */
#define SECS        3
#define CHUNK       1024  /* frames per push */

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        SDL_Log("sdl3_audio: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    const char* drv = SDL_GetCurrentAudioDriver();
    const int is_makaos = drv && (SDL_strcmp(drv, "makaos") == 0);
    const int freq = is_makaos ? FREQ_MAKAOS : FREQ_OTHER;
    SDL_Log("sdl3_audio: audio driver: %s -> %d Hz", drv ? drv : "(none)", freq);

    SDL_AudioSpec spec;
    spec.freq     = RATE;
    spec.format   = SDL_AUDIO_S16;
    spec.channels = CH;

    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) {
        SDL_Log("sdl3_audio: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!SDL_ResumeAudioStreamDevice(stream)) {
        SDL_Log("sdl3_audio: resume failed: %s", SDL_GetError());
    }

    const int total_frames = RATE * SECS;
    /* Keep at most ~4 chunks queued so we exercise backpressure, not buffering. */
    const int queue_cap = (int)(CHUNK * CH * sizeof(Sint16) * 4);

    Sint16 buf[CHUNK * CH];
    double phase = 0.0;
    const double inc = 2.0 * M_PI * (double)freq / (double)RATE;

    int frame = 0;
    while (frame < total_frames) {
        while (SDL_GetAudioStreamQueued(stream) > queue_cap) {
            SDL_Delay(5);
        }
        int n = CHUNK;
        if (frame + n > total_frames) n = total_frames - frame;
        for (int i = 0; i < n; i++) {
            Sint16 s = (Sint16)(sin(phase) * 9000.0);   /* ~27% -- clean, no clip */
            phase += inc;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
            buf[i * 2 + 0] = s;
            buf[i * 2 + 1] = s;
        }
        if (!SDL_PutAudioStreamData(stream, buf, n * CH * (int)sizeof(Sint16))) {
            SDL_Log("sdl3_audio: put failed: %s", SDL_GetError());
            break;
        }
        frame += n;
    }

    /* Wait for the queue + device buffer to drain before tearing down. */
    while (SDL_GetAudioStreamQueued(stream) > 0) {
        SDL_Delay(10);
    }
    SDL_Delay(300);

    SDL_Log("sdl3_audio: played %d frames of %d Hz, done", frame, freq);
    SDL_DestroyAudioStream(stream);
    SDL_Quit();
    return 0;
}
