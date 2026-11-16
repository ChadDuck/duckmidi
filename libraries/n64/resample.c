#include "sample.h"
#include "bake/bake.h"
#include "../../src/text/en.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    return rate >= 1000 && rate < r->rate && frames && es_pcm_taps(ratio) <= ES_PCM_TAPS &&
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

    bool big = native == ES_NATIVE_SM64, lowered = false;
    size_t header = r->audio.size - (size_t)r->frames * 2;
    size_t size = header + (size_t)frames * 2;
    uint8_t *output = malloc(size);

    if (!output) {
        es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
        return ES_RESAMPLE_FAILED;
    }

    memcpy(output, r->audio.data, header);
    int status = es_pcm_resample(r->audio.data + header, r->frames, r->loop_start, r->loop_end, big,
                                 ratio, output + header, frames, start, end, host, &lowered);

    if (status != ES_PCM_OK) {
        free(output);

        if (status == ES_PCM_MEMORY) {
            es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
        }

        return status == ES_PCM_LIMIT ? ES_RESAMPLE_LIMIT : ES_RESAMPLE_FAILED;
    }

    if (lowered) {
        r->fixes |= 1u << ES_FIX_GAIN;
    }

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
