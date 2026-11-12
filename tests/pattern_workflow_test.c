#include "editor/internal.h"
#include <assert.h>
#define TEST_NAME "pattern_workflow"
#include "test_common.h"
extern void test_pattern_workflow_ui(Editor *, void (*)(bool, const char *));
extern void test_pattern_rename_ui(Editor *);
extern void recording_feedback_audio_render(DmAudio *, int);
extern int recording_feedback_audio_preset(DmAudio *, int, bool);

static void fixture(Editor *e) {
    ed_release_piano(e);
    editor_smoke(e);
    e->project.clip_count = e->project.automation_count = 0;

    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        e->project.patterns[i].count = 0;
        e->project.patterns[i].length = 1;
    }

    e->selected_pattern = 0;
    e->edit_focus = FOCUS_NOTES;
    e->project.patterns[0].kind = 0;
    e->tool = TOOL_SELECT;
    e->pattern_mode = false;
    e->record = e->recording_take = e->take_snapshot = e->take_started = false;
    e->automation_node = -1;
    e->automation_all = false;
    e->event_selection.size = e->event_drag.size = 0;
    e->automation_cc = 11;
    e->automation_raw = false;
    e->overwrite = true;
    e->preview_program = 0;
    e->preview_override = true;
    e->selected_clip = -1;
    e->drag = DRAG_NONE;
    e->edit_moved = false;
}

static void sizing_names(Editor *e) {
    fixture(e);
    DmPattern *p = ed_pattern(e);
    check(p->length == 1, "empty pattern is one beat");
    dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0});
    dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 1, .length = .5});
    dm_note_add(p, (DmNote){0, 1, 60, 100, 0});
    ed_changed(e);
    check(p->length == 1, "exact beat boundary does not add another beat");
    dm_note_add(p, (DmNote){3.5, 1, 64, 100, 0});
    ed_changed(e);
    check(p->length == 5 && dm_clip_length(&e->project, e->project.clips[0]) == 5 &&
              dm_clip_length(&e->project, e->project.clips[1]) == .5,
          "auto placement grows, explicit placement stays fixed");
    p->count = 1;
    ed_changed(e);
    check(p->length == 1, "removal shrinks auto size");
    p->count = 0;
    ed_changed(e);
    check(p->length == 1, "cleared pattern retains one-beat minimum");
    dm_automation_add(&e->project, (DmAutomation){20, 0xe0, 0, 0, 64, 0, 0, 0, 0, false});
    ed_changed(e);
    check(p->length == 1 && e->project.automation_count == 1,
          "trailing controller data stays stored without extending automatic note placements");
    int base = dm_pattern_add(&e->project, "Pattern 2");
    int a = dm_pattern_copy(&e->project, base), b = dm_pattern_copy(&e->project, base),
        c = dm_pattern_copy(&e->project, a);
    char expected[64];
    snprintf(expected, 64, "%s-2", e->project.patterns[base].name);
    check(!strcmp(e->project.patterns[a].name, expected), "first duplicate gets suffix two");
    snprintf(expected, 64, "%s-3", e->project.patterns[base].name);
    check(!strcmp(e->project.patterns[b].name, expected), "next duplicate gets suffix three");
    snprintf(expected, 64, "%s-4", e->project.patterns[base].name);
    check(!strcmp(e->project.patterns[c].name, expected),
          "duplicate of duplicate increments its original name family");
    check(e->project.patterns[a].uid != e->project.patterns[base].uid,
          "pattern copies have independent identities");
    DmBytes bytes = {0};
    DmProject loaded;
    dm_project_init(&loaded);
    char error[256];
    check(dm_pack(&e->project, &bytes) &&
              dm_unpack(&loaded, bytes.data, bytes.size, error, sizeof(error)),
          "schema six roundtrip");
    check(loaded.patterns[a].uid == e->project.patterns[a].uid, "stable identity survives saving");
    dm_bytes_free(&bytes);
    uint32_t saved = e->project.patterns[a].uid;
    e->project.patterns[a].uid = e->project.patterns[base].uid;
    check(dm_pack(&e->project, &bytes) &&
              !dm_unpack(&loaded, bytes.data, bytes.size, error, sizeof(error)),
          "duplicate pattern identities are rejected");
    e->project.patterns[a].uid = saved;
    dm_bytes_free(&bytes);
    check(dm_pack(&e->project, &bytes), "pack current identity fixture");
    bytes.data[8] = 5;
    check(!dm_unpack(&loaded, bytes.data, bytes.size, error, sizeof(error)),
          "previous project schema rejected");
    dm_bytes_free(&bytes);
    dm_project_free(&loaded);
}

static void brush_velocity(Editor *e) {
    fixture(e);
    DmPattern *p = ed_pattern(e);
    View v = {1, 0, 1340, 0};
    UiRect grid = {100, 100, 500, 300};
    UiMouse m = {.clicked = true, .left = true, .shift = true, .pos = {105, 110}};
    int history = e->undo_count;
    ed_brush_begin(e, p, grid, &v, 80, 20, m);
    m.clicked = false;
    m.pos = (UiPoint){205, 70};
    ed_brush_drag(e, p, grid, &v, 80, 20, m);
    check(p->count >= 6 && e->undo_count == history + 1,
          "brush fills crossed cells in one undo step");

    for (size_t i = 0; i < p->count; ++i) {
        check(p->notes[i].length == .25 &&
                  fabs(p->notes[i].beat * 4 - round(p->notes[i].beat * 4)) < 1e-8,
              "brush notes are quarter-beat quantized");
    }

    m.left = false;
    m.released = true;
    ed_brush_drag(e, p, grid, &v, 80, 20, m);
    check(!e->drag, "brush release commits stroke");
    p->count = 0;
    m = (UiMouse){.clicked = true, .left = true, .shift = true, .ctrl = true, .pos = {105, 110}};
    ed_brush_begin(e, p, grid, &v, 80, 20, m);
    m.clicked = false;
    m.pos = (UiPoint){205, 10};
    ed_brush_drag(e, p, grid, &v, 80, 20, m);

    for (size_t i = 0; i < p->count; ++i) {
        check(p->notes[i].key == 60, "Ctrl brush keeps starting pitch");
    }

    p->count = 0;
    dm_note_add(p, (DmNote){0, 1, 60, 50, 1});
    dm_note_add(p, (DmNote){1, 1, 64, 75, 1});
    m = (UiMouse){.alt = true, .wheel = -1, .pos = {120, 110}};
    check(ed_note_velocity_wheel(e, p, (UiRect){36, 72, 564, 328}, &v, 64, 20, 80, m, true),
          "Alt-wheel consumed over note");
    check(p->notes[0].velocity == 45 && p->notes[1].velocity == 70,
          "selected velocities change relatively by five");
    check(ed_velocity_delta(p, 0, 100, p->notes) == 57 && p->notes[0].velocity == 102 &&
              p->notes[1].velocity == 127,
          "velocity group clamps delta without collapsing differences");
    e->automation_cc = DM_EVENT_VELOCITY;
    e->edit_focus = FOCUS_EVENTS;
    p->notes[0].selected = p->notes[1].selected = 0;
    ed_select_all(e);
    check(p->notes[0].selected && p->notes[1].selected, "velocity Ctrl+A selects every note value");
}

static void linked_copy(Editor *e) {
    fixture(e);
    e->edit_focus = FOCUS_PLAYLIST;
    uint32_t uid = e->project.patterns[1].uid;
    dm_clip_add(&e->project,
                (DmClip){.beat = 4, .pattern = 1, .channel = 2, .length = 2.5, .selected = 1});
    ed_clipboard(e, CLIPBOARD_COPY);
    dm_pattern_remove(&e->project, 0);
    e->selected_pattern = 0;
    dm_pattern_name(&e->project, 0, "Renamed linked source");
    e->cursor_beat[0] = 8;
    ed_paste(e);
    check(e->project.pattern_count == 1 && e->project.patterns[0].uid == uid,
          "paste links source after rename and index renumber");
    check(e->project.clip_count == 2 && e->project.clips[1].pattern == 0 &&
              e->project.clips[1].length == 2.5 && e->project.clips[1].beat == 8,
          "paste preserves placement length and destination beat");
    size_t clips = e->project.clip_count;
    ed_duplicate_clips(e);
    check(e->project.clip_count == clips + 1 && e->project.pattern_count == 1,
          "Ctrl+D duplicates linked placement without cloning notes");
    check(e->project.clips[clips].length == 2.5 && e->project.clips[clips].beat == 11,
          "duplicate keeps independent length and advances by occupied span");
    e->project.clips[clips].length = .75;
    check(e->project.clips[1].length == 2.5, "copied lengths are independent");
    ed_checkpoint(e);
    int created = dm_pattern_add(&e->project, "Undo candidate");
    uint32_t created_uid = e->project.patterns[created].uid;
    ed_commit(e);
    ed_undo(e, false);
    int replacement = dm_pattern_add(&e->project, "Replacement");
    check(e->project.patterns[replacement].uid > created_uid,
          "undo followed by new creation cannot reuse clipboard identities");
    ++e->project_session;
    size_t patterns = e->project.pattern_count;
    ed_paste(e);
    check(e->project.pattern_count == patterns + 1 &&
              e->project.clips[e->project.clip_count - 1].pattern == (int)patterns,
          "paste into another project session clones content instead of linking equal ids");
}

static void recording(Editor *e) {
    fixture(e);
    e->record = true;
    e->recording_take = true;
    dm_clip_add(&e->project, (DmClip){.beat = 120, .pattern = 0, .channel = 0});
    e->selected_clip = 0;
    e->edit_focus = FOCUS_PLAYLIST;
    check(ed_recording_origin(e, 120.25) == 120,
          "with the playlist focused on a placement, recording starts at the playhead");
    check(ed_recording_origin(e, 125.5) == 124,
          "past the placement, recording starts at the pattern's beginning with the bar offset");
    e->edit_focus = FOCUS_NOTES;
    check(ed_recording_origin(e, 120.25) == 120 && ed_recording_origin(e, 131.75) == 128,
          "without playlist focus, recording keeps only the position within the bar");
    e->edit_focus = FOCUS_PLAYLIST;
    char error[256];
    dm_audio_publish(e->audio, &e->project, 61, false, error, sizeof(error));
    e->record_start[60] = 120.25;
    e->record_origin[60] = 120;
    ed_record_note(e, 60);
    check(ed_pattern(e)->count == 1 && fabs(ed_pattern(e)->notes[0].beat - .25) < 1e-8 &&
              fabs(ed_pattern(e)->notes[0].length - 1.75) < .01,
          "late-song recording stores pattern-local beat and played duration");
    check(ed_pattern(e)->length == 2, "recording extends only local content span");
    e->pattern_mode = true;
    check(ed_recording_origin(e, 120) == 0, "Pattern-mode recording already uses local beats");
}

static void group_points(Editor *e) {
    fixture(e);
    e->edit_focus = FOCUS_EVENTS;
    dm_automation_add(&e->project, (DmAutomation){1, 0xb0, 0, 11, 30, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){3, 0xb0, 0, 11, 60, 0, 0, 0, 0, false});
    dm_bytes(&e->event_selection, e->project.automation, 2 * sizeof(DmAutomation));
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};
    UiMouse m = {.clicked = true, .left = true, .pos = {340, 180}};
    ed_automation_lane_click(e, lane, &v, 80, m, 1);
    int history = e->undo_count;
    m.clicked = false;
    m.pos = (UiPoint){380, 160};
    ed_automation_node_drag(e, lane, &v, 80, m);
    check(fabs(e->project.automation[0].beat - 1.5) < 1e-8 &&
              fabs(e->project.automation[1].beat - 3.5) < 1e-8,
          "group time delta is anchored to clicked selected point");
    check(e->project.automation[1].b - e->project.automation[0].b == 30,
          "group values retain their relative difference");
    m.left = false;
    m.released = true;
    ed_automation_node_drag(e, lane, &v, 80, m);
    check(e->event_selection.size == 2 * sizeof(DmAutomation) && e->undo_count == history + 1,
          "group drag keeps selection and one undo step");
    ed_undo(e, false);
    check(e->project.automation[0].beat == 1 && e->project.automation[1].beat == 3,
          "undo restores entire event group");
    dm_bytes_free(&e->event_selection);
    dm_bytes(&e->event_selection, e->project.automation, 2 * sizeof(DmAutomation));
    m = (UiMouse){.clicked = true, .left = true, .pos = {340, 180}};
    ed_automation_lane_click(e, lane, &v, 80, m, 1);
    m.clicked = false;
    m.alt = true;
    m.pos = (UiPoint){353, 180};
    ed_automation_node_drag(e, lane, &v, 80, m);
    check(fabs(e->project.automation[0].beat - 1.1625) < 1e-8 &&
              fabs(e->project.automation[1].beat - 3.1625) < 1e-8,
          "Alt group drag bypasses quarter-beat snapping");
    m.pos.x = -100;
    ed_automation_node_drag(e, lane, &v, 80, m);
    check(e->project.automation[0].beat == 0 && e->project.automation[1].beat == 2,
          "group time clamping preserves spacing at song start");
    m.left = false;
    m.released = true;
    ed_automation_node_drag(e, lane, &v, 80, m);
    fixture(e);
    e->project.patterns[0].kind = 1;
    e->automation_cc = DM_EVENT_BEND;
    e->edit_focus = FOCUS_EVENTS;
    dm_automation_add(&e->project, (DmAutomation){1, 0xe0, 0, 0, 64, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){3, 0xe0, 0, 0, 96, 0, 0, 0, 0, false});
    float first = ed_automation_pitch(e, e->project.automation[0]);
    float second = ed_automation_pitch(e, e->project.automation[1]);
    dm_bytes(&e->event_selection, e->project.automation, 2 * sizeof(DmAutomation));
    m = (UiMouse){.clicked = true, .left = true, .pos = {340, 180}};
    ed_automation_lane_click(e, lane, &v, 80, m, 1);
    m.clicked = false;
    m.pos.y -= 40;
    ed_automation_node_drag(e, lane, &v, 80, m);
    EventDragItem *items = (EventDragItem *)e->event_drag.data;
    check(fabs(ed_automation_pitch(e, e->project.automation[items[0].index]) - first - 2) < .02 &&
              fabs(ed_automation_pitch(e, e->project.automation[items[1].index]) - second - 2) <
                  .02,
          "standalone musical pitch group preserves semitone spacing");
    m.pos.x = 180;
    ed_automation_node_drag(e, lane, &v, 80, m);
    items = (EventDragItem *)e->event_drag.data;
    check(
        e->project.automation[items[0].index].beat == 0 &&
            e->project.automation[items[1].index].beat == 2 &&
            fabs(ed_automation_pitch(e, e->project.automation[items[0].index]) - first - 2) < .02 &&
            fabs(ed_automation_pitch(e, e->project.automation[items[1].index]) - second - 2) < .02,
        "group pitch remains correct after crossing inserted bend-range messages");
    m.left = false;
    m.released = true;
    ed_automation_node_drag(e, lane, &v, 80, m);
}

/* A copy takes the next free number after its pattern's base name; renaming starts a new base. */
static void duplicate_names(void) {
    DmProject p;
    dm_project_init(&p);
    int base = dm_pattern_add(&p, "Bass");
    assert(base >= 0);
    int two = dm_pattern_copy(&p, base), three = dm_pattern_copy(&p, two),
        four = dm_pattern_copy(&p, base);
    assert(!strcmp(p.patterns[two].name, "Bass-2") && !strcmp(p.patterns[three].name, "Bass-3") &&
           !strcmp(p.patterns[four].name, "Bass-4"));
    dm_pattern_name(&p, three, "Lead");
    int lead = dm_pattern_copy(&p, three);
    assert(!strcmp(p.patterns[lead].name, "Lead-2"));
    int independent = dm_pattern_add(&p, "Bass-5");
    assert(independent >= 0);
    int next = dm_pattern_copy(&p, base);
    assert(!strcmp(p.patterns[next].name, "Bass-6"));
    DmBytes bytes = {0};
    DmProject q;
    dm_project_init(&q);
    char error[256];
    assert(dm_pack(&p, &bytes) && dm_unpack(&q, bytes.data, bytes.size, error, sizeof(error)));
    assert(!strcmp(q.patterns[next].base_name, "Bass") && q.patterns[next].duplicate_index == 6);
    dm_project_free(&p);
    dm_project_free(&q);
    dm_bytes_free(&bytes);
    puts(
        "PASS tracked pattern bases, sequential duplicate names, rename reset, collisions and persistence");
}

static void pattern_moves(void) {
    DmProject p;
    dm_project_init(&p);

    for (int i = 0; i < 4; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "P%d", i);
        assert(dm_pattern_add(&p, name) == i);
    }

    assert(dm_clip_add(&p, (DmClip){.pattern = 0}) && dm_clip_add(&p, (DmClip){.pattern = 3}));
    check(dm_pattern_move(&p, 0, 3) == 2 && !strcmp(p.patterns[2].name, "P0") &&
              !strcmp(p.patterns[0].name, "P1"),
          "a pattern moves in front of the target pattern");
    check(p.clips[0].pattern == 2 && p.clips[1].pattern == 3, "placements follow moved patterns");
    check(dm_pattern_move(&p, 3, 0) == 0 && !strcmp(p.patterns[0].name, "P3") &&
              p.clips[1].pattern == 0 && p.clips[0].pattern == 3,
          "moving a pattern forward shifts the ones it passes");
    dm_project_free(&p);
}

static bool note_event(DmExportEvent event) {
    int type = event.status & MIDI_STATUS_MASK;
    return type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF;
}

static bool control_event(DmExportEvent event) {
    return event.status < MIDI_SYSTEM && !note_event(event);
}

static bool same_control(DmExportEvent a, DmExportEvent b) {
    int type = a.status & MIDI_STATUS_MASK;
    return a.status == b.status && a.port == b.port &&
           ((type != MIDI_CONTROL && type != MIDI_KEY_PRESSURE) || a.a == b.a);
}

static int control_at(const DmExportEvent *events, size_t count, DmExportEvent key) {
    int value = -1;

    for (size_t i = 0; i < count && events[i].tick <= key.tick; ++i) {
        DmExportEvent a = events[i];

        if (same_control(a, key)) {
            int type = a.status & MIDI_STATUS_MASK;
            value = type == MIDI_PITCH_BEND ? (int)(a.a + (a.b << 7))
                    : type == MIDI_PROGRAM || type == MIDI_CHANNEL_PRESSURE ? (int)a.a
                                                                            : (int)a.b;
        }
    }

    return value;
}

static DmExportEvent *export_fixture(const DmProject *p, size_t *count) {
    DmExportEvent *events = NULL;
    uint64_t duration;
    char error[256] = {0};
    check(dm_export_timeline(p, &events, count, &duration, error, sizeof(error)), error);
    return events;
}

/* Compare controller states within one MIDI unit, ignoring redundant messages and source
   identities. The cut instant is checked structurally: an interpolated boundary value replaces
   playback's replay of pre-offset messages. Notes must match throughout the song. */
static void unique_playback(const DmProject *p, const DmExportEvent *before, size_t before_count,
                            uint64_t control_end, uint64_t cut_tick) {
    size_t after_count;
    DmExportEvent *after = export_fixture(p, &after_count);
    size_t a = 0, b = 0;
    bool same = true;

    for (;;) {
        while (a < before_count && !note_event(before[a])) {
            ++a;
        }

        while (b < after_count && !note_event(after[b])) {
            ++b;
        }

        if (a == before_count || b == after_count) {
            same &= a == before_count && b == after_count;
            break;
        }

        same &= before[a].tick == after[b].tick && before[a].status == after[b].status &&
                before[a].a == after[b].a && before[a].b == after[b].b &&
                before[a].port == after[b].port;
        ++a;
        ++b;
    }

    check(same, "making a slice unique preserves all song notes and edge retriggers");
    same = true;

    for (int pass = 0; pass < 2; ++pass) {
        const DmExportEvent *events = pass ? after : before;
        size_t count = pass ? after_count : before_count;

        for (size_t i = 0; i < count && events[i].tick < control_end; ++i) {
            if (events[i].tick != cut_tick && control_event(events[i]) &&
                abs(control_at(before, before_count, events[i]) -
                    control_at(after, after_count, events[i])) > 1) {
                fprintf(stderr,
                        "controller mismatch tick %llu status %x control %u port %u: %d / %d\n",
                        (unsigned long long)events[i].tick, events[i].status, events[i].a,
                        events[i].port, control_at(before, before_count, events[i]),
                        control_at(after, after_count, events[i]));
                same = false;
                break;
            }
        }
    }

    check(same, "making a slice unique preserves controller values through the fitted span");
    free(after);
}

static void unique_slices(Editor *e) {
    ed_new_project(e);
    DmProject *p = &e->project;
    DmPattern *source = &p->patterns[0];
    source->color = 9;
    source->port = 3;
    source->graph_lanes[MIDI_CC_MODULATION] = DM_GRAPH_DEFAULT;
    check(
        dm_note_add(source,
                    (DmNote){.beat = 0, .length = 1, .key = 55, .velocity = 90, .port = 3}) &&
            dm_note_add(source, (DmNote){.beat = 1.5,
                                         .length = 1,
                                         .key = 60,
                                         .velocity = 91,
                                         .port = 3,
                                         .bend_id = 7}) &&
            dm_note_add(
                source,
                (DmNote){.beat = 2.5, .length = .5, .key = 62, .velocity = 92, .port = 3}) &&
            dm_note_add(source,
                        (DmNote){.beat = 3.5, .length = 2, .key = 64, .velocity = 93, .port = 3}) &&
            dm_note_add(source,
                        (DmNote){.beat = 6, .length = 2, .key = 67, .velocity = 94, .port = 3}),
        "slice notes include both crossing edges and notes outside the middle window");
    DmAutomation a = {.pattern = 0,
                      .type = MIDI_CONTROL,
                      .a = MIDI_CC_EXPRESSION,
                      .b = 20,
                      .curve = 1,
                      .bend = 24,
                      .port = 3};
    check(dm_automation_add(p, a), "CC curve begins before the cut");
    a.beat = 6;
    a.b = 110;
    a.bend = 0;
    check(dm_automation_add(p, a), "CC curve ends beyond the cut");
    a = (DmAutomation){.pattern = 0,
                       .beat = 1.5,
                       .type = MIDI_PITCH_BEND,
                       .b = 64,
                       .curve = 2,
                       .anchor = 60,
                       .port = 3,
                       .bend_note = 7,
                       .bend_root = DM_BEND_ROOT_PITCH};
    check(dm_automation_add(p, a), "crossing note has an attached bend root");
    a.beat = 2.5;
    a.b = 80;
    a.bend_note = 0;
    a.bend_root = DM_BEND_ROOT_NONE;
    check(dm_automation_add(p, a), "attached pitch curve has an endpoint");
    a.beat = 3;
    a.b = 64;
    a.curve = -2;
    check(dm_automation_add(p, a), "attached pitch curve restores");

    for (int i = 0; i < 3; ++i) {
        a = (DmAutomation){.pattern = 0,
                           .beat = (double[]){1.8, 2.5, 3.75}[i],
                           .type = MIDI_CONTROL,
                           .a = MIDI_CC_MODULATION,
                           .b = 50 + 20 * i,
                           .port = 3};
        check(dm_automation_add(p, a), "graph bars straddle and end inside the slice");
    }

    dm_patterns_fit(p);
    check(dm_clip_add(p, (DmClip){.pattern = 0, .beat = 8, .channel = 0}), "slice placement");
    ed_split_clip(e, 0, 10);
    ed_split_clip(e, 1, 12);
    check(p->clip_count == 3 && p->clips[1].offset == 2 && p->clips[1].length == 2,
          "two cuts make a middle window into the original pattern");
    size_t before_count, pattern_count = p->pattern_count, automation_count = p->automation_count;
    DmExportEvent *before = export_fixture(p, &before_count);
    ed_checkpoint(e);
    int id = dm_clip_make_unique(p, 1);
    check(id >= 0, "make the middle placement unique");
    ed_commit(e);
    DmPattern *unique = &p->patterns[id];
    check(unique->count == 3 && unique->notes[0].beat == 0 && unique->notes[0].length == .5 &&
              unique->notes[1].beat == .5 && unique->notes[2].beat == 1.5 &&
              unique->notes[2].length == .5 && unique->length == 2 && p->clips[1].pattern == id &&
              p->clips[1].offset == 0 && p->clips[1].length == 0,
          "unique middle notes start at zero, cut both edges and follow fitted length");
    check(unique->color == 9 && unique->port == 3 &&
              unique->graph_lanes[MIDI_CC_MODULATION] == DM_GRAPH_DEFAULT &&
              !strcmp(unique->name, "Pattern 1-2") && unique->uid != p->patterns[0].uid &&
              p->patterns[0].count == 5,
          "unique slice keeps copy metadata, naming and the untouched source");
    bool attached = false, bounded = true;

    for (size_t i = 0; i < p->automation_count; ++i) {
        a = p->automation[i];

        if (a.pattern == id) {
            bounded &= a.beat >= 0 && a.beat <= 2;
            attached |= a.beat == 0 && a.bend_note == 7 && unique->notes[0].bend_id == 7;
        }
    }

    check(bounded && attached, "slice automation stays inside its window with bends attached");
    unique_playback(p, before, before_count, UINT64_MAX, 10 * DM_PPQ);

    ed_undo(e, false);
    check(p->pattern_count == pattern_count && p->automation_count == automation_count &&
              p->clips[1].pattern == 0 && p->clips[1].offset == 2 && p->clips[1].length == 2,
          "undo restores the slice, pattern list and all automation");
    unique_playback(p, before, before_count, UINT64_MAX, 10 * DM_PPQ);
    ed_undo(e, true);
    check(p->clips[1].pattern == id && !p->clips[1].offset && !p->clips[1].length,
          "redo restores the unique placement and its trimmed pattern");
    unique_playback(p, before, before_count, UINT64_MAX, 10 * DM_PPQ);
    unique = &p->patterns[id];
    unique->count = 1;
    dm_patterns_fit(p);
    check(unique->length == 1 && dm_clip_length(p, p->clips[1]) == 1,
          "editing the unique pattern refits its following placement");
    free(before);

    /* Automatic note fitting deliberately ignores trailing automation and silence. */
    ed_new_project(e);
    p = &e->project;
    check(dm_note_add(&p->patterns[0],
                      (DmNote){.beat = 2, .length = .5, .key = 60, .velocity = 100}) &&
              dm_note_add(&p->patterns[0],
                          (DmNote){.beat = 6, .length = 1, .key = 64, .velocity = 100}),
          "short fitted slice notes");
    dm_patterns_fit(p);
    a = (DmAutomation){.pattern = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40};
    check(dm_automation_add(p, a) && dm_clip_add(p, (DmClip){.pattern = 0}),
          "short fitted slice controls");
    ed_split_clip(e, 0, 2);
    ed_split_clip(e, 1, 4);
    before = export_fixture(p, &before_count);
    id = dm_clip_make_unique(p, 1);
    check(id >= 0 && p->patterns[id].length == 1 && !p->clips[1].length && !p->clips[1].offset,
          "a slice's silent tail does not pin the new pattern length");
    unique_playback(p, before, before_count, 3 * DM_PPQ, 2 * DM_PPQ);
    size_t after_count;
    DmExportEvent *after = export_fixture(p, &after_count);
    DmExportEvent key = {.tick = 3 * DM_PPQ, .status = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION};
    check(control_at(before, before_count, key) == 40 && control_at(after, after_count, key) == 127,
          "fitting a short slice deliberately releases sustained controls at its new end");
    free(before);
    free(after);
}

static void unique_graph_slice(Editor *e) {
    ed_new_project(e);
    DmProject *p = &e->project;
    p->patterns[0].graph_lanes[MIDI_CC_VOLUME] = DM_GRAPH_DEFAULT;
    check(dm_note_add(&p->patterns[0],
                      (DmNote){.beat = 1, .length = 4, .key = 60, .velocity = 100, .port = 4}) &&
              dm_port_mix_set(p, (DmPortMix){.port = 4, .channel = 0, .volume = 90, .pan = 64}),
          "partial graph cell has a separate port mixer default");
    DmAutomation a = {
        .pattern = 0, .beat = 1, .type = MIDI_CONTROL, .a = MIDI_CC_VOLUME, .b = 40, .port = 4};
    check(dm_automation_add(p, a), "last graph cell starts before the cut");
    a = (DmAutomation){.pattern = 0,
                       .beat = .75,
                       .type = MIDI_CONTROL,
                       .a = MIDI_CC_PORTAMENTO_TIME,
                       .b = 20,
                       .curve = 7,
                       .bend = 16,
                       .port = 4};
    check(dm_automation_add(p, a), "portamento curve begins before the cut");
    a.beat = 4;
    a.b = 80;
    a.bend = 0;
    check(dm_automation_add(p, a), "portamento curve ends beyond the cut");
    dm_patterns_fit(p);
    check(dm_clip_add(p, (DmClip){.pattern = 0, .beat = 2}), "partial graph placement");
    ed_split_clip(e, 0, 3.125);
    ed_split_clip(e, 1, 5.125);
    size_t before_count;
    DmExportEvent *before = export_fixture(p, &before_count);
    int id = dm_clip_make_unique(p, 1);
    check(id >= 0 && p->patterns[id].length == 2, "make a fractional graph cell slice unique");
    unique_playback(p, before, before_count, 0, UINT64_MAX);
    bool bar = false;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation point = p->automation[i];

        if (point.pattern == id && point.a == MIDI_CC_VOLUME) {
            bar |= point.beat == 0 && point.curve == 0 && point.b == 40;
        }
    }

    check(bar, "a graph bar before the offset becomes an ordinary bar at zero");
    size_t after_count;
    DmExportEvent *after = export_fixture(p, &after_count);
    DmExportEvent key = {
        .tick = (uint64_t)(3.375 * DM_PPQ), .status = MIDI_CONTROL, .a = MIDI_CC_VOLUME, .port = 4};
    check(control_at(after, after_count, key) == 90,
          "the carried graph bar lasts its normal cell and restores to the port mixer value");
    free(before);
    free(after);
}

static void unique_automation_slices(Editor *e) {
    for (int mode = 0; mode < 6; ++mode) {
        ed_new_project(e);
        DmProject *p = &e->project;
        DmPattern *source = &p->patterns[0];
        p->lane_count[0] = 1;
        source->kind = 1;
        source->mode = mode % 3 != 0;
        source->event = mode % 3 == 2 ? DM_EVENT_BEND : MIDI_CC_EXPRESSION;
        source->length = 8;
        source->speed = 1.7;
        source->skew = -.23;
        source->tension = .14;
        source->pulse = .42;
        source->amount = .6;
        source->port = 2;
        source->color = 4;
        source->graph_lanes[MIDI_CC_EXPRESSION] = DM_GRAPH_DEFAULT;

        if (mode % 3 == 0) {
            DmAutomation a = {.pattern = 0,
                              .type = MIDI_CONTROL,
                              .a = MIDI_CC_EXPRESSION,
                              .b = 15,
                              .curve = 1,
                              .bend = mode == 3 ? 3 : 21,
                              .port = 2};
            check(dm_automation_add(p, a), "standalone curve start");
            a.beat = 8;
            a.b = 115;
            a.bend = 0;
            check(dm_automation_add(p, a), "standalone curve end");

            if (mode == 3) {
                a.beat = 1;
                a.b = 30;
                a.bend = 9;
                a.hold = a.ramp = true;
                a.anchor = 54;
                check(dm_automation_add(p, a), "interior held node with authoring metadata");
                a.beat = 2;
                a.b = 40;
                a.bend = 11;
                a.hold = false;
                check(dm_automation_add(p, a), "interior curve resumes before the second cut");
            }
        }

        check(dm_clip_add(p, (DmClip){.pattern = 0, .channel = 0, .lane = 1, .beat = 4}),
              "automation placement");
        ed_split_clip(e, 0, 4.37);
        ed_split_clip(e, 1, mode < 3 ? 6.37 : 6.495);
        size_t before_count;
        DmExportEvent *before = export_fixture(p, &before_count);
        int id = dm_clip_make_unique(p, 1);
        check(id >= 0 && p->patterns[id].kind == 1 &&
                  p->patterns[id].event == (mode % 3 == 2 ? DM_EVENT_BEND : MIDI_CC_EXPRESSION) &&
                  p->patterns[id].port == 2 && p->patterns[id].color == 4,
              "automation copies keep their kind, event, port and color");

        if (mode % 3) {
            check(p->patterns[id].mode == 1 && p->patterns[id].length == 8 &&
                      fabs(p->clips[1].offset - .37) < 1e-9 &&
                      fabs(p->clips[1].length - (mode < 3 ? 2 : 2.125)) < 1e-9,
                  "sliced LFOs stay editable LFOs and preserve their placement window");
        } else {
            size_t nodes = 0, original_nodes = 0;

            for (size_t i = 0; i < p->automation_count; ++i) {
                DmAutomation point = p->automation[i];

                if (point.pattern == 0) {
                    ++original_nodes;
                } else if (point.pattern == id) {
                    ++nodes;
                    check(point.curve == 1, "a sliced curve keeps its original editable identity");

                    if (mode == 3 && fabs(point.beat - .63) < 1e-9) {
                        check(
                            point.hold && point.ramp && point.bend == 9 && point.anchor == 54 &&
                                point.b == 30 && point.port == 2,
                            "an interior node preserves hold, ramp, bend, anchor, value and port");
                    }
                }
            }

            check(nodes >= 2 && nodes <= original_nodes + 2 && !p->patterns[id].mode &&
                      !p->clips[1].offset && !p->clips[1].length &&
                      p->patterns[id].length == (mode < 3 ? 2 : 3),
                  "a curve crossing both cuts adds at most two boundary nodes and fits");
            bool start = false, end = false;
            double offset = .37, length = mode < 3 ? 2 : 2.125;

            for (size_t i = 0; i < p->automation_count; ++i) {
                DmAutomation first = p->automation[i];

                if (first.pattern != 0 || !dm_automation_curves(first)) {
                    continue;
                }

                for (size_t j = i + 1; j < p->automation_count; ++j) {
                    DmAutomation last = p->automation[j];

                    if (last.pattern != 0 || last.curve != first.curve) {
                        continue;
                    }

                    for (size_t k = 0; k < p->automation_count; ++k) {
                        DmAutomation point = p->automation[k];

                        if (point.pattern != id) {
                            continue;
                        }

                        if (fabs(point.beat) < 1e-9 && first.beat < offset && last.beat > offset) {
                            start =
                                point.b ==
                                (int)llround(dm_curve_value(
                                    first, last, (offset - first.beat) / (last.beat - first.beat)));
                        }

                        if (fabs(point.beat - length) < 1e-9 && first.beat < offset + length &&
                            last.beat > offset + length) {
                            end = point.b ==
                                  (int)llround(dm_curve_value(first, last,
                                                              (offset + length - first.beat) /
                                                                  (last.beat - first.beat)));
                        }
                    }

                    break;
                }
            }

            check(start && end, "boundary nodes keep the source curve's interpolated edge values");
        }
        unique_playback(p, before, before_count, UINT64_MAX, (uint64_t)llround(4.37 * DM_PPQ));
        DmBytes bytes = {0};
        DmProject loaded;
        dm_project_init(&loaded);
        char error[256] = {0};
        check(dm_pack(p, &bytes) &&
                  dm_unpack(&loaded, bytes.data, bytes.size, error, sizeof(error)),
              "trimmed automation and intact LFOs remain valid saved patterns");
        unique_playback(&loaded, before, before_count, UINT64_MAX,
                        (uint64_t)llround(4.37 * DM_PPQ));
        dm_project_free(&loaded);
        dm_bytes_free(&bytes);
        free(before);
    }

    ed_new_project(e);
    DmProject *p = &e->project;
    DmPattern *source = &p->patterns[0];
    source->kind = source->mode = 1;
    source->length = 8;
    source->event = MIDI_CC_MODULATION;
    check(dm_clip_add(p, (DmClip){.pattern = 0, .length = 8, .lane = 1}),
          "unsliced explicit placement");
    int id = dm_clip_make_unique(p, 0);
    check(id >= 0 && p->patterns[id].mode == 1 && p->patterns[id].length == 8 &&
              p->clips[0].length == 8 && !p->clips[0].offset,
          "unsliced placements retain the full-copy behavior and LFO mode");
    check(dm_clip_make_unique(p, p->clip_count) < 0, "invalid placement cannot make a pattern");
}

int main(void) {
    Editor *e = test_editor_open("pattern_workflow",
                                 (TestDesktop){.soundfont = TEST_SOUNDFONT, .paused = true});
    SDL_Renderer *r = e->renderer;
    ui_frame();
    ui_root(0, 900);
    unique_slices(e);
    unique_graph_slice(e);
    unique_automation_slices(e);
    sizing_names(e);
    brush_velocity(e);
    linked_copy(e);
    recording(e);
    group_points(e);
    ui_end();
    ui_render(r);
    fixture(e);
    test_pattern_workflow_ui(e, check);
    fixture(e);
    test_pattern_rename_ui(e);
    duplicate_names();
    pattern_moves();
    printf("PASS pattern_workflow: %d checks\n", checks);
    test_editor_close(e);
}
