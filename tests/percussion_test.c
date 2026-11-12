/* The percussion mapping window: learning a key, playing the draft, the project's own mapping
   over the global one, Enter, undo, and the window's lifetime across projects. */
#include "editor/internal.h"
#include <assert.h>
#define TEST_NAME "percussion"
#include "test_common.h"

/* Draws a few frames so windows opened by the step before are measured and shown. */
static void draw(Editor *e) {
    for (int frame = 0; frame < 6; frame++) {
        ui_frame();
        editor_frame(e, 1400, 900);
        ui_render(e->renderer);
    }
}

static void fixture(Editor *e) {
    ed_save_free(e);
    ed_release_piano(e);
    ed_clear_history(e->undo, &e->undo_count);
    ed_clear_history(e->redo, &e->redo_count);
    dm_project_free(&e->project);
    dm_project_init(&e->project);
    ++e->project_session;
    int p = dm_pattern_add(&e->project, "Generated fixture");
    assert(p == 0);
    assert(dm_note_add(&e->project.patterns[0], (DmNote){0, 1, 60, 100, 0}));
    e->active = true;
    e->selected_pattern = e->selected_channel = e->pattern_channel = 0;
    e->drag = DRAG_NONE;
    e->tool = TOOL_SELECT;
    e->percussion_window = false;
}

static void key(Editor *e, SDL_Scancode scancode) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = scancode;
    event.key.down = true;
    editor_event(e, &event);
}

static void learning_and_override(Editor *e) {
    fixture(e);
    e->piano = false;
    e->piano_octave = 2;
    ed_percussion_open(e);
    e->percussion_note = 38;
    e->percussion_listening = true;
    SDL_Event key = {0};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_F1;
    key.key.key = SDLK_F1;
    editor_event(e, &key);
    assert(e->percussion_listening);
    key.key.scancode = SDL_SCANCODE_ESCAPE;
    key.key.key = SDLK_ESCAPE;
    editor_event(e, &key);
    assert(!e->percussion_listening && e->percussion_window);
    e->percussion_listening = true;
    key.key.scancode = SDL_SCANCODE_Z;
    key.key.key = SDLK_Z;
    editor_event(e, &key);
    assert(!e->percussion_listening && !e->percussion_draft[36] && e->percussion_draft[38]);

    /* Not listening: the key plays the drum it is mapped to in the draft. */
    editor_event(e, &key);
    assert(e->held[SDL_SCANCODE_Z] == 38 && e->held_channels[SDL_SCANCODE_Z] == MIDI_DRUM_CHANNEL);
    key.type = SDL_EVENT_KEY_UP;
    editor_event(e, &key);
    assert(e->held[SDL_SCANCODE_Z] == -1);
    key.type = SDL_EVENT_KEY_DOWN;
    ed_percussion_save(e, true);
    assert(e->project.percussion_override && e->dirty);
    draw(e);
    assert(ed_keyboard_note(e, SDL_SCANCODE_Z, true) == 38 &&
           ed_keyboard_note(e, SDL_SCANCODE_Z, false) == 72);
    assert(e->percussion_global[36]);
    DmBytes b = {0};
    assert(dm_pack(&e->project, &b));
    DmProject copy;
    dm_project_init(&copy);
    char error[256];
    assert(dm_unpack(&copy, b.data, b.size, error, sizeof(error)));
    assert(copy.percussion_override && copy.percussion_keys[38]);
    dm_project_free(&copy);
    dm_bytes_free(&b);
    ed_undo(e, false);
    assert(!e->project.percussion_override && ed_keyboard_note(e, SDL_SCANCODE_Z, true) == 36);
    e->percussion_window = false;
    puts("PASS percussion learning, override precedence, persistence and undo");
}

static void percussion_window(Editor *e) {
    ed_new_project(e);
    e->project.percussion_override = true;
    memset(e->project.percussion_keys, 0, 128);
    ed_percussion_open(e);
    e->percussion_draft[36] = 1;
    key(e, SDL_SCANCODE_RETURN);
    check(!e->percussion_window && e->project.percussion_keys[36] == 1,
          "Enter applies the draft to the project's own mapping and closes the window");

    ed_percussion_open(e);
    e->percussion_draft[38] = 2;
    check(e->percussion_window, "percussion window opens");
    ed_new_project(e);
    check(!e->percussion_window && !e->project.percussion_override &&
              e->project.percussion_keys[36] == 0 &&
              !memcmp(e->percussion_draft, e->percussion_global, 128),
          "a new project closes the window and drops the unsaved draft");
}

int main(void) {
    Editor *e = test_editor_open("Percussion", (TestDesktop){.renderer = "software"});
    snprintf(e->settings_path, DM_PATH, "agents/reports/percussion-test.ini");
    e->percussion_global[36] = (uint8_t)(ed_piano_index(SDL_SCANCODE_Z) + 1);
    learning_and_override(e);
    percussion_window(e);
    printf("PASS percussion: %d checks\n", checks);
    test_editor_close(e);
    return 0;
}
