/* Drives the port controls: the mixer's grouped port strips and the note colour popup. */
#include "test_ui.h"
#include <cassert>

/* Each port of a channel has its own strip; typing a volume applies on Enter, not on Escape. */
extern "C" void test_port_strips_ui(Editor *e) {
    frame(e);
    frame(e);
    ImGuiWindow *group = nullptr, *strip = nullptr;
    int children = 0;

    for (auto *window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && std::strstr(window->Name, "/channel group_") &&
            (!group || window->Pos.x < group->Pos.x)) {
            group = window;
        }
    }

    assert(group && group->Size.x >= 120);

    for (auto *window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && window->ParentWindow == group &&
            std::strstr(window->Name, "/port strip_")) {
            children++;

            if (!strip || window->Pos.x < strip->Pos.x) {
                strip = window;
            }
        }
    }

    assert(children == 2 && strip);
    ImGui::ActivateItemByID(strip->GetID("70"));
    frame(e);
    frame(e);
    assert(ImGui::GetCurrentContext()->OpenPopupStack.Size);
    replace(e, "89");
    assert(dm_port_mix(&e->project, 0, 0).volume == 70);
    ed_shortcut(e, SDL_SCANCODE_ESCAPE, (SDL_Keymod)0, false);
    frame(e);
    assert(dm_port_mix(&e->project, 0, 0).volume == 70);
    ImGui::ActivateItemByID(strip->GetID("70"));
    frame(e);
    frame(e);
    replace(e, "89");
    key(e, ImGuiKey_Enter);
    assert(dm_port_mix(&e->project, 0, 0).volume == 89 &&
           dm_port_mix(&e->project, 1, 0).volume == 35);
    capture(e, "agents/reports/ports-mixer-strips.bmp");
}

/* Alt + C opens the colour popup; a swatch moves the selected notes to that port, and the port
   buttons show only while the channel uses more than one port. */
extern "C" void test_note_color_popup_ui(Editor *e) {
    e->edit_focus = FOCUS_NOTES;
    e->selected_pattern = e->pattern_channel = 0;
    e->project.patterns[0].notes[0].selected = 1;
    frame(e);
    ed_shortcut(e, SDL_SCANCODE_C, SDL_KMOD_ALT, false);
    assert(e->note_color_popup);
    frame(e);
    frame(e);
    auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
    assert(stack.Size);
    auto *popup = stack.back().Window;
    assert(popup && popup->Active);
    float left = FLT_MAX, top = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;

    for (const auto &vertex : popup->DrawList->VtxBuffer) {
        if (vertex.col == ed_port_color(e, 2)) {
            left = ImMin(left, vertex.pos.x);
            right = ImMax(right, vertex.pos.x);
            top = ImMin(top, vertex.pos.y);
            bottom = ImMax(bottom, vertex.pos.y);
        }
    }

    assert(left < right && top < bottom);
    float px = (left + right) / 2, py = (top + bottom) / 2;
    capture(e, "agents/reports/ports-note-colors.bmp");
    frame(e, px, py);
    frame(e, px, py, true);
    frame(e, px, py, false);
    assert(e->project.patterns[0].notes[0].port == 2 && ed_port_buttons_width(e) > 0);
    int another = dm_pattern_add(&e->project, "Another pattern");
    assert(another >= 0 && dm_clip_add(&e->project, (DmClip){.pattern = another, .channel = 0}));
    e->selected_pattern = another;
    assert(ed_port_buttons_width(e) > 0);
    e->selected_pattern = 0;

    for (size_t i = 0; i < e->project.patterns[0].count; i++) {
        e->project.patterns[0].notes[i].selected = 1;
    }

    assert(ed_color_notes(e, 0) && ed_port_buttons_width(e) == 0);
    ed_undo(e, false);
    e->note_ports[0] = 0;
}
