#define main duckmidi_app_main
#include "../src/main.c"
#include "editor_internal.h"
#undef main
#include <windows.h>
#include <assert.h>
static HWND found;

static BOOL CALLBACK picker_window(HWND window, LPARAM parameter) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    wchar_t kind[64];
    GetClassNameW(window, kind, 64);

    if (process == (DWORD)parameter && IsWindowVisible(window) && !wcscmp(kind, L"#32770")) {
        found = window;
    }

    return TRUE;
}

int main(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 1;
    }

    SDL_Window *window =
        SDL_CreateWindow("duckmidi isolated picker benchmark", 640, 400, SDL_WINDOW_HIDDEN);
    Editor *e = calloc(1, sizeof(*e));
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "direct3d11");

    if (!window || !renderer || !ui_init(window, renderer, NULL)) {
        return 1;
    }

    editor_init(e, window, renderer, NULL, "", true);
    char *directory = SDL_GetCurrentDirectory();
    snprintf(e->midi_folder, DM_PATH, "%ssamples/", directory);
    snprintf(e->font_folder, DM_PATH, "%sagents/reports/", directory);
    SDL_free(directory);
    const int kinds[] = {DIALOG_OPEN, DIALOG_FONT, DIALOG_MIDI, DIALOG_EXPORT_PLUGIN};
    const char *names[] = {"open", "soundfont", "save", "plugin-save"};
    assert(ed_export_count(e) > 0);
    e->export_type = 1;

    for (int attempt = 0; attempt < 8; ++attempt) {
        found = NULL;
        Uint64 start = SDL_GetTicks(), shown = 0, closed = 0;
        ed_dialog(e, kinds[attempt % 4]);
        Uint64 returned = SDL_GetTicks();

        while (e->dialog_busy && SDL_GetTicks() - start < 15000) {
            SDL_Event event;
            bool events = false;

            while (SDL_PollEvent(&event)) {
                events = true;
                editor_event(e, &event);
            }

            EnumWindows(picker_window, (LPARAM)GetCurrentProcessId());

            if (found && !shown) {
                shown = SDL_GetTicks();
            }

            if (found && shown && !closed && SDL_GetTicks() - shown > 250) {
                closed = SDL_GetTicks();
                PostMessageW(found, WM_COMMAND, IDCANCEL, 0);
            }

            window_frame(e, window, false, events);
        }

        printf("picker=%s API_return_ms=%llu first_visible_ms=%llu cancel_callback_ms=%llu "
               "total_ms=%llu completed=%d\n",
               names[attempt % 4], (unsigned long long)(returned - start),
               (unsigned long long)(shown ? shown - start : 0),
               (unsigned long long)(closed ? SDL_GetTicks() - closed : 0),
               (unsigned long long)(SDL_GetTicks() - start), !e->dialog_busy);
        fflush(stdout);

        if (e->dialog_busy) {
            return 2;
        }
    }

    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
