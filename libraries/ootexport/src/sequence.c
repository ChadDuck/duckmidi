/* MIDI to OoT sequence (aseq.h MML assembly) and its soundfont.

   Notes are counted in sequence ticks (48 per beat, fewer for tempos above 255 BPM). Each MIDI
   channel becomes a sequence channel whose script sets the instrument and controllers; its notes
   are spread over up to four layers, and more overlapping notes move to free channels that copy
   the controllers. Layer scripts are cut at bar lines, and bars that repeat become subroutines. */
#include "oot.h"
#include "../../export_support/curves.h"
#include "../../../src/text/en.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DELAY 0x7FFFu
#define DRUM_INSTRUMENT 127
#define SECTIONS 2

enum {
    CTRL_INSTR,
    CTRL_VOL,
    CTRL_EXPR,
    CTRL_PAN,
    CTRL_REVERB,
    CTRL_BEND,
    CTRL_VIB,
    CTRL_VIBFREQ, /* Preset vibrato rate; -1 leaves the engine default. */
    CTRL_KINDS
};

typedef struct {
    uint64_t start, end;
    uint8_t channel, key, velocity;
    bool drum;
    uint16_t program; /* GM program at note start (drums: the key) */
    int value;        /* Mapping value */
    int voice;
    uint64_t source_event;
    double peak_factor;
} Note;

typedef struct {
    uint64_t tick;
    uint8_t channel, kind;
    int value;
} Control;

typedef struct {
    uint64_t tick;
    unsigned micros; /* Microseconds per quarter */
} Tempo;

/* One layer command, with its encoded size. */
typedef struct {
    uint64_t at;
    uint32_t size;
    char text[48];
} Command;

typedef struct {
    uint64_t hash;
    size_t first, count; /* Commands */
    int entry;           /* Layer transposition on entry */
    int velocity, gate;  /* Short-note velocity and gate on entry, or -1 */
    int sub;             /* Subroutine index, or -1 */
} Chunk;

/* One emitted channel command, kept so identical stream tails can be found before writing. */
typedef struct {
    char text[48];
    uint8_t size;
} Emitted;

typedef struct {
    Emitted *items;
    size_t count, capacity;
    bool failed;
} Emitting;

typedef struct {
    const OotRequest *request;
    const DmeHost *host;
    OotResult *result;
    char *error;
    size_t capacity;
    unsigned tpb; /* Sequence ticks per beat */
    Note *notes;
    size_t note_count, note_capacity;
    Control *controls;
    size_t control_count, control_capacity;
    Tempo *tempos;
    size_t tempo_count, tempo_capacity;
    uint64_t sections[SECTIONS][2];
    unsigned section_count;
    bool loops;
    int out_of[16];            /* Output channel -> source channel, or -1 */
    int group_channel[16][16]; /* Source channel, voice group -> output channel */
    bool drum_out[16];
    bool has_pan[16], preset_pan[16], custom_pan[16];
    uint16_t midi_fx[ES_FX_KINDS];          /* Channels whose MIDI reverb or modulation wins. */
    int programs[1024 + ES_NATIVE_REGIONS]; /* Custom font program per value, or -1 */
    int instrument_order[128];
    unsigned instrument_count;
    int drum_for_key[128]; /* Custom font: drum per key, or -1 */
    int drum_original[128];
    Command *commands;
    size_t command_count, command_capacity;
    Chunk *chunks;
    size_t chunk_count, chunk_capacity;
    unsigned sub_count;
    uint64_t bytes;
    Emitting *capture; /* When set, emit() records commands here instead of writing them. */
    bool short_notes;  /* Channels use short notes: velocity and gate are layer state. */
    bool warned_long, warned_low, warned_drum_key, warned_unsupported;
} Song;

static bool grow(void **data, size_t *capacity, size_t count, size_t width) {
    if (count < *capacity) {
        return true;
    }

    size_t next = *capacity ? *capacity * 2 : 256;
    void *grown = realloc(*data, next * width);

    if (!grown) {
        return false;
    }

    *data = grown;
    *capacity = next;
    return true;
}

static void diagnose(Song *s, uint32_t severity, const char *format, ...) {
    OotResult *r = s->result;

    if (!grow((void **)&r->diagnostics, &r->diagnostic_capacity, r->diagnostic_count,
              sizeof(*r->diagnostics))) {
        return;
    }

    OotDiagnostic *d = &r->diagnostics[r->diagnostic_count++];
    d->severity = severity;
    d->flags = 0;
    va_list args;
    va_start(args, format);
    vsnprintf(d->text, sizeof(d->text), format, args);
    va_end(args);
}

/* A warning-list item under a heading; ALTERED when notes had to change to fit. */
static void diagnose_item(Song *s, uint32_t flags, const char *heading, const char *format, ...) {
    char item[256];
    va_list args;
    va_start(args, format);
    vsnprintf(item, sizeof(item), format, args);
    va_end(args);
    diagnose(s, DME_WARNING, "%s\n%s", heading, item);

    if (s->result->diagnostic_count) {
        s->result->diagnostics[s->result->diagnostic_count - 1].flags = DME_LIST | flags;
    }
}

static bool cancelled(Song *s, uint32_t percent) {
    if (s->host && s->host->progress) {
        s->host->progress(s->host->user, percent);
    }

    return s->host && s->host->cancelled && s->host->cancelled(s->host->user);
}

static unsigned var_size(uint64_t value) {
    return value < 0x80 ? 1 : 2;
}

static uint64_t to_seq(const Song *s, uint64_t tick) {
    uint32_t ppq = s->request->ppq;
    return (tick * s->tpb + ppq / 2) / ppq;
}

bool oot_name_valid(const char *name) {
    if (!name || !*name || strlen(name) > 48 || !(name[0] >= 'a' && name[0] <= 'z')) {
        return false;
    }

    for (const char *c = name; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) {
            return false;
        }
    }

    return true;
}

/* ---- Reading the arrangement ---- */

static bool add_control(Song *s, uint64_t tick, unsigned channel, unsigned kind, int value) {
    if (!grow((void **)&s->controls, &s->control_capacity, s->control_count,
              sizeof(*s->controls))) {
        return false;
    }

    s->controls[s->control_count++] = (Control){tick, (uint8_t)channel, (uint8_t)kind, value};
    return true;
}

static bool close_note(Song *s, int *open, uint64_t tick) {
    if (*open >= 0) {
        Note *n = &s->notes[*open];
        n->end = tick > n->start ? tick : n->start + 1;
        *open = -1;
    }

    return true;
}

/* Preset reverb and vibrato become channel controls at note starts; see es_native_channel_fx.
   They are merged into the tick-ordered control list, after MIDI controls at the same tick. */
static bool custom_effects(Song *s) {
    static const uint8_t kinds[ES_FX_KINDS] = {CTRL_REVERB, CTRL_VIBFREQ, CTRL_VIB};
    const EsNativeSamples *custom = s->request->custom;
    EsNativeFxNote *notes = malloc((s->note_count + 1) * sizeof(*notes));
    size_t count = 0;

    if (!notes) {
        return false;
    }

    for (size_t i = 0; i < s->note_count; i++) {
        const Note *n = &s->notes[i];

        /* Hidden originals were replaced by their custom region copies. */
        if (n->voice != -3) {
            notes[count++] = (EsNativeFxNote){n->start, n->end, n->channel,
                                              n->value >= 1024 ? n->value - 1024 : -1};
        }
    }

    EsNativeFxChange *changes;
    size_t change_count;
    uint16_t shared;
    /* OoT channels keep the engine's default rate, 0x800 / 32. */
    bool ok = es_native_channel_fx(custom, notes, count, ES_NATIVE_OOT, false, 64, s->midi_fx,
                                   &changes, &change_count, &shared);
    free(notes);

    if (!ok || !change_count) {
        free(changes);
        return ok;
    }

    Control *merged = malloc((s->control_count + change_count) * sizeof(*merged));

    if (!merged) {
        free(changes);
        return false;
    }

    size_t a = 0, b = 0, used = 0;

    while (a < s->control_count || b < change_count) {
        if (b == change_count || (a < s->control_count && s->controls[a].tick <= changes[b].tick)) {
            merged[used++] = s->controls[a++];
        } else {
            const EsNativeFxChange *f = &changes[b++];
            merged[used++] = (Control){f->tick, (uint8_t)f->channel, kinds[f->kind], f->value};
        }
    }

    free(changes);
    free(s->controls);
    s->controls = merged;
    s->control_count = s->control_capacity = used;

    if (shared) {
        diagnose_item(s, 0, DM_TEXT_EXPORT_LIST_CONVERTED, "%s", DM_TEXT_EXPORT_ITEM_SHARED_FX);
    }

    return true;
}

/* Applies Cut / Cut by to the custom note copies from `first` on, which are in start order. */
static bool choke_custom(Song *s, size_t first) {
    if (!es_native_has_choke(s->request->custom) || first == s->note_count) {
        return true;
    }

    EsNativeChokeNote *notes = malloc((s->note_count - first) * sizeof(*notes));

    if (!notes) {
        return false;
    }

    for (size_t i = first; i < s->note_count; i++) {
        Note *n = &s->notes[i];
        notes[i - first] =
            (EsNativeChokeNote){n->start, &n->end, n->channel, (uint16_t)(n->value - 1024)};
    }

    bool ok = es_native_choke(s->request->custom, notes, s->note_count - first, s->host);
    free(notes);
    return ok;
}

static bool read_events(Song *s) {
    const OotRequest *q = s->request;
    int open[16][128], sustained[16][128];
    unsigned program[16] = {0}, rpn[16][2], range[16];
    bool pedal[16] = {false};
    memset(open, -1, sizeof(open));
    memset(sustained, 0, sizeof(sustained));

    for (unsigned c = 0; c < 16; c++) {
        rpn[c][0] = rpn[c][1] = 127;
        range[c] = 2;
    }

    for (uint64_t i = 0; i < q->event_count; i++) {
        const DmeEvent *e = &q->events[i];
        unsigned channel = e->status & MIDI_CHANNEL_MASK;

        /* Authored MIDI pan, reverb and modulation take precedence over preset values. */
        if ((e->status & MIDI_STATUS_MASK) == MIDI_CONTROL) {
            if (e->a == MIDI_CC_PAN && !(e->flags & DME_EVENT_DEFAULT_CONTROL)) {
                s->has_pan[channel] = true;
            } else if (e->a == MIDI_CC_REVERB) {
                s->midi_fx[ES_FX_REVERB] |= 1u << channel;
            } else if (e->a == MIDI_CC_MODULATION) {
                s->midi_fx[ES_FX_DEPTH] |= 1u << channel;
            }
        }

        if (!(i % 4096) && cancelled(s, (uint32_t)(i * 20 / (q->event_count + 1)))) {
            return false;
        }

        if (e->status == 0xff) {
            if (e->a &&
                grow((void **)&s->tempos, &s->tempo_capacity, s->tempo_count, sizeof(*s->tempos))) {
                s->tempos[s->tempo_count++] = (Tempo){e->tick, (unsigned)e->a};
            }

            continue;
        }

        if (e->status < MIDI_STATUS || e->status >= MIDI_SYSTEM) {
            if (e->status == 0xf0 && !s->warned_unsupported) {
                s->warned_unsupported = true;
                diagnose(s, DME_INFO,
                         "Messages OoT cannot play, such as system exclusive, are skipped.");
            }

            continue;
        }

        unsigned ch = e->status & MIDI_CHANNEL_MASK, type = e->status & MIDI_STATUS_MASK;
        unsigned a = e->a & 127, b = e->b & 127;
        bool drum = ch == MIDI_DRUM_CHANNEL;

        if (type == MIDI_NOTE_ON && b) {
            close_note(s, &open[ch][a], e->tick);
            sustained[ch][a] = 0;

            if (!grow((void **)&s->notes, &s->note_capacity, s->note_count, sizeof(*s->notes))) {
                return false;
            }

            unsigned original_program =
                q->source_patches ? q->source_patches[i].program : program[ch];

            if (original_program >= 128) {
                return false;
            }

            int value = drum ? q->drums[a] : q->instruments[ch][original_program];
            if (q->stock_values && q->stock_values[i] >= 0) {
                value = q->stock_values[i];
            }
            s->notes[s->note_count] = (Note){e->tick,
                                             e->tick + 1,
                                             (uint8_t)ch,
                                             (uint8_t)a,
                                             (uint8_t)b,
                                             drum,
                                             (uint16_t)(drum ? a : original_program),
                                             value,
                                             -1,
                                             i};
            open[ch][a] = (int)s->note_count++;
        } else if (type == MIDI_NOTE_OFF || type == MIDI_NOTE_ON) {
            if (pedal[ch] && open[ch][a] >= 0) {
                sustained[ch][a] = 1;
            } else {
                close_note(s, &open[ch][a], e->tick);
            }
        } else if (type == MIDI_PROGRAM) {
            program[ch] = a;
        } else if (type == MIDI_PITCH_BEND) {
            int bend = (int)((e->a & 127) | (e->b & 127) << 7) - 8192;
            int cents = (int)lround(bend / 8192.0 * range[ch] * 100.0);

            if (!add_control(s, e->tick, ch, CTRL_BEND, cents)) {
                return false;
            }
        } else if (type == MIDI_CONTROL) {
            int kind = -1, value = (int)b;

            if (a == MIDI_CC_VOLUME) {
                kind = CTRL_VOL;
            } else if (a == MIDI_CC_EXPRESSION) {
                kind = CTRL_EXPR;
            } else if (a == MIDI_CC_PAN) {
                kind = CTRL_PAN;
            } else if (a == MIDI_CC_REVERB) {
                kind = CTRL_REVERB;
            } else if (a == MIDI_CC_MODULATION) {
                kind = CTRL_VIB;
                value = (int)lround(b * 8.0 / 127.0);
            } else if (a == MIDI_CC_SUSTAIN) {
                pedal[ch] = b >= 64;

                if (!pedal[ch]) {
                    for (unsigned k = 0; k < 128; k++) {
                        if (sustained[ch][k]) {
                            sustained[ch][k] = 0;
                            close_note(s, &open[ch][k], e->tick);
                        }
                    }
                }
            } else if (a == MIDI_CC_RPN_MSB) {
                rpn[ch][0] = b;
            } else if (a == MIDI_CC_RPN_LSB) {
                rpn[ch][1] = b;
            } else if (a == MIDI_CC_DATA && rpn[ch][0] == 0 && rpn[ch][1] == 0) {
                range[ch] = b ? b : 1;
            }

            if (kind >= 0 && !add_control(s, e->tick, ch, (unsigned)kind, value)) {
                return false;
            }
        }
    }

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned k = 0; k < 128; k++) {
            close_note(s, &open[ch][k], q->duration);
        }
    }

    if (q->custom && q->custom->notes) {
        size_t original = s->note_count;

        for (size_t i = 0; i < original; i++) {
            Note n = s->notes[i];
            const EsNativeNote *selection = &q->custom->notes[n.source_event];

            if (!selection->custom) {
                continue;
            }

            s->notes[i].voice = -3;

            for (unsigned layer = 0; layer < selection->count; layer++) {
                if (!grow((void **)&s->notes, &s->note_capacity, s->note_count,
                          sizeof(*s->notes))) {
                    return false;
                }

                n.value = 1024 + q->custom->layers[selection->first + layer];
                s->custom_pan[n.channel] = true;
                s->preset_pan[n.channel] |=
                    !s->has_pan[n.channel] &&
                    (n.drum || q->custom->regions[n.value - 1024].params.pan != 0);
                s->notes[s->note_count++] = n;
            }
        }

        if (!choke_custom(s, original) || !custom_effects(s)) {
            return false;
        }
    }

    return true;
}

/* ---- Timing and sections ---- */

static void choose_ticks(Song *s) {
    unsigned fastest = 120;

    for (size_t i = 0; i < s->tempo_count; i++) {
        unsigned bpm = (unsigned)(60000000.0 / s->tempos[i].micros + 0.5);
        fastest = bpm > fastest ? bpm : fastest;
    }

    s->tpb = 48;

    while (s->tpb > 6 && fastest * s->tpb / 48 > 255) {
        s->tpb /= 2;
    }

    if (s->tpb < 48) {
        diagnose(s, DME_INFO,
                 "Tempos above 255 BPM play at %u ticks per beat, so very short notes are rounded.",
                 s->tpb);
    }

    if (fastest * s->tpb / 48 > 255) {
        diagnose_item(s, 0, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_MAX_TEMPO,
                      255 * 48 / s->tpb);
    }
}

static unsigned tempo_bpm(const Song *s, unsigned microseconds) {
    double bpm = 60000000.0 / microseconds * s->tpb / 48.0;
    long value = lround(bpm);
    return (unsigned)(value < 1 ? 1 : value > 255 ? 255 : value);
}

static int compare_tempos(const void *left, const void *right) {
    const Tempo *a = left, *b = right;
    return a->tick < b->tick ? -1 : a->tick > b->tick;
}

static bool plan_sections(Song *s) {
    const OotRequest *q = s->request;
    uint64_t length = to_seq(s, q->duration);

    if (length < 1) {
        length = 1;
    }

    s->loops = q->loop != OOT_LOOP_NONE;

    if (q->loop == OOT_LOOP_REGION) {
        uint64_t start = to_seq(s, q->loop_start);
        uint64_t end = q->loop_end ? to_seq(s, q->loop_end) : length;

        if (end > length) {
            end = length;
        }

        if (start >= end) {
            snprintf(s->error, s->capacity, "The loop start must come before the loop end.");
            return false;
        }

        if (start) {
            s->sections[s->section_count][0] = 0;
            s->sections[s->section_count++][1] = start;
        }

        s->sections[s->section_count][0] = start;
        s->sections[s->section_count++][1] = end;
        return true;
    }

    s->sections[0][0] = 0;
    s->sections[0][1] = length;
    s->section_count = 1;
    return true;
}

/* Converts notes to sequence ticks and splits them at section boundaries. */
static bool split_notes(Song *s) {
    size_t count = s->note_count;
    uint64_t last_end = s->sections[s->section_count - 1][1];
    size_t dropped = 0;

    for (size_t i = 0; i < count; i++) {
        Note n = s->notes[i];
        n.start = to_seq(s, n.start);
        n.end = to_seq(s, n.end);

        if (n.end <= n.start) {
            n.end = n.start + 1;
        }

        /* Silent drums and notes after the end take no voice (-3). */
        if (n.drum && n.value == OOT_SILENT) {
            s->notes[i].voice = -3;
            continue;
        }

        if (n.start >= last_end) {
            s->notes[i].voice = -3;
            dropped++;
            continue;
        }

        if (n.end > last_end) {
            n.end = last_end;
        }

        s->notes[i] = n;

        for (unsigned sec = 0; sec + 1 < s->section_count; sec++) {
            uint64_t boundary = s->sections[sec][1];

            if (s->notes[i].start < boundary && s->notes[i].end > boundary) {
                if (!grow((void **)&s->notes, &s->note_capacity, s->note_count,
                          sizeof(*s->notes))) {
                    return false;
                }

                Note tail = s->notes[i];
                tail.start = boundary;
                s->notes[i].end = boundary;
                s->notes[s->note_count++] = tail;
            }
        }
    }

    if (dropped && s->request->loop == OOT_LOOP_REGION) {
        diagnose(s, DME_INFO, "%zu notes after the loop end are not exported.", dropped);
    }

    for (size_t i = 0; i < s->control_count; i++) {
        s->controls[i].tick = to_seq(s, s->controls[i].tick);
    }

    for (size_t i = 0; i < s->tempo_count; i++) {
        s->tempos[i].tick = to_seq(s, s->tempos[i].tick);
    }

    return true;
}

static int compare_notes(const void *left, const void *right) {
    const Note *a = left, *b = right;

    if (a->channel != b->channel) {
        return a->channel - b->channel;
    }

    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }

    return a->key - b->key;
}

static unsigned section_of(const Song *s, uint64_t tick) {
    for (unsigned sec = 0; sec < s->section_count; sec++) {
        if (tick < s->sections[sec][1]) {
            return sec;
        }
    }

    return s->section_count - 1;
}

/* ---- Voices ---- */

static bool assign_voices(Song *s) {
    qsort(s->notes, s->note_count, sizeof(*s->notes), compare_notes);
    unsigned groups[16] = {0};
    bool used[16] = {false};

    for (size_t i = 0; i < s->note_count; i++) {
        if (s->notes[i].voice >= -1) {
            used[s->notes[i].channel] = true;
        }
    }

    for (size_t i = 0; i < s->control_count; i++) {
        used[s->controls[i].channel] = true;
    }

    uint64_t ends[64];

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned sec = 0; sec < s->section_count; sec++) {
            memset(ends, 0, sizeof(ends));

            for (size_t i = 0; i < s->note_count; i++) {
                Note *n = &s->notes[i];

                if (n->voice < -1 || n->channel != ch || section_of(s, n->start) != sec) {
                    continue;
                }

                int voice = -1;

                for (int v = 0; v < 64 && voice < 0; v++) {
                    if (ends[v] <= n->start) {
                        voice = v;
                    }
                }

                if (voice < 0) {
                    n->voice = -2;
                    continue;
                }

                ends[voice] = n->end;
                n->voice = voice;

                if ((unsigned)voice / 4 + 1 > groups[ch]) {
                    groups[ch] = (unsigned)voice / 4 + 1;
                }
            }
        }
    }

    for (unsigned out = 0; out < 16; out++) {
        s->out_of[out] = used[out] ? (int)out : -1;
        s->drum_out[out] = out == MIDI_DRUM_CHANNEL;
    }

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned g = 0; g < 16; g++) {
            s->group_channel[ch][g] = g == 0 && used[ch] ? (int)ch : -1;
        }
    }

    /* Groups past the first need free channels that copy the source channel's controllers. */
    unsigned moved = 0;

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned g = 1; g < groups[ch]; g++) {
            int free_channel = -1;

            for (unsigned out = 0; out < 16 && free_channel < 0; out++) {
                if (s->out_of[out] < 0) {
                    free_channel = (int)out;
                }
            }

            if (free_channel < 0) {
                break;
            }

            s->out_of[free_channel] = (int)ch;
            s->drum_out[free_channel] = ch == MIDI_DRUM_CHANNEL;
            s->group_channel[ch][g] = free_channel;
            moved++;
        }
    }

    for (size_t i = 0; i < s->note_count; i++) {
        Note *n = &s->notes[i];

        if (n->voice >= 0 && s->group_channel[n->channel][n->voice / 4] < 0) {
            n->voice = -2;
        }
    }

    if (moved) {
        diagnose(s, DME_INFO,
                 "Channels with more than 4 overlapping notes continue on %u free channel%s.",
                 moved, moved == 1 ? "" : "s");
    }

    return true;
}

/* ---- Instruments ---- */

static int instrument_program(Song *s, int value, bool *ok) {
    int wave = oot_synth_wave(value);

    if (wave >= 0) {
        return wave;
    }

    if (!(value >= 1024 && s->request->custom &&
          (size_t)(value - 1024) < s->request->custom->count) &&
        !oot_instrument_valid(value)) {
        *ok = false;
        return 0;
    }

    if (s->request->font != OOT_CUSTOM_FONT) {
        int program = oot_font_program(s->request->font, value);

        if (program < 0) {
            *ok = false;
            return 0;
        }

        return program;
    }

    if (s->programs[value] < 0) {
        if (s->instrument_count >= 126) {
            if (value >= 1024 && s->request->capacity) {
                s->request->capacity[value - 1024] = true;
                return 0;
            }

            *ok = false;
            return 0;
        }

        s->programs[value] = (int)s->instrument_count;
        s->instrument_order[s->instrument_count++] = value;
    }

    return s->programs[value];
}

static void collect_instruments(Song *s) {
    for (int v = 0; v < 1024 + ES_NATIVE_REGIONS; v++) {
        s->programs[v] = -1;
    }

    for (unsigned k = 0; k < 128; k++) {
        s->drum_for_key[k] = -1;
        s->drum_original[k] = -1;
    }

    for (size_t i = 0; i < s->note_count; i++) {
        const Note *n = &s->notes[i];

        if (n->drum && n->voice >= 0 && n->value < 1024 && n->key >= 21 && n->key <= 84 &&
            n->value != OOT_SILENT && oot_drum_valid(n->value)) {
            s->drum_for_key[n->key] = n->value;
            s->drum_original[n->key] = n->key;
        }
    }

    bool bad = false;

    for (size_t i = 0; i < s->note_count; i++) {
        Note *n = &s->notes[i];

        if (n->voice < 0) {
            continue;
        }

        if (n->drum) {
            if (n->value >= 1024) {
                unsigned key;

                for (key = 21; key <= 84; key++) {
                    if (s->drum_for_key[key] == n->value && s->drum_original[key] == n->key) {
                        break;
                    }
                }

                if (key > 84) {
                    for (key = 21; key <= 84 && s->drum_for_key[key] >= 0; key++) {
                    }
                }

                if (key > 84) {
                    if (s->request->capacity) {
                        s->request->capacity[n->value - 1024] = true;
                        n->voice = -3;
                        continue;
                    }

                    snprintf(s->error, s->capacity, DM_TEXT_NATIVE_OOT_PERCUSSION_CAPACITY);
                    bad = true;
                    n->voice = -3;
                    continue;
                }

                s->drum_for_key[key] = n->value;
                s->drum_original[key] = n->key;
                n->key = (uint8_t)key;
                continue;
            }

            if (n->value == OOT_SILENT || !oot_drum_valid(n->value) ||
                (s->request->font != OOT_CUSTOM_FONT &&
                 oot_font_drum_hi(s->request->font, n->value) < 0)) {
                n->voice = -3;
                continue;
            }

            if (s->request->font == OOT_CUSTOM_FONT) {
                if (n->key < 21 || n->key > 84) {
                    if (!s->warned_drum_key) {
                        s->warned_drum_key = true;
                        diagnose_item(s, 0, DM_TEXT_EXPORT_LIST_OMITTED, "%s",
                                      DM_TEXT_EXPORT_ITEM_PERCUSSION_KEYS);
                    }

                    n->voice = -3;
                    continue;
                }

                s->drum_for_key[n->key] = n->value;
            }

            continue;
        }

        bool ok = true;
        instrument_program(s, n->value, &ok);

        if (!ok && s->request->capacity) {
            for (unsigned j = s->instrument_count; j > 0; j--) {
                int value = s->instrument_order[j - 1];

                if (value >= 1024) {
                    s->request->capacity[value - 1024] = true;
                    ok = true;
                    break;
                }
            }
        }

        bad |= !ok;
    }

    if (bad) {
        diagnose_item(s, 0, DM_TEXT_EXPORT_LIST_CONVERTED, "%s", DM_TEXT_EXPORT_ITEM_MISSING_FONT);
    }
}

/* ---- Range analysis ---- */

static bool add_issue(Song *s, const Note *n, double rate) {
    OotResult *r = s->result;

    for (size_t i = 0; i < r->range_count; i++) {
        OotRangeIssue *issue = &r->ranges[i];

        if (issue->channel == n->channel && issue->program == n->program &&
            issue->percussion == n->drum && issue->value == n->value &&
            s->request->events[issue->source_event].port ==
                s->request->events[n->source_event].port &&
            (!s->request->source_patches || s->request->source_patches[issue->source_event].bank ==
                                                s->request->source_patches[n->source_event].bank)) {
            issue->low = n->key < issue->low ? n->key : issue->low;
            issue->high = n->key > issue->high ? n->key : issue->high;
            issue->rate = rate > issue->rate ? rate : issue->rate;
            issue->count++;
            issue->peak_factor = fmax(issue->peak_factor, n->peak_factor);
            return true;
        }
    }

    if (!grow((void **)&r->ranges, &r->range_capacity, r->range_count, sizeof(*r->ranges))) {
        return false;
    }

    r->ranges[r->range_count++] =
        (OotRangeIssue){n->channel, n->program, n->key,   n->key,          1,
                        n->drum,    rate,       n->value, n->source_event, n->peak_factor};
    return true;
}

static double note_rate(const OotRequest *q, const Note *n) {
    if (n->value >= 1024 && q->custom) {
        const EsNativeRegion *r = &q->custom->regions[n->value - 1024];
        return r->tuning * pow(2, ((double)(n->drum ? n->program : n->key) - 60) / 12);
    }

    return n->drum ? oot_drum_rate(q->font, n->value, n->key)
                   : oot_instrument_rate(n->value, n->key);
}

static int bend_amount(int, bool *);

typedef struct {
    uint64_t tick;
    double factor;
} RateChange;

static size_t rate_upper(const RateChange *changes, size_t count, uint64_t tick) {
    size_t low = 0;
    while (low < count) {
        size_t mid = low + (count - low) / 2;
        if (changes[mid].tick <= tick) {
            low = mid + 1;
        } else {
            count = mid;
        }
    }

    return low;
}

static bool note_rate_factors(Song *s) {
    for (unsigned channel = 0; channel < 16; channel++) {
        RateChange *changes = malloc((s->control_count + 1) * sizeof(*changes));
        if (!changes) {
            return false;
        }

        size_t count = 1;
        changes[0] = (RateChange){0, 1};
        int bend = 0, vibrato = 0;
        for (size_t i = 0; i < s->control_count; i++) {
            if (!(i % 4096) && cancelled(s, 20)) {
                free(changes);
                return false;
            }
            const Control *c = &s->controls[i];
            if (c->channel != channel || (c->kind != CTRL_BEND && c->kind != CTRL_VIB)) {
                continue;
            }

            if (c->kind == CTRL_BEND) {
                bend = c->value;
            } else {
                vibrato = c->value;
            }

            bool fine;
            int amount = bend_amount(bend, &fine);
            double factor =
                pow(2, amount * (fine ? 2.0 : 12.0) / (128 * 12)) * (1 + vibrato / 512.0);
            changes[count++] = (RateChange){to_seq(s, c->tick), factor};
        }

        size_t base = 1;
        while (base < count) {
            base *= 2;
        }

        double *tree = calloc(base * 2, sizeof(*tree));
        if (!tree) {
            free(changes);
            return false;
        }

        for (size_t i = 0; i < count; i++) {
            tree[base + i] = changes[i].factor;
        }

        for (size_t i = base; i > 1; i--) {
            tree[i - 1] = fmax(tree[(i - 1) * 2], tree[(i - 1) * 2 + 1]);
        }

        for (size_t i = 0; i < s->note_count; i++) {
            Note *n = &s->notes[i];
            if (n->channel != channel || n->voice < 0) {
                continue;
            }

            size_t left = base + rate_upper(changes, count, n->start) - 1;
            size_t right = base + rate_upper(changes, count, n->end - 1);
            double peak = 0;
            for (; left < right; left /= 2, right /= 2) {
                if (left & 1) {
                    peak = fmax(peak, tree[left++]);
                }

                if (right & 1) {
                    peak = fmax(peak, tree[--right]);
                }
            }

            n->peak_factor = peak;
        }

        free(changes);
        free(tree);
    }

    return true;
}

static bool analyze_ranges(Song *s) {
    if (!note_rate_factors(s)) {
        return false;
    }
    for (size_t i = 0; i < s->note_count; i++) {
        const Note *n = &s->notes[i];

        if (n->voice < 0) {
            continue;
        }

        double rate = note_rate(s->request, n) * n->peak_factor;

        if (n->value >= 1024 && rate > OOT_MAX_RATE && s->request->overspeed) {
            double *over = &s->request->overspeed[n->value - 1024];
            *over = fmax(*over, rate / OOT_MAX_RATE);
            continue;
        }

        if (n->value >= 1024 && rate > OOT_MAX_RATE) {
            snprintf(s->error, s->capacity, DM_TEXT_NATIVE_OOT_RATE,
                     s->request->custom->regions[n->value - 1024].name, rate);
            return false;
        }

        if (rate > OOT_MAX_RATE && !add_issue(s, n, rate)) {
            return false;
        }
    }

    return true;
}

bool oot_range_supported(const OotRequest *request, const OotRangeIssue *issue, int value) {
    return oot_font_range_supported(request->font, issue, value);
}

bool oot_font_range_supported(int font, const OotRangeIssue *issue, int value) {
    for (unsigned key = issue->low; key <= issue->high; key++) {
        double rate = issue->percussion ? oot_drum_rate(font, value, key)
                                        : oot_instrument_rate(value, key);

        if (rate * fmax(1, issue->peak_factor) > OOT_MAX_RATE) {
            return false;
        }
    }

    return true;
}

/* ---- Layer scripts ---- */

static bool add_command(Song *s, uint64_t at, uint32_t size, const char *format, ...) {
    if (!grow((void **)&s->commands, &s->command_capacity, s->command_count,
              sizeof(*s->commands))) {
        return false;
    }

    Command *c = &s->commands[s->command_count++];
    c->at = at;
    c->size = size;
    va_list args;
    va_start(args, format);
    vsnprintf(c->text, sizeof(c->text), format, args);
    va_end(args);
    return true;
}

static bool add_rest(Song *s, uint64_t at, uint64_t ticks) {
    while (ticks) {
        uint64_t step = ticks > MAX_DELAY ? MAX_DELAY : ticks;

        if (!add_command(s, at, 1 + var_size(step), "ldelay %llu", (unsigned long long)step)) {
            return false;
        }

        at += step;
        ticks -= step;
    }

    return true;
}

/* Smallest gate whose silent part equals `rest` ticks of `delay`, or -1. */
static int gate_for(uint64_t delay, uint64_t rest) {
    for (uint64_t g = rest * 256 / delay; g < 256; g++) {
        uint64_t silent = (g * delay) >> 8;

        if (silent == rest && g) {
            return (int)g;
        }

        if (silent > rest) {
            break;
        }
    }

    return -1;
}

/* Builds one layer's commands; returns the index range in s->commands through *first. */
static bool build_layer(Song *s, const Note **voice, size_t count, uint64_t start, size_t *first) {
    *first = s->command_count;
    uint64_t cursor = start;
    int transpose = 0;
    int instrument = -1;
    unsigned pan = 64;
    uint64_t pan_bar = UINT64_MAX;
    uint64_t instrument_bar = UINT64_MAX;
    /* Short notes: the layer's velocity and gate; shared bars match only with the same entry
       values (see chunk_layer). */
    int velocity = -1, gate_state = -1;
    const OotRequest *q = s->request;

    for (size_t i = 0; i < count; i++) {
        const Note *n = voice[i];

        if (n->start > cursor && !add_rest(s, cursor, n->start - cursor)) {
            return false;
        }

        cursor = n->start;
        uint64_t bar = (n->start - start) / (4ull * s->tpb);

        if (!n->drum && s->preset_pan[n->channel]) {
            unsigned wanted =
                n->value >= 1024 ? es_native_pan(&q->custom->regions[n->value - 1024]) : 64;

            if ((wanted != pan || bar != pan_bar) &&
                !add_command(s, cursor, 2, "notepan %u", wanted)) {
                return false;
            }

            pan = wanted;
            pan_bar = bar;
        }

        /* Each bar establishes its instrument so repeated chunks have the same entry state. */
        if (!n->drum && (n->value != instrument || bar != instrument_bar)) {
            bool valid = true;
            int program = instrument_program(s, n->value, &valid);

            if (!valid || !add_command(s, cursor, 2, "instr %d", program)) {
                return false;
            }

            instrument = n->value;
            instrument_bar = bar;
        }

        int key = n->key;

        if (n->drum) {
            key =
                q->font == OOT_CUSTOM_FONT ? n->key : (int)oot_drum_key(q->font, n->value, n->key);
        } else {
            while (key < 21) {
                key += 12;

                if (!s->warned_low) {
                    s->warned_low = true;
                    diagnose(s, DME_INFO, "Notes below A0 play an octave higher.");
                }
            }
        }

        int semitone = key - 21;

        if (semitone - transpose < 0 || semitone - transpose > 63) {
            int wanted = semitone <= 63 ? 0 : (semitone - 63 + 11) / 12 * 12;

            if (!add_command(s, cursor, 2, "transpose %d", wanted)) {
                return false;
            }

            transpose = wanted;
        }

        uint64_t length = n->end - n->start;

        if (length > MAX_DELAY) {
            length = MAX_DELAY;

            if (!s->warned_long) {
                s->warned_long = true;
                diagnose(s, DME_INFO, "Notes longer than %u ticks are shortened.", MAX_DELAY);
            }
        }

        uint64_t next = i + 1 < count ? voice[i + 1]->start : n->start + length;
        uint64_t rest = next > n->start + length ? next - n->start - length : 0;
        uint64_t delay = length + rest;
        int gate = rest && delay <= MAX_DELAY ? gate_for(delay, rest) : -1;
        int pitch = semitone - transpose;

        if (s->short_notes) {
            /* A large notedv plays with gate 0; short notes keep the layer's gate. */
            int wanted = gate > 0 ? gate : 0;
            uint64_t step = gate > 0 ? delay : length;

            if (n->velocity != velocity && !add_command(s, cursor, 2, "shortvel %u", n->velocity)) {
                return false;
            }

            if (wanted != gate_state && !add_command(s, cursor, 2, "shortgate %d", wanted)) {
                return false;
            }

            velocity = n->velocity;
            gate_state = wanted;

            if (!add_command(s, cursor, 1 + var_size(step), "shortdvg %d, %llu", pitch,
                             (unsigned long long)step)) {
                return false;
            }

            cursor += step;
        } else if (gate > 0) {
            if (!add_command(s, cursor, 3 + var_size(delay), "notedvg %d, %llu, %u, %d", pitch,
                             (unsigned long long)delay, n->velocity, gate)) {
                return false;
            }

            cursor += delay;
        } else {
            if (!add_command(s, cursor, 2 + var_size(length), "notedv %d, %llu, %u", pitch,
                             (unsigned long long)length, n->velocity)) {
                return false;
            }

            cursor += length;
        }
    }

    return true;
}

static uint64_t hash_text(uint64_t hash, const char *text) {
    for (; *text; text++) {
        hash = (hash ^ (unsigned char)*text) * 1099511628211ULL;
    }

    return (hash ^ 0xff) * 1099511628211ULL;
}

static bool same_chunk(const Song *s, const Chunk *a, const Chunk *b) {
    if (a->hash != b->hash || a->count != b->count || a->entry != b->entry ||
        a->velocity != b->velocity || a->gate != b->gate) {
        return false;
    }

    for (size_t i = 0; i < a->count; i++) {
        if (strcmp(s->commands[a->first + i].text, s->commands[b->first + i].text)) {
            return false;
        }
    }

    return true;
}

/* Cuts a layer's commands at bar lines. Records the chunks of one layer as [*begin, end). */
static bool chunk_layer(Song *s, size_t first, uint64_t start, size_t *begin) {
    *begin = s->chunk_count;
    uint64_t bar = 4ull * s->tpb;
    int transpose = 0, velocity = -1, gate = -1;
    size_t i = first;

    while (i < s->command_count) {
        uint64_t index = (s->commands[i].at - start) / bar;

        if (!grow((void **)&s->chunks, &s->chunk_capacity, s->chunk_count, sizeof(*s->chunks))) {
            return false;
        }

        Chunk c = {14695981039346656037ULL, i, 0, transpose, velocity, gate, -1};
        c.hash = hash_text(c.hash, transpose ? "t" : "0");

        while (i < s->command_count && (s->commands[i].at - start) / bar == index) {
            c.hash = hash_text(c.hash, s->commands[i].text);

            if (!strncmp(s->commands[i].text, "transpose ", 10)) {
                transpose = atoi(s->commands[i].text + 10);
            } else if (!strncmp(s->commands[i].text, "shortvel ", 9)) {
                velocity = atoi(s->commands[i].text + 9);
            } else if (!strncmp(s->commands[i].text, "shortgate ", 10)) {
                gate = atoi(s->commands[i].text + 10);
            }

            c.count++;
            i++;
        }

        s->chunks[s->chunk_count++] = c;
    }

    return true;
}

static uint32_t chunk_size(const Song *s, const Chunk *c) {
    uint32_t size = 0;

    for (size_t i = 0; i < c->count; i++) {
        size += s->commands[c->first + i].size;
    }

    return size;
}

/* Repeated bars become subroutines when that saves bytes. */
static void share_chunks(Song *s) {
    for (size_t i = 0; i < s->chunk_count; i++) {
        Chunk *c = &s->chunks[i];

        if (c->sub >= 0) {
            continue;
        }

        size_t copies = 1;

        for (size_t j = i + 1; j < s->chunk_count; j++) {
            copies += s->chunks[j].sub < 0 && same_chunk(s, c, &s->chunks[j]);
        }

        uint32_t size = chunk_size(s, c);

        if (copies < 2 || size + 1 + 3 * copies >= size * copies) {
            continue;
        }

        int sub = (int)s->sub_count++;
        c->sub = sub;

        for (size_t j = i + 1; j < s->chunk_count; j++) {
            if (s->chunks[j].sub < 0 && same_chunk(s, c, &s->chunks[j])) {
                s->chunks[j].sub = sub;
            }
        }
    }
}

/* ---- Writing ---- */

typedef struct {
    size_t chunk_begin, chunk_end;
    unsigned channel, section, layer;
} LayerRef;

static void emit(Song *s, OotText *t, uint32_t size, const char *format, ...) {
    char line[160];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if (s->capture) {
        Emitting *e = s->capture;

        if (strlen(line) >= sizeof(e->items->text) ||
            !grow((void **)&e->items, &e->capacity, e->count, sizeof(*e->items))) {
            e->failed = true;
            return;
        }

        snprintf(e->items[e->count].text, sizeof(e->items->text), "%s", line);
        e->items[e->count++].size = (uint8_t)size;
        return;
    }

    oot_text_add(t, "    %s\n", line);
    s->bytes += size;
}

static void emit_delay(Song *s, OotText *t, uint64_t ticks) {
    while (ticks) {
        uint64_t step = ticks > MAX_DELAY ? MAX_DELAY : ticks;
        emit(s, t, 1 + var_size(step), "delay %llu", (unsigned long long)step);
        ticks -= step;
    }
}

typedef struct {
    int values[CTRL_KINDS];
} ChannelState;

static ChannelState state_at(const Song *s, unsigned source, uint64_t tick) {
    ChannelState state = {{-1, 127, 127, 64, 0, 0, 0, -1}};

    for (size_t i = 0; i < s->control_count; i++) {
        const Control *c = &s->controls[i];

        if (c->channel == source && c->tick <= tick) {
            state.values[c->kind] = c->value;
        }
    }

    return state;
}

static int bend_amount(int cents, bool *fine) {
    *fine = abs(cents) <= 200;
    double amount = *fine ? cents / 200.0 * 128.0 : cents / 1200.0 * 128.0;
    long value = lround(amount);
    return (int)(value < -128 ? -128 : value > 127 ? 127 : value);
}

static void emit_control(Song *s, OotText *t, unsigned kind, int value, bool drum) {
    bool ok = true, fine;

    switch (kind) {
    case CTRL_INSTR:
        if (!drum && value >= 0) {
            int program = instrument_program(s, value, &ok);
            emit(s, t, 2, "instr %d", ok ? program : 0);
        }

        break;
    case CTRL_VOL:
        emit(s, t, 2, "vol %d", value);
        break;
    case CTRL_EXPR:
        emit(s, t, 2, "volexp %d", value);
        break;
    case CTRL_PAN:
        emit(s, t, 2, "pan %d", value);
        break;
    case CTRL_REVERB:
        emit(s, t, 2, "reverb %d", value);
        break;
    case CTRL_BEND: {
        int amount = bend_amount(value, &fine);
        emit(s, t, 2, "%s %d", fine ? "bendfine" : "bend", amount);
        break;
    }
    case CTRL_VIB:
        emit(s, t, 2, "vibdepth %d", value);
        break;
    case CTRL_VIBFREQ:
        emit(s, t, 2, "vibfreq %d", value);
        break;
    default:
        break;
    }
}

static int first_note_value(const Song *s, unsigned out, unsigned sec) {
    for (size_t i = 0; i < s->note_count; i++) {
        const Note *n = &s->notes[i];

        if (n->voice >= 0 && s->group_channel[n->channel][n->voice / 4] == (int)out &&
            section_of(s, n->start) == sec) {
            return n->value;
        }
    }

    return -1;
}

static void write_head(Song *s, OotText *t, unsigned out, unsigned sec, const LayerRef *layers,
                       size_t layer_count) {
    unsigned source = (unsigned)s->out_of[out];
    bool drum = s->drum_out[out];
    ChannelState state = state_at(s, source, s->sections[sec][0]);

    if (state.values[CTRL_INSTR] < 0) {
        state.values[CTRL_INSTR] = first_note_value(s, out, sec);
    }

    oot_text_add(t, "\n.channel CHAN_%u_%u\n", out, sec);
    emit(s, t, 1, s->short_notes ? "short" : "noshort");

    if (drum) {
        emit(s, t, 2, "instr FONTANY_INSTR_DRUM");
        emit(s, t, 2, "panweight %u", s->custom_pan[source] && s->has_pan[source] ? 128 : 0);
    } else if (s->preset_pan[source]) {
        emit(s, t, 2, "panweight 0");
    }

    for (unsigned kind = 0; kind < CTRL_KINDS; kind++) {
        int value = state.values[kind];

        if (((kind == CTRL_BEND || kind == CTRL_VIB || kind == CTRL_REVERB) && !value) ||
            (kind == CTRL_VIBFREQ && value < 0)) {
            continue;
        }

        emit_control(s, t, kind, value, drum);
    }

    for (size_t i = 0; i < layer_count; i++) {
        if (layers[i].channel == out && layers[i].section == sec) {
            emit(s, t, 3, "ldlayer %u, LAYER_%u_%u_%u", layers[i].layer, out, sec, layers[i].layer);
        }
    }
}

/* The channel's controller changes for the section, ending with its final delay and `end`. */
static void write_stream(Song *s, OotText *t, unsigned out, unsigned sec) {
    unsigned source = (unsigned)s->out_of[out];
    bool drum = s->drum_out[out];
    uint64_t start = s->sections[sec][0], end = s->sections[sec][1];
    ChannelState state = state_at(s, source, start);

    if (state.values[CTRL_INSTR] < 0) {
        state.values[CTRL_INSTR] = first_note_value(s, out, sec);
    }

    uint64_t cursor = start;
    int last[CTRL_KINDS];
    memcpy(last, state.values, sizeof(last));

    for (size_t i = 0; i < s->control_count; i++) {
        const Control *c = &s->controls[i];

        if (c->channel != source || c->tick < start || c->tick >= end) {
            continue;
        }

        /* Only the last change of a kind at one tick matters. */
        bool superseded = false;

        for (size_t j = i + 1; j < s->control_count && s->controls[j].tick == c->tick; j++) {
            superseded |= s->controls[j].channel == c->channel && s->controls[j].kind == c->kind;
        }

        int value = c->value;

        if (c->kind == CTRL_BEND) {
            bool fine;
            int amount = bend_amount(value, &fine), previous = bend_amount(last[c->kind], &fine);

            if (amount == previous && (abs(value) <= 200) == (abs(last[c->kind]) <= 200)) {
                last[c->kind] = value;
                continue;
            }
        }

        if (superseded || value == last[c->kind]) {
            continue;
        }

        emit_delay(s, t, c->tick - cursor);
        cursor = c->tick;
        emit_control(s, t, c->kind, value, drum);
        last[c->kind] = value;
    }

    emit_delay(s, t, end - cursor);
    emit(s, t, 1, "end");
}

static void write_layer(Song *s, OotText *t, const LayerRef *ref) {
    oot_text_add(t, "\n.layer LAYER_%u_%u_%u\n", ref->channel, ref->section, ref->layer);

    for (size_t c = ref->chunk_begin; c < ref->chunk_end; c++) {
        const Chunk *chunk = &s->chunks[c];

        if (chunk->sub >= 0) {
            emit(s, t, 3, "call LSUB_%d", chunk->sub);
            continue;
        }

        for (size_t i = 0; i < chunk->count; i++) {
            const Command *command = &s->commands[chunk->first + i];
            emit(s, t, command->size, "%s", command->text);
        }
    }

    emit(s, t, 1, "end");
}

static void write_subroutines(Song *s, OotText *t) {
    for (unsigned sub = 0; sub < s->sub_count; sub++) {
        for (size_t c = 0; c < s->chunk_count; c++) {
            const Chunk *chunk = &s->chunks[c];

            if (chunk->sub != (int)sub) {
                continue;
            }

            oot_text_add(t, "\n.layer LSUB_%u\n", sub);

            for (size_t i = 0; i < chunk->count; i++) {
                const Command *command = &s->commands[chunk->first + i];
                emit(s, t, command->size, "%s", command->text);
            }

            emit(s, t, 1, "end");
            break;
        }
    }
}

static void write_sequence_script(Song *s, OotText *t, unsigned mask) {
    const OotRequest *q = s->request;
    oot_text_add(t, "\n.sequence SEQ_START\n");
    emit(s, t, 2, "mutebhv 0x20");
    emit(s, t, 2, "mutescale 50");
    emit(s, t, 3, "initchan 0x%04X", mask);
    emit(s, t, 2, "vol %u", q->volume);

    for (unsigned sec = 0; sec < s->section_count; sec++) {
        uint64_t start = s->sections[sec][0], end = s->sections[sec][1];
        bool repeats = s->loops && sec + 1 == s->section_count;

        if (repeats) {
            oot_text_add(t, "SEQ_LOOP:\n");
        }

        unsigned bpm = 120 * s->tpb / 48;

        for (size_t i = 0; i < s->tempo_count && s->tempos[i].tick <= start; i++) {
            bpm = tempo_bpm(s, s->tempos[i].micros);
        }

        emit(s, t, 2, "tempo %u", bpm);

        for (unsigned out = 0; out < 16; out++) {
            if (mask & (1u << out)) {
                emit(s, t, 3, "ldchan %u, CHAN_%u_%u", out, out, sec);
            }
        }

        uint64_t cursor = start;

        for (size_t i = 0; i < s->tempo_count; i++) {
            const Tempo *tempo = &s->tempos[i];

            if (tempo->tick <= start || tempo->tick >= end ||
                (i + 1 < s->tempo_count && s->tempos[i + 1].tick == tempo->tick)) {
                continue;
            }

            unsigned next = tempo_bpm(s, tempo->micros);

            if (next == bpm) {
                continue;
            }

            emit_delay(s, t, tempo->tick - cursor);
            cursor = tempo->tick;
            emit(s, t, 2, "tempo %u", next);
            bpm = next;
        }

        emit_delay(s, t, end - cursor);

        if (repeats) {
            emit(s, t, 3, "jump SEQ_LOOP");
        }
    }

    if (!s->loops) {
        emit(s, t, 3, "freechan 0x%04X", mask);
        emit(s, t, 1, "end");
    }
}

static int compare_note_pointers(const void *left, const void *right) {
    const Note *a = *(const Note *const *)left, *b = *(const Note *const *)right;
    return a->start < b->start ? -1 : a->start > b->start;
}

/* Channel scripts. Each controller stream is rendered first; a stream whose tail repeats an
   earlier stream's tail jumps into it, as M64 export does, so channels that share volume or pan
   automation store it once. */
static bool write_channels(Song *s, OotText *t, unsigned mask, const LayerRef *layers,
                           size_t layer_count) {
    enum { STREAMS = 16 * SECTIONS };

    Emitting streams[STREAMS] = {{0}};
    size_t cut[STREAMS], target[STREAMS], at[STREAMS];
    bool *labels[STREAMS] = {0}, shared[STREAMS] = {0}, ok = true;

    for (unsigned id = 0; id < STREAMS && ok; id++) {
        unsigned out = id / SECTIONS, sec = id % SECTIONS;

        if (!(mask & (1u << out)) || sec >= s->section_count) {
            continue;
        }

        s->capture = &streams[id];
        write_stream(s, t, out, sec);
        s->capture = NULL;
        labels[id] = calloc(streams[id].count + 1, sizeof(bool));
        ok = !streams[id].failed && labels[id];
        size_t best = 0;

        for (unsigned prev = 0; prev < id && ok; prev++) {
            const Emitting *a = &streams[id], *b = &streams[prev];

            if (!b->count || shared[prev]) {
                continue;
            }

            size_t match = 0, bytes = 0;

            while (
                match < a->count && match < b->count &&
                !strcmp(a->items[a->count - 1 - match].text, b->items[b->count - 1 - match].text)) {
                bytes += a->items[a->count - 1 - match].size;
                match++;
            }

            if (bytes > 3 && bytes - 3 > best) {
                best = bytes - 3;
                shared[id] = true;
                cut[id] = a->count - match;
                target[id] = prev;
                at[id] = b->count - match;
            }
        }

        if (shared[id]) {
            labels[target[id]][at[id]] = true;
        }
    }

    for (unsigned id = 0; id < STREAMS && ok; id++) {
        unsigned out = id / SECTIONS, sec = id % SECTIONS;

        if (!(mask & (1u << out)) || sec >= s->section_count) {
            continue;
        }

        write_head(s, t, out, sec, layers, layer_count);
        const Emitting *e = &streams[id];
        size_t stop = shared[id] ? cut[id] : e->count;

        for (size_t k = 0; k < stop; k++) {
            if (labels[id][k]) {
                oot_text_add(t, "CTRL_%u_%zu:\n", id, k);
            }

            emit(s, t, e->items[k].size, "%s", e->items[k].text);
        }

        if (shared[id]) {
            emit(s, t, 3, "jump CTRL_%u_%zu", (unsigned)target[id], at[id]);
        }
    }

    for (unsigned id = 0; id < STREAMS; id++) {
        free(streams[id].items);
        free(labels[id]);
    }

    return ok;
}

static bool write_sequence(Song *s) {
    const OotRequest *q = s->request;
    OotText *t = &s->result->sequence;
    unsigned mask = 0;
    LayerRef layers[16 * SECTIONS * 4];
    size_t layer_count = 0;
    const Note **voice = calloc(s->note_count + 1, sizeof(*voice));

    if (!voice) {
        return false;
    }

    s->command_count = s->chunk_count = 0;
    s->sub_count = 0; /* Each render numbers its own subroutines. */

    for (unsigned out = 0; out < 16; out++) {
        if (s->out_of[out] < 0) {
            continue;
        }

        mask |= 1u << out;

        for (unsigned sec = 0; sec < s->section_count; sec++) {
            for (unsigned layer = 0; layer < 4; layer++) {
                size_t count = 0;

                for (size_t i = 0; i < s->note_count; i++) {
                    const Note *n = &s->notes[i];

                    if (n->voice >= 0 && n->voice % 4 == (int)layer &&
                        s->group_channel[n->channel][n->voice / 4] == (int)out &&
                        section_of(s, n->start) == sec) {
                        voice[count++] = n;
                    }
                }

                if (!count) {
                    continue;
                }

                qsort(voice, count, sizeof(*voice), compare_note_pointers);
                size_t first;
                LayerRef *ref = &layers[layer_count++];
                *ref = (LayerRef){0, 0, out, sec, layer};

                if (!build_layer(s, voice, count, s->sections[sec][0], &first) ||
                    !chunk_layer(s, first, s->sections[sec][0], &ref->chunk_begin)) {
                    free(voice);
                    return false;
                }

                ref->chunk_end = s->chunk_count;
            }
        }

        if (cancelled(s, 40 + out * 3)) {
            free(voice);
            return false;
        }
    }

    free(voice);
    share_chunks(s);
    s->bytes = 0;
    oot_text_add(t, "/* %s: generated by duckmidi for zeldaret/oot. */\n#include \"aseq.h\"\n",
                 q->name);

    if (q->font == OOT_CUSTOM_FONT) {
        oot_text_add(t, "#include \"Soundfont_%s.h\"\n", q->name);
    } else {
        oot_text_add(t, "#include \"Soundfont_%d.h\"\n", q->font);
    }

    oot_text_add(t, "\n.startseq Sequence_%s\n", q->name);
    write_sequence_script(s, t, mask);

    if (!write_channels(s, t, mask, layers, layer_count)) {
        return false;
    }

    for (size_t i = 0; i < layer_count; i++) {
        write_layer(s, t, &layers[i]);
    }

    write_subroutines(s, t);
    oot_text_add(t, "\n.endseq Sequence_%s\n", q->name);
    s->result->sequence_bytes = (s->bytes + 15) & ~(uint64_t)15;
    s->result->channels = (unsigned)__builtin_popcount(mask);

    if (s->bytes > 0xFFFF) {
        snprintf(s->error, s->capacity,
                 "The sequence needs %llu bytes; OoT sequences address at most 64 KiB.",
                 (unsigned long long)s->bytes);
        return false;
    }

    return !t->failed;
}

/* ---- Soundfont ---- */

typedef struct {
    uint16_t release;
    uint8_t count;
    const int16_t (*points)[2];
} EnvelopeRef;

static int envelope_index(EnvelopeRef *list, unsigned *count, uint16_t release, uint8_t points,
                          const int16_t (*data)[2]) {
    for (unsigned i = 0; i < *count; i++) {
        if (list[i].release == release && list[i].count == points &&
            !memcmp(list[i].points, data, sizeof(*data) * points)) {
            return (int)i;
        }
    }

    list[*count] = (EnvelopeRef){release, points, data};
    return (int)(*count)++;
}

static bool write_soundfont(Song *s) {
    const OotRequest *q = s->request;
    OotText *t = &s->result->soundfont;
    EnvelopeRef envelopes[128 + 128];
    unsigned envelope_count = 0;
    int instrument_env[128], drum_env[128];
    bool sample_used[1024] = {false};
    unsigned sample_count = q->custom ? (unsigned)q->custom->count : 0;
    unsigned drum_keys = 0, max_key = 0;

    for (unsigned i = 0; i < s->instrument_count; i++) {
        int value = s->instrument_order[i];

        if (value >= 1024) {
            const EsNativeRegion *r = &q->custom->regions[value - 1024];
            instrument_env[i] = envelope_index(envelopes, &envelope_count, r->release,
                                               (uint8_t)r->point_count, r->points);
            continue;
        }

        const OotInstrument *in = &oot_instruments[s->instrument_order[i]];
        instrument_env[i] =
            envelope_index(envelopes, &envelope_count, in->release, in->point_count, in->points);

        for (unsigned r = 0; r < in->region_count; r++) {
            sample_count += !sample_used[in->regions[r].sample];
            sample_used[in->regions[r].sample] = true;
        }
    }

    for (unsigned k = 0; k < 128; k++) {
        int d = s->drum_for_key[k];

        if (d < 0) {
            continue;
        }

        if (d >= 1024) {
            const EsNativeRegion *r = &q->custom->regions[d - 1024];
            drum_env[k] = envelope_index(envelopes, &envelope_count, r->release,
                                         (uint8_t)r->point_count, r->points);
            drum_keys++;
            max_key = k;
            continue;
        }

        const OotDrum *drum = &oot_drums[d];
        drum_env[k] = envelope_index(envelopes, &envelope_count, drum->release, drum->point_count,
                                     drum->points);
        sample_count += !sample_used[drum->sample];
        sample_used[drum->sample] = true;
        drum_keys++;
        max_key = k;
    }

    oot_text_add(
        t,
        "<!-- %s: generated by duckmidi from vanilla OoT instruments on sample bank 0. -->\n"
        "<Soundfont Name=\"Soundfont_%s\" Index=\"%d\" Medium=\"MEDIUM_CART\" "
        "CachePolicy=\"CACHE_LOAD_TEMPORARY\" "
        "SampleBank=\"$(BUILD_DIR)/%s\" "
        "%s>\n    <Envelopes>\n",
        q->name, q->name, q->font_index,
        q->sample_bank ? q->sample_bank : "assets/audio/samplebanks/SampleBank_0.xml",
        !q->sample_bank || !strcmp(q->sample_bank, "assets/audio/samplebanks/SampleBank_0.xml")
            ? "Indirect=\"1\""
            : "");

    for (unsigned e = 0; e < envelope_count; e++) {
        oot_text_add(t, "        <Envelope Name=\"Env%u\" Release=\"%u\">\n", e,
                     envelopes[e].release);

        for (unsigned p = 0; p < envelopes[e].count; p++) {
            oot_text_add(t, "            <Point Delay=\"%d\" Arg=\"%d\"/>\n",
                         envelopes[e].points[p][0], envelopes[e].points[p][1]);
        }

        oot_text_add(t, "        </Envelope>\n");
    }

    oot_text_add(t, "    </Envelopes>\n    <Samples>\n");

    for (unsigned i = 0; i < 1024; i++) {
        if (sample_used[i]) {
            oot_text_add(t, "        <Sample Name=\"SAMPLE_0_%u\"/>\n", i);
        }
    }

    for (size_t i = 0; q->custom && i < q->custom->count; i++) {
        const EsNativeRegion *r = &q->custom->regions[i];
        unsigned stock;
        int consumed = 0;
        bool listed = sscanf(r->name, "SAMPLE_0_%u%n", &stock, &consumed) == 1 &&
                      !r->name[consumed] && stock < 1024 && sample_used[stock];

        for (size_t earlier = 0; earlier < i; earlier++) {
            listed |= !strcmp(q->custom->regions[earlier].name, r->name);
        }

        if (!listed) {
            oot_text_add(t, "        <Sample Name=\"%s\"/>\n", r->name);
        }
    }

    oot_text_add(t, "    </Samples>\n");

    if (drum_keys) {
        oot_text_add(t, "    <Drums>\n");

        for (unsigned k = 0; k < 128; k++) {
            int d = s->drum_for_key[k];

            if (d < 0) {
                continue;
            }

            if (d >= 1024) {
                const EsNativeRegion *r = &q->custom->regions[d - 1024];
                char note[16];
                oot_note_name((int)k, note, sizeof(note));
                double played = es_native_drum_key(r, (unsigned)s->drum_original[k]);
                double tuning = (r->reference_tuning > 0 ? r->reference_tuning : r->tuning) *
                                pow(2, (played - k) / 12);
                oot_text_add(t,
                             "        <Drum Name=\"DRUM_%u\" Note=\"%s\" Pan=\"%u\" "
                             "Envelope=\"Env%d\" Sample=\"%s\" BaseNote=\"C4\" "
                             "SampleRate=\"%.12g\"/>\n",
                             k, note, es_native_pan(r), drum_env[k], r->name, tuning * 32000);
                continue;
            }

            const OotDrum *drum = &oot_drums[d];
            int played = (int)oot_drum_key(OOT_CUSTOM_FONT, d, k);
            char note[16], base[16];
            oot_note_name((int)k, note, sizeof(note));
            oot_note_name(drum->base - (played - drum->lo), base, sizeof(base));
            oot_text_add(t,
                         "        <Drum Name=\"DRUM_%u\" Note=\"%s\" Pan=\"%u\" Envelope=\"Env%d\" "
                         "Sample=\"SAMPLE_0_%u\" BaseNote=\"%s\"/>\n",
                         k, note, drum->pan, drum_env[k], drum->sample, base);
        }

        oot_text_add(t, "    </Drums>\n");
    }

    if (s->instrument_count) {
        oot_text_add(t, "    <Instruments>\n");

        for (unsigned i = 0; i < s->instrument_count; i++) {
            int value = s->instrument_order[i];

            if (value >= 1024) {
                const EsNativeRegion *r = &q->custom->regions[value - 1024];
                oot_text_add(t,
                             "        <Instrument ProgramNumber=\"%u\" Name=\"INST_%u\" "
                             "Envelope=\"Env%d\" Sample=\"%s\" BaseNote=\"C4\" "
                             "SampleRate=\"%.12g\"/>\n",
                             i, i, instrument_env[i], r->name,
                             (r->reference_tuning > 0 ? r->reference_tuning : r->tuning) * 32000);
                continue;
            }

            const OotInstrument *in = &oot_instruments[s->instrument_order[i]];
            oot_text_add(t,
                         "        <Instrument ProgramNumber=\"%u\" Name=\"INST_%u\" "
                         "Envelope=\"Env%d\" %s/>\n",
                         i, i, instrument_env[i], in->attributes);
        }

        oot_text_add(t, "    </Instruments>\n");
    }

    oot_text_add(t, "</Soundfont>\n");

    /* Conservative built size: aligned tables, envelopes and every emitted sample, including
       unused custom definitions. A sample needs 16 header bytes, up to 528 bytes for an
       order-2 codebook with all 16 native predictors, and 48 bytes of loop state. */
    uint64_t size = 0x20 + 4ull * s->instrument_count + 0x20ull * s->instrument_count;
    size += drum_keys ? 4ull * (max_key - 20) + 0x10ull * drum_keys : 0;
    size += 0x250ull * sample_count;

    for (unsigned e = 0; e < envelope_count; e++) {
        size += ((4ull * (envelopes[e].count + 1)) + 15) & ~15ull;
    }

    s->result->soundfont_bytes = (size + 15) & ~15ull;
    s->result->instruments = s->instrument_count;
    s->result->drums = drum_keys;
    return !t->failed;
}

/* ---- Entry point ---- */

void oot_result_free(OotResult *result) {
    if (!result) {
        return;
    }

    oot_text_free(&result->sequence);
    oot_text_free(&result->soundfont);
    free(result->diagnostics);
    free(result->ranges);
    *result = (OotResult){0};
}

/* Writes the sequence with short and with large notes and keeps the smaller; short notes save
   the velocity and gate bytes of notes that repeat them. */
static bool write_smallest(Song *s) {
    OotText *t = &s->result->sequence;
    s->short_notes = true;
    t->size = 0;
    *s->error = 0;
    write_sequence(s);
    uint64_t short_bytes = s->bytes;

    if (t->failed) {
        return false;
    }

    s->short_notes = false;
    t->size = 0;
    *s->error = 0;
    bool ok = write_sequence(s);

    if (t->failed || s->bytes <= short_bytes) {
        return ok && !t->failed;
    }

    s->short_notes = true;
    t->size = 0;
    *s->error = 0;
    return write_sequence(s);
}

typedef struct {
    Song *song;
    const Control *source;
    size_t count;
} Fitting;

/* es_fit_curves callback: writes the sequence with the kept controls. */
static int render_kept(void *user, const bool *keep, uint64_t *bytes) {
    Fitting *f = user;
    Song *s = f->song;

    if (cancelled(s, 90)) {
        return -1;
    }

    s->control_count = 0;

    for (size_t i = 0; i < f->count; i++) {
        if (keep[i]) {
            s->controls[s->control_count++] = f->source[i];
        }
    }

    bool fits = write_smallest(s);
    *bytes = s->bytes;

    /* A failure other than size (memory, cancellation) stops the search. */
    return s->result->sequence.failed || (!fits && s->bytes <= 0xFFFF) ? -1 : fits;
}

/* Last resort: rounds note starts, ends and velocities to coarser grids until the song fits.
   A note shortened to nothing by the next note of its layer is merged into it. Notes stay in
   their loop section. */
static bool fit_notes(Song *s) {
    static const uint64_t grids[] = {2, 3, 4, 6, 8, 12, 24, 48, 96, 192};
    Note *original = malloc((s->note_count + 1) * sizeof(*original));

    if (!original) {
        return false;
    }

    memcpy(original, s->notes, s->note_count * sizeof(*original));
    bool fits = false;

    for (size_t pass = 0; pass < sizeof(grids) / sizeof(*grids) && !fits; pass++) {
        uint64_t grid = grids[pass];
        unsigned quantum = pass < 2 ? 4 : pass < 5 ? 8 : 16;
        int previous[16][64];

        if (cancelled(s, 95)) {
            break;
        }

        memset(previous, -1, sizeof(previous));

        for (size_t i = 0; i < s->note_count; i++) {
            Note n = original[i];

            if (n.voice < 0 || n.voice >= 64 || n.end <= n.start) {
                s->notes[i] = n;
                continue;
            }

            unsigned sec = section_of(s, n.start);
            uint64_t low = s->sections[sec][0], high = s->sections[sec][1];
            n.start = (n.start + grid / 2) / grid * grid;
            n.end = (n.end + grid / 2) / grid * grid;
            n.start = n.start < low ? low : n.start >= high ? high - 1 : n.start;
            n.end = n.end > high ? high : n.end;
            n.end = n.end <= n.start ? n.start + 1 : n.end;
            unsigned velocity = (n.velocity + quantum / 2) / quantum * quantum;
            n.velocity = (uint8_t)(velocity > 127 ? 127 : velocity ? velocity : 1);
            int prior = previous[n.channel][n.voice];

            if (prior >= 0 && s->notes[prior].end > n.start) {
                Note *old = &s->notes[prior];

                if (old->start == n.start) {
                    /* Same start: one note, the longer length and the louder velocity. */
                    old->end = old->end > n.end ? old->end : n.end;
                    old->velocity = old->velocity > n.velocity ? old->velocity : n.velocity;
                    n.voice = -1;
                } else {
                    old->end = n.start;
                }
            }

            s->notes[i] = n;

            if (n.voice >= 0) {
                previous[n.channel][n.voice] = (int)i;
            }
        }

        fits = write_smallest(s);

        if (s->result->sequence.failed) {
            break;
        }
    }

    if (fits) {
        diagnose_item(s, DME_ALTERED, DM_TEXT_EXPORT_LIST_FIT, "%s", DM_TEXT_EXPORT_ITEM_REENCODED);
    } else {
        memcpy(s->notes, original, s->note_count * sizeof(*original));
    }

    free(original);
    return fits;
}

/* Writes the sequence; when it passes OoT's 64 KiB address limit, simplifies controller curves
   (es_fit_curves) until it fits: within audible error bounds first, densest curves first, then
   by spacing changes apart, keeping as many changes as fit. Notes are never changed here. */
static bool write_fitting(Song *s) {
    OotText *t = &s->result->sequence;

    if (write_smallest(s) || t->failed || s->bytes <= 0xFFFF) {
        return !t->failed && s->bytes <= 0xFFFF;
    }

    if (!s->control_count) {
        return fit_notes(s);
    }

    size_t count = s->control_count;
    Control *source = malloc(count * sizeof(*source));
    EsCurvePoint *points = malloc(count * sizeof(*points));
    bool *keep = malloc(count * sizeof(*keep));
    int fits = -1;

    if (source && points && keep) {
        memcpy(source, s->controls, count * sizeof(*source));

        for (size_t i = 0; i < count; i++) {
            unsigned kind = source[i].kind;
            points[i] = (EsCurvePoint){source[i].tick, source[i].value,
                                       (uint32_t)source[i].channel * CTRL_KINDS + kind,
                                       kind == CTRL_VOL || kind == CTRL_EXPR ? ES_CURVE_VOLUME
                                       : kind == CTRL_PAN                    ? ES_CURVE_PAN
                                       : kind == CTRL_BEND                   ? ES_CURVE_BEND
                                                                             : ES_CURVE_FIXED};
        }

        Fitting fitting = {s, source, count};
        fits =
            es_fit_curves(points, count, s->bytes, 0xFFFF, s->tpb / 2, render_kept, &fitting, keep);
    }

    if (fits > 0) {
        diagnose_item(s, 0, DM_TEXT_EXPORT_LIST_FIT, "%s", DM_TEXT_EXPORT_ITEM_CURVES);
    } else if (!fits && es_thin_curves(points, count, 0, ES_CURVE_SPACINGS - 1, s->tpb / 2, keep) !=
                            SIZE_MAX) {
        /* Even the simplest curves do not fit: keep them and change notes as a last resort. */
        Fitting fitting = {s, source, count};
        uint64_t bytes;

        int simplest = render_kept(&fitting, keep, &bytes);

        if (simplest >= 0) {
            diagnose_item(s, 0, DM_TEXT_EXPORT_LIST_FIT, "%s", DM_TEXT_EXPORT_ITEM_CURVES);
            fits = simplest > 0 || fit_notes(s);
        }
    }

    free(source);
    free(points);
    free(keep);
    return fits > 0;
}

int32_t oot_convert(const OotRequest *q, const DmeHost *host, bool analyze, OotResult *result,
                    char *error, size_t capacity) {
    *result = (OotResult){0};
    if (error && capacity) {
        *error = 0;
    }

    if (!q || !q->ppq || (!q->events && q->event_count) || q->volume < 1 || q->volume > 127 ||
        !oot_name_valid(q->name) || (q->font != OOT_CUSTOM_FONT && !oot_music_font(q->font))) {
        snprintf(error, capacity, "Invalid OoT export settings.");
        return DME_INVALID;
    }

    Song *s = calloc(1, sizeof(*s));

    if (!s) {
        snprintf(error, capacity, "Cannot allocate the OoT conversion.");
        return DME_NOMEM;
    }

    s->request = q;
    s->host = host;
    s->result = result;
    s->error = error;
    s->capacity = capacity;
    int32_t status = DME_FAILED;

    if (!read_events(s)) {
        status = cancelled(s, 0) ? DME_CANCELLED : DME_NOMEM;
        goto done;
    }

    qsort(s->tempos, s->tempo_count, sizeof(*s->tempos), compare_tempos);
    choose_ticks(s);

    if (!plan_sections(s)) {
        status = DME_INVALID;
        goto done;
    }

    if (!split_notes(s) || !assign_voices(s)) {
        status = DME_NOMEM;
        goto done;
    }

    collect_instruments(s);

    if (*error) {
        status = DME_INVALID;
        goto done;
    }
    size_t playable = 0, skipped = 0;

    for (size_t i = 0; i < s->note_count; i++) {
        playable += s->notes[i].voice >= 0;
        skipped += s->notes[i].voice == -2;
    }

    if (!playable) {
        snprintf(error, capacity, "The arrangement has no notes OoT can play.");
        status = DME_INVALID;
        goto done;
    }

    if (skipped) {
        diagnose_item(s, DME_ALTERED, DM_TEXT_EXPORT_LIST_FIT, DM_TEXT_EXPORT_ITEM_SKIPPED,
                      skipped);
    }

    if (!analyze_ranges(s)) {
        status = cancelled(s, 20) ? DME_CANCELLED : *error ? DME_INVALID : DME_NOMEM;
        goto done;
    }

    if (analyze) {
        status = DME_OK;
        goto done;
    }

    if (cancelled(s, 30)) {
        status = DME_CANCELLED;
        goto done;
    }

    if (!write_fitting(s)) {
        status = cancelled(s, 90) ? DME_CANCELLED : *error ? DME_FAILED : DME_NOMEM;
        goto done;
    }

    if (q->font == OOT_CUSTOM_FONT && !write_soundfont(s)) {
        status = DME_NOMEM;
        goto done;
    }

    for (size_t i = 0; i < result->range_count; i++) {
        const OotRangeIssue *r = &result->ranges[i];
        diagnose_item(s, 0, DM_TEXT_EXPORT_LIST_RISK,
                      "Channel %u %s %u: keys %u-%u need %.2fx playback; OoT clamps near 4x, so "
                      "those notes play flat.",
                      r->channel + 1, r->percussion ? "drum key" : "program", r->program, r->low,
                      r->high, r->rate);
    }

    if (q->enlarge_memory) {
        /* Export to Repository raises the caches; a saved package's INSTALL.txt explains how. */
    } else if (result->sequence_bytes > OOT_SEQ_CACHE_COMMON) {
        diagnose_item(s, 0, DM_TEXT_EXPORT_LIST_RISK,
                      "The sequence is %llu bytes; most scenes hold at most %u, so it will not "
                      "load there unless Sequence memory is set to Enlarge.",
                      (unsigned long long)result->sequence_bytes, OOT_SEQ_CACHE_COMMON);
    } else if (result->sequence_bytes > OOT_SEQ_CACHE_MIN) {
        diagnose(s, DME_INFO,
                 "The sequence is %llu bytes; it fits most scenes but not those using audio "
                 "spec 10 (%u bytes).",
                 (unsigned long long)result->sequence_bytes, OOT_SEQ_CACHE_MIN);
    }

    if (!q->enlarge_memory && result->soundfont_bytes > OOT_FONT_CACHE) {
        diagnose_item(s, 0, DM_TEXT_EXPORT_LIST_RISK,
                      "The custom sample set is about %llu bytes, more than the %u-byte font "
                      "cache; use fewer instruments or set Sequence memory to Enlarge.",
                      (unsigned long long)result->soundfont_bytes, OOT_FONT_CACHE);
    }

    status = DME_OK;

done:
    free(s->notes);
    free(s->controls);
    free(s->tempos);
    free(s->commands);
    free(s->chunks);
    free(s);
    return status;
}
