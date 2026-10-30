#include "editor_internal.h"
void test_editor_combine(Editor *e);
static int checks;
static void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL port UI: %s\n", label);
        exit(1);
    }
}
static size_t count_notes(Editor *e) {
    size_t n = 0;
    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        n += e->project.patterns[i].count;
    }
    return n;
}
static void draw(Editor *e, const char *capture) {
    ui_frame();
    editor_frame(e, 1400, 900);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    if (capture) {
        SDL_Surface *s = SDL_RenderReadPixels(e->renderer, NULL);
        check(s && SDL_SaveBMP(s, capture), "UI capture");
        SDL_DestroySurface(s);
    }
    SDL_RenderPresent(e->renderer);
}
int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Port import validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "actual UI");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", true);
    ed_load(e, "samples/stalks.mid");
    check(!e->config && e->project.port == 27 && count_notes(e) == 1484,
          "normal file-open exposes native port routing");
    e->config = true;
    draw(e, NULL);
    draw(e, NULL);
    test_editor_combine(e);
    check(e->project.port == DM_PORT_COMBINED && count_notes(e) == 4119 &&
              e->project.pattern_count == 7,
          "actual combine button imports complete arrangement");
    check(e->config && e->dirty && !e->confirm && !*e->path,
          "pristine import combines without a redundant save prompt");
    check(e->undo_count == 0 && e->project.view_count == 0,
          "combined mode starts a distinct source view history");
    draw(e, NULL);
    draw(e, "agents/reports/stalks-config.bmp");
    ed_load(e, "samples/stalks.mid");
    ed_checkpoint(e);
    e->project.patterns[0].notes[0].key = 50;
    ed_changed(e);
    e->config = true;
    draw(e, NULL);
    test_editor_combine(e);
    check(e->confirm && e->pending_action == PENDING_COMBINE && e->project.port == 27,
          "combining an edited import offers Save Discard Cancel first");
    e->confirm = false;
    e->pending_action = 0;
    check(e->project.patterns[0].notes[0].key == 50, "cancel retains edits");
    ed_load(e, "agents/reports/ports-collision.mid");
    size_t before = count_notes(e);
    ed_combine_source_ports(e);
    check(e->project.port != DM_PORT_COMBINED && count_notes(e) == before &&
              strstr(e->status, "Cannot combine ports"),
          "collision failure retains the old project and explains the conflict");
    printf("PASS port UI: %d checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
