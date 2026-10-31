#include "internal.h"

static void warning_once(Compile *c, unsigned code, const M64Event *e, const char *message) {
    if (!c->warned[code]) {
        c->warned[code] = true;
        diagnose(c, M64_WARNING, code, e, "%s", message);
    }
}

static void release_pedal(Compile *c, unsigned channel, uint64_t tick) {
    for (size_t i = 0; i < c->count; i++) {
        if (c->notes[i].channel == channel && c->notes[i].released &&
            c->notes[i].end == UINT64_MAX) {
            c->notes[i].end = tick;
        }
    }
}

static void end_key(Compile *c, unsigned channel, unsigned key, uint64_t tick, bool all) {
    Channel *ch = &c->channels[channel];
    int index = ch->active[key];

    while (index >= 0) {
        Note *n = &c->notes[index];
        n->released = true;

        if (!ch->pedal) {
            n->end = tick;
        }

        ch->active[key] = n->next;

        if (!all) {
            break;
        }

        index = n->next;
    }

    if (ch->active[key] < 0) {
        ch->tail[key] = -1;
    }
}

static bool same_definition(const M64Instrument *a, const M64Instrument *b) {
    return !strcmp(a->sample, b->sample) && a->tuning == b->tuning &&
           a->release_rate == b->release_rate && a->attack == b->attack && a->decay == b->decay &&
           a->sustain == b->sustain;
}

static bool assign_instrument(Compile *c, Note *n) {
    if (n->channel == MIDI_DRUM_CHANNEL) {
        if (c->drums[n->key] < 0) {
            c->drums[n->key] = (int)c->drum_count;
            c->percussion[c->drum_count++] = n->key;
        }
    } else if (c->slots[n->program] < 0) {
        for (unsigned i = 0; i < c->program_count; i++) {
            if ((n->program & 128) == (c->programs[i] & 128) &&
                same_definition(program_definition(c, n->program),
                                program_definition(c, c->programs[i]))) {
                c->slots[n->program] = (int)i;
                return true;
            }
        }

        if (c->program_count >= 127) {
            c->result->status = M64_LIMIT;
            diagnose(c, M64_ERROR, 20, NULL, "More than 127 distinct melodic instruments.");
            return false;
        }

        c->slots[n->program] = (int)c->program_count;
        c->programs[c->program_count++] = n->program;
    }

    return true;
}

static int note_order(const void *left, const void *right) {
    const Note *a = left, *b = right;

    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }

    return a->section < b->section ? -1 : a->section > b->section;
}

/* Loop regions: the song plays from the start once, then [loop_start, loop_end) repeats. Notes
   are cut at the loop end, and notes sounding across the loop start are split there, so the
   repeated part starts each note it needs. Each note keeps its channel and layer. */
static bool split_region(Compile *c) {
    const M64Request *q = c->request;
    c->region = true;
    c->loop_start = tick48(c, q->loop_start);
    c->loop_end = q->loop_end ? tick48(c, q->loop_end) : c->duration;

    if (c->loop_end <= c->loop_start) {
        c->result->status = M64_INVALID;
        diagnose(c, M64_ERROR, 60, NULL, "The loop region is shorter than one SM64 tick.");
        return false;
    }

    size_t write = 0, count = c->count;

    for (size_t i = 0; i < count; ++i) {
        Note n = c->notes[i];

        if (n.start >= c->loop_end) {
            continue;
        }

        n.end = n.end > c->loop_end ? c->loop_end : n.end;
        n.section = n.start >= c->loop_start;
        c->notes[write++] = n;
    }

    c->count = write;

    for (size_t i = 0; i < write; ++i) {
        Note n = c->notes[i];

        if (n.section || n.end <= c->loop_start) {
            continue;
        }

        if (!reserve((void **)&c->notes, &c->capacity, c->count + 1, sizeof(Note))) {
            c->result->status = M64_NOMEM;
            return false;
        }

        c->notes[i].end = c->loop_start;
        n.start = c->loop_start;
        n.section = 1;
        c->notes[c->count++] = n;
    }

    qsort(c->notes, c->count, sizeof(Note), note_order);
    c->duration = c->loop_end;
    diagnose(
        c, M64_INFO, 61, NULL,
        "The loop region repeats from beat %.2f to beat %.2f; notes crossing its start restart "
        "there.",
        (double)c->loop_start / 48 / c->tempo_scale, (double)c->loop_end / 48 / c->tempo_scale);
    return true;
}

bool timeline(Compile *c) {
    const M64Request *q = c->request;
    c->duration = tick48(c, q->duration);

    for (unsigned ch = 0; ch < 16; ch++) {
        Channel *s = &c->channels[ch];
        s->volume = 100;
        s->expression = 127;
        s->pan = 64;
        s->range = 2;
        s->rpn_lsb = s->rpn_msb = 127;

        for (unsigned k = 0; k < 128; k++) {
            s->active[k] = s->tail[k] = -1;
        }

        c->sources[ch] = -1;
    }

    uint64_t previous = 0;

    for (uint64_t i = 0; i < q->event_count; i++) {
        const M64Event *e = &q->events[i];

        if ((i && e->tick < previous) || e->tick > q->duration || e->status > 255 ||
            (e->status < MIDI_SYSTEM && (e->status < MIDI_STATUS || e->a > 127 || e->b > 127)) ||
            (e->status == MIDI_META && (!e->a || e->a > 16777215))) {
            c->result->status = M64_INVALID;
            diagnose(c, M64_ERROR, 3, e, "Invalid or unsorted timeline event.");
            return false;
        }

        previous = e->tick;

        if (!(i % 1024) &&
            cancelled(c, 1, (uint32_t)(i * 100 / (q->event_count ? q->event_count : 1)))) {
            return false;
        }

        uint64_t t = tick48(c, e->tick);

        if (e->tick * 48 % q->ppq) {
            warning_once(c, 30, e, "Timing rounded to SM64's 48 ticks per quarter note.");
        }

        if (e->status == MIDI_META) {
            double bpm = 60000000.0 / e->a;
            bpm *= c->tempo_scale;

            if (bpm < 1) {
                warning_once(c, 4, e,
                             "Extremely slow tempos are clamped to the engine's minimum BPM.");
            }

            if (fabs(bpm - round(bpm)) > 0.0001) {
                warning_once(c, 31, e, "Fractional tempos rounded to integer BPM.");
            }

            continue;
        }

        if (e->status >= MIDI_SYSTEM) {
            warning_once(
                c, 32, e,
                "SysEx or unsupported source messages are omitted; GM channel 10 is treated as percussion.");
            continue;
        }

        unsigned channel = e->status & MIDI_CHANNEL_MASK, type = e->status & MIDI_STATUS_MASK;
        Channel *s = &c->channels[channel];

        if (type == MIDI_PROGRAM) {
            s->program = e->a;
            continue;
        }

        if (type == MIDI_NOTE_OFF || (type == MIDI_NOTE_ON && !e->b)) {
            if (s->active[e->a] < 0) {
                warning_once(c, 33, e, "Unmatched note-off ignored.");
            }

            end_key(c, channel, e->a, t, false);
            continue;
        }

        if (type == MIDI_NOTE_ON) {
            if (!reserve((void **)&c->notes, &c->capacity, c->count + 1, sizeof(Note))) {
                c->result->status = M64_NOMEM;
                return false;
            }

            Note n = {0};
            n.start = t;
            n.end = UINT64_MAX;
            n.source = e->source_id;
            n.channel = channel;
            n.key = e->a;
            n.velocity = e->b;
            n.program = s->program | ((channel != MIDI_DRUM_CHANNEL && e->a >= 117) ? 128u : 0u);
            n.next = -1;

            if (channel != MIDI_DRUM_CHANNEL && q->channel_programs) {
                n.program |= channel << 8;
            }

            int index = (int)c->count;
            c->notes[c->count++] = n;

            if (s->tail[e->a] >= 0) {
                c->notes[s->tail[e->a]].next = index;
            } else {
                s->active[e->a] = index;
            }

            s->tail[e->a] = index;
            c->sources[channel] = (int)channel;

            if (channel == MIDI_DRUM_CHANNEL && s->program) {
                warning_once(
                    c, 34, e,
                    "GM standard drum mapping is used; alternate drum kits are approximated.");
            }
        } else if (type == MIDI_CONTROL) {
            switch (e->a) {
            case MIDI_CC_SUSTAIN:
                if (s->pedal && e->b < 64) {
                    release_pedal(c, channel, t);
                }

                s->pedal = e->b >= 64;
                break;
            case MIDI_CC_SOUND_OFF:
            case MIDI_CC_NOTES_OFF:
                for (unsigned key = 0; key < 128; key++) {
                    end_key(c, channel, key, t, true);
                }

                if (e->a == 120) {
                    release_pedal(c, channel, t);
                }
                break;
            case MIDI_CC_RESET:
                s->pedal = 0;
                release_pedal(c, channel, t);
                break;
            case MIDI_CC_BANK:
            case MIDI_CC_BANK_LSB:
                if (e->b) {
                    warning_once(c, 35, e, "Nonzero MIDI banks use the GM program approximation.");
                }
                break;
            case MIDI_CC_MODULATION:
                if (e->b) {
                    warning_once(c, 41, e, "Modulation is approximated with SM64 vibrato depth.");
                }
                break;
            case 7:
            case 10:
            case 11:
            case 91:
            case 100:
            case 101:
            case 6:
            case 38:
                break;
            default:
                if (!c->warned[128 + e->a]) {
                    c->warned[128 + e->a] = true;
                    diagnose(c, M64_WARNING, 128 + e->a, e, "MIDI controller %u is omitted.", e->a);
                }
                break;
            }
        } else if (type != MIDI_PITCH_BEND) {
            warning_once(c, 36, e, "Channel/polyphonic pressure is omitted.");
        }
    }

    if (!c->count) {
        c->result->status = M64_INVALID;
        diagnose(c, M64_ERROR, 5, NULL, "No notes in the selected arrangement.");
        return false;
    }

    uint64_t until[16][4] = {{0}};
    int active[16][4];

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned l = 0; l < 4; l++) {
            active[ch][l] = -1;
        }
    }

    size_t write = 0;

    for (size_t i = 0; i < c->count; i++) {
        if (!(i % 1024) && cancelled(c, 1, 75)) {
            return false;
        }

        Note n = c->notes[i];
        M64Event location = {n.start * q->ppq / 48, n.source, MIDI_NOTE_ON | n.channel, n.key,
                             n.velocity};

        if (n.end == UINT64_MAX) {
            n.end = c->duration;

            if (!c->warned[37]) {
                c->warned[37] = true;
                diagnose(c, M64_WARNING, 37, NULL,
                         "Held or unterminated notes end at the arrangement boundary.");
            }
        }

        if (n.end <= n.start) {
            warning_once(c, 38, NULL,
                         "Notes shorter than one target tick were extended to one tick.");
            n.end = n.start + 1;
        }

        if (n.end - n.start > 32767) {
            warning_once(
                c, 21, &location,
                "Very long notes are retriggered in segments to fit the engine's note duration field.");
        }

        if (n.end > c->duration) {
            c->duration = n.end;
        }

        unsigned voices = 0;
        int output = -1, layer = -1;

        for (unsigned ch = 0; ch < 16; ch++) {
            for (unsigned l = 0; l < 4; l++) {
                voices += until[ch][l] > n.start;

                if (c->sources[ch] == (int)n.channel && until[ch][l] <= n.start && output < 0) {
                    output = (int)ch;
                    layer = (int)l;
                }
            }
        }

        if (voices >= q->voice_limit && !c->warned[22]) {
            c->warned[22] = true;
            diagnose(
                c, M64_WARNING, 22, &location,
                "The arrangement exceeds the %u-voice warning threshold. Notes are retained; the game may steal voices during playback. Actual capacity depends on the target ROM.",
                q->voice_limit);
        }

        if (output < 0) {
            for (unsigned ch = 0; ch < 16; ch++) {
                if (c->sources[ch] < 0) {
                    c->sources[ch] = (int)n.channel;
                    output = (int)ch;
                    layer = 0;
                    warning_once(
                        c, 39, NULL,
                        "Dense chords use additional SM64 channels; controller changes are mirrored.");
                    break;
                }
            }
        }

        if (output < 0) {
            int quietest = 128;

            for (unsigned ch = 0; ch < 16; ch++) {
                if (c->sources[ch] == (int)n.channel) {
                    for (unsigned l = 0; l < 4; l++) {
                        int index = active[ch][l];

                        if (index >= 0 && (int)c->notes[index].velocity < quietest) {
                            quietest = (int)c->notes[index].velocity;
                            output = (int)ch;
                            layer = (int)l;
                        }
                    }
                }
            }

            warning_once(
                c, 23, &location,
                "Dense chords exceed the engine's available layers. Quieter voices were shortened or omitted to retain the stronger notes.");

            if (output < 0) {
                continue;
            }

            Note *old = &c->notes[active[output][layer]];

            if (old->start == n.start && old->velocity >= n.velocity) {
                continue;
            }

            old->end = n.start;
        }

        n.output = (uint32_t)output;
        n.layer = (uint32_t)layer;
        until[output][layer] = n.end;
        active[output][layer] = (int)write;

        if (!assign_instrument(c, &n)) {
            return false;
        }

        c->notes[write++] = n;
    }

    c->count = write;

    if (q->loop == 2 && !split_region(c)) {
        return false;
    }

    diagnose(
        c, M64_INFO, 40, NULL,
        "GM timbres use SM64 sample approximations. Release tails and sound effects share the game's voice budget.");
    return !cancelled(c, 1, 100);
}
