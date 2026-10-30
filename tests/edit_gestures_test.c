/* Production editor boundaries and state transactions, with a real hidden ImGui/SDL renderer. */
#include "editor_internal.h"
static int checks;
static void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL edit_gestures: %s\n", label);
        exit(1);
    }
}
static void history_and_duplication(Editor *e) {
    editor_smoke(e);
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->piano = true;
    ed_shortcut(e, SDL_SCANCODE_A, SDL_KMOD_CTRL, false);
    bool all = true;
    for (size_t i = 0; i < ed_pattern(e)->count; ++i) {
        all &= ed_pattern(e)->notes[i].selected;
    }
    check(all, "Ctrl A selects every note with keyboard piano enabled");
    check(ed_reserved(SDL_SCANCODE_A, SDL_KMOD_CTRL), "Ctrl A cannot be reassigned");
    e->context = 1;
    e->edit_focus = FOCUS_PLAYLIST;
    ed_shortcut(e, SDL_SCANCODE_A, SDL_KMOD_CTRL, false);
    all = true;
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        all &= e->project.clips[i].selected;
    }
    check(all, "Ctrl A selects all playlist placements");
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    DmPattern *p = ed_pattern(e);
    p->count = 0;
    p->length = 4;
    dm_note_add(p, (DmNote){.25, .25, 60, 100, 1});
    dm_note_add(p, (DmNote){.75, .25, 64, 90, 1});
    ed_shortcut(e, SDL_SCANCODE_D, SDL_KMOD_CTRL, false);
    check(p->count == 4 && p->notes[2].beat == 1.25 && p->notes[3].beat == 1.75,
          "Ctrl D duplicates the occupied beat, retaining positions within it");
    check(!p->notes[0].selected && !p->notes[1].selected && p->notes[2].selected &&
              p->notes[3].selected,
          "only the duplicated notes remain selected");
    ed_undo(e, false);
    p = ed_pattern(e);
    check(p->count == 2, "duplicate operation is one undo");
    ed_undo(e, true);
    p = ed_pattern(e);
    check(p->count == 4, "duplicate operation is one redo");
    p->count = 0;
    dm_note_add(p, (DmNote){.25, .25, 60, 100, 1});
    dm_note_add(p, (DmNote){3.25, .5, 64, 90, 1});
    ed_duplicate_notes(e);
    check(p->notes[2].beat == 4.25 && p->notes[3].beat == 7.25,
          "a bar-sized selection duplicates to the next bar");
    for (size_t i = 0; i < p->count; ++i) {
        p->notes[i].selected = 0;
    }
    size_t count = p->count;
    ed_duplicate_notes(e);
    check(p->count == count, "empty selection does not duplicate anything");
    e->piano = false;
    e->recording_take = true;
    e->take_snapshot = false;
    int snapshots = e->undo_count;
    ed_checkpoint(e);
    ed_add_note(e, p, (DmNote){8, .1, 70, 100, 0});
    ed_checkpoint(e);
    ed_add_note(e, p, (DmNote){8.3, .1, 72, 100, 0});
    check(e->undo_count == snapshots + 1, "one take creates one snapshot for multiple changes");
    e->recording_take = e->take_snapshot = false;
    ed_undo(e, false);
    check(ed_pattern(e)->count == count, "take undo restores the state before all its changes");
    ed_undo(e, true);
    check(ed_pattern(e)->count == count + 2, "take redo restores all changes together");
}
static void resizing_and_quantize(Editor *e) {
    editor_smoke(e);
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    DmPattern *p = ed_pattern(e);
    p->count = 0;
    dm_note_add(p, (DmNote){1, 1, 60, 100, 1});
    dm_note_add(p, (DmNote){2, .5, 64, 90, 1});
    dm_note_add(p, (DmNote){3, 2, 67, 90, 0});
    free(e->drag_notes);
    e->drag_notes = malloc(3 * sizeof(DmNote));
    memcpy(e->drag_notes, p->notes, 3 * sizeof(DmNote));
    e->drag_note_count = 3;
    e->drag_beat = 1;
    e->drag_length = 1;
    e->drag_edge = 1;
    ed_resize_note_group(e, p, 2, false);
    check(p->notes[0].length == 3 && p->notes[1].length == 2.5 && p->notes[2].length == 2,
          "right edge grows every selected note by the same amount");
    ed_resize_note_group(e, p, -100, false);
    double minimum = 1.0 / DM_PPQ;
    check(fabs(p->notes[1].length - minimum) < 1e-8 &&
              fabs(p->notes[0].length - (.5 + minimum)) < 1e-8,
          "the shortest note stops the whole group from shrinking further");
    ed_resize_note_group(e, p, 100000, false);
    check(p->notes[0].length > 99999 && p->notes[1].length > 99999,
          "right resize has no artificial length cap");
    e->drag_edge = -1;
    ed_resize_note_group(e, p, 100, false);
    check(fabs(p->notes[0].beat + p->notes[0].length - 2) < 1e-8 &&
              fabs(p->notes[1].beat + p->notes[1].length - 2.5) < 1e-8,
          "left edge group resizing retains every original end");
    check(fabs(p->notes[1].length - minimum) < 1e-8, "left edge also stops at shortest note");
    ed_resize_note_group(e, p, -100, false);
    check(p->notes[0].beat == 0 && p->notes[1].beat == 1,
          "left edge movement stops at the earliest possible beat for the whole group");
    p->count = 0;
    dm_note_add(p, (DmNote){.13, .39, 60, 100, 1});
    dm_note_add(p, (DmNote){1.13, .4, 64, 100, 1});
    dm_note_add(p, (DmNote){2.13, .03, 67, 100, 1});
    dm_note_add(p, (DmNote){3.13, .39, 69, 100, 0});
    dm_quantize(p, .25);
    check(p->notes[0].beat == .25 && p->notes[0].length == .25 && p->notes[1].length == .25,
          "quantize snaps starts and ends to equal grid spans");
    check(p->notes[2].length == .25, "collapsed quantized notes retain one grid cell");
    check(p->notes[3].beat == 3.13 && p->notes[3].length == .39,
          "quantize preserves unselected notes when a selection exists");
}
static void gestures(Editor *e) {
    editor_smoke(e);
    e->piano = true;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->tool = 0;
    DmPattern *p = ed_pattern(e);
    UiRect grid = {100, 100, 500, 300}, r = {36, 72, 564, 328};
    UiMouse mouse = {.pos = {120, 110}, .clicked = true, .ctrl = true};
    size_t count = p->count;
    ed_piano_click(e, p, r, grid, &e->roll_view, 80, 20, mouse, 0, (UiRect){100, 100, 40, 20});
    check(e->marquee && e->drag == 0 && p->count == count && e->tool == 0,
          "Ctrl left starts temporary marquee even over an existing note");
    e->marquee = false;
    e->context = 1;
    e->edit_focus = FOCUS_PLAYLIST;
    int list[] = {0, 1};
    ed_playlist_click(e, r, grid, &e->playlist_view, 64, 40, list, 2, mouse, 0);
    check(e->marquee && e->drag == 0 && e->tool == 0,
          "Shift left also overrides a playlist clip without changing the chosen tool");
    e->marquee = false;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->erase_active = false;
    int snapshots = e->undo_count;
    mouse = (UiMouse){.pos = {120, 110}, .right = true, .right_clicked = true};
    ed_piano_mouse(e, p, r, grid, &e->roll_view, 80, 20, mouse, true, 0, (UiRect){0});
    mouse.right_clicked = false;
    ed_piano_mouse(e, p, r, grid, &e->roll_view, 80, 20, mouse, true, 0, (UiRect){0});
    check(p->count == count - 2 && e->undo_count == snapshots + 1,
          "held right mouse deletes hovered notes as one sweep transaction");
    ed_undo(e, false);
    check(ed_pattern(e)->count == count, "one undo restores the entire erase sweep");
}
int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL initializes");
    SDL_Window *window = SDL_CreateWindow("edit_gestures validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "direct3d11");
    check(window && renderer && ui_init(window, renderer, NULL), "real renderer initializes");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, window, renderer, NULL, "", true);
    ui_frame();
    ui_root(0, 900);
    history_and_duplication(e);
    resizing_and_quantize(e);
    gestures(e);
    e->automation_cc = 11;
    UiRect lane = {0, 0, 400, 200};
    DmAutomation control = {1, 0xb0, 0, 11, 64, 0, 0, 0, 0, false};
    UiPoint before = ed_automation_point(e, control, lane, &e->roll_view, 80);
    ed_automation_pan_y(e, lane, 30);
    UiPoint after = ed_automation_point(e, control, lane, &e->roll_view, 80);
    check(fabs(after.y - before.y) < .001,
          "numeric controller lane keeps its full range when panned");
    check(abs(ed_lane_value(e, lane, after.y, control, false) - 64) <= 1,
          "fitted controller coordinates preserve their MIDI values");
    ui_end();
    ui_render(renderer);
    e->piano = false;
    e->confirm = true;
    e->pending_action = PENDING_QUIT;
    for (int frame = 0; frame < 2; ++frame) {
        ui_frame();
        editor_frame(e, 1400, 900);
        SDL_RenderClear(renderer);
        ui_render(renderer);
        if (frame == 1) {
            SDL_Surface *capture = SDL_RenderReadPixels(renderer, NULL);
            check(capture && SDL_SaveBMP(capture, "agents/reports/edit_gestures-confirm.bmp"),
                  "capture save confirmation");
            SDL_DestroySurface(capture);
        }
        SDL_RenderPresent(renderer);
    }
    e->confirm = false;
    uint8_t tempo[] = {0xff, 0x51, 3, 7, 0xa1, 0x20};
    uint8_t meter[] = {0xff, 0x58, 4, 3, 2, 24, 8};
    DmBytes bytes = {tempo, sizeof(tempo), sizeof(tempo)};
    dm_event_add(&e->project, 0, 0, 0, &bytes);
    bytes = (DmBytes){meter, sizeof(meter), sizeof(meter)};
    dm_event_add(&e->project, 2, 0, 0, &bytes);
    ed_song_event_open(e, 1, false);
    check(e->event_edit == 1 && e->event_kind == 1 && e->event_beat == 2 && e->event_numerator == 3,
          "source-event selection updates the selected event's identity and editable fields");
    for (int frame = 0; frame < 2; ++frame) {
        ui_frame();
        editor_frame(e, 1400, 900);
        SDL_RenderClear(renderer);
        ui_render(renderer);
        if (frame == 1) {
            SDL_Surface *capture = SDL_RenderReadPixels(renderer, NULL);
            check(capture && SDL_SaveBMP(capture, "agents/reports/edit_gestures-event.bmp"),
                  "capture selected source event");
            SDL_DestroySurface(capture);
        }
        SDL_RenderPresent(renderer);
    }
    printf("PASS edit_gestures: %d targeted checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
