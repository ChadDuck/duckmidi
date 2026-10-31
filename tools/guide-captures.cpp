/* Renders the editor states shown in docs/user-guide.md and saves each as a BMP under
   agents/reports/guide-captures/. tools/update-guide-images.ps1 builds this program, runs it and
   converts the captures into docs/images/. The sample project is only read. */
extern "C" {
#include "editor_internal.h"
#include "text/en.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cstdio>
#include <cstring>

static const char *output = "agents/reports/guide-captures";

static void frame(Editor *e, float x = 0, float y = 0, bool down = false, bool right = false,
                  bool shift = false) {
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down);
    io.AddMouseButtonEvent(1, right);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    ui_frame();
    editor_frame(e, 1400, 900);
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_RenderPresent(e->renderer);
}

static void settle(Editor *e) {
    for (int i = 0; i < 4; ++i) {
        frame(e);
    }
}

static ImGuiWindow *window(const char *part) {
    for (auto *w : ImGui::GetCurrentContext()->Windows) {
        if (w->Active && std::strstr(w->Name, part)) {
            return w;
        }
    }

    return nullptr;
}

/* Saves the area of the last frame, re-rendered so it is read before being presented. */
static void capture(Editor *e, const char *name, SDL_Rect area) {
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_Surface *s = SDL_RenderReadPixels(e->renderer, area.w ? &area : nullptr);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s.bmp", output, name);

    if (!s || !SDL_SaveBMP(s, path)) {
        std::fprintf(stderr, "Could not save %s: %s\n", path, SDL_GetError());
    }

    SDL_DestroySurface(s);
}

static SDL_Rect around(ImGuiWindow *w, int margin = 6) {
    if (!w) {
        return {0, 0, 0, 0};
    }

    int x = (int)w->Pos.x - margin, y = (int)w->Pos.y - margin;
    int right = (int)(w->Pos.x + w->Size.x) + margin, bottom = (int)(w->Pos.y + w->Size.y) + margin;
    x = x < 0 ? 0 : x;
    y = y < 0 ? 0 : y;
    right = right > 1400 ? 1400 : right;
    bottom = bottom > 900 ? 900 : bottom;
    return {x, y, right - x, bottom - y};
}

static int first_note_pattern(Editor *e) {
    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        if (!e->project.patterns[i].kind && e->project.patterns[i].count) {
            return (int)i;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    const char *sample = argc > 1 ? argv[1] : "assets/funky-beat.duck";
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        return 1;
    }

    SDL_CreateDirectory(output);
    SDL_Window *sdl_window = SDL_CreateWindow("Guide captures", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(sdl_window, "software");
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));

    if (!sdl_window || !renderer || !audio || !ui_init(sdl_window, renderer, nullptr)) {
        std::fprintf(stderr, "Could not start: %s\n", SDL_GetError());
        return 1;
    }

    auto *e = (Editor *)SDL_calloc(1, sizeof(Editor));
    editor_init(e, sdl_window, renderer, audio, "", "", true);
    e->update_prompt_pending = false;
    e->update_prompted = true;
    settle(e);
    capture(e, "welcome", {0, 0, 0, 0});

    ed_load(e, sample);

    if (!e->active) {
        std::fprintf(stderr, "Could not open %s: %s\n", sample, e->status);
        return 1;
    }

    e->status[0] = 0;
    e->update_prompt_pending = false;
    e->update_window = false;
    int pattern = first_note_pattern(e);
    ed_choose_pattern_card(e, pattern, false);
    e->roll = true;
    e->automation_open = false;
    settle(e);
    capture(e, "overview", {0, 0, 0, 0});
    capture(e, "patterns", around(window("/Patterns_"), 0));
    capture(e, "piano-roll", around(window("/Piano roll_"), 0));

    /* Events section: a modulation line and a held point. */
    int id = ed_curve_id(e);
    DmAutomation a = {.beat = 0,
                      .type = MIDI_CONTROL,
                      .channel = 0,
                      .a = MIDI_CC_MODULATION,
                      .b = 0,
                      .pattern = pattern,
                      .curve = id,
                      .anchor = 60};
    dm_automation_add(&e->project, a);
    a.beat = 1;
    a.b = 110;
    dm_automation_add(&e->project, a);
    a.beat = 1.5;
    a.b = 50;
    a.hold = true;
    dm_automation_add(&e->project, a);
    a.beat = 2.5;
    a.b = 90;
    a.hold = false;
    dm_automation_add(&e->project, a);
    e->automation_open = true;
    e->automation_cc = MIDI_CC_MODULATION;
    settle(e);
    capture(e, "events", around(window("/Piano roll_"), 0));
    e->automation_open = false;

    e->instrument_channel = 0;
    settle(e);
    capture(e, "instrument", around(window(DM_TEXT_INSTRUMENT), 0));
    e->instrument_channel = -1;

    ed_percussion_open(e);
    settle(e);
    capture(e, "percussion-mapping", around(window(DM_TEXT_PERCUSSION_KEYBOARD_MAPPING), 0));
    e->percussion_window = false;

    e->export_window = true;
    settle(e);
    capture(e, "export", around(window(DM_TEXT_EXPORT), 0));
    e->export_window = false;

    ed_update_open(e);
    settle(e);
    capture(e, "updates", around(window(DM_TEXT_UPDATES), 0));
    e->update_window = false;
    settle(e);

    std::printf("Saved guide captures to %s\n", output);
    editor_free(e);
    SDL_free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
    return 0;
}
