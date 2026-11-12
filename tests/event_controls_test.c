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
    check(scheduled_value(&schedule, MIDI_CC_VOLUME, .55) == 64 &&
              scheduled_value(&schedule, MIDI_CC_VOLUME, .65) == 93 &&
              scheduled_value(&schedule, MIDI_CC_PAN, .65) == 80,
          "the Volume bar plays and resets while imported Pan keeps its Point value");
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
    ui_end();
    ui_render(r);
    fixture(e, 64, true);
    test_event_controls_ui(e, check);
    test_event_controls_audio(audio, check);
    printf("PASS event controls: %d checks\n", checks);
    test_editor_close(e);
}
