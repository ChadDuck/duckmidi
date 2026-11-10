extern "C" {
#include "editor/internal.h"
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

    ImGui::ActivateItemByID(id);
    frame(e);
}
