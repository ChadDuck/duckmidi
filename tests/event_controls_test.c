#include "editor/internal.h"
#define TEST_NAME "event controls"
#include "test_common.h"

extern void test_event_controls_ui(Editor *, void (*)(bool, const char *));
extern void test_event_controls_audio(DmAudio *, void (*)(bool, const char *));

static int scheduled_value(const DmSchedule *s, int cc, double seconds) {
    int value = -1;

    for (size_t i = 0; i < s->count; ++i) {
        DmScheduled a = s->items[i];

        if (a.seconds > seconds + 1e-8) {
            break;
        }

        if (a.status == 0xb0 && a.a == cc) {
            value = a.b;
        }
    }

    return value;
}

static void fixture(Editor *e, int cc, bool standalone) {
    editor_smoke(e);
    e->project.automation_count = e->project.clip_count = 0;
    e->selected_pattern = e->pattern_channel = 0;
    DmPattern *p = ed_pattern(e);
    p->count = 0;
    p->kind = standalone;
    p->mode = 0;
    p->event = cc;
    p->length = 4;
    dm_note_add(p, (DmNote){0, 4, 60, 100, 0});
    e->automation_cc = cc;
    e->automation_node = -1;
    e->automation_all = false;
    e->event_selection.size = e->event_drag.size = 0;
    e->last_automation_valid = false;
    e->drag = DRAG_NONE;
    e->edit_focus = FOCUS_EVENTS;
    e->tool = TOOL_SELECT;
    e->project.channels[0].volume = 93;
    e->project.channels[0].pan = 37;

    if (standalone) {
        e->project.lane_count[0] = 1;
    }

    dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0, .lane = standalone, .length = 4});
}

static void continuous(Editor *e) {
    const int controls[] = {7, 10, 91, 93, 74, 71};

    for (int k = 0; k < 6; ++k) {
        int cc = controls[k];
        fixture(e, cc, true);
        ed_automation_line(e, 1, 3, 20, 100, cc);
        DmSchedule s = {0};
        check(dm_schedule(&e->project, &s), "schedule continuous section");
        check(scheduled_value(&s, cc, 1) == 60, "continuous section interpolates");
        int baseline = cc == 7 ? 93 : cc == 10 ? 37 : cc >= 70 && cc <= 79 ? 64 : 0;
        check(scheduled_value(&s, cc, 1.5) == baseline, "section restores channel baseline");
        dm_schedule_free(&s);
        ed_pattern(e)->mode = 1;
        ed_pattern(e)->amount = 1;
        check(dm_schedule(&e->project, &s), "schedule new controller LFO");
        check(scheduled_value(&s, cc, .05) != scheduled_value(&s, cc, .25),
              "LFO modulates controller");
        check(scheduled_value(&s, cc, 2) == baseline, "LFO placement restores baseline");
        dm_schedule_free(&s);
    }
}

static void imported_mix_controls(Editor *e) {
    static const uint8_t midi[] = {
        'M',
        'T',
        'h',
        'd',
        0,
        0,
        0,
        6,
        0,
        0,
        0,
        1,
        0,
        96,
        'M',
        'T',
        'r',
        'k',
        0,
        0,
        0,
        28,
        0,
        MIDI_CONTROL,
        MIDI_CC_VOLUME,
        93,
        0,
        MIDI_CONTROL,
        MIDI_CC_PAN,
        37,
        0,
        MIDI_NOTE_ON,
        60,
        100,
        96,
        MIDI_CONTROL,
        MIDI_CC_VOLUME,
        45,
        0,
        MIDI_CONTROL,
        MIDI_CC_PAN,
        80,
        96,
        MIDI_NOTE_OFF,
        60,
        0,
        0,
        MIDI_META,
        MIDI_META_END,
        0,
    };
    fixture(e, MIDI_CC_VOLUME, false);
    char error[256] = {0};
    check(dm_import(&e->project, midi, sizeof(midi), 0, 0, error, sizeof(error)), error);
    ed_open_pattern(e, 0, false);
    check(e->automation_cc == MIDI_CC_VOLUME && !ed_pattern(e)->kind &&
              ed_event_type_used(e, MIDI_CC_VOLUME) && ed_event_type_used(e, MIDI_CC_PAN),
          "opening an imported note pattern keeps its Volume lane and exposes both controls");

    ed_graph_mode_request(e, 0, 16);
    check(e->graph_switch && e->graph_switch_event == MIDI_CC_VOLUME,
          "converting imported Volume points uses the normal warning");
    e->graph_switch = false;
    ed_graph_mode(e, 0, 16);

    UiRect lane = {100, 100, 600, 200};
    View view = {.zoom = 1};
    UiPoint bar = {lane.x + 80, lane.y + lane.h - 8 - 64.5f * (lane.h - 8) / 127};
    ed_graph_paint(e, lane, &view, 80, bar, bar, true);
    DmSchedule schedule = {0};
    check(dm_schedule(&e->project, &schedule), "schedule edited imported controls");
    check(ed_pattern(e)->graph_hold[MIDI_CC_VOLUME] &&
              scheduled_value(&schedule, MIDI_CC_VOLUME, .55) == 64 &&
              scheduled_value(&schedule, MIDI_CC_VOLUME, .65) == 64 &&
              scheduled_value(&schedule, MIDI_CC_PAN, .65) == 80,
          "the Volume bar holds after conversion while imported Pan keeps its Point value");
    dm_schedule_free(&schedule);

    e->automation_cc = MIDI_CC_PAN;
    check(!ed_graph_resolution(e), "Volume's Graph conversion leaves Pan in Point");
    ed_graph_mode(e, 0, 32);
    ed_open_pattern(e, 0, false);
    check(e->automation_cc == MIDI_CC_PAN && ed_graph_resolution(e) == 32 &&
              dm_graph_resolution(ed_pattern(e), MIDI_CC_VOLUME) == 16 && !ed_pattern(e)->mode,
          "Pan and Volume keep independent Graph settings in the note pattern");
}

static void switches_and_glide(Editor *e) {
    UiRect lane = {100, 100, 600, 200};
    View v = {.zoom = 1};

    for (int cc = 64; cc <= 66; cc += 2) {
        for (int kind = 0; kind <= 1; ++kind) {
            fixture(e, cc, kind);
            UiMouse m = {.clicked = true, .left = true, .pos = {180, 200}};
            ed_automation_input(e, lane, &v, 80, m, true, -1);
            m.clicked = false;
            m.left = false;
            m.released = true;
            m.pos.x = 340;
            ed_automation_input(e, lane, &v, 80, m, true, -1);
            check(e->project.automation_count == 3 && e->drag == DRAG_NONE,
                  "pedal drag creates bounded interval in both editors");
            DmSchedule s = {0};
            check(dm_schedule(&e->project, &s), "schedule pedal interval");
            check(scheduled_value(&s, cc, .75) == 127 && scheduled_value(&s, cc, 1.5) == 0,
                  "pedal enables and disables without interpolating");

            for (size_t i = 0; i < s.count; ++i) {
                if (s.items[i].status == 0xb0 && s.items[i].a == cc) {
                    check(s.items[i].b == 0 || s.items[i].b == 127, "pedal values are binary");
                }
            }

            dm_schedule_free(&s);
            e->automation_all = true;
            DmProject copy;
            dm_project_init(&copy);
            check(ed_copy_events(e, &copy) && copy.automation_count == 3,
                  "copying pedal interval preserves exactly its endpoints and release");
            dm_project_free(&copy);
            e->automation_all = false;
            UiPoint at = ed_automation_point(e, e->project.automation[0], lane, &v, 80);
            check(at.y == 200, "pedal nodes stay on a single line");
            e->automation_all = true;
            ed_delete_events(e);
            check(e->project.automation_count == 0, "deleting pedal removes restoration too");
        }
    }

    fixture(e, 5, true);
    /* Dragging past the click buffer draws a section; release places it. */
    UiMouse m = {.clicked = true, .left = true, .pos = {180, 180}};
    ed_automation_lane_click(e, lane, &v, 80, m, -1);
    check(e->drag == DRAG_EVENT_PRESS && e->project.automation_count == 0,
          "a press waits to tell a click from a drag");
    m.clicked = false;
    m.pos = (UiPoint){200, 180};
    ed_automation_press(e, lane, &v, 80, m);
    check(e->drag == DRAG_EVENT_LINE, "moving past the buffer starts a line");
    m.left = false;
    m.released = true;
    ed_automation_drag(e, lane, &v, 80, m);
    check(e->project.automation_count == 3, "dragged portamento line creates bounded section");
    m = (UiMouse){.clicked = true, .shift = true, .pos = {340, 140}};
    ed_automation_lane_click(e, lane, &v, 80, m, -1);
    ed_automation_press(e, lane, &v, 80, (UiMouse){.released = true, .pos = {340, 140}});
    check(e->project.automation_count == 4, "portamento connects additional points");
    DmSchedule s = {0};
    check(dm_schedule(&e->project, &s), "schedule automatic portamento switch");
    check(scheduled_value(&s, 65, .75) == 127 && scheduled_value(&s, 65, 1.5) == 0,
          "glide switch follows section boundaries");
    check(scheduled_value(&s, 5, 1.5) == 0, "glide time restores at endpoint");
    dm_schedule_free(&s);
    e->project.clips[0].length = 2;
    check(dm_schedule(&e->project, &s) && scheduled_value(&s, 65, 1) == 0,
          "trimmed placement disables portamento");
    dm_schedule_free(&s);
    /* A note placement can finish before an authored pedal/glide section. */
    fixture(e, 64, false);
    ed_pattern(e)->count = 0;
    dm_note_add(ed_pattern(e), (DmNote){0, 2, 60, 100, 0});
    e->project.clips[0].length = 0;
    ed_automation_line(e, 1, 6, 127, 127, 64);
    check(dm_schedule(&e->project, &s) && scheduled_value(&s, 64, .75) == 127 &&
              scheduled_value(&s, 64, 1) == 0,
          "automatic note placement releases a truncated pedal");
    dm_schedule_free(&s);
    fixture(e, 5, true);
    ed_automation_line(e, 1, 3, 10, 90, 5);
    DmBytes packed = {0};
    DmProject loaded;
    dm_project_init(&loaded);
    char error[256];
    check(dm_pack(&e->project, &packed) &&
              dm_unpack(&loaded, packed.data, packed.size, error, sizeof(error)),
          "new controls use current project schema");
    dm_bytes_free(&packed);
    dm_project_free(&loaded);
}

static int exported_value_at(const DmExportEvent *events, size_t count, uint32_t status, int cc,
                             uint64_t tick) {
    int value = -1;

    for (size_t i = 0; i < count; ++i) {
        if (events[i].tick > tick) {
            break;
        }

        if (events[i].status == status && (cc < 0 || events[i].a == (uint32_t)cc)) {
            value = cc < 0 ? (int)(events[i].a + (events[i].b << 7)) : (int)events[i].b;
        }
    }

    return value;
}

static void check_events_equal_after(const DmExportEvent *before, size_t before_count,
                                     const DmExportEvent *after, size_t after_count,
                                     uint64_t after_tick) {
    size_t i = 0, j = 0;

    while (i < before_count && before[i].tick <= after_tick) {
        ++i;
    }

    while (j < after_count && after[j].tick <= after_tick) {
        ++j;
    }

    while (i < before_count && j < after_count) {
        check(before[i].tick == after[j].tick && before[i].status == after[j].status &&
                  before[i].a == after[j].a && before[i].b == after[j].b,
              "events after section match before edit");
        ++i;
        ++j;
    }

    check(i == before_count && j == after_count, "same number of events after section");
}

static void section_ends(Editor *e) {
    char error[256] = {0};
    uint64_t dur = 0;

    /* a. Line section drawn over imported CC data: events after the section match before edit. */
    fixture(e, MIDI_CC_EXPRESSION, false);
    ed_pattern(e)->length = 8;
    ed_pattern(e)->notes[0].length = 8;
    e->project.clips[0].length = 8;
    dm_automation_add(
        &e->project,
        (DmAutomation){.beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40});
    dm_automation_add(
        &e->project,
        (DmAutomation){.beat = 2, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 80});
    dm_automation_add(
        &e->project,
        (DmAutomation){.beat = 6, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 100});
    DmExportEvent *before = NULL, *after = NULL;
    size_t before_count = 0, after_count = 0;
    check(dm_export_timeline(&e->project, &before, &before_count, &dur, error, sizeof(error)),
          error);
    ed_automation_line(e, 1, 4, 20, 120, MIDI_CC_EXPRESSION);
    check(dm_export_timeline(&e->project, &after, &after_count, &dur, error, sizeof(error)), error);
    check(exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_EXPRESSION, 4 * DM_PPQ) == 80,
          "Line section over CC restores replaced value at section end");
    check_events_equal_after(before, before_count, after, after_count, 4 * DM_PPQ);
    free(before);
    free(after);

    /* b. Discrete bend at bar 8 (beat 28) held to bar 12, redrawn over beats 26-30. */
    fixture(e, DM_EVENT_BEND, false);
    ed_pattern(e)->length = 48;
    ed_pattern(e)->notes[0].length = 48;
    e->project.clips[0].length = 48;
    dm_automation_add(&e->project,
                      (DmAutomation){.beat = 28, .type = MIDI_PITCH_BEND, .a = 0, .b = 96});
    before = after = NULL;
    before_count = after_count = 0;
    check(dm_export_timeline(&e->project, &before, &before_count, &dur, error, sizeof(error)),
          error);
    e->bend_range = 2;
    ed_automation_line(e, 26, 30, MIDI_BEND_CENTER, 10000, DM_EVENT_BEND);
    check(dm_export_timeline(&e->project, &after, &after_count, &dur, error, sizeof(error)), error);
    check(exported_value_at(after, after_count, MIDI_PITCH_BEND, -1, 30 * DM_PPQ) == (96 << 7),
          "redrawn bend section carries bar-8 value after beat 30");
    check_events_equal_after(before, before_count, after, after_count, 30 * DM_PPQ);
    free(before);
    free(after);

    /* c. Sustain section starting while pedal is down ends with CC64 = 0. */
    fixture(e, MIDI_CC_SUSTAIN, false);
    dm_automation_add(
        &e->project,
        (DmAutomation){.beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_SUSTAIN, .b = 127});
    ed_automation_line(e, 1, 3, 127, 127, MIDI_CC_SUSTAIN);
    after = NULL;
    after_count = 0;
    check(dm_export_timeline(&e->project, &after, &after_count, &dur, error, sizeof(error)), error);
    check(exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_SUSTAIN, 3 * DM_PPQ) == 0,
          "sustain section ends with pedal up even when pedal was down before it");
    free(after);

    /* e. Bend-range section setting range 12 over earlier range 24 returns to 24. */
    fixture(e, DM_EVENT_BEND, false);
    ed_pattern(e)->length = 8;
    ed_pattern(e)->notes[0].length = 8;
    e->project.clips[0].length = 8;
    ed_curve_range(e, 0, 24, 0, 127, 127, 0);
    e->bend_range = 12;
    ed_automation_line(e, 2, 4, MIDI_BEND_CENTER, 10000, DM_EVENT_BEND);
    after = NULL;
    after_count = 0;
    check(dm_export_timeline(&e->project, &after, &after_count, &dur, error, sizeof(error)), error);
    check(exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_DATA, 2 * DM_PPQ) == 12,
          "bend-range section sets range 12");
    check(exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_DATA, 4 * DM_PPQ) == 24,
          "bend-range section returns to earlier range 24");
    free(after);

    /* f. A section starting on the beat where another ends returns to what that one returned
       to: the lane is drawn at the value that plays. */
    fixture(e, MIDI_CC_EXPRESSION, false);
    ed_pattern(e)->length = 16;
    ed_pattern(e)->notes[0].length = 16;
    e->project.clips[0].length = 16;
    const DmAutomation points[] = {
        {.beat = 0, .b = 40},
        {.beat = 6, .b = 100, .curve = 1},
        {.beat = 10, .b = 20, .curve = 1},
        {.beat = 10, .b = 0, .curve = -1},
        {.beat = 10, .b = 60, .curve = 2},
        {.beat = 13, .b = 110, .curve = 2},
        {.beat = 13, .b = 0, .curve = -2},
    };

    for (size_t i = 0; i < sizeof(points) / sizeof(*points); ++i) {
        DmAutomation point = points[i];
        point.type = MIDI_CONTROL;
        point.a = MIDI_CC_EXPRESSION;
        dm_automation_add(&e->project, point);
    }

    int returned = -1;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].curve == -2) {
            check(ed_restoration_value(e, i, &returned), "back-to-back section returns");
        }
    }

    after = NULL;
    after_count = 0;
    check(dm_export_timeline(&e->project, &after, &after_count, &dur, error, sizeof(error)), error);
    check(returned == 40 && exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_EXPRESSION,
                                              13 * DM_PPQ) == 40,
          "a section starting where another ends returns to what that one returned to");
    free(after);
}

/* The project's exported events; free with free(). */
static DmExportEvent *exported(Editor *e, size_t *count) {
    DmExportEvent *events = NULL;
    uint64_t duration = 0;
    char error[256] = {0};
    check(dm_export_timeline(&e->project, &events, count, &duration, error, sizeof(error)), error);
    return events;
}

static size_t count_points(Editor *e, int type, int curve_sign) {
    size_t n = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        n += a.type == type && (curve_sign > 0 ? a.curve > 0 : curve_sign < 0 ? a.curve < 0 : 1);
    }

    return n;
}

/* Section ends for every kind of point (W14's list): what plays after the edit is what played
   before it, and drawing never disturbs another setting or another section. */
static void section_end_kinds(Editor *e) {
    size_t before_count = 0, after_count = 0;

    /* A standalone point holds until the next point; later events are unchanged. */
    fixture(e, MIDI_CC_EXPRESSION, false);
    ed_pattern(e)->length = 8;
    ed_pattern(e)->notes[0].length = 8;
    e->project.clips[0].length = 8;
    dm_automation_add(
        &e->project,
        (DmAutomation){.beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40});
    dm_automation_add(
        &e->project,
        (DmAutomation){.beat = 6, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 80});
    DmExportEvent *before = exported(e, &before_count);
    dm_automation_add(
        &e->project,
        (DmAutomation){.beat = 2, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 70});
    DmExportEvent *after = exported(e, &after_count);
    check(exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_EXPRESSION, 5 * DM_PPQ) == 70,
          "a standalone point holds until the next point");
    check_events_equal_after(before, before_count, after, after_count, 6 * DM_PPQ - 1);
    free(before);
    free(after);

    /* A program change holds until the next one; later events are unchanged. */
    fixture(e, DM_EVENT_PROGRAM, false);
    ed_pattern(e)->length = 8;
    ed_pattern(e)->notes[0].length = 8;
    e->project.clips[0].length = 8;
    dm_automation_add(&e->project, (DmAutomation){.beat = 6, .type = MIDI_PROGRAM, .a = 30});
    before = exported(e, &before_count);
    dm_automation_add(&e->project, (DmAutomation){.beat = 2, .type = MIDI_PROGRAM, .a = 12});
    after = exported(e, &after_count);
    check(exported_value_at(after, after_count, MIDI_PROGRAM, -1, 5 * DM_PPQ) == 12,
          "a program change holds until the next one");
    check_events_equal_after(before, before_count, after, after_count, 6 * DM_PPQ - 1);
    free(before);
    free(after);

    /* A channel pressure section over pressure points replaces them, keeps the last replaced
       value after it, and leaves a pitch bend inside it alone. */
    fixture(e, DM_EVENT_CHANNEL_PRESSURE, false);
    ed_pattern(e)->length = 8;
    ed_pattern(e)->notes[0].length = 8;
    e->project.clips[0].length = 8;
    dm_automation_add(&e->project,
                      (DmAutomation){.beat = 0, .type = MIDI_CHANNEL_PRESSURE, .a = 20});
    dm_automation_add(&e->project,
                      (DmAutomation){.beat = 2, .type = MIDI_CHANNEL_PRESSURE, .a = 90});
    dm_automation_add(&e->project,
                      (DmAutomation){.beat = 2, .type = MIDI_PITCH_BEND, .a = 0, .b = 80});
    before = exported(e, &before_count);
    ed_automation_line(e, 1, 3, 10, 50, DM_EVENT_CHANNEL_PRESSURE);
    after = exported(e, &after_count);
    bool bend_kept = false, pressure_inside = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        bend_kept |= a.type == MIDI_PITCH_BEND && a.beat == 2 && a.b == 80;
        pressure_inside |= a.type == MIDI_CHANNEL_PRESSURE && !a.curve && a.beat > 1 && a.beat < 3;
    }

    check(bend_kept && !pressure_inside,
          "a pressure section replaces pressure points only, leaving the bend in place");
    check(exported_value_at(after, after_count, MIDI_CHANNEL_PRESSURE, -1, 3 * DM_PPQ) == 90,
          "after a pressure section the last replaced value plays");
    check_events_equal_after(before, before_count, after, after_count, 3 * DM_PPQ);
    free(before);
    free(after);

    /* A bend Line with its own range: events after it are unchanged, range included. */
    fixture(e, DM_EVENT_BEND, false);
    ed_pattern(e)->length = 8;
    ed_pattern(e)->notes[0].length = 8;
    e->project.clips[0].length = 8;
    ed_curve_range(e, 0, 24, 0, 127, 127, 0);
    dm_automation_add(&e->project,
                      (DmAutomation){.beat = 6, .type = MIDI_PITCH_BEND, .a = 0, .b = 72});
    before = exported(e, &before_count);
    e->bend_range = 12;
    ed_automation_line(e, 2, 4, MIDI_BEND_CENTER, 10000, DM_EVENT_BEND);
    after = exported(e, &after_count);
    check(exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_DATA, 4 * DM_PPQ) == 24,
          "a bend Line with its own range returns to the earlier range");
    check_events_equal_after(before, before_count, after, after_count, 4 * DM_PPQ);
    free(before);
    free(after);

    /* A section drawn where another ends leaves the first one whole and returns to the value
       from before both. */
    fixture(e, MIDI_CC_EXPRESSION, false);
    ed_pattern(e)->length = 8;
    ed_pattern(e)->notes[0].length = 8;
    e->project.clips[0].length = 8;
    dm_automation_add(
        &e->project,
        (DmAutomation){.beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40});
    ed_automation_line(e, 1, 3, 100, 20, MIDI_CC_EXPRESSION);
    ed_automation_line(e, 3, 5, 60, 110, MIDI_CC_EXPRESSION);
    after = exported(e, &after_count);
    check(count_points(e, MIDI_CONTROL, 1) == 4 && count_points(e, MIDI_CONTROL, -1) == 2,
          "a section drawn where another ends keeps both sections whole");
    check(exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_EXPRESSION, 6 * DM_PPQ) == 40,
          "back-to-back drawn sections return to the value from before both");
    free(after);

    /* After a pedal section the pedal stays up only until the next pedal message: a Channel
       Event pressing it later in the placement takes effect then. */
    fixture(e, MIDI_CC_SUSTAIN, false);
    ed_pattern(e)->length = 8;
    ed_pattern(e)->notes[0].length = 8;
    e->project.clips[0].length = 8;
    ed_automation_line(e, 1, 2, 127, 127, MIDI_CC_SUSTAIN);
    uint8_t pedal[] = {MIDI_CONTROL, MIDI_CC_SUSTAIN, 127};
    DmBytes message = {0};
    check(dm_bytes(&message, pedal, sizeof(pedal)) && dm_event_add(&e->project, 3, 0, 0, &message),
          "Channel Event pedal down");
    dm_bytes_free(&message);
    after = exported(e, &after_count);
    check(
        exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_SUSTAIN, 2 * DM_PPQ + 1) == 0 &&
            exported_value_at(after, after_count, MIDI_CONTROL, MIDI_CC_SUSTAIN, 3 * DM_PPQ) == 127,
        "a pedal section ends with the pedal up until the next pedal message");
    free(after);
}

/* Program changes in the events editor are edited like every other event type: a click adds a
   point, a selected point is removed with Delete, and right-dragging erases points. */
static void program_change_points(Editor *e) {
    fixture(e, -1, true);
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};

    for (int i = 0; i < 3; ++i) {
        e->automation_node = -1;
        ed_automation_lane_click(e, lane, &v, 80,
                                 (UiMouse){.clicked = true, .pos = {140 + 80.0f * i, 180}}, -1);
        ed_automation_press(e, lane, &v, 80,
                            (UiMouse){.released = true, .pos = {140 + 80.0f * i, 180}});
    }

    size_t added = e->project.automation_count;
    check(added == 3, "left clicks add program change points");
    ed_automation_lane_click(e, lane, &v, 80,
                             (UiMouse){.clicked = true, .left = true, .pos = {140, 180}}, 0);
    ed_automation_node_drag(e, lane, &v, 80, (UiMouse){.released = true, .pos = {140, 180}});
    e->drag = DRAG_NONE;
    ed_shortcut(e, SDL_SCANCODE_DELETE, SDL_KMOD_NONE, false);
    check(e->project.automation_count == added - 1,
          "Delete removes a selected program change point");
    e->erase_active = false;
    ed_erase_node(e, 0);
    check(e->project.automation_count == added - 2,
          "right-drag erase removes program change points");
}

static void heard_dim_stretch(Editor *e) {
    fixture(e, MIDI_CC_EXPRESSION, true);
    ed_pattern(e)->length = 8;
    e->project.clips[0].length = 8;
    e->project.lane_count[0] = 2;

    int a = dm_pattern_add(&e->project, "Under");
    check(a >= 0, "add underlying automation pattern");
    e->project.patterns[a].kind = 1;
    e->project.patterns[a].event = MIDI_CC_EXPRESSION;
    e->project.patterns[a].length = 8;
    check(dm_automation_add(
              &e->project,
              (DmAutomation){
                  .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40, .pattern = a}),
          "add underlying CC11 point");
    check(dm_clip_add(&e->project,
                      (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 2, .length = 8}),
          "place underlying pattern on another row");

    ed_automation_line(e, 2, 4, 20, 100, MIDI_CC_EXPRESSION);
    ++e->export_revision;
    e->selected_clip = 0;
    e->selected_pattern = 0;

    int heard = -1;
    check(ed_heard_value(e, 5.0, &heard) && heard == 40,
          "releasing stretch with selected placement hears underlying CC11");

    e->selected_clip = -1;
    heard = -1;
    check(!ed_heard_value(e, 5.0, &heard) && heard == 127,
          "without a selected placement, dim stretch falls back to default");

    e->selected_clip = 0;
    ed_graph_mode(e, 0, 16);
    ++e->export_revision;
    heard = -1;
    check(ed_heard_value(e, 5.0, &heard) && heard == 40,
          "non-held Graph lane release hears underlying CC11");
}

int main(void) {
    Editor *e = test_editor_open("Event controls", (TestDesktop){.audio = true});
    SDL_Renderer *r = e->renderer;
    DmAudio *audio = e->audio;
    ui_frame();
    ui_root(0, 900);
    continuous(e);
    imported_mix_controls(e);
    switches_and_glide(e);
    program_change_points(e);
    section_ends(e);
    section_end_kinds(e);
    heard_dim_stretch(e);
    ui_end();
    ui_render(r);
    fixture(e, 64, true);
    test_event_controls_ui(e, check);
    test_event_controls_audio(audio, check);
    printf("PASS event controls: %d checks\n", checks);
    test_editor_close(e);
}
