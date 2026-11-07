#include "sample.h"
#include "internal.h"
#include "../../src/text/en.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read16(const uint8_t *p, bool big) {
    return big ? (uint16_t)((unsigned)p[0] << 8 | p[1]) : (uint16_t)((unsigned)p[1] << 8 | p[0]);
}

static void write16(uint8_t *p, uint16_t value, bool big) {
    p[big ? 1 : 0] = (uint8_t)value;
    p[big ? 0 : 1] = (uint8_t)(value >> 8);
}

static void write32(uint8_t *p, uint32_t value, bool big) {
    write16(p, (uint16_t)(big ? value >> 16 : value), big);
    write16(p + 2, (uint16_t)(big ? value : value >> 16), big);
}

/* Replacement consumers may contain arbitrary native sequences and runtime pitch controls.
   Never raise their initialized resampling rates. A band-limited copy at a lower source rate
   preserves the selected preset's pitch while keeping that conservative engine-rate bound. */
bool es_native_resample_fits(const EsNativeRegion *r, double maximum_tuning) {
    if (!isfinite(maximum_tuning) || maximum_tuning <= 0 || !isfinite(r->tuning) ||
        r->tuning <= 0 || !r->rate || !r->frames) {
        return false;
    }

    if (r->tuning <= maximum_tuning) {
        return true;
    }

    uint32_t rate = (uint32_t)floor(r->rate * (maximum_tuning / r->tuning));

    if (rate < 1000 || rate >= r->rate) {
        return false;
    }

    double ratio = (double)rate / r->rate;
    uint32_t frames = (uint32_t)ceil(r->frames * ratio);
    uint32_t start = (uint32_t)llround(r->loop_start * ratio);
    uint32_t end = (uint32_t)llround(r->loop_end * ratio);
    unsigned taps = (unsigned)ceil(128 / ratio);
    return rate >= 1000 && rate < r->rate && frames && taps + (taps & 1u) <= 32768 &&
           (!r->loop_end || (start < end && end <= frames));
}

int es_native_resample(EsNativeRegion *r, double maximum_tuning, uint32_t native,
                       const DmeHost *host, char *error, size_t cap) {
    if (!es_native_resample_fits(r, maximum_tuning)) {
        return ES_RESAMPLE_LIMIT;
    }

    if (r->tuning <= maximum_tuning) {
        return ES_RESAMPLE_OK;
    }

    uint32_t rate = (uint32_t)floor(r->rate * (maximum_tuning / r->tuning));

    if (rate < 1000 || rate >= r->rate) {
        return ES_RESAMPLE_LIMIT;
    }

    double ratio = (double)rate / r->rate;
    uint32_t frames = (uint32_t)ceil(r->frames * ratio);
    uint32_t start = (uint32_t)llround(r->loop_start * ratio);
    uint32_t end = (uint32_t)llround(r->loop_end * ratio);

    if (!frames || (r->loop_end && (start >= end || end > frames))) {
        return ES_RESAMPLE_LIMIT;
    }

    bool big = native == ES_NATIVE_SM64;
    size_t header = r->audio.size - (size_t)r->frames * 2;
    size_t size = header + (size_t)frames * 2;
    uint8_t *output = malloc(size);

    /* Polyphase low-pass, with a Blackman window and normalized DC gain. */
    enum { PHASES = 256 };

    unsigned taps = (unsigned)ceil(128 / ratio);
    taps += taps & 1u;

    if (taps > 32768) {
        free(output);
        return ES_RESAMPLE_LIMIT;
    }

    double *coefficients = malloc((size_t)PHASES * taps * sizeof(double));
    float *filtered = malloc((size_t)frames * sizeof(*filtered));

    if (!output || !coefficients || !filtered) {
        free(output);
        free(coefficients);
        free(filtered);
        es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
        return ES_RESAMPLE_FAILED;
    }

    const double pi = 3.14159265358979323846, cutoff = ratio * .94;

    for (unsigned phase = 0; phase < PHASES; phase++) {
        if (host && host->cancelled && host->cancelled(host->user)) {
            free(output);
            free(coefficients);
            free(filtered);
            return ES_RESAMPLE_FAILED;
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

    memcpy(output, r->audio.data, header);
    bool ok = true;
    double peak = 0;

    for (uint32_t i = 0; ok && i < frames; i++) {
        if (!(i % 1024) && host && host->cancelled && host->cancelled(host->user)) {
            ok = false;
            break;
        }

        double position = (double)i / ratio;
        int64_t center = (int64_t)floor(position);
        unsigned phase = (unsigned)floor((position - center) * PHASES);
        double value = 0;

        for (unsigned tap = 0; tap < taps; tap++) {
            int64_t at = center + (int)tap - (int)taps / 2 + 1;

            if (r->loop_end && i >= start && i < end) {
                int64_t length = r->loop_end - r->loop_start;
                at = r->loop_start + ((at - r->loop_start) % length + length) % length;
            } else {
                at = at < 0 ? 0 : at >= r->frames ? r->frames - 1 : at;
            }

            int16_t sample = (int16_t)read16(r->audio.data + header + (size_t)at * 2, big);
            value += sample * coefficients[phase * taps + tap];
        }

        filtered[i] = (float)value;
        peak = fmax(peak, fabs(filtered[i]));
    }

    free(coefficients);

    if (!ok) {
        free(output);
        free(filtered);
        return ES_RESAMPLE_FAILED;
    }

    /* Lower the entire region when the low-pass overshoots; preserve its waveform. */
    double gain = peak > 32767 ? 32766 / peak : 1;

    if (gain < 1) {
        r->fixes |= 1u << ES_FIX_GAIN;
    }

    for (uint32_t i = 0; i < frames; i++) {
        if (!(i % 4096) && host && host->cancelled && host->cancelled(host->user)) {
            free(filtered);
            free(output);
            return ES_RESAMPLE_FAILED;
        }

        write16(output + header + (size_t)i * 2, (uint16_t)(int16_t)floor(filtered[i] * gain + .5),
                big);
    }

    free(filtered);

    write32(output + 4, (uint32_t)size - 8, big);
    write32(output + header - (big ? 12 : 4), frames * 2 + (big ? 8 : 0), big);

    if (big) {
        write32(output + 22, frames, true);
        int exponent;
        double fraction = frexp((double)rate, &exponent);
        write16(output + 28, (uint16_t)(16382 + exponent), true);
        uint64_t mantissa = (uint64_t)ldexp(fraction, 64);
        write32(output + 30, (uint32_t)(mantissa >> 32), true);
        write32(output + 34, (uint32_t)mantissa, true);

        if (r->loop_end) {
            write32(output + 50, start, true);
            write32(output + 58, end, true);
        }
    } else {
        write32(output + 24, rate, false);
        write32(output + 28, rate * 2, false);

        if (r->loop_end) {
            write32(output + 52, 1000000000u / rate, false);
            write32(output + 88, start, false);
            write32(output + 92, end, false);
        }
    }

    free(r->audio.data);
    r->audio.data = output;
    r->audio.size = size;
    r->tuning *= ratio;
    r->rate = rate;
    r->frames = frames;
    r->loop_start = start;
    r->loop_end = end;
    r->fixes |= 1u << ES_FIX_FORMAT;
    return ES_RESAMPLE_OK;
}
