#include "bake.h"
#include <math.h>
#include <stdlib.h>

static uint16_t read16(const uint8_t *p, bool big) {
    return big ? (uint16_t)((unsigned)p[0] << 8 | p[1]) : (uint16_t)((unsigned)p[1] << 8 | p[0]);
}

static void write16(uint8_t *p, uint16_t value, bool big) {
    p[big ? 1 : 0] = (uint8_t)value;
    p[big ? 0 : 1] = (uint8_t)(value >> 8);
}

static bool cancelled(const DmeHost *host) {
    return host && host->cancelled && host->cancelled(host->user);
}

unsigned es_pcm_taps(double ratio) {
    unsigned taps = (unsigned)ceil(128 / ratio);
    return taps + (taps & 1u);
}

int es_pcm_resample(const uint8_t *in, uint32_t frames, uint32_t loop_start, uint32_t loop_end,
                    bool big, double ratio, uint8_t *out, uint32_t out_frames, uint32_t out_start,
                    uint32_t out_end, const DmeHost *host, bool *lowered) {
    /* Polyphase low-pass, with a Blackman window and normalized DC gain. */
    enum { PHASES = 256 };

    unsigned taps = es_pcm_taps(ratio);

    if (taps > ES_PCM_TAPS) {
        return ES_PCM_LIMIT;
    }

    double *coefficients = malloc((size_t)PHASES * taps * sizeof(double));
    float *filtered = malloc((size_t)out_frames * sizeof(*filtered));

    if (!coefficients || !filtered) {
        free(coefficients);
        free(filtered);
        return ES_PCM_MEMORY;
    }

    const double pi = 3.14159265358979323846, cutoff = ratio * .94;

    for (unsigned phase = 0; phase < PHASES; phase++) {
        if (cancelled(host)) {
            free(coefficients);
            free(filtered);
            return ES_PCM_CANCELLED;
        }

        double sum = 0;

        for (unsigned tap = 0; tap < taps; tap++) {
            double x = (double)((int)tap - (int)taps / 2 + 1) - (double)phase / PHASES;
            double window =
                .42 + .5 * cos(pi * x / (taps / 2)) + .08 * cos(2 * pi * x / (taps / 2));
            double weight = fabs(x) < 1e-12 ? cutoff : sin(pi * cutoff * x) / (pi * x);
            coefficients[phase * taps + tap] = weight * window;
            sum += weight * window;
        }

        for (unsigned tap = 0; tap < taps; tap++) {
            coefficients[phase * taps + tap] /= sum;
        }
    }

    double peak = 0;

    for (uint32_t i = 0; i < out_frames; i++) {
        if (!(i % 1024) && cancelled(host)) {
            free(coefficients);
            free(filtered);
            return ES_PCM_CANCELLED;
        }

        double position = (double)i / ratio;
        int64_t center = (int64_t)floor(position);
        unsigned phase = (unsigned)floor((position - center) * PHASES);
        double value = 0;

        for (unsigned tap = 0; tap < taps; tap++) {
            int64_t at = center + (int)tap - (int)taps / 2 + 1;

            if (loop_end && i >= out_start && i < out_end) {
                int64_t length = loop_end - loop_start;
                at = loop_start + ((at - loop_start) % length + length) % length;
            } else {
                at = at < 0 ? 0 : at >= frames ? frames - 1 : at;
            }

            int16_t sample = (int16_t)read16(in + (size_t)at * 2, big);
            value += sample * coefficients[phase * taps + tap];
        }

        filtered[i] = (float)value;
        peak = fmax(peak, fabs(filtered[i]));
    }

    free(coefficients);

    /* Lower the entire output when the low-pass overshoots; preserve its waveform. */
    double gain = peak > 32767 ? 32766 / peak : 1;
    *lowered = gain < 1;

    for (uint32_t i = 0; i < out_frames; i++) {
        if (!(i % 4096) && cancelled(host)) {
            free(filtered);
            return ES_PCM_CANCELLED;
        }

        write16(out + (size_t)i * 2, (uint16_t)(int16_t)floor(filtered[i] * gain + .5), big);
    }

    free(filtered);
    return ES_PCM_OK;
}
