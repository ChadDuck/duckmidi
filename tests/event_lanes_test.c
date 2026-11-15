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

static void hold_until_pattern_end(Editor *e) {
    /* 1. Imported discrete points hold. */
    static const uint8_t smf[] = {
        'M',  'T',  'h',  'd',  0,    0,    0,    6,    0,    0,    0,    1,    0,
        0x60, 'M',  'T',  'r',  'k',  0,    0,    0,    16,   0x00, 0x90, 0x3c, 0x64,
        0x00, 0xb0, 0x0b, 0x40, 0x60, 0x80, 0x3c, 0x00, 0x00, 0xff, 0x2f, 0x00,
    };
    char error[128] = {0};
    ed_new_project(e);
    check(dm_import(&e->project, smf, sizeof(smf), 0, 0, error, sizeof(error)), error);
    e->selected_pattern = 0;
    ed_pitch_cache_begin(e);
    bool found_imported = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].type == MIDI_CONTROL &&
            e->project.automation[i].a == MIDI_CC_EXPRESSION) {
            found_imported = true;
            check(ed_point_holds(e, i), "imported discrete point holds");
        }
    }

    ed_pitch_cache_end(e);
    check(found_imported, "imported CC11 point present");

    /* 2. Shift-connecting a lone point gives a Line whose end does not hold. */
    ed_new_project(e);
    e->automation_cc = MIDI_CC_EXPRESSION;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_EVENTS;
    check(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0}), "fixture placement");
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};
    ed_automation_lane_click(e, lane, &v, 80,
                             (UiMouse){.pos = {100, 200}, .clicked = true, .left = true}, -1);
    ed_automation_press(e, lane, &v, 80, (UiMouse){.pos = {100, 200}, .released = true});
    ed_automation_lane_click(
        e, lane, &v, 80, (UiMouse){.pos = {260, 140}, .clicked = true, .left = true, .shift = true},
        -1);
    ed_automation_press(e, lane, &v, 80,
                        (UiMouse){.pos = {260, 140}, .released = true, .shift = true});
    ed_pitch_cache_begin(e);
    int line_nodes = 0;
    int restorations = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].curve > 0) {
            line_nodes++;
            check(!ed_point_holds(e, i), "Shift-connected Line nodes do not hold");
        } else if (e->project.automation[i].curve < 0) {
            restorations++;
            check(!ed_point_holds(e, i), "Shift-connected restoration does not hold");
        }
    }

    ed_pitch_cache_end(e);
    check(line_nodes == 2 && restorations == 1,
          "Shift-connect creates a 2-node Line with restoration");

    /* 3. Copy/paste and dm_clip_make_unique keep a held Line held. */
    ed_new_project(e);
    e->automation_cc = MIDI_CC_EXPRESSION;
    e->selected_pattern = 0;
    e->project.patterns[0].kind = 1;
    e->project.patterns[0].event = MIDI_CC_EXPRESSION;
    e->project.patterns[0].length = 8;
    e->project.lane_count[0] = 1;
    check(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0, .lane = 1}),
          "held Line placement");
    check(dm_automation_add(&e->project, (DmAutomation){.beat = 0,
                                                        .type = MIDI_CONTROL,
                                                        .a = MIDI_CC_EXPRESSION,
                                                        .b = 30,
                                                        .curve = 1,
                                                        .pattern = 0}),
          "held Line start");
    check(dm_automation_add(&e->project, (DmAutomation){.beat = 2,
                                                        .type = MIDI_CONTROL,
                                                        .a = MIDI_CC_EXPRESSION,
                                                        .b = 90,
                                                        .curve = 1,
                                                        .pattern = 0}),
          "held Line end");
    e->event_selection.size = 0;
    check(dm_bytes(&e->event_selection, e->project.automation,
                   e->project.automation_count * sizeof(DmAutomation)),
          "select held Line");
    DmProject clip;
    dm_project_init(&clip);
    check(ed_copy_events(e, &clip), "copy held Line");
    ed_paste_events(e, &clip, 4);
    dm_project_free(&clip);
    ed_pitch_cache_begin(e);
    bool pasted_holds = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].beat == 6 && e->project.automation[i].curve > 0) {
            pasted_holds = ed_point_holds(e, i);
        }
    }

    ed_pitch_cache_end(e);
    check(pasted_holds, "pasted held Line end still holds");

    int unique = dm_clip_make_unique(&e->project, 0);
    check(unique >= 0, "make unique on held Line placement");
    e->selected_pattern = e->project.clips[0].pattern;
    ed_pitch_cache_begin(e);
    bool unique_holds = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].pattern == e->selected_pattern &&
            e->project.automation[i].beat == 2 && e->project.automation[i].curve > 0) {
            unique_holds = ed_point_holds(e, i);
        }
    }

    ed_pitch_cache_end(e);
    check(unique_holds, "unique copy of held Line still holds");

    /* 4. ed_restoration_value: live earlier value when one exists, false when nothing precedes. */
    ed_new_project(e);
    e->selected_pattern = 0;
    e->automation_cc = MIDI_CC_EXPRESSION;
    check(dm_automation_add(
              &e->project,
              (DmAutomation){
                  .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 42, .pattern = 0}),
          "prior point for restoration");
    ed_automation_line(e, 1, 3, 20, 100, MIDI_CC_EXPRESSION);
    size_t restore_idx = SIZE_MAX;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].curve < 0) {
            restore_idx = i;
            break;
        }
    }

    int restore_val = -1;
    check(restore_idx < e->project.automation_count &&
              ed_restoration_value(e, restore_idx, &restore_val) && restore_val == 42,
          "ed_restoration_value returns true with earlier value");
    e->project.automation[0].b = 77;
    check(ed_restoration_value(e, restore_idx, &restore_val) && restore_val == 77,
          "ed_restoration_value reads earlier value live");

    ed_new_project(e);
    e->selected_pattern = 0;
    e->automation_cc = MIDI_CC_EXPRESSION;
    ed_automation_line(e, 1, 3, 20, 100, MIDI_CC_EXPRESSION);
    restore_idx = SIZE_MAX;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].curve < 0) {
            restore_idx = i;
            break;
        }
    }

    restore_val = 99;
    check(restore_idx < e->project.automation_count &&
              !ed_restoration_value(e, restore_idx, &restore_val),
          "ed_restoration_value returns false when nothing precedes the section");

    /* A second section after one that released also releases, as playback does. */
    ed_automation_line(e, 5, 7, 30, 90, MIDI_CC_EXPRESSION);
    check(ed_value_before(e, 4, MIDI_CC_EXPRESSION) == 127,
          "after a releasing restoration the pattern holds no value");
    restore_idx = SIZE_MAX;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].curve < 0 && e->project.automation[i].beat > 4) {
            restore_idx = i;
        }
    }

    check(restore_idx < e->project.automation_count &&
              !ed_restoration_value(e, restore_idx, &restore_val),
          "a section after a released section releases");
}

/* Make unique and pasting a pattern into another project keep which Graph lanes hold. */
static void graph_hold_copies(Editor *e) {
    ed_new_project(e);
    DmPattern *source = &e->project.patterns[0];
    source->kind = 1;
    source->event = MIDI_CC_EXPRESSION;
    source->length = 4;
    source->graph_lanes[MIDI_CC_EXPRESSION] = DM_GRAPH_DEFAULT;
    source->graph_hold[MIDI_CC_EXPRESSION] = true;
    source->graph_lanes[MIDI_CC_MODULATION] = DM_GRAPH_DEFAULT;
    e->project.lane_count[0] = 1;
    check(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0, .lane = 1}),
          "held Graph lane placement");
    int unique = dm_clip_make_unique(&e->project, 0);
    const DmPattern *copy = unique > 0 ? &e->project.patterns[unique] : NULL;
    check(copy && copy->graph_hold[MIDI_CC_EXPRESSION] && !copy->graph_hold[MIDI_CC_MODULATION],
          "Make unique keeps which Graph lanes hold");

    e->selected_pattern = 0;
    ed_focus_edit(e, FOCUS_PATTERNS);
    e->pattern_highlight = true;
    ed_clipboard(e, CLIPBOARD_COPY);
    ed_new_project(e);
    ed_focus_edit(e, FOCUS_PATTERNS);
    size_t patterns = e->project.pattern_count;
    ed_paste(e);
    copy = e->project.pattern_count == patterns + 1 ? &e->project.patterns[patterns] : NULL;
    check(copy && copy->graph_lanes[MIDI_CC_EXPRESSION] == DM_GRAPH_DEFAULT &&
              copy->graph_hold[MIDI_CC_EXPRESSION] && !copy->graph_hold[MIDI_CC_MODULATION],
          "a pattern pasted into another project keeps which Graph lanes hold");
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
    hold_until_pattern_end(e);
    graph_hold_copies(e);
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
