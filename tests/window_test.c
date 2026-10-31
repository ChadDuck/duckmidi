/* Exercise the actual application loop's platform boundary without a visible window. */
#define main duckmidi_application_main
#include "../src/main.c"
#undef main

static void verify(bool ok, const char *ed_message) {
    if (!ok) {
        fprintf(stderr, "FAIL window: %s\n", ed_message);
        exit(1);
    }
}

int main(void) {
    verify(SDL_Init(SDL_INIT_VIDEO), "SDL video");
    SDL_Window *window =
        SDL_CreateWindow("Window validation", 1400, 900, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "direct3d11");
    verify(window && renderer && ui_init(window, renderer, NULL), "actual hidden UI");
    Editor *editor = calloc(1, sizeof(*editor));
    verify(editor != NULL, "editor allocation");
    editor_init(editor, window, renderer, NULL, "", true);
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    SDL_Rect bounds;
    verify(display && SDL_GetDisplayUsableBounds(display, &bounds), "display detection");
    editor->display = 0; /* A missing/replaced previous display triggers the real reset path. */
    monitor_window(editor, window);
    SDL_SyncWindow(window);
    int width = 0, height = 0, x = 0, y = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_GetWindowPosition(window, &x, &y);
    verify(width == bounds.w / 2 && height == bounds.h / 2, "half-monitor default size");
    verify(abs(x - (bounds.x + (bounds.w - width) / 2)) <= 1 &&
               abs(y - (bounds.y + (bounds.h - height) / 2)) <= 1,
           "centered default position");
    uint64_t start = SDL_GetTicks();
    verify(!window_frame(editor, window, false, false), "unfocused idle loop skips rendering");
    verify(SDL_GetTicks() - start >= 90, "idle loop is throttled");
    verify(window_frame(editor, window, false, true),
           "incoming events can refresh an unfocused window");
    verify(window_frame(editor, window, true, false),
           "isolated desktop capture mode remains runnable");
    editor->dialog_busy = true;
    start = SDL_GetTicks();
    /* During a native picker the editor keeps drawing at about 30 fps, and checks messages every
       millisecond in between. */
    verify(window_frame(editor, window, false, false), "native picker keeps the editor drawing");
    verify(!window_frame(editor, window, false, false), "native picker limits redraws to ~30 fps");
    verify(SDL_GetTicks() - start < 75, "native picker bypasses the 100ms idle throttle");
    editor->dialog_busy = false;
    printf("PASS window: detected display %u; centered %dx%d; idle rendering skipped\n", display,
           width, height);
    editor_free(editor);
    free(editor);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
