#include "spectrum.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846f

float dm_visual_smooth(float value, float target, float seconds, float rise, float fall) {
    float time = target > value ? rise : fall;
    return target + (value - target) * expf(-fmaxf(0, seconds) / time);
}

static void transform(float *real, float *imaginary) {
    const int n = DM_SPECTRUM_FRAMES;

    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;

        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }

        j ^= bit;

        if (i < j) {
            float swap = real[i];
            real[i] = real[j];
            real[j] = swap;
        }
    }

    for (int width = 2; width <= n; width *= 2) {
        float angle = -2 * PI / width, step_r = cosf(angle), step_i = sinf(angle);

        for (int start = 0; start < n; start += width) {
            float wr = 1, wi = 0;

            for (int j = 0; j < width / 2; ++j) {
                int a = start + j, b = a + width / 2;
                float r = wr * real[b] - wi * imaginary[b];
                float im = wr * imaginary[b] + wi * real[b];
                real[b] = real[a] - r;
                imaginary[b] = imaginary[a] - im;
                real[a] += r;
                imaginary[a] += im;
                float next = wr * step_r - wi * step_i;
                wi = wr * step_i + wi * step_r;
                wr = next;
            }
        }
    }
}

float dm_spectrum_band_edge(int edge) {
    /* Start at the second resolved bin so every logarithmic band owns at least
       one distinct bin, including the lowest bands. */
    const float bin_hz = 48000.f / DM_SPECTRUM_FRAMES, low = 2 * bin_hz;
    int bin = 2;

    for (int i = 1; i <= edge; i++) {
        int next = (int)ceilf(low * powf(18000 / low, (float)i / DM_SPECTRUM_BANDS) / bin_hz);
        bin = next > bin ? next : bin + 1;
    }

    return bin * bin_hz;
}

void dm_spectrum_analyze(const float *stereo, float *bands) {
    float real[DM_SPECTRUM_FRAMES], imaginary[DM_SPECTRUM_FRAMES];
    float power[DM_SPECTRUM_FRAMES / 2 + 1] = {0};

    for (int channel = 0; channel < 2; ++channel) {
        memset(imaginary, 0, sizeof(imaginary));

        for (int i = 0; i < DM_SPECTRUM_FRAMES; ++i) {
            float window = .5f - .5f * cosf(2 * PI * i / (DM_SPECTRUM_FRAMES - 1));
            real[i] = stereo[i * 2 + channel] * window;
        }

        transform(real, imaginary);

        for (int i = 1; i <= DM_SPECTRUM_FRAMES / 2; ++i) {
            power[i] += (real[i] * real[i] + imaginary[i] * imaginary[i]) * .5f;
        }
    }

    for (int band = 0; band < DM_SPECTRUM_BANDS; ++band) {
        float low = dm_spectrum_band_edge(band);
        float high = dm_spectrum_band_edge(band + 1);
        int first = (int)fmaxf(1, ceilf(low * DM_SPECTRUM_FRAMES / 48000));
        int last = (int)fminf(DM_SPECTRUM_FRAMES / 2 + 1, ceilf(high * DM_SPECTRUM_FRAMES / 48000));
        float peak = 0;

        for (int i = first; i < last; ++i) {
            peak = fmaxf(peak, power[i]);
        }

        float magnitude = sqrtf(peak) * 4 / DM_SPECTRUM_FRAMES;
        float decibels = 20 * log10f(fmaxf(magnitude, .000001f));
        bands[band] = fmaxf(0, fminf(1, (decibels + 72) / 60));
    }
}
