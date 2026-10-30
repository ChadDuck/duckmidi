#include "editor.h"
#include "text/en.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void application_icon(SDL_Window *window) {
    HWND handle=(HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    HICON large=LoadIconW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(1));
    if(handle&&large){SendMessageW(handle,WM_SETICON,ICON_BIG,(LPARAM)large);SendMessageW(handle,WM_SETICON,ICON_SMALL,(LPARAM)large);}
}
#endif

static SDL_Renderer *accelerated_renderer(SDL_Window *window) {
#ifdef _WIN32
    const char *backends[] = {"direct3d11", "direct3d12", "opengl"};
#elif defined(__APPLE__)
    const char *backends[] = {"metal", "opengl"};
#else
    const char *backends[] = {"opengl", "opengles2", "vulkan"};
#endif
    for (size_t i = 0; i < sizeof(backends) / sizeof(*backends); ++i) {
        SDL_Renderer *r = SDL_CreateRenderer(window, backends[i]);
        if (r) {
            return r;
        }
    }
    return NULL;
}
static void monitor_window(Editor *editor, SDL_Window *window) {
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (!display) {
        display = SDL_GetPrimaryDisplay();
    }
    if (!display || display == editor->display) {
        return;
    }
    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(display, &bounds)) {
        int width = bounds.w / 2, height = bounds.h / 2;
        SDL_RestoreWindow(window);
        SDL_SetWindowMinimumSize(window, width < 640 ? width : 640, height < 400 ? height : 400);
        SDL_SetWindowSize(window, width, height);
        SDL_SetWindowPosition(window, bounds.x + (bounds.w - width) / 2,
                              bounds.y + (bounds.h - height) / 2);
    }
    editor->display = display;
}
static bool window_frame(Editor *editor, SDL_Window *window, bool smoke, bool events) {
    monitor_window(editor, window);
    SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    editor->minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
    editor->focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
    // Native dialogs can send synchronous messages to their owner while opening and closing.
    // Keep pumping that owner's message queue promptly without rendering the covered editor.
    if (editor->dialog_busy) {
        editor_background(editor);
        SDL_Delay(1);
        return false;
    }
    if (!smoke && editor->minimized) {
        editor_background(editor);
        if (editor->confirm) {
            SDL_RestoreWindow(window);
        }
        SDL_Delay(100);
        return false;
    }
    if (!smoke && !editor->focused) {
        editor_background(editor);
        SDL_Delay(100);
        if (!events && !editor->drop_active && !dm_audio_playing(editor->audio) &&
            !dm_audio_busy(editor->audio)) {
            return false;
        }
    }
    return true;
}
static void editor_loop(Editor *editor, SDL_Window *window, SDL_Renderer *renderer, bool smoke) {
    while (editor->running) {
        SDL_Event event;
        bool events = false;
        while (SDL_PollEvent(&event)) {
            events = true;
            ui_event(&event);
            editor_event(editor, &event);
        }
        if (!window_frame(editor, window, smoke, events)) {
            continue;
        }
        const float scales[] = {.85f, 1, 1.25f};
        ui_set_scale(scales[editor->ui_size]);
        ui_frame();
        int width = 0, height = 0;
        SDL_GetWindowSize(window, &width, &height);
        editor_frame(editor, (float)width, (float)height);
        SDL_SetRenderDrawColor(renderer, 10, 10, 10, 255);
        SDL_RenderClear(renderer);
        ui_render(renderer);
        if (smoke && ++editor->smoke_frames == 30) {
            SDL_Surface *surface = SDL_RenderReadPixels(renderer, NULL);
            if (surface) {
                SDL_SaveBMP(surface, editor->active ? "agents/reports/editor-smoke.bmp"
                                                    : "agents/reports/welcome-smoke.bmp");
                SDL_DestroySurface(surface);
            }
            printf(DM_TEXT_RENDERER_S_AUDIO_S_PATTERNS_ZU_CLIPS_ZU_N, SDL_GetRendererName(renderer),
                   dm_audio_driver(editor->audio), editor->project.pattern_count,
                   editor->project.clip_count);
            editor->running = false;
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(1);
    }
}
static bool editor_allocation(Editor *editor, DmAudio *audio, SDL_Renderer *renderer,
                              SDL_Window *window) {
    if (!editor) {
        dm_audio_destroy(audio);
        ui_shutdown();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    return true;
}
static int launch_mode(int argc, char **argv) {
    if (argc < 2) {
        return 0;
    }
    if (!strcmp(argv[1], "--welcome-smoke")) {
        return 2;
    }
    return !strcmp(argv[1], "--smoke");
}
int main(int argc, char **argv) {
    int mode = launch_mode(argc, argv);
    bool welcome = mode == 2, smoke = mode != 0;
#ifdef _WIN32
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "wasapi", SDL_HINT_OVERRIDE);
#endif
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, DM_TEXT_SDL_VIDEO_S_N, SDL_GetError());
        return 1;
    }
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_Window *window = SDL_CreateWindow(DM_TEXT_APP_NAME, 1400, 900,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                              (smoke ? SDL_WINDOW_HIDDEN : 0));
    if (!window) {
        fprintf(stderr, DM_TEXT_WINDOW_S_N, SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 640, 400);
#ifdef _WIN32
    application_icon(window);
#endif
    SDL_Renderer *renderer = accelerated_renderer(window);
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DM_TEXT_APP_NAME,
                                 DM_TEXT_NO_HARDWARE_ACCELERATED_GRAPHICS_BACKEND_IS_AVAILABLE,
                                 window);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    const char *base = SDL_GetBasePath();
    char ini[DM_PATH];
    snprintf(ini, sizeof(ini), DM_TEXT_SDUCKMIDI_INI, base ? base : "");
    if (!ui_init(window, renderer, smoke ? NULL : ini)) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    char error[512] = {0};
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    Editor *editor = calloc(1, sizeof(*editor));
    if (!editor_allocation(editor, audio, renderer, window)) {
        return 1;
    }
    editor_init(editor, window, renderer, audio, base ? base : "", smoke);
    if (*error) {
        snprintf(editor->status, sizeof(editor->status), "%s", error);
    }
    if (smoke && !welcome) {
        editor_smoke(editor);
    }
    editor_loop(editor, window, renderer, smoke);
    DmAudio *final_audio = editor->audio;
    editor_free(editor);
    free(editor);
    dm_audio_destroy(final_audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
