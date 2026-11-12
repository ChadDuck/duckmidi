#include "editor/internal.h"
#define TEST_NAME "visualizers"
#include "test_common.h"

extern void recording_feedback_audio_render(DmAudio *, int);
extern void test_visualizers_ui(Editor *, void (*)(bool, const char *));
extern void visualizers_audio_panic_checks(DmAudio *, void (*)(bool, const char *));

int main(void) {
    Editor *e = test_editor_open(
        "Visualizers",
        (TestDesktop){.soundfont = "agents/reports/generated-sine.sf2", .paused = true});
    DmAudio *audio = e->audio;
    editor_smoke(e);
    e->project.patterns[0].count = 0;
    dm_note_add(&e->project.patterns[0], (DmNote){0, 16, 69, 100, 0});
    e->project.patterns[0].length = 16;
    e->pattern_mode = true;
    float stereo[DM_SPECTRUM_FRAMES * 2];
    check(!dm_audio_analyzer_read(audio, stereo), "analyzer starts disabled");
    dm_audio_analyzer_enable(audio, true);
    recording_feedback_audio_render(audio, 4096);
    check(!dm_audio_analyzer_read(audio, stereo), "paused output does not activate the analyzer");
    dm_audio_analyzer_enable(audio, false);
    test_visualizers_ui(e, check);
    visualizers_audio_panic_checks(audio, check);
    printf("PASS visualizers: %d checks\n", checks);
    test_editor_close(e);
}
