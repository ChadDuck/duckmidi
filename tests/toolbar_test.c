/* The toolbars: Record, Play and Wait buttons, status-bar help, the pattern editor's corner menu
   and lane ruler, the icon outlines and the mouse cursors. */
#include "editor/internal.h"
#define TEST_NAME "toolbar"
#include "test_common.h"
extern void test_toolbar_ui(Editor *, void (*)(bool, const char *));
extern void test_pattern_toolbar_ui(Editor *, void (*)(bool, const char *));

int main(void) {
    Editor *e =
        test_editor_open("Toolbar", (TestDesktop){.soundfont = TEST_SOUNDFONT, .paused = true});
    editor_smoke(e);
    test_toolbar_ui(e, check);
    editor_smoke(e);
    test_pattern_toolbar_ui(e, check);
    printf("PASS toolbar: %d checks\n", checks);
    test_editor_close(e);
    return 0;
}
