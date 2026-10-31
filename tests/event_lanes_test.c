#include "editor_internal.h"
extern void test_event_lanes_ui(Editor *, void (*)(bool, const char *));
static int checks;

static void check(bool ok, const char *label) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL event lanes: %s\n", label);
        exit(1);
    }
}

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

    e->automation_cc = -2;
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
    e->automation_cc = -2;
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

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Event lanes", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "real renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", "", true);
    editor_smoke(e);
    ui_frame();
    ui_root(0, 900);
    numeric_ranges(e);
    parameter_ownership(e);
    ui_end();
    ui_render(r);
    editor_smoke(e);
    test_event_lanes_ui(e, check);
    printf("PASS event lanes: %d checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
