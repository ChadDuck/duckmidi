#include "editor/internal.h"
#include "text/en.h"

typedef enum {
    SONG_TEMPO,
    SONG_METER,
    SONG_TEXT,
    SONG_TRACK_NAME,
    SONG_SMPTE,
    SONG_KEY,
    SONG_MARKER,
    SONG_PROGRAM,
    SONG_LOOP_START,
    SONG_LOOP_END,
    SONG_KIND_COUNT,
} SongEventKind;

static const char *const song_event_names[] = {
    DM_TEXT_TEMPO,          DM_TEXT_METER,         DM_TEXT_TEXT,   DM_TEXT_TRACK_NAME,
    DM_TEXT_SMPTE_OFFSET,   DM_TEXT_KEY_SIGNATURE, DM_TEXT_MARKER, DM_TEXT_PROGRAM_CHANGE,
    DM_TEXT_M64_LOOP_START, DM_TEXT_M64_LOOP_END,
};

double ed_song_event_bar(const Editor *e, double beat, double *start, double *end) {
    DmMeter meter = dm_meter(&e->project, beat);
    double bar = floor((beat - meter.origin) / meter.bar);
    *start = meter.origin + bar * meter.bar;

    /* Subtraction can round a beat at a fractional meter origin across a boundary. */
    if (*start > beat) {
        --bar;
        *start = meter.origin + bar * meter.bar;
    }

    *end = fmin(meter.origin + (bar + 1) * meter.bar, meter.next_change);

    if (*end <= beat) {
        ++bar;
        *start = *end;
        *end = fmin(meter.origin + (bar + 1) * meter.bar, meter.next_change);
    }

    return meter.completed + bar + 1;
}

static bool song_event_row(const Editor *e, MidiRow row) {
    if (row.automation) {
        DmAutomation a = e->project.automation[row.index];
        return a.type == MIDI_PROGRAM && a.pattern < 0;
    }

    const DmBytes *m = &e->project.events[row.index].message;

    if (m->size >= 2 && m->data[0] == MIDI_META) {
        switch (m->data[1]) {
        case MIDI_META_TEMPO:
        case MIDI_META_TIME:
        case MIDI_META_TEXT:
        case MIDI_META_TRACK_NAME:
        case MIDI_META_SMPTE:
        case MIDI_META_KEY:
        case MIDI_META_MARKER:
            return true;
        }
    }

    return m->size == 2 && (m->data[0] & MIDI_STATUS_MASK) == MIDI_PROGRAM;
}

size_t ed_song_event_rows(Editor *e, double start, double end) {
    size_t count = ed_midi_rows_between(e, start, end);
    MidiRow *rows = (MidiRow *)e->midi_rows.data;
    size_t selected = 0;
    DmProgramHistory history[DM_PORT_COMBINED][16] = {0};

    for (size_t i = 0; i < count; ++i) {
        MidiRow row = rows[i];
        int prior = dm_project_program_repeat(history, &e->project, row.index, row.automation);

        if (prior < 0 && song_event_row(e, row)) {
            rows[selected++] = row;
        }
    }

    e->midi_rows.size = selected * sizeof(*rows);
    return selected;
}

static void song_event_add_draft(Editor *e, SongEventKind kind) {
    uint8_t data[32] = {MIDI_META, 0, 0};
    static const uint8_t types[] = {
        [SONG_TEXT] = MIDI_META_TEXT,     [SONG_TRACK_NAME] = MIDI_META_TRACK_NAME,
        [SONG_SMPTE] = MIDI_META_SMPTE,   [SONG_KEY] = MIDI_META_KEY,
        [SONG_MARKER] = MIDI_META_MARKER,
    };
    size_t size = 3;

    switch (kind) {
    case SONG_TEMPO: {
        int us = (int)llround(60000000.0 / e->project.bpm);
        uint8_t tempo[] = {MIDI_META, MIDI_META_TEMPO, 3, us >> 16, us >> 8, us};
        memcpy(data, tempo, sizeof(tempo));
        size = sizeof(tempo);
        break;
    }
    case SONG_METER: {
        int power = 0;

        while ((1 << power) < e->project.denominator) {
            ++power;
        }

        uint8_t meter[] = {MIDI_META, MIDI_META_TIME, 4, e->project.numerator, power, 24, 8};
        memcpy(data, meter, sizeof(meter));
        size = sizeof(meter);
        break;
    }
    case SONG_PROGRAM: {
        int channel = e->selected_channel;
        data[0] = MIDI_PROGRAM | channel;
        data[1] = (uint8_t)fmax(0, e->project.channels[channel].program);
        size = 2;
        break;
    }
    case SONG_LOOP_START:
    case SONG_LOOP_END: {
        const char *text = kind == SONG_LOOP_START ? DM_LOOP_START_MARKER : DM_LOOP_END_MARKER;
        size_t length = strlen(text);
        data[1] = MIDI_META_MARKER;
        data[2] = (uint8_t)length;
        memcpy(data + 3, text, length);
        size += length;
        break;
    }
    default:
        data[1] = types[kind];
        data[2] = kind == SONG_SMPTE ? 5 : kind == SONG_KEY ? 2 : 0;
        size += data[2];
        break;
    }

    e->midi_draft.size = 0;
    e->midi_error[0] = 0;
    e->midi_sample_dirty = false;
    e->song_event_kind = kind;

    if (!dm_bytes(&e->midi_draft, data, size)) {
        snprintf(e->midi_error, sizeof(e->midi_error), DM_TEXT_CANNOT_ALLOCATE_MIDI);
    }
}

static void song_event_mode(Editor *e, bool add) {
    e->song_event_add = add;
    e->midi_event_index = -1;
    e->midi_error[0] = 0;
    e->event_beat = e->song_event_anchor;

    if (add) {
        song_event_add_draft(e, SONG_TEMPO);
        return;
    }

    double start, end;
    ed_song_event_bar(e, e->song_event_anchor, &start, &end);

    if (ed_song_event_rows(e, start, end)) {
        ed_midi_select(e, ((MidiRow *)e->midi_rows.data)[0]);
    }
}

void ed_song_event_open_bar(Editor *e, double beat, bool add) {
    e->midi_events = false;
    e->song_event = true;
    e->song_event_bound = true;
    e->song_event_anchor = beat;
    song_event_mode(e, add);
}

void ed_new_song_event(Editor *e) {
    ed_song_event_open_bar(e, e->cursor_beat[0], true);
    e->song_event_bound = false;
}

bool ed_song_event_position_valid(Editor *e) {
    if (!isfinite(e->event_beat) || e->event_beat < 0) {
        return false;
    }

    if (e->song_event_bound || !e->song_event_add) {
        double start, end;
        ed_song_event_bar(e, e->song_event_anchor, &start, &end);

        if (e->event_beat < start || e->event_beat >= end) {
            snprintf(e->midi_error, sizeof(e->midi_error), DM_TEXT_SONG_EVENT_BEAT_RANGE);
            return false;
        }
    }

    e->midi_error[0] = 0;
    return true;
}

static void replace_loop_point(DmProject *p, double beat, int kind) {
    /* Insertion follows existing events at the same beat. Keep the new loop point. */
    size_t added = p->event_count - 1;

    while (p->events[added].beat != beat || dm_loop_marker(&p->events[added].message) != kind) {
        --added;
    }

    size_t kept = 0;

    for (size_t i = 0; i < p->event_count; ++i) {
        if (i != added && dm_loop_marker(&p->events[i].message) == kind) {
            dm_bytes_free(&p->events[i].message);
        } else {
            p->events[kept++] = p->events[i];
        }
    }

    p->event_count = kept;
}

bool ed_song_event_add(Editor *e) {
    if (!ed_song_event_position_valid(e) || !dm_message_valid(&e->midi_draft)) {
        return false;
    }

    const DmBytes *m = &e->midi_draft;
    double beat = e->event_beat;
    bool pushed = ed_checkpoint(e);
    bool saved;

    if (e->song_event_kind == SONG_PROGRAM) {
        DmAutomation a = {.beat = beat,
                          .type = MIDI_PROGRAM,
                          .channel = m->data[0] & MIDI_CHANNEL_MASK,
                          .a = m->data[1],
                          .pattern = -1};
        saved = dm_automation_add(&e->project, a);
    } else {
        saved = dm_event_add(&e->project, beat, 0, 0, m);
    }

    if (!saved) {
        ed_checkpoint_abandon(e, pushed);
        snprintf(e->midi_error, sizeof(e->midi_error), DM_TEXT_SONG_EVENT_SAVE_FAILED);
        return false;
    }

    int loop = dm_loop_marker(m);

    if (loop) {
        replace_loop_point(&e->project, beat, loop);
    }

    if (beat == 0) {
        ed_midi_apply_initial(e, m->data);
    }

    ed_commit(e);

    if (e->song_event_bound) {
        e->song_event_anchor = beat;
        song_event_mode(e, false);
    } else {
        e->song_event = false;
    }

    return true;
}

void ed_song_event_position(Editor *e) {
    float beat = (float)(e->event_beat + 1);

    if (ui_float(DM_TEXT_BEAT, &beat, 1, 1000001)) {
        e->event_beat = beat - 1;
    }

    ui_same();

    if (ui_button(DM_TEXT_USE_PLAYHEAD, 0, 0)) {
        e->event_beat = !e->pattern_mode && dm_audio_playing(e->audio)
                            ? dm_seconds_beat(&e->project, dm_audio_position(e->audio))
                            : e->cursor_beat[0];
    }
}

void ed_song_event_dialog(Editor *e) {
    if (!e->song_event) {
        return;
    }

    /* Add fits its fields; Edit's panels take the spare height, first 60% of the window. */
    float height = ui_display().y * .6f;

    if (ui_flexible_dialog(DM_TEXT_SONG_EVENT, &e->song_event, fminf(1000, ui_display().x - 40),
                           height, false, e->song_event_add)) {
        ui_width(140);

        if (ui_combo(DM_TEXT_SONG_EVENT_MODE,
                     e->song_event_add ? DM_TEXT_SONG_EVENT_ADD : DM_TEXT_EDIT)) {
            if (ui_select(DM_TEXT_SONG_EVENT_ADD, e->song_event_add, 0, 0) && !e->song_event_add) {
                song_event_mode(e, true);
            }

            if (ui_select(DM_TEXT_EDIT, !e->song_event_add, 0, 0) && e->song_event_add) {
                if (!e->song_event_bound) {
                    e->song_event_anchor = e->event_beat;
                }

                song_event_mode(e, false);
            }

            ui_combo_end();
        }

        double start, end;
        double bar = ed_song_event_bar(e, e->song_event_anchor, &start, &end);

        if (e->song_event_bound || !e->song_event_add) {
            char label[48];
            snprintf(label, sizeof(label), DM_TEXT_SONG_EVENT_BAR, bar);
            ui_same();
            ui_dim(label);
        }

        if (e->song_event_add) {
            ed_song_event_position(e);
            ui_width(240);

            if (ui_combo(DM_TEXT_TYPE, song_event_names[e->song_event_kind])) {
                for (int i = 0; i < SONG_KIND_COUNT; ++i) {
                    if (ui_select(song_event_names[i], e->song_event_kind == i, 0, 0)) {
                        song_event_add_draft(e, i);
                    }
                }

                ui_combo_end();
            }

            ed_midi_event_fields(e);

            if (*e->midi_error) {
                ui_text(e->midi_error);
            }

            if (ui_button(DM_TEXT_ADD_EVENT, 120, 0)) {
                ed_song_event_add(e);
            }
        } else {
            size_t count = ed_song_event_rows(e, start, end);
            ed_midi_event_panels(e, count, &e->song_event_split, ui_flex(200, height - 90));
        }
    }

    ui_end();
}
