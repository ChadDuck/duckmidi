/* Drives the floating windows: they own the clicks that land on them, open at the size their
   contents need and close on Enter where they are forms. */
#include "test_ui.h"
#include <cassert>

static void covered_click(Editor *e, bool *flag, const char *title, float x, float y) {
    *flag = true;
    frame(e);
    frame(e); /* A window is placed once its contents are measured. */
    frame(e);
    auto *w = ImGui::FindWindowByName(title);
    assert(w && w->Active);
    ImGui::SetWindowPos(w, {x - 70, y - 60});
    frame(e, x, y);
    e->selected_channel = 3;
    e->instrument_channel = -1;
    frame(e, x, y, true);
    frame(e, x, y, false);
    assert(e->selected_channel == 3 && e->instrument_channel == -1);

    if (ImGui::GetCurrentContext()->OpenPopupStack.Size) {
        ImGui::ClosePopupToLevel(0, true);
    }

    *flag = false;
    frame(e);
}

/* A click on a window that covers the playlist reaches the window, not the row behind it. */
static void covered_clicks(Editor *e) {
    assert(e->sidebar == 465 && e->midi_split == 0);
    e->focus_pattern = true;
    frame(e);
    frame(e);
    auto *playlist = find_window("/Playlist_");
    assert(playlist);
    float x = playlist->WorkRect.Min.x + 30, y = playlist->WorkRect.Min.y + 160;
    e->selected_channel = 3;
    frame(e, x, y);
    frame(e, x, y, true);
    frame(e, x, y, false);
    assert(e->selected_channel != 3 || e->instrument_channel >= 0);
    e->instrument_channel = -1;
    frame(e);
    covered_click(e, &e->config, "Project settings", x, y);
    covered_click(e, &e->preferences, "Preferences", x, y);
    covered_click(e, &e->shortcuts, "Keyboard shortcuts", x, y);
}

/* Generate Patterns asks its question in a small window and grows only to show the preview. */
static void generate_window(Editor *e) {
    e->generate_window = true;
    frame(e);
    frame(e);
    auto *window = ImGui::FindWindowByName("Generate Patterns");
    assert(window && window->Active && window->Size.x <= 560 && window->Size.y <= 230);
    capture(e, "agents/reports/floating-windows-generate-small.bmp");
    ed_generate_start(e);
    uint64_t start = SDL_GetTicks();

    while (ed_generate_busy(e) && SDL_GetTicks() - start < 10000) {
        ed_generate_poll(e);
        SDL_Delay(1);
    }

    assert(ed_generate_preview(e));
    frame(e);
    frame(e);
    assert(window->Size.x >= 900 && window->Size.y >= 600);
    capture(e, "agents/reports/floating-windows-generate-preview.bmp");
    e->generate_window = false;
    ed_generate_poll(e);
    frame(e);
}

/* The MIDI event list is wide enough to read and the shortcut list never scrolls sideways. */
static void list_widths(Editor *e) {
    e->midi_events = true;
    frame(e);
    frame(e);
    auto *list = find_window("/MIDI event list_");
    assert(list && list->Size.x >= 450);
    e->midi_events = false;
    frame(e);
    e->shortcuts = true;
    frame(e);
    frame(e);
    auto *shortcuts = ImGui::FindWindowByName("Keyboard shortcuts");
    assert(shortcuts);
    auto *rows = find_window("/Shortcut list_");
    assert(rows && rows->ScrollMax.x == 0);
    capture(e, "agents/reports/floating-windows-shortcuts.bmp");
    e->shortcuts = false;
    frame(e);
}

/* In a narrow window the Recent submenu opens beside the File menu instead of over it. */
static void recent_submenu(Editor *e) {
    e->recent_count = 10;

    for (int i = 0; i < 10; i++) {
        snprintf(e->recent[i], DM_PATH,
                 "C:/Generated fixtures/Long directory name/very-long-file-name-number-%d.mid", i);
    }

    SDL_SetWindowSize(e->window, 640, 900);
    frame(e);
    frame(e, 18, 12);
    frame(e, 18, 12, true);
    frame(e, 18, 12, false);
    frame(e);
    auto &menus = ImGui::GetCurrentContext()->OpenPopupStack;
    assert(menus.Size);
    auto *file = menus[0].Window;
    assert(file);
    ImGuiID recent = file->GetID("Recent");
    float mx = file->WorkRect.Min.x + 20, my = 0;

    for (float probe = file->WorkRect.Min.y; probe < file->WorkRect.Max.y; probe += 3) {
        frame(e, mx, probe);

        if (ImGui::GetCurrentContext()->HoveredId == recent) {
            my = probe;
            break;
        }
    }

    assert(my);
    frame(e, mx, my, true);
    frame(e, mx, my, false);
    frame(e, mx, my);
    frame(e, mx, my);
    assert(menus.Size >= 2);
    auto *submenu = menus.back().Window;
    assert(submenu && submenu->Active);
    assert(submenu->Pos.x >= file->Pos.x + file->Size.x - 5 ||
           submenu->Pos.x + submenu->Size.x <= file->Pos.x + 5);
    capture(e, "agents/reports/floating-windows-recent-narrow.bmp");
    ImGui::ClosePopupToLevel(0, true);
    e->recent_count = 0;
    SDL_SetWindowSize(e->window, 1400, 900);
    e->sidebar = 465;
    frame(e);
}

/* Project settings fits its fields; Enter applies it from the window and from a text field. */
static void project_settings(Editor *e) {
    e->config = true;
    e->new_mode = 0;
    frame(e);
    frame(e);
    key(e, ImGuiKey_Enter);
    assert(!e->config);
    e->config = true;
    frame(e);
    frame(e);
    auto *settings = ImGui::FindWindowByName("Project settings");
    assert(settings && settings->Size.y < 420);
    ImGui::ActivateItemByID(settings->GetID("Name"));
    frame(e);
    replace(e, "Settings name");
    key(e, ImGuiKey_Enter);
    assert(!e->config);
}

/* The Song events window is hidden until its contents are measured, then opens centered
   at its fitted height, without moving afterwards. */
static void song_event_window(Editor *e, Check check) {
    ed_song_event_open_bar(e, 0, true, -1);
    frame(e, 0, 0, false);
    auto *w = ImGui::FindWindowByName(DM_TEXT_SONG_EVENT);
    check(w && w->Active && w->Hidden, "the Song events window is hidden while it measures");
    int hidden = 1;

    while (w->Hidden && hidden < 4) {
        frame(e, 0, 0, false);
        hidden += w->Hidden;
    }

    check(hidden <= 2, "the Song events window shows within two frames");
    ImVec2 pos = w->Pos, size = w->Size;
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    check(!w->Hidden && std::fabs(pos.y + size.y * .5f - center.y) < 1.5f &&
              std::fabs(pos.x + size.x * .5f - center.x) < 1.5f,
          "the Song events window appears centered");
    check(size.y < ImGui::GetMainViewport()->Size.y * .5f,
          "the Add mode Song events window fits its fields");
    frame(e, 0, 0, false);
    frame(e, 0, 0, false);
    check(w->Pos.x == pos.x && w->Pos.y == pos.y && w->Size.x == size.x && w->Size.y == size.y,
          "the Song events window does not move or resize after it appears");
    e->song_event = false;
    frame(e, 0, 0, false);
}

extern "C" void test_floating_windows_ui(Editor *e, Check check) {
    covered_clicks(e);
    generate_window(e);
    list_widths(e);
    recent_submenu(e);
    project_settings(e);
    song_event_window(e, check);
}
