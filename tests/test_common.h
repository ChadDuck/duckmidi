/* Shared by the regression tests: one counted check and, for tests of the whole editor, the hidden
   window, the editor and the audio-job wait they all set up. Define TEST_NAME, the label printed
   with a failure, before including this file; include "editor/internal.h" first to get the editor
   helpers. */
#ifndef DUCKMIDI_TEST_COMMON_H
#define DUCKMIDI_TEST_COMMON_H
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef TEST_NAME
#error "Define TEST_NAME before including test_common.h"
#endif

static int checks;

static inline void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL " TEST_NAME ": %s\n", text);
        exit(1);
    }
}

#ifdef DM_EDITOR_INTERNAL_H
#define TEST_SOUNDFONT "agents/reports/generated-programs.sf2"

/* Stops the audio device so a test renders by hand; defined by tests/audio_probe.c. */
extern void recording_feedback_audio_pause(DmAudio *);

typedef struct {
    const char *renderer;  /* SDL renderer name; NULL is "direct3d11" */
    const char *driver;    /* SDL audio driver hint, or NULL for the default device */
    const char *soundfont; /* loaded before the editor starts; implies audio */
    bool audio;            /* create the audio engine */
    bool paused;           /* pause the device once the engine exists (needs the audio probe) */
    int width, height;     /* window size; 0 is 1400 x 900 */
} TestDesktop;

/* Waits up to 30 s for the audio worker and returns the result of the job it finished. */
static inline bool test_audio_wait(DmAudio *audio) {
    bool result = true;
    char message[512] = {0};
    uint64_t start = SDL_GetTicks();

    while (dm_audio_busy(audio) && SDL_GetTicks() - start < 30000) {
        bool success = false;
        int kind = 0;

        if (dm_audio_poll(audio, message, sizeof(message), &success, &kind) && kind) {
            result = success;

            if (!success) {
                fprintf(stderr, "Audio job: %s\n", message);
            }
        }

        SDL_Delay(1);
    }

    check(!dm_audio_busy(audio), "audio job finishes");
    return result;
}

/* A hidden window with the UI, an optional audio engine and a started editor. */
static inline Editor *test_editor_open(const char *title, TestDesktop setup) {
    bool audio = setup.audio || setup.soundfont;
    DmAudio *engine = NULL;
    char error[512] = {0};

    if (setup.driver) {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, setup.driver);
    }

    check(SDL_Init(SDL_INIT_VIDEO | (audio ? SDL_INIT_AUDIO : 0)), "SDL starts");
    SDL_Window *window = SDL_CreateWindow(title, setup.width ? setup.width : 1400,
                                          setup.height ? setup.height : 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer =
        SDL_CreateRenderer(window, setup.renderer ? setup.renderer : "direct3d11");
    check(window && renderer && ui_init(window, renderer, NULL), "hidden window and UI");

    if (audio) {
        engine = dm_audio_create(error, sizeof(error));
        check(engine != NULL, "audio engine");

        if (setup.paused) {
            recording_feedback_audio_pause(engine);
        }
    }

    if (setup.soundfont) {
        check(dm_audio_soundfont(engine, setup.soundfont, error, sizeof(error)) &&
                  test_audio_wait(engine),
              "fixture SoundFont loads");
    }

    Editor *e = calloc(1, sizeof(*e));
    check(e != NULL, "editor allocation");
    editor_init(e, window, renderer, engine, "", "", true);
    return e;
}

static inline void test_editor_close(Editor *e) {
    SDL_Window *window = e->window;
    SDL_Renderer *renderer = e->renderer;
    DmAudio *audio = e->audio;
    editor_free(e);
    free(e);

    if (audio) {
        dm_audio_destroy(audio);
    }

    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
#endif

#endif
