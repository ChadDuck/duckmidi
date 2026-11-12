/* The floating windows: which one owns a click, the sizes they open at, Enter on forms and the
   first-launch update question. */
#include "editor/internal.h"
#include <assert.h>
#define TEST_NAME "floating windows"
#include "test_common.h"
extern void test_floating_windows_ui(Editor *, void (*)(bool, const char *));

/* Draws a few frames so windows opened by the step before are measured and shown. */
static void draw(Editor *e) {
    for (int frame = 0; frame < 6; frame++) {
        ui_frame();
        editor_frame(e, 1400, 900);
        ui_render(e->renderer);
    }
}

/* The first-launch update question waits for an open file, then shows. No network: nothing is
   applied or checked. */
static void update_question(Editor *e) {
    e->active = false;
    e->update_prompted = false;
    ed_update_launch(e);
    draw(e);
    assert(!e->update_window && e->update_prompt_pending && !e->update);
    ed_new_project(e);
    draw(e);
    assert(e->update_window && !e->update_prompt_pending && e->update_choice == 1 && !e->update);
    e->update_window = false;
    e->update_prompted = true;
    puts("PASS update question waits for an open file");
}

/* A pattern with notes on a playlist row, so Generate Patterns has something to preview. */
static void fixture(Editor *e) {
    ed_new_project(e);
    DmPattern *pattern = &e->project.patterns[0];
    pattern->count = 0;
    e->project.clip_count = 0;
    assert(dm_note_add(pattern, (DmNote){0, 4, 60, 100, 0, 0}) &&
           dm_note_add(pattern, (DmNote){0, 1, 60, 100, 0, 1}) &&
           dm_note_add(pattern, (DmNote){0, 2, 67, 100, 0, 127}));
    assert(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0}));
    e->selected_pattern = e->pattern_channel = 0;
}

int main(void) {
    Editor *e = test_editor_open("Floating windows", (TestDesktop){.renderer = "software"});
    update_question(e);
    fixture(e);
    test_floating_windows_ui(e, check);
    printf("PASS floating windows: %d checks\n", checks);
    test_editor_close(e);
    return 0;
}
