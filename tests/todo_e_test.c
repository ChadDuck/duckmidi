/* Checks for the 2026-11-09 backlog: selection clicks in the playlist, piano roll and event
   lanes, the Song events window opening at its fitted size, and event lane drawing. */
#include "editor/internal.h"
#include <assert.h>
extern void recording_feedback_audio_pause(DmAudio *);
extern void test_todo_e_ui(Editor *, void (*)(bool, const char *));

static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL todo e: %s\n", text);
        exit(1);
    }
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO));
    SDL_Window *window = SDL_CreateWindow("Todo E checks", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    assert(window && renderer && ui_init(window, renderer, NULL));
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    assert(audio);
    recording_feedback_audio_pause(audio);
    Editor *e = calloc(1, sizeof(*e));
    assert(e);
    editor_init(e, window, renderer, audio, "", "", true);
    ed_new_project(e);
    e->selected_pattern = 0;
    e->selected_channel = e->pattern_channel = 0;
    e->selected_clip = e->playlist_context_clip = -1;
    test_todo_e_ui(e, check);
    printf("PASS todo e: %d checks\n", checks);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
