/* Shared by the C++ halves of the editor tests, which drive the real windows through ImGui:
   one frame runner with mouse input, typed input, a window lookup and a screen capture. */
#ifndef DUCKMIDI_TEST_UI_H
#define DUCKMIDI_TEST_UI_H
extern "C" {
#include "editor/internal.h"
#include "text/en.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cfloat>
#include <cmath>
#include <cstring>

typedef void (*Check)(bool, const char *);

/* Draws one editor frame at the window's size with the mouse at (x, y). */
static inline void frame(Editor *e, float x = -100, float y = -100, bool left = false,
                         bool right = false, bool alt = false, bool shift = false) {
    auto &io = ImGui::GetIO();
    int width, height;
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, left);
    io.AddMouseButtonEvent(1, right);
    io.AddKeyEvent(ImGuiMod_Alt, alt);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    SDL_GetWindowSize(e->window, &width, &height);
    ui_frame();
    editor_frame(e, (float)width, (float)height);
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
}

static inline void click(Editor *e, float x, float y, bool right = false, bool alt = false) {
    frame(e, x, y, false, false, alt);
    frame(e, x, y, !right, right, alt);
    frame(e, x, y, false, false, alt);
}

/* Presses and releases a key the way ImGui widgets see it. */
static inline void key(Editor *e, ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    frame(e);
    ImGui::GetIO().AddKeyEvent(key, false);
    frame(e);
}

/* Replaces the focused text field's contents. */
static inline void replace(Editor *e, const char *text) {
    auto &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    key(e, ImGuiKey_A);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame(e);
    io.AddInputCharactersUTF8(text);
    frame(e);
}

/* The active window whose name contains part, such as "/Playlist_" for a child window. */
static inline ImGuiWindow *find_window(const char *part) {
    for (auto *w : ImGui::GetCurrentContext()->Windows) {
        if (w->Active && std::strstr(w->Name, part)) {
            return w;
        }
    }

    return nullptr;
}

/* Saves the frame just drawn for review by eye; nothing reads the file back. */
static inline void capture(Editor *e, const char *path) {
    SDL_Surface *surface = SDL_RenderReadPixels(e->renderer, nullptr);

    if (surface) {
        SDL_SaveBMP(surface, path);
        SDL_DestroySurface(surface);
    }
}

#endif
