#include "editor_internal.h"
static int checks;
static void check(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL selection_history: %s\n", text);
        exit(1);
    }
}
void test_selection_history_ui(Editor *e, void (*check)(bool, const char *));
static void editing_focus(Editor *e) {
    for (int mode = 0; mode < 2; ++mode) {
        editor_smoke(e);
        e->pattern_mode = mode;
        e->automation_cc = 11;
        dm_automation_add(&e->project, (DmAutomation){.5, 0xb0, 0, 11, 90, 0, 0, 0, 0, false});
        e->automation_node = 0;
        e->project.clips[0].selected = 1;
        e->project.patterns[0].notes[0].selected = 1;
        size_t notes = ed_pattern(e)->count, clips = e->project.clip_count;
        ed_focus_edit(e, FOCUS_EVENTS);
        ed_shortcut(e, SDL_SCANCODE_DELETE, 0, false);
        check(!e->project.automation_count && e->project.clip_count == clips &&
                  ed_pattern(e)->count == notes,
              "Delete in event focus removes the graph point in either playback mode");
        dm_automation_add(&e->project, (DmAutomation){.5, 0xb0, 0, 11, 90, 0, 0, 0, 0, false});
        dm_automation_add(&e->project, (DmAutomation){1, 0xb0, 0, 11, 60, 0, 0, 0, 0, false});
        dm_automation_add(&e->project, (DmAutomation){1, 0xb0, 0, 1, 60, 0, 0, 0, 0, false});
        ed_shortcut(e, SDL_SCANCODE_A, SDL_KMOD_CTRL, false);
        check(e->automation_all, "Ctrl+A selects displayed graph points");
        ed_shortcut(e, SDL_SCANCODE_DELETE, 0, false);
        check(e->project.automation_count == 1 && e->project.automation[0].a == 1 &&
                  ed_pattern(e)->count == notes && e->project.clip_count == clips,
              "graph Ctrl+A/Delete leaves other event types, notes and clips intact");
        ed_focus_edit(e, FOCUS_NOTES);
        ed_shortcut(e, SDL_SCANCODE_DELETE, 0, false);
        check(ed_pattern(e)->count == notes - 1 && e->project.clip_count == clips,
              "note focus deletes notes rather than unrelated selected clips");
        ed_focus_edit(e, FOCUS_OTHER);
        ed_shortcut(e, SDL_SCANCODE_DELETE, 0, false);
        check(e->project.clip_count == clips,
              "mixer focus does not delete unfocused clip selections");
    }
}
static void history(Editor *e) {
    editor_smoke(e);
    int old = ed_pattern(e)->notes[0].key;
    ed_checkpoint(e);
    ed_pattern(e)->notes[0].key = old + 1;
    ed_changed(e);
    ed_pattern(e)->notes[0].selected = 0;
    int history_count = e->undo_count;
    ed_choose_pattern_card(e, 1, false);
    e->project.patterns[1].notes[0].selected = 1;
    e->selected_channel = 7;
    e->pattern_channel = 1;
    e->roll_view.scroll_x = 100;
    e->roll_view.scroll_y = 700;
    check(e->undo_count == history_count, "pattern selection is not an undo action");
    ed_undo(e, false);
    check(e->project.patterns[0].notes[0].key == old && e->selected_pattern == 1 &&
              e->selected_channel == 7 && e->pattern_channel == 1 && e->roll_view.scroll_x == 100 &&
              e->roll_view.scroll_y == 700,
          "undo changes the other pattern in the background without changing view/channel");
    check(!e->project.patterns[0].notes[0].selected && e->project.patterns[1].notes[0].selected,
          "undo does not restore historical note selections");
    ed_undo(e, true);
    check(e->project.patterns[0].notes[0].key == old + 1 && e->selected_pattern == 1,
          "redo also leaves the currently open pattern alone");
    editor_smoke(e);
    e->dirty = false;
    UiMouse m = {0};
    m.pos = (UiPoint){100, 100};
    history_count = e->undo_count;
    ed_playlist_hit_click(e, m, 0, 0);
    check(e->undo_count == history_count && e->pattern_highlight,
          "playlist selection synchronizes the card without history");
    m.released = true;
    int list[] = {0, 1, 9};
    ed_playlist_drag(e, (UiRect){0, 0, 600, 400}, &e->playlist_view, 64, 40, list, 3, m);
    check(!e->dirty && e->undo_count == history_count,
          "stationary playlist release remains selection only");
    e->automation_cc = 11;
    dm_automation_add(&e->project, (DmAutomation){1, 0xb0, 0, 11, 80, 0, 0, 0, 0, false});
    m = (UiMouse){0};
    m.pos = (UiPoint){80, 50};
    ed_automation_lane_click(e, (UiRect){0, 0, 400, 200}, &e->roll_view, 80, m, 0);
    m.released = true;
    ed_automation_node_drag(e, (UiRect){0, 0, 400, 200}, &e->roll_view, 80, m);
    check(e->automation_node == 0 && e->undo_count == history_count && !e->dirty,
          "selecting and releasing a graph point stays silent and out of history");
}
static void selection_visibility(Editor *e) {
    editor_smoke(e);
    ed_open_pattern(e, 0, false);
    e->project.clips[0].selected = 1;
    ed_pattern(e)->notes[0].selected = 1;
    ed_focus_edit(e, FOCUS_NOTES);
    check(ed_deselect_focused(e) && !ed_pattern(e)->notes[0].selected &&
              e->project.clips[0].selected,
          "Escape in note focus clears notes without clearing playlist selection");
    ed_focus_edit(e, FOCUS_PATTERNS);
    check(ed_deselect_focused(e) && !e->pattern_highlight && !e->project.clips[0].selected &&
              e->roll && e->roll_auto_hidden,
          "deselecting a pattern synchronizes highlights and temporarily hides its editor");
    ed_open_pattern(e, 1, false);
    check(e->roll && !e->roll_auto_hidden,
          "another pattern restores an automatically hidden editor");
    ed_toggle_roll(e);
    ed_open_pattern(e, 0, false);
    check(!e->roll, "ordinary pattern selection respects manual editor hiding");
    ed_open_pattern(e, 0, true);
    check(e->roll && !e->roll_auto_hidden, "new pattern creation explicitly restores the editor");
    ed_focus_edit(e, FOCUS_EVENTS);
    e->automation_all = true;
    e->automation_node = 0;
    check(ed_deselect_focused(e) && !e->automation_all && e->automation_node < 0,
          "Escape in event focus clears graph selection");
}
static void copy_seeking(Editor *e) {
    editor_smoke(e);
    ed_pattern(e)->length = 8;
    e->project.clip_count = 0;
    for (int i = 0; i < 3; ++i) {
        dm_clip_add(&e->project, (DmClip){i * 8, 0, 0, 0, 0, 0});
    }
    e->selected_clip = -1;
    e->cursor_beat[0] = 2;
    check(ed_pattern_seek_origin(e) == 0, "first-half seek targets the current copy");
    e->cursor_beat[0] = 4;
    check(ed_pattern_seek_origin(e) == 0, "exact halfway stays with the current copy");
    e->cursor_beat[0] = 6;
    check(ed_pattern_seek_origin(e) == 8, "past-halfway seek targets the next matching copy");
    e->cursor_beat[0] = 15;
    check(ed_pattern_seek_origin(e) == 16, "later copies obey the same halfway rule");
    e->cursor_beat[0] = 23;
    check(ed_pattern_seek_origin(e) == 16, "last copy stays current when no later copy exists");
    e->pattern_mode = false;
    e->scrub_origin = 8;
    ed_scrub_publish(e, 2, 2, false);
    check(e->cursor_beat[0] == 10 && e->start_beat[0] == 10,
          "Song seek maps pattern-local beat through chosen copy");
    e->pattern_mode = true;
    ed_scrub_publish(e, 2, 2, false);
    check(e->cursor_beat[1] == 2 && e->pattern_mode,
          "Pattern seek stays local and preserves playback choice");
}
int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("selection_history checks", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", true);
    editing_focus(e);
    history(e);
    selection_visibility(e);
    copy_seeking(e);
    editor_smoke(e);
    test_selection_history_ui(e, check);
    printf("PASS selection_history: %d focused checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
