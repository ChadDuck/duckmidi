#include "sample.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Native note updates per second at 32 kHz NTSC: SM64 and HackerSM64 run four per 60 Hz frame,
   OoT three. Vibrato phase advances 32 * byte per update over a 65536-step cycle. */
static double updates(uint32_t native) {
    return native == ES_NATIVE_SM64 ? 240 : 180;
}

static unsigned clamp_byte(double value, unsigned lo, unsigned hi) {
    return (unsigned)fmax(lo, fmin(hi, round(value)));
}

static unsigned rate_byte(double hz, uint32_t native) {
    return clamp_byte(hz * 2048 / updates(native), 1, 255);
}

/* SM64 scales the frequency offset linearly, so its upward and downward cents differ. Choose the
   byte whose larger peak error is smallest. HackerSM64 reserves 255 to disable vibrato. OoT's
   law is symmetric in cents. */
static unsigned depth_byte(double cents, uint32_t native, bool hacker) {
    if (cents <= 0) {
        return 0;
    }

    if (native == ES_NATIVE_OOT) {
        return clamp_byte(512 * (pow(2, cents / 1200) - 1), 1, 255);
    }

    const double up = pow(2, 120.0 / 127) - 1, down = pow(2, -120.0 / 127) - 1;
    unsigned best = 1;
    double best_error = HUGE_VAL;

    for (unsigned b = 1; b <= (hacker ? 254u : 255u); b++) {
        double plus = 1200 * log2(1 + b / 512.0 * up);
        double minus = -1200 * log2(1 + b / 512.0 * down);
        double error = fmax(fabs(plus - cents), fabs(minus - cents));

        if (error < best_error) {
            best_error = error;
            best = b;
        }
    }

    return best;
}

typedef struct {
    int value[ES_FX_KINDS]; /* -1 leaves that kind unchanged. */
} Wanted;

static Wanted region_fx(const EsNativeRegion *r, uint32_t native, bool hacker, bool midi_depth) {
    const DmeSampleToneParams *t = &r->params.tone;
    Wanted w = {{clamp_byte(fmax(0, fmin(1, t->reverb)) * 127, 0, 127), -1, -1}};
    unsigned depth = depth_byte(t->vibrato_depth, native, hacker);

    /* With CC1 on the channel MIDI owns depth, but a set preset rate still applies. */
    if (!midi_depth) {
        w.value[ES_FX_DEPTH] = (int)depth;
    }

    if (t->vibrato_rate > 0 || (depth && !midi_depth)) {
        w.value[ES_FX_RATE] = (int)rate_byte(t->vibrato_rate > 0 ? t->vibrato_rate : 5, native);
    }

    return w;
}

typedef struct {
    uint64_t start;
    size_t index;
} Order;

/* Start order; equal starts keep their input order. */
static int note_order(const void *left, const void *right) {
    const Order *a = left, *b = right;
    return a->start < b->start   ? -1
           : a->start > b->start ? 1
           : a->index < b->index ? -1
                                 : a->index > b->index;
}

bool es_native_channel_fx(const EsNativeSamples *samples, const EsNativeFxNote *notes, size_t count,
                          uint32_t native, bool hacker, unsigned initial_rate,
                          const uint16_t midi[ES_FX_KINDS], EsNativeFxChange **changes,
                          size_t *change_count, uint16_t *shared) {
    *changes = NULL;
    *change_count = 0;
    *shared = 0;
    Wanted *wanted = samples && samples->count ? calloc(samples->count * 2, sizeof(*wanted)) : NULL;
    uint16_t active[ES_FX_KINDS] = {0};

    if (samples && samples->count && !wanted) {
        return false;
    }

    /* Each region has a value with and without MIDI depth ownership. */
    for (size_t i = 0; wanted && i < samples->count; i++) {
        wanted[i * 2] = region_fx(&samples->regions[i], native, hacker, false);
        wanted[i * 2 + 1] = region_fx(&samples->regions[i], native, hacker, true);
    }

    /* Only channels where a custom note sets a non-default value change; others stay as is. */
    for (size_t i = 0; wanted && i < count; i++) {
        const EsNativeFxNote *n = &notes[i];

        if (n->region < 0 || n->channel >= 16) {
            continue;
        }

        const DmeSampleToneParams *t = &samples->regions[n->region].params.tone;
        bool sets[ES_FX_KINDS] = {t->reverb > 0, t->vibrato_rate > 0 || t->vibrato_depth > 0,
                                  t->vibrato_depth > 0};

        for (unsigned k = 0; k < ES_FX_KINDS; k++) {
            if (sets[k] && !(midi[k] & (1u << n->channel))) {
                active[k] |= 1u << n->channel;
            }
        }
    }

    if (!active[ES_FX_REVERB] && !active[ES_FX_RATE] && !active[ES_FX_DEPTH]) {
        free(wanted);
        return true;
    }

    Order *order = malloc(count * sizeof(*order));
    EsNativeFxChange *out = malloc(count * ES_FX_KINDS * sizeof(*out));

    if (!order || !out) {
        free(order);
        free(out);
        free(wanted);
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        order[i] = (Order){notes[i].start, i};
    }

    qsort(order, count, sizeof(*order), note_order);
    /* Channels start without reverb send or vibrato depth, at the converter's rate. */
    int current[16][ES_FX_KINDS];
    uint64_t holder[16][ES_FX_KINDS] = {{0}};
    size_t used = 0;

    for (unsigned ch = 0; ch < 16; ch++) {
        current[ch][ES_FX_REVERB] = current[ch][ES_FX_DEPTH] = 0;
        current[ch][ES_FX_RATE] = (int)initial_rate;
    }

    for (size_t i = 0; i < count; i++) {
        const EsNativeFxNote *n = &notes[order[i].index];

        if (n->channel >= 16) {
            continue;
        }

        bool midi_depth = (midi[ES_FX_DEPTH] & (1u << n->channel)) != 0;
        /* Ordinary notes return to the defaults; they leave the rate as it is. */
        Wanted w = n->region >= 0 ? wanted[n->region * 2 + midi_depth] : (Wanted){{0, -1, 0}};

        for (unsigned k = 0; k < ES_FX_KINDS; k++) {
            int value = w.value[k];

            if (!(active[k] & (1u << n->channel)) || value < 0) {
                continue;
            }

            if (value == current[n->channel][k]) {
                holder[n->channel][k] =
                    holder[n->channel][k] > n->end ? holder[n->channel][k] : n->end;
                continue;
            }

            /* A note still sounding with the previous value hears the new one. */
            if (holder[n->channel][k] > n->start) {
                *shared |= 1u << n->channel;
            }

            out[used++] = (EsNativeFxChange){n->start, n->channel, (uint8_t)k, (uint8_t)value};
            current[n->channel][k] = value;
            holder[n->channel][k] = n->end;
        }
    }

    free(order);
    free(wanted);
    *changes = out;
    *change_count = used;
    return true;
}
