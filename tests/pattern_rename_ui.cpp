/* Drives the pattern rename field with typed input. */
#include "test_ui.h"
#include <cassert>

/* Rename focuses its field, Enter applies the name and one Escape discards it. */
extern "C" void test_pattern_rename_ui(Editor *e) {
    frame(e);
    e->rename_pattern = 0;
    std::strcpy(e->rename_text, "Original draft");
    frame(e);
    frame(e);
    assert(ui_text_active());
    replace(e, "Renamed");
    key(e, ImGuiKey_Enter);
    assert(e->rename_pattern == -1 && !std::strcmp(e->project.patterns[0].name, "Renamed"));
    e->rename_pattern = 0;
    std::strcpy(e->rename_text, "Discard this");
    frame(e);
    frame(e);
    assert(ui_text_active());
    ed_shortcut(e, SDL_SCANCODE_ESCAPE, (SDL_Keymod)0, false);
    frame(e);
    assert(e->rename_pattern == -1 && !std::strcmp(e->project.patterns[0].name, "Renamed"));
}
