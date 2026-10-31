#include "internal.h"

static bool bend(Compile *c, Buffer *b, Channel *s, const M64Event *e) {
    double semitones = ((double)s->bend / MIDI_BEND_CENTER) * (s->range + s->cents / 100.0);
    double multiplier = pow(2.0, semitones / 12.0);

    if (multiplier < 1.0 / 32768.0 || multiplier >= 2.0) {
        if (!c->warned[50]) {
            c->warned[50] = true;
            diagnose(c, M64_WARNING, 50, e,
                     "Pitch bend beyond the engine's multiplier range is clamped.");
        }

        if (multiplier < 1.0 / 32768.0) {
            multiplier = 1.0 / 32768.0;
        } else {
            multiplier = 65535.0 / 32768.0;
        }
    }

    unsigned value = (unsigned)lround(multiplier * 32768.0);

    if (value > 65535) {
        value = 65535;
    }

    if (value != s->encoded_bend) {
        put(b, 0xde);
        put(b, value >> 8);
        put(b, value);
        s->encoded_bend = value;
    }

    return true;
}

static bool control(Compile *c, Buffer *b, Channel *s, const M64Event *e) {
    unsigned type = e->status & MIDI_STATUS_MASK;

    if (type == MIDI_PITCH_BEND) {
        s->bend = (int)(e->a | (e->b << 7)) - MIDI_BEND_CENTER;
        return bend(c, b, s, e);
    }

    if (type != MIDI_CONTROL) {
        return true;
    }

    switch (e->a) {
    case MIDI_CC_VOLUME:
        if (s->volume == e->b) {
            return true;
        }

        s->volume = e->b;
        break;
    case MIDI_CC_EXPRESSION:
        if (s->expression == e->b) {
            return true;
        }

        s->expression = e->b;
        break;
    case MIDI_CC_PAN:
        if (s->pan != e->b) {
            put(b, 0xdd);
            put(b, (e->b * 128 + 63) / 127);
            s->pan = e->b;
        }

        return true;
    case MIDI_CC_REVERB:
        if (s->reverb != e->b) {
            put(b, 0xd4);
            put(b, e->b);
            s->reverb = e->b;
        }

        return true;
    case MIDI_CC_MODULATION:
        if (s->modulation != e->b) {
            put(b, 0xd8);
            put(b, e->b / 4);
            s->modulation = e->b;
        }

        return true;
    case MIDI_CC_RPN_LSB:
        s->rpn_lsb = e->b;
        return true;
    case MIDI_CC_RPN_MSB:
        s->rpn_msb = e->b;
        return true;
    case MIDI_CC_DATA:
    case MIDI_CC_DATA_LSB:
        if (!s->rpn_lsb && !s->rpn_msb) {
            if (e->a == 6) {
                s->range = e->b;
            } else {
                s->cents = e->b;
            }

            return bend(c, b, s, e);
        }

        if (!c->warned[51]) {
            c->warned[51] = true;
            diagnose(c, M64_WARNING, 51, e, "Only pitch-bend-range RPN is supported.");
        }

        return true;
    case 121:
        s->expression = 127;
        s->bend = 0;
        s->rpn_lsb = s->rpn_msb = 127;
        s->encoded_bend = 32768;
        s->modulation = 0;
        put(b, 0xd8);
        put(b, 0);
        put(b, 0xde);
        put(b, 0x80);
        put(b, 0);
        break;
    default:
        return true;
    }

    put(b, 0xdf);
    put(b, (s->volume * s->expression + 63) / 127);
    return true;
}

/* Section bounds in 48-tick units. Without a loop region everything is one section. */
static uint64_t section_begin(const Compile *c) {
    return c->region && c->emit_section ? c->loop_start : 0;
}

static uint64_t section_end(const Compile *c) {
    if (!c->region) {
        return UINT64_MAX;
    }

    return c->emit_section ? c->loop_end : c->loop_start;
}

/* Controls before a section begins are replayed at its start, so a repeated region opens with
   the same channel state each time. */
static bool controller_stream(Compile *c, unsigned ch, Buffer *b) {
    Channel state = {0};
    state.volume = 100;
    state.expression = 127;
    state.range = 2;
    state.rpn_msb = state.rpn_lsb = 127;
    state.pan = 64;
    state.encoded_bend = 32768;
    uint64_t now = 0, begin = section_begin(c), end = section_end(c);

    for (uint64_t i = 0; i < c->request->event_count; i++) {
        const M64Event *e = &c->request->events[i];
        unsigned type = e->status & MIDI_STATUS_MASK;

        if (e->status >= MIDI_SYSTEM || (int)(e->status & MIDI_CHANNEL_MASK) != c->sources[ch] ||
            (type != MIDI_CONTROL && type != MIDI_PITCH_BEND)) {
            continue;
        }

        if (c->quality_grid &&
            (type == MIDI_PITCH_BEND || e->a == MIDI_CC_MODULATION || e->a == MIDI_CC_VOLUME ||
             e->a == MIDI_CC_PAN || e->a == MIDI_CC_EXPRESSION || e->a == MIDI_CC_REVERB)) {
            uint64_t bucket = tick48(c, e->tick) / c->quality_grid;
            bool replaced = false;

            for (uint64_t next = i + 1; next < c->request->event_count; next++) {
                const M64Event *later = &c->request->events[next];

                if (tick48(c, later->tick) / c->quality_grid > bucket) {
                    break;
                }

                if (later->status == e->status && (type == MIDI_PITCH_BEND || later->a == e->a)) {
                    replaced = true;
                    break;
                }
            }

            if (replaced) {
                continue;
            }
        }

        uint64_t t = tick48(c, e->tick);

        if (c->quality_grid) {
            t = t / c->quality_grid * c->quality_grid;
        }

        if (t >= end) {
            break;
        }

        t = t > begin ? t - begin : 0;
        Buffer command = {0};
        bool ok = control(c, &command, &state, e);

        if (command.size) {
            delay(b, 0xfd, t - now);
            now = t;
            bytes(b, command.data, command.size);
        }

        if (command.failed) {
            b->failed = true;
        }

        free(command.data);

        if (!ok) {
            return false;
        }
    }

    put(b, 0xf3); /* Keep layers alive after the last controller. */
    return !b->failed;
}

static size_t next_controller(const Buffer *b, size_t pos) {
    unsigned op = b->data[pos++];

    if (op == 0xfd) {
        if (b->data[pos++] & 128) {
            pos++;
        }
    } else if (op == 0xde) {
        pos += 2;
    } else if (op != 0xf3) {
        pos++;
    }

    return pos;
}

static unsigned tempo_value(const Compile *c, const M64Event *e) {
    return (unsigned)fmax(1, round(60000000.0 / e->a * c->tempo_scale));
}

/* Writes the tempo changes of the current section to the sequence script. Returns the time of the
   last write, relative to the section. */
static uint64_t tempo_stream(Compile *c, Buffer *b, unsigned *last_tempo) {
    uint64_t now = 0, begin = section_begin(c), end = section_end(c);

    for (uint64_t i = 0; i < c->request->event_count; i++) {
        const M64Event *e = &c->request->events[i];

        if (e->status != MIDI_META) {
            continue;
        }

        uint64_t t = tick48(c, e->tick);

        if (c->quality_grid) {
            t = t / c->quality_grid * c->quality_grid;
            bool replaced = false;

            for (uint64_t next = i + 1; next < c->request->event_count; next++) {
                const M64Event *later = &c->request->events[next];

                if (tick48(c, later->tick) / c->quality_grid > t / c->quality_grid) {
                    break;
                }

                if (later->status == MIDI_META) {
                    replaced = true;
                    break;
                }
            }

            if (replaced) {
                continue;
            }
        }

        if (t >= end) {
            break;
        }

        if (t < begin) {
            continue;
        }

        unsigned value = tempo_value(c, e);

        if (value == *last_tempo) {
            continue;
        }

        delay(b, 0xfd, t - begin - now);
        now = t - begin;
        *last_tempo = value;
        put(b, 0xdd);
        put(b, value);
    }

    return now;
}

/* The tempo in effect when the loop region begins. */
static unsigned region_tempo(Compile *c) {
    unsigned tempo = (unsigned)fmax(1, round(120 * c->tempo_scale));

    for (uint64_t i = 0; i < c->request->event_count; i++) {
        const M64Event *e = &c->request->events[i];

        if (e->status == MIDI_META && tick48(c, e->tick) <= c->loop_start) {
            tempo = tempo_value(c, e);
        }
    }

    return tempo;
}

/* Channel scripts and their layers for the current section. channel_fix holds the addresses the
   sequence script uses to start each channel. */
static bool channel_scripts(Compile *c, Buffer *b, const size_t channel_fix[16]) {
    size_t layer_fix[16][4] = {{0}};
    Buffer streams[16] = {0};
    size_t stream_start[16] = {0};
    bool shared[16] = {0}, ok = true;

    for (unsigned ch = 0; ch < 16; ch++) {
        if (c->sources[ch] >= 0) {
            if (cancelled(c, 2, ch * 100 / 16)) {
                ok = false;
                break;
            }

            address(b, channel_fix[ch], b->size);
            put(b, 0xc4);
            put(b, 0xc1);
            put(b, c->sources[ch] == MIDI_DRUM_CHANNEL ? 127 : 0);
            put(b, 0xdb);
            put(b, 0);
            put(b, 0xdf);
            put(b, 100);
            put(b, 0xe0);
            put(b, 128);
            put(b, 0xdd);
            put(b, 64);
            put(b, 0xdc);
            put(b, 128);
            put(b, 0xde);
            put(b, 0x80);
            put(b, 0);
            put(b, 0xd4);
            put(b, 0);
            put(b, 0xd7);
            put(b, 8);
            put(b, 0xd8);
            put(b, 0);
            put(b, 0xd2);
            put(b, 0);

            for (unsigned l = 0; l < 4; l++) {
                bool used = false;

                for (size_t n = 0; n < c->count; n++) {
                    const Note *note = &c->notes[n];

                    if (note->output == ch && note->layer == l &&
                        (!c->region || note->section == c->emit_section)) {
                        used = true;
                        break;
                    }
                }

                if (used) {
                    put(b, 0x90 + l);
                    layer_fix[ch][l] = b->size;
                    put(b, 0);
                    put(b, 0);
                }
            }

            Buffer *stream = &streams[ch];

            if (!controller_stream(c, ch, stream)) {
                c->result->status = M64_NOMEM;
                ok = false;
                break;
            }

            size_t saving = 0, target = 0;

            for (unsigned prev = 0; prev < ch; prev++) {
                if (streams[prev].size && !shared[prev]) {
                    Buffer *other = &streams[prev];
                    size_t match = 0;

                    while (match < stream->size && match < other->size &&
                           stream->data[stream->size - 1 - match] ==
                               other->data[other->size - 1 - match]) {
                        match++;
                    }

                    size_t from = 0, to = 0;

                    while (from < stream->size - match) {
                        from = next_controller(stream, from);
                    }

                    while (to < other->size - match) {
                        to = next_controller(other, to);
                    }

                    while (stream->size - from > saving + 3 && other->size - to > saving + 3) {
                        size_t left = stream->size - from, right = other->size - to;

                        if (left > right) {
                            from = next_controller(stream, from);
                        } else if (right > left) {
                            to = next_controller(other, to);
                        } else {
                            if (stream_start[prev] + to <= UINT16_MAX) {
                                saving = left - 3;
                                target = stream_start[prev] + to;
                            }
                            break;
                        }
                    }
                }
            }

            stream_start[ch] = b->size;

            if (saving) {
                bytes(b, stream->data, stream->size - saving - 3);
                put(b, 0xfb);
                size_t fix = b->size;
                put(b, 0);
                put(b, 0);
                address(b, fix, target);
                shared[ch] = true;
            } else {
                bytes(b, stream->data, stream->size);
            }
        }
    }

    for (unsigned ch = 0; ch < 16; ch++) {
        free(streams[ch].data);
    }

    return ok && emit_layers(c, b, layer_fix);
}

static void start_channels(Buffer *b, unsigned mask, size_t fix[16]) {
    for (unsigned ch = 0; ch < 16; ch++) {
        if (mask & (1u << ch)) {
            put(b, 0x90 + ch);
            fix[ch] = b->size;
            put(b, 0);
            put(b, 0);
        }
    }
}

bool sequence(Compile *c) {
    Buffer *b = &c->result->files[M64_SEQUENCE];
    size_t intro_fix[16] = {0}, region_fix[16] = {0};
    unsigned mask = 0;

    for (unsigned ch = 0; ch < 16; ch++) {
        if (c->sources[ch] >= 0) {
            mask |= 1u << ch;
        }
    }

    put(b, 0xd3);
    put(b, 0x20); /* soften when the game mutes music */
    put(b, 0xd5);
    put(b, 0x32);
    put(b, 0xd0);
    put(b, 0); /* Explicit default note allocation policy. */
    put(b, 0xdf);
    put(b, 0); /* sequence transpose */
    put(b, 0xdb);
    put(b, 127);
    put(b, 0xdd);
    put(b, (unsigned)fmax(1, round(120 * c->tempo_scale)));
    put(b, 0xd7);
    put(b, mask >> 8);
    put(b, mask);
    unsigned last_tempo = (unsigned)fmax(1, round(120 * c->tempo_scale));
    bool intro = !c->region || c->loop_start > 0;

    if (intro) {
        c->emit_section = 0;
        start_channels(b, mask, intro_fix);
        uint64_t now = tempo_stream(c, b, &last_tempo);
        uint64_t end = c->region ? c->loop_start : c->duration + (c->request->loop ? 0 : 48);
        delay(b, 0xfd, end - now);
    }

    if (c->region) {
        /* Each pass of the region restarts its channels and restores its opening tempo. */
        size_t label = b->size;
        c->emit_section = 1;
        start_channels(b, mask, region_fix);
        last_tempo = region_tempo(c);
        put(b, 0xdd);
        put(b, last_tempo);
        uint64_t now = tempo_stream(c, b, &last_tempo);
        delay(b, 0xfd, c->loop_end - c->loop_start - now);
        put(b, 0xfb);
        size_t fix = b->size;
        put(b, 0);
        put(b, 0);
        address(b, fix, label);
    } else if (c->request->loop) {
        put(b, 0xfb);
        put(b, 0);
        put(b, 0);
    } else {
        put(b, 0xff);
    }

    bool ok = true;

    if (intro) {
        c->emit_section = 0;
        ok = channel_scripts(c, b, intro_fix);
    }

    if (ok && c->region) {
        c->emit_section = 1;
        ok = channel_scripts(c, b, region_fix);
    }

    if (!ok) {
        if (c->result->status == M64_OK) {
            c->result->status = M64_NOMEM;
        }

        return false;
    }

    if (b->failed) {
        c->result->status = M64_NOMEM;
        return false;
    }

    if (b->address_failed) {
        c->result->status = M64_LIMIT;
        return false;
    }

    return !cancelled(c, 2, 100);
}
