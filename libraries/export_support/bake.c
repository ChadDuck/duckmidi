#include "sample.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Export-only baking of a zone's static filter and chorus into mono PCM, approximating
   duckmidi's FluidSynth playback at the root key. Baked effects move with playback pitch. */

enum { SETTLE_PASSES = 64 };

static const double pi = 3.14159265358979323846;

typedef struct {
    double b0, b1, b2, a1, a2;
    double x1, x2, y1, y2;
} Biquad;

/* FluidSynth's IIR low-pass: SoundFont resonance in dB less 3.01, linear Q, and a 1/sqrt(Q)
   gain that keeps resonance from clipping. A zero cutoff uses the default 13500 cents. */
static Biquad lowpass(const DmeSampleToneParams *t, uint32_t rate) {
    double hz = t->cutoff > 0 ? t->cutoff : 8.176 * pow(2, 13500 / 1200.0);
    hz = fmax(5, fmin(hz, .45 * rate));
    double q = pow(10, (fmax(0, fmin(96, t->resonance)) - 3.01) / 20);
    double omega = 2 * pi * hz / rate, s = sin(omega), c = cos(omega);
    double alpha = s / (2 * q), a0 = 1 / (1 + alpha), gain = 1 / sqrt(q);
    double b1 = (1 - c) * a0 * gain;
    return (Biquad){b1 / 2, b1, b1 / 2, -2 * c * a0, (1 - alpha) * a0, 0, 0, 0, 0};
}

static float step(Biquad *f, float x) {
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return (float)y;
}

/* The loop body is written from a pass whose filter state has settled around the loop, so its
   end leads into its start without a step. */
static void filter(float *out, const float *in, uint32_t frames, uint32_t loop_start,
                   uint32_t loop_end, Biquad f) {
    uint32_t until = loop_end ? loop_start : frames;

    for (uint32_t i = 0; i < until; i++) {
        out[i] = step(&f, in[i]);
    }

    if (!loop_end) {
        return;
    }

    for (unsigned pass = 0; pass < SETTLE_PASSES; pass++) {
        double change = 0;

        for (uint32_t i = loop_start; i < loop_end; i++) {
            float y = step(&f, in[i]);
            change = fmax(change, fabs(y - out[i]));
            out[i] = y;
        }

        if (pass && change < 1e-7) {
            break;
        }
    }

    for (uint32_t i = loop_end; i < frames; i++) {
        out[i] = step(&f, in[i]);
    }
}

/* FluidSynth's default chorus: three voices, 0.3 Hz sine sweep, 8 ms depth, level 2. The
   stereo voices fold to mono at half level. Inside a loop, delayed reads wrap to the loop end
   and the sweep is rounded to whole cycles per loop, or held still when the loop is shorter
   than one cycle. */
static void chorus(float *out, const float *in, uint32_t frames, uint32_t rate, uint32_t loop_start,
                   uint32_t loop_end, double send) {
    const unsigned voices = 3;
    const double level = 2, base = .002 * rate, depth = .008 * rate;
    double hz = .3;
    bool still = false;

    if (loop_end) {
        double cycles = round(hz * (loop_end - loop_start) / rate);
        still = cycles < 1;
        hz = still ? 0 : cycles * rate / (loop_end - loop_start);
    }

    for (uint32_t i = 0; i < frames; i++) {
        double wet = 0;

        for (unsigned v = 0; v < voices; v++) {
            double phase = still ? 0 : 2 * pi * hz * i / rate;
            double delay = base + depth / 2 * (1 + sin(phase + 2 * pi * v / voices));
            double at = i - delay;

            if (loop_end && i >= loop_start && i < loop_end && at < loop_start) {
                at += loop_end - loop_start;
            }

            if (at < 0) {
                continue;
            }

            uint32_t left = (uint32_t)at;
            double fraction = at - left;
            float a = in[left], b = left + 1 < frames ? in[left + 1] : 0;

            if (loop_end && left + 1 == loop_end && i < loop_end) {
                b = in[loop_start];
            }

            wet += a + (b - a) * fraction;
        }

        out[i] = (float)(in[i] + send * level / voices * .5 * wet);
    }
}

bool es_native_bake(float *pcm, uint32_t frames, uint32_t rate, uint32_t loop_start,
                    uint32_t loop_end, const DmeSampleToneParams *tone, uint32_t *fixes) {
    bool filtered = tone->cutoff > 0 || tone->resonance > 0;
    bool chorused = tone->chorus > 0;

    if (!frames || (!filtered && !chorused)) {
        return true;
    }

    float *copy = malloc((size_t)frames * sizeof(*copy));

    if (!copy) {
        return false;
    }

    if (filtered) {
        memcpy(copy, pcm, (size_t)frames * sizeof(*copy));
        filter(pcm, copy, frames, loop_start, loop_end, lowpass(tone, rate));
    }

    /* A loop shorter than the longest chorus delay cannot hold its echo. */
    if (chorused && loop_end && loop_end - loop_start <= .011 * rate) {
        *fixes = (*fixes & ~(1u << ES_FIX_CHORUS)) | 1u << ES_FIX_CHORUS_SHORT;
    } else if (chorused) {
        memcpy(copy, pcm, (size_t)frames * sizeof(*copy));
        chorus(pcm, copy, frames, rate, loop_start, loop_end, fmin(1, tone->chorus));
    }

    free(copy);
    return true;
}
