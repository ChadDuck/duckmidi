#include "editor_internal.h"

extern void test_event_controls_ui(Editor *, void (*)(bool, const char *));
extern void test_event_controls_audio(DmAudio *, void (*)(bool, const char *));
static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL event controls: %s\n", text);
        exit(1);
    }
}

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
    e->drag = 0;
    e->context = 2;
    e->edit_focus = FOCUS_EVENTS;
    e->tool = 0;
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

    fixture(e, 7, false);
    check(!ed_automation_option(e, 7) && !ed_automation_option(e, 10),
          "volume and pan absent from note pattern selector");
    ed_open_pattern(e, 0, false);
    check(e->automation_cc == -3, "opening note pattern leaves volume automation view");
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
            check(e->project.automation_count == 3 && e->drag == 0,
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
    UiMouse m = {.clicked = true, .left = true, .shift = true, .pos = {180, 180}};
    ed_automation_lane_click(e, lane, &v, 80, m, -1);
    m.clicked = false;
    m.left = false;
    m.released = true;
    ed_automation_drag(e, lane, &v, 80, m);
    check(e->project.automation_count == 3,
          "single Shift portamento click creates bounded section");
    m = (UiMouse){.clicked = true, .pos = {340, 140}};
    ed_automation_lane_click(e, lane, &v, 80, m, -1);
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
    }

    size_t added = e->project.automation_count;
    check(added == 3, "left clicks add program change points");
    ed_automation_lane_click(e, lane, &v, 80,
                             (UiMouse){.clicked = true, .left = true, .pos = {140, 180}}, 0);
    ed_automation_node_drag(e, lane, &v, 80, (UiMouse){.released = true, .pos = {140, 180}});
    e->drag = 0;
    ed_shortcut(e, SDL_SCANCODE_DELETE, SDL_KMOD_NONE, false);
    check(e->project.automation_count == added - 1,
          "Delete removes a selected program change point");
    e->erase_active = false;
    ed_erase_node(e, 0);
    check(e->project.automation_count == added - 2,
          "right-drag erase removes program change points");
}

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Event controls", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio != NULL, "audio backend");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, audio, "", true);
    ui_frame();
    ui_root(0, 900);
    continuous(e);
    switches_and_glide(e);
    program_change_points(e);
    ui_end();
    ui_render(r);
    fixture(e, 64, true);
    test_event_controls_ui(e, check);
    test_event_controls_audio(audio, check);
    printf("PASS event controls: %d checks\n", checks);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
