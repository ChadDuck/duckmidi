/* sm64coopdx target: mods replace a sequence with smlua_audio_utils_replace_sequence, which plays
   it with one existing bank and cannot add banks. The sequence therefore uses the extended bank
   0x2A, which collects the instruments of SM64's music banks, instead of a generated bank.

   Each mapped sample plays with the bank instrument that has that sample and the closest
   envelope. Stock instruments are tuned like stock sequences: note 39 is C4, so melodic notes
   are written 21 semitones below their MIDI key. Drums use the bank's percussion where it has
   the sample, and otherwise a melodic instrument at the note that gives the drum's pitch.

   A layer can select instruments 0 to 126 and the percussion (127); later instruments are only
   reachable as the channel's instrument, so they are used only on channels with one instrument. */
#include "internal.h"
#include <ctype.h>

static int sample_index(const char *name) {
    unsigned value;

    if (!name || !isxdigit((unsigned char)name[0]) || !isxdigit((unsigned char)name[1]) ||
        sscanf(name, "%2x", &value) != 1) {
        return -1;
    }

    return (int)value;
}

/* Samples found only in instruments 127 and later, with the layer-selectable sample that plays
   in their place on channels that change instruments. */
static const int layer_substitutes[][2] = {{0x36, 0x1B}, {0x37, 0x1B}, {0x41, 0x34},
                                           {0x43, 0x2E}, {0x44, 0x04}, {0x45, 0x16}};

static double ratio_cost(double a, double b) {
    return fabs(log((a > 0 ? a : 1) / (b > 0 ? b : 1)));
}

static double melodic_cost(const M64Instrument *d, int index) {
    const CoopdxInstrument *b = &coopdx_instruments[index];
    int sample = sample_index(d->sample);
    double cost;

    if (!b->present || sample < 0) {
        return -1;
    }

    if (b->normal.sample == sample) {
        cost = 0;
    } else if (b->lo.sample == sample || b->hi.sample == sample) {
        cost = 1;
    } else {
        return -1;
    }

    cost += ratio_cost(b->release_rate, d->release_rate);
    cost += b->sustained != (d->sustain > 0) ? 2 : 0;
    cost += fabs((double)b->attack - (double)d->attack) / 8;
    return cost + (index >= COOPDX_PERCUSSION ? 0.01 : 0);
}

static int best_instrument(const M64Instrument *d, bool layer) {
    int best = -1;
    double best_cost = 0;

    for (int i = 0; i < COOPDX_INSTRUMENTS; i++) {
        /* 0x7F selects percussion and 0x80-0x83 select synthetic waves. */
        if (i >= COOPDX_PERCUSSION && (layer || i <= 0x83)) {
            continue;
        }

        double cost = melodic_cost(d, i);

        if (cost >= 0 && (best < 0 || cost < best_cost)) {
            best = i;
            best_cost = cost;
        }
    }

    return best;
}

/* Drum samples. Melodic instruments that hold them play each in its own note range only. */
static bool unpitched(int sample) {
    return (sample >= 0x06 && sample <= 0x13) || sample == 0x1E || sample == 0x1F ||
           sample == 0x20 || sample == 0x32 || sample == 0x33 || sample == 0x38;
}

/* The notes of a bank instrument that play the sample; false if it does not have it. */
static bool sample_range(int index, int sample, int *low, int *high) {
    const CoopdxInstrument *b = &coopdx_instruments[index];

    if (b->normal.sample == sample) {
        *low = b->range_lo;
        *high = b->range_hi;
    } else if (b->lo.sample == sample) {
        *low = 0;
        *high = b->range_lo - 1;
    } else if (b->hi.sample == sample) {
        *low = b->range_hi + 1;
        *high = 127;
    } else {
        return false;
    }

    return *low <= *high;
}

/* The percussion entry with the sample whose pitch is closest to the rate; -1 if none. */
static int nearest_drum(int sample, double rate) {
    int best = -1;

    for (int i = 0; i < 64; i++) {
        if (coopdx_drums[i].sample == sample &&
            (best < 0 || ratio_cost(rate, coopdx_drums[i].tuning) <
                             ratio_cost(rate, coopdx_drums[best].tuning))) {
            best = i;
        }
    }

    return best;
}

/* Drums: the bank's percussion entry with the sample, or a melodic instrument note. */
static void resolve_drum(Compile *c, unsigned key) {
    const M64Instrument *d = drum_definition(c, key);
    int sample = sample_index(d->sample);
    double best = -1, exact_pitch = 0;

    for (int i = 0; i < 64; i++) {
        if (coopdx_drums[i].sample == sample) {
            double cost = fabs(12 * log2(d->tuning / coopdx_drums[i].tuning));

            if (best < 0 || cost < best) {
                best = cost;
                c->drum_instrument[key] = COOPDX_PERCUSSION;
                c->drum_pitch[key] = i;
                exact_pitch = i;
            }
        }
    }

    for (int i = 0; i < COOPDX_PERCUSSION; i++) {
        const CoopdxInstrument *b = &coopdx_instruments[i];
        const CoopdxSound *sounds[3] = {&b->lo, &b->normal, &b->hi};
        int low[3] = {0, b->range_lo, b->range_hi + 1};
        int high[3] = {b->range_lo - 1, b->range_hi, 127};

        for (int s = 0; s < 3 && b->present; s++) {
            if (sounds[s]->sample != sample || low[s] > high[s]) {
                continue;
            }

            double exact = 39 + 12 * log2(d->tuning / sounds[s]->tuning);
            double pitch = floor(exact + 0.5);
            pitch = pitch < low[s] ? low[s] : pitch > high[s] ? high[s] : pitch;
            double cost = fabs(pitch - exact) + 0.25 + 0.1 * ratio_cost(b->release_rate, 10);

            if (best < 0 || cost < best) {
                best = cost;
                c->drum_instrument[key] = i;
                c->drum_pitch[key] = (int)pitch;
                exact_pitch = exact;
            }
        }
    }

    if (best < 0) {
        c->drum_instrument[key] = -1;
        diagnose(c, M64_WARNING, 72, NULL,
                 "Drum note %u uses %s, which the sm64coopdx bank does not have; it is silent.",
                 key, d->sample);
    } else if (c->drum_instrument[key] != COOPDX_PERCUSSION &&
               fabs(c->drum_pitch[key] - exact_pitch) > 1) {
        double shift = c->drum_pitch[key] - exact_pitch;
        diagnose(c, M64_INFO, 71, NULL,
                 "Drum note %u plays %s %.0f semitones %s than mapped: the sm64coopdx bank has "
                 "that sample only in a fixed note range.",
                 key, d->sample, fabs(shift), shift > 0 ? "higher" : "lower");
    }
}

bool coopdx_resolve(Compile *c) {
    bool switches[16] = {0};
    int first[16];

    for (unsigned slot = 0; slot < c->program_count; slot++) {
        const M64Instrument *d = program_definition(c, c->programs[slot]);
        c->fixed[slot] = best_instrument(d, false);
        c->fixed_layer[slot] = best_instrument(d, true);

        for (size_t i = 0;
             c->fixed_layer[slot] < 0 && i < sizeof(layer_substitutes) / sizeof(*layer_substitutes);
             i++) {
            if (layer_substitutes[i][0] == sample_index(d->sample)) {
                char name[4];
                M64Instrument substitute = *d;
                snprintf(name, sizeof(name), "%02X", layer_substitutes[i][1]);
                substitute.sample = name;
                c->fixed_layer[slot] = best_instrument(&substitute, true);
            }
        }

        /* A sample the bank has only as percussion plays from there at the nearest pitch. */
        if (c->fixed[slot] < 0 && nearest_drum(sample_index(d->sample), d->tuning) >= 0) {
            c->fixed[slot] = c->fixed_layer[slot] = COOPDX_PERCUSSION;
            diagnose(c, M64_INFO, 71, NULL,
                     "%s is a percussion sound in the sm64coopdx bank; notes play its closest "
                     "pitch.",
                     d->sample);
        }

        if (c->fixed[slot] < 0) {
            c->result->status = M64_UNSUPPORTED;
            diagnose(c, M64_ERROR, 70, NULL,
                     "The sm64coopdx bank has no instrument with %s; choose another mapping.",
                     d->sample);
            return false;
        }
    }

    for (unsigned key = 0; key < 384; key++) {
        if (c->drums[key] >= 0) {
            resolve_drum(c, key);
        }
    }

    for (unsigned ch = 0; ch < 16; ch++) {
        first[ch] = -1;
    }

    for (size_t i = 0; i < c->count; i++) {
        const Note *n = &c->notes[i];

        if (n->channel == MIDI_DRUM_CHANNEL || n->output >= 16) {
            continue;
        }

        int instrument = c->fixed[c->slots[n->program]];

        if (first[n->output] < 0) {
            first[n->output] = instrument;
        } else if (first[n->output] != instrument) {
            switches[n->output] = true;
        }
    }

    /* A layer that switches instruments sets one before its first note of each section, since
       a loop can return to a section with another instrument selected. */
    bool melodic_drums = false;

    for (unsigned key = 0; key < 384; key++) {
        melodic_drums |= c->drums[key] >= 0 && c->drum_instrument[key] >= 0 &&
                         c->drum_instrument[key] != COOPDX_PERCUSSION;
    }

    for (unsigned ch = 0; ch < 16; ch++) {
        bool drums = c->sources[ch] == MIDI_DRUM_CHANNEL;
        c->switches[ch] = switches[ch];
        c->channel_instrument[ch] = drums                           ? COOPDX_PERCUSSION
                                    : switches[ch] || first[ch] < 0 ? 0
                                                                    : first[ch];
        c->layer_instrument[ch] = drums          ? (melodic_drums ? -1 : COOPDX_PERCUSSION)
                                  : switches[ch] ? -1
                                                 : first[ch];
    }

    for (unsigned slot = 0; slot < c->program_count; slot++) {
        if (c->fixed[slot] == c->fixed_layer[slot]) {
            continue;
        }

        for (unsigned ch = 0; ch < 16; ch++) {
            if (!switches[ch]) {
                continue;
            }

            for (size_t i = 0; i < c->count; i++) {
                const Note *n = &c->notes[i];

                if (n->output == ch && n->channel != MIDI_DRUM_CHANNEL &&
                    c->slots[n->program] == (int)slot) {
                    const char *sample = program_definition(c, c->programs[slot])->sample;

                    if (c->fixed_layer[slot] < 0) {
                        diagnose(c, M64_WARNING, 72, NULL,
                                 "Channel %u changes instruments, and the sm64coopdx bank has %s "
                                 "only as a channel instrument; its notes are silent.",
                                 ch + 1, sample);
                    } else {
                        diagnose(c, M64_WARNING, 71, NULL,
                                 "Channel %u changes instruments, so %s plays with the closest "
                                 "instrument a note layer can select in the sm64coopdx bank.",
                                 ch + 1, sample);
                    }

                    ch = 16;
                    break;
                }
            }
        }
    }

    return true;
}

/* The bank instrument and note value for one note; instrument -1 plays nothing. */
void coopdx_note(const Compile *c, const Note *n, int *instrument, unsigned *pitch) {
    if (n->channel == MIDI_DRUM_CHANNEL) {
        *instrument = c->drum_instrument[n->drum];
        *pitch = (unsigned)c->drum_pitch[n->drum];
        return;
    }

    int slot = c->slots[n->program];
    const M64Instrument *d = program_definition(c, n->program);
    int sample = sample_index(d->sample);
    *instrument = c->switches[n->output] ? c->fixed_layer[slot] : c->fixed[slot];

    if (*instrument == COOPDX_PERCUSSION) {
        double rate = d->tuning * pow(2.0, ((double)n->key - 60) / 12.0);
        *pitch = (unsigned)nearest_drum(sample, rate);
        return;
    }

    int key = (int)n->key - 21;

    while (key < 0) {
        key += 12;
    }

    /* An unpitched sample stays in its own note range, where another sample would play. */
    int low, high;

    if (*instrument >= 0 && unpitched(sample) && sample_range(*instrument, sample, &low, &high)) {
        key = key < low ? low : key > high ? high : key;
    }

    *pitch = (unsigned)key;
}
