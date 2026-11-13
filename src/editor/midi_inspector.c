#include "editor/internal.h"
#include "text/en.h"

static const char *controller_names[128] = {[0] = DM_TEXT_BANK_SELECT_MSB,
                                            [1] = DM_TEXT_MODULATION,
                                            [2] = DM_TEXT_BREATH,
                                            [4] = DM_TEXT_FOOT_CONTROLLER,
                                            [5] = DM_TEXT_PORTAMENTO_TIME,
                                            [6] = DM_TEXT_DATA_ENTRY_MSB,
                                            [7] = DM_TEXT_VOLUME,
                                            [8] = DM_TEXT_BALANCE,
                                            [10] = DM_TEXT_PAN,
                                            [11] = DM_TEXT_EXPRESSION,
                                            [12] = DM_TEXT_EFFECT_CONTROL_1,
                                            [13] = DM_TEXT_EFFECT_CONTROL_2,
                                            [16] = DM_TEXT_GENERAL_PURPOSE_1,
                                            [17] = DM_TEXT_GENERAL_PURPOSE_2,
                                            [18] = DM_TEXT_GENERAL_PURPOSE_3,
                                            [19] = DM_TEXT_GENERAL_PURPOSE_4,
                                            [32] = DM_TEXT_BANK_SELECT_LSB,
                                            [33] = DM_TEXT_MODULATION_LSB,
                                            [37] = DM_TEXT_PORTAMENTO_TIME_LSB,
                                            [38] = DM_TEXT_DATA_ENTRY_LSB,
                                            [39] = DM_TEXT_VOLUME_LSB,
                                            [42] = DM_TEXT_PAN_LSB,
                                            [43] = DM_TEXT_EXPRESSION_LSB,
                                            [64] = DM_TEXT_SUSTAIN,
                                            [65] = DM_TEXT_PORTAMENTO_ENABLE,
                                            [66] = DM_TEXT_SOSTENUTO,
                                            [67] = DM_TEXT_SOFT_PEDAL,
                                            [68] = DM_TEXT_LEGATO,
                                            [69] = DM_TEXT_HOLD_2,
                                            [70] = DM_TEXT_SOUND_VARIATION,
                                            [71] = DM_TEXT_RESONANCE,
                                            [72] = DM_TEXT_RELEASE_TIME,
                                            [73] = DM_TEXT_ATTACK_TIME,
                                            [74] = DM_TEXT_BRIGHTNESS,
                                            [75] = DM_TEXT_DECAY_TIME,
                                            [76] = DM_TEXT_VIBRATO_RATE,
                                            [77] = DM_TEXT_VIBRATO_DEPTH,
                                            [78] = DM_TEXT_VIBRATO_DELAY,
                                            [84] = DM_TEXT_PORTAMENTO_SOURCE_KEY,
                                            [88] = DM_TEXT_HIGH_RESOLUTION_VELOCITY,
                                            [91] = DM_TEXT_REVERB_SEND,
                                            [92] = DM_TEXT_TREMOLO_EFFECT_2,
                                            [93] = DM_TEXT_CHORUS_SEND,
                                            [94] = DM_TEXT_EFFECT_4_DEPTH,
                                            [95] = DM_TEXT_EFFECT_5_DEPTH,
                                            [96] = DM_TEXT_DATA_INCREMENT,
                                            [97] = DM_TEXT_DATA_DECREMENT,
                                            [98] = DM_TEXT_NRPN_LSB,
                                            [99] = DM_TEXT_NRPN_MSB,
                                            [100] = DM_TEXT_RPN_LSB,
                                            [101] = DM_TEXT_RPN_MSB,
                                            [120] = DM_TEXT_SOUND_OFF,
                                            [121] = DM_TEXT_RESET_CONTROLLERS,
                                            [122] = DM_TEXT_LOCAL_CONTROL,
                                            [123] = DM_TEXT_NOTES_OFF,
                                            [124] = DM_TEXT_OMNI_OFF,
                                            [125] = DM_TEXT_OMNI,
                                            [126] = DM_TEXT_MONO_MODE,
                                            [127] = DM_TEXT_POLY_MODE};
static bool midi_format_bytes(Editor *e);
static const char *controller_name(int cc);
static void midi_event_list(Editor *e, size_t count);
static bool midi_selection_valid(Editor *e);
static void midi_meta_fields(Editor *e);
static void midi_event_inspector(Editor *e);

static size_t midi_first_at(const DmProject *p, bool automation, double beat) {
    size_t first = 0, last = automation ? p->automation_count : p->event_count;

    while (first < last) {
        size_t middle = first + (last - first) / 2;
        double at = automation ? p->automation[middle].beat : p->events[middle].beat;

        if (at < beat) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }

    return first;
}

size_t ed_midi_rows_between(Editor *e, double start, double end) {
    const DmProject *p = &e->project;
    size_t source = midi_first_at(p, false, start), source_end = midi_first_at(p, false, end);
    size_t automation = midi_first_at(p, true, start), automation_end = midi_first_at(p, true, end);
    size_t capacity = (source_end - source + automation_end - automation) * sizeof(MidiRow);
    e->midi_rows.size = 0;

    if (!capacity ||
        !dm_reserve((void **)&e->midi_rows.data, &e->midi_rows.capacity, capacity, 1)) {
        return 0;
    }

    MidiRow *rows = (MidiRow *)e->midi_rows.data;
    /* Source events retain precedence at an equal beat. */
    size_t count = 0;

    while (source < source_end || automation < automation_end) {
        if (source < source_end && (automation == automation_end ||
                                    p->events[source].beat <= p->automation[automation].beat)) {
            rows[count++] = (MidiRow){p->events[source].beat, (int)source, false};
            ++source;
        } else {
            rows[count++] = (MidiRow){p->automation[automation].beat, (int)automation, true};
            ++automation;
        }
    }

    e->midi_rows.size = count * sizeof(*rows);
    return count;
}

size_t ed_midi_rows(Editor *e) {
    return ed_midi_rows_between(e, 0, INFINITY);
}

static bool midi_format_bytes(Editor *e) {
    size_t capacity = e->midi_draft.size * 4 + 1024;

    if (!dm_reserve((void **)&e->midi_text.data, &e->midi_text.capacity, capacity, 1)) {
        return false;
    }

    size_t used = 0;

    for (size_t i = 0; i < e->midi_draft.size; ++i) {
        used +=
            (size_t)snprintf((char *)e->midi_text.data + used, capacity - used,
                             e->midi_hex ? "%s%02X" : "%s%u", i ? " " : "", e->midi_draft.data[i]);
    }

    e->midi_text.data[used] = 0;
    e->midi_text.size = used + 1;
    return true;
}

bool ed_midi_parse_bytes(Editor *e) {
    DmBytes next = {0};
    const char *p = (const char *)e->midi_text.data;

    while (p && *p) {
        while (isspace((unsigned char)*p) || *p == ',') {
            ++p;
        }

        if (!*p) {
            break;
        }

        char *end;
        errno = 0;
        unsigned long value = strtoul(p, &end, e->midi_hex ? 16 : 10);

        if (errno || end == p || *p == '-' || value > 255 ||
            (*end && !isspace((unsigned char)*end) && *end != ',')) {
            goto invalid;
        }

        uint8_t byte = (uint8_t)value;

        if (!dm_bytes(&next, &byte, 1)) {
            goto invalid;
        }

        p = end;
    }

    if (!dm_message_valid(&next)) {
        goto invalid;
    }

    dm_bytes_free(&e->midi_draft);
    e->midi_draft = next;
    e->midi_error[0] = 0;
    return true;
invalid:
    dm_bytes_free(&next);
    snprintf(e->midi_error, sizeof(e->midi_error), DM_TEXT_ENTER_ONE_VALID);
    return false;
}

void ed_midi_select(Editor *e, MidiRow row) {
    e->event_beat = row.beat;
    e->midi_selection_revision = e->export_revision;
    e->midi_event_kind = row.automation ? 1 : 0;
    e->midi_event_index = row.index;
    e->midi_sample_dirty = false;
    e->midi_error[0] = 0;
    dm_bytes_free(&e->midi_draft);

    if (row.automation) {
        DmAutomation a = e->project.automation[row.index];
        uint8_t bytes[] = {(uint8_t)(a.type | a.channel), (uint8_t)a.a, (uint8_t)a.b};
        dm_bytes(&e->midi_draft, bytes,
                 a.type == MIDI_PROGRAM || a.type == MIDI_CHANNEL_PRESSURE ? 2 : 3);
        e->midi_source_track = a.channel;
        e->midi_source_port = a.port;
    } else {
        DmEvent *source = &e->project.events[row.index];
        dm_bytes(&e->midi_draft, source->message.data, source->message.size);
        e->midi_source_track = source->track;
        e->midi_source_port = source->port;
    }

    if (!e->midi_draft.size) {
        e->midi_event_index = -1;
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_MIDI);
        return;
    }

    if (row.beat == 0 && (!row.automation || e->project.automation[row.index].pattern < 0)) {
        uint8_t *m = e->midi_draft.data;
        int c = m[0] & MIDI_CHANNEL_MASK;

        if ((m[0] & MIDI_STATUS_MASK) == MIDI_PROGRAM) {
            m[1] = (uint8_t)fmax(0, e->project.channels[c].program);
        }

        if ((m[0] & MIDI_STATUS_MASK) == MIDI_CONTROL && m[1] == MIDI_CC_VOLUME) {
            m[2] = e->project.channels[c].volume;
        }

        if ((m[0] & MIDI_STATUS_MASK) == MIDI_CONTROL && m[1] == MIDI_CC_PAN) {
            m[2] = e->project.channels[c].pan;
        }

        if (m[0] == MIDI_META && m[1] == MIDI_META_TEMPO) {
            int us = (int)llround(60000000.0 / e->project.bpm);
            m[3] = us >> 16;
            m[4] = us >> 8;
            m[5] = us;
        }

        if (m[0] == MIDI_META && m[1] == MIDI_META_TIME) {
            int power = 0;

            while ((1 << power) < e->project.denominator) {
                ++power;
            }

            m[3] = e->project.numerator;
            m[4] = power;
        }
    }

    if (!e->song_event && !midi_format_bytes(e)) {
        e->midi_event_index = -1;
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_MIDI);
    }
}

static const char *controller_name(int cc) {
    return cc >= 0 && cc < 128 && controller_names[cc] ? controller_names[cc]
                                                       : DM_TEXT_DEVICE_SPECIFIC_CONTROLLER;
}

const char *ed_midi_row_name(Editor *e, MidiRow row) {
    static char label[160];
    uint8_t status, first;

    if (row.automation) {
        DmAutomation a = e->project.automation[row.index];
        status = a.type | a.channel;
        first = a.a;
    } else {
        DmBytes *m = &e->project.events[row.index].message;
        if (m->size < 2 || m->data[0] >= MIDI_SYSTEM) {
            return ed_event_label(m);
        }

        status = m->data[0];
        first = m->data[1];
    }

    switch (status & MIDI_STATUS_MASK) {
    case MIDI_NOTE_OFF:
        return DM_TEXT_NOTE_RELEASE;
    case MIDI_NOTE_ON:
        return DM_TEXT_NOTE_ATTACK;
    case MIDI_KEY_PRESSURE:
        return DM_TEXT_POLYPHONIC_PRESSURE;
    case MIDI_CONTROL:
        snprintf(label, sizeof(label), "%s", controller_name(first));
        return label;
    case MIDI_PROGRAM:
        snprintf(label, sizeof(label), DM_TEXT_INSTRUMENT_LABEL, dm_program_name(first));
        return label;
    case MIDI_CHANNEL_PRESSURE:
        return DM_TEXT_CHANNEL_PRESSURE;
    case MIDI_PITCH_BEND:
        return DM_TEXT_PITCH_BEND;
    default:
        return DM_TEXT_MIDI_EVENT;
    }
}

static void midi_event_list(Editor *e, size_t count) {
    MidiRow *rows = (MidiRow *)e->midi_rows.data;
    UiPoint origin = ui_cursor();
    float step = 36, scroll = ui_scroll_value().y;
    size_t first = (size_t)fmax(0, floorf(scroll / step));
    size_t last = (size_t)fmin((double)count, first + ceilf(ui_window_rect().h / step) + 3);
    ui_position(origin.x, origin.y + first * step);

    for (size_t i = first; i < last; ++i) {
        MidiRow row = rows[i];
        char label[256];
        snprintf(label, sizeof(label), DM_TEXT_EVENT_ROW_ID, ed_midi_row_name(e, row), row.beat + 1,
                 row.automation, row.index);
        bool selected =
            e->midi_event_index == row.index && e->midi_event_kind == (int)row.automation;

        if (e->song_event && !e->song_event_add && ed_song_event_pick_count(e)) {
            selected = ed_song_event_picked(e, row);
        }

        if (ui_select(label, selected, 0, 28)) {
            if (e->song_event && !e->song_event_add) {
                ed_song_event_pick(e, i, ed_select_pick(e, ui_mouse()));
            } else {
                ed_midi_select(e, row);
            }
        }
    }

    ui_position(origin.x, origin.y + count * step);
    ui_space(1);
}

static bool midi_selection_valid(Editor *e) {
    if (e->midi_event_index < 0 || e->midi_selection_revision != e->export_revision) {
        return false;
    }

    size_t i = (size_t)e->midi_event_index;
    return e->midi_event_kind ? i < e->project.automation_count : i < e->project.event_count;
}

size_t ed_midi_payload_offset(const DmBytes *m) {
    size_t at = m->data[0] == MIDI_META ? 2 : 1;

    while (at < m->size && (m->data[at++] & 128)) {
    }

    return at;
}

bool ed_midi_replace_payload(Editor *e, const uint8_t *payload, size_t size) {
    uint8_t length[4];
    size_t count = 0, n = size;

    if (size > 0x0fffffff) {
        return false;
    }

    do {
        length[count++] = (uint8_t)(n & 127);
        n >>= 7;
    } while (n);
    DmBytes next = {0};
    size_t prefix = e->midi_draft.data[0] == MIDI_META ? 2 : 1;
    bool ok = dm_bytes(&next, e->midi_draft.data, prefix);

    while (count && ok) {
        --count;
        uint8_t byte = length[count] | (count ? 128 : 0);
        ok = dm_bytes(&next, &byte, 1);
    }

    ok = ok && dm_bytes(&next, payload, size);

    if (!ok) {
        dm_bytes_free(&next);
        return false;
    }

    dm_bytes_free(&e->midi_draft);
    e->midi_draft = next;
    return true;
}

/* Named fields for a channel message, shared by the MIDI Events and song event windows. */
static void midi_channel_fields(Editor *e, uint8_t *m, size_t size) {
    int type = m[0] & MIDI_STATUS_MASK, channel = (m[0] & MIDI_CHANNEL_MASK) + 1;

    bool bound = e->song_event && e->song_event_channel >= 0;
    /* The compact Add form keeps only the fields needed to choose the event. */
    bool adding = e->song_event && e->song_event_add;
    ui_disabled(bound);

    if (ui_int(adding ? DM_TEXT_EVENT_CHANNEL : DM_TEXT_MIDI_CHANNEL, &channel, 1, 16)) {
        m[0] = type | (channel - 1);
    }

    ui_disabled_end();

    if (type == MIDI_PROGRAM) {
        bool drums = dm_channel_drums(&e->project, channel - 1);
        const char *current = drums ? dm_drum_kit_name(m[1]) : dm_program_name(m[1]);

        if (ui_combo(DM_TEXT_INSTRUMENT, current ? current : DM_TEXT_DRUM_KIT)) {
            for (int i = 0; i < 128; ++i) {
                const char *name = drums ? dm_drum_kit_name(i) : dm_program_name(i);

                if (name && ui_select(name, m[1] == i, 0, 0)) {
                    m[1] = (uint8_t)i;
                }
            }

            ui_combo_end();
        }

        if (!adding) {
            char number[80];
            snprintf(number, sizeof(number), DM_TEXT_PROGRAM_MIDI_VALUE, m[1] + 1, m[1]);
            ui_dim(number);
        }

        ed_sample_program_source(e);
    } else if (type == MIDI_CONTROL) {
        char label[128];
        snprintf(label, sizeof(label), "%s", controller_name(m[1]));

        /* Song events hold only Volume and Pan controllers; another number would be hidden. */
        ui_disabled(e->song_event);

        if (ui_combo(DM_TEXT_CONTROLLER, label)) {
            for (int i = 0; i < 128; ++i) {
                if (!controller_names[i]) {
                    continue;
                }

                snprintf(label, sizeof(label), "%s", controller_name(i));

                if (ui_select(label, m[1] == i, 0, 0)) {
                    m[1] = (uint8_t)i;
                }
            }

            ui_combo_end();
        }

        ui_disabled_end();
        int cc = m[1], value = m[2];

        if ((cc >= 64 && cc <= 69) || cc == 122) {
            bool on = value >= 64;

            if (ui_check(DM_TEXT_ENABLED, &on)) {
                m[2] = on ? 127 : 0;
            }

            ui_dim(on ? DM_TEXT_ON_127 : DM_TEXT_OFF_0_LABEL);
        } else {
            const char *name = cc == 10               ? DM_TEXT_PAN_64_CENTER
                               : cc == 5              ? DM_TEXT_GLIDE_TIME
                               : cc == 7              ? DM_TEXT_CHANNEL_VOLUME
                               : cc == 11             ? DM_TEXT_EXPRESSION
                               : cc == 91 || cc == 93 ? DM_TEXT_SEND_AMOUNT
                               : cc == 126            ? DM_TEXT_MONO_CHANNEL_COUNT
                                                      : DM_TEXT_CONTROLLER_VALUE;

            if (ui_int(name, &value, 0, 127)) {
                m[2] = (uint8_t)value;
            }

            if (cc == 10) {
                ui_dim(DM_TEXT_0_LEFT_64);
            }

            if (cc >= 70 && cc <= 79) {
                ui_dim(DM_TEXT_RESPONSE_DEPENDS_INSTRUMENT);
            }

            if (cc >= 98 && cc <= 101) {
                ui_dim(DM_TEXT_SELECTS_PARAMETER_SUBSEQUENT);
            }

            if (cc == 96 || cc == 97 || cc == 120 || cc == 121 || cc == 123 || cc >= 124) {
                ui_dim(DM_TEXT_COMMAND_MODE_MESSAGE);
            }
        }
    } else if (type == MIDI_PITCH_BEND) {
        int bend = m[1] | (m[2] << 7);

        if (ui_int(DM_TEXT_PITCH_BEND, &bend, 0, MIDI_BEND_MAX)) {
            m[1] = bend & 127;
            m[2] = bend >> 7;
        }

        ui_dim(DM_TEXT_8192_NEUTRAL_SEMITONE);
    } else {
        int first = m[1], second = size > 2 ? m[2] : 0;

        if (type == MIDI_CHANNEL_PRESSURE) {
            if (ui_int(DM_TEXT_CHANNEL_PRESSURE, &first, 0, 127)) {
                m[1] = (uint8_t)first;
            }
        } else {
            if (ui_int(DM_TEXT_NOTE_KEY, &first, 0, 127)) {
                m[1] = (uint8_t)first;
            }

            char note[32];
            ed_pitch_text(note, sizeof(note), (float)first);
            ui_dim(note);
            const char *name = type == MIDI_KEY_PRESSURE ? DM_TEXT_KEY_PRESSURE
                               : type == MIDI_NOTE_OFF   ? DM_TEXT_RELEASE_VELOCITY
                                                         : DM_TEXT_ATTACK_VELOCITY;

            if (ui_int(name, &second, 0, 127)) {
                m[2] = (uint8_t)second;
            }

            if (type == MIDI_NOTE_ON && !second) {
                ui_dim(DM_TEXT_VELOCITY_0_RELEASES);
            }
        }
    }
}

static void midi_smpte_fields(uint8_t *payload) {
    static const char *rates[] = {DM_TEXT_SMPTE_24, DM_TEXT_SMPTE_25, DM_TEXT_SMPTE_29,
                                  DM_TEXT_SMPTE_30};
    static const int frame_counts[] = {24, 25, 30, 30};
    static const char *labels[] = {DM_TEXT_MINUTES, DM_TEXT_SECONDS, DM_TEXT_FRAMES,
                                   DM_TEXT_SUBFRAMES};
    int rate = (payload[0] >> 5) & 3;

    if (ui_combo(DM_TEXT_SMPTE_RATE, rates[rate])) {
        for (int i = 0; i < 4; ++i) {
            if (ui_select(rates[i], rate == i, 0, 0)) {
                rate = i;
                payload[0] = (uint8_t)((payload[0] & 31) | (rate << 5));

                if (payload[3] >= frame_counts[rate]) {
                    payload[3] = (uint8_t)(frame_counts[rate] - 1);
                }
            }
        }

        ui_combo_end();
    }

    int hours = payload[0] & 31;

    if (ui_int(DM_TEXT_SMPTE_HOURS, &hours, 0, 23)) {
        payload[0] = (uint8_t)((rate << 5) | hours);
    }

    int maxima[] = {59, 59, frame_counts[rate] - 1, 99};

    for (int i = 0; i < 4; ++i) {
        int value = payload[i + 1];

        if (ui_int(labels[i], &value, 0, maxima[i])) {
            payload[i + 1] = (uint8_t)value;
        }
    }
}

static void midi_meta_fields(Editor *e) {
    DmBytes *draft = &e->midi_draft;
    uint8_t *m = draft->data;
    size_t offset = ed_midi_payload_offset(draft), size = draft->size - offset;
    uint8_t *payload = m + offset;

    if (m[0] != MIDI_META) {
        ui_text(m[0] == MIDI_SYSEX ? DM_TEXT_SYSTEM_EXCLUSIVE
                                   : DM_TEXT_SYSTEM_EXCLUSIVE_CONTINUATION);
        ui_dim(DM_TEXT_DEVICE_SPECIFIC_MESSAGE);
        return;
    }

    if (!e->song_event || !e->song_event_add) {
        ui_text(ed_event_label(draft));
    }

    if (e->song_event && dm_loop_marker(draft)) {
        ui_wrap(DM_TEXT_LOOP_POINT_HELP);
    } else if (m[1] >= MIDI_META_TEXT && m[1] <= 9) {
        size_t capacity = size + 4096;

        if (!dm_reserve((void **)&e->midi_text.data, &e->midi_text.capacity, capacity, 1)) {
            ui_dim(DM_TEXT_CANNOT_ALLOCATE_MIDI);
            return;
        }

        char *text = (char *)e->midi_text.data;
        memcpy(text, payload, size);
        text[size] = 0;

        if (ui_multiline("##Event text", text, e->midi_text.capacity, 90)) {
            ed_midi_replace_payload(e, (const uint8_t *)text, strlen(text));
        }
    } else if (m[1] == MIDI_META_TEMPO && size == 3) {
        int us = (payload[0] << 16) | (payload[1] << 8) | payload[2];
        float bpm = (float)(60000000.0 / us);

        if (ui_float(DM_TEXT_BPM, &bpm, DM_BPM_MIN, DM_BPM_MAX)) {
            us = (int)llround(60000000.0 / bpm);
            payload[0] = us >> 16;
            payload[1] = us >> 8;
            payload[2] = us;
        }
    } else if (m[1] == MIDI_META_TIME && size == 4) {
        int numerator = payload[0];

        if (ui_int(DM_TEXT_NUMERATOR, &numerator, 1, 255)) {
            payload[0] = (uint8_t)numerator;
        }

        char denominator[16];
        snprintf(denominator, sizeof(denominator), "%d", 1 << payload[1]);

        if (ui_combo(DM_TEXT_DENOMINATOR, denominator)) {
            for (int i = 0; i < 8; ++i) {
                snprintf(denominator, sizeof(denominator), "%d", 1 << i);

                if (ui_select(denominator, payload[1] == i, 0, 0)) {
                    payload[1] = (uint8_t)i;
                }
            }

            ui_combo_end();
        }

        int ticks = payload[2], units = payload[3];

        if (ui_int(DM_TEXT_METRONOME_TICKS, &ticks, 0, 255)) {
            payload[2] = (uint8_t)ticks;
        }

        if (ui_int(DM_TEXT_32NDS_PER_QUARTER, &units, 0, 255)) {
            payload[3] = (uint8_t)units;
        }
    } else if (m[1] == MIDI_META_KEY && size == 2) {
        int sharps = (int8_t)payload[0];

        if (ui_int(DM_TEXT_SHARPS_FLATS, &sharps, -7, 7)) {
            payload[0] = (uint8_t)(int8_t)sharps;
        }

        if (ui_combo(DM_TEXT_SCALE, payload[1] ? DM_TEXT_MINOR : DM_TEXT_MAJOR)) {
            if (ui_select(DM_TEXT_MAJOR, !payload[1], 0, 0)) {
                payload[1] = 0;
            }

            if (ui_select(DM_TEXT_MINOR, payload[1] == 1, 0, 0)) {
                payload[1] = 1;
            }

            ui_combo_end();
        }
    } else if ((m[1] == 0x20 || m[1] == MIDI_META_PORT) && size == 1) {
        int value = payload[0] + (m[1] == 0x20);

        if (ui_int(m[1] == 0x20 ? DM_TEXT_CHANNEL_PREFIX : DM_TEXT_MIDI_PORT, &value,
                   m[1] == 0x20 ? 1 : 0, m[1] == 0x20 ? 16 : 255)) {
            payload[0] = (uint8_t)(value - (m[1] == 0x20));
        }
    } else if (m[1] == 0x00 && size == 2) {
        int sequence = (payload[0] << 8) | payload[1];

        if (ui_int(DM_TEXT_SEQUENCE_NUMBER, &sequence, 0, 65535)) {
            payload[0] = sequence >> 8;
            payload[1] = sequence;
        }
    } else if (m[1] == MIDI_META_SMPTE && size == 5) {
        midi_smpte_fields(payload);
    } else {
        ui_dim(e->song_event ? DM_TEXT_SONG_EVENT_EDIT_MIDI : DM_TEXT_USE_MIDI_VALUES);
    }
}

void ed_midi_event_fields(Editor *e) {
    if (!e->midi_draft.size) {
        return;
    }

    if (e->midi_draft.data[0] < MIDI_SYSTEM) {
        midi_channel_fields(e, e->midi_draft.data, e->midi_draft.size);
    } else {
        midi_meta_fields(e);
    }
}

void ed_midi_apply_initial(Editor *e, const uint8_t *m) {
    int type = m[0] & MIDI_STATUS_MASK;

    if (type == MIDI_PROGRAM) {
        e->project.channels[m[0] & MIDI_CHANNEL_MASK].program = m[1];
    }

    if (type == MIDI_CONTROL && m[1] == MIDI_CC_VOLUME) {
        e->project.channels[m[0] & MIDI_CHANNEL_MASK].volume = m[2];
    }

    if (type == MIDI_CONTROL && m[1] == MIDI_CC_PAN) {
        e->project.channels[m[0] & MIDI_CHANNEL_MASK].pan = m[2];
    }

    if (m[0] == MIDI_META && m[1] == MIDI_META_TEMPO) {
        e->project.bpm = 60000000.0 / ((m[3] << 16) | (m[4] << 8) | m[5]);
    }

    if (m[0] == MIDI_META && m[1] == MIDI_META_TIME) {
        e->project.numerator = m[3];
        e->project.denominator = 1 << m[4];
    }
}

bool ed_midi_apply_draft(Editor *e) {
    if (!midi_selection_valid(e) || !isfinite(e->event_beat) || e->event_beat < 0 ||
        e->midi_source_track < 0 || e->midi_source_track > 65535 || e->midi_source_port < 0 ||
        e->midi_source_port > 255) {
        return false;
    }

    if (e->song_event && !ed_song_event_position_valid(e)) {
        return false;
    }

    if (!e->song_event && e->midi_values && !ed_midi_parse_bytes(e)) {
        return false;
    }

    DmBytes *draft = &e->midi_draft;

    if (!dm_message_valid(draft)) {
        return false;
    }

    const uint8_t *m = draft->data;
    int type = m[0] & MIDI_STATUS_MASK;
    bool initial = !e->midi_event_kind || e->project.automation[e->midi_event_index].pattern < 0;

    if (e->midi_event_kind && (type < MIDI_KEY_PRESSURE || type > MIDI_PITCH_BEND)) {
        snprintf(e->midi_error, sizeof(e->midi_error), DM_TEXT_AUTOMATION_REQUIRES_CONTROLLER);
        return false;
    }

    DmBytes copy = {0};

    if (!e->midi_event_kind && !dm_bytes(&copy, draft->data, draft->size)) {
        return false;
    }

    if (!ed_checkpoint(e)) {
        dm_bytes_free(&copy);
        return false;
    }

    if (e->song_event) {
        dm_collapse_program_repeats(&e->project, e->midi_event_index, e->midi_event_kind);
    }

    if (e->midi_event_kind) {
        DmAutomation *a = &e->project.automation[e->midi_event_index];
        a->beat = e->event_beat;
        a->type = type;
        a->channel = m[0] & MIDI_CHANNEL_MASK;
        a->a = m[1];
        a->b = draft->size > 2 ? m[2] : 0;
        ed_sort_automation(e);
    } else {
        DmEvent *event = &e->project.events[e->midi_event_index];
        dm_bytes_free(&event->message);
        event->message = copy;
        event->beat = e->event_beat;
        event->track = e->midi_source_track;
        event->port = e->midi_source_port;

        for (size_t i = 1; i < e->project.event_count; ++i) {
            DmEvent item = e->project.events[i];
            size_t at = i;

            while (at && e->project.events[at - 1].beat > item.beat) {
                e->project.events[at] = e->project.events[at - 1];
                --at;
            }

            e->project.events[at] = item;
        }
    }

    if (initial && e->event_beat == 0) {
        ed_midi_apply_initial(e, m);
    }

    if (e->song_event) {
        e->song_event_anchor = e->event_beat;
    }

    ed_commit(e);
    e->midi_event_index = -1;
    return true;
}

static void midi_event_inspector(Editor *e) {
    if (e->song_event && !e->song_event_add && ed_song_event_pick_count(e) > 1) {
        ed_song_events_move_panel(e);
        return;
    }

    if (!midi_selection_valid(e)) {
        e->midi_event_index = -1;
        ui_dim(DM_TEXT_SELECT_MIDI_EVENT);
        return;
    }

    ed_song_event_position(e, &e->event_beat, DM_TEXT_BEAT, 0);
    ui_separator();

    if (e->song_event) {
        ed_midi_event_fields(e);
    } else {
        bool raw = e->midi_values;

        if (ui_check(DM_TEXT_MIDI_VALUES, &raw)) {
            if (raw) {
                if (midi_format_bytes(e)) {
                    e->midi_values = true;
                }
            } else if (ed_midi_parse_bytes(e)) {
                e->midi_values = false;
            }
        }

        if (e->midi_values) {
            bool hex = e->midi_hex;

            if (ui_check(DM_TEXT_HEXADECIMAL, &hex) && ed_midi_parse_bytes(e)) {
                e->midi_hex = hex;
                midi_format_bytes(e);
            }

            ui_dim(e->midi_hex ? DM_TEXT_HEX_BYTES_SEPARATED : DM_TEXT_DECIMAL_BYTES_SEPARATED);
            ui_multiline("##Message bytes", (char *)e->midi_text.data, e->midi_text.capacity, 100);
            ui_dim(DM_TEXT_INCLUDES_STATUS_BYTE);
        } else {
            ed_midi_event_fields(e);
        }
    }

    if (*e->midi_error) {
        ui_text(e->midi_error);
    }

    /* Events keep their imported source track unchanged; it is not shown or edited. */
    if (!e->midi_event_kind) {
        if (!e->song_event) {
            ui_int(DM_TEXT_SOURCE_MIDI_PORT, &e->midi_source_port, 0, 255);
        }
    } else {
        DmAutomation a = e->project.automation[e->midi_event_index];

        if (a.pattern >= 0) {
            ui_dim(e->project.patterns[a.pattern].name);
        }
    }

    if (ui_button(DM_TEXT_APPLY_EVENT, 120, 0) && ed_midi_apply_draft(e)) {
        return;
    }

    if (ui_button(DM_TEXT_DELETE_EVENT, 120, 0)) {
        if (ed_checkpoint(e)) {
            size_t i = (size_t)e->midi_event_index;

            if (e->song_event) {
                dm_collapse_program_repeats(&e->project, (int)i, e->midi_event_kind);
            }

            if (e->midi_event_kind) {
                memmove(e->project.automation + i, e->project.automation + i + 1,
                        (e->project.automation_count - i - 1) * sizeof(DmAutomation));
                --e->project.automation_count;
            } else {
                dm_bytes_free(&e->project.events[i].message);
                memmove(e->project.events + i, e->project.events + i + 1,
                        (e->project.event_count - i - 1) * sizeof(DmEvent));
                --e->project.event_count;
            }

            e->midi_error[0] = 0;
            ed_commit(e);
            e->midi_event_index = -1;
        }
    }
}

void ed_midi_event_panels(Editor *e, size_t count, float *split, float height) {
    UiPoint size = ui_available();

    if (height > 0) {
        size.y = height;
    }
    /* The split is the list's share of the panels' width, so it follows the window's size and
       is not set from the narrow width of a frame in which the window is being measured. */
    float panels = size.x - 8 - ui_item_spacing_x() * 2;

    if (*split <= 0 || *split >= 1) {
        *split = .5f;
    }

    float left = ed_clampf(*split * panels, 130, fmaxf(130, size.x - 260));
    ui_child(DM_TEXT_MIDI_EVENT_LIST, left, size.y, true);

    if (e->song_event && !count) {
        ui_dim(e->song_event_channel < 0 ? DM_TEXT_SONG_EVENT_EMPTY : DM_TEXT_CHANNEL_EVENT_EMPTY);
    } else {
        midi_event_list(e, count);
    }
    ui_child_end();
    ui_same();
    if (ui_splitter(&left, 130, fmaxf(130, size.x - 260), size.y) && panels > 0) {
        *split = left / panels;
    }

    ui_same();
    ui_child(DM_TEXT_MIDI_EVENT_DETAILS, 0, size.y, true);
    midi_event_inspector(e);
    ui_child_end();
}

void ed_midi_event_window(Editor *e) {
    if (!e->midi_events) {
        return;
    }

    size_t count = ed_midi_rows(e);

    /* First opened at 60% of the window height, with the list and details panels of equal width
       around the centered divider. */
    float h = ui_display().y * .6f;

    if (ui_dialog(DM_TEXT_MIDI_EVENTS, &e->midi_events,
                  (UiDialog){.width = 1000, .height = h, .size = UI_RESIZABLE})) {
        ed_midi_event_panels(e, count, &e->midi_split, 0);
    }

    ui_dialog_end();
}
