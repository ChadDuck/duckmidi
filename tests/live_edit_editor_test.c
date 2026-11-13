#include "editor/internal.h"
#define TEST_NAME "live edit UI"
#include "test_common.h"
extern void test_live_edit_ui(Editor *, void (*)(bool, const char *));

int main(void) {
    Editor *e = test_editor_open("Live edit UI", (TestDesktop){0});
    SDL_Renderer *r = e->renderer;
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
    e->automation_cc = DM_EVENT_BEND;
    e->drag = DRAG_EVENT_POINT;
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
    e->drag = DRAG_EVENT_POINT;
    e->edit_moved = false;
    ed_automation_node_drag(e, (UiRect){100, 100, 400, 300}, &e->roll_view, 80, mouse);
    check(!e->edit_feedback, "non-pitch controller drag does not audition an arbitrary note");
    e->edit_focus = FOCUS_PLAYLIST;
    e->tool = TOOL_SELECT;
    e->drag = DRAG_NONE;
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

    mouse = (UiMouse){.clicked = true, .ctrl = true};
    ed_playlist_hit_click(e, mouse, 0, 0);
    ed_playlist_hit_click(e, mouse, 1, 0);
    check(e->project.clips[0].selected && e->project.clips[1].selected && !e->drag,
          "Ctrl click adds to the playlist selection without starting a drag");
    ed_playlist_hit_click(e, mouse, 0, 0);
    check(!e->project.clips[0].selected && e->project.clips[1].selected,
          "Ctrl click toggles only the clicked playlist placement");
    ui_end();
    ui_render(r);
    printf("PASS live edit UI: %d checks\n", checks);
    test_editor_close(e);
}
