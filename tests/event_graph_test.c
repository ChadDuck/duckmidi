#include "editor/internal.h"
static int checks;

static void check(bool ok, const char *what) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL event graph: %s\n", what);
        exit(1);
    }
}

static const UiRect lane = {100, 200, 800, 200};
static View view = {.zoom = 1};
static const float scale = 80;

static float beat_x(double beat) {
    return lane.x + (float)beat * scale;
}

/* The middle of the value's row, since lane values truncate. */
static float value_y(int value) {
    return lane.y + lane.h - 8 - (value + .5f) / MIDI_DATA_MAX * (lane.h - 8);
}

static void fixture(Editor *e, int cc) {
    editor_smoke(e);
    e->active = true;
    e->selected_pattern = 0;
    e->pattern_channel = 0;
    e->project.automation_count = 0;
    e->event_selection.size = 0;
    e->automation_all = false;
    e->automation_node = -1;
    e->automation_cc = cc;
    e->automation_raw = false;
    ed_focus_edit(e, FOCUS_EVENTS);
}

static DmAutomation cc_point(double beat, int cc, int value, int curve) {
    return (DmAutomation){beat, MIDI_CONTROL, 0, cc, value, 0, curve, 60};
}

static DmAutomation bend_point(double beat, int value, int curve) {
    return (DmAutomation){beat, MIDI_PITCH_BEND, 0, value & 127, value >> 7, 0, curve, 60};
}

static void add(Editor *e, DmAutomation a) {
    check(dm_automation_add(&e->project, a), "fixture point is valid");
}

/* Value of the last scheduled message of a channel-1 lane at or before `seconds`. */
static int scheduled(const DmSchedule *s, int type, int cc, double seconds) {
    int value = -1;

    for (size_t i = 0; i < s->count; ++i) {
        DmScheduled a = s->items[i];

        if (a.status != type || a.seconds > seconds + 1e-8) {
            continue;
        }

        if (type == MIDI_PITCH_BEND) {
            value = a.a | a.b << 7;
        } else if (a.a == cc) {
            value = a.b;
        }
    }

    return value;
}

static size_t lane_points(Editor *e) {
    size_t n = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        n += ed_automation_matches(e, e->project.automation[i]);
    }

    return n;
}

static bool lane_point(Editor *e, double beat, int value) {
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (ed_automation_matches(e, a) && fabs(a.beat - beat) < 1e-9 &&
            ed_event_value(a) == value) {
            return true;
        }
    }

    return false;
}

static bool only_discrete(Editor *e, int pattern) {
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern == pattern && (a.curve || a.ramp)) {
            return false;
        }
    }

    return true;
}

/* To Graph keeps the value the lane plays at every cell start: a bent modulation line with a
   held node and restoration, an expression ramp, and a pitch section with its range setup. */
static void converter(Editor *e) {
    fixture(e, MIDI_CC_MODULATION);
    DmAutomation bent = cc_point(0, MIDI_CC_MODULATION, 0, 1);
    bent.bend = 30;
    add(e, bent);
    DmAutomation held = cc_point(2, MIDI_CC_MODULATION, 127, 1);
    held.hold = true;
    add(e, held);
    add(e, cc_point(3, MIDI_CC_MODULATION, 40, 1));
    add(e, cc_point(3, MIDI_CC_MODULATION, 10, -1));
    DmAutomation ramp = cc_point(1, MIDI_CC_EXPRESSION, 20, 0);
    ramp.ramp = true;
    add(e, ramp);
    add(e, cc_point(3, MIDI_CC_EXPRESSION, 100, 0));
    add(e, cc_point(.5, MIDI_CC_RPN_MSB, 0, 2));
    add(e, cc_point(.5, MIDI_CC_RPN_LSB, 0, 2));
    add(e, cc_point(.5, MIDI_CC_DATA, 12, 2));
    add(e, bend_point(.5, 4000, 2));
    add(e, bend_point(2.5, 12000, 2));
    add(e, bend_point(2.5, MIDI_BEND_CENTER, -2));
    add(e, cc_point(2.5, MIDI_CC_DATA, 2, -2));
    DmAutomation bass = cc_point(1, MIDI_CC_MODULATION, 90, 0); /* Untouched pattern. */
    bass.pattern = 1;
    bass.ramp = true;
    add(e, bass);
    size_t other = 0;

    DmSchedule before = {0}, after = {0};
    check(dm_schedule(&e->project, &before), "schedule the point lanes");
    int history = e->undo_count;
    ed_graph_mode(e, 0, 16);
    check(ed_graph_resolution(e) == 16 && !ed_pattern(e)->mode && e->undo_count == history + 1,
          "Graph mode is one undo step at 1/16");
    check(!only_discrete(e, 0) && !dm_graph_resolution(ed_pattern(e), MIDI_CC_EXPRESSION) &&
              !dm_graph_resolution(ed_pattern(e), -2),
          "converting modulation leaves the expression ramp and pitch bend in Point");
    check(dm_schedule(&e->project, &after), "schedule the mixed Point and Graph lanes");

    for (double beat = 0; beat <= 4; beat += .125) {
        double time = dm_beat_seconds(&e->project, beat);
        check(scheduled(&before, MIDI_CONTROL, MIDI_CC_EXPRESSION, time) ==
                      scheduled(&after, MIDI_CONTROL, MIDI_CC_EXPRESSION, time) &&
                  scheduled(&before, MIDI_PITCH_BEND, 0, time) ==
                      scheduled(&after, MIDI_PITCH_BEND, 0, time) &&
                  scheduled(&before, MIDI_CONTROL, MIDI_CC_DATA, time) ==
                      scheduled(&after, MIDI_CONTROL, MIDI_CC_DATA, time),
              "the Point lanes keep their original playback and bend range");
    }

    dm_schedule_free(&after);
    e->automation_cc = MIDI_CC_EXPRESSION;
    ed_graph_mode(e, 0, 32);
    e->automation_cc = -2;
    ed_graph_mode(e, 0, 16);
    e->automation_cc = MIDI_CC_MODULATION;
    check(ed_graph_resolution(e) == 16 && only_discrete(e, 0),
          "each Graph lane converts separately and keeps its resolution");

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        other += e->project.automation[i].pattern == 1 && e->project.automation[i].ramp;
    }

    check(other == 1, "other patterns keep their ramps");
    check(dm_schedule(&e->project, &after), "schedule the graph lanes");
    double seconds = 60 / e->project.bpm;

    /* Through each lane's last bar; after its cell a graph lane returns to its default. */
    for (double beat = 0; beat <= 3; beat += .25) {
        check(scheduled(&before, MIDI_CONTROL, 1, beat * seconds) ==
                  scheduled(&after, MIDI_CONTROL, 1, beat * seconds),
              "modulation plays the same value at every cell start");
        check(scheduled(&before, MIDI_CONTROL, 11, beat * seconds) ==
                  scheduled(&after, MIDI_CONTROL, 11, beat * seconds),
              "the ramp plays the same value at every cell start");
        check(abs(scheduled(&before, MIDI_PITCH_BEND, 0, beat * seconds) -
                  scheduled(&after, MIDI_PITCH_BEND, 0, beat * seconds)) <= 1,
              "pitch bend plays the same value at every cell start");
        check(scheduled(&before, MIDI_CONTROL, MIDI_CC_DATA, beat * seconds) ==
                  scheduled(&after, MIDI_CONTROL, MIDI_CC_DATA, beat * seconds),
              "the bend range is unchanged");
    }

    check(scheduled(&after, MIDI_CONTROL, 1, 3.2 * seconds) == 10 &&
              scheduled(&after, MIDI_CONTROL, 11, 3.2 * seconds) == 127 &&
              scheduled(&after, MIDI_CONTROL, 1, 3.3 * seconds) == 0,
          "each Graph lane returns to its default after its own cell");
    dm_schedule_free(&before);
    dm_schedule_free(&after);
    check(lane_point(e, 2, 127) && !lane_point(e, 2.5, 127) && lane_point(e, 3, 10) &&
              !lane_point(e, 3, 40),
          "a held node is one bar, and the section ends at its restoration value");

    ed_graph_mode(e, 0, 0);
    check(!ed_graph_resolution(e) && only_discrete(e, 0) &&
              dm_graph_resolution(ed_pattern(e), MIDI_CC_EXPRESSION) == 32,
          "To Point keeps the bars and the other lane's Graph setting");
    ed_undo(e, false);
    ed_undo(e, false);
    ed_undo(e, false);
    ed_undo(e, false);
    check(!dm_pattern_has_graph(ed_pattern(e)) && !only_discrete(e, 0),
          "undo restores the line sections and independent settings");
}

static void modes(Editor *e) {
    fixture(e, MIDI_CC_MODULATION);
    ed_graph_mode(e, 0, 32);
    check(ed_graph_resolution(e) == 32 && ed_event_snap(e) == .125, "1/32 cells are 1/8 beat");
    ed_graph_mode(e, 1, 32);
    check(ed_pattern(e)->mode == 1 && !ed_graph_resolution(e), "LFO hides the Graph lanes");
    ed_graph_mode(e, 0, 32);

    for (int cc = -5; cc <= 127; ++cc) {
        e->automation_cc = cc;

        if (cc == -3 || ed_point_only_event(cc)) {
            check(dm_graph_lane(cc) < 0 && !ed_graph_resolution(e),
                  "velocity and point-only events never show a graph");
        }
    }

    e->automation_cc = -2;
    check(!ed_graph_resolution(e) && !ed_automation_raw(e), "pitch bend still opens in Point");
    add(e, bend_point(0, 8000, 0));
    ed_graph_mode_request(e, 0, 64);
    check(e->graph_switch && e->graph_switch_event == -2 && !ed_graph_resolution(e),
          "switching a populated lane asks before conversion");
    e->graph_switch = false;
    e->automation_cc = MIDI_CC_EXPRESSION;
    ed_graph_mode_request(e, 0, 16);
    check(!e->graph_switch && ed_graph_resolution(e) == 16,
          "an empty lane enters Graph even when another type has data");
    ed_graph_mode_request(e, 0, 0);
    check(!e->graph_switch && !ed_graph_resolution(e),
          "an empty lane returns to Point without a warning");
    e->automation_cc = -2;
    ed_graph_mode(e, 0, 64);
    ed_graph_mode_request(e, 0, 0);
    check(e->graph_switch && !e->graph_switch_graph && ed_graph_resolution(e) == 64,
          "a populated Graph lane asks before changing to Point");
    e->graph_switch = false;
    check(ed_graph_resolution(e) == 64 && ed_automation_raw(e),
          "graph pitch bends use raw MIDI values");
    DmBytes saved = {0};
    DmProject loaded;
    dm_project_init(&loaded);
    check(dm_pack(&e->project, &saved) &&
              dm_unpack(&loaded, saved.data, saved.size, e->status, sizeof(e->status)) &&
              dm_graph_resolution(&loaded.patterns[0], -2) == 64 &&
              dm_graph_resolution(&loaded.patterns[0], MIDI_CC_MODULATION) == 32 &&
              !dm_graph_resolution(&loaded.patterns[0], MIDI_CC_EXPRESSION),
          "each event type's mode and resolution are saved and reloaded");
    dm_bytes_free(&saved);
    dm_project_free(&loaded);
}

static void painting(Editor *e) {
    fixture(e, MIDI_CC_MODULATION);
    ed_graph_mode(e, 0, 16);
    ed_graph_paint(e, lane, &view, scale, (UiPoint){beat_x(.1), value_y(20)},
                   (UiPoint){beat_x(.9), value_y(80)}, false);
    check(lane_points(e) == 4, "a drag across 4 cells at 1/16 paints 4 bars");

    for (int i = 0; i < 4; ++i) {
        bool found = false;

        for (size_t j = 0; j < e->project.automation_count; ++j) {
            found |= fabs(e->project.automation[j].beat - i * .25) < 1e-9;
        }

        check(found, "each bar starts its cell");
    }

    ed_graph_paint(e, lane, &view, scale, (UiPoint){beat_x(.3), value_y(100)},
                   (UiPoint){beat_x(.3), value_y(100)}, false);
    check(lane_points(e) == 4 && lane_point(e, .25, 96),
          "repainting a cell replaces its bar, at the nearest value row");
    ed_graph_paint(e, lane, &view, scale, (UiPoint){beat_x(1.1), value_y(50)},
                   (UiPoint){beat_x(1.1), value_y(50)}, true);
    check(lane_points(e) == 5 && fabs(e->project.automation[4].beat - 1.1) < 1e-4,
          "Alt paints at the exact beat");

    e->erase_active = false;
    check(ed_graph_erase(e, lane, &view, scale, (UiPoint){beat_x(.3), 0}, (UiPoint){beat_x(.6), 0}),
          "a right drag erases the bars it crosses");
    check(lane_points(e) == 3 && !lane_point(e, .25, 96), "the crossed bars are gone");
    e->erase_active = false;
    check(
        !ed_graph_erase(e, lane, &view, scale, (UiPoint){beat_x(3), 0}, (UiPoint){beat_x(3.2), 0}),
        "erasing empty cells changes nothing");
}

/* A lane's last bar lasts one cell, then the lane returns to its default; earlier bars hold until
   the next bar. */
static void returns(Editor *e) {
    fixture(e, -2);
    ed_graph_mode(e, 0, 16);
    add(e, bend_point(.25, 13312, 0));
    DmSchedule s = {0};
    check(dm_schedule(&e->project, &s), "schedule a single bend bar");
    double seconds = 60 / e->project.bpm;
    check(scheduled(&s, MIDI_PITCH_BEND, 0, .3 * seconds) == 13312 &&
              scheduled(&s, MIDI_PITCH_BEND, 0, .5 * seconds) == MIDI_BEND_CENTER &&
              scheduled(&s, MIDI_PITCH_BEND, 0, 3 * seconds) == MIDI_BEND_CENTER,
          "the last bar returns to no pitch bend when its cell ends");
    dm_schedule_free(&s);
    add(e, bend_point(2, 11000, 0));
    check(dm_schedule(&e->project, &s), "schedule two bend bars");
    check(scheduled(&s, MIDI_PITCH_BEND, 0, 1.5 * seconds) == 13312 &&
              scheduled(&s, MIDI_PITCH_BEND, 0, 2.1 * seconds) == 11000 &&
              scheduled(&s, MIDI_PITCH_BEND, 0, 2.3 * seconds) == MIDI_BEND_CENTER,
          "a bar holds until the next bar, and only the last bar returns");
    dm_schedule_free(&s);
    ed_graph_mode(e, 0, 0);
    check(dm_schedule(&e->project, &s), "schedule the same points in Point mode");
    check(scheduled(&s, MIDI_PITCH_BEND, 0, 3 * seconds) == 11000,
          "Point mode playback is unchanged");
    dm_schedule_free(&s);
}

static void snapping(Editor *e) {
    fixture(e, MIDI_CC_MODULATION);
    ed_graph_mode(e, 0, 16);
    check(ed_value_step(e, lane.h) == 16, "a 200 px lane has rows 16 values apart");
    check(ed_value_step(e, 430) == 8, "a taller lane has finer rows");
    e->event_lane_height = lane.h;
    /* Values as recorded: off the rows until Snap bars is pressed. */
    add(e, cc_point(.30, MIDI_CC_MODULATION, 10, 0));
    add(e, cc_point(.40, MIDI_CC_MODULATION, 20, 0));
    add(e, cc_point(.55, MIDI_CC_MODULATION, 125, 0));
    add(e, cc_point(.60, MIDI_CC_EXPRESSION, 40, 0));
    check(lane_point(e, .30, 10) && lane_point(e, .40, 20), "recorded values are not snapped");
    int history = e->undo_count;
    ed_graph_snap(e);
    check(lane_points(e) == 3 && lane_point(e, .30, 16) && lane_point(e, .40, 16) &&
              lane_point(e, .55, 127),
          "Snap bars moves values to the nearest row and keeps their beats");
    check(e->undo_count == history + 1, "Snap bars is one undo step");
    ed_graph_snap(e);
    check(e->undo_count == history + 1, "snapping snapped bars adds no undo step");
    e->automation_cc = MIDI_CC_EXPRESSION;
    check(lane_point(e, .6, 40), "other lanes are not snapped");
    e->automation_cc = MIDI_CC_MODULATION;
    check(ed_lane_value(e, lane, value_y(41), cc_point(0, MIDI_CC_MODULATION, 0, 0), false) == 48 &&
              ed_lane_value(e, lane, value_y(41), cc_point(0, MIDI_CC_MODULATION, 0, 0), true) ==
                  41,
          "drawn values snap to rows unless Alt is held");
    e->automation_cc = -1;
    check(!ed_value_step(e, lane.h), "program changes have no value rows");
}

static void selection_and_clipboard(Editor *e) {
    fixture(e, MIDI_CC_MODULATION);
    ed_graph_mode(e, 0, 16);
    ed_graph_paint(e, lane, &view, scale, (UiPoint){beat_x(0), value_y(60)},
                   (UiPoint){beat_x(1.9), value_y(60)}, false);
    check(lane_points(e) == 8, "eight bars painted");

    /* A marquee touching the tops of bars 2 and 3 selects them. */
    ui_frame();
    ui_root(0, 900);
    e->event_marquee = true;
    e->event_marquee_start = (UiPoint){beat_x(.6), value_y(70)};
    UiMouse mouse = {.pos = {beat_x(.8), value_y(50)}, .left = true};
    ed_event_box_select(e, lane, &view, scale, mouse);
    ui_end();
    ui_render(e->renderer);
    check(e->event_selection.size / sizeof(DmAutomation) == 2, "the marquee selects the bars");
    ed_delete_events(e);
    check(lane_points(e) == 6 && !lane_point(e, .5, 60) && !lane_point(e, .75, 60),
          "marquee and delete remove the selected bars");

    /* Pasting a Point lane's line into a Graph pattern pastes bars. */
    e->selected_pattern = 1;
    e->project.automation_count = 0;
    add(e, cc_point(0, MIDI_CC_MODULATION, 0, 1));
    e->project.automation[0].pattern = 1;
    add(e, cc_point(1, MIDI_CC_MODULATION, 127, 1));
    e->project.automation[1].pattern = 1;
    ed_sort_automation(e);
    ed_copy_event_lane(e, false);
    e->selected_pattern = 0;
    ed_graph_mode(e, 0, 16);
    size_t before = lane_points(e);
    ed_paste_event_lane(e);
    check(only_discrete(e, 0) && lane_points(e) == before + 5 && lane_point(e, .5, 64),
          "a pasted line becomes bars in a Graph pattern");
}

static void mixed_clipboard(Editor *e) {
    fixture(e, MIDI_CC_MODULATION);
    add(e, cc_point(0, MIDI_CC_MODULATION, 0, 1));
    add(e, bend_point(0, 4000, 2));
    add(e, cc_point(1, MIDI_CC_MODULATION, 127, 1));
    add(e, bend_point(1, 12000, 2));
    ed_copy_event_lane(e, true);
    e->selected_pattern = 1;
    ed_graph_mode(e, 0, 32);
    ed_paste_event_lane(e);
    check(lane_points(e) == 9 && lane_point(e, .5, 64),
          "Paste all samples the modulation line at the destination's Graph resolution");
    e->automation_cc = -2;
    check(!ed_graph_resolution(e) && lane_points(e) == 2 && !only_discrete(e, 1),
          "Paste all keeps the pitch bend line in the destination's Point lane");
}

static void input_frame(Editor *e, UiMouse mouse) {
    ui_frame();
    ui_root(0, 900);
    int hit = ed_automation_plot(e, lane, &view, scale, mouse);
    ed_automation_input(e, lane, &view, scale, mouse, true, hit);
    ui_end();
    ui_render(e->renderer);
}

static void additive_marquee(Editor *e) {
    fixture(e, MIDI_CC_MODULATION);
    ed_graph_mode(e, 0, 16);
    add(e, cc_point(.25, MIDI_CC_MODULATION, 60, 0));
    add(e, cc_point(1, MIDI_CC_MODULATION, 60, 0));
    add(e, cc_point(1.5, MIDI_CC_MODULATION, 60, 0));
    e->automation_node = 0;
    UiMouse mouse = {.pos = {beat_x(.9), value_y(70)}, .left = true, .clicked = true, .ctrl = true};
    input_frame(e, mouse);
    mouse = (UiMouse){.pos = {beat_x(1.8), value_y(50)}, .left = true, .shift = true};
    input_frame(e, mouse);
    check(ed_event_selected(e, 0) && ed_event_selected(e, 1) && ed_event_selected(e, 2),
          "Shift adds bars to a single preselected event");
    mouse.pos.x = beat_x(1.2);
    input_frame(e, mouse);
    check(ed_event_selected(e, 0) && ed_event_selected(e, 1) && !ed_event_selected(e, 2),
          "shrinking the Graph marquee removes swept-in bars outside the box");
    mouse.shift = false;
    input_frame(e, mouse);
    check(!ed_event_selected(e, 0) && ed_event_selected(e, 1),
          "releasing Shift returns to replacement selection");
    mouse = (UiMouse){.pos = mouse.pos, .released = true, .shift = true};
    input_frame(e, mouse);
    check(ed_event_selected(e, 0) && ed_event_selected(e, 1) && !ed_event_selected(e, 2) &&
              !e->event_marquee && e->edit_focus == FOCUS_EVENTS,
          "the final Shift state is applied on mouse release");
}

/* The lane's own gestures: a left drag paints in one undo step, a right drag erases. */
static void gestures(Editor *e) {
    fixture(e, MIDI_CC_MODULATION);
    ed_graph_mode(e, 0, 16);
    int history = e->undo_count;
    UiMouse mouse = {.pos = {beat_x(.1), value_y(30)}, .left = true, .clicked = true};
    input_frame(e, mouse);
    mouse.clicked = false;
    mouse.pos = (UiPoint){beat_x(1.6), value_y(90)};
    input_frame(e, mouse);
    mouse = (UiMouse){.pos = mouse.pos, .released = true};
    input_frame(e, mouse);
    check(lane_points(e) == 7 && e->undo_count == history + 1 && !e->drag,
          "a left drag paints every crossed cell in one undo step");

    mouse = (UiMouse){.pos = {beat_x(.3), value_y(60)}, .right = true, .right_clicked = true};
    input_frame(e, mouse);
    mouse.right_clicked = false;
    mouse.pos.x = beat_x(.8);
    input_frame(e, mouse);
    mouse = (UiMouse){.pos = mouse.pos};
    input_frame(e, mouse);
    e->erase_active = false; /* The window frame ends the gesture when the button is up. */
    check(lane_points(e) == 4 && e->undo_count == history + 2 && !e->drag,
          "a right drag erases the crossed bars in one undo step");
}

/* Bars hit-test by their rectangle; dense and bipolar lanes draw without errors. */
static void drawing(Editor *e) {
    fixture(e, MIDI_CC_MODULATION);
    ed_graph_mode(e, 0, 16);
    ed_graph_paint(e, lane, &view, scale, (UiPoint){beat_x(0), value_y(20)},
                   (UiPoint){beat_x(3.9), value_y(120)}, false);
    ui_frame();
    ui_root(0, 900);
    ui_clip(lane);
    UiMouse mouse = {.pos = {beat_x(1.1), value_y(5)}};
    int hit = ed_automation_plot(e, lane, &view, scale, mouse);
    check(hit >= 0 && fabs(e->project.automation[hit].beat - 1) < 1e-9,
          "a bar is hit anywhere inside its rectangle");
    mouse.pos.y = value_y(127);
    check(ed_automation_plot(e, lane, &view, scale, mouse) < 0, "above the bar misses it");

    e->automation_cc = -2;
    ed_graph_paint(e, lane, &view, scale, (UiPoint){beat_x(4), lane.y + 20},
                   (UiPoint){beat_x(8), lane.y + lane.h - 20}, false);

    for (int i = 0; i < 20000; ++i) {
        add(e, bend_point(8 + i * .001, (i * 37) % 16384, 0));
    }

    View wide = {.zoom = .15f};
    ed_automation_plot(e, lane, &wide, 80 * wide.zoom, mouse);
    ed_automation_plot(e, lane, &view, scale, mouse);
    ui_unclip();
    ui_end();
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
}

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Event graph", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "software");
    check(w && r && ui_init(w, r, NULL), "renderer");
    Editor *e = calloc(1, sizeof(*e));
    check(e != NULL, "editor allocation");
    editor_init(e, w, r, NULL, "", "", true);
    converter(e);
    modes(e);
    painting(e);
    returns(e);
    snapping(e);
    selection_and_clipboard(e);
    mixed_clipboard(e);
    additive_marquee(e);
    gestures(e);
    drawing(e);
    printf("PASS event graph: %d checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
