#include "editor_internal.h"

extern void test_samples_ui(Editor *, const char *, void (*)(bool, const char *));

static void check(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL Samples editor: %s\n", message);
        exit(1);
    }

    printf("PASS %s\n", message);
    fflush(stdout);
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi");
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), "initialize SDL");
    SDL_Window *window =
        SDL_CreateWindow("Samples fixture", 1400, 900, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, "direct3d11") : NULL;
    check(renderer && ui_init(window, renderer, NULL), "create isolated renderer");
    SDL_Time now;
    check(SDL_GetCurrentTime(&now), "timestamp for fixture directories");
    char root[DM_PATH], error[512];
    snprintf(root, sizeof(root), "agents/reports/samples-editor-%llu/", (unsigned long long)now);
    check(SDL_CreateDirectory(root), "create isolated test data folder");
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio != NULL, "create audition audio");
    check(dm_audio_soundfont(audio, "agents/reports/generated-programs.sf2", error, sizeof(error)),
          "load generated audition SoundFont");
    uint64_t timeout = SDL_GetTicks() + 10000;

    while (dm_audio_busy(audio) && SDL_GetTicks() < timeout) {
        bool success = false;

        if (dm_audio_poll(audio, error, sizeof(error), &success, NULL)) {
            check(success, "generated SoundFont completes");
        }

        SDL_Delay(2);
    }

    check(!dm_audio_busy(audio), "SoundFont worker finishes");
    Editor *editor = calloc(1, sizeof(*editor));
    check(editor != NULL, "allocate editor");
    editor_init(editor, window, renderer, audio, "agents/reports/", root, true);
    editor_smoke(editor);
    test_samples_ui(editor, root, check);
    editor_free(editor);
    free(editor);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    puts("PASS Samples editor import, isolated audition, publish and routed assignment");
}
