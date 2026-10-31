extern "C" {
#include "editor.h"
}
#include "imgui.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_internal.h"
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace toolbar_geometry {
#include "toolbar_icons.h"
}

extern "C" void test_event_controls_ui(Editor *, void (*)(bool, const char *));

static ImGuiWindow *roll_window() {
    auto &windows = ImGui::GetCurrentContext()->Windows;

    for (auto *window : windows) {
        if (window->Active && std::strstr(window->Name, "/Piano roll_")) {
            return window;
        }
    }

    return nullptr;
}

static UiRect pattern_event_lane() {
    float x = FLT_MAX, y = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;

    for (const auto &v : roll_window()->DrawList->VtxBuffer) {
        if (v.col == 0xff121212u) {
            x = ImMin(x, v.pos.x);
            y = ImMin(y, v.pos.y);
            right = ImMax(right, v.pos.x);
            bottom = ImMax(bottom, v.pos.y);
        }
    }

    return {x, y, right - x, bottom - y};
}

static void mouse_frame(Editor *e, float x, float y, bool down, bool alt = false,
                        bool shift = false, bool ctrl = false, float wheel = 0,
                        bool right = false) {
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down);
    io.AddMouseButtonEvent(1, right);
    io.AddMouseWheelEvent(0, wheel);
    io.AddKeyEvent(ImGuiMod_Alt, alt);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    ui_frame();
    editor_frame(e, 1400, 900);
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_RenderPresent(e->renderer);
}

extern "C" void test_editor_mouse(Editor *e, void (*check)(bool, const char *)) {
    auto *window = roll_window();
    check(window != nullptr, "piano roll window is rendered");
    float gx = window->Pos.x + window->WindowPadding.x + 64;
    float gy = window->Pos.y + window->WindowPadding.y + ImGui::GetFrameHeight() +
               ImGui::GetStyle().ItemSpacing.y + 28;
    float y = gy + (70 - 68) * 20 + 10, x = gx + 4 * 80 + 3;
    size_t count = e->project.patterns[0].count;
    e->tool = 0;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y, false);
    check(e->project.patterns[0].count == count + 1, "mouse inserts a note");
    size_t index = e->project.patterns[0].count - 1;
    float edge = gx + 4 * 80 + 19;
    mouse_frame(e, edge, y, true);
    mouse_frame(e, edge + 40, y, true);
    mouse_frame(e, edge + 40, y, false);
    check(e->project.patterns[0].notes[index].length >= .7, "mouse resizes note edge");
    e->tool = 2;
    mouse_frame(e, gx + 4 * 80 + 21, y, true);
    mouse_frame(e, gx + 4 * 80 + 21, y, false);
    check(e->project.patterns[0].count == count + 2, "slice tool splits a note");
    e->tool = 1;
    mouse_frame(e, gx + 4 * 80 - 4, y - 14, true);
    mouse_frame(e, gx + 4 * 80 + 65, y + 14, true);
    mouse_frame(e, gx + 4 * 80 + 65, y + 14, false);
    int selected = 0;

    for (size_t i = 0; i < e->project.patterns[0].count; ++i) {
        selected += e->project.patterns[0].notes[i].selected != 0;
    }

    check(selected >= 2, "marquee selects intersecting notes");
    SDL_Event remove;
    SDL_zero(remove);
    remove.type = SDL_EVENT_KEY_DOWN;
    remove.key.down = true;
    remove.key.scancode = SDL_SCANCODE_DELETE;
    editor_event(e, &remove);
    e->tool = 0;
    float bend_x = gx + .5f * 80 + 15, bend_y = gy + (70 - 64) * 20 + 10;
    mouse_frame(e, bend_x, bend_y, false, false, true);
    mouse_frame(e, bend_x, bend_y, true, false, true);
    mouse_frame(e, bend_x + 80, bend_y - 40, true, false, true);
    mouse_frame(e, bend_x + 80, bend_y - 40, false, false);
    check(e->project.automation_count > 0, "Shift drag creates pitch automation");
    e->automation_open = false;
}

static ImGuiWindow *find_window(const char *part) {
    for (auto *w : ImGui::GetCurrentContext()->Windows) {
        if (w->Active && std::strstr(w->Name, part)) {
            return w;
        }
    }

    return nullptr;
}

static void direct_key(Editor *e, SDL_Scancode key, SDL_Keymod mods = SDL_KMOD_NONE) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.down = true;
    event.key.scancode = key;
    event.key.mod = mods;
    editor_event(e, &event);
}

static UiPoint roll_origin() {
    auto *w = roll_window();
    return {w->Pos.x + w->WindowPadding.x + 64,
            w->Pos.y + w->WindowPadding.y + 32 + ImGui::GetStyle().ItemSpacing.y + 28};
}

static void group_drag(Editor *e, void (*check)(bool, const char *)) {
    auto &p = e->project.patterns[0];
    p.count = 0;
    p.length = 4;
    e->project.automation_count = 0;
    e->selected_pattern = 0;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->tool = 0;
    e->roll_view.scroll_x = 0;
    e->roll_view.scroll_y = (127 - 70) * 20;
    e->roll_view.zoom = 1;
    dm_note_add(&p, {0, .5, 64, 100, 1});
    dm_note_add(&p, {1, .5, 67, 100, 1});
    mouse_frame(e, 0, 0, false);
    auto grid = roll_origin();
    float x = grid.x + 20, y = grid.y + (70 - 64) * 20 + 10;
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 80, y - 20, true);
    mouse_frame(e, x + 80, y - 20, false);
    check(std::fabs(p.notes[0].beat - 1) < 1e-6 && std::fabs(p.notes[1].beat - 2) < 1e-6,
          "selected notes move as one group in time");
    check(p.notes[0].key == 65 && p.notes[1].key == 68,
          "selected notes preserve pitch spacing while dragging");
    e->cursor_beat[1] = 3.25;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    size_t before = p.count;
    direct_key(e, SDL_SCANCODE_C, SDL_KMOD_CTRL);
    direct_key(e, SDL_SCANCODE_V, SDL_KMOD_CTRL);
    check(p.count == before + 2 && std::fabs(p.notes[before].beat - 3.25) < 1e-6,
          "note group pastes at the piano playhead");
    check(std::fabs(p.notes[before + 1].beat - p.notes[before].beat - 1) < 1e-6,
          "pasted note group keeps relative timing");
}

static void quantized_alignment(Editor *e, void (*check)(bool, const char *)) {
    auto &ed_pattern = e->project.patterns[0];
    ed_pattern.count = 0;
    ed_pattern.length = 4;
    e->project.automation_count = 0;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->automation_open = false;
    dm_note_add(&ed_pattern, {.37, .5, 64, 100, 1});
    direct_key(e, SDL_SCANCODE_Q, SDL_KMOD_CTRL);
    mouse_frame(e, 0, 0, false);
    auto grid = roll_origin();
    float expected = grid.x + 20;
    bool notch = false, note = false;

    for (auto &v : roll_window()->DrawList->VtxBuffer) {
        if (v.col == 0xff262626u && std::fabs(v.pos.x - expected) < 1.1f && v.pos.y < grid.y - 1 &&
            v.pos.y > grid.y - 5) {
            notch = true;
        }

        if (v.col == e->accent && std::fabs(v.pos.x - expected) < 1.1f && v.pos.y > grid.y) {
            note = true;
        }
    }

    check(std::fabs(ed_pattern.notes[0].beat - .25) < 1e-6 && notch && note,
          "quantized note edges align with the quarter-beat ruler notches");
}

static float underlay_min_y(Editor *e) {
    float minimum = FLT_MAX;
    auto *w = roll_window();
    ImU32 color = (e->accent & 0xffffffu) | 0x88000000u;

    for (auto &vertex : w->DrawList->VtxBuffer) {
        if (vertex.col == color) {
            minimum = ImMin(minimum, vertex.pos.y - roll_origin().y);
        }
    }

    return minimum;
}

static void curve_edits(Editor *e, void (*check)(bool, const char *)) {
    auto &p = e->project.patterns[0];
    p.count = 0;
    p.length = 4;
    e->project.automation_count = 0;
    e->roll_view.scroll_y = (127 - 70) * 20;
    e->roll_height = 320;
    e->automation_open = false;
    dm_note_add(&p, {0, 2, 64, 100, 0});
    mouse_frame(e, 0, 0, false);
    auto grid = roll_origin();
    float x = grid.x + 20, y = grid.y + (70 - 64) * 20 + 10;
    mouse_frame(e, x, y, false, false, true);
    mouse_frame(e, x, y, true, false, true);
    mouse_frame(e, x + 80, y - 40, true, false, true);
    mouse_frame(e, x + 80, y - 40, false);
    e->automation_open = false;
    mouse_frame(e, 0, 0, false);
    float before = underlay_min_y(e);
    e->roll_height = 450;
    mouse_frame(e, 0, 0, false);
    float after = underlay_min_y(e);
    check(before < FLT_MAX && std::fabs(before - after) < .1f,
          "pitch overlay stays at the same note pitch when the roll is resized");
    int endpoint = -1, id = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        auto a = e->project.automation[i];

        if (a.type == 0xe0 && a.curve > 0) {
            endpoint = (int)i;
            id = a.curve;
        }
    }

    check(endpoint >= 0, "bend stores editable endpoints");
    float height_before = e->automation_height;
    e->automation_height = 240;
    e->automation_open = true;
    e->automation_cc = -2;
    e->automation_center = 64;
    mouse_frame(e, 0, 0, false);
    auto lane = pattern_event_lane();
    float lane_x = lane.x, lane_y = lane.y;
    auto node = e->project.automation[endpoint];
    int value = node.a + (node.b << 7);
    x = lane_x + (float)node.beat * 80;
    float pitch_step = 20;
    y = lane_y + lane.h * .5f - 2 * pitch_step;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 40, y + 24, true, false, true);
    mouse_frame(e, x + 40, y + 24, false);
    bool moved = false, reset = false;

    for (auto i = 0u; i < e->project.automation_count; ++i) {
        auto a = e->project.automation[i];

        if (a.curve == id && a.type == 0xe0 && std::fabs(a.beat - node.beat - .5) < 1e-6 &&
            a.a + (a.b << 7) == value) {
            moved = true;
        }

        if (a.curve == -id && a.type == 0xe0 && std::fabs(a.beat - node.beat - .5) < 1e-6) {
            reset = true;
        }
    }

    check(moved, "Shift-drag moves an event point horizontally while retaining its value");
    check(reset, "moving the last event point also moves its baseline restoration");
    // Vertical-only editing keeps the same time; free horizontal editing bypasses quarter beats.
    x += 40;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 45, y - pitch_step, true, false, false, true);
    mouse_frame(e, x + 45, y - pitch_step, false);
    int edited = -1;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        auto a = e->project.automation[i];

        if (a.curve == id && a.type == 0xe0 && std::fabs(a.beat - node.beat - .5) < 1e-6) {
            edited = (int)i;
        }
    }

    check(edited >= 0 &&
              e->project.automation[edited].a + (e->project.automation[edited].b << 7) > value,
          "Ctrl-drag changes pitch while retaining event time");
    y -= pitch_step;
    mouse_frame(e, x, y, false, true);
    mouse_frame(e, x, y, true, true);
    mouse_frame(e, x + 13, y, true, true, true);
    mouse_frame(e, x + 13, y, false);
    bool free_time = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        auto a = e->project.automation[i];

        if (a.curve == id && a.type == 0xe0 && std::fabs(a.beat - node.beat - .5 - .1625) < 1e-6) {
            free_time = true;
        }
    }

    check(free_time, "Alt plus Shift moves an event point without time snapping");
    size_t points = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].type == 0xe0 && e->project.automation[i].curve == id) {
            ++points;
        }
    }

    e->automation_node = -1; /* The moved point stays selected; a click would only deselect it. */
    e->event_selection.size = 0;
    mouse_frame(e, lane_x + 3 * 80, lane_y + 50, false);
    mouse_frame(e, lane_x + 3 * 80, lane_y + 50, true);
    mouse_frame(e, lane_x + 3 * 80, lane_y + 50, false);
    size_t extended = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].type == 0xe0 && e->project.automation[i].curve == id) {
            ++extended;
        }
    }

    check(extended == points + 1,
          "left click extends the closest event section at the cursor value");
    // This fixture extends controllers beyond its two-beat notes. A manual placement
    // extension includes those stored points without changing automatic note sizing.
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].pattern == 0) {
            e->project.clips[i].length = 4;
        }
    }

    DmSchedule schedule = {};
    check(dm_schedule(&e->project, &schedule), "edited event section compiles for playback");
    int final = -1;

    for (size_t i = 0; i < schedule.count; ++i) {
        if (schedule.items[i].status == 0xe0 && std::fabs(schedule.items[i].seconds - 1.5) < 1e-6) {
            final = schedule.items[i].a + (schedule.items[i].b << 7);
        }
    }

    check(final == 8192,
          "extended event section resets to its incoming value after the last point");
    dm_schedule_free(&schedule);

    mouse_frame(e, 0, 0, false);
    auto *surface = SDL_RenderReadPixels(e->renderer, nullptr);

    if (surface) {
        SDL_SaveBMP(surface, "agents/reports/todo-editor.bmp");
        SDL_DestroySurface(surface);
    }

    e->automation_open = false;
    e->automation_height = height_before;
}

static void labels_and_drop(Editor *e, void (*check)(bool, const char *)) {
    e->roll_height = 320;
    e->playlist_view.scroll_y = 0;
    e->tool = 0;
    mouse_frame(e, 0, 0, false);
    auto *w = find_window("/Playlist_");
    check(w != nullptr, "playlist exists");
    float x = w->Pos.x + w->WindowPadding.x + 24;
    float y = w->Pos.y + w->WindowPadding.y + ImGui::GetFrameHeight() +
              ImGui::GetStyle().ItemSpacing.y + 28 + 64 * 2 + 32;
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y, false);
    check(e->selected_channel == 1 && e->instrument_channel < 0,
          "left channel-label click selects a playable channel without opening a dialog");
    auto *patterns = find_window("/Patterns_");
    check(patterns != nullptr, "pattern browser exists");
    x = patterns->Pos.x + patterns->WindowPadding.x + 24;
    y = patterns->Pos.y + patterns->WindowPadding.y + ImGui::GetFrameHeight() +
        ImGui::GetStyle().ItemSpacing.y + 10 + 25;
    size_t count = e->project.clip_count;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 16, y + 5, true);
    float target_x = w->Pos.x + w->WindowPadding.x + e->label_width + 40 * 5;
    float target_y = w->Pos.y + w->WindowPadding.y + ImGui::GetFrameHeight() +
                     ImGui::GetStyle().ItemSpacing.y + 28 + 64 + 32;
    mouse_frame(e, target_x, target_y, true);
    mouse_frame(e, target_x, target_y, true);
    mouse_frame(e, target_x, target_y, false);
    check(e->project.clip_count == count + 1,
          "dragging a pattern card onto the playlist places a clip");
}

static void layout_and_mixer(Editor *e, void (*check)(bool, const char *)) {
    e->automation_open = false;
    mouse_frame(e, 0, 0, false);
    auto *ed_playlist = find_window("/Playlist_");
    float top = ed_playlist->Pos.y + ed_playlist->WindowPadding.y + ImGui::GetFrameHeight() +
                ImGui::GetStyle().ItemSpacing.y;
    float edge = ed_playlist->Pos.x + ed_playlist->WindowPadding.x + e->label_width;
    float old = e->label_width;
    mouse_frame(e, edge, top + 110, true);
    mouse_frame(e, edge + 60, top + 110, true);
    mouse_frame(e, edge + 60, top + 110, false);
    check(e->label_width > old + 40, "playlist label width can be resized independently");
    float old_mix = e->mixer_height;
    auto *ed_mixer = find_window("/Mixer_");
    float x = ed_mixer->Pos.x + 60, y = ed_mixer->Pos.y - 5;
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y - 40, true);
    mouse_frame(e, x, y - 40, false);
    check(e->mixer_height > old_mix + 20, "mixer divider expands the vertical channel area");
    auto *strip = find_window("/Channel strips_");
    ImGuiWindow *first = nullptr;

    for (auto *w : ImGui::GetCurrentContext()->Windows) {
        if (w->Active && std::strstr(w->Name, "/port strip_") &&
            (!first || w->Pos.x < first->Pos.x)) {
            first = w;
        }
    }

    check(strip && first, "mixer strips are laid out");
    e->project.channels[0].volume = 42;
    auto &io = ImGui::GetIO();
    mouse_frame(e, first->Pos.x + 22, first->Pos.y + 50, false);
    io.AddMouseButtonEvent(2, true);
    mouse_frame(e, first->Pos.x + 22, first->Pos.y + 50, false);
    check(e->project.channels[0].volume == e->reset_channels[0].volume,
          "middle-click restores the loaded mixer volume");
    io.AddMouseButtonEvent(2, false);
    mouse_frame(e, 0, 0, false);
    e->playlist_view.zoom = .15f;
    e->playlist_view.scroll_x = 1000;
    mouse_frame(e, 0, 0, false);
    check(e->playlist_view.scroll_x == 1000, "zooming out preserves free horizontal navigation");
    e->playlist_view.scroll_x = 0;
    e->playlist_view.zoom = 1;
    mouse_frame(e, 0, 0, false);
    ed_playlist = find_window("/Playlist_");
    top = ed_playlist->Pos.y + ed_playlist->WindowPadding.y + ImGui::GetFrameHeight() +
          ImGui::GetStyle().ItemSpacing.y;
    x = ed_playlist->Pos.x + ed_playlist->WindowPadding.x + e->label_width + 120;
    y = top + 12;
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 40, y, true);
    mouse_frame(e, x + 40, y, false);
    check(e->start_beat[0] == 4 && e->cursor_beat[0] == 4,
          "dragging a ruler selects and remembers the playback start");
    e->context = 1;
    e->edit_focus = FOCUS_PLAYLIST;
    direct_key(e, SDL_SCANCODE_SPACE, SDL_KMOD_SHIFT);
    check(e->start_beat[0] == 0, "Shift-Space starts from the beginning");
}

static float tempo_card_width() {
    auto *w = find_window("/Playlist_");
    float left = FLT_MAX, right = -FLT_MAX;

    for (auto &v : w->DrawList->VtxBuffer) {
        if (v.col == 0xff2b367bu) {
            left = ImMin(left, v.pos.x);
            right = ImMax(right, v.pos.x);
        }
    }

    return right - left;
}

static void event_inspection(Editor *e, void (*check)(bool, const char *)) {
    e->project.event_count = 0;
    e->playlist_view.zoom = 1;
    e->playlist_view.scroll_x = 0;
    const uint8_t tempo[] = {0xff, 0x51, 3, 7, 0xa1, 0x20};
    DmBytes bytes = {(unsigned char *)tempo, sizeof(tempo), sizeof(tempo)};
    check(dm_event_add(&e->project, 2, 0, 0, &bytes), "create conductor fixture");
    mouse_frame(e, 0, 0, false);
    float old = tempo_card_width();
    e->playlist_view.zoom = 2;
    mouse_frame(e, 0, 0, false);
    float bigger = tempo_card_width();
    check(old > 10 && bigger > old * 1.8f,
          "tempo event cards expand with zoom and use their auburn color");
    auto *w = find_window("/Playlist_");
    float top =
        w->Pos.y + w->WindowPadding.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    float x = w->Pos.x + w->WindowPadding.x + e->label_width + 2 * 80 + 8, y = top + 43;
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y, false);
    check(e->song_event && e->event_edit == 0 && e->event_kind == 0 &&
              std::fabs(e->event_bpm - 120) < .01f,
          "clicking an existing event opens its editable tempo inspector");
    e->event_bpm = 90;
    auto *ed_dialog = ImGui::FindWindowByName("Song event");
    ImGui::ActivateItemByID(ed_dialog->GetID("Apply event"));
    mouse_frame(e, 0, 0, false);
    check(!e->song_event && std::fabs(dm_beat_seconds(&e->project, 3) - (1 + 2.0 / 3)) < .0001,
          "event inspector applies a changed tempo to playback timing");
    e->playlist_view.zoom = 1;
}

static void shortcut_and_dialogs(Editor *e, void (*check)(bool, const char *)) {
    e->shortcuts = true;
    e->capture = ACT_PLAY_START;
    SDL_Keymod mods = (SDL_Keymod)(SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT | SDL_KMOD_GUI);
    direct_key(e, SDL_SCANCODE_F8, mods);
    check(e->capture < 0 && e->bindings[ACT_PLAY_START].mods == mods,
          "shortcut capture accepts combined Ctrl Alt Shift and Win modifiers");
    e->shortcuts = false;
    e->instrument_channel = 0;
    mouse_frame(e, 0, 0, false);
    int program = e->project.channels[0].program;
    direct_key(e, SDL_SCANCODE_DOWN);
    check(e->project.channels[0].program == program + 1,
          "instrument arrow selection has contextual priority");
    e->instrument_channel = -1;
    e->piano = true;
    direct_key(e, SDL_SCANCODE_KP_2);
    direct_key(e, SDL_SCANCODE_Q);
    check(e->held[SDL_SCANCODE_Q] == 72,
          "keyboard octave shortcut remains active with the keyboard piano enabled");
    SDL_Event release;
    SDL_zero(release);
    release.type = SDL_EVENT_KEY_UP;
    release.key.scancode = SDL_SCANCODE_Q;
    editor_event(e, &release);
    direct_key(e, SDL_SCANCODE_KP_1);
    e->piano = false;
    e->about = true;
    mouse_frame(e, 0, 0, false);
    auto *about = ImGui::FindWindowByName("About duckmidi");
    ImGui::SetWindowPos(about, ImVec2(1300, 850));
    SDL_SetWindowSize(e->window, 960, 540);
    SDL_PumpEvents();
    ui_frame();
    editor_frame(e, 960, 540);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_RenderPresent(e->renderer);
    check(about->Pos.x >= 0 && about->Pos.y >= 0 && about->Pos.x + about->Size.x <= 961 &&
              about->Pos.y + about->Size.y <= 541,
          "floating dialogs stay reachable after a window resize");
    e->about = false;
    SDL_SetWindowSize(e->window, 1400, 900);
    SDL_PumpEvents();
    mouse_frame(e, 0, 0, false);
}

static void transport_shortcuts_ui(Editor *e, void (*check)(bool, const char *)) {
    editor_smoke(e);
    e->automation_open = false;
    e->pattern_channel = 0;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->tool = 0;
    e->roll_height = 400;
    auto &p = e->project.patterns[0];
    p.length = 64;
    e->project.automation_count = 0;
    e->roll_view = {1, 0, (127 - 70) * 20, 0};
    mouse_frame(e, 0, 0, false);
    auto grid = roll_origin();
    auto *pattern_browser = find_window("/Patterns_");
    auto *arrangement = find_window("/Playlist_");
    check(std::fabs(pattern_browser->Pos.y - arrangement->Pos.y) < .1f &&
              std::fabs(pattern_browser->WindowPadding.y - arrangement->WindowPadding.y) < .1f,
          "pattern and arrangement section titles share the same top inset");
    float vertical = e->roll_view.scroll_y;
    mouse_frame(e, grid.x + 200, grid.y + 60, false);
    mouse_frame(e, grid.x + 200, grid.y + 60, false, false, false, false, -1);
    check(e->roll_view.scroll_x > 0 && e->roll_view.scroll_y == vertical,
          "plain wheel moves the piano grid horizontally");
    float horizontal = e->roll_view.scroll_x;
    mouse_frame(e, grid.x + 200, grid.y + 60, false, false, true);
    mouse_frame(e, grid.x + 200, grid.y + 60, false, false, true, false, -1);
    check(e->roll_view.scroll_x == horizontal && e->roll_view.scroll_y > vertical,
          "Shift wheel moves the piano grid vertically");
    float zoom = e->roll_view.zoom;
    mouse_frame(e, grid.x + 200, grid.y - 15, false);
    mouse_frame(e, grid.x + 200, grid.y - 15, false, false, false, false, 1);
    check(e->roll_view.zoom > zoom, "wheel over piano ruler zooms");
    auto *ed_playlist = find_window("/Playlist_");
    float px = ed_playlist->Pos.x + ed_playlist->WindowPadding.x + e->label_width + 100;
    float py = ed_playlist->Pos.y + ed_playlist->WindowPadding.y + 40 + 10;
    zoom = e->playlist_view.zoom;
    mouse_frame(e, px, py, false);
    mouse_frame(e, px, py, false, false, false, false, 1);
    check(e->playlist_view.zoom > zoom, "wheel over arrangement ruler zooms");
    e->roll_view = {1, 0, (127 - 70) * 20, 0};
    p.count = 0;
    dm_note_add(&p, {0, 2, 64, 100, 0});
    mouse_frame(e, 0, 0, false);
    grid = roll_origin();
    float x = grid.x + 40, y = grid.y + 6 * 20 + 10;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, false, false, false, false, 0, true);
    mouse_frame(e, x, y, false);
    check(p.count == 0 && ImGui::GetCurrentContext()->OpenPopupStack.Size == 0,
          "Right click deletes a note without opening a menu on release");
    dm_note_add(&p, {0, 2, 64, 100, 0});
    dm_automation_add(&e->project, {0, 0xe0, 0, 0, 64, 0, 1, 64});
    dm_automation_add(&e->project, {2, 0xe0, 0, 127, 127, 0, 1, 64});
    dm_automation_add(&e->project, {2, 0xe0, 0, 0, 64, 0, -1, 64});
    x = grid.x + 80;
    y = grid.y + 5 * 20 + 10;
    mouse_frame(e, x, y, false, true);
    mouse_frame(e, x, y, true, true);
    mouse_frame(e, x + 20, y - 20, true, true);
    mouse_frame(e, x + 20, y - 20, false);
    check(e->project.automation_count == 4 && p.count == 1,
          "real Alt drag on overlay inserts and moves a point without adding a note");
    mouse_frame(e, x + 20, y - 20, false, false, true);
    mouse_frame(e, x + 20, y - 20, true, false, true);
    mouse_frame(e, x + 20, y - 20, false);
    check(e->automation_node >= 0 && e->drag == 0,
          "real Shift click selects overlay point without starting note drag");
    direct_key(e, SDL_SCANCODE_DELETE);
    check(e->project.automation_count == 3 && p.count == 1,
          "real overlay selection routes Delete to the point");
    e->automation_open = true;
    e->automation_cc = -2;
    e->automation_height = 180;
    mouse_frame(e, 0, 0, false);
}

static void middle_frame(Editor *e, float x, float y, bool down) {
    ImGui::GetIO().AddMouseButtonEvent(2, down);
    mouse_frame(e, x, y, false);
}

static void edit_gestures_ui(Editor *e, void (*check)(bool, const char *)) {
    editor_smoke(e);
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->tool = 0;
    e->roll_height = 400;
    e->automation_open = false;
    e->roll_view = {1, 200, 1140, 0};
    e->project.patterns[0].length = 64;
    mouse_frame(e, 0, 0, false);
    auto grid = roll_origin();
    float x = grid.x + 200, y = grid.y + 60;
    mouse_frame(e, x, y, false);
    float sx = e->roll_view.scroll_x, sy = e->roll_view.scroll_y;
    middle_frame(e, x, y, true);
    middle_frame(e, x - 30, y - 20, true);
    check(e->roll_view.scroll_x > sx + 25 && e->roll_view.scroll_y > sy + 15,
          "middle drag pans the piano roll on both axes");
    middle_frame(e, x - 30, y - 20, false);
    check(e->pan_context == 0, "releasing middle mouse ends pan capture");

    for (int c = 0; c < 16; ++c) {
        e->project.channels[c].enabled = true;
        e->project.channels[c].program = 0;
    }

    e->playlist_view = {1, 200, 100, 0};
    mouse_frame(e, 0, 0, false);
    auto *ed_playlist = find_window("/Playlist_");
    float px = ed_playlist->Pos.x + ed_playlist->WindowPadding.x + e->label_width + 100;
    float py = ed_playlist->Pos.y + ed_playlist->WindowPadding.y + 40 + 28 + 80;
    mouse_frame(e, px, py, false);
    sx = e->playlist_view.scroll_x;
    sy = e->playlist_view.scroll_y;
    middle_frame(e, px, py, true);
    middle_frame(e, px - 25, py - 18, true);
    check(e->playlist_view.scroll_x > sx + 20 && e->playlist_view.scroll_y > sy + 12,
          "middle drag pans the arrangement on both axes");
    middle_frame(e, px - 25, py - 18, false);
    // An unrelated floating dialog must consume its own wheel input.
    e->about = true;
    mouse_frame(e, 0, 0, false);
    auto *about = ImGui::FindWindowByName("About duckmidi");
    ImGui::SetWindowPos(about, ImVec2(x - 30, y - 30));
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, false);
    sx = e->roll_view.scroll_x;
    sy = e->roll_view.scroll_y;
    float zoom = e->roll_view.zoom;
    mouse_frame(e, x, y, false, false, false, false, -1);
    check(e->roll_view.scroll_x == sx && e->roll_view.scroll_y == sy && e->roll_view.zoom == zoom,
          "floating dialog wheel does not scroll the piano underneath");
    ImGui::SetWindowPos(about, ImVec2(px - 30, py - 30));
    mouse_frame(e, px, py, false);
    mouse_frame(e, px, py, false);
    sx = e->playlist_view.scroll_x;
    sy = e->playlist_view.scroll_y;
    mouse_frame(e, px, py, false, false, false, false, -1);
    check(e->playlist_view.scroll_x == sx && e->playlist_view.scroll_y == sy,
          "floating dialog wheel does not scroll the arrangement underneath");
    e->about = false;
    e->automation_open = true;
    e->automation_cc = -2;
    e->automation_raw = false;
    e->automation_center = 64;
    e->automation_height = 180;
    mouse_frame(e, 0, 0, false);
    auto lane = pattern_event_lane();
    float lx = lane.x + 200, ly = lane.y + lane.h * .5f;
    mouse_frame(e, lx, ly, false);
    sx = e->roll_view.scroll_x;
    middle_frame(e, lx, ly, true);
    middle_frame(e, lx - 30, ly + 18, true);
    check(e->roll_view.scroll_x > sx + 25 && e->automation_center > 64.8f,
          "middle drag pans time and pitch in the event lane");
    middle_frame(e, lx - 30, ly + 18, false);
    e->edit_focus = FOCUS_NOTES;
    e->context = 2;
    mouse_frame(e, lane.x + lane.w * .5f, lane.y - 28 - 14, false);
    mouse_frame(e, lane.x + lane.w * .5f, lane.y - 28 - 14, true);
    mouse_frame(e, lane.x + lane.w * .5f, lane.y - 28 - 14, false);
    check(e->edit_focus == FOCUS_EVENTS, "clicking the events toolbar focuses the events section");
    e->automation_open = false;
    e->roll_view = {1, 0, 1140, 0};
    auto &p = e->project.patterns[0];
    p.count = 0;
    dm_note_add(&p, {0, 2, 64, 100, 1});
    dm_note_add(&p, {1, 1, 67, 100, 1});
    mouse_frame(e, 0, 0, false);
    grid = roll_origin();
    x = grid.x + 2 * 80 - 3;
    y = grid.y + (70 - 64) * 20 + 10;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 40, y, true);
    mouse_frame(e, x + 40, y, false);
    check(std::fabs(p.notes[0].length - 2.5) < .001 && std::fabs(p.notes[1].length - 1.5) < .001,
          "actual selected-note edge drag resizes the group equally");
    size_t notes = p.count;
    x = grid.x + 40;
    mouse_frame(e, x, y, false, false, false, true);
    mouse_frame(e, x, y, true, false, false, true);
    mouse_frame(e, grid.x + 200, y - 80, true, false, false, true);
    mouse_frame(e, grid.x + 200, y - 80, false);
    check(p.count == notes && e->tool == 0 && !e->drag && !e->marquee,
          "Ctrl marquee is temporary and does not insert or move a note");
    auto *patterns = find_window("/Patterns_");
    float cx = patterns->Pos.x + patterns->WindowPadding.x + 20;
    float cy = patterns->Pos.y + patterns->WindowPadding.y + 80;
    mouse_frame(e, cx, cy, false);
    mouse_frame(e, cx, cy, true);
    mouse_frame(e, cx, cy, false);
    check(e->pattern_highlight, "pattern card highlights when clicked");
    int selected = e->selected_pattern;
    mouse_frame(e, 800, 53, true);
    mouse_frame(e, 800, 53, false);
    check(e->pattern_highlight && e->selected_pattern == selected,
          "clicking another section retains the shared pattern selection");
    e->confirm = true;
    mouse_frame(e, 0, 0, false);
    auto *confirm = ImGui::FindWindowByName("Unsaved project");
    check(confirm && (confirm->Flags & ImGuiWindowFlags_NoResize) && confirm->Size.x == 430 &&
              confirm->Size.y == 160,
          "save confirmation is a fixed-size window");
    e->confirm = false;
}

static void capture_workspace_navigation(Editor *e, const char *path);

static void metadata_export_ui(Editor *e, void (*check)(bool, const char *)) {
    e->config = e->preferences = e->shortcuts = false;
    e->export_window = true;
    check(e->export_metadata == DM_METADATA_DUCKMIDI_ONLY,
          "export defaults to keeping only duckmidi editor metadata");
    mouse_frame(e, 0, 0, false);
    auto *ed_export_dialog = ImGui::FindWindowByName("Export");
    check(ed_export_dialog && ed_export_dialog->Active, "MIDI export dialog is visible");
    const char *labels[] = {"Keep all editor metadata", "Keep only duckmidi editor metadata",
                            "Strip all editor metadata"};
    const int choices[] = {0, 2, 1};

    for (int choice : choices) {
        ImGui::ActivateItemByID(ed_export_dialog->GetID("Editor metadata"));
        mouse_frame(e, 0, 0, false);
        mouse_frame(e, 0, 0, false);
        auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
        check(popups.Size > 0 && popups.back().Window, "metadata choices popup opens");

        if (choice == 0) {
            capture_workspace_navigation(e, "agents/reports/midi-metadata-choices.bmp");
        }

        ImGui::ActivateItemByID(popups.back().Window->GetID(labels[choice]));
        mouse_frame(e, 0, 0, false);
        mouse_frame(e, 0, 0, false);
        check(e->export_metadata == (DmMetadataMode)choice,
              "metadata choice applies through the UI");
    }

    capture_workspace_navigation(e, "agents/reports/midi-metadata-choices-default.bmp");
    e->export_window = false;
}

extern "C" void test_editor_updates(Editor *e, void (*check)(bool, const char *)) {
    editor_smoke(e);
    group_drag(e, check);
    quantized_alignment(e, check);
    curve_edits(e, check);
    labels_and_drop(e, check);
    layout_and_mixer(e, check);
    event_inspection(e, check);
    shortcut_and_dialogs(e, check);
    transport_shortcuts_ui(e, check);
    edit_gestures_ui(e, check);
    metadata_export_ui(e, check);
}

extern "C" void test_editor_stop(Editor *e) {
    mouse_frame(e, 0, 0, false);
    auto *root = ImGui::FindWindowByName("##workspace");
    ImGui::ActivateItemByID(root->GetID("##Stop"));
    mouse_frame(e, 0, 0, false);
}

extern "C" void test_editor_combine(Editor *e, const char *label) {
    mouse_frame(e, 0, 0, false);
    auto *ed_settings = ImGui::FindWindowByName("Project settings");
    ImGui::ActivateItemByID(ed_settings->GetID(label));
    mouse_frame(e, 0, 0, false);
}

static float event_lane_y() {
    float y = FLT_MAX;

    for (auto &v : roll_window()->DrawList->VtxBuffer) {
        if (v.col == 0xff121212u) {
            y = ImMin(y, v.pos.y);
        }
    }

    return y;
}

static void capture_workspace_navigation(Editor *e, const char *path) {
    for (int i = 0; i < 3; ++i) {
        mouse_frame(e, 0, 0, false);
    }

    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_Surface *s = SDL_RenderReadPixels(e->renderer, nullptr);

    if (s) {
        SDL_SaveBMP(s, path);
        SDL_DestroySurface(s);
    }
}
#ifdef DM_TEST_VISUALIZERS
extern "C" void recording_feedback_audio_render(DmAudio *, int);
extern "C" int visualizers_audio_voices(DmAudio *);

extern "C" void test_visualizers_ui(Editor *e, void (*check)(bool, const char *)) {
    e->about = true;
    e->roll = true;
    e->roll_height = 480;
    e->focus_pattern = false;
    capture_workspace_navigation(e, "agents/reports/about-paused.bmp");
    auto *about = ImGui::FindWindowByName("About duckmidi");
    check(about && about->Active && about->Size.y == 158 && !about->ScrollbarY,
          "compact About fits its information without scrolling");
    float maximum = 0;

    for (float band : e->spectrum) {
        maximum = ImMax(maximum, band);
    }

    check(maximum == 0, "About spectrum stays hidden before playback");
    char error[512];
    check(
        dm_audio_publish_pattern(e->audio, &e->project, 0, 0, 0, 16, 0, true, error, sizeof(error)),
        "start generated playback");
    bool interpolated = false;
    float previous = 0, previous_target = 0;

    for (int frame = 0; frame < 48; ++frame) {
        recording_feedback_audio_render(e->audio, 768);
        SDL_Delay(16);
        mouse_frame(e, 0, 0, false);

        if (frame && e->mixer_peak_targets[0] == previous_target &&
            std::fabs(e->mixer_peaks[0] - previous) > .000001f) {
            interpolated = true;
        }

        previous = e->mixer_peaks[0];
        previous_target = e->mixer_peak_targets[0];
    }

    maximum = 0;

    for (float band : e->spectrum) {
        maximum = ImMax(maximum, band);
    }

    check(maximum > .1f && maximum <= 1, "real synth output drives the About spectrum");
    check(interpolated, "mixer bars animate on GUI frames between peak samples");
    capture_workspace_navigation(e, "agents/reports/about-playing.bmp");
    int last_bar = -1, first_body_text = INT_MAX;
    ImU32 text_color = ImGui::GetColorU32(ImGuiCol_Text);

    for (int i = 0; i < about->DrawList->VtxBuffer.Size; ++i) {
        auto v = about->DrawList->VtxBuffer[i];

        if ((v.col & 0x00ffffffu) == (e->accent & 0x00ffffffu) && (v.col >> 24) >= 51 &&
            (v.col >> 24) <= 136) {
            last_bar = i;
        }

        if (v.col == text_color && v.pos.y > about->Pos.y + about->TitleBarHeight + 8) {
            first_body_text = ImMin(first_body_text, i);
        }
    }

    check(last_bar >= 0 && last_bar < first_body_text,
          "spectrum draws behind the About information");
    float stereo[DM_SPECTRUM_FRAMES * 2];
    float falling_before = 0;

    for (float band : e->spectrum) {
        falling_before = ImMax(falling_before, band);
    }

    auto *transport_window = ImGui::FindWindowByName("##workspace");
    SDL_Delay(16);
    ImGui::ActivateItemByID(transport_window->GetID("##Stop"));
    mouse_frame(e, 0, 0, false);
    maximum = 0;

    for (float band : e->spectrum) {
        maximum = ImMax(maximum, band);
    }

    check(maximum > 0 && maximum < falling_before,
          "Stop begins a natural fall instead of hiding the bars");
    check(!dm_audio_analyzer_read(e->audio, stereo),
          "stopped analyzer releases capture while its display falls");

    for (int frame = 0; frame < 85; ++frame) {
        float before = maximum;
        SDL_Delay(16);
        mouse_frame(e, 0, 0, false);
        maximum = 0;

        for (float band : e->spectrum) {
            maximum = ImMax(maximum, band);
        }

        check(maximum <= before, "stopped spectrum decays monotonically");

        if (frame == 10) {
            capture_workspace_navigation(e, "agents/reports/about-spectrum-falling.bmp");
        }
    }

    check(maximum == 0, "stopped bars disappear completely after their fall");
    check(
        dm_audio_publish_pattern(e->audio, &e->project, 0, 0, 0, 16, 0, true, error, sizeof(error)),
        "restart generated playback");
    mouse_frame(e, 0, 0, false);
    recording_feedback_audio_render(e->audio, 4096);
    e->about = false;
    mouse_frame(e, 0, 0, false);
    check(!dm_audio_analyzer_read(e->audio, stereo),
          "closing About disables capture during playback");
    dm_audio_transport(e->audio, false);
    dm_audio_control(e->audio, 0, 64, 127);
    dm_audio_preview(e->audio, 0, 60, 100, true);
    e->held[SDL_SCANCODE_Z] = 60;
    e->held_channels[SDL_SCANCODE_Z] = 0;
    recording_feedback_audio_render(e->audio, 512);
    check(visualizers_audio_voices(e->audio) > 0,
          "Shift-Stop starts with a sounding keyboard voice");
    mouse_frame(e, 0, 0, false, false, true);
    auto *workspace = ImGui::FindWindowByName("##workspace");
    ImGui::ActivateItemByID(workspace->GetID("##Stop"));
    mouse_frame(e, 0, 0, false, false, true);
    recording_feedback_audio_render(e->audio, 512);
    check(std::strstr(e->status, "MIDI panic") && e->held[SDL_SCANCODE_Z] == -1 &&
              visualizers_audio_voices(e->audio) == 0,
          "Shift-click Stop triggers panic and releases held editor keys");
    mouse_frame(e, 0, 0, false);
    // Exercise the renderer with full-height edge bands to verify flush geometry.
    ui_frame();
    ImGui::SetNextWindowSize({560, 130});
    ImGui::Begin("Spectrum bounds");
    auto *w = ImGui::GetCurrentWindow();
    int first = w->DrawList->VtxBuffer.Size;
    float bands[3] = {1, 1, 1};
    ui_spectrum(bands, 3, 100, e->accent);
    float left = FLT_MAX, top = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;

    for (int i = first; i < w->DrawList->VtxBuffer.Size; ++i) {
        auto v = w->DrawList->VtxBuffer[i].pos;
        left = ImMin(left, v.x);
        right = ImMax(right, v.x);
        top = ImMin(top, v.y);
        bottom = ImMax(bottom, v.y);
    }

    check(std::fabs(left - w->Pos.x - w->WindowBorderSize) < .01f &&
              std::fabs(right - w->Pos.x - w->Size.x + w->WindowBorderSize) < .01f &&
              std::fabs(bottom - w->Pos.y - w->Size.y + w->WindowBorderSize) < .01f,
          "spectrum spans the full interior width and touches the inside of the bottom border");
    check(bottom - top == 100, "spectrum peaks are capped at 100 logical pixels");
    float radius = ImMax(0.f, w->WindowRounding - w->WindowBorderSize);

    for (int i = first; i < w->DrawList->VtxBuffer.Size; ++i) {
        auto v = w->DrawList->VtxBuffer[i].pos;
        float cx = ImClamp(v.x, left + radius, right - radius);
        float cy = ImClamp(v.y, w->Pos.y + w->WindowBorderSize + radius, bottom - radius);
        float dx = v.x - cx, dy = v.y - cy;
        check(dx * dx + dy * dy <= radius * radius + .01f,
              "every spectrum vertex stays within the rounded window interior");
    }

    first = w->DrawList->VtxBuffer.Size;
    float short_bands[3] = {.02f, .01f, .02f};
    ui_spectrum(short_bands, 3, 100, e->accent);

    for (int i = first; i < w->DrawList->VtxBuffer.Size; ++i) {
        auto v = w->DrawList->VtxBuffer[i].pos;
        float cx = ImClamp(v.x, left + radius, right - radius), cy = bottom - radius;
        float dx = v.x - cx, dy = ImMax(0.f, v.y - cy);
        check(dx * dx + dy * dy <= radius * radius + .01f,
              "falling low bars also follow the window's curved bottom corners");
    }

    ImGui::End();
    ui_render(e->renderer);
}
#endif
extern "C" void test_event_lanes_ui(Editor *e, void (*check)(bool, const char *)) {
    e->roll = true;
    e->roll_height = 600;
    e->automation_open = true;
    e->focus_pattern = false;
    e->pattern_mode = true;
    e->cursor_beat[1] = 2;
    e->project.automation_count = 0;
    e->roll_view = {1, 0, (127 - 65) * 20.f, 0};
    const int events[] = {-3, 1, -2, 64};
    const float heights[] = {140.f, 260.f};

    for (int event : events) {
        e->automation_cc = event;
        e->automation_raw = true;

        for (float height : heights) {
            e->automation_height = height;
            mouse_frame(e, 0, 0, false);
            UiRect lane = pattern_event_lane();
            check(lane.w > 100 && lane.h > 30, "resized pattern event lane renders");
            float x = lane.x + 160;
            SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
            SDL_RenderClear(e->renderer);
            ui_render(e->renderer);
            SDL_Surface *surface = SDL_RenderReadPixels(e->renderer, nullptr);
            check(surface != nullptr, "read rendered event lane");

            for (int i = 1; i <= 3; ++i) {
                Uint8 r, g, b, a;
                SDL_ReadSurfacePixel(surface, (int)x, (int)(lane.y + lane.h * i / 4), &r, &g, &b,
                                     &a);
                check(std::abs((int)r - (int)(e->accent & 255)) < 3 &&
                          std::abs((int)g - (int)((e->accent >> 8) & 255)) < 3 &&
                          std::abs((int)b - (int)((e->accent >> 16) & 255)) < 3,
                      "playhead stays visible over the full event grid at each section size");
            }

            SDL_DestroySurface(surface);
            mouse_frame(e, lane.x + 100, lane.y + lane.h / 2, false, false, true, false, 2);
            check(e->roll_view.scroll_x == 0,
                  "Shift-wheel in numeric lane does not scroll the timeline");
        }
    }

    capture_workspace_navigation(e, "agents/reports/event-lanes-pedal-playhead.bmp");
    e->automation_cc = -2;
    e->automation_raw = false;
    e->automation_center = 60;
    capture_workspace_navigation(e, "agents/reports/event-lanes-note-pitch.bmp");
    UiRect lane = pattern_event_lane();
    mouse_frame(e, lane.x + 100, lane.y + 40, false, false, true, false, 1);
    check(e->automation_center > 60, "note-pitch lane retains Shift-wheel semitone scrolling");
    e->automation_raw = true;
    capture_workspace_navigation(e, "agents/reports/event-lanes-raw-pitch.bmp");
    auto &p = e->project.patterns[0];
    p.kind = 1;
    p.event = 1;
    p.mode = 0;
    e->automation_cc = 1;
    e->automation_browser = true;
    capture_workspace_navigation(e, "agents/reports/event-lanes-modulation.bmp");
    auto *editor = find_window("/Event Editor_");
    check(editor != nullptr, "standalone numeric editor renders");
    p.mode = 1;
    p.center = 64;
    p.amount = 1;
    p.speed = 1;
    capture_workspace_navigation(e, "agents/reports/event-lanes-modulation-lfo.bmp");
    uint8_t bytes[] = {0xb0, 3, 50};
    DmBytes ed_message = {bytes, sizeof(bytes), sizeof(bytes)};
    check(dm_event_add(&e->project, 0, 0, 0, &ed_message), "add isolated device-specific event");
    e->midi_events = true;
    e->midi_values = false;
    e->midi_event_index = (int)e->project.event_count - 1;
    e->midi_event_kind = 0;
    e->event_edit = e->midi_event_index;
    e->event_auto_edit = -1;
    dm_bytes_free(&e->midi_draft);
    dm_bytes(&e->midi_draft, bytes, sizeof(bytes));
    mouse_frame(e, 0, 0, false);
    auto *details = find_window("/MIDI event details_");
    check(details != nullptr, "existing device-specific event remains inspectable");
    ImGui::ActivateItemByID(details->GetID("Controller"));
    mouse_frame(e, 0, 0, false);
    mouse_frame(e, 0, 0, false);
    auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
    check(popups.Size && popups.back().Window, "controller choices open");
    auto *popup = popups.back().Window;
    check(popup->ContentSize.y < 90 * ImGui::GetTextLineHeightWithSpacing(),
          "controller menu excludes unnamed device-specific choices");
    capture_workspace_navigation(e, "agents/reports/event-lanes-controller-options.bmp");
    direct_key(e, SDL_SCANCODE_ESCAPE);
    e->midi_events = false;
}

extern "C" void test_toolbar_followup_ui(Editor *e, void (*check)(bool, const char *)) {
    for (const auto &shape : toolbar_geometry::toolbar_shapes) {
        int points = shape.count / 2;

        for (int i = 0; i < points - 1 + shape.kind; ++i) {
            int next = (i + 1) % points;
            check(std::hypot(shape.values[i * 2] - shape.values[next * 2],
                             shape.values[i * 2 + 1] - shape.values[next * 2 + 1]) > .00001,
                  "baked icon contours have no zero-length joins, including their closure");
        }
    }

    e->roll = true;
    e->roll_height = 480;
    e->automation_open = false;
    e->record = false;
    e->wait_input = true;
    e->wait_armed = false;
    e->pattern_mode = true;
    e->piano = true;
    mouse_frame(e, 0, 0, false);
    auto *workspace = ImGui::FindWindowByName("##workspace");
    ImGui::ActivateItemByID(workspace->GetID("##Record"));
    mouse_frame(e, 0, 0, false);
    check(e->record && !dm_audio_playing(e->audio),
          "Record button alone leaves paused transport paused");
    ImGui::ActivateItemByID(workspace->GetID("##Play"));
    mouse_frame(e, 0, 0, false);
    check(e->wait_armed && !dm_audio_playing(e->audio), "Play button arms Wait recording");
    capture_workspace_navigation(e, "agents/reports/toolbar-wait-armed.bmp");
    ImGui::ActivateItemByID(workspace->GetID("##Play"));
    mouse_frame(e, 0, 0, false);
    check(!e->wait_armed && !dm_audio_playing(e->audio), "armed Pause button cancels Wait");
    ImGui::ActivateItemByID(workspace->GetID("##Record"));
    mouse_frame(e, 0, 0, false);
    auto *patterns = find_window("/Patterns_");
    float px = patterns->WorkRect.Min.x + 24;
    float py =
        patterns->WorkRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 35;
    mouse_frame(e, px, py, false);
    SDL_Delay(400);
    mouse_frame(e, px, py, false);
    mouse_frame(e, px, py, false);
    check(std::strstr(e->hover, "click a playlist row") != nullptr,
          "pattern help is published to the status bar text");
    bool tooltip = false;

    for (auto *w : ImGui::GetCurrentContext()->Windows) {
        tooltip |= w->Active && std::strstr(w->Name, "##Tooltip");
    }

    check(!tooltip, "pattern hover has no floating tooltip");
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_Surface *help_capture = SDL_RenderReadPixels(e->renderer, nullptr);
    check(help_capture != nullptr, "capture hovered status help");
    SDL_SaveBMP(help_capture, "agents/reports/toolbar-pattern-status-help.bmp");
    SDL_DestroySurface(help_capture);
    auto *ed_roll = roll_window();
    mouse_frame(e, 0, 0, false);
    ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    float min_y = FLT_MAX, max_y = -FLT_MAX;

    for (const auto &vertex : ed_roll->DrawList->VtxBuffer) {
        if (vertex.col == text && vertex.pos.x > ed_roll->WorkRect.Min.x + 3 &&
            vertex.pos.x < ed_roll->WorkRect.Min.x + 20 && vertex.pos.y > ed_roll->WorkRect.Min.y &&
            vertex.pos.y < ed_roll->WorkRect.Min.y + 32) {
            min_y = ImMin(min_y, vertex.pos.y);
            max_y = ImMax(max_y, vertex.pos.y);
        }
    }

    check(min_y < max_y && std::fabs((min_y + max_y) * .5f - ed_roll->WorkRect.Min.y - 16) < 1.5f,
          "corner arrow aligns with the centerline of the toolbar controls");
    float mx = ed_roll->WorkRect.Min.x + 10, my = ed_roll->WorkRect.Min.y + 16;
    mouse_frame(e, mx, my, false);
    mouse_frame(e, mx, my, true);
    mouse_frame(e, mx, my, false);
    auto *popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
    check(popup && std::fabs(popup->Pos.x - ed_roll->WorkRect.Min.x - 2) < 1 &&
              popup->Pos.y >= ed_roll->WorkRect.Min.y + 32,
          "Quantize submenu opens directly below the arrow without covering toolbar text");
    capture_workspace_navigation(e, "agents/reports/toolbar-corner-menu-below.bmp");
    direct_key(e, SDL_SCANCODE_ESCAPE);
    mouse_frame(e, 0, 0, false);
    capture_workspace_navigation(e, "agents/reports/toolbar-compact-icons.bmp");
    ui_set_scale(1.25f);
    capture_workspace_navigation(e, "agents/reports/toolbar-large-icons.bmp");
    ui_set_scale(1);
}

extern "C" void test_interface_ui(Editor *e, void (*check)(bool, const char *)) {
    e->roll = true;
    e->roll_height = 500;
    e->automation_open = true;
    e->automation_height = 200;
    e->automation_cc = -3;
    e->focus_pattern = false;
    e->piano = false;
    e->roll_view = {1, 0, (127 - 64) * 20.f, 0};
    e->project.patterns[0].notes[0].beat = .13;
    mouse_frame(e, 0, 0, false);
    auto *ed_roll = roll_window();
    check(ed_roll != nullptr, "pattern editor renders");
    float mx = ed_roll->WorkRect.Min.x + 10, my = ed_roll->WorkRect.Min.y + 14;
    mouse_frame(e, mx, my, false);
    mouse_frame(e, mx, my, true);
    mouse_frame(e, mx, my, false);
    check(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId),
          "top-left triangle opens editor menu");
    auto *popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
    check(popup != nullptr && popup->Pos.y < ed_roll->WorkRect.Min.y + 40,
          "editor menu anchors to the top edge without vertical centering");
    capture_workspace_navigation(e, "agents/reports/interface-pattern-menu.bmp");
    float qx = popup->WorkRect.Min.x + 25;
    float qy = popup->WorkRect.Min.y + ImGui::GetTextLineHeightWithSpacing() + 8;
    mouse_frame(e, qx, qy, true);
    mouse_frame(e, qx, qy, false);
    check(std::fabs(e->project.patterns[0].notes[0].beat - .25) < .001,
          "menu Quantize performs existing quarter-beat action");
    float lx = FLT_MAX, ly = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;

    for (const auto &v : ed_roll->DrawList->VtxBuffer) {
        if (v.col == 0xff121212u) {
            lx = ImMin(lx, v.pos.x);
            ly = ImMin(ly, v.pos.y);
            right = ImMax(right, v.pos.x);
            bottom = ImMax(bottom, v.pos.y);
        }
    }

    check(lx < right && ly < bottom, "velocity lane has a drawing region");
    size_t notes = e->project.patterns[0].count, automation = e->project.automation_count;
    float rx = lx + 160, ry = ly - 14;
    mouse_frame(e, rx, ry, false);
    mouse_frame(e, rx, ry, true);
    mouse_frame(e, rx, ry, false);
    check(std::fabs(e->cursor_beat[1] - 2) < .001 && !e->scrub &&
              e->project.patterns[0].count == notes && e->project.automation_count == automation,
          "event-lane ruler seeks without editing notes or controller points");
    mouse_frame(e, lx + 80, ry, false, false, false, false, 0, true);
    mouse_frame(e, lx + 240, ry, false, false, false, false, 0, true);
    mouse_frame(e, lx + 240, ry, false);
    check(e->loop_enabled[1] && std::fabs(e->loop_start[1] - 1) < .001 &&
              std::fabs(e->loop_end[1] - 3) < .001,
          "event-lane ruler draws a loop region");
    capture_workspace_navigation(e, "agents/reports/interface-icons-ruler.bmp");
    e->os_cursor = false;
    mouse_frame(e, 330, 90, false);
    SDL_Cursor *custom = SDL_GetCursor();
    check(custom != nullptr, "native Iconoir cursor exists");
    e->os_cursor = true;
    mouse_frame(e, 330, 90, false);
    check(SDL_GetCursor() && SDL_GetCursor() != custom,
          "OS cursor preference restores native default");
    double elapsed[2];

    for (int mode = 0; mode < 2; ++mode) {
        e->os_cursor = mode != 0;
        mouse_frame(e, 330, 90, false);
        SDL_Cursor *cached = SDL_GetCursor();
        Uint64 start = SDL_GetPerformanceCounter();

        for (int i = 0; i < 200; ++i) {
            mouse_frame(e, 330 + i % 3, 90, false);
        }

        elapsed[mode] =
            1000.0 * (SDL_GetPerformanceCounter() - start) / SDL_GetPerformanceFrequency();
        check(SDL_GetCursor() == cached,
              "cursor handle remains cached across 200 mouse-motion frames");
    }

    std::printf("CURSOR 200 motion frames: Iconoir=%.3f ms, OS=%.3f ms\n", elapsed[0], elapsed[1]);
    e->os_cursor = false;
    uint8_t pedal[] = {0xb0, 66, 127}, program[] = {0xc0, 81};
    DmBytes ed_message = {pedal, sizeof(pedal), sizeof(pedal)};
    dm_event_add(&e->project, 1, 0, 0, &ed_message);
    ed_message = {program, sizeof(program), sizeof(program)};
    dm_event_add(&e->project, 2, 0, 0, &ed_message);
    e->midi_values = false;
    e->midi_events = true;
    e->midi_event_index = -1;
    mouse_frame(e, 0, 0, false);
    auto *list = find_window("/MIDI event list_");
    check(list != nullptr, "expanded MIDI browser renders");
    float ex = list->WorkRect.Min.x + 30, ey = list->WorkRect.Min.y + 10;
    mouse_frame(e, ex, ey, false);
    mouse_frame(e, ex, ey, true);
    mouse_frame(e, ex, ey, false);
    check(e->midi_event_index >= 0 && e->midi_draft.size == 3,
          "MIDI browser selects a named controller for friendly editing");
    capture_workspace_navigation(e, "agents/reports/interface-midi-friendly.bmp");
    auto *details = find_window("/MIDI event details_");
    ImGui::ActivateItemByID(details->GetID("MIDI values"));
    mouse_frame(e, 0, 0, false);
    check(e->midi_values, "MIDI values checkbox opens editable raw message mode");
    capture_workspace_navigation(e, "agents/reports/interface-midi-raw.bmp");
    ImGui::ActivateItemByID(details->GetID("Hexadecimal"));
    mouse_frame(e, 0, 0, false);
    check(!e->midi_hex && std::strstr((char *)e->midi_text.data, "176 66 127"),
          "decimal display toggle converts the complete raw MIDI message");
    ImGui::ActivateItemByID(details->GetID("MIDI values"));
    mouse_frame(e, 0, 0, false);
    check(!e->midi_values, "raw message can return to friendly fields without applying");
    mouse_frame(e, ex, ey + 36, false);
    mouse_frame(e, ex, ey + 36, true);
    mouse_frame(e, ex, ey + 36, false);
    check(e->midi_draft.size == 2 && (e->midi_draft.data[0] & 0xf0) == 0xc0,
          "instrument event opens a friendly program selector");
    capture_workspace_navigation(e, "agents/reports/interface-midi-program.bmp");
    e->midi_events = false;
}

extern "C" void test_event_controls_ui(Editor *e, void (*check)(bool, const char *)) {
    e->roll = true;
    e->automation_browser = true;
    e->automation_open = true;
    e->focus_pattern = false;
    e->roll_view = {1, 0, 0, 0};
    e->project.automation_count = 0;
    e->automation_cc = 64;
    auto &p = e->project.patterns[0];
    p.kind = 1;
    p.mode = 0;
    p.event = 64;
    capture_workspace_navigation(e, "agents/reports/event-controls-pedal-empty.bmp");
    auto *w = find_window("/Event Editor_");
    check(w != nullptr, "standalone pedal editor renders");
    float gx = w->WorkRect.Min.x + 64;
    float gy = w->WorkRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 28;
    float y = (gy + w->WorkRect.Max.y - 24) * .5f;
    mouse_frame(e, gx + 80, y, false);
    mouse_frame(e, gx + 80, y, true);
    mouse_frame(e, gx + 240, y + 4, true);
    mouse_frame(e, gx + 240, y + 4, false);
    check(e->project.automation_count == 3, "real mouse draws sustain interval on single line");
    capture_workspace_navigation(e, "agents/reports/event-controls-sustain.bmp");
    e->automation_cc = 66;
    p.event = 66;
    e->project.automation_count = 0;
    mouse_frame(e, gx + 80, y, false);
    mouse_frame(e, gx + 80, y, true);
    mouse_frame(e, gx + 240, y, true);
    mouse_frame(e, gx + 240, y, false);
    check(e->project.automation_count == 3, "real mouse draws sostenuto interval");
    e->automation_cc = 5;
    p.event = 5;
    e->project.automation_count = 0;
    mouse_frame(e, gx + 80, y, false, false, true);
    mouse_frame(e, gx + 80, y, true, false, true);
    mouse_frame(e, gx + 240, y - 45, true, false, true);
    mouse_frame(e, gx + 240, y - 45, false, false, true);
    check(e->project.automation_count == 3, "real mouse draws portamento timing section");
    capture_workspace_navigation(e, "agents/reports/event-controls-portamento.bmp");
    e->automation_cc = 7;
    p.event = 7;
    e->project.automation_count = 0;
    mouse_frame(e, gx + 80, y, false, false, true);
    mouse_frame(e, gx + 80, y, true, false, true);
    mouse_frame(e, gx + 240, y - 45, true, false, true);
    mouse_frame(e, gx + 240, y - 45, false, false, true);
    check(e->project.automation_count == 3, "real mouse draws volume section");
    capture_workspace_navigation(e, "agents/reports/event-controls-volume.bmp");
    p.mode = 1;
    p.event = 93;
    e->automation_cc = 93;
    capture_workspace_navigation(e, "agents/reports/event-controls-chorus-lfo.bmp");
}

extern "C" void test_channel_routing_ui(Editor *e, void (*check)(bool, const char *)) {
    e->roll = false;
    e->label_width = 240;
    e->playlist_view = {1, 0, 0, 0};
    e->selected_pattern = 0;
    e->pattern_highlight = true;
    e->preview_override = false;
    e->project.clip_count = 0;

    for (int c = 0; c < 16; ++c) {
        e->project.channels[c] = {false, false, false, -1, 100, 64};
        e->project.lane_count[c] = 0;
        e->channel_expanded[c] = false;
    }

    e->project.channels[0] = {true, false, false, 0, 100, 64};
    e->project.channels[2] = {true, false, false, 33, 100, 64};
    dm_clip_add(&e->project, {.beat = 0, .pattern = 0, .channel = 0, .length = 4});
    mouse_frame(e, 0, 0, false);
    auto *w = find_window("/Playlist_");
    check(w != nullptr, "routing playlist renders");
    float gx = w->WorkRect.Min.x + e->label_width;
    float top = w->WorkRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 28;
    float x = gx + 40, y = top + 64 + 32, target = top + 3 * 64 + 32;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, target, true);
    mouse_frame(e, x, target, false);
    check(e->project.clips[0].channel == 2 && e->selected_channel == 2 && e->pattern_channel == 2 &&
              e->preview_source_channel == 2 && e->preview_program == 33,
          "real mouse move keeps selected row and audible preview on destination channel");
    e->project.lane_count[2] = 1;
    e->channel_expanded[2] = true;
    capture_workspace_navigation(e, "agents/reports/channel-routing-destination.bmp");
    auto *patterns = find_window("/Patterns_");
    check(patterns != nullptr, "routing pattern browser renders");
    float sx = patterns->WorkRect.Min.x + 24;
    float sy =
        patterns->WorkRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 35;
    int selected = e->selected_clip;
    mouse_frame(e, sx, sy, false);
    mouse_frame(e, sx, sy, true);
    mouse_frame(e, sx + 16, sy + 5, true);
    float dx = gx + 200, dy = top + 4 * 64 + 32;
    mouse_frame(e, dx, dy, true);
    mouse_frame(e, dx, dy, true);
    mouse_frame(e, dx, dy, false);
    check(e->project.clip_count == 1 && e->selected_clip == selected &&
              e->project.clips[0].channel == 2,
          "dropping notes onto automation subrow creates no placement or phantom selection");
    capture_workspace_navigation(e, "agents/reports/channel-routing-rejected-drop.bmp");
}

extern "C" void test_workspace_navigation_ui(Editor *e, void (*check)(bool, const char *)) {
    e->piano = false;
    e->song_event = e->config = e->preferences = e->shortcuts = false;
    e->automation_node = -1;
    e->automation_open = true;
    e->roll_height = 480;
    e->automation_cc = -2;
    e->roll_view = {1, 200, 1100, 0};
    e->project.patterns[0].length = 64;
    mouse_frame(e, 0, 0, false);
    float pitch_y = event_lane_y();
    e->automation_cc = 11;
    mouse_frame(e, 0, 0, false);
    check(std::fabs(pitch_y - event_lane_y()) < .1f,
          "event grid stays aligned across pitch/controller choices");
    e->automation_cc = -2;
    capture_workspace_navigation(e, "agents/reports/workspace_navigation-editor.bmp");
    check(e->automation_cc == -2, "pitch bend remains selected after rendering");
    bool time_mode = e->duration_beats;
    mouse_frame(e, 250, 53, false);
    mouse_frame(e, 250, 53, true);
    mouse_frame(e, 250, 53, false);
    check(e->duration_beats != time_mode, "plain duration text toggles beat display on click");
    e->automation_open = false;
    mouse_frame(e, 0, 0, false);
    auto *w = roll_window();
    float right = w->WorkRect.Max.x;
    float bottom = w->WorkRect.Max.y - 24;
    float x = w->WorkRect.Min.x + 150;
    float old = e->roll_view.scroll_x;
    mouse_frame(e, x, bottom + 10, false);
    mouse_frame(e, x, bottom + 10, false, false, false, false, -1);
    check(e->roll_view.scroll_x > old + 60, "wheel on horizontal scrollbar moves time");
    float y = w->WorkRect.Min.y + 100;
    old = e->roll_view.scroll_y;
    mouse_frame(e, right - 6, y, false);
    mouse_frame(e, right - 6, y, false, false, false, false, -1);
    check(e->roll_view.scroll_y > old + 60, "wheel on vertical scrollbar moves pitches");
    float ruler_y = w->WorkRect.Min.y + 40 + 10;
    float grid_x = w->WorkRect.Min.x + 64;
    e->roll_view.scroll_x = 200;
    mouse_frame(e, grid_x + 100, ruler_y, false);
    mouse_frame(e, grid_x + 100, ruler_y, true);
    old = e->roll_view.scroll_x;

    for (int i = 0; i < 5; ++i) {
        SDL_Delay(10);
        mouse_frame(e, right + 100, ruler_y, true);
    }

    check(e->scrub && e->roll_view.scroll_x > old,
          "dragging piano playhead beyond right edge scrolls time");

    for (int i = 0; i < 5; ++i) {
        SDL_Delay(10);
        mouse_frame(e, grid_x - 120, ruler_y, true);
    }

    check(e->roll_view.scroll_x >= 0, "left playhead autoscroll remains bounded");
    mouse_frame(e, grid_x - 120, ruler_y, false);
    check(!e->scrub, "playhead releases outside its canvas");
    auto *ed_playlist = find_window("/Playlist_");
    float playlist_y = ed_playlist->WorkRect.Min.y + ImGui::GetFrameHeight() +
                       ImGui::GetStyle().ItemSpacing.y + 10;
    float playlist_x = ed_playlist->WorkRect.Min.x + e->label_width + 100;
    e->playlist_view.scroll_x = 0;
    mouse_frame(e, playlist_x, playlist_y, false);
    mouse_frame(e, playlist_x, playlist_y, true);

    for (int i = 0; i < 5; ++i) {
        SDL_Delay(10);
        mouse_frame(e, ed_playlist->WorkRect.Max.x + 100, playlist_y, true);
    }

    check(e->scrub && e->playlist_view.scroll_x > 0,
          "dragging arrangement playhead beyond right edge scrolls time");
    mouse_frame(e, playlist_x, playlist_y, false);

    for (int c = 0; c < 16; ++c) {
        e->project.channels[c] = {true, false, false, 0, 100, 64};
    }

    mouse_frame(e, 0, 0, false);
    mouse_frame(e, 0, 0, false);
    auto *strips = find_window("/Channel strips_");
    check(strips && strips->ScrollMax.x > 0, "mixer has horizontal content for wheel test");
    int volume = e->project.channels[0].volume, pan = e->project.channels[0].pan;
    float mx = strips->WorkRect.Min.x + 18, my = strips->WorkRect.Min.y + 55;
    mouse_frame(e, mx, my, false);
    mouse_frame(e, mx, my, false, false, false, false, -1);
    mouse_frame(e, mx, my, false);
    check(strips->Scroll.x > 0 && e->project.channels[0].volume == volume &&
              e->project.channels[0].pan == pan,
          "mixer wheel scrolls strips without changing volume or pan");
    e->song_event = true;
    e->event_source_kind = 0;
    e->event_kind = 0;
    e->event_beat = 0;
    e->event_bpm = (float)e->project.bpm;
    e->event_numerator = e->project.numerator;
    e->event_denominator = e->project.denominator;
    e->cursor_beat[0] = 7.5;
    capture_workspace_navigation(e, "agents/reports/workspace_navigation-event.bmp");
    auto *event = ImGui::FindWindowByName("Song event");
    ImGui::ActivateItemByID(event->GetID("Use playhead"));
    mouse_frame(e, 0, 0, false);
    check(std::fabs(e->event_beat - 7.5) < .001, "Use playhead fills the arrangement event beat");
    capture_workspace_navigation(e, "agents/reports/workspace_navigation-event.bmp");
    e->song_event = false;
    e->about = true;
    mouse_frame(e, 0, 0, false);
    auto *about = ImGui::FindWindowByName("About duckmidi");
    check(about && (about->Flags & ImGuiWindowFlags_NoResize),
          "About window has no resize affordance");
    capture_workspace_navigation(e, "agents/reports/workspace_navigation-about.bmp");
    e->about = false;
}

extern "C" void test_selection_history_ui(Editor *e, void (*check)(bool, const char *)) {
    e->piano = false;
    e->automation_open = false;
    e->roll_height = 420;
    mouse_frame(e, 0, 0, false);
    auto grid = roll_origin();
    float x = grid.x + 12, y = grid.y + (70 - 60) * 20 + 10;
    int history = e->undo_count;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y, false);
    check(e->edit_focus == FOCUS_NOTES && e->project.patterns[0].notes[0].selected,
          "actual stationary note click focuses and selects its note");
    check(e->undo_count == history && !e->dirty,
          "stationary note selection creates no edit history");
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(!e->project.patterns[0].notes[0].selected && e->roll,
          "Escape in note canvas deselects without hiding editor");
    size_t count = e->project.patterns[0].count;
    mouse_frame(e, x, y, false, false, true);
    mouse_frame(e, x, y, false, false, true, false, 0, true);
    mouse_frame(e, x, y, false, false, true);
    check(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId),
          "Shift right click opens note menu");
    mouse_frame(e, x, y, false);
    mouse_frame(e, 1390, 880, true);
    mouse_frame(e, 1390, 880, false);
    check(e->project.patterns[0].count == count,
          "plain left click dismisses note popup without editing");
    mouse_frame(e, x, y, false, false, false, false, 0, true);
    mouse_frame(e, x, y, false);
    check(e->project.patterns[0].count == count - 1, "plain right click erases a note");
    auto *ed_playlist = find_window("/Playlist_");
    float px = ed_playlist->WorkRect.Min.x + e->label_width + 100;
    float py = ed_playlist->WorkRect.Min.y + ImGui::GetFrameHeight() +
               ImGui::GetStyle().ItemSpacing.y + 10;
    e->pattern_mode = true;
    mouse_frame(e, px, py, false);
    mouse_frame(e, px, py, true);
    mouse_frame(e, px, py, false);
    check(!e->pattern_mode, "actual playlist ruler seek switches Pattern playback to Song");
    e->pattern_mode = true;
    auto *ed_roll = roll_window();
    float ry = ed_roll->WorkRect.Min.y + 40 + 10;
    mouse_frame(e, grid.x + 100, ry, false);
    mouse_frame(e, grid.x + 100, ry, true);
    mouse_frame(e, grid.x + 100, ry, false);
    check(e->pattern_mode, "actual pattern ruler seek retains Pattern playback");
    capture_workspace_navigation(e, "agents/reports/selection_history-editor.bmp");
    e->preferences = true;
    capture_workspace_navigation(e, "agents/reports/selection_history-preferences.bmp");
    e->preferences = false;
    e->shortcuts = true;
    capture_workspace_navigation(e, "agents/reports/selection_history-shortcuts.bmp");
    e->shortcuts = false;
    e->config = true;
    capture_workspace_navigation(e, "agents/reports/selection_history-project.bmp");
    e->config = false;
    e->export_window = true;
    capture_workspace_navigation(e, "agents/reports/selection_history-export.bmp");
    e->export_window = false;
}

extern "C" void test_automation_transfer_ui(Editor *e, void (*check)(bool, const char *)) {
    check(e->mixer_height == 420, "default mixer height grows to 1.5 times its previous height");
    e->automation_open = true;
    e->automation_raw = true;
    e->automation_cc = 11;
    e->roll_height = 500;
    dm_automation_add(&e->project, {0, 0xb0, 0, 11, 64, 0, 8, 0});
    dm_automation_add(&e->project, {2, 0xb0, 0, 11, 96, 0, 8, 0});
    dm_automation_add(&e->project, {2.001, 0xb0, 0, 11, 127, 0, -8, 0});
    capture_workspace_navigation(e, "agents/reports/automation_transfer-transfer.bmp");
    auto *ed_roll = roll_window();
    ImGui::ActivateItemByID(ed_roll->GetID("Transfer Automation"));
    mouse_frame(e, 0, 0, false);
    mouse_frame(e, 0, 0, false);
    auto *events = find_window("/Event Editor_");
    check(events && e->automation_browser && e->project.patterns[e->selected_pattern].kind,
          "actual transfer button opens Event Editor and automation browser");
    check(e->channel_expanded[0] && e->project.lane_count[0] == 1,
          "transfer reveals channel subrow");
    capture_workspace_navigation(e, "agents/reports/automation_transfer-point.bmp");
    auto *ed_playlist = find_window("/Playlist_");
    float px = ed_playlist->WorkRect.Min.x + e->label_width + 20;
    float py = ed_playlist->WorkRect.Min.y + ImGui::GetFrameHeight() +
               ImGui::GetStyle().ItemSpacing.y + 10;
    mouse_frame(e, px, py, false);
    mouse_frame(e, px, py, false, false, false, false, 0, true);
    mouse_frame(e, px + 80, py, false, false, false, false, 0, true);
    mouse_frame(e, px + 80, py, false);
    check(e->loop_enabled[0] && std::fabs(e->loop_end[0] - e->loop_start[0] - 2) < .001 &&
              !e->pattern_mode,
          "actual right ruler drag selects a Song loop");
    e->pattern_highlight = false;
    e->channel_selected = false;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        e->project.clips[i].selected = 0;
    }

    e->edit_focus = FOCUS_PLAYLIST;
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(!e->loop_enabled[0], "Escape clears playlist loop after selections are gone");
    events = find_window("/Event Editor_");
    float ex = events->WorkRect.Min.x + 64 + 20, ey = events->WorkRect.Min.y + 40 + 10;
    mouse_frame(e, ex, ey, false);
    mouse_frame(e, ex, ey, false, false, false, false, 0, true);
    mouse_frame(e, ex + 80, ey, false, false, false, false, 0, true);
    mouse_frame(e, ex + 80, ey, false);
    check(e->loop_enabled[1] && e->pattern_mode,
          "Event Editor ruler selection switches to Pattern playback");
    e->edit_focus = FOCUS_EVENTS;
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(!e->loop_enabled[1] && !e->pattern_mode, "Event Editor Escape returns Song playback");
    auto &p = e->project.patterns[e->selected_pattern];
    p.mode = 1;
    p.event = 11;
    e->automation_cc = 11;
    capture_workspace_navigation(e, "agents/reports/automation_transfer-lfo.bmp");
    events = find_window("/Event Editor_");
    float kx = events->WorkRect.Max.x - 476 + 68 + 30;
    float ky = events->WorkRect.Min.y + 23 + (events->WorkRect.GetWidth() < 980 ? 40 : 0);
    double before = p.speed;
    mouse_frame(e, kx, ky, false);
    mouse_frame(e, kx, ky, true);
    mouse_frame(e, kx + 25, ky, true);
    check(p.speed == before, "horizontal knob movement does not change LFO speed");
    mouse_frame(e, kx + 25, ky - 20, true);
    mouse_frame(e, kx + 25, ky - 20, false);
    check(p.speed > before, "upward knob movement increases LFO speed");
    mouse_frame(e, kx, ky, false);
    middle_frame(e, kx, ky, true);
    middle_frame(e, kx, ky, false);
    check(p.speed == 1, "middle knob click restores sensible default");
    capture_workspace_navigation(e, "agents/reports/automation_transfer-lfo.bmp");
    p.mode = 0;
    e->automation_node = -1;
    e->roll_height = 400;
    capture_workspace_navigation(e, "agents/reports/automation_transfer-point-edit.bmp");
    events = find_window("/Event Editor_");
    ex = events->WorkRect.Min.x + 64 + 3 * 80;
    ey = events->WorkRect.Min.y + 40 + 28 + 70;
    size_t nodes = e->project.automation_count, placements = e->project.clip_count;
    mouse_frame(e, ex, ey, false);
    mouse_frame(e, ex, ey, true);
    mouse_frame(e, ex, ey, false);
    check(e->project.automation_count > nodes && e->edit_focus == FOCUS_EVENTS,
          "Event Editor supports manual point creation");
    direct_key(e, SDL_SCANCODE_A, SDL_KMOD_CTRL);
    direct_key(e, SDL_SCANCODE_DELETE);
    check(e->project.automation_count == 0 && e->project.clip_count == placements,
          "Event Editor select-all Delete removes points without removing placements");
    direct_key(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    mouse_frame(e, 0, 0, false);
    ed_playlist = find_window("/Playlist_");
    float gx = ed_playlist->WorkRect.Min.x + e->label_width;
    float rowy = ed_playlist->WorkRect.Min.y + ImGui::GetFrameHeight() +
                 ImGui::GetStyle().ItemSpacing.y + 28 + 2 * 64;
    double original_pattern_length = e->project.patterns[e->selected_pattern].length;
    float edge = gx + (float)dm_clip_length(&e->project, e->project.clips[8]) * 40 - 5;
    mouse_frame(e, edge, rowy + 20, false);
    mouse_frame(e, edge, rowy + 20, true);
    check(e->drag == 9, "automation placement edge enters duration resize");
    mouse_frame(e, edge + 80, rowy + 20, true);
    mouse_frame(e, edge + 80, rowy + 20, false);
    check(std::fabs(e->project.clips[8].length - 6) < .001 &&
              e->project.patterns[e->selected_pattern].length == original_pattern_length,
          "playlist edge changes only the automation placement duration");
    float labelx = ed_playlist->WorkRect.Min.x + 30;
    mouse_frame(e, labelx, rowy + 20, false);
    mouse_frame(e, labelx, rowy + 20, false, false, false, false, 0, true);
    mouse_frame(e, labelx, rowy + 20, false);
    check(e->lane_settings, "subrow context opens lane controls");
    std::snprintf(e->rename_text, 64, "%s", "Expression % ## literal");
    auto *ed_dialog = ImGui::FindWindowByName("Automation lane");
    ImGui::ClearActiveID();
    ImGui::ActivateItemByID(ed_dialog->GetID("Apply name"));
    mouse_frame(e, 0, 0, false);
    check(!std::strcmp(e->project.lane_names[0][0], "Expression % ## literal"),
          "lane names retain literal percent and hash characters");
    e->lane_settings = true;
    e->lane_channel = 0;
    e->lane_index = 1;
    mouse_frame(e, 0, 0, false);
    ed_dialog = ImGui::FindWindowByName("Automation lane");
    ImGui::ActivateItemByID(ed_dialog->GetID("Add subrow"));
    mouse_frame(e, 0, 0, false);
    check(e->project.lane_count[0] == 2, "lane controls add another stacked subrow");
    e->lane_settings = true;
    e->lane_index = 2;
    mouse_frame(e, 0, 0, false);
    ed_dialog = ImGui::FindWindowByName("Automation lane");
    ImGui::ActivateItemByID(ed_dialog->GetID("Remove subrow"));
    mouse_frame(e, 0, 0, false);
    check(e->project.lane_count[0] == 1 && e->project.clip_count == placements,
          "removing an empty subrow retains the other lanes and placements");
    capture_workspace_navigation(e, "agents/reports/automation_transfer-lanes.bmp");
    auto &pitch = e->project.patterns[e->selected_pattern];
    pitch.mode = 1;
    pitch.event = -2;
    e->automation_cc = -2;
    e->automation_raw = false;
    capture_workspace_navigation(e, "agents/reports/automation_transfer-pitch.bmp");
    events = find_window("/Event Editor_");
    ImGui::ActivateItemByID(events->GetID("MIDI values"));
    mouse_frame(e, 0, 0, false);
    check(e->automation_raw, "Event Editor switches pitch labels to raw MIDI values");
    capture_workspace_navigation(e, "agents/reports/automation_transfer-pitch-raw.bmp");
}

extern "C" void test_lfo_shapes_ui(Editor *e, void (*check)(bool, const char *)) {
    capture_workspace_navigation(e, "agents/reports/lfo_shapes-header.bmp");
    auto *events = find_window("/Event Editor_");
    auto &p = e->project.patterns[e->selected_pattern];
    float cx = events->WorkRect.Max.x - 476 + 30;
    float ky = events->WorkRect.Min.y + 23 + (events->WorkRect.GetWidth() < 980 ? 40 : 0);
    float sx = cx + 68;
    mouse_frame(e, sx, ky, false);
    mouse_frame(e, sx, ky, true);
    mouse_frame(e, sx, ky - 1, true);
    check(p.speed == 1, "nearby speed movement snaps exactly to a beat division");
    mouse_frame(e, sx, ky - 10, true);
    mouse_frame(e, sx, ky - 10, false);
    check(p.speed > 1.1, "continued speed movement escapes snap without sticking");
    mouse_frame(e, sx, ky, false);
    middle_frame(e, sx, ky, true);
    middle_frame(e, sx, ky, false);
    check(p.speed == 1, "speed reset remains functional");
    float gx = events->WorkRect.Min.x + 64, gy = events->WorkRect.Min.y + 108;
    float h = events->WorkRect.Max.y - gy - 24;
    float x = gx + 100, y = gy + h * .5f;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y - 20, true);
    mouse_frame(e, x, y - 20, false);
    check(p.center > 64 && std::fmod(p.center, 10) == 0,
          "waveform drag snaps controller center in tens");
    mouse_frame(e, cx, ky, false);
    middle_frame(e, cx, ky, true);
    middle_frame(e, cx, ky, false);
    check(p.center == 64, "middle center knob restores integer midpoint");
    mouse_frame(e, x, y, false, true);
    mouse_frame(e, x, y, true, true);
    mouse_frame(e, x, y - 3, true, true);
    mouse_frame(e, x, y - 3, false, true);
    check(p.center > 64 && p.center == std::round(p.center) && std::fmod(p.center, 10) != 0,
          "Alt waveform drag uses integer controller steps");
    mouse_frame(e, 0, 0, false);
    bool before = p.inverted;
    ImGui::ActivateItemByID(events->GetID("Invert"));
    mouse_frame(e, 0, 0, false);
    check(p.inverted != before, "header Invert toggle changes applied waveform");
    p.event = -2;
    e->automation_cc = -2;
    p.pitch_center = 60;
    e->automation_center = 60;
    e->automation_raw = false;
    capture_workspace_navigation(e, "agents/reports/lfo_shapes-pitch.bmp");
    events = find_window("/Event Editor_");
    gy = events->WorkRect.Min.y + 108;
    h = events->WorkRect.Max.y - gy - 24;
    y = gy + h * .5f;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y - 20, true);
    mouse_frame(e, x, y - 20, false);
    check(p.pitch_center > 60 && p.pitch_center == std::round(p.pitch_center),
          "pitch waveform drag snaps to notes");
    double pitch = p.pitch_center;
    mouse_frame(e, x, y, false, true);
    mouse_frame(e, x, y, true, true);
    mouse_frame(e, x, y - 3, true, true);
    mouse_frame(e, x, y - 3, false, true);
    check(p.pitch_center > pitch && std::fabs(p.pitch_center - std::round(p.pitch_center)) > .01,
          "Alt pitch drag allows between-note centers");
    mouse_frame(e, 0, 0, false);
    pitch = p.pitch_center;
    float view = e->automation_center;
    float vx = events->WorkRect.Max.x - 6;
    mouse_frame(e, vx, gy + h * .5f, false);
    mouse_frame(e, vx, gy + h * .5f, false, false, false, false, -1);
    mouse_frame(e, 0, 0, false);
    check(e->automation_center < view && p.pitch_center == pitch,
          "vertical scrollbar changes pitch view without changing LFO center");
    e->roll_view.zoom = 2;
    mouse_frame(e, 0, 0, false);
    check(e->roll_view.zoom == 1, "entering LFO uses standard scale");
    mouse_frame(e, x, y, false, false, false, true, 1);
    check(e->roll_view.zoom == 1, "Ctrl wheel cannot zoom LFO");
    mouse_frame(e, x, gy - 15, false, false, false, false, 1);
    check(e->roll_view.zoom == 1, "ruler wheel cannot zoom LFO");
    mouse_frame(e, 0, 0, false);
    p.event = 11;
    e->automation_cc = 11;
    p.center = 64;
    p.inverted = false;
    p.speed = .25;
    p.amount = .5;
    p.tension = -1;
    p.skew = 0;
    p.pulse = .5;
    capture_workspace_navigation(e, "agents/reports/lfo_shapes-spikes.bmp");
    p.tension = 0;
    capture_workspace_navigation(e, "agents/reports/lfo_shapes-triangle.bmp");
    p.tension = 1;
    capture_workspace_navigation(e, "agents/reports/lfo_shapes-square.bmp");
    p.tension = 0;
    p.skew = -1;
    capture_workspace_navigation(e, "agents/reports/lfo_shapes-saw-down.bmp");
    p.skew = 1;
    capture_workspace_navigation(e, "agents/reports/lfo_shapes-saw-up.bmp");
    p.skew = 0;
    p.pulse = .05;
    capture_workspace_navigation(e, "agents/reports/lfo_shapes-narrow.bmp");
    p.pulse = .95;
    capture_workspace_navigation(e, "agents/reports/lfo_shapes-wide.bmp");
    p.pulse = .5;
    auto *ed_playlist = find_window("/Playlist_");
    float px = ed_playlist->WorkRect.Min.x + e->label_width + 20;
    float py = ed_playlist->WorkRect.Min.y + ImGui::GetFrameHeight() +
               ImGui::GetStyle().ItemSpacing.y + 28 + 20 + 64;
    size_t placements = e->project.clip_count;
    int history = e->undo_count;
    mouse_frame(e, px, py, false);
    mouse_frame(e, px, py, false, false, true);
    mouse_frame(e, px, py, false, false, true, false, 0, true);
    mouse_frame(e, px, py, false, false, true);
    check(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
              e->project.clip_count == placements,
          "Shift right playlist click opens a menu without erasing");
    mouse_frame(e, 1390, 880, true);
    mouse_frame(e, 1390, 880, false);
    mouse_frame(e, 0, 0, false);
    mouse_frame(e, px, py, false);
    mouse_frame(e, px, py, false, false, false, false, 0, true);
    mouse_frame(e, px + 4 * 40, py, false, false, false, false, 0, true);
    mouse_frame(e, px + 4 * 40, py, false);
    check(e->project.clip_count == placements - 2 && e->undo_count == history + 1,
          "plain right drag erases multiple placements as one action");
}

extern "C" void test_event_clipboard_ui(Editor *e, void (*check)(bool, const char *)) {
    auto &p = e->project.patterns[0];
    p.kind = 1;
    p.mode = 0;
    p.event = -2;
    e->selected_pattern = 0;
    e->automation_cc = -2;
    e->automation_center = 60;
    e->automation_node = -1;
    e->automation_all = false;
    e->automation_browser = true;
    e->roll_height = 550;
    e->roll_view = {1, 0, 0, 0};
    dm_automation_add(&e->project, {0, 0xe0, 0, 0, 64, 0, 0, 0});
    dm_automation_add(&e->project, {2, 0xe0, 0, 0, 96, 0, 0, 0});
    dm_automation_add(&e->project, {4, 0xe0, 0, 0, 64, 0, 0, 0});
    capture_workspace_navigation(e, "agents/reports/event_clipboard-step-lines.bmp");
    auto *events = find_window("/Event Editor_");
    float gx = events->WorkRect.Min.x + 64;
    float gy =
        events->WorkRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 28;
    float h = events->WorkRect.Max.y - gy - 24;
    float y = gy + h * .5f;
    mouse_frame(e, gx + 2, y, false);
    mouse_frame(e, gx + 2, y, true);
    mouse_frame(e, gx + 2, y, false);
    check(e->edit_focus == FOCUS_EVENTS, "event canvas receives clipboard focus");
    direct_key(e, SDL_SCANCODE_A, SDL_KMOD_CTRL);
    direct_key(e, SDL_SCANCODE_C, SDL_KMOD_CTRL);
    check(e->clipboard.size > 0, "real Ctrl+C copies selected event points");
    size_t count = e->project.automation_count;
    e->cursor_beat[1] = 8;
    direct_key(e, SDL_SCANCODE_V, SDL_KMOD_CTRL);
    check(e->project.automation_count > count &&
              e->event_selection.size == 3 * sizeof(DmAutomation),
          "real Ctrl+V selects pasted event points");
    count = e->project.automation_count;
    direct_key(e, SDL_SCANCODE_D, SDL_KMOD_CTRL);
    check(e->project.automation_count > count, "real Ctrl+D duplicates selected events");
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(!e->event_selection.size && !e->automation_all && e->automation_node < 0,
          "Escape clears pasted event selection");
    p.mode = 1;
    count = e->project.automation_count;
    direct_key(e, SDL_SCANCODE_D, SDL_KMOD_CTRL);
    check(e->project.automation_count == count, "LFO mode suppresses point duplication");
    p.mode = 0;
    e->roll_view.zoom = .15f;
    e->automation_center = 60;
    capture_workspace_navigation(e, "agents/reports/event_clipboard-dense-view.bmp");
    p.mode = 1;
    p.event = 11;
    e->automation_cc = 11;
    p.speed = 1.0 / 3;
    e->project.numerator = 6;
    e->project.denominator = 8;
    e->selected_clip = 0;
    capture_workspace_navigation(e, "agents/reports/event_clipboard-six-eight.bmp");
}
#ifdef DM_TEST_RECORDING_FEEDBACK
extern "C" void recording_feedback_audio_render(DmAudio *, int);
extern "C" int recording_feedback_audio_count(DmAudio *);

static UiRect recording_feedback_event_lane() {
    float x = FLT_MAX, y = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;

    for (auto &v : roll_window()->DrawList->VtxBuffer) {
        if (v.col == 0xff121212u) {
            x = ImMin(x, v.pos.x);
            y = ImMin(y, v.pos.y);
            right = ImMax(right, v.pos.x);
            bottom = ImMax(bottom, v.pos.y);
        }
    }

    return {x, y, right - x, bottom - y};
}

static bool recording_feedback_tooltip() {
    for (auto *w : ImGui::GetCurrentContext()->Windows) {
        if (w->Active && std::strstr(w->Name, "##Tooltip")) {
            return true;
        }
    }

    return false;
}

extern "C" void test_recording_feedback_ui(Editor *e, void (*check)(bool, const char *)) {
    e->piano = false;
    e->roll_height = 480;
    e->automation_height = 180;
    e->automation_open = true;
    e->roll_view = {1, 0, (127 - 64) * 20.0f, 0};
    e->tool = 0;
    e->selected_clip = 0;
    auto &p = e->project.patterns[0];
    p.kind = 0;
    p.length = 4;
    dm_note_add(&p, {0, 1, 60, 100, 0});
    capture_workspace_navigation(e, "agents/reports/recording_feedback-piano.bmp");
    auto *ed_roll = roll_window();
    e->selected_channel = 1;
    int old_program = e->project.channels[1].program;
    int history = e->undo_count;
    ImGui::ActivateItemByID(ed_roll->GetID("Use selected channel"));
    mouse_frame(e, 0, 0, false);
    check(e->preview_program == old_program && e->project.channels[1].program == old_program &&
              e->undo_count == history,
          "Use selected channel copies only the editing preview instrument");
    auto grid = roll_origin();
    float x = grid.x + 20, y = grid.y + 4 * 20 + 10;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y, false);
    recording_feedback_audio_render(e->audio, 256);
    check(recording_feedback_audio_count(e->audio) == 0, "selecting a piano-roll note is silent");
    mouse_frame(e, x, y, false, false, false, false, 0, true);
    mouse_frame(e, x, y, false);
    recording_feedback_audio_render(e->audio, 256);
    check(p.count == 0 && recording_feedback_audio_count(e->audio) == 0,
          "right-drag note deletion is silent");
    e->automation_open = false; /* The open events section covers this part of the playlist. */
    mouse_frame(e, 0, 0, false);
    auto *ed_playlist = find_window("/Playlist_");
    double source_length = dm_clip_length(&e->project, e->project.clips[0]);
    float px = ed_playlist->WorkRect.Min.x + e->label_width + (float)source_length * 40 - 5;
    float py = ed_playlist->WorkRect.Min.y + ImGui::GetFrameHeight() +
               ImGui::GetStyle().ItemSpacing.y + 28 + 20 + 64;
    mouse_frame(e, px, py, false);
    mouse_frame(e, px, py, true);
    check(e->drag == 9, "note placement edge activates resizing");
    mouse_frame(e, px + 40, py, true);
    mouse_frame(e, px + 40, py, false);
    check(e->project.clips[0].length == source_length + 1 && p.length == source_length &&
              e->project.clips[1].length == 0,
          "mouse resize extends only the targeted placement");
    recording_feedback_audio_render(e->audio, 256);
    check(recording_feedback_audio_count(e->audio) == 0,
          "playlist selection and resizing are silent");
    e->automation_open = true;
    e->project.automation_count = 0;
    e->automation_cc = 11;
    e->context = 2;
    dm_automation_add(&e->project, {0, 0xb0, 0, 11, 64, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, {2, 0xb0, 0, 11, 64, 0, 0, 0, 0, false});
    mouse_frame(e, 0, 0, false);
    auto lane = recording_feedback_event_lane();
    x = lane.x + 80;
    y = lane.y + lane.h - 8 - 64.0f / 127 * (lane.h - 8);
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y - 20, true);
    mouse_frame(e, x, y - 20, false);
    check(e->project.automation[0].ramp && e->project.automation[0].bend > 0,
          "event-lane hollow handle curves a flat held-value connection");
    recording_feedback_audio_render(e->audio, 256);
    check(recording_feedback_audio_count(e->audio) == 0,
          "editing event curves does not audition notes");
    capture_workspace_navigation(e, "agents/reports/recording_feedback-curve-panel.bmp");
    mouse_frame(e, x, y - 20, false);
    middle_frame(e, x, y - 20, true);
    middle_frame(e, x, y - 20, false);
    check(e->project.automation[0].bend == 0 && e->project.automation[0].ramp,
          "middle click resets a converted connection to a straight ramp");
    mouse_frame(e, lane.x + 40, y, false, false, true);
    mouse_frame(e, lane.x + 40, y, true, false, true);
    mouse_frame(e, lane.x + 40, y, false, false, true);
    mouse_frame(e, 0, 0, false);
    check(e->project.automation_count == 3, "Shift left click inserts an event point");
    p.kind = 1;
    p.mode = 0;
    p.event = 11;
    p.count = 0;
    e->automation_browser = true;
    e->focus_pattern = true;
    capture_workspace_navigation(e, "agents/reports/recording_feedback-event-editor.bmp");
    auto *events = find_window("/Event Editor_");
    float gx = events->WorkRect.Min.x + 64;
    float gy =
        events->WorkRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 28;
    float h = events->WorkRect.Max.y - gy - 24;
    e->project.automation_count = 0;
    dm_automation_add(&e->project, {0, 0xb0, 0, 11, 32, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, {2, 0xb0, 0, 11, 96, 0, 0, 0, 0, false});
    mouse_frame(e, 0, 0, false);
    x = gx + 80;
    y = gy + h - 8 - 32.0f / 127 * (h - 16);
    float markerTop = FLT_MAX, markerBottom = -FLT_MAX;

    for (auto &vertex : events->DrawList->VtxBuffer) {
        if (vertex.col == 0xff6fc4f5u && std::fabs(vertex.pos.x - x) < 6) {
            markerTop = ImMin(markerTop, vertex.pos.y);
            markerBottom = ImMax(markerBottom, vertex.pos.y);
        }
    }

    if (markerTop < FLT_MAX) {
        y = (markerTop + markerBottom) * .5f;
    }

    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y - 25, true);
    mouse_frame(e, x, y - 25, false);
    check(e->project.automation[0].ramp && e->project.automation[0].bend != 0,
          "standalone Event Editor curve handle changes its outgoing curve");
    capture_workspace_navigation(e, "agents/reports/recording_feedback-event-curve.bmp");
    mouse_frame(e, gx + 160, gy + h - 8 - 96.0f / 127 * (h - 16), false);
    mouse_frame(e, gx + 160, gy + h - 8 - 96.0f / 127 * (h - 16), false);
    check(!recording_feedback_tooltip(), "event points no longer show hover tooltips");
    p.mode = 1;
    mouse_frame(e, 0, 0, false);
    events = find_window("/Event Editor_");
    x = events->WorkRect.Max.x - 476 + 98;
    y = events->WorkRect.Min.y + 23;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, false);
    check(!recording_feedback_tooltip(), "LFO knobs no longer show hover tooltips");
    p.kind = 0;
    p.mode = 0;
    p.event = -2;
    e->automation_cc = -2;
    e->automation_center = 60;
    e->automation_browser = false;
    e->roll_view = {1, 0, (127 - 64) * 20.0f, 0};
    e->project.automation_count = 0;
    dm_note_add(&p, {0, 1, 60, 100, 0});
    dm_automation_add(&e->project, {0, 0xe0, 0, 0, 64, 0, 0, 60, 0, false});
    dm_automation_add(&e->project, {2, 0xe0, 0, 0, 96, 0, 0, 60, 0, false});
    mouse_frame(e, 0, 0, false);
    grid = roll_origin();
    x = grid.x + 80;
    y = grid.y + 4 * 20 + 10;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y - 20, true);
    mouse_frame(e, x, y - 20, false);
    check(e->project.automation[0].ramp && e->project.automation[0].bend != 0 &&
              p.notes[0].length == 1,
          "piano-roll overlay handle curves pitch without resizing a note beneath it");
    capture_workspace_navigation(e, "agents/reports/recording_feedback-pitch-overlay.bmp");
    SDL_SetWindowSize(e->window, 960, 540);
    mouse_frame(e, 0, 0, false);
    mouse_frame(e, 0, 0, false);
    capture_workspace_navigation(e, "agents/reports/recording_feedback-narrow.bmp");
    SDL_SetWindowSize(e->window, 1400, 900);
    mouse_frame(e, 0, 0, false);
}

#endif

#ifdef DM_TEST_SOURCE_WORKFLOW
static UiRect source_workflow_lane() {
    float x = FLT_MAX, y = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;

    for (auto &v : roll_window()->DrawList->VtxBuffer) {
        if (v.col == 0xff121212u) {
            x = ImMin(x, v.pos.x);
            y = ImMin(y, v.pos.y);
            right = ImMax(right, v.pos.x);
            bottom = ImMax(bottom, v.pos.y);
        }
    }

    return {x, y, right - x, bottom - y};
}

static void source_workflow_scale_frame(Editor *e) {
    ui_frame();
    editor_frame(e, 1400, 900);
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_RenderPresent(e->renderer);
}

static void source_workflow_capture(Editor *e, const char *path) {
    source_workflow_scale_frame(e);
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    auto *surface = SDL_RenderReadPixels(e->renderer, nullptr);

    if (surface) {
        SDL_SaveBMP(surface, path);
        SDL_DestroySurface(surface);
    }
}

extern "C" void test_source_workflow_ui(Editor *e, void (*check)(bool, const char *)) {
    e->automation_open = true;
    e->roll_height = 480;
    e->automation_height = 180;
    e->roll_view = {1, 0, (127 - 64) * 20.0f, 0};
    e->automation_cc = 11;
    e->tool = 0;
    auto &p = e->project.patterns[0];
    p.kind = 0;
    p.mode = 0;
    p.count = 0;
    dm_note_add(&p, {0, 1, 60, 100, 0});
    e->project.automation_count = 0;
    dm_automation_add(&e->project, {.5, 0xb0, 0, 11, 50, 0, 0, 0, 0, true});
    dm_automation_add(&e->project, {1.5, 0xb0, 0, 11, 70, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, {3, 0xb0, 0, 11, 90, 0, 0, 0, 0, false});
    mouse_frame(e, 0, 0, false);
    auto lane = source_workflow_lane();
    float x = lane.x + 15, y = lane.y + 10;
    mouse_frame(e, x, y, false, false, false, true);
    mouse_frame(e, x, y, true, false, false, true);
    mouse_frame(e, lane.x + 145, lane.y + lane.h - 5, true, false, false, true);
    mouse_frame(e, lane.x + 145, lane.y + lane.h - 5, false, false, false, true);
    check(e->project.automation_count == 3 && e->event_selection.size == 2 * sizeof(DmAutomation) &&
              !p.notes[0].selected,
          "Ctrl marquee selects lower event-lane points without inserting or selecting notes");

    for (int click = 0; click < 2; ++click) {
        mouse_frame(e, lane.x + 100, lane.y + 50, false);
        mouse_frame(e, lane.x + 100, lane.y + 50, true);
        mouse_frame(e, lane.x + 100, lane.y + 50, false);
        check((click ? e->project.automation_count >= 4u : e->project.automation_count == 3u) &&
                  (click || !e->event_selection.size),
              click ? "left click inserts a connected point"
                    : "left click with a selection only deselects");
    }

    e->tool = 1;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, lane.x + 145, lane.y + lane.h - 5, true);
    mouse_frame(e, lane.x + 145, lane.y + lane.h - 5, false);
    check(e->event_selection.size == 3 * sizeof(DmAutomation),
          "manual marquee tool selects event points");
    capture_workspace_navigation(e, "agents/reports/source_workflow-event-marquee.bmp");
    e->tool = 0;
    e->event_selection.size = 0;
    e->automation_node = -1;
    float vy = lane.y + lane.h - 8 - 64.0f / 127 * (lane.h - 8);
    mouse_frame(e, lane.x + 240, vy, false);
    mouse_frame(e, lane.x + 240, vy, true);
    source_workflow_capture(e, "agents/reports/source_workflow-value-tip.bmp");
    mouse_frame(e, lane.x + 240, vy, false);
    e->project.automation_count = 0;
    dm_automation_add(&e->project, {.5, 0xb0, 0, 11, 50, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, {1.5, 0xb0, 0, 11, 70, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, {3, 0xb0, 0, 11, 90, 0, 0, 0, 0, false});
    p.kind = 1;
    p.event = 11;
    e->automation_browser = true;
    e->focus_pattern = true;
    mouse_frame(e, 0, 0, false);
    auto *events = find_window("/Event Editor_");
    float gx = events->WorkRect.Min.x + 64;
    float gy =
        events->WorkRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 28;
    float h = events->WorkRect.Max.y - gy - 24;
    size_t count = e->project.automation_count;
    mouse_frame(e, gx + 10, gy + 5, false, false, false, true);
    mouse_frame(e, gx + 10, gy + 5, true, false, false, true);
    mouse_frame(e, gx + 145, gy + h - 3, true, false, false, true);
    mouse_frame(e, gx + 145, gy + h - 3, false, false, false, true);
    check(e->project.automation_count == count &&
              e->event_selection.size == 2 * sizeof(DmAutomation),
          "standalone Point editor supports Ctrl marquee");
    e->event_selection.size = 0;
    p.kind = 0;
    e->automation_browser = false;
    e->focus_pattern = false;
    e->roll_view.scroll_x = 80000;
    e->playlist_view.scroll_x = 120000;
    mouse_frame(e, 0, 0, false);
    check(e->roll_view.scroll_x == 80000 && e->playlist_view.scroll_x == 120000,
          "rendering does not clamp scrolling to song or pattern duration");
    e->roll_view.scroll_x = e->playlist_view.scroll_x = 0;
    e->preferences = true;
    e->config = true;
    mouse_frame(e, 0, 0, false);
    mouse_frame(e, 0, 0, false);
    auto *prefs = ImGui::FindWindowByName("Preferences");
    ImGui::FocusWindow(prefs);
    mouse_frame(e, 0, 0, false);
    ImGui::ActivateItemByID(prefs->GetID("MIDI output"));
    mouse_frame(e, 0, 0, false);
    check(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId), "preference dropdown opens");
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(e->preferences && e->config && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId),
          "Escape closes top option before floating windows");
    mouse_frame(e, 20, 130, false);
    mouse_frame(e, 20, 130, true);
    mouse_frame(e, 20, 130, false);
    e->edit_focus = FOCUS_PLAYLIST;
    e->context = 1;
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(!e->preferences && e->config,
          "topmost unfocused floating window closes before workspace selection");
    mouse_frame(e, 0, 0, false);
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(!e->config, "next Escape closes remaining floating window");
    mouse_frame(e, 0, 0, false);
    e->pattern_highlight = true;
    e->project.clips[0].selected = 1;
    e->channel_selected = true;
    e->roll = true;
    e->roll_auto_hidden = false;
    e->edit_focus = FOCUS_PLAYLIST;
    e->context = 1;
    e->loop_enabled[0] = e->loop_enabled[1] = false;
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(!e->pattern_highlight && !e->project.clips[0].selected && e->roll && !e->roll_auto_hidden,
          "playlist Escape first deselects patterns while editor stays visible");
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(!e->channel_selected && e->roll, "next Escape deselects the channel");
    direct_key(e, SDL_SCANCODE_ESCAPE);
    check(!e->roll, "next Escape closes the pattern editor");
    e->roll = true;
    e->channel_selected = true;
    uint8_t tempo[] = {0xff, 0x51, 3, 7, 0xa1, 0x20};
    DmBytes ed_message = {tempo, 6, 6};
    dm_event_add(&e->project, 0, 0, 0, &ed_message);
    e->midi_events = true;
    e->midi_event_index = -1;
    mouse_frame(e, 0, 0, false);
    mouse_frame(e, 0, 0, false);
    auto *list = find_window("/MIDI event list_");
    auto *details = find_window("/MIDI event details_");
    check(list && details && std::fabs(list->Size.y - details->Size.y) < .1f,
          "MIDI browser has full-height side-by-side panels");
    x = list->WorkRect.Min.x + 30;
    y = list->WorkRect.Min.y + 10;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y, false);
    check(e->midi_event_index >= 0 && e->event_kind == 0,
          "MIDI browser selects hidden initial tempo");
    int tempo_us = (int)std::llround(60000000.0 / 155);
    e->midi_draft.data[3] = tempo_us >> 16;
    e->midi_draft.data[4] = tempo_us >> 8;
    e->midi_draft.data[5] = tempo_us;
    ImGui::ActivateItemByID(details->GetID("Apply event"));
    mouse_frame(e, 0, 0, false);
    check(std::fabs(e->project.bpm - 155) < .001,
          "MIDI browser Apply updates effective initial tempo");
    float split = e->midi_split;
    x = list->Pos.x + list->Size.x + 12;
    y = list->Pos.y + 100;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 45, y, true);
    mouse_frame(e, x + 45, y, false);
    check(e->midi_split > split, "MIDI browser panel divider drags horizontally");
    x = list->WorkRect.Min.x + 30;
    y = list->WorkRect.Min.y + 10;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y, false);
    capture_workspace_navigation(e, "agents/reports/source_workflow-midi-events.bmp");
    e->midi_events = false;
    e->focus_pattern = true;
    e->roll_view.scroll_x = 0;
    e->selected_channel = 1;
    e->preview_source_channel = -1;
    e->preview_program = 0;
    ui_set_scale(1.25f);
    source_workflow_capture(e, "agents/reports/source_workflow-large.bmp");
    check(std::fabs(ui_display().x - 1120) < 1, "Large scales the entire logical interface");
    auto *ed_roll = roll_window();
    x = ed_roll->WorkRect.Max.x - 90;
    y = ed_roll->WorkRect.Min.y + 16;
    SDL_Event motion = {};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.windowID = SDL_GetWindowID(e->window);
    motion.motion.x = x * 1.25f;
    motion.motion.y = y * 1.25f;
    ui_event(&motion);
    source_workflow_scale_frame(e);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    source_workflow_scale_frame(e);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    source_workflow_scale_frame(e);
    check(e->preview_program == e->project.channels[1].program && e->preview_override,
          "scaled mouse hit target activates Use selected channel");
    ui_set_scale(.85f);
    source_workflow_capture(e, "agents/reports/source_workflow-small.bmp");
    check(std::fabs(ui_display().x - 1400 / .85f) < 1, "Small scales the entire interface");
    ui_set_scale(1);
    source_workflow_capture(e, "agents/reports/source_workflow-medium.bmp");
    e->drop_active = true;
    e->drop_handled = false;
    e->drop_position = {20, 20};
    source_workflow_capture(e, "agents/reports/source_workflow-drop-project.bmp");
    e->preferences = true;
    source_workflow_scale_frame(e);
    e->drop_position = {e->preferences_rect.x + 40, e->preferences_rect.y + 40};
    source_workflow_capture(e, "agents/reports/source_workflow-drop-soundfont.bmp");
    check(e->preferences_rect.w > 0 && e->drop_active,
          "SoundFont drop overlay uses floating preference bounds");
    e->drop_active = false;
    source_workflow_capture(e, "agents/reports/source_workflow-preferences.bmp");
    e->preferences = false;
}
#endif

extern "C" void test_live_edit_ui(Editor *e, void (*check)(bool, const char *)) {
    e->roll_height = 480;
    e->automation_open = false;
    e->focus_pattern = false;
    e->roll_view = {1, 0, (127 - 64) * 20.0f, 0};
    e->tool = 0;
    auto &p = e->project.patterns[0];
    p.count = 0;
    p.length = 4;
    dm_note_add(&p, {0, 2, 60, 100, 0});
    mouse_frame(e, 0, 0, false);
    auto grid = roll_origin();
    float y = grid.y + 4 * 20 + 10;
    mouse_frame(e, grid.x + 2, y, false);
    mouse_frame(e, grid.x + 2, y, true);
    mouse_frame(e, grid.x + 22, y, true);
    check(e->drag_edge == -1 && !e->edit_feedback, "left-edge resize produces no audition");
    mouse_frame(e, grid.x + 22, y, false);
    mouse_frame(e, grid.x + 158, y, false);
    mouse_frame(e, grid.x + 158, y, true);
    mouse_frame(e, grid.x + 198, y, true);
    check(e->drag_edge == 1 && !e->edit_feedback, "right-edge resize produces no audition");
    mouse_frame(e, grid.x + 198, y, false);
    mouse_frame(e, grid.x + 80, y, false);
    mouse_frame(e, grid.x + 80, y, true);
    check(e->edit_feedback && e->drag_edge == 0, "note body click auditions independently");
    mouse_frame(e, grid.x + 80, y - 40, true);
    check(e->edit_feedback && p.notes[0].key == 62,
          "vertical note drag keeps pitch audition active");
    mouse_frame(e, grid.x + 80, y - 40, false);
    check(!e->edit_feedback, "mouse release ends edit audition");
    mouse_frame(e, grid.x - 32, y, false);
    mouse_frame(e, grid.x - 32, y, true);
    check(e->held[SDL_SCANCODE_UNKNOWN] == 60, "left-hand piano keeps existing key behavior");
    mouse_frame(e, grid.x - 32, y, false);
    check(e->held[SDL_SCANCODE_UNKNOWN] == -1, "left-hand piano key releases normally");
    p.notes[0].key = 60;
    dm_automation_add(&e->project, {0.5, 0xe0, 0, 0, 96, 0, 0, 60});
    mouse_frame(e, grid.x + 40, y - 20, false, true);
    mouse_frame(e, grid.x + 40, y - 20, true, true);
    mouse_frame(e, grid.x + 40, y - 40, true, true);
    check(e->drag == 7 && e->edit_feedback, "pitch-bend overlay point drag auditions its pitch");
    mouse_frame(e, grid.x + 40, y - 40, false);
    check(!e->edit_feedback, "pitch-bend audition ends on release");
    size_t before = e->project.patterns[0].count;
    int history = e->undo_count;
    float nx = grid.x + 280, ny = y + 20;
    mouse_frame(e, nx, ny, false);
    mouse_frame(e, nx, ny, true);
    check(e->project.patterns[0].count == before + 1 && e->drag == 2 && e->drag_edge == 0 &&
              e->edit_feedback,
          "insertion immediately holds preview and starts body drag, even near the new edge");
    mouse_frame(e, nx, ny, true);
    check(e->edit_feedback && e->project.patterns[0].count == before + 1,
          "holding insertion sustains feedback without painting duplicate notes");
    mouse_frame(e, nx + 40, ny - 40, true);
    auto moved = e->project.patterns[0].notes[before];
    check(std::fabs(moved.beat - 4) < .001 && moved.key == 61 &&
              std::fabs(moved.length - .25) < .001,
          "same insertion gesture moves time and pitch instead of resizing");
    mouse_frame(e, nx + 40, ny - 40, false);
    check(!e->edit_feedback && e->drag == 0 && e->undo_count == history + 1,
          "release ends insert drag and keeps one undo entry");
    direct_key(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    check(e->project.patterns[0].count == before, "one undo removes the inserted and moved note");
    direct_key(e, SDL_SCANCODE_Y, SDL_KMOD_CTRL);
    check(e->project.patterns[0].count == before + 1 &&
              e->project.patterns[0].notes[before].key == 61,
          "redo restores the final dragged position");
    mouse_frame(e, grid.x + 10, y + 15, false, false, false, true);
    mouse_frame(e, grid.x + 10, y + 15, true, false, false, true);
    mouse_frame(e, grid.x + 240, y - 15, true, false, false, true);
    check(e->marquee && !e->drag && !e->edit_feedback,
          "Ctrl drag selects notes without inserting, dragging or auditioning them");
    mouse_frame(e, grid.x + 240, y - 15, false);
    check(!e->marquee && e->project.patterns[0].count == before + 1,
          "Ctrl marquee is temporary and preserves note count");
}

#ifdef DM_TEST_PATTERN_WORKFLOW
extern "C" void recording_feedback_audio_render(DmAudio *, int);
extern "C" int recording_feedback_audio_preset(DmAudio *, int, bool);

static UiRect pattern_workflow_lane() {
    float x = FLT_MAX, y = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;

    for (auto &v : roll_window()->DrawList->VtxBuffer) {
        if (v.col == 0xff121212u) {
            x = ImMin(x, v.pos.x);
            y = ImMin(y, v.pos.y);
            right = ImMax(right, v.pos.x);
            bottom = ImMax(bottom, v.pos.y);
        }
    }

    return {x, y, right - x, bottom - y};
}

extern "C" void test_pattern_workflow_ui(Editor *e, void (*check)(bool, const char *)) {
    e->piano = true;
    e->roll_height = 500;
    e->automation_open = false;
    e->focus_pattern = false;
    e->roll_view = {1, 0, (127 - 64) * 20.0f, 0};
    auto &p = e->project.patterns[0];
    p.count = 0;
    p.kind = 0;
    mouse_frame(e, 0, 0, false);
    direct_key(e, SDL_SCANCODE_5);
    check(e->tool == 4 && e->held[SDL_SCANCODE_5] < 0,
          "5 selects Brush while keyboard piano is enabled");
    auto grid = roll_origin();
    float x = grid.x + 20, y = grid.y + 90;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 120, y - 40, true);
    mouse_frame(e, x + 120, y - 40, false);
    check(p.count >= 7 && e->drag == 0, "manual Brush paints a gap-free quarter-beat stroke");
    p.count = 0;
    e->tool = 0;
    mouse_frame(e, x, y, false, false, true, true);
    mouse_frame(e, x, y, true, false, true, true);
    mouse_frame(e, x + 120, y - 60, true, false, true, true);
    mouse_frame(e, x + 120, y - 60, false);
    check(p.count == 0, "Ctrl+Shift with Note tool no longer activates temporary Brush");
    e->tool = 4;
    mouse_frame(e, x, y, false, false, false, true);
    mouse_frame(e, x, y, true, false, false, true);
    mouse_frame(e, x + 120, y - 60, true, false, false, true);
    mouse_frame(e, x + 120, y - 60, false);
    bool locked = p.count >= 7;

    for (size_t i = 0; i < p.count; ++i) {
        locked &= p.notes[i].key == 60;
    }

    check(locked, "explicit Brush with Ctrl has priority over marquee and locks starting pitch");
    capture_workspace_navigation(e, "agents/reports/pattern_workflow-brush.bmp");
    p.count = 0;
    e->tool = 0;
    dm_note_add(&p, {.25, 1, 60, 50, 1});
    dm_note_add(&p, {1.5, 1, 64, 75, 1});
    e->automation_open = true;
    e->automation_height = 180;
    e->automation_cc = -3;
    e->piano = false;
    mouse_frame(e, 0, 0, false);
    grid = roll_origin();
    x = grid.x + 40;
    y = grid.y + 90;
    float scroll = e->roll_view.scroll_x;
    mouse_frame(e, x, y, false, true);
    mouse_frame(e, x, y, false, true, false, false, -1);
    check(p.notes[0].velocity == 45 && p.notes[1].velocity == 70 && e->roll_view.scroll_x == scroll,
          "real Alt-wheel changes selected note velocities without scrolling");
    mouse_frame(e, 0, 0, false);
    auto lane = pattern_workflow_lane();
    check(lane.w > 100 && lane.h > 30, "velocity lane fills existing event panel");
    x = lane.x + 20;
    y = lane.y + lane.h - 8 - 45 * (lane.h - 8) / 127;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x, y - 25 * (lane.h - 8) / 127, true);
    mouse_frame(e, x, y - 25 * (lane.h - 8) / 127, false);
    check(p.notes[0].velocity == 70 && p.notes[1].velocity == 95,
          "velocity stem drag snaps anchor to tens and moves group relatively");
    y = lane.y + lane.h - 8 - 70 * (lane.h - 8) / 127;
    mouse_frame(e, x, y, false, true);
    mouse_frame(e, x, y, true, true);
    mouse_frame(e, x, y - 3, true, true);
    mouse_frame(e, x, y - 3, false);
    check(p.notes[0].velocity > 70 && p.notes[0].velocity < 80 &&
              p.notes[1].velocity - p.notes[0].velocity == 25,
          "Alt velocity stem drag uses individual integer values");
    p.notes[0].selected = p.notes[1].selected = 0;
    direct_key(e, SDL_SCANCODE_A, SDL_KMOD_CTRL);
    check(p.notes[0].selected && p.notes[1].selected, "real Ctrl+A selects all velocity stems");
    capture_workspace_navigation(e, "agents/reports/pattern_workflow-velocity.bmp");
    e->automation_cc = 11;
    e->project.automation_count = 0;
    e->automation_node = -1;
    e->event_selection.size = 0;
    dm_automation_add(&e->project, {.5, 0xb0, 0, 11, 30, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, {1.5, 0xb0, 0, 11, 60, 0, 0, 0, 0, false});
    mouse_frame(e, 0, 0, false);
    lane = pattern_workflow_lane();
    e->edit_focus = FOCUS_EVENTS;
    direct_key(e, SDL_SCANCODE_A, SDL_KMOD_CTRL);
    x = lane.x + 120;
    y = lane.y + lane.h - 8 - 60 * (lane.h - 8) / 127;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 40, y - 10, true);
    mouse_frame(e, x + 40, y - 10, false);
    check(std::fabs(e->project.automation[0].beat - 1) < .001 &&
              std::fabs(e->project.automation[1].beat - 2) < .001,
          "real selected-point drag moves the group from the clicked anchor");
    check(e->project.automation[1].b - e->project.automation[0].b == 30,
          "real event drag preserves relative MIDI values");
    capture_workspace_navigation(e, "agents/reports/pattern_workflow-event-group.bmp");
    p.kind = 1;
    p.mode = 0;
    p.event = 11;
    e->automation_browser = true;
    e->focus_pattern = true;
    mouse_frame(e, 0, 0, false);
    auto *events = find_window("/Event Editor_");
    check(events != nullptr, "standalone Point editor opens for group validation");
    float gx = events->WorkRect.Min.x + 64;
    float gy =
        events->WorkRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 28;
    float h = events->WorkRect.Max.y - gy - 24;
    e->edit_focus = FOCUS_EVENTS;
    direct_key(e, SDL_SCANCODE_A, SDL_KMOD_CTRL);
    x = gx + 160;
    y = gy + h - 8 - e->project.automation[1].b * (h - 16) / 127.0f;
    mouse_frame(e, x, y, false);
    mouse_frame(e, x, y, true);
    mouse_frame(e, x + 40, y, true);
    mouse_frame(e, x + 40, y, false);
    check(std::fabs(e->project.automation[0].beat - 1.5) < .001 &&
              std::fabs(e->project.automation[1].beat - 2.5) < .001,
          "standalone Point editor mouse drag moves selected points together");
    capture_workspace_navigation(e, "agents/reports/pattern_workflow-standalone-group.bmp");
    p.kind = 0;
    e->automation_browser = false;
    e->focus_pattern = true;
    mouse_frame(e, 0, 0, false);
    e->instrument_channel = 2;
    e->project.channels[2].enabled = true;
    e->project.channels[2].program = 40;
    mouse_frame(e, 0, 0, false);
    direct_key(e, SDL_SCANCODE_Z);
    recording_feedback_audio_render(e->audio, 256);
    check(
        e->held[SDL_SCANCODE_Z] >= 0 && e->held_channels[SDL_SCANCODE_Z] == 2 &&
            recording_feedback_audio_preset(e->audio, 2, true) == 40,
        "topmost Instrument allows keyboard piano with its selected preset even when Keys is off");
    SDL_Event up = {};
    up.type = SDL_EVENT_KEY_UP;
    up.key.scancode = SDL_SCANCODE_Z;
    editor_event(e, &up);
    recording_feedback_audio_render(e->audio, 256);
    check(e->held[SDL_SCANCODE_Z] < 0, "instrument piano key releases through normal key-up");
    capture_workspace_navigation(e, "agents/reports/pattern_workflow-instrument.bmp");
    mouse_frame(e, roll_window()->Pos.x + 20, roll_window()->Pos.y + 20, false);
    direct_key(e, SDL_SCANCODE_RETURN);
    mouse_frame(e, 0, 0, false);
    check(e->instrument_channel < 0,
          "Enter accepts topmost Instrument independently of workspace focus");
}
#endif
