#include "editor_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks;
void test_editor_mouse(Editor *e, void (*check)(bool, const char *));
void test_editor_updates(Editor *e, void (*check)(bool, const char *));
static void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL editor: %s\n", label);
        exit(1);
    }
}
static void press(Editor *e, SDL_Scancode key, SDL_Keymod mods) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.down = true;
    event.key.scancode = key;
    event.key.mod = mods;
    editor_event(e, &event);
}
static void frame(Editor *e) {
    ui_frame();
    editor_frame(e, 1400, 900);
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_RenderPresent(e->renderer);
}
static void notes(Editor *e) {
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->selected_pattern = 0;
    for (size_t i = 0; i < e->project.patterns[0].count; ++i) {
        e->project.patterns[0].notes[i].selected = 0;
    }
    e->project.patterns[0].notes[0].selected = 1;
    int pitch = e->project.patterns[0].notes[0].key;
    press(e, SDL_SCANCODE_UP, SDL_KMOD_SHIFT);
    check(e->project.patterns[0].notes[0].key == pitch + 1, "semitone transpose");
    press(e, SDL_SCANCODE_UP, SDL_KMOD_CTRL);
    check(e->project.patterns[0].notes[0].key == pitch + 13, "octave transpose");
    size_t count = e->project.patterns[0].count;
    press(e, SDL_SCANCODE_DELETE, 0);
    check(e->project.patterns[0].count == count - 1, "delete note");
    press(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    check(e->project.patterns[0].count == count, "undo note deletion");
    press(e, SDL_SCANCODE_Y, SDL_KMOD_CTRL);
    check(e->project.patterns[0].count == count - 1, "redo note deletion");
    press(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    e->project.patterns[0].notes[0].selected = 1;
    press(e, SDL_SCANCODE_C, SDL_KMOD_CTRL);
    press(e, SDL_SCANCODE_V, SDL_KMOD_CTRL);
    check(e->project.patterns[0].count == count + 1, "copy and paste notes");
}
static void clips(Editor *e) {
    e->context = 1;
    e->edit_focus = FOCUS_PLAYLIST;
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        e->project.clips[i].selected = 0;
    }
    e->project.clips[0].selected = 1;
    size_t count = e->project.clip_count;
    press(e, SDL_SCANCODE_C, SDL_KMOD_CTRL);
    press(e, SDL_SCANCODE_V, SDL_KMOD_CTRL);
    check(e->project.clip_count == count + 1, "copy and paste clips");
    press(e, SDL_SCANCODE_DELETE, 0);
    check(e->project.clip_count == count, "delete pasted clip");
    press(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    check(e->project.clip_count == count + 1, "undo clip deletion");
}
static void test_shortcuts(Editor *e) {
    press(e, SDL_SCANCODE_F1, 0);
    check(e->piano, "keyboard piano enabled");
    e->tool = 3;
    press(e, SDL_SCANCODE_1, 0);
    check(e->tool == 3, "piano suppresses tool keys");
    press(e, SDL_SCANCODE_K, 0);
    check(!e->shortcuts, "piano suppresses letter shortcuts");
    press(e, SDL_SCANCODE_F1, 0);
    check(!e->piano, "keyboard piano disabled");
    press(e, SDL_SCANCODE_K, 0);
    check(e->shortcuts, "open shortcut window");
    frame(e);
    e->about = true;
    frame(e);
    press(e, SDL_SCANCODE_ESCAPE, 0);
    check(!e->about && e->shortcuts, "Escape closes topmost window");
    frame(e);
    press(e, SDL_SCANCODE_ESCAPE, 0);
    check(!e->shortcuts && e->running, "Escape closes floating window without quitting");
    e->selected_pattern = (int)e->project.pattern_count - 1;
    size_t count = e->project.pattern_count;
    press(e, SDL_SCANCODE_KP_PLUS, 0);
    check(e->project.pattern_count == count + 1, "next creates a pattern at end");
    press(e, SDL_SCANCODE_KP_MINUS, 0);
    check(e->selected_pattern == (int)count - 1, "previous pattern");
    check(e->project.pattern_count == count,
          "unused newly-created pattern is removed when stepping back");
}
static void save_load(Editor *e) {
    snprintf(e->project.name, sizeof(e->project.name), "Editor validation");
    snprintf(e->path, sizeof(e->path), "agents/reports/editor-validation.duck");
    e->dirty = true;
    press(e, SDL_SCANCODE_S, SDL_KMOD_CTRL);
    frame(e);
    uint64_t save_start=SDL_GetTicks();
    while(ed_save_busy(e)&&SDL_GetTicks()-save_start<10000){ed_save_poll(e);SDL_Delay(1);}
    check(!e->dirty, "Ctrl+S saves project");
    snprintf(e->project.name, sizeof(e->project.name), "Changed in isolated test");
    SDL_Event drop;
    SDL_zero(drop);
    drop.type = SDL_EVENT_DROP_FILE;
    drop.drop.data = "agents/reports/editor-validation.duck";
    editor_event(e, &drop);
    check(!strcmp(e->project.name, "Editor validation"), "drop loads saved project");
    check(!e->config, "project load bypasses import configuration");
}
int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL video initialization");
    SDL_Window *window = SDL_CreateWindow("duckmidi validation", 1400, 900, SDL_WINDOW_HIDDEN);
    check(window != NULL, "window");
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "direct3d11");
    check(renderer != NULL, "GPU renderer");
    check(ui_init(window, renderer, NULL), "ImGui initialization");
    Editor *e = calloc(1, sizeof(*e));
    check(e != NULL, "editor allocation");
    editor_init(e, window, renderer, NULL, "", true);
    editor_smoke(e);
    frame(e);
    test_editor_mouse(e, check);
    notes(e);
    clips(e);
    test_shortcuts(e);
    save_load(e);
    test_editor_updates(e, check);
    frame(e);
    printf("PASS editor: %d checks (editing, clipboard, undo/redo, shortcut context, Escape, "
           "save/drop)\n",
           checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
