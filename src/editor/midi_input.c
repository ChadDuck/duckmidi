#include "editor_internal.h"
#include "midi_input_core.h"
#include "text/en.h"

/* MIDI input recording into the open pattern, alongside the computer-keyboard piano.

   Events arrive with their own song positions, collect in a take buffer and are written to the
   project at most every DM_MIDI_COMMIT_MS. Recorded material lies behind the playhead and is
   already heard through monitoring, so playback (ed_publish) is rebuilt only when it can change
   what plays next: when replace mode first clears a lane (its later points must stop),
   PUBLISH_LEAD before the transport wraps (so the next pass plays this one), at the wrap, and
   when recording is switched off during playback. Stop and seek republish by themselves. This
   keeps schedule rebuilds of large projects off the per-batch path. */

#define INPUT_LANES 258   /* Controllers 0-127, pitch bend, channel pressure, poly keys 0-127. */
#define PUBLISH_LEAD 0.25 /* Seconds before a wrap at which the pass is published. */

typedef struct {
    double start, origin;
    uint8_t velocity, port;
    bool active;
} InputNote;

typedef struct {
    double first, last, committed_first, committed_last;
    bool touched, committed;
} InputLane;

typedef struct {
    DmAutomation point;
    size_t order;
} InputPoint;

struct DmEditorMidiInput {
    InputNote held[DM_MIDI_INPUTS][MIDI_CHANNELS][MIDI_KEYS];
    DmBytes notes, points;
    InputLane lanes[INPUT_LANES]; /* The current loop pass's touched controller lanes. */
    uint64_t signature, commit_tick, discard_ns, wait_ns, session;
    uint32_t loop, pattern_uid;
    int pattern, channel, port;
    double wait_beat;
    bool dirty;          /* Recorded data not yet written to the project. */
    bool unpublished;    /* Written data playback has not been rebuilt with. */
    bool lead_published; /* This pass was published ahead of its wrap. */
    bool loop_seen, waiting, finishing;
};

static unsigned lane_index(DmAutomation point) {
    return point.type == MIDI_CONTROL            ? (unsigned)point.a
           : point.type == MIDI_PITCH_BEND       ? 128
           : point.type == MIDI_CHANNEL_PRESSURE ? 129
                                                 : 130 + (unsigned)point.a;
}

static void memory_error(Editor *e) {
    ed_message(e, DM_TEXT_MIDI_INPUT_MEMORY);
}

void ed_midi_input_attach(Editor *e, DmMidiInput *input) {
    if (e->midi_input == input) {
        return;
    }

    ed_midi_input_finish(e, false);
    dm_audio_attach_midi_input(e->audio, NULL);
    dm_midi_input_destroy(e->midi_input);
    e->midi_input = input;
    dm_audio_attach_midi_input(e->audio, input);

    if (e->midi_input_state) {
        e->midi_input_state->signature = 0;
        e->midi_input_state->loop_seen = false;
    }
}

void ed_midi_input_init(Editor *e) {
    e->midi_input_state = calloc(1, sizeof(*e->midi_input_state));
    e->midi_input = dm_midi_input_create(NULL);

    if (e->midi_input_state) {
        e->midi_input_state->pattern = -1;
    }

    if (e->midi_input && e->midi_input_state) {
        dm_audio_attach_midi_input(e->audio, e->midi_input);
    }
}

void ed_midi_input_restore(Editor *e) {
    if (!e->midi_input) {
        return;
    }

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        if (*e->input_names[i]) {
            dm_midi_input_open(e->midi_input, e->input_names[i]);
        }
    }
}

void ed_midi_input_names(Editor *e) {
    if (!e->midi_input) {
        return;
    }

    DmMidiInputStatus status[DM_MIDI_INPUTS];
    dm_midi_input_status(e->midi_input, status);
    memset(e->input_names, 0, sizeof(e->input_names));
    unsigned count = 0;

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        if (status[i].requested) {
            snprintf(e->input_names[count++], DM_MIDI_NAME, "%s", status[i].name);
        }
    }
}

void ed_midi_input_take(Editor *e) {
    struct DmEditorMidiInput *m = e->midi_input_state;

    if (!m) {
        return;
    }

    memset(m->lanes, 0, sizeof(m->lanes));
    m->pattern = e->selected_pattern;
    m->pattern_uid = ed_pattern(e)->uid;
    m->session = e->project_session;
    m->channel = e->pattern_channel;
    m->port = ed_note_port(e);
    m->commit_tick = SDL_GetTicks();
    m->dirty = m->unpublished = m->lead_published = m->loop_seen = false;
}

/* Whether an existing point gives way to the take. Replace mode clears a touched lane in the
   pattern; overlap mode clears the touched range. A commit removes only what earlier commits of
   the same pass have not, so the pass keeps every point it received. */
static bool replaced(Editor *e, DmAutomation point) {
    struct DmEditorMidiInput *m = e->midi_input_state;

    if (point.pattern != m->pattern || point.channel != m->channel || point.port != m->port ||
        (point.type != MIDI_CONTROL && point.type != MIDI_PITCH_BEND &&
         point.type != MIDI_CHANNEL_PRESSURE && point.type != MIDI_KEY_PRESSURE)) {
        return false;
    }

    InputLane lane = m->lanes[lane_index(point)];

    if (!lane.touched) {
        return false;
    }

    if (!e->overwrite) {
        return !lane.committed;
    }

    return point.beat >= lane.first && point.beat <= lane.last &&
           (!lane.committed || point.beat < lane.committed_first ||
            point.beat > lane.committed_last);
}

static int point_compare(const void *left, const void *right) {
    const InputPoint *a = left, *b = right;

    if (a->point.beat != b->point.beat) {
        return a->point.beat < b->point.beat ? -1 : 1;
    }

    return a->order < b->order ? -1 : a->order != b->order;
}

/* The take's pattern by identity. A removed pattern or a replaced project cancels pending data,
   even when another pattern now has the same index. Monitoring continues regardless. */
static bool resolve_take_pattern(Editor *e) {
    struct DmEditorMidiInput *m = e->midi_input_state;

    for (size_t i = 0;
         m->session == e->project_session && m->pattern_uid && i < e->project.pattern_count; ++i) {
        if (e->project.patterns[i].uid == m->pattern_uid) {
            m->pattern = (int)i;
            return true;
        }
    }

    memset(m->held, 0, sizeof(m->held));
    m->notes.size = m->points.size = 0;
    m->dirty = m->unpublished = m->loop_seen = false;
    m->pattern = -1;
    e->take_started = false;
    return false;
}

/* Merges pending points into the sorted automation in one pass. Returns the number of existing
   points removed, or -1 when memory runs out. */
static long merge_points(Editor *e) {
    struct DmEditorMidiInput *m = e->midi_input_state;
    size_t points = m->points.size / sizeof(InputPoint);

    if (!points) {
        return 0;
    }

    InputPoint *pending = (InputPoint *)m->points.data;
    size_t total = e->project.automation_count + points;
    DmAutomation *merged = malloc(total * sizeof(*merged));

    if (!merged) {
        return -1;
    }

    for (size_t i = 0; i < points; ++i) {
        pending[i].point.pattern = m->pattern;
    }

    qsort(pending, points, sizeof(*pending), point_compare);
    size_t old = 0, next = 0, used = 0;

    while (old < e->project.automation_count || next < points) {
        if (old < e->project.automation_count && replaced(e, e->project.automation[old])) {
            ++old;
            continue;
        }

        if (next < points &&
            (old == e->project.automation_count ||
             dm_automation_later(e->project.automation[old], pending[next].point))) {
            merged[used++] = pending[next++].point;
        } else {
            merged[used++] = e->project.automation[old++];
        }
    }

    free(e->project.automation);
    e->project.automation = merged;
    e->project.automation_count = used;
    e->project.automation_capacity = total;

    for (unsigned i = 0; i < INPUT_LANES; ++i) {
        InputLane *lane = &m->lanes[i];

        if (lane->touched) {
            lane->committed = true;
            lane->committed_first = lane->first;
            lane->committed_last = lane->last;
        }
    }

    m->points.size = 0;
    return (long)(total - used);
}

/* Writes pending data; with publish, also rebuilds playback for anything written earlier. */
static void commit(Editor *e, bool publish) {
    struct DmEditorMidiInput *m = e->midi_input_state;

    if (!m->dirty) {
        if (publish && m->unpublished) {
            ed_publish(e);
            m->unpublished = false;
        }

        return;
    }

    if (!resolve_take_pattern(e)) {
        return;
    }

    long removed = merge_points(e);

    if (removed < 0) {
        memory_error(e);
        return;
    }

    /* Replace mode removes a lane's points across the pattern, including those still to play. */
    bool upcoming = removed > 0 && !e->overwrite;
    DmPattern *pattern = &e->project.patterns[m->pattern];
    DmNote *notes = (DmNote *)m->notes.data;
    size_t count = m->notes.size / sizeof(*notes);

    for (size_t i = 0; i < count; ++i) {
        DmNote note = notes[i];

        if (!ed_record_insert(e, pattern, note.key, note.velocity, note.port, note.beat,
                              note.beat + note.length, 0)) {
            memmove(notes, notes + i, (count - i) * sizeof(*notes));
            m->notes.size = (count - i) * sizeof(*notes);
            memory_error(e);
            return;
        }
    }

    m->notes.size = 0;
    m->dirty = false;
    m->commit_tick = SDL_GetTicks();
    ed_changed(e);

    if (publish || upcoming) {
        ed_publish(e);
        m->unpublished = false;
    } else {
        m->unpublished = true;
    }
}

void ed_midi_input_flush(Editor *e) {
    if (e->midi_input_state) {
        commit(e, false);
    }
}

static void note_end(Editor *e, InputNote *note, unsigned key, double beat) {
    if (!note->active) {
        return;
    }

    double start = fmax(0, note->start - note->origin);
    double end = fmax(start, beat - note->origin);
    DmNote result = {start, fmax(1.0 / DM_PPQ, end - start), key, note->velocity, 0, note->port};
    struct DmEditorMidiInput *m = e->midi_input_state;

    if (dm_bytes(&m->notes, &result, sizeof(result))) {
        m->dirty = true;
        note->active = false;
    } else {
        memory_error(e);
    }
}

static void close_device(Editor *e, unsigned device, double beat) {
    struct DmEditorMidiInput *m = e->midi_input_state;

    for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
        for (unsigned key = 0; key < MIDI_KEYS; ++key) {
            note_end(e, &m->held[device][c][key], key, beat);
        }
    }
}

static void close_notes(Editor *e, double beat) {
    for (unsigned d = 0; d < DM_MIDI_INPUTS; ++d) {
        close_device(e, d, beat);
    }
}

/* Beat at which the transport wraps back. */
static double wrap_beat(Editor *e) {
    return e->loop_enabled[e->pattern_mode] ? e->loop_end[e->pattern_mode] : ed_transport_length(e);
}

/* At a loop wrap, held notes end at the loop end and the next pass replaces lanes afresh. */
static void loop_wrap(Editor *e) {
    struct DmEditorMidiInput *m = e->midi_input_state;
    close_notes(e, wrap_beat(e));
    commit(e, true);
    memset(m->lanes, 0, sizeof(m->lanes));
    m->lead_published = false;
    e->take_origin_set = false;
}

/* Publishes the pass shortly before the wrap, so material near the loop start plays next pass;
   material recorded after this is near the loop end and is published at the wrap. */
static void publish_ahead(Editor *e) {
    struct DmEditorMidiInput *m = e->midi_input_state;

    if (m->lead_published || !e->take_started || !dm_audio_playing(e->audio) ||
        (!m->unpublished && !m->dirty)) {
        return;
    }

    double left = dm_beat_seconds(&e->project, wrap_beat(e)) - dm_audio_position(e->audio);

    if (left >= 0 && left <= PUBLISH_LEAD) {
        commit(e, true);
        m->lead_published = true;
    }
}

static double now_beat(Editor *e) {
    return dm_seconds_beat(&e->project, dm_audio_position(e->audio));
}

static void input_event(Editor *e, DmMidiInputEvent event) {
    struct DmEditorMidiInput *m = e->midi_input_state;
    int type = event.status & MIDI_STATUS_MASK;

    if (event.ticks_ns <= m->discard_ns) {
        return;
    }

    if (event.flags & DM_MIDI_EVENT_DEVICE_RELEASE) {
        close_device(e, event.device, dm_seconds_beat(&e->project, fmax(0, event.song_seconds)));
        return;
    }

    if (type == MIDI_NOTE_OFF &&
        !m->held[event.device][event.status & MIDI_CHANNEL_MASK][event.a].active) {
        return;
    }

    if (!e->record || !e->active || !ed_pattern(e) || ed_pattern(e)->kind) {
        return;
    }

    bool waiting = e->wait_armed;

    if (waiting && type != MIDI_NOTE_ON) {
        return;
    }

    if (!(event.flags & DM_MIDI_EVENT_PLAYING) && !waiting && !m->waiting) {
        return;
    }

    if (!ed_record_start(e) || !e->take_started) {
        return;
    }

    double beat = dm_seconds_beat(&e->project, fmax(0, event.song_seconds));

    /* Wait for input starts playback at the first note, which was stamped while stopped. Events
       until the transport reports playing are placed by elapsed time from that note. */
    if (waiting) {
        m->waiting = true;
        m->wait_ns = event.ticks_ns;
        m->wait_beat = e->start_beat[e->pattern_mode];
    }

    if (m->waiting && !(event.flags & DM_MIDI_EVENT_PLAYING)) {
        double seconds =
            dm_beat_seconds(&e->project, m->wait_beat) + (event.ticks_ns - m->wait_ns) / 1e9;
        beat = dm_seconds_beat(&e->project, seconds);
    } else if (event.flags & DM_MIDI_EVENT_PLAYING) {
        m->waiting = false;
    }

    if (m->loop_seen && event.loop != m->loop) {
        loop_wrap(e);
    }

    m->loop = event.loop;
    m->loop_seen = true;
    double origin = ed_record_origin(e, beat);
    InputNote *note = &m->held[event.device][event.status & MIDI_CHANNEL_MASK][event.a];

    if (type == MIDI_NOTE_ON) {
        note_end(e, note, event.a, beat);
        *note = (InputNote){beat, origin, event.b, ed_note_port(e), true};
        return;
    }

    if (type == MIDI_NOTE_OFF) {
        note_end(e, note, event.a, beat);
        return;
    }

    DmAutomation point = {.beat = fmax(0, beat - origin),
                          .type = type,
                          .channel = e->pattern_channel,
                          .a = event.a,
                          .b = event.b,
                          .pattern = e->selected_pattern,
                          .port = (uint8_t)ed_note_port(e)};
    InputPoint pending = {point, m->points.size / sizeof(InputPoint)};

    if (!dm_bytes(&m->points, &pending, sizeof(pending))) {
        memory_error(e);
        return;
    }

    InputLane *lane = &m->lanes[lane_index(point)];

    if (!lane->touched) {
        lane->first = lane->last = point.beat;
        lane->touched = true;
    } else {
        lane->first = fmin(lane->first, point.beat);
        lane->last = fmax(lane->last, point.beat);
    }

    m->dirty = true;
}

/* The state ed_keyboard_channel reads; the route is recomputed only when it changes. */
static uint64_t route_signature(Editor *e) {
    uint64_t hash = 1469598103934665603ULL;
    int values[] = {e->active,           e->context,          e->channel_selected,
                    e->selected_channel, e->selected_pattern, e->pattern_channel,
                    e->preview_program,  e->preview_override, e->pattern_mode,
                    (int)ed_note_port(e)};

    for (unsigned i = 0; i < sizeof(values) / sizeof(*values); ++i) {
        hash = (hash ^ (unsigned)values[i]) * 1099511628211ULL;
    }

    return hash;
}

static void update_route(Editor *e, bool enabled) {
    struct DmEditorMidiInput *m = e->midi_input_state;
    uint64_t signature = route_signature(e);

    if (signature == m->signature) {
        return;
    }

    int route = enabled ? ed_keyboard_channel(e) : 0;

    /* A different pattern, channel or port ends the take, as it does for the keyboard. */
    if (e->take_started && (m->pattern != e->selected_pattern || m->channel != e->pattern_channel ||
                            m->port != (int)ed_note_port(e))) {
        close_notes(e, now_beat(e));
        commit(e, true);
        e->take_started = false;
    }

    dm_audio_input_route(e->audio, route, enabled);
    m->signature = route_signature(e);
}

void ed_midi_input_frame(Editor *e) {
    struct DmEditorMidiInput *m = e->midi_input_state;

    if (!m || !e->midi_input) {
        return;
    }

    if (!dm_midi_input_begin(e->midi_input, 1)) {
        if (m->dirty && SDL_GetTicks() - m->commit_tick >= DM_MIDI_COMMIT_MS) {
            commit(e, false);
        }

        return;
    }

    if (e->take_started) {
        resolve_take_pattern(e);
    }

    bool enabled = e->active && !(e->context == 1 && !e->channel_selected);
    update_route(e, enabled);

    if (dm_midi_input_overflow(e->midi_input, 1)) {
        close_notes(e, now_beat(e));
        m->discard_ns = SDL_GetTicksNS();
    }

    DmMidiInputEvent event;
    unsigned n = 0;

    for (; n < DM_MIDI_RECORD_DRAIN && dm_midi_input_pop(e->midi_input, 1, &event); ++n) {
        if (enabled) {
            input_event(e, event);
        }
    }

    dm_midi_input_end(e->midi_input, 1);
    DmMidiClockValue clock;

    /* A wrap with no input after it still ends held notes at the loop end. */
    if (n < DM_MIDI_RECORD_DRAIN &&
        dm_midi_clock_read(dm_midi_input_clock(e->midi_input), &clock)) {
        if (m->loop_seen && e->record && e->take_started && clock.playing &&
            clock.loop != m->loop) {
            loop_wrap(e);
        }

        m->loop = clock.loop;
    }

    if (SDL_GetTicks() - m->commit_tick >= DM_MIDI_COMMIT_MS) {
        commit(e, false);
    }

    publish_ahead(e);
}

void ed_midi_input_finish(Editor *e, bool publish) {
    struct DmEditorMidiInput *m = e->midi_input_state;

    if (!m || m->finishing) {
        return;
    }

    m->finishing = true;

    if (e->take_started || (e->recording_take && !e->take_snapshot) || e->wait_armed) {
        ed_midi_input_frame(e);
    }

    close_notes(e, now_beat(e));
    commit(e, publish);
    m->discard_ns = SDL_GetTicksNS();
    m->waiting = false;
    m->finishing = false;
}

void ed_midi_input_free(Editor *e) {
    dm_audio_attach_midi_input(e->audio, NULL);
    dm_midi_input_destroy(e->midi_input);
    e->midi_input = NULL;

    if (e->midi_input_state) {
        dm_bytes_free(&e->midi_input_state->notes);
        dm_bytes_free(&e->midi_input_state->points);
        free(e->midi_input_state);
        e->midi_input_state = NULL;
    }
}
