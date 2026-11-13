extern "C" {
#include "editor/internal.h"
#include "text/en.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cassert>

static void preferences_frame(Editor *e) {
    ui_frame();
    ed_preferences(e);
    ui_render(e->renderer);
}

extern "C" void test_midi_binding_controls(Editor *e, EdMidiTarget target) {
    e->preferences = true;
    preferences_frame(e);
    preferences_frame(e);
    auto *window = ImGui::FindWindowByName(DM_TEXT_PREFERENCES);
    IM_ASSERT(window);
    window->StateStorage.SetInt(window->GetID(DM_TEXT_MIDI_CONFIGURATION), 1);
    window->StateStorage.SetInt(window->GetID(DM_TEXT_MIDI_BIND_CONTROLS), 1);
    window->StateStorage.SetInt(window->GetID(DM_TEXT_MIDI_BIND_EVENTS), 1);
    preferences_frame(e);
    char id[48];
    ed_midi_target_id(target, id, sizeof(id));
    auto row = window->GetID((int)SDL_crc32(0, id, strlen(id)));
    auto learn = ImHashStr(DM_TEXT_MIDI_BIND_LEARN_SHORT, 0, row);

    const float positions[] = {0.5f, 1.0f, 0.0f};

    for (float scroll : positions) {
        ImGui::SetScrollY(window, window->ScrollMax.y * scroll);
        preferences_frame(e);
        ImGui::ActivateItemByID(learn);
        preferences_frame(e);

        if (ed_midi_binding_learning(e, target)) {
            break;
        }
    }

    e->preferences = false;
}

static void mixer_frame(Editor *e) {
    bool open = true;
    ui_frame();

    UiDialog dialog = {};
    dialog.width = 640;
    dialog.height = 400;
    dialog.size = UI_RESIZABLE;

    if (ui_dialog("Mixer fixture", &open, dialog)) {
        ed_mixer(e, 600, 300);
    }

    ui_dialog_end();
    ui_render(e->renderer);
}

extern "C" void test_midi_mixer_learn(Editor *e) {
    mixer_frame(e);
    mixer_frame(e);
    ImGuiWindow *strip = nullptr;

    for (auto *window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && strstr(window->Name, "/port strip_") &&
            (!strip || window->Pos.x < strip->Pos.x)) {
            strip = window;
        }
    }

    assert(strip);
    ImVec2 origin = strip->DC.CursorStartPos;
    float width = strip->WorkRect.Max.x - origin.x;
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(origin.x + (width - 34) * .5f + 12, origin.y + 39);
    io.AddMouseButtonEvent(1, true);
    mixer_frame(e);
    io.AddMouseButtonEvent(1, false);
    mixer_frame(e);
    auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
    assert(popups.Size && popups.back().Window);
    ImGui::ActivateItemByID(popups.back().Window->GetID(DM_TEXT_MIDI_BIND_LEARN));
    mixer_frame(e);
}

extern "C" void test_midi_binding_clear_all(Editor *e) {
    size_t original = ed_midi_binding_count(e);
    assert(original);
    e->preferences = true;
    preferences_frame(e);
    preferences_frame(e);
    auto *window = ImGui::FindWindowByName(DM_TEXT_PREFERENCES);
    assert(window);
    window->StateStorage.SetInt(window->GetID(DM_TEXT_MIDI_BIND_EVENTS), 0);
    window->StateStorage.SetInt(window->GetID(DM_TEXT_MIDI_BIND_CONTROLS), 1);
    ImGui::SetScrollY(window, window->ScrollMax.y);
    preferences_frame(e);
    auto confirm = [e, window](const char *choice) {
        ImGui::ActivateItemByID(window->GetID(DM_TEXT_MIDI_BIND_CLEAR_ALL));
        preferences_frame(e);
        assert(e->midi_clear_bindings && ed_midi_binding_count(e));
        auto *modal = ImGui::FindWindowByName(DM_TEXT_MIDI_BIND_CLEAR_TITLE);
        assert(modal);
        ImGui::ActivateItemByID(modal->GetID(choice));
        preferences_frame(e);
        assert(!e->midi_clear_bindings);
    };
    confirm(DM_TEXT_CANCEL);
    assert(ed_midi_binding_count(e) == original);
    confirm(DM_TEXT_MIDI_BIND_CLEAR_ALL);
    e->preferences = false;
}
