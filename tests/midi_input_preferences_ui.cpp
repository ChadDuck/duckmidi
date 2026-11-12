extern "C" {
#include "editor/internal.h"
#include "text/en.h"
}
#include "imgui.h"
#include "imgui_internal.h"

static void frame(Editor *e) {
    ui_frame();
    ed_preferences(e);
    ui_render(e->renderer);
}

extern "C" void test_midi_input_preferences_ui(Editor *e, const char *label) {
    frame(e);

    if (!label) {
        return;
    }

    auto *preferences = ImGui::FindWindowByName("Preferences");
    IM_ASSERT(preferences);
    ImGuiID id = preferences->GetID(label);

    if (!strncmp(label, "Fixture ", 8)) {
        int device = (int)SDL_crc32(0, label, strlen(label));
        id = ImHashStr("##midi-input-device", 0, preferences->GetID(device));
    }

    if (!strcmp(label, DM_TEXT_MIDI_CONFIGURATION)) {
        // The initial parent is closed, with no child state until it is opened.
        IM_ASSERT(!e->input_preferences_open);
        IM_ASSERT(!preferences->StateStorage.GetInt(id, 0));
        IM_ASSERT(preferences->StateStorage.GetInt(preferences->GetID(DM_TEXT_MIDI_INPUT), -1) ==
                  -1);
        IM_ASSERT(preferences->StateStorage.GetInt(preferences->GetID(DM_TEXT_MIDI_BIND_CONTROLS),
                                                   -1) == -1);
        IM_ASSERT(preferences->StateStorage.GetInt(preferences->GetID(DM_TEXT_MIDI_BIND_EVENTS),
                                                   -1) == -1);
    }

    ImGui::ActivateItemByID(id);
    frame(e);

    if (!strcmp(label, DM_TEXT_MIDI_CONFIGURATION)) {
        IM_ASSERT(e->input_preferences_open);
        IM_ASSERT(preferences->StateStorage.GetInt(id, 0) == 1);
        IM_ASSERT(
            !preferences->StateStorage.GetInt(preferences->GetID(DM_TEXT_MIDI_BIND_LIMITED), 1));
    }
}
