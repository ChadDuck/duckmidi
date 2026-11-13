#include "editor/internal.h"
#include "text/en.h"

/* In the Type dropdown's order: timing, the channel's setup, loop points, then the text and
   marker events. */
typedef enum {
    SONG_TEMPO,
    SONG_METER,
    CHANNEL_PROGRAM,
    CHANNEL_VOLUME,
    CHANNEL_PAN,
    SONG_LOOP_START,
    SONG_LOOP_END,
    SONG_TEXT,
    SONG_TRACK_NAME,
    SONG_SMPTE,
    SONG_KEY,
    SONG_MARKER,
    SONG_KIND_COUNT,
} SongEventKind;

static const struct {
    const char *name;
    bool channel;
} song_event_types[] = {
    [SONG_TEMPO] = {DM_TEXT_TEMPO, false},
    [SONG_METER] = {DM_TEXT_METER, false},
    [CHANNEL_PROGRAM] = {DM_TEXT_PROGRAM_CHANGE, true},
    [CHANNEL_VOLUME] = {DM_TEXT_VOLUME, true},
    [CHANNEL_PAN] = {DM_TEXT_PAN, true},
    [SONG_LOOP_START] = {DM_TEXT_M64_LOOP_START, false},
    [SONG_LOOP_END] = {DM_TEXT_M64_LOOP_END, false},
    [SONG_TEXT] = {DM_TEXT_TEXT, false},
    [SONG_TRACK_NAME] = {DM_TEXT_TRACK_NAME, false},
    [SONG_SMPTE] = {DM_TEXT_SMPTE_OFFSET, false},
    [SONG_KEY] = {DM_TEXT_KEY_SIGNATURE, false},
    [SONG_MARKER] = {DM_TEXT_MARKER, false},
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

static bool channel_event_type(int type, int a) {
    return type == MIDI_PROGRAM ||
           (type == MIDI_CONTROL && (a == MIDI_CC_VOLUME || a == MIDI_CC_PAN));
}

static bool song_event_row(const Editor *e, MidiRow row, int channel) {
    if (row.automation) {
        DmAutomation a = e->project.automation[row.index];
        return a.pattern < 0 &&
               (channel >= 0 ? a.channel == channel : !e->channel_expanded[a.channel]) &&
               channel_event_type(a.type, a.a);
    }

    const DmBytes *m = &e->project.events[row.index].message;

    if (m->size && m->data[0] < MIDI_SYSTEM) {
        int type = m->size ? m->data[0] & MIDI_STATUS_MASK : 0;
        int source = m->data[0] & MIDI_CHANNEL_MASK;
        return m->size >= 2 && (channel >= 0 ? source == channel : !e->channel_expanded[source]) &&
               channel_event_type(type, m->data[1]) &&
               (type == MIDI_PROGRAM || (m->size == 3 && row.beat != 0));
    }

    if (channel < 0 && m->size >= 2 && m->data[0] == MIDI_META) {
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

    return false;
}

size_t ed_song_event_rows(Editor *e, double start, double end, int channel) {
    size_t count = ed_midi_rows_between(e, start, end);
    MidiRow *rows = (MidiRow *)e->midi_rows.data;
    size_t selected = 0;
    DmProgramHistory history[DM_PORT_COMBINED][16] = {0};

    for (size_t i = 0; i < count; ++i) {
        MidiRow row = rows[i];
        int prior = dm_project_program_repeat(history, &e->project, row.index, row.automation);

        if (prior < 0 && song_event_row(e, row, channel)) {
            rows[selected++] = row;
        }
    }

    e->midi_rows.size = selected * sizeof(*rows);
    return selected;
}

static void song_event_clear_picks(Editor *e) {
    e->song_event_picks.size = 0;
    e->song_event_pick_anchor = -1;
    e->song_event_selection_revision = e->export_revision;
    e->song_event_move_by = 0;
    e->song_event_move_to = 0;
}

size_t ed_song_event_pick_count(Editor *e) {
    if (e->song_event_selection_revision != e->export_revision) {
        song_event_clear_picks(e);
    }

    size_t count = e->song_event_picks.size / sizeof(MidiRow);
    const MidiRow *picks = (const MidiRow *)e->song_event_picks.data;

    for (size_t i = 0; i < count; ++i) {
        MidiRow row = picks[i];

        if (row.index < 0 || (size_t)row.index >= (row.automation ? e->project.automation_count
                                                                  : e->project.event_count)) {
            song_event_clear_picks(e);
            return 0;
        }
    }

    return count;
}

bool ed_song_event_picked(Editor *e, MidiRow row) {
    size_t count = ed_song_event_pick_count(e);
    const MidiRow *picks = (const MidiRow *)e->song_event_picks.data;

    for (size_t i = 0; i < count; ++i) {
        if (picks[i].index == row.index && picks[i].automation == row.automation) {
            return true;
        }
    }

    return false;
}

/* Before the first modifier click, the event open in the details panel counts as picked. */
static bool song_event_single(const Editor *e, MidiRow row) {
    return e->midi_selection_revision == e->export_revision && e->midi_event_index == row.index &&
           e->midi_event_kind == (int)row.automation;
}

/* After the picks change: one opens in the details panel; several start Move to at the beat
   of the first picked row in the list, the earliest. */
static void song_event_picked(Editor *e) {
    size_t count = ed_song_event_pick_count(e);
    const MidiRow *picks = (const MidiRow *)e->song_event_picks.data;
    e->midi_event_index = -1;

    if (count == 1) {
        ed_midi_select(e, picks[0]);
    } else if (count > 1) {
        e->song_event_move_to = picks[0].beat;

        for (size_t i = 1; i < count; ++i) {
            e->song_event_move_to = fmin(e->song_event_move_to, picks[i].beat);
        }
    }
}

/* A click on list row clicked, with the modifiers' pick (see SelectPick). The picks become the
   listed rows the click selects; a range runs from the anchor row, and without the anchor in
   the list a range click picks only the clicked row. One pick opens it in the details panel. */
void ed_song_event_pick(Editor *e, size_t clicked, SelectPick pick) {
    size_t count = e->midi_rows.size / sizeof(MidiRow);

    if (!e->song_event || e->song_event_add || clicked >= count) {
        return;
    }

    const MidiRow *rows = (const MidiRow *)e->midi_rows.data;
    bool seed = !ed_song_event_pick_count(e);
    size_t anchor = count, single = count;

    for (size_t i = 0; i < count; ++i) {
        if (rows[i].index == e->song_event_pick_anchor &&
            rows[i].automation == e->song_event_pick_automation) {
            anchor = i;
        }

        if (song_event_single(e, rows[i])) {
            single = i;
        }
    }

    /* The event the window opened on anchors a range until a click sets the anchor. */
    if (anchor == count && seed) {
        anchor = single;
    }

    bool range = pick == PICK_RANGE || pick == PICK_ADD_RANGE;

    if (range && anchor == count) {
        pick = PICK_ONE;
        range = false;
    }

    size_t first = range ? (anchor < clicked ? anchor : clicked) : clicked;
    size_t last = range ? (anchor > clicked ? anchor : clicked) : clicked;
    DmBytes picks = {0};

    for (size_t i = 0; i < count; ++i) {
        bool before = seed ? song_event_single(e, rows[i]) : ed_song_event_picked(e, rows[i]);

        if (ed_pick_selects(pick, before, i == clicked, i >= first && i <= last) &&
            !dm_bytes(&picks, &rows[i], sizeof(MidiRow))) {
            dm_bytes_free(&picks);
            ed_message(e, DM_TEXT_CANNOT_ALLOCATE_MIDI);
            return;
        }
    }

    dm_bytes_free(&e->song_event_picks);
    e->song_event_picks = picks;

    if (!range) {
        e->song_event_pick_anchor = rows[clicked].index;
        e->song_event_pick_automation = rows[clicked].automation;
    }

    song_event_picked(e);
}

static bool song_event_move_bounds(Editor *e, double *earliest, double *latest) {
    size_t count = ed_song_event_pick_count(e);
    const MidiRow *picks = (const MidiRow *)e->song_event_picks.data;
    *earliest = INFINITY;
    *latest = 0;

    for (size_t i = 0; i < count; ++i) {
        MidiRow row = picks[i];

        double beat = row.automation ? e->project.automation[row.index].beat
                                     : e->project.events[row.index].beat;
        *earliest = fmin(*earliest, beat);
        *latest = fmax(*latest, beat);
    }

    return count > 0;
}

/* Sorting carries the marks because event indices change when a moved row crosses another. */
static void song_event_sort_marked(DmProject *p, bool *marks, bool automation) {
    size_t count = automation ? p->automation_count : p->event_count;

    for (size_t i = 1; i < count; ++i) {
        size_t at = i;
        bool marked = marks[i];

        if (automation) {
            DmAutomation item = p->automation[i];

            while (at && dm_automation_later(p->automation[at - 1], item)) {
                p->automation[at] = p->automation[at - 1];
                marks[at] = marks[at - 1];
                --at;
            }

            p->automation[at] = item;
        } else {
            DmEvent item = p->events[i];

            while (at && p->events[at - 1].beat > item.beat) {
                p->events[at] = p->events[at - 1];
                marks[at] = marks[at - 1];
                --at;
            }

            p->events[at] = item;
        }

        marks[at] = marked;
    }
}

static void song_event_move_marked(Editor *e, bool *marks, bool automation, double value,
                                   bool absolute) {
    DmProject *p = &e->project;
    dm_collapse_marked_program_repeats(p, marks, automation);
    size_t count = automation ? p->automation_count : p->event_count;

    for (size_t i = 0; i < count; ++i) {
        if (!marks[i]) {
            continue;
        }

        double *beat = automation ? &p->automation[i].beat : &p->events[i].beat;
        *beat = absolute ? value : *beat + value;

        if (*beat == 0) {
            if (!automation) {
                ed_midi_apply_initial(e, p->events[i].message.data);
            } else if (p->automation[i].pattern < 0) {
                DmAutomation a = p->automation[i];
                uint8_t bytes[] = {a.type | a.channel, a.a, a.b};
                ed_midi_apply_initial(e, bytes);
            }
        }
    }

    song_event_sort_marked(p, marks, automation);

    for (size_t i = 0; i < count; ++i) {
        if (marks[i]) {
            double beat = automation ? p->automation[i].beat : p->events[i].beat;
            MidiRow row = {beat, (int)i, automation};
            dm_bytes(&e->song_event_picks, &row, sizeof(row));
        }
    }
}

bool ed_song_events_move(Editor *e, double value, bool absolute) {
    double earliest, latest;

    if (!e->song_event || e->song_event_add || !isfinite(value) ||
        !song_event_move_bounds(e, &earliest, &latest)) {
        return false;
    }

    value = absolute ? fmax(0, fmin(DM_MAX_BEAT, value))
                     : fmax(-earliest, fmin(DM_MAX_BEAT - latest, value));
    bool *events = calloc(e->project.event_count + 1, sizeof(bool));
    bool *automation = calloc(e->project.automation_count + 1, sizeof(bool));

    if (!events || !automation) {
        free(events);
        free(automation);
        return false;
    }

    size_t count = ed_song_event_pick_count(e);
    MidiRow *picks = (MidiRow *)e->song_event_picks.data;

    for (size_t i = 0; i < count; ++i) {
        (picks[i].automation ? automation : events)[picks[i].index] = true;
    }

    if (!ed_checkpoint(e)) {
        free(events);
        free(automation);
        return false;
    }

    e->song_event_picks.size = 0;
    song_event_move_marked(e, events, false, value, absolute);
    song_event_move_marked(e, automation, true, value, absolute);
    free(events);
    free(automation);
    e->song_event_anchor = absolute ? value : earliest + value;
    e->song_event_pick_anchor = -1;
    e->midi_event_index = -1;
    ed_commit(e);
    e->song_event_selection_revision = e->export_revision;
    song_event_picked(e);
    return true;
}

void ed_song_events_move_panel(Editor *e) {
    char label[80];
    snprintf(label, sizeof(label), DM_TEXT_SONG_EVENTS_SELECTED, ed_song_event_pick_count(e));
    ui_align_frame();
    ui_text(label);
    ui_same();
    UiPoint at = ui_cursor();
    ui_position(fmaxf(at.x, at.x + ui_available().x - 24), at.y);
    ui_info(DM_TEXT_SONG_EVENTS_MOVE_HELP);
    double earliest, latest;

    if (!song_event_move_bounds(e, &earliest, &latest)) {
        return;
    }

    e->song_event_move_by =
        (float)fmax(-earliest, fmin(DM_MAX_BEAT - latest, e->song_event_move_by));
    ui_float(DM_TEXT_SONG_EVENTS_MOVE_BY, &e->song_event_move_by, (float)-earliest,
             (float)(DM_MAX_BEAT - latest));

    if (ui_button(DM_TEXT_SONG_EVENTS_MOVE_RELATIVE, 0, 0)) {
        ed_song_events_move(e, e->song_event_move_by, false);
    }

    ui_separator();
    ed_song_event_position(e, &e->song_event_move_to, DM_TEXT_SONG_EVENTS_MOVE_TO, 0);

    if (ui_button(DM_TEXT_SONG_EVENTS_MOVE_ABSOLUTE, 0, 0)) {
        ed_song_events_move(e, e->song_event_move_to, true);
    }
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
    case CHANNEL_PROGRAM: {
        int channel = e->song_event_channel >= 0 ? e->song_event_channel : e->selected_channel;
        data[0] = MIDI_PROGRAM | channel;
        data[1] = (uint8_t)fmax(0, e->project.channels[channel].program);
        size = 2;
        break;
    }
    case CHANNEL_VOLUME:
    case CHANNEL_PAN: {
        int channel = e->song_event_channel >= 0 ? e->song_event_channel : e->selected_channel;
        data[0] = MIDI_CONTROL | channel;
        data[1] = kind == CHANNEL_VOLUME ? MIDI_CC_VOLUME : MIDI_CC_PAN;
        data[2] = kind == CHANNEL_VOLUME ? e->project.channels[channel].volume
                                         : e->project.channels[channel].pan;
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
    song_event_clear_picks(e);
    e->song_event_add = add;
    e->midi_event_index = -1;
    e->midi_error[0] = 0;
    e->event_beat = e->song_event_anchor;

    if (add) {
        song_event_add_draft(e, e->song_event_channel < 0 ? SONG_TEMPO : CHANNEL_PROGRAM);
        return;
    }

    double start, end;
    ed_song_event_bar(e, e->song_event_anchor, &start, &end);

    if (ed_song_event_rows(e, start, end, e->song_event_channel)) {
        ed_midi_select(e, ((MidiRow *)e->midi_rows.data)[0]);
    }
}

void ed_song_event_open_bar(Editor *e, double beat, bool add, int channel) {
    e->song_event_channel = channel;
    e->midi_events = false;
    e->song_event = true;
    e->song_event_bound = true;
    e->song_event_anchor = beat;
    song_event_mode(e, add);
}

void ed_new_song_event(Editor *e) {
    ed_song_event_open_bar(e, e->cursor_beat[0], true, -1);
    e->song_event_bound = false;
}

bool ed_song_event_position_valid(Editor *e) {
    if (!isfinite(e->event_beat) || e->event_beat < 0 || e->event_beat > DM_MAX_BEAT) {
        return false;
    }

    if (e->song_event_channel >= 0) {
        const DmBytes *m = &e->midi_draft;

        if (!dm_message_valid(m) || (m->data[0] & MIDI_CHANNEL_MASK) != e->song_event_channel ||
            !channel_event_type(m->data[0] & MIDI_STATUS_MASK, m->data[1])) {
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
    if (!ed_checkpoint(e)) {
        return false;
    }

    bool saved;

    if (m->data[0] < MIDI_SYSTEM && channel_event_type(m->data[0] & MIDI_STATUS_MASK, m->data[1])) {
        DmAutomation a = {.beat = beat,
                          .type = m->data[0] & MIDI_STATUS_MASK,
                          .channel = m->data[0] & MIDI_CHANNEL_MASK,
                          .a = m->data[1],
                          .b = m->size > 2 ? m->data[2] : 0,
                          .pattern = -1};
        saved = dm_automation_add(&e->project, a);
    } else {
        saved = dm_event_add(&e->project, beat, 0, 0, m);
    }

    if (!saved) {
        ed_checkpoint_abandon(e);
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

/* With a field column width, the beat field and Use playhead fill the column and the label
   follows in the label column; otherwise the label sits beside the field. */
/* The beat field (1-based) with Use playhead, editing *beat (0-based). */
void ed_song_event_position(Editor *e, double *beat, const char *label, float column) {
    float shown = (float)(*beat + 1);
    bool fill = column > 0;

    if (fill) {
        float button = ui_button_row_width((const char *const[]){DM_TEXT_USE_PLAYHEAD}, 1);
        ui_width(column - button - ui_item_spacing_x());
    }

    char id[64];
    snprintf(id, sizeof(id), "%s%s", fill ? "##" : "", label);

    if (ui_float(id, &shown, 1, DM_MAX_BEAT + 1)) {
        *beat = shown - 1;
    }

    ui_same();

    if (ui_button(DM_TEXT_USE_PLAYHEAD, 0, 0)) {
        *beat = !e->pattern_mode && dm_audio_playing(e->audio)
                    ? dm_seconds_beat(&e->project, dm_audio_position(e->audio))
                    : e->cursor_beat[0];
        *beat = fmin(*beat, DM_MAX_BEAT);
    }

    if (fill) {
        ui_same_label();
        ui_align_frame();
        ui_text(label);
    }

    if (e->song_event_add && isfinite(e->event_beat) && e->event_beat >= 0) {
        e->song_event_anchor = e->event_beat;
    }
}

/* Add lays its fields out like the other windows: one field column across the window, with
   each label to its right in a column wide enough for the longest of them. */
static float add_field_column(void) {
    static const char *const labels[] = {
        DM_TEXT_SONG_EVENT_MODE, DM_TEXT_BEAT,       DM_TEXT_TYPE,          DM_TEXT_EVENT_CHANNEL,
        DM_TEXT_INSTRUMENT,      DM_TEXT_CONTROLLER, DM_TEXT_SAMPLE_SOURCE,
    };
    float label = 0;

    for (size_t i = 0; i < sizeof(labels) / sizeof(*labels); ++i) {
        label = fmaxf(label, ui_text_width(labels[i]));
    }

    return fmaxf(160, ui_available().x - label - ui_item_spacing_x());
}

void ed_song_event_dialog(Editor *e) {
    if (!e->song_event) {
        return;
    }

    /* Add fits its fields in a compact window. Edit opens at about a quarter of the display in
       each direction, within limits that keep both panels usable on small and large screens;
       its panels take the spare height. */
    UiPoint display = ui_display();
    float height = ed_clampf(display.y * .26f, 420, 600);
    float width = e->song_event_add ? 460 : ed_clampf(display.x * .25f, 760, 1000);

    char title[192];
    const char *name = DM_TEXT_SONG_EVENT;

    if (e->song_event_channel >= 0) {
        int channel = e->song_event_channel;
        int program = e->project.channels[channel].program;
        const char *instrument = dm_channel_drums(&e->project, channel)
                                     ? DM_TEXT_PERCUSSION
                                     : dm_program_name(program < 0 ? 0 : program);
        snprintf(title, sizeof(title), DM_TEXT_CHANNEL_EVENT_TITLE, channel + 1, instrument);
        name = title;
    }

    if (ui_dialog(name, &e->song_event,
                  (UiDialog){.width = width,
                             .height = height,
                             .size = UI_FLEXIBLE,
                             .layout = e->song_event_add})) {
        float column = e->song_event_add ? add_field_column() : 0;
        ui_width(column > 0 ? column : 140);

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

        if (!e->song_event_add) {
            char label[48];
            snprintf(label, sizeof(label), DM_TEXT_SONG_EVENT_BAR, bar);
            ui_same();
            ui_dim(label);
        }

        if (e->song_event_add) {
            ed_song_event_position(e, &e->event_beat, DM_TEXT_BEAT, column);
            ui_push_width(column);

            if (ui_combo(DM_TEXT_TYPE, song_event_types[e->song_event_kind].name)) {
                for (int i = 0; i < SONG_KIND_COUNT; ++i) {
                    if (e->song_event_channel >= 0 && !song_event_types[i].channel) {
                        continue;
                    }

                    if (ui_select(song_event_types[i].name, e->song_event_kind == i, 0, 0)) {
                        song_event_add_draft(e, i);
                    }
                }

                ui_combo_end();
            }

            ed_midi_event_fields(e);
            ui_pop_width();

            if (*e->midi_error) {
                ui_text(e->midi_error);
            }

            if (ui_button(DM_TEXT_ADD_EVENT, ui_available().x, 0)) {
                ed_song_event_add(e);
            }
        } else {
            size_t count = ed_song_event_rows(e, start, end, e->song_event_channel);
            ed_midi_event_panels(e, count, &e->song_event_split, ui_flex(200, height - 90));
        }
    }

    ui_dialog_end();
}
