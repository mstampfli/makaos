// tone.c — Continuous sine-wave tone generator for AC97 driver testing.
//
// Writes a 440 Hz (A4) sine wave as signed 16-bit stereo PCM at 48000 Hz
// to /dev/dsp.  Runs for ~10 seconds then exits.
//
// Build and listen: if you hear a clean, steady tone the AC97 driver is fine.
// If it sounds choppy it narrows the problem to the driver or the write path.

#include "libc.h"
#include "stdio.h"
#include "math.h"

// Output parameters — must match kernel AC97_SAMPLE_RATE / AC97_CHANNELS.
#define SAMPLE_RATE   48000
#define CHANNELS      2
#define BITS          16
#define FREQ_HZ       440        // A4
#define DURATION_SEC  10

// Write one chunk per call: 512 stereo frames = ~10.7 ms @ 48 kHz.
// Keeping chunks small keeps latency low and tests the ring drain path.
#define CHUNK_FRAMES  512
#define CHUNK_BYTES   (CHUNK_FRAMES * CHANNELS * (BITS / 8))


static short buf[CHUNK_FRAMES * CHANNELS];

// Integer sine approximation using the Taylor series around 0.
// Accurate enough for audio; avoids libm dependency.
// We keep a phase accumulator as a fixed-point angle in units of
// 2*pi / SAMPLE_RATE radians per sample.

// pi approximation (we don't have M_PI from math.h guaranteed here).
#define MY_PI 3.14159265358979323846

int main(void) {
    int fd = open("/dev/dsp", O_WRONLY, 0);
    if (fd < 0) {
        printf("tone: cannot open /dev/dsp\n");
        return 0;
    }

    int total_frames = SAMPLE_RATE * DURATION_SEC;
    int frame = 0;
    double phase = 0.0;
    double phase_inc = 2.0 * MY_PI * FREQ_HZ / SAMPLE_RATE;

    while (frame < total_frames) {
        int n = CHUNK_FRAMES;
        if (frame + n > total_frames) n = total_frames - frame;

        for (int i = 0; i < n; i++) {
            double s = sin(phase);
            phase += phase_inc;
            if (phase >= 2.0 * MY_PI) phase -= 2.0 * MY_PI;

            short samp = (short)(s * 28000.0);   // ~85% of max to avoid clipping
            buf[i * 2 + 0] = samp;  // left
            buf[i * 2 + 1] = samp;  // right
        }

        write(fd, buf, (unsigned int)(n * CHANNELS * (BITS / 8)));
        frame += n;
    }

    close(fd);
}
