#include "editor/internal.h"
#define TEST_NAME "event lanes"
#include "test_common.h"
extern void test_event_lanes_ui(Editor *, void (*)(bool, const char *));
extern void test_event_lane_modes_ui(Editor *, void (*)(bool, const char *));

static void numeric_ranges(Editor *e) {
    const int events[] = {1, 11, 7, 10, 5, 91, 93, 74, 71, 92, 76, 77, -2};

    for (size_t event = 0; event < sizeof(events) / sizeof(*events); ++event) {
        e->automation_cc = events[event];
        e->automation_raw = true;
        int maximum = events[event] == -2 ? 16383 : 127;
        DmAutomation a = {.type = events[event] == -2 ? 0xe0 : 0xb0, .a = events[event]};

        for (int height = 35; height <= 435; height += 100) {
            UiRect lane = {100, 200, 500, (float)height};

            for (int value = 0; value <= maximum; value += maximum) {
                ed_set_event_value(&a, value);
                UiPoint point = ed_automation_point(e, a, lane, &e->roll_view, 80);
                /* The maximum sits on the lane's top edge, flush with the ruler above. */
                check(fabsf(point.y - (value ? lane.y : lane.y + lane.h - 8)) < .001,
                      "full numeric range fits the lane at every section height");
                ed_automation_pan_y(e, lane, 120);
                check(fabsf(ed_automation_point(e, a, lane, &e->roll_view, 80).y - point.y) < .001,
                      "numeric lanes reject vertical panning");
                check(abs(ed_lane_value(e, lane, point.y, a, false) - value) <= 1,
                      "drawing coordinates retain endpoint values after resize and pan");
            }
        }
    }

    e->automation_cc = DM_EVENT_BEND;
    e->automation_raw = false;
    e->automation_center = 60;
    UiRect lane = {100, 200, 500, 200};
    ed_automation_pan_y(e, lane, 40);
    check(e->automation_center == 62, "note-pitch mode retains semitone-based scrolling");

    for (int cc = 64; cc <= 66; cc += 2) {
        e->automation_cc = cc;
        DmAutomation a = {.type = 0xb0, .a = cc, .b = 127};
        check(ed_automation_point(e, a, lane, &e->roll_view, 80).y == lane.y + lane.h / 2,
              "pedals retain their single-line presentation");
    }
}

static void parameter_ownership(Editor *e) {
    e->project.automation_count = 0;
    e->automation_cc = DM_EVENT_BEND;
    e->drag_key = 60;
    e->bend_range = 12;
    DmAutomation imported = {.beat = 0, .type = 0xb0, .a = 101, .b = 0, .pattern = 0};
    dm_automation_add(&e->project, imported);
    ed_automation_line(e, 1, 3, 8192, 10000, -2);
    size_t original = e->project.automation_count;
    check(original > 3, "drawing pitch bend creates its RPN range and restore messages");
    size_t rows = ed_midi_rows(e);
    check(rows == e->project.event_count + original,
          "MIDI inspector preserves access to parameter messages");

    for (int removed = 0; removed < 2; ++removed) {
        e->automation_node = -1;

        for (size_t i = 0; i < e->project.automation_count; ++i) {
            if (e->project.automation[i].type == 0xe0 && e->project.automation[i].curve > 0) {
                e->automation_node = (int)i;
                break;
            }
        }

        check(e->automation_node >= 0, "find a generated pitch point");
        ed_remove_selected_node(e);
    }

    check(e->project.automation_count == 1 && ed_same_event(e->project.automation[0], imported),
          "deleting final pitch point removes its dependencies and preserves imported RPN");
    ed_automation_line(e, 1, 3, 8192, 10000, -2);
    ed_automation_clear(e);
    check(e->project.automation_count == 1 && ed_same_event(e->project.automation[0], imported),
          "clearing pitch bend removes its dependencies and preserves imported RPN");
}

/* Shift + drag joins the drag's start to the nearest point and continues that line to the
   release, all in one curve and one undo step. */
static void shift_drag_line(Editor *e) {
    ed_new_project(e);
    e->automation_cc = 11;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_EVENTS;
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};
    check(dm_automation_add(&e->project,
                            (DmAutomation){.type = MIDI_CONTROL, .a = 11, .b = 30, .pattern = 0}),
          "fixture point");
    int history = e->undo_count;
    ed_automation_lane_click(
        e, lane, &v, 80, (UiMouse){.pos = {260, 180}, .clicked = true, .left = true, .shift = true},
        -1);
    ed_automation_press(e, lane, &v, 80, (UiMouse){.pos = {420, 140}, .left = true, .shift = true});
    check(e->drag == DRAG_EVENT_LINE, "Shift + drag past the buffer draws a line");
    ed_automation_drag(e, lane, &v, 80, (UiMouse){.pos = {420, 140}, .released = true});
    int curve = 0, nodes = 0, restore = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        curve = a.beat == 0 ? a.curve : curve;
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        nodes += curve > 0 && a.curve == curve;
        restore += curve > 0 && a.curve == -curve;
    }

    check(curve > 0 && nodes == 3 && restore == 1,
          "the neighbour, the drag start and the release form one connected line");
    check(e->undo_count == history + 1, "a Shift + drag line is one undo step");
    e->event_connect = false;
}

/* Drawing a pitch-bend line preview leaves the project untouched, including other lines' bend
   ranges (the preview once borrowed a real line's id and widened its range). */
static void pitch_preview_is_read_only(Editor *e) {
    ed_new_project(e);
    e->automation_cc = DM_EVENT_BEND;
    e->automation_raw = false;
    e->automation_center = 60;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_EVENTS;
    ed_automation_line(e, 0, 1, MIDI_BEND_CENTER, MIDI_BEND_CENTER + 2000, -2);
    size_t count = e->project.automation_count;
    DmAutomation *before = malloc(count * sizeof(*before));
    check(before && count, "fixture pitch line");
    memcpy(before, e->project.automation, count * sizeof(*before));
    UiRect lane = {100, 100, 600, 400};
    View v = {1, 0, 0, 0};
    e->drag = DRAG_EVENT_LINE;
    e->event_connect = true;
    e->marquee_start = (UiPoint){260, 120};

    for (int y = 110; y < 500; y += 40) {
        ed_automation_drag(e, lane, &v, 80, (UiMouse){.pos = {500, (float)y}, .left = true});
    }

    check(e->project.automation_count == count &&
              !memcmp(before, e->project.automation, count * sizeof(*before)),
          "the line preview does not change any points");
    free(before);
    e->drag = DRAG_NONE;
    e->event_connect = false;
}

/* Program changes are discrete: dragging places one point at the press and never draws a line. */
static void program_change_drag(Editor *e) {
    ed_new_project(e);
    e->automation_cc = DM_EVENT_PROGRAM;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_EVENTS;
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};
    ed_automation_lane_click(
        e, lane, &v, 80, (UiMouse){.pos = {260, 180}, .clicked = true, .left = true, .shift = true},
        -1);
    ed_automation_press(e, lane, &v, 80, (UiMouse){.pos = {420, 120}, .left = true});
    check(e->drag == DRAG_EVENT_PRESS, "dragging a program change draws no line");
    ed_automation_press(e, lane, &v, 80, (UiMouse){.pos = {420, 120}, .released = true});
    check(e->drag == DRAG_NONE && e->project.automation_count == 1 &&
              e->project.automation[0].type == MIDI_PROGRAM && e->project.automation[0].beat == 2,
          "a program change drag places one point where it was pressed");
}

/* Channel pressure offers only the Point mode, but it still draws lines, and a line plays changing
   pressure values in the first data byte. */
static void channel_pressure_line(Editor *e) {
    ed_new_project(e);
    e->automation_cc = DM_EVENT_CHANNEL_PRESSURE;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_EVENTS;
    check(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0}), "fixture placement");
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};
    ed_automation_lane_click(e, lane, &v, 80,
                             (UiMouse){.pos = {100, 292}, .clicked = true, .left = true}, -1);
    ed_automation_press(e, lane, &v, 80, (UiMouse){.pos = {260, 108}, .left = true});
    check(e->drag == DRAG_EVENT_LINE, "dragging channel pressure draws a line");
    ed_automation_drag(e, lane, &v, 80, (UiMouse){.pos = {260, 108}, .released = true});
    int nodes = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        nodes += e->project.automation[i].type == MIDI_CHANNEL_PRESSURE &&
                 e->project.automation[i].curve > 0;
    }

    check(nodes == 2, "a channel pressure line has two connected points");
    DmSchedule s = {0};
    check(dm_schedule(&e->project, &s), "schedule channel pressure");
    int low = 128, high = -1;

    for (size_t i = 0; i < s.count; ++i) {
        if ((s.items[i].status & MIDI_STATUS_MASK) == MIDI_CHANNEL_PRESSURE &&
            s.items[i].seconds > .05 && s.items[i].seconds < .95) {
            low = s.items[i].a < low ? s.items[i].a : low;
            high = s.items[i].a > high ? s.items[i].a : high;
        }
    }

    check(low < high && high - low > 50, "channel pressure lines play changing values");
    dm_schedule_free(&s);
}

int main(void) {
    Editor *e = test_editor_open("Event lanes", (TestDesktop){0});
    SDL_Renderer *r = e->renderer;
    editor_smoke(e);
    ui_frame();
    ui_root(0, 900);
    numeric_ranges(e);
    parameter_ownership(e);
    ui_end();
    ui_render(r);
    editor_smoke(e);
    test_event_lanes_ui(e, check);
    ui_frame();
    ui_root(0, 900);
    shift_drag_line(e);
    pitch_preview_is_read_only(e);
    program_change_drag(e);
    channel_pressure_line(e);
    ui_end();
    ui_render(r);
    ed_new_project(e);
    e->selected_pattern = 0;
    e->selected_channel = e->pattern_channel = 0;
    e->selected_clip = e->playlist_context_clip = -1;
    test_event_lane_modes_ui(e, check);
    printf("PASS event lanes: %d checks\n", checks);
    test_editor_close(e);
}
