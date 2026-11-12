/* Exercise the actual application loop's platform boundary without a visible window. */
#define SDL_MAIN_HANDLED
#define main duckmidi_application_main
#include "../src/main.c"
#undef main

#define TEST_NAME "application window"
#include "test_common.h"

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL video");
    SDL_Window *window =
        SDL_CreateWindow("Application window", 1400, 900, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "direct3d11");
    check(window && renderer && ui_init(window, renderer, NULL), "actual hidden UI");
    Editor *editor = calloc(1, sizeof(*editor));
    check(editor != NULL, "editor allocation");
    editor_init(editor, window, renderer, NULL, "", "", true);
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    SDL_Rect bounds;
    check(display && SDL_GetDisplayUsableBounds(display, &bounds), "display detection");
    editor->display = 0; /* A missing/replaced previous display triggers the real reset path. */
    monitor_window(editor, window);
    SDL_SyncWindow(window);
    int width = 0, height = 0, x = 0, y = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_GetWindowPosition(window, &x, &y);
    check(width == bounds.w / 2 && height == bounds.h / 2, "half-monitor default size");
    check(abs(x - (bounds.x + (bounds.w - width) / 2)) <= 1 &&
              abs(y - (bounds.y + (bounds.h - height) / 2)) <= 1,
          "centered default position");
    uint64_t start = SDL_GetTicks();
    check(window_frame(editor, window, false), "unfocused window keeps rendering");
    check(SDL_GetTicks() - start < 75, "unfocused window is not throttled");
    check(window_frame(editor, window, true), "isolated desktop capture mode remains runnable");
    editor->dialog_busy = true;
    start = SDL_GetTicks();
    /* During a native picker the editor keeps drawing at about 30 fps, and checks messages every
       millisecond in between. */
    check(window_frame(editor, window, false), "native picker keeps the editor drawing");
    check(!window_frame(editor, window, false), "native picker limits redraws to ~30 fps");
    check(SDL_GetTicks() - start < 75, "native picker bypasses the 100ms minimized throttle");
    editor->dialog_busy = false;
    printf(
        "PASS application window: detected display %u; centered %dx%d; unfocused rendering kept\n",
        display, width, height);
    editor_free(editor);
    free(editor);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
