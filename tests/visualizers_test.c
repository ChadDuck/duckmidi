#include "editor_internal.h"

extern void recording_feedback_audio_pause(DmAudio *);
extern void recording_feedback_audio_render(DmAudio *, int);
extern void test_visualizers_ui(Editor *, void (*)(bool, const char *));
extern void visualizers_audio_panic_checks(DmAudio *, void (*)(bool, const char *));
static int checks;
static void check(bool ok, const char *label) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL visualizers: %s\n", label); exit(1); }
}
int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Visualizers", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "real renderer");
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio && dm_audio_soundfont(audio, "agents/reports/generated-sine.sf2", error, sizeof(error)), "generated SoundFont");
    uint64_t start = SDL_GetTicks();
    while (dm_audio_busy(audio)) {
        bool success = false;
        if (dm_audio_poll(audio, error, sizeof(error), &success, NULL)) check(success, error);
        check(SDL_GetTicks() - start < 30000, "font timeout");
        SDL_Delay(5);
    }
    recording_feedback_audio_pause(audio);
    Editor *e = calloc(1, sizeof(*e)); editor_init(e, w, r, audio, "", true); editor_smoke(e);
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
    editor_free(e); free(e); dm_audio_destroy(audio); ui_shutdown();
    SDL_DestroyRenderer(r); SDL_DestroyWindow(w); SDL_Quit();
}
