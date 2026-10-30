#include "editor_internal.h"
static void check(bool ok, const char *label) {
    if (!ok) {
        fprintf(stderr, "FAIL inspector: %s\n", label);
        exit(1);
    }
}
int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Inspector", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", true);
    editor_smoke(e);
    uint8_t bytes[] = {0xb2, 7, 120};
    DmBytes message = {bytes, 3, 3};
    for (int i = 0; i < 50000; ++i) {
        check(dm_event_add(&e->project, i * .25, 0, 0, &message), "source fixture");
        check(dm_automation_add(
                  &e->project,
                  (DmAutomation){.beat = i * .25, .pattern = 0, .type = 0xb0, .a = 1, .b = 64}),
              "automation fixture");
    }
    uint64_t begin = SDL_GetPerformanceCounter();
    for (int i = 0; i < 25; ++i) {
        check(ed_midi_rows(e) == 100000, "all events retained");
    }
    double elapsed =
        1000.0 * (SDL_GetPerformanceCounter() - begin) / SDL_GetPerformanceFrequency() / 25;
    MidiRow *rows = (MidiRow *)e->midi_rows.data;
    for (int i = 0; i < 50000; ++i) {
        check(!rows[i * 2].automation && rows[i * 2].index == i && rows[i * 2].beat == i * .25,
              "stable source order at equal beats");
        check(rows[i * 2 + 1].automation && rows[i * 2 + 1].index == i &&
                  rows[i * 2 + 1].beat == i * .25,
              "stable automation order at equal beats");
    }
    printf("PASS inspector: 100000 sorted rows; average rebuild %.3f ms (25 runs)\n", elapsed);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
