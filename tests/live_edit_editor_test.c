#include "editor_internal.h"
static int checks;

static void check(bool value, const char *text) {
    ++checks;

    if (!value) {
        fprintf(stderr, "FAIL live edit UI: %s\n", text);
        exit(1);
    }
}

extern void test_live_edit_ui(Editor *, void (*)(bool, const char *));

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Live edit UI", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", true);
    editor_smoke(e);
    test_live_edit_ui(e, check);
    ui_frame();
    ui_root(0, 900);
    e->project.automation_count = 0;
    check(dm_automation_add(&e->project, (DmAutomation){.pattern = e->selected_pattern,
                                                        .type = 0xe0,
                                                        .channel = e->pattern_channel,
                                                        .b = 64,
                                                        .anchor = 60}),
          "isolated pitch automation fixture");
    e->event_drag.size = 0;
    e->automation_node = 0;
    e->automation_raw = true;
    e->automation_cc = -2;
    e->drag = 5;
    e->edit_moved = false;
    e->node_beat = e->project.automation[0].beat;
    e->marquee_start = (UiPoint){120, 200};
    UiMouse mouse = {.left = true, .pos = {140, 160}};
    ed_automation_node_drag(e, (UiRect){100, 100, 400, 300}, &e->roll_view, 80, mouse);
    check(e->edit_feedback, "event lane pitch-bend point drag auditions pitch");
    ed_stop_edit_feedback(e);
    e->project.automation[0].type = 0xb0;
    e->project.automation[0].a = 11;
    e->automation_node = 0;
    e->automation_cc = 11;
    e->drag = 5;
    e->edit_moved = false;
    ed_automation_node_drag(e, (UiRect){100, 100, 400, 300}, &e->roll_view, 80, mouse);
    check(!e->edit_feedback, "non-pitch controller drag does not audition an arbitrary note");
    e->context = 1;
    e->edit_focus = FOCUS_PLAYLIST;
    e->tool = 0;
    e->drag = 0;
    int list[] = {0, 1, 9};
    size_t clips = e->project.clip_count;
    mouse = (UiMouse){.clicked = true, .ctrl = true, .pos = {120, 110}};
    ed_playlist_click(e, (UiRect){36, 72, 564, 328}, (UiRect){100, 100, 500, 300},
                      &e->playlist_view, 64, 40, list, 3, mouse, 0);
    check(e->marquee && !e->drag && e->project.clip_count == clips,
          "Ctrl over a playlist placement starts marquee instead of toggling or moving it");
    e->marquee = false;

    for (size_t i = 0; i < clips; ++i) {
        e->project.clips[i].selected = 0;
    }

    mouse = (UiMouse){.clicked = true, .shift = true};
    ed_playlist_hit_click(e, mouse, 0, 0);
    e->drag = 0;
    ed_playlist_hit_click(e, mouse, 1, 0);
    e->drag = 0;
    check(e->project.clips[0].selected && e->project.clips[1].selected,
          "Shift click keeps additive playlist selection");
    ed_playlist_hit_click(e, mouse, 0, 0);
    check(!e->project.clips[0].selected && e->project.clips[1].selected,
          "Shift click toggles only the clicked playlist placement");
    ui_end();
    ui_render(r);
    printf("PASS live edit UI: %d checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
