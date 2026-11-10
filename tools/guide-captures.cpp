/* Renders the editor states shown in docs/user-guide.md and saves each as a BMP under
   agents/reports/guide-captures/. tools/update-guide-images.ps1 builds this program, runs it and
   converts the captures into docs/images/. The sample project is only read. */
extern "C" {
#include "editor/export_internal.h"
#include "text/en.h"
#include "../include/duckmidi_samples_tool.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static const char *output = "agents/reports/guide-captures";
/* Settings and the sample library live beside the captures, never in the user's preferences. */
static const char *data = "agents/reports/guide-captures/data/";

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

/* A decaying plucked tone as 16-bit mono WAV, so the Samples waveform shows a clear shape. */
static void write_pluck(const char *path, double frequency) {
    const uint32_t rate = 32000, frames = rate * 6 / 5, bytes = frames * 2;
    unsigned char header[44] = {'R', 'I', 'F', 'F', 0,  0, 0, 0, 'W', 'A', 'V', 'E',
                                'f', 'm', 't', ' ', 16, 0, 0, 0, 1,   0,   1,   0};
    auto put = [&](int at, uint32_t value, int size) {
        for (int i = 0; i < size; ++i) {
            header[at + i] = (unsigned char)(value >> (8 * i));
        }
    };
    put(4, 36 + bytes, 4);
    put(24, rate, 4);
    put(28, rate * 2, 4);
    put(32, 2, 2);
    put(34, 16, 2);
    std::memcpy(header + 36, "data", 4);
    put(40, bytes, 4);
    SDL_IOStream *file = SDL_IOFromFile(path, "wb");

    if (!file) {
        return;
    }

    SDL_WriteIO(file, header, sizeof(header));

    for (uint32_t i = 0; i < frames; ++i) {
        double t = (double)i / rate, phase = 6.283185307179586 * frequency * t;
        double value = std::exp(-3.5 * t) * (.6 * std::sin(phase) + .25 * std::sin(2 * phase) +
                                             .1 * std::sin(3 * phase));
        int16_t sample = (int16_t)(value * 32000);
        SDL_WriteIO(file, &sample, 2);
    }

    SDL_CloseIO(file);
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
    SDL_CreateDirectory(data);
    SDL_Window *sdl_window = SDL_CreateWindow("Guide captures", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(sdl_window, "software");
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));

    if (!sdl_window || !renderer || !audio || !ui_init(sdl_window, renderer, nullptr)) {
        std::fprintf(stderr, "Could not start: %s\n", SDL_GetError());
        return 1;
    }

    auto *e = (Editor *)SDL_calloc(1, sizeof(Editor));
    editor_init(e, sdl_window, renderer, audio, "", data, true);
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

    ed_open_instrument(e, -1, false);
    settle(e);
    capture(e, "instrument", around(window(DM_TEXT_INSTRUMENT), 0));
    ed_close_instrument(e);

    ed_percussion_open(e);
    settle(e);
    capture(e, "percussion-mapping", around(window(DM_TEXT_PERCUSSION_KEYBOARD_MAPPING), 0));
    e->percussion_window = false;

    e->export_window = true;
    settle(e);
    capture(e, "export", around(window(DM_TEXT_EXPORT), 0));

    /* The M64 plugin's options and its channel/instrument mapping list. */
    DmExportRegistry *registry = &ed_export_host(e)->registry;

    for (size_t i = 0; i < registry->count; ++i) {
        if (!std::strcmp(registry->formats[i].api->id, "org.duckmidi.sm64")) {
            e->export_type = (int)i + 1;
        }
    }

    for (int i = 0; i < 60; ++i) {
        frame(e);
        SDL_Delay(10);
    }

    capture(e, "export-m64", around(window(DM_TEXT_EXPORT), 0));
    e->export_window = false;
    settle(e);

    /* Samples with two imported files, the first selected. */
    ed_samples_open(e);
    settle(e);
    DmNativeTool *tool = dm_export_registry_tool(registry, DME_SAMPLES_TOOL_ID);
    ImGuiWindow *samples = window("Samples");

    if (tool && samples) {
        auto *api = static_cast<const DmeSamplesApi *>(tool->api->interface_at(DME_SAMPLES_API));
        const char *files[] = {"agents/reports/guide-captures/data/Pluck C4.wav",
                               "agents/reports/guide-captures/data/Pluck G4.wav"};
        write_pluck(files[0], 261.63);
        write_pluck(files[1], 392.0);
        float x = (samples->Pos.x + samples->Size.x / 2) * ui_scale();
        float y = (samples->Pos.y + samples->Size.y / 2) * ui_scale();
        SDL_Event event = {};
        event.drop.x = x;
        event.drop.y = y;
        event.type = SDL_EVENT_DROP_BEGIN;
        editor_event(e, &event);
        event.type = SDL_EVENT_DROP_POSITION;
        editor_event(e, &event);

        for (const char *file : files) {
            event.type = SDL_EVENT_DROP_FILE;
            event.drop.data = file;
            editor_event(e, &event);
        }

        event.type = SDL_EVENT_DROP_COMPLETE;
        event.drop.data = nullptr;
        editor_event(e, &event);
        uint64_t end = SDL_GetTicks() + 8000;

        while (api->count(tool->instance) < 2 && SDL_GetTicks() < end) {
            frame(e);
            SDL_Delay(2);
        }

        settle(e);
        capture(e, "samples", around(window("Samples"), 0));
    } else {
        std::fprintf(stderr, "Samples is unavailable; build the plugins first\n");
    }

    ed_samples_close(e);
    settle(e);

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
