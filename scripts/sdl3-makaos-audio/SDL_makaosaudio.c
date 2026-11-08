// MakaOS native audio backend for SDL3.
//
// MakaOS exposes a single blocking PCM output node at /dev/dsp backed by the
// kernel Intel HDA driver: signed 16-bit little-endian stereo at 48000 Hz,
// fixed (no format negotiation).  The node is pollable -- POLLOUT is raised
// once the DMA FIFO can absorb a normal playback buffer -- so this backend
// follows SDL3's required split: block for readiness in WaitDevice (which
// runs OUTSIDE the audio device lock) via poll(), then hand the buffer over
// in PlayDevice with a write() that does not block.
//
// It mirrors SDL's OSS /dev/dsp backend, minus the SNDCTL ioctls MakaOS does
// not implement: the fixed hardware format stands in for format negotiation,
// and poll(POLLOUT) stands in for SNDCTL_DSP_GETOSPACE.
//
// Installed into SDL's tree at src/audio/makaos/ by scripts/port-sdl3.sh.

#include "SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_MAKAOS

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "../SDL_sysaudio.h"
#include "SDL_makaosaudio.h"

#define MAKAOS_DSP_NODE "/dev/dsp"

// The kernel HDA path is hardwired to this format; SDL's stream layer converts
// the application's audio to it transparently.
#define MAKAOS_HW_FORMAT   SDL_AUDIO_S16LE
#define MAKAOS_HW_CHANNELS 2
#define MAKAOS_HW_FREQ     48000

static bool MAKAOSAUDIO_OpenDevice(SDL_AudioDevice *device)
{
    device->hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*device->hidden));
    if (!device->hidden) {
        return false;
    }
    device->hidden->audio_fd = -1;

    const int fd = open(MAKAOS_DSP_NODE, O_WRONLY, 0);
    if (fd < 0) {
        return SDL_SetError("MakaOS audio: cannot open %s", MAKAOS_DSP_NODE);
    }
    device->hidden->audio_fd = fd;

    // /dev/dsp offers exactly one format; pin the device spec to it and have
    // SDL recompute buffer_size / silence_value for the pinned spec.  The
    // half-FIFO POLLOUT watermark in the kernel is sized so this buffer's
    // write() lands without blocking.
    device->spec.format = MAKAOS_HW_FORMAT;
    device->spec.channels = MAKAOS_HW_CHANNELS;
    device->spec.freq = MAKAOS_HW_FREQ;
    SDL_UpdatedAudioDeviceFormat(device);

    device->hidden->mixbuf = (Uint8 *)SDL_malloc(device->buffer_size);
    if (!device->hidden->mixbuf) {
        return false;
    }
    SDL_memset(device->hidden->mixbuf, device->silence_value, device->buffer_size);

    return true;
}

static bool MAKAOSAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *h = device->hidden;

    // Block until /dev/dsp can accept a full buffer without write() blocking.
    // Cap each poll so device shutdown is observed promptly.  This is the
    // only place this backend blocks, exactly as SDL3 requires -- PlayDevice
    // runs under device->lock and must not.
    while (!SDL_GetAtomicInt(&device->shutdown)) {
        struct pollfd pfd;
        pfd.fd = h->audio_fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        const int rc = poll(&pfd, 1, 100);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        } else if (rc == 0) {
            continue;  // timeout -- re-check shutdown, keep waiting
        } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return false;
        } else if (pfd.revents & POLLOUT) {
            break;     // room available
        }
    }
    return true;
}

static bool MAKAOSAUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    struct SDL_PrivateAudioData *h = device->hidden;

    // WaitDevice guaranteed FIFO room for a normal buffer, so this write()
    // returns without blocking; the loop only covers a short-write tail.
    const Uint8 *p = buffer;
    int remaining = buflen;
    while (remaining > 0) {
        const int w = (int)write(h->audio_fd, p, (size_t)remaining);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        } else if (w == 0) {
            return false;  // no bytes accepted -- device gone (no HDA)
        }
        p += w;
        remaining -= w;
    }
    return true;
}

static Uint8 *MAKAOSAUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    (void)buffer_size;  // keep the full device->buffer_size
    return device->hidden->mixbuf;
}

static void MAKAOSAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    if (device->hidden) {
        if (device->hidden->audio_fd >= 0) {
            close(device->hidden->audio_fd);
        }
        SDL_free(device->hidden->mixbuf);
        SDL_free(device->hidden);
        device->hidden = NULL;
    }
}

static bool MAKAOSAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    // Probe: the device node must be openable, else return false so SDL falls
    // through to the disk and dummy fallbacks that sit after us in bootstrap[].
    const int fd = open(MAKAOS_DSP_NODE, O_WRONLY, 0);
    if (fd < 0) {
        return false;
    }
    close(fd);

    impl->OpenDevice = MAKAOSAUDIO_OpenDevice;
    impl->WaitDevice = MAKAOSAUDIO_WaitDevice;
    impl->PlayDevice = MAKAOSAUDIO_PlayDevice;
    impl->GetDeviceBuf = MAKAOSAUDIO_GetDeviceBuf;
    impl->CloseDevice = MAKAOSAUDIO_CloseDevice;

    impl->OnlyHasDefaultPlaybackDevice = true;

    return true;
}

AudioBootStrap MAKAOSAUDIO_bootstrap = {
    "makaos", "MakaOS HDA (/dev/dsp)", MAKAOSAUDIO_Init, false
};

#endif // SDL_AUDIO_DRIVER_MAKAOS
