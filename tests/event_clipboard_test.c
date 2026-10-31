#include "editor_internal.h"
static int checks;

static void check(bool ok, const char *what) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL event_clipboard: %s\n", what);
        exit(1);
    }
}

extern void test_event_clipboard_ui(Editor *, void (*)(bool, const char *));

static int pitch_node(Editor *e, double beat) {
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern == e->selected_pattern && a.type == 0xe0 && a.curve >= 0 &&
            fabs(a.beat - beat) < 1e-7) {
            return (int)i;
        }
    }

    return -1;
}

static void fixture(Editor *e) {
    editor_smoke(e);
    e->selected_pattern = 0;
    e->project.patterns[0].count = 0;
    dm_note_add(&e->project.patterns[0], (DmNote){0, 8, 60, 100, 0});
    e->project.automation_count = 0;
    e->event_selection.size = 0;
    e->automation_all = false;
    e->automation_node = -1;
    e->automation_cc = -2;
    e->automation_center = 60;
    e->automation_raw = false;
    e->piano = false;
    e->pattern_channel = 0;
    ed_focus_edit(e, FOCUS_EVENTS);
}

static void musical_sync(void) {
    DmPattern p = {.pulse = .5, .amount = 1};
    const double bars[] = {4, 2, 3, 3.5};

    for (int i = 0; i < 4; ++i) {
        p.speed = dm_lfo_speed_snap(1.02 / bars[i], bars[i]);
        check(fabs(p.speed - 1 / bars[i]) < 1e-9, "whole-bar snap follows time signature");
        check(fabs(dm_lfo(&p, 0) - 1) < 1e-9 && fabs(dm_lfo(&p, bars[i] * .5) + 1) < 1e-9 &&
                  fabs(dm_lfo(&p, bars[i]) - 1) < 1e-9,
              "peaks align to bar starts and trough to half-bar");
        check(fabs(dm_lfo_speed_snap(2.03 / bars[i], bars[i]) - 2 / bars[i]) < 1e-9,
              "half-bar snap follows time signature");
    }

    check(dm_lfo_speed_snap(.29, 4) == .29, "free speed remains available away from sync");
}

static int scheduled_value(const DmSchedule *s, int type, double seconds) {
    int value = -1;

    for (size_t i = 0; i < s->count; ++i) {
        DmScheduled a = s->items[i];

        if (a.status == type && a.seconds <= seconds + 1e-8) {
            value = type == 0xe0 ? a.a + (a.b << 7) : a.a == 11 ? a.b : value;
        }
    }

    return value;
}

static void scheduled_sync(void) {
    DmProject p;
    dm_project_init(&p);
    p.numerator = 6;
    p.denominator = 8;
    p.channels[0].program = 0;
    int id = dm_pattern_add(&p, "6/8 LFO");
    p.patterns[id].kind = p.patterns[id].mode = 1;
    p.patterns[id].event = 11;
    p.patterns[id].speed = 1.0 / 3;
    dm_clip_add(&p, (DmClip){0, id, 0, 0, 6, 1});

    for (int i = 0; i < 2; ++i) {
        p.bpm = i ? 180 : 60;
        DmSchedule schedule = {0};
        check(dm_schedule(&p, &schedule), "musically synced LFO schedules");
        check(scheduled_value(&schedule, 0xb0, 1.5 * 60 / p.bpm) == 32 &&
                  scheduled_value(&schedule, 0xb0, 3 * 60 / p.bpm) == 96,
              "6/8 trough and next peak follow tempo in actual MIDI output");
        dm_schedule_free(&schedule);
    }

    p.patterns[id].mode = 0;
    dm_automation_add(&p, (DmAutomation){0, 0xe0, 0, 0, 64, id, 0, 0, 0, false});
    dm_automation_add(&p, (DmAutomation){2, 0xe0, 0, 0, 96, id, 0, 0, 0, false});
    DmSchedule schedule = {0};
    check(dm_schedule(&p, &schedule), "held-value point schedule");
    check(scheduled_value(&schedule, 0xe0, 60 / p.bpm) == 8192 &&
              scheduled_value(&schedule, 0xe0, 2.5 * 60 / p.bpm) == 12288,
          "visual step connections retain held values without adding audible ramps");
    dm_schedule_free(&schedule);
    dm_project_free(&p);
}

static void pitch_ranges(Editor *e) {
    fixture(e);
    e->drag_key = 60;
    e->bend_range = 12;
    ed_automation_line(e, 0, 2, 8192, 12288, -2);
    int last = pitch_node(e, 2);
    check(last >= 0, "original limited-range section");
    DmAutomation moved = ed_expand_pitch_range(e, e->project.automation[last], 110);
    check(ed_automation_range(e, moved) == 127 && fabs(ed_automation_pitch(e, moved) - 66) < .02,
          "range expansion preserves existing endpoint pitch");
    UiRect lane = {100, 100, 500, 300};
    ed_set_event_value(&moved, ed_lane_value(e, lane, 250 - 50 * 20, moved, false));
    check(fabs(ed_automation_pitch(e, moved) - 110) < .02, "expanded bend reaches requested pitch");
    int first = pitch_node(e, 0);
    check(fabs(ed_automation_pitch(e, e->project.automation[first]) - 60) < .02,
          "other section points keep their physical pitch");
    fixture(e);
    e->drag = 4;
    e->marquee_start = (UiPoint){180, 250 - 36 * 20};
    UiMouse mouse = {.pos = e->marquee_start, .released = true};
    ed_automation_drag(e, lane, &e->roll_view, 80, mouse);
    int point = pitch_node(e, 1);
    check(point >= 0 && fabs(ed_automation_pitch(e, e->project.automation[point]) - 96) < .02,
          "new standalone point is not limited by default two-semitone range");
    fixture(e);
    dm_automation_add(&e->project, (DmAutomation){0, 0xe0, 0, 0, 64, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){2, 0xe0, 0, 0, 96, 0, 0, 0, 0, false});
    moved = ed_expand_pitch_range(e, e->project.automation[0], 100);
    last = pitch_node(e, 2);
    check(fabs(ed_automation_pitch(e, e->project.automation[last]) - 61) < .02,
          "expanding discrete pitch preserves later held-value points");
    check(ed_lane_step(lane) == 20 && ed_lane_step((UiRect){0, 0, 500, 1800}) == 20,
          "pitch row spacing stays constant in both editors");
}

static void event_clipboard(Editor *e) {
    fixture(e);
    e->automation_cc = 11;
    dm_automation_add(&e->project, (DmAutomation){.25, 0xb0, 0, 11, 20, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){.75, 0xb0, 0, 11, 80, 0, 0, 0, 0, false});
    ed_select_all(e);
    ed_clipboard(e, 0);
    check(e->clipboard.size > 0, "event copy creates an internal clipboard");
    e->cursor_beat[1] = 4;
    int history = e->undo_count;
    ed_clipboard(e, 2);
    check(e->project.automation_count == 4 && e->undo_count == history + 1,
          "paste adds selected points as one undo step");
    check(e->project.automation[2].beat == 4 && e->project.automation[3].beat == 4.5 &&
              ed_event_selected(e, 2) && ed_event_selected(e, 3) && !ed_event_selected(e, 0),
          "paste preserves spacing and selects only pasted points");
    ed_duplicate_events(e);
    check(e->project.automation_count == 6 && e->project.automation[4].beat == 5 &&
              e->project.automation[5].beat == 5.5 && ed_event_selected(e, 4),
          "duplicate uses the next occupied beat block and selects copies");
    ed_clipboard(e, 1);
    check(e->project.automation_count == 4, "cut removes only selected event copies");
    ed_undo(e, false);
    check(e->project.automation_count == 6, "one undo restores cut points");
    fixture(e);
    e->automation_cc = 11;
    dm_automation_add(&e->project, (DmAutomation){0, 0xb0, 0, 11, 20, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){1, 0xb0, 0, 11, 80, 0, 0, 0, 0, false});
    ed_select_all(e);
    ed_duplicate_events(e);
    check(e->project.automation[2].beat == 2 && e->project.automation[3].beat == 3,
          "isolated point on a beat boundary occupies that beat when finding the next block");
    fixture(e);
    e->bend_range = 12;
    e->drag_key = 60;
    ed_automation_line(e, 0, 2, 8192, 12288, -2);
    ed_select_all(e);
    ed_clipboard(e, 0);
    ed_curve_range(e, 3, 2, 0, 127, 127, 0);
    e->cursor_beat[1] = 4;
    ed_clipboard(e, 2);
    int point = pitch_node(e, 6);
    check(point >= 0 && fabs(ed_automation_pitch(e, e->project.automation[point]) - 66) < .02,
          "pitch line paste carries bend-range protocol dependencies");
    check(e->project.automation[point].curve != e->project.automation[pitch_node(e, 2)].curve,
          "pasted lines receive independent section identities");
    check(ed_pitch_state(e, 6).range == 2,
          "pasted pitch section restores the destination's incoming sensitivity");
    ed_delete_selection(e);
    check(pitch_node(e, 6) < 0 && pitch_node(e, 2) >= 0,
          "deleting pasted section preserves the original section");
    fixture(e);
    ed_curve_range(e, 0, 12, 0, 127, 127, 0);
    dm_automation_add(&e->project, (DmAutomation){1, 0xe0, 0, 0, 96, 0, 0, 0, 0, false});
    e->automation_node = pitch_node(e, 1);
    ed_clipboard(e, 0);
    fixture(e);
    dm_automation_add(&e->project, (DmAutomation){10, 0xe0, 0, 0, 96, 0, 0, 0, 0, false});
    e->cursor_beat[1] = 4;
    ed_clipboard(e, 2);
    point = pitch_node(e, 4);
    check(point >= 0 && fabs(ed_automation_pitch(e, e->project.automation[point]) - 66) < .02,
          "discrete pitch paste retains its semitone amount across different sensitivities");
    point = pitch_node(e, 10);
    check(point >= 0 && fabs(ed_automation_pitch(e, e->project.automation[point]) - 61) < .02,
          "discrete pitch paste preserves subsequent destination pitches");
}

static void transfer_points(Editor *e) {
    fixture(e);
    dm_automation_add(&e->project, (DmAutomation){0, 0xe0, 0, 0, 64, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){8, 0xe0, 0, 0, 64, 0, 0, 0, 0, false});
    check(ed_transfer_automation(e), "source endpoint messages transfer");
    check(e->project.automation_count == 2 && pitch_node(e, 0) >= 0 && pitch_node(e, 8) >= 0,
          "transfer retains endpoint messages without inventing new points");
    size_t before = e->project.automation_count;
    dm_pattern_add(&e->project, "Blank");
    check(e->project.automation_count == before, "blank creation adds no default endpoints");
}

int main(void) {
    musical_sync();
    scheduled_sync();
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("event_clipboard", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", true);
    ui_frame();
    pitch_ranges(e);
    event_clipboard(e);
    transfer_points(e);
    ui_render(r);
    fixture(e);
    test_event_clipboard_ui(e, check);
    printf("PASS event_clipboard: %d focused checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
