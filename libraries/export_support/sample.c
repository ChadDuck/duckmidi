#include "sample.h"
#include "internal.h"
#include "../../include/duckmidi_midi.h"
#include "../../src/text/en.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PREP_OK, PREP_INVALID, PREP_FAILED };

static void u16(uint8_t *p, uint16_t n, bool big) {
    p[big ? 1 : 0] = (uint8_t)n;
    p[big ? 0 : 1] = (uint8_t)(n >> 8);
}

static void u32(uint8_t *p, uint32_t n, bool big) {
    u16(p, (uint16_t)(big ? n >> 16 : n), big);
    u16(p + 2, (uint16_t)(big ? n : n >> 16), big);
}

static void point(EsNativeRegion *r, double seconds, double level, uint32_t native) {
    if (seconds <= 0) {
        return;
    }

    (void)native;
    unsigned duration = (unsigned)fmin(32767, fmax(1, floor(seconds * 240 + .5)));
    /* Both engines square the envelope value: SM64 multiplies the note by its square and OoT
       squares each target. The square root keeps the level's amplitude. */
    r->points[r->point_count][0] = (int16_t)duration;
    r->points[r->point_count++][1] = (int16_t)floor(sqrt(level) * 32767 + .5);
}

/* duckmidi plays decay like a SoundFont: the level falls at a steady 96 dB per decay time until it
   reaches sustain (or silence). The games interpolate in straight lines, so the decay is split
   into points of at most 12 dB each, using the envelope points that remain. */
static void decay(EsNativeRegion *r, const DmeSampleZoneParams *z, uint32_t native) {
    double time = fmax(z->decay, 1.0 / 240);
    double span = z->sustain > 0 ? fmin(96, -0.96 * 20 * log10(z->sustain)) : 96;
    unsigned left = 8 - r->point_count;
    unsigned count = (unsigned)fmax(1, fmin(left, ceil(span / 12)));
    double length = fmax(time * span / 96, 1.0 / 240);

    for (unsigned i = 1; i <= count; ++i) {
        double level = i == count ? z->sustain : pow(10, -span * i / count / 20);
        point(r, length / count, level, native);
    }
}

static void envelope(EsNativeRegion *r, uint32_t native) {
    const DmeSampleZoneParams *z = &r->params;
    point(r, z->delay, 0, native);
    point(r, fmax(z->attack, 1.0 / 240), 1, native);
    point(r, z->hold, 1, native);
    decay(r, z, native);
    /* duckmidi releases at 96 dB per release time; the games fade the envelope value in a straight
       line, which SM64 then squares. A native fade of this fraction of the release time crosses
       each level down to -60 dB closest to duckmidi's (least squares over the levels). */
    double target = z->release * (native == ES_NATIVE_SM64 ? .458 : .382);
    double best = HUGE_VAL;

    for (unsigned i = 1; i <= 255; i++) {
        double time;

        if (native == ES_NATIVE_SM64) {
            time = 32767.0 / (i * 24.0 * 240.0);
        } else {
            const double fast[] = {.75, .66, .5, .33, .25};
            double scale = i >= 251   ? fast[i - 251]
                           : i >= 128 ? 251 - i
                           : i >= 16  ? 4 * (143 - i)
                                      : 60 * (23 - i);
            time = scale / 60.0;
        }

        double distance = fabs(time - target);

        if (distance < best) {
            best = distance;
            r->release = (uint16_t)i;
            r->release_seconds = time;
        }
    }

    /* Small release differences are ordinary table rounding and only reach the details. */
    if (best > fmax(.05, target * .1)) {
        r->fixes |= 1u << ES_FIX_RELEASE;
    }
}

static double clamp_value(EsNativeRegion *r, double value, double maximum) {
    if (value < 0 || value > maximum) {
        r->fixes |= 1u << ES_FIX_TIMING;
        return value < 0 ? 0 : maximum;
    }

    return value;
}

/* Records settings the games cannot play yet and clamps values to native bounds. Returns false
   for a zone with a non-finite or out-of-range identity value; that row then uses its ordinary
   game instrument. Key tracking is checked per note, since drum entries represent it exactly. */
static bool sanitize(EsNativeRegion *r) {
    DmeSampleZoneParams *z = &r->params;
    const DmeSampleToneParams *t = &z->tone;

    if (z->size != sizeof(*z) || z->root > 127 || z->key_lo > z->key_hi || z->key_hi > 127 ||
        z->velocity_lo > z->velocity_hi || z->velocity_hi > 127 ||
        z->loop_mode > DME_SAMPLE_LOOP_SUSTAIN || !isfinite(z->cents) || !isfinite(t->coarse) ||
        !isfinite(t->fine) || !isfinite(z->pan) || !isfinite(t->tracking) || !isfinite(t->cutoff) ||
        !isfinite(t->resonance) || !isfinite(t->filter_depth) || !isfinite(t->vibrato_rate) ||
        !isfinite(t->vibrato_depth) || !isfinite(t->chorus) || !isfinite(t->reverb) ||
        !isfinite(z->gain_db) || !isfinite(z->delay) || !isfinite(z->attack) ||
        !isfinite(z->hold) || !isfinite(z->decay) || !isfinite(z->sustain) ||
        !isfinite(z->release)) {
        return false;
    }

    const struct {
        bool used;
        unsigned fix;
    } omitted[] = {
        {z->pan < -1 || z->pan > 1, ES_FIX_PAN},
        {z->cut_group || z->cut_by, ES_FIX_CHOKE},
        {z->loop_mode == DME_SAMPLE_LOOP_SUSTAIN, ES_FIX_SUSTAIN_LOOP},
        {t->cutoff || t->resonance, ES_FIX_FILTER},
        {t->filter_depth != 0, ES_FIX_FILTER_ENVELOPE},
        {t->chorus != 0, ES_FIX_CHORUS},
    };

    for (size_t i = 0; i < SDL_arraysize(omitted); i++) {
        if (omitted[i].used) {
            r->fixes |= 1u << omitted[i].fix;
        }
    }

    /* A sustain-only loop plays as a continuous loop: the native engines loop through release. */
    if (z->loop_mode == DME_SAMPLE_LOOP_SUSTAIN) {
        z->loop_mode = DME_SAMPLE_LOOP_CONTINUOUS;
    }

    z->delay = clamp_value(r, z->delay, 100);
    z->attack = clamp_value(r, z->attack, 100);
    z->hold = clamp_value(r, z->hold, 100);
    z->decay = clamp_value(r, z->decay, 100);
    z->release = clamp_value(r, z->release, 22);
    z->sustain = clamp_value(r, z->sustain, 1);
    return true;
}

enum { READ_CHUNK = 8192 };

/* Reads `count` (at most READ_CHUNK / channels) frames from `frame`, mixed to mono at `gain`.
   Non-finite source values are silenced. */
static int read_mono(EsNativeRegion *r, const DmeSampleAsset *a, uint64_t handle, uint32_t asset,
                     uint32_t frame, uint32_t count, double gain, float *out, const DmeHost *host,
                     char *error, size_t cap) {
    float pcm[READ_CHUNK];

    if ((host->cancelled && host->cancelled(host->user)) ||
        host->read_frozen_sample(host->user, handle, asset, r->params.playback_start + frame, count,
                                 pcm, (uint64_t)count * a->channels) != DME_OK) {
        es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_READ);
        return PREP_FAILED;
    }

    for (uint32_t i = 0; i < count; i++) {
        double value = 0;

        for (uint32_t c = 0; c < a->channels; c++) {
            value += pcm[i * a->channels + c];
        }

        value = value / a->channels * gain;

        if (!isfinite(value)) {
            r->fixes |= 1u << ES_FIX_FORMAT;
            value = 0;
        }

        out[i] = (float)value;
    }

    return PREP_OK;
}

static void quantize(EsNativeRegion *r, size_t header, bool big, uint32_t frame, float value) {
    double clamped = fmax(-1, fmin(1, value));
    int32_t quantized = (int32_t)floor(clamped * 32768 + .5);
    quantized = quantized > 32767 ? 32767 : quantized;
    u16(r->audio.data + header + (size_t)frame * 2, (uint16_t)quantized, big);
}

/* Streams the native PCM at the given linear gain. Reports the largest scaled magnitude so the
   caller can retry with lower gain instead of clipping. */
static int write_pcm(EsNativeRegion *r, const DmeSampleAsset *a, uint64_t handle, uint32_t asset,
                     size_t header, bool big, double gain, double *peak, const DmeHost *host,
                     char *error, size_t cap) {
    float mono[READ_CHUNK];
    uint32_t chunk = READ_CHUNK / a->channels;
    *peak = 0;

    for (uint32_t frame = 0; frame < r->frames;) {
        uint32_t count = r->frames - frame < chunk ? r->frames - frame : chunk;
        int status = read_mono(r, a, handle, asset, frame, count, gain, mono, host, error, cap);

        if (status != PREP_OK) {
            return status;
        }

        for (uint32_t i = 0; i < count; i++) {
            *peak = fmax(*peak, fabs(mono[i]));
            quantize(r, header, big, frame + i, mono[i]);
        }

        frame += count;
    }

    return PREP_OK;
}

/* Reads the whole region, bakes its filter and chorus, then writes it scaled to fit. */
static int write_baked(EsNativeRegion *r, const DmeSampleAsset *a, uint64_t handle, uint32_t asset,
                       size_t header, bool big, double gain, const DmeHost *host, char *error,
                       size_t cap) {
    float *mono = malloc((size_t)r->frames * sizeof(*mono));
    uint32_t chunk = READ_CHUNK / a->channels;
    int status = mono ? PREP_OK : PREP_FAILED;

    if (!mono) {
        es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
    }

    for (uint32_t frame = 0; status == PREP_OK && frame < r->frames; frame += chunk) {
        uint32_t count = r->frames - frame < chunk ? r->frames - frame : chunk;
        status = read_mono(r, a, handle, asset, frame, count, gain, mono + frame, host, error, cap);
    }

    if (status == PREP_OK && !es_native_bake(mono, r->frames, a->rate, r->loop_start, r->loop_end,
                                             &r->params.tone, &r->fixes)) {
        es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
        status = PREP_FAILED;
    }

    double peak = 0;

    for (uint32_t i = 0; status == PREP_OK && i < r->frames; i++) {
        peak = fmax(peak, fabs(mono[i]));
    }

    /* Resonance and chorus can raise peaks; lower the level just enough to fit. */
    double scale = peak > 1 ? 32766.0 / 32768 / peak : 1;

    if (peak > 1) {
        r->fixes |= 1u << ES_FIX_GAIN;
    }

    for (uint32_t i = 0; status == PREP_OK && i < r->frames; i++) {
        quantize(r, header, big, i, (float)(mono[i] * scale));
    }

    free(mono);
    return status;
}

static int audio(EsNativeRegion *r, const DmeSampleAsset *a, uint64_t handle, uint32_t asset,
                 uint32_t native, const DmeHost *host, char *error, size_t cap) {
    DmeSampleZoneParams *z = &r->params;

    if (a->size != sizeof(*a) || !a->rate || !a->channels || a->channels > 32) {
        return PREP_INVALID;
    }

    uint64_t end = z->playback_end ? z->playback_end : a->frames;

    if (end > a->frames) {
        r->fixes |= 1u << ES_FIX_LENGTH;
        end = a->frames;
    }

    if (z->playback_start >= end) {
        return PREP_INVALID;
    }

    if (end - z->playback_start > 32000000) {
        r->fixes |= 1u << ES_FIX_LENGTH;
        end = z->playback_start + 32000000;
    }

    if (z->loop_mode &&
        (z->loop_start < z->playback_start || z->loop_start >= z->loop_end || z->loop_end > end)) {
        r->fixes |= 1u << ES_FIX_LENGTH;
        z->loop_mode = DME_SAMPLE_NO_LOOP;
    }

    r->frames = (uint32_t)(end - z->playback_start);
    r->rate = a->rate;
    r->source_rate = a->rate;
    r->source_channels = a->channels;
    r->loop_start = z->loop_mode ? (uint32_t)(z->loop_start - z->playback_start) : 0;
    r->loop_end = z->loop_mode ? (uint32_t)(z->loop_end - z->playback_start) : 0;
    r->tuning =
        (double)a->rate / 32000 *
        pow(2, (60.0 - z->root - z->cents / 100 + z->tone.coarse + z->tone.fine / 100) / 12);

    if (!isfinite(r->tuning) || r->tuning <= 0) {
        return PREP_INVALID;
    }

    size_t header =
        native == ES_NATIVE_SM64 ? (z->loop_mode ? 124 : 54) : (z->loop_mode ? 112 : 44);
    size_t bytes = (size_t)r->frames * 2;
    r->audio = (EsFile){
        .data = calloc(1, header + bytes), .size = header + bytes, .exists = true, .mode = 0644};

    if (!r->audio.data) {
        es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
        return PREP_FAILED;
    }

    uint8_t *p = r->audio.data;
    bool big = native == ES_NATIVE_SM64;

    if (big) {
        memcpy(p, "FORM", 4);
        u32(p + 4, (uint32_t)r->audio.size - 8, true);
        memcpy(p + 8, "AIFFCOMM", 8);
        u32(p + 16, 18, true);
        u16(p + 20, 1, true);
        u32(p + 22, r->frames, true);
        u16(p + 26, 16, true);
        int exponent;
        double fraction = frexp((double)a->rate, &exponent);
        u16(p + 28, (uint16_t)(16382 + exponent), true);
        uint64_t mantissa = (uint64_t)ldexp(fraction, 64);
        u32(p + 30, (uint32_t)(mantissa >> 32), true);
        u32(p + 34, (uint32_t)mantissa, true);
        size_t offset = 38;

        if (z->loop_mode) {
            memcpy(p + offset, "MARK", 4);
            u32(p + offset + 4, 18, true);
            u16(p + offset + 8, 2, true);
            u16(p + offset + 10, 1, true);
            u32(p + offset + 12, r->loop_start, true);
            u16(p + offset + 18, 2, true);
            u32(p + offset + 20, r->loop_end, true);
            offset += 26;
            memcpy(p + offset, "INST", 4);
            u32(p + offset + 4, 20, true);
            p[offset + 8] = 60;
            p[offset + 11] = p[offset + 13] = 127;
            u16(p + offset + 16, 1, true);
            u16(p + offset + 18, 1, true);
            u16(p + offset + 20, 2, true);
            offset += 28;
            /* Reserve an even-sized annotation chunk to keep a stable PCM offset. */
            memcpy(p + offset, "ANNO", 4);
            u32(p + offset + 4, 8, true);
            offset += 16;
        }

        memcpy(p + offset, "SSND", 4);
        u32(p + offset + 4, (uint32_t)bytes + 8, true);
    } else {
        memcpy(p, "RIFF", 4);
        u32(p + 4, (uint32_t)r->audio.size - 8, false);
        memcpy(p + 8, "WAVEfmt ", 8);
        u32(p + 16, 16, false);
        u16(p + 20, 1, false);
        u16(p + 22, 1, false);
        u32(p + 24, a->rate, false);
        u32(p + 28, a->rate * 2, false);
        u16(p + 32, 2, false);
        u16(p + 34, 16, false);
        size_t offset = 36;

        if (z->loop_mode) {
            memcpy(p + offset, "smpl", 4);
            u32(p + offset + 4, 60, false);
            u32(p + offset + 16, 1000000000u / a->rate, false);
            u32(p + offset + 20, 60, false);
            u32(p + offset + 36, 1, false);
            u32(p + offset + 52, r->loop_start, false);
            /* OoT's converter copies this endpoint verbatim into its half-open loop. */
            u32(p + offset + 56, r->loop_end, false);
            offset += 68;
        }

        memcpy(p + offset, "data", 4);
        u32(p + offset + 4, (uint32_t)bytes, false);
    }

    /* Bound the exponent before conversion so even an extreme finite gain stays finite. */
    double gain = pow(10, fmax(-600, fmin(600, z->gain_db)) / 20), peak = 0;
    const DmeSampleToneParams *t = &z->tone;
    bool bake = t->cutoff > 0 || t->resonance > 0 || t->chorus > 0;
    int status = bake ? write_baked(r, a, handle, asset, header, big, gain, host, error, cap)
                      : write_pcm(r, a, handle, asset, header, big, gain, &peak, host, error, cap);

    /* Lower the gain just enough to fit rather than clip. */
    if (!bake && status == PREP_OK && peak > 1) {
        r->fixes |= 1u << ES_FIX_GAIN;
        status = write_pcm(r, a, handle, asset, header, big, gain / peak * (32766.0 / 32768), &peak,
                           host, error, cap);
    }

    if (status != PREP_OK) {
        return status;
    }

    if (a->channels > 2) {
        r->fixes |= 1u << ES_FIX_FORMAT;
    }

    if (a->rate > 192000) {
        r->fixes |= 1u << ES_FIX_FORMAT;
        int resampled =
            es_native_resample(r, r->tuning * (192000.0 / r->rate), native, host, error, cap);

        if (resampled != ES_RESAMPLE_OK) {
            return resampled == ES_RESAMPLE_LIMIT ? PREP_INVALID : PREP_FAILED;
        }
    }

    envelope(r, native);
    return PREP_OK;
}

void es_native_fix(EsNativeSamples *s, unsigned kind, const char *preset) {
    if (kind >= ES_FIX_COUNT) {
        return;
    }

    if (!s->fixes[kind].count++ && preset) {
        snprintf(s->fixes[kind].first, sizeof(s->fixes[kind].first), "%s", preset);
    }
}

void es_native_add_instead(EsNativeSamples *s, EsNativeRegion *r) {
    (void)s;
    free(r->target);
    r->target = NULL;
    r->reference_tuning = 0;
    snprintf(r->name, sizeof(r->name), "%s", r->add_name);
    r->fixes |= 1u << ES_FIX_REPLACE_ADD;
}

void es_native_fallback(EsNativeSamples *s, size_t region, unsigned kind) {
    uint16_t choice = s->regions[region].choice;
    size_t first = region, end = region + 1;

    while (first && s->regions[first - 1].choice == choice) {
        first--;
    }

    while (end < s->count && s->regions[end].choice == choice) {
        end++;
    }

    es_native_fix(s, kind, s->regions[region].preset);

    for (uint64_t i = 0; i < s->event_count; i++) {
        EsNativeNote *note = &s->notes[i];

        if (note->custom && note->choice == choice) {
            *note = (EsNativeNote){0};
        }
    }

    for (size_t i = 0; i < s->layer_count; i++) {
        if (s->layers[i] >= end) {
            s->layers[i] -= (uint16_t)(end - first);
        }
    }

    for (size_t i = first; i < end; i++) {
        es_file_free(&s->regions[i].audio);
        free(s->regions[i].target);
    }

    memmove(s->regions + first, s->regions + end, (s->count - end) * sizeof(*s->regions));
    s->count -= end - first;
}

int es_native_fit(EsNativeSamples *s, const double *overspeed, const bool *capacity,
                  uint32_t native, bool *other_bank, const DmeHost *host, char *error, size_t cap) {
    bool changed = false;

    /* Walk down so removing a selection never shifts a region still to be visited. */
    for (size_t i = s->count; i > 0;) {
        EsNativeRegion *region = &s->regions[--i];
        bool full = capacity && capacity[i];
        bool fast = !full && overspeed && overspeed[i] > 1;

        if (!full && !fast) {
            continue;
        }

        changed = true;

        /* A Replace keeps its consumers' calibration, so only an added copy can be refit. */
        if (region->target) {
            es_native_add_instead(s, region);
        }

        int status = full ? ES_RESAMPLE_LIMIT
                          : es_native_resample(region, region->tuning / overspeed[i] * .9999,
                                               native, host, error, cap);

        if (status == ES_RESAMPLE_FAILED) {
            return ES_FIT_FAILED;
        }

        if (status == ES_RESAMPLE_LIMIT) {
            uint16_t choice = region->choice;

            while (i && s->regions[i - 1].choice == choice) {
                i--;
            }

            es_native_fallback(s, i, ES_FIX_ORDINARY);
        }
    }

    /* Added or ordinary notes need the default sample bank, so every Replace becomes an Add. */
    if (changed && *other_bank) {
        for (size_t i = 0; i < s->count; i++) {
            if (s->regions[i].target) {
                es_native_add_instead(s, &s->regions[i]);
            }
        }

        *other_bank = false;
    }

    return changed ? ES_FIT_RETRY : ES_FIT_DONE;
}

unsigned es_native_pan(const EsNativeRegion *r) {
    return (unsigned)fmax(0, fmin(127, round(64 + r->params.pan * 63)));
}

static double tracking(const EsNativeRegion *r) {
    const DmeSampleToneParams *t = &r->params.tone;
    return t->custom_tracking ? t->tracking : r->percussion ? 0 : 100;
}

double es_native_drum_key(const EsNativeRegion *r, unsigned key) {
    return r->params.root + ((double)key - r->params.root) * tracking(r) / 100;
}

static const char *const fix_labels[ES_FIX_COUNT] = {
    [ES_FIX_PAN] = DM_TEXT_NATIVE_FIX_PAN_LABEL,
    [ES_FIX_CHOKE] = DM_TEXT_NATIVE_FIX_CHOKE_LABEL,
    [ES_FIX_SUSTAIN_LOOP] = DM_TEXT_NATIVE_FIX_SUSTAIN_LOOP_LABEL,
    [ES_FIX_FILTER] = DM_TEXT_NATIVE_FIX_FILTER_LABEL,
    [ES_FIX_FILTER_ENVELOPE] = DM_TEXT_NATIVE_FIX_FILTER_ENV_LABEL,
    [ES_FIX_CHORUS] = DM_TEXT_NATIVE_FIX_CHORUS_LABEL,
    [ES_FIX_CHORUS_SHORT] = DM_TEXT_NATIVE_FIX_CHORUS_SHORT_LABEL,
    [ES_FIX_TRACKING] = DM_TEXT_NATIVE_FIX_TRACKING_LABEL,
    [ES_FIX_GAIN] = DM_TEXT_NATIVE_FIX_GAIN_LABEL,
    [ES_FIX_TIMING] = DM_TEXT_NATIVE_FIX_TIMING_LABEL,
    [ES_FIX_RELEASE] = DM_TEXT_NATIVE_FIX_RELEASE_LABEL,
    [ES_FIX_LENGTH] = DM_TEXT_NATIVE_FIX_LENGTH_LABEL,
    [ES_FIX_FORMAT] = DM_TEXT_NATIVE_FIX_FORMAT_LABEL,
    [ES_FIX_REPLACE_ZONE] = DM_TEXT_NATIVE_FIX_REPLACE_ZONE_LABEL,
    [ES_FIX_REPLACE_ADD] = DM_TEXT_NATIVE_FIX_REPLACE_ADD_LABEL,
    [ES_FIX_ORDINARY] = DM_TEXT_NATIVE_FIX_ORDINARY_LABEL,
    [ES_FIX_IGNORED] = DM_TEXT_NATIVE_FIX_IGNORED_LABEL,
    [ES_FIX_DRUMS] = DM_TEXT_NATIVE_FIX_DRUMS_LABEL,
};

void es_native_samples_report(const EsNativeSamples *samples, uint32_t native,
                              const DmeHost *host) {
    (void)native;

    if (!host->diagnostic) {
        return;
    }

    /* One warning list item per preset and change, such as "epiano: chorus baked into the
       sample"; the host lists each item once. */
    char message[1536];

    for (unsigned k = 0; k < ES_FIX_COUNT; k++) {
        const EsNativeFix *fix = &samples->fixes[k];

        if (fix->count && *fix->first) {
            snprintf(message, sizeof(message), "%s\n%s: %s", DM_TEXT_EXPORT_LIST_SAMPLES,
                     fix->first, fix_labels[k]);
        } else if (fix->count) {
            snprintf(message, sizeof(message), DM_TEXT_NATIVE_FIX_ROWS, DM_TEXT_EXPORT_LIST_SAMPLES,
                     fix_labels[k], fix->count);
        } else {
            continue;
        }

        host->diagnostic(host->user, DME_WARNING, DME_POPUP | DME_LIST, message);
    }

    for (size_t i = 0; i < samples->count; i++) {
        const EsNativeRegion *r = &samples->regions[i];

        for (unsigned k = 0; k < ES_FIX_COUNT; k++) {
            if (r->fixes & (1u << k)) {
                snprintf(message, sizeof(message), "%s\n%s: %s", DM_TEXT_EXPORT_LIST_SAMPLES,
                         r->preset, fix_labels[k]);
                host->diagnostic(host->user, DME_WARNING, DME_POPUP | DME_LIST, message);
            }
        }
    }
}

void es_native_samples_free(EsNativeSamples *s) {
    for (size_t i = 0; i < s->count; i++) {
        es_file_free(&s->regions[i].audio);
        free(s->regions[i].target);
    }

    free(s->regions);
    free(s->notes);
    free(s->layers);
    *s = (EsNativeSamples){0};
}

static bool same_text(DmeString a, DmeString b) {
    return a.length == b.length && (!a.length || !memcmp(a.data, b.data, (size_t)a.length));
}

/* Rows choosing the same saved preset share its native regions, so one sample is written once
   and its derived names stay unique. A Replace is the same only for the same physical target. */
static bool same_preset(const DmeCustomSelection *a, const DmeCustomSelection *b) {
    return a->sample.flags == b->sample.flags && a->sample.generation == b->sample.generation &&
           same_text(a->sample.locator, b->sample.locator) &&
           same_text(a->sample.set_id, b->sample.set_id) &&
           same_text(a->sample.preset_id, b->sample.preset_id);
}

static bool same_choice(const DmeCustomSelection *a, const DmeCustomSelection *b) {
    return a == b || (a->operation == b->operation && same_preset(a, b) &&
                      (a->operation != DME_SAMPLE_REPLACE ||
                       (same_text(a->physical_target, b->physical_target) &&
                        a->region_set == b->region_set && a->replace_zone == b->replace_zone)));
}

typedef struct {
    uint32_t pending[128][16], banks[128][16], programs[128][16];
} SourceWalk;

/* Follows bank and program state in event order. Returns true for a note-on, with the source
   row it belongs to. */
static bool note_source(const DmeContext *c, uint64_t i, SourceWalk *w, DmeMappingSource *out) {
    const DmeEvent *e = &c->events[i];

    if (e->status < MIDI_STATUS || e->status >= MIDI_SYSTEM || e->port >= 128) {
        return false;
    }

    uint32_t ch = e->status & MIDI_CHANNEL_MASK, type = e->status & MIDI_STATUS_MASK;

    if (type == MIDI_PROGRAM) {
        w->programs[e->port][ch] = e->a;
        w->banks[e->port][ch] = w->pending[e->port][ch];
    } else if (type == MIDI_CONTROL && e->a == MIDI_CC_BANK) {
        w->pending[e->port][ch] = (w->pending[e->port][ch] & 127) | (e->b << 7);
    } else if (type == MIDI_CONTROL && e->a == MIDI_CC_BANK_LSB) {
        w->pending[e->port][ch] = (w->pending[e->port][ch] & ~127u) | e->b;
    }

    if (type != MIDI_NOTE_ON || !e->b) {
        return false;
    }

    const DmeEffectivePatch *patch = c->effective_patches ? &c->effective_patches[i] : NULL;
    bool drums = patch ? (patch->flags & DME_SAMPLE_PERCUSSION) != 0
                       : (c->percussion_channels & (1u << ch)) != 0;
    *out = (DmeMappingSource){.channel = ch,
                              .port = e->port,
                              .bank = patch ? patch->bank : w->banks[e->port][ch],
                              .program = drums   ? 0
                                         : patch ? patch->program
                                                 : w->programs[e->port][ch],
                              .drum_key = drums ? e->a : 0,
                              .flags = drums ? DME_SAMPLE_PERCUSSION : 0};
    return true;
}

static bool zone_matches(const DmeSampleZoneParams *z, const DmeEvent *e) {
    return e->a >= z->key_lo && e->a <= z->key_hi && e->b >= z->velocity_lo &&
           e->b <= z->velocity_hi;
}

/* A layered Replace without a chosen region uses the region this song plays most often. */
static bool likely_zone(const DmeContext *c, const EsMappingJob *mapping, uint32_t target,
                        const DmeCustomSelection *choice, const DmeFrozenSample *frozen,
                        SourceWalk *walk, const DmeHost *host, uint32_t *selected) {
    uint64_t *hits = calloc(frozen->zone_count, sizeof(*hits));
    uint32_t best = 0;

    if (!hits) {
        return false;
    }

    memset(walk, 0, sizeof(*walk));

    for (uint64_t i = 0; i < c->event_count; i++) {
        if (!(i % 64) && host->cancelled && host->cancelled(host->user)) {
            free(hits);
            return false;
        }

        DmeMappingSource source;

        if (!note_source(c, i, walk, &source)) {
            continue;
        }

        const DmeCustomSelection *row = es_mapping_job_selection(mapping, target, source);

        if (!row || !row->enabled || !same_choice(row, choice)) {
            continue;
        }

        for (uint32_t z = 0; z < frozen->zone_count; z++) {
            hits[z] += zone_matches(&frozen->zones[z].params, &c->events[i]);
        }
    }

    for (uint32_t z = 1; z < frozen->zone_count; z++) {
        best = hits[z] > hits[best] ? z : best;
    }

    free(hits);
    *selected = best;
    return true;
}

/* Added samples are named after their preset, as a file and symbol name the game tools accept:
   letters, digits and single underscores, never starting with a digit. A layered preset adds
   its one-based region number. A short suffix from the set, preset and region identity keeps
   presets that share a name in different sets from overwriting each other. */
static void region_name(char *out, size_t cap, const char *preset, uint32_t region,
                        const uint8_t *hash) {
    char base[49];
    size_t used = 0;

    for (size_t i = 0; preset[i] && used < sizeof(base) - 1; i++) {
        char c = preset[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            base[used++] = c;
        } else if (used && base[used - 1] != '_') {
            base[used++] = '_';
        }
    }

    while (used && base[used - 1] == '_') {
        used--;
    }

    base[used] = 0;
    const char *prefix = !used ? "sample" : base[0] >= '0' && base[0] <= '9' ? "s_" : "";
    char number[16] = "";

    if (region) {
        snprintf(number, sizeof(number), "_%u", region);
    }

    snprintf(out, cap, "%s%s%s_%02x%02x%02x", prefix, base, number, hash[0], hash[1], hash[2]);
}

static void rollback(EsNativeSamples *s, size_t start) {
    while (s->count > start) {
        EsNativeRegion *r = &s->regions[--s->count];
        es_file_free(&r->audio);
        free(r->target);
        *r = (EsNativeRegion){0};
    }
}

typedef struct {
    const DmeCustomSelection *choice;
    uint16_t start, count;
    uint16_t scope;
    bool failed; /* Its notes use the ordinary game instrument. */
} Choice;

enum { CHOICES = 1024 };

/* The bank has 128 custom drum entries, each a distinct region/key pair. Keep whole selections
   together: a row that cannot fit plays its ordinary drum instead of losing some layers. */
static void fit_drums(EsNativeSamples *s, const DmeContext *c) {
    bool pairs[ES_NATIVE_REGIONS][128] = {{0}};
    uint32_t entries[CHOICES] = {0};
    bool fallback[CHOICES] = {0};

    for (uint64_t i = 0; i < c->event_count; i++) {
        const DmeEvent *e = &c->events[i];
        const EsNativeNote *note = &s->notes[i];
        bool drums = c->effective_patches
                         ? (c->effective_patches[i].flags & DME_SAMPLE_PERCUSSION) != 0
                         : (c->percussion_channels & (1u << (e->status & MIDI_CHANNEL_MASK))) != 0;

        if (!note->custom || !drums || e->a >= 128) {
            continue;
        }

        for (unsigned j = 0; j < note->count; j++) {
            uint16_t region = s->layers[note->first + j];

            if (!pairs[region][e->a]) {
                pairs[region][e->a] = true;
                entries[s->regions[region].choice]++;
            }
        }
    }

    unsigned used = 0;

    for (unsigned i = 0; i < CHOICES; i++) {
        fallback[i] = entries[i] > 128 - used;

        if (!fallback[i]) {
            used += entries[i];
        }
    }

    for (size_t i = s->count; i > 0;) {
        i--;

        if (fallback[s->regions[i].choice]) {
            uint16_t choice = s->regions[i].choice;

            while (i && s->regions[i - 1].choice == choice) {
                i--;
            }

            es_native_fallback(s, i, ES_FIX_DRUMS);
        }
    }
}

/* Prepares one newly seen choice. Returns PREP_INVALID when the row falls back to its ordinary
   instrument; PREP_FAILED only for cancellation, read or memory failures. */
static int prepare_choice(EsNativeSamples *s, Choice *slot, const EsMappingJob *mapping,
                          uint32_t target, uint32_t native, const DmeContext *c,
                          const DmeHost *host, SourceWalk *scratch, uint16_t choice_id, char *error,
                          size_t cap) {
    const DmeCustomSelection *choice = slot->choice;
    DmeFrozenSample frozen = {.size = sizeof(frozen)};

    if (!choice->sample.locator.length || !choice->sample.set_id.length ||
        !choice->sample.preset_id.length || !choice->sample.generation) {
        es_native_fix(s, ES_FIX_ORDINARY, NULL);
        return PREP_INVALID;
    }

    int32_t frozen_status = host->freeze_sample(host->user, &choice->sample, &frozen, error, cap);

    if (frozen_status != DME_OK) {
        if ((frozen_status != DME_INVALID && frozen_status != DME_UNAVAILABLE) ||
            (host->cancelled && host->cancelled(host->user))) {
            return PREP_FAILED;
        }

        *error = 0;
        es_native_fix(s, ES_FIX_ORDINARY, NULL);
        return PREP_INVALID;
    }

    if (frozen.size != sizeof(frozen) || frozen.preset_count != 1 || !frozen.presets ||
        frozen.presets[0].size != sizeof(DmeSamplePreset) || !frozen.zones || !frozen.assets ||
        !frozen.asset_count || frozen.asset_count > 32768 || frozen.presets[0].first_zone ||
        frozen.presets[0].zone_count != frozen.zone_count || !frozen.zone_count ||
        frozen.zone_count > 32768) {
        es_native_fix(s, ES_FIX_ORDINARY, NULL);
        return PREP_INVALID;
    }

    char preset[96];
    DmeString name = frozen.presets[0].name;
    snprintf(preset, sizeof(preset), "%.*s",
             !name.data         ? 0
             : name.length > 95 ? 95
                                : (int)name.length,
             name.data ? name.data : "");
    bool replacing = choice->operation == DME_SAMPLE_REPLACE;
    uint32_t replace_zone = choice->replace_zone;

    if (replacing && frozen.zone_count > 1 &&
        (!choice->region_set || replace_zone >= frozen.zone_count)) {
        if (!likely_zone(c, mapping, target, choice, &frozen, scratch, host, &replace_zone)) {
            if (!host->cancelled || !host->cancelled(host->user)) {
                es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
            }

            return PREP_FAILED;
        }

        es_native_fix(s, ES_FIX_REPLACE_ZONE, preset);
    }

    uint32_t count = replacing ? 1 : frozen.zone_count;

    if (count > ES_NATIVE_REGIONS - s->count) {
        es_native_fix(s, ES_FIX_ORDINARY, preset);
        return PREP_INVALID;
    }

    slot->start = (uint16_t)s->count;
    slot->count = (uint16_t)count;

    for (uint32_t selected_zone = 0; selected_zone < count; selected_zone++) {
        uint32_t zone = replacing && frozen.zone_count > 1 ? replace_zone : selected_zone;

        if (!es_reserve((void **)&s->regions, &s->capacity, s->count + 1, sizeof(*s->regions))) {
            es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
            return PREP_FAILED;
        }

        EsNativeRegion *r = &s->regions[s->count++];
        *r = (EsNativeRegion){.params = frozen.zones[zone].params,
                              .choice = choice_id,
                              .preset_scope = slot->scope,
                              .percussion = (choice->sample.flags & DME_SAMPLE_PERCUSSION) != 0};
        snprintf(r->preset, sizeof(r->preset), "%s", preset);
        uint8_t hash[32];
        char identity[18000];
        int n = snprintf(identity, sizeof(identity), "%.*s/%.*s/%u",
                         (int)choice->sample.set_id.length, choice->sample.set_id.data,
                         (int)choice->sample.preset_id.length, choice->sample.preset_id.data, zone);

        if (n < 0 || (size_t)n >= sizeof(identity)) {
            rollback(s, slot->start);
            es_native_fix(s, ES_FIX_ORDINARY, preset);
            return PREP_INVALID;
        }

        es_sha256(identity, (size_t)n, hash);
        region_name(r->name, sizeof(r->name), preset, frozen.zone_count > 1 ? zone + 1 : 0, hash);
        snprintf(r->add_name, sizeof(r->add_name), "%s", r->name);

        if (replacing) {
            size_t length = (size_t)choice->physical_target.length;

            if (length && length <= 8192 && choice->physical_target.data) {
                r->target = malloc(length + 1);

                if (!r->target) {
                    es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
                    return PREP_FAILED;
                }

                memcpy(r->target, choice->physical_target.data, length);
                r->target[length] = 0;
            } else {
                r->fixes |= 1u << ES_FIX_REPLACE_ADD;
            }
        }

        uint32_t asset = frozen.zones[zone].asset;
        int status =
            frozen.zones[zone].size == sizeof(DmeSampleZone) && asset < frozen.asset_count &&
                    sanitize(r)
                ? audio(r, &frozen.assets[asset], frozen.handle, asset, native, host, error, cap)
                : PREP_INVALID;

        if (status != PREP_OK) {
            rollback(s, slot->start);

            if (status == PREP_INVALID) {
                es_native_fix(s, ES_FIX_ORDINARY, preset);
            }

            return status;
        }
    }

    return PREP_OK;
}

bool es_native_samples(EsNativeSamples *s, const EsMappingJob *mapping, uint32_t target,
                       uint32_t native, const DmeContext *c, const DmeHost *host, char *error,
                       size_t cap) {
    if (s->notes) {
        return s->event_count == c->event_count;
    }

    s->event_count = c->event_count;

    if (!es_mapping_job_custom(mapping, target)) {
        return true;
    }

    if (!host->freeze_sample || !host->read_frozen_sample || c->event_count > 2000000) {
        es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_SERVICE);
        return false;
    }

    s->notes = calloc((size_t)c->event_count + 1, sizeof(*s->notes));
    Choice *choices = calloc(CHOICES, sizeof(*choices));
    SourceWalk *walk = calloc(2, sizeof(*walk));
    size_t choice_count = 0;
    bool ok = s->notes && choices && walk, overflow = false;

    if (!ok) {
        es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
    }

    for (uint64_t i = 0; ok && i < c->event_count; i++) {
        if (!(i % 4096) && host->cancelled && host->cancelled(host->user)) {
            ok = false;
            break;
        }

        const DmeEvent *e = &c->events[i];
        DmeMappingSource source;

        if (!note_source(c, i, &walk[0], &source)) {
            continue;
        }

        const DmeCustomSelection *choice = es_mapping_job_selection(mapping, target, source);

        if (!choice || !choice->enabled) {
            continue;
        }

        size_t selection;

        for (selection = 0;
             selection < choice_count && !same_choice(choices[selection].choice, choice);
             selection++) {
        }

        if (selection == choice_count) {
            if (choice_count == CHOICES) {
                if (!overflow) {
                    overflow = true;
                    es_native_fix(s, ES_FIX_ORDINARY, NULL);
                }

                continue;
            }

            choices[choice_count++] = (Choice){.choice = choice};

            size_t scope = 0;

            while (scope < selection && !same_preset(choices[scope].choice, choice)) {
                scope++;
            }

            choices[selection].scope = scope < selection ? choices[scope].scope : (uint16_t)scope;
            int status = prepare_choice(s, &choices[selection], mapping, target, native, c, host,
                                        &walk[1], (uint16_t)selection, error, cap);
            ok = status != PREP_FAILED;
            choices[selection].failed = status != PREP_OK;
        }

        if (!ok || choices[selection].failed) {
            continue;
        }

        const Choice *slot = &choices[selection];

        if (s->layer_count > 16000000 - slot->count) {
            es_native_fix(s, ES_FIX_ORDINARY, s->regions[slot->start].preset);
            continue;
        }

        EsNativeNote *note = &s->notes[i];
        note->custom = true;
        note->choice = (uint16_t)selection;
        note->first = (uint32_t)s->layer_count;

        for (unsigned j = 0; j < slot->count; j++) {
            uint16_t index = slot->start + j;

            if (!zone_matches(&s->regions[index].params, e)) {
                continue;
            }

            /* Percussion rows become drum entries, which follow any tracking exactly. A melodic
               instrument always tracks 100%. */
            if (!(source.flags & DME_SAMPLE_PERCUSSION) && tracking(&s->regions[index]) != 100) {
                s->regions[index].fixes |= 1u << ES_FIX_TRACKING;
            }

            if (!es_reserve((void **)&s->layers, &s->layer_capacity, s->layer_count + 1,
                            sizeof(*s->layers))) {
                es_fail(error, cap, DM_TEXT_NATIVE_SAMPLE_MEMORY);
                ok = false;
                break;
            }

            s->layers[s->layer_count++] = index;
            note->count++;
        }
    }

    free(choices);
    free(walk);

    if (ok) {
        fit_drums(s, c);
    }

    return ok;
}
