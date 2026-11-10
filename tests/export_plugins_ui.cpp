extern "C" {
#include "editor/export_internal.h"
#include "preferences.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cassert>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

static void frame(Editor *e, float x, float y, bool down) {
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down);
    ui_frame();
    editor_frame(e, 1000, 1000);
    ui_render(e->renderer);
}

static ImGuiWindow *child(const char *part) {
    for (auto *window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && std::strstr(window->Name, part)) {
            return window;
        }
    }

    return nullptr;
}

static void click(Editor *e, ImGuiWindow *window, const char *label, float x = -1) {
    assert(window);
    for (int i = 0; i < 6; i++) {
        frame(e, -100, -100, false);
    }

    ImGuiID id = window->GetID(label);
    if (x < 0) {
        x = window->WorkRect.Min.x + 20;
    }

    for (float y = window->WorkRect.Min.y; y < window->WorkRect.Max.y; y += 3) {
        frame(e, x, y, false);
        if (ImGui::GetCurrentContext()->HoveredId == id) {
            frame(e, x, y, true);
            frame(e, x, y, false);
            frame(e, -100, -100, false);
            return;
        }
    }

    auto *capture = SDL_RenderReadPixels(e->renderer, nullptr);
    if (capture) {
        SDL_SaveBMP(capture, "agents/reports/export-plugins/missing-control.bmp");
        SDL_DestroySurface(capture);
    }

    std::fprintf(stderr, "Missing visible control %s in %s (%g,%g)-(%g,%g), busy=%d\n", label,
                 window->Name, window->WorkRect.Min.x, window->WorkRect.Min.y,
                 window->WorkRect.Max.x, window->WorkRect.Max.y, ed_export_busy(e));
    assert(false);
}

static void wait_catalog(Editor *e) {
    auto *host = ed_export_host(e);
    uint64_t deadline = SDL_GetTicks() + 15000;

    while (host->catalog_work.thread && SDL_GetTicks() < deadline) {
        ed_export_poll(e);
        SDL_Delay(1);
    }

    assert(!host->catalog_work.thread);
}

extern "C" void test_export_catalog_refresh(Editor *e, size_t fixture) {
    int previous_type = e->export_type;
    e->export_type = (int)fixture + 1;

    for (int i = 0; i < 6; i++) {
        frame(e, -100, -100, false);
    }

    wait_catalog(e);
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    auto *window = ImGui::FindWindowByName("Export");
    assert(window && window->Active);
    ImVec2 size = window->Size;
    std::vector<ImDrawVert> vertices(window->DrawList->VtxBuffer.begin(),
                                     window->DrawList->VtxBuffer.end());
    auto *host = ed_export_host(e);

    /* A real saved-library refresh must leave the rendered controls and layout unchanged. */
    ed_export_catalog_changed(e);
    frame(e, -100, -100, false);
    assert(!ed_export_busy(e) && window->Size.x == size.x && window->Size.y == size.y);
    assert(vertices.size() == (size_t)window->DrawList->VtxBuffer.Size &&
           !std::memcmp(vertices.data(), window->DrawList->VtxBuffer.Data,
                        vertices.size() * sizeof(ImDrawVert)));
    wait_catalog(e);

    /* Settings and a foreground export remain available before the scan is polled. */
    ed_export_catalog_changed(e);
    assert(ed_export_context(e) && host->catalog_work.thread && !ed_export_busy(e));
    assert(ed_export_set(e, fixture, "amount", {DME_INT, 8, nullptr}));
    ed_export_start(e, fixture, "agents/reports/export-plugins/worker/during-refresh.txt");
    assert(ed_export_busy(e) && host->work.thread && host->catalog_work.thread);
    uint64_t deadline = SDL_GetTicks() + 15000;

    while (ed_export_busy(e) && SDL_GetTicks() < deadline) {
        ed_export_poll(e);

        if (ed_export_review_open(e)) {
            ed_export_review_decide(e, 1);
        }

        SDL_Delay(1);
    }

    assert(!ed_export_busy(e) && std::strstr(e->status, "complete"));
    wait_catalog(e);
    assert(ed_export_set(e, fixture, "amount", {DME_INT, 7, nullptr}));

    /* A newer save invalidates an in-flight scan; the next scan consumes that notification. */
    ed_export_catalog_changed(e);
    assert(ed_export_context(e) && host->catalog_work.thread);
    ed_export_catalog_changed(e);
    wait_catalog(e);
    assert(host->catalog_dirty);
    assert(ed_export_context(e) && host->catalog_work.thread);
    wait_catalog(e);
    assert(!host->catalog_dirty);
    SDL_Delay(5100);
    assert(ed_export_context(e) && !host->catalog_work.thread);
    e->export_type = previous_type;
    std::puts("PASS event-driven catalog refresh: unchanged rendering, responsive settings/export, "
              "newer-save retry and no periodic scan");
}

static DmeCustomSelection custom_selection(Editor *e) {
    DmeUi ui = {};
    auto *format = ed_export_selected(e);
    assert(format->api->describe(format->instance, &ui) == DME_OK);
    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING) {
            assert(ui.fields[i].row_count && ui.fields[i].rows[0].custom);
            return ui.fields[i].rows[0].custom->selection;
        }
    }

    assert(false);
    return {};
}

static void set_custom_enabled(Editor *e, bool enabled) {
    DmeUi ui = {};
    auto *format = ed_export_selected(e);
    assert(format->api->describe(format->instance, &ui) == DME_OK);

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING) {
            DmeCustomSelection selection = ui.fields[i].rows[0].custom->selection;
            selection.enabled = enabled;
            char error[512] = {0};
            assert(format->api->set_custom(format->instance, ui.fields[i].rows[0].id, &selection,
                                           error, sizeof(error)) == DME_OK);
        }
    }

    for (int i = 0; i < 3; i++) {
        frame(e, -100, -100, false);
    }
}

extern "C" void test_export_custom_panel(Editor *e) {
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    assert(!custom_selection(e).enabled && !custom_selection(e).show_all);
    click(e, child("/mapping_"), "Custom");
    assert(custom_selection(e).enabled && !custom_selection(e).sample.preset_id.length);
    auto *presets = child("/custom-presets_");
    assert(presets);
    float filtered_height = presets->DC.CursorMaxPos.y - presets->DC.CursorStartPos.y;
    click(e, child("/mapping_"), "Show all");
    presets = child("/custom-presets_");
    assert(presets &&
           presets->DC.CursorMaxPos.y - presets->DC.CursorStartPos.y > filtered_height + 5);
    assert(custom_selection(e).show_all && !custom_selection(e).sample.preset_id.length);
    float x = presets->WorkRect.Min.x + 20;
    float y = presets->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * .5f;
    frame(e, x, y, false);
    frame(e, x, y, true);
    frame(e, x, y, false);
    assert(custom_selection(e).sample.preset_id.length);
    frame(e, -100, -100, false);
    click(e, child("/mapping_"), "##sample-operation");
    auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
    assert(stack.Size && stack.back().Window);
    click(e, stack.back().Window, "Replace");
    assert(custom_selection(e).operation == DME_SAMPLE_REPLACE);
    SDL_Surface *capture = SDL_RenderReadPixels(e->renderer, nullptr);
    assert(capture && SDL_SaveBMP(capture, "agents/reports/export-plugins/custom-replace.bmp"));
    SDL_DestroySurface(capture);
    click(e, child("/mapping_"), "##sample-operation");
    assert(stack.Size && stack.back().Window);
    click(e, stack.back().Window, "Add");
    assert(custom_selection(e).operation == DME_SAMPLE_ADD);
}

extern "C" void test_export_history_panel(Editor *e) {
    ed_export_warning_close(e);
    for (int i = 0; i < 6; i++) {
        frame(e, -100, -100, false);
    }

    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    auto *main = ImGui::FindWindowByName("Export");
    assert(main && main->Active);
    click(e, main, ">##repository-history", main->WorkRect.Max.x - 15);
    auto *history = ImGui::FindWindowByName("##repository-history-drawer");
    main = ImGui::FindWindowByName("Export");
    assert(history && history->Active && ed_export_host(e)->history_open);
    assert(std::fabs(history->Pos.y - main->Pos.y) < 1 &&
           std::fabs(history->Size.y - main->Size.y) < 1 &&
           std::fabs(history->Pos.x - main->Pos.x - main->Size.x) < 1);
    /* A drawer of the Export window: no title, close button or resize border of its own. */
    assert((history->Flags & ImGuiWindowFlags_NoTitleBar) &&
           (history->Flags & ImGuiWindowFlags_NoResize) && !history->HasCloseButton);

    /* Enable off: the drawer explains the .bkp copies and hides the history actions; the
       choice round-trips through the preferences file (smoke editors do not save on their
       own). */
    char saved_settings[DM_PATH];
    std::snprintf(saved_settings, sizeof(saved_settings), "%s", e->settings_path);
    std::snprintf(e->settings_path, DM_PATH,
                  "agents/reports/export-plugins/history-preferences.ini");
    auto settle = [&]() {
        uint64_t deadline = SDL_GetTicks() + 15000;

        do {
            frame(e, -100, -100, false);
            SDL_Delay(1);
        } while (ed_export_busy(e) && SDL_GetTicks() < deadline);

        frame(e, -100, -100, false);
    };
    click(e, history, "Enable");
    settle();
    assert(!e->repository_history);
    assert(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names));
    e->repository_history = true;
    assert(dm_preferences_load(e, ed_shortcut_schema()) && !e->repository_history);
    char *ini = static_cast<char *>(
        SDL_LoadFile("agents/reports/export-plugins/history-preferences.ini", nullptr));
    assert(ini && std::strstr(ini, "repository_history=0"));
    SDL_free(ini);
    SDL_Surface *off = SDL_RenderReadPixels(e->renderer, nullptr);
    assert(off && SDL_SaveBMP(off, "agents/reports/export-plugins/history-off.bmp"));
    SDL_DestroySurface(off);
    click(e, history, "Enable");
    settle();
    assert(e->repository_history);
    click(e, history, "Initialize");
    frame(e, -100, -100, false);
    auto *confirm = ImGui::FindWindowByName("Confirm repository action");
    assert(confirm && confirm->Active);
    click(e, confirm, "Cancel");
    std::snprintf(e->settings_path, DM_PATH, "%s", saved_settings);

    /* Export resizes vertically; the drawer and the mapping panels follow it. The window never
       becomes shorter than its contents need. */
    ImGuiWindow *panel = nullptr;

    for (ImGuiWindow *child : main->DC.ChildWindows) {
        panel = strstr(child->Name, "sources") ? child : panel;
    }

    assert(panel);
    /* Drag the bottom-right resize grip, as a user would: until the user resizes it, the
       window keeps its default height, so setting its size directly is undone next frame. */
    auto resize = [&](float height) {
        float x = main->Pos.x + main->Size.x - 4, y = main->Pos.y + main->Size.y - 4;
        float target = y + height - main->Size.y;
        frame(e, x, y, false);
        frame(e, x, y, true);
        frame(e, x, target, true);
        frame(e, x, target, false);

        for (int i = 0; i < 3; i++) {
            frame(e, -100, -100, false);
        }
    };

    /* Without Custom the contents fit the 1000-pixel test display with room to grow. */
    set_custom_enabled(e, false);
    resize(60);
    float main_height = main->Size.y, panel_height = panel->Size.y;
    assert(main_height > 200 && std::fabs(history->Size.y - main_height) < 1);
    assert(main_height + 120 < ImGui::GetMainViewport()->WorkSize.y);
    resize(main_height + 120);
    assert(std::fabs(main->Size.y - main_height - 120) < 2 &&
           std::fabs(panel->Size.y - panel_height - 120) < 2 &&
           std::fabs(history->Size.y - main->Size.y) < 1);
    resize(60);
    assert(std::fabs(main->Size.y - main_height) < 2 &&
           std::fabs(panel->Size.y - panel_height) < 2);
    for (const char *action : {"Initialize", "Revert all"}) {
        click(e, history, action);
        frame(e, -100, -100, false);
        auto *modal = ImGui::FindWindowByName("Confirm repository action");
        assert(modal && modal->Active && (modal->Flags & ImGuiWindowFlags_Modal));
        click(e, modal, "Cancel");
        assert(!ed_export_host(e)->history_confirm && !ed_export_busy(e));
    }

    SDL_Surface *capture = SDL_RenderReadPixels(e->renderer, nullptr);
    assert(capture && SDL_SaveBMP(capture, "agents/reports/export-plugins/history-sidecar.bmp"));
    SDL_DestroySurface(capture);
    for (float scale : {1.25f, 1.5f, 2.0f, 2.5f}) {
        ui_set_scale(scale);
        for (int i = 0; i < 8; i++) {
            frame(e, -100, -100, false);
        }

        auto *view = ImGui::GetMainViewport();
        if (history->Pos.x + history->Size.x > view->WorkPos.x + view->WorkSize.x + 1 ||
            history->Pos.y + history->Size.y > view->WorkPos.y + view->WorkSize.y + 1 ||
            std::fabs(history->Size.y - main->Size.y) >= 1) {
            std::fprintf(stderr,
                         "scale=%g main=(%g,%g %g,%g) history=(%g,%g %g,%g) work=(%g,%g %g,%g)\n",
                         scale, main->Pos.x, main->Pos.y, main->Size.x, main->Size.y,
                         history->Pos.x, history->Pos.y, history->Size.x, history->Size.y,
                         view->WorkPos.x, view->WorkPos.y, view->WorkSize.x, view->WorkSize.y);
        }
        assert(history->Pos.x + history->Size.x <= view->WorkPos.x + view->WorkSize.x + 1 &&
               history->Pos.y + history->Size.y <= view->WorkPos.y + view->WorkSize.y + 1 &&
               std::fabs(history->Pos.y - main->Pos.y) < 1 &&
               std::fabs(history->Size.y - main->Size.y) < 1);
    }

    ui_set_scale(1);
    e->export_window = false;
    frame(e, -100, -100, false);
    assert(!history->Active);
    e->export_window = true;
    frame(e, -100, -100, false);
    assert(history->Active);
    ed_export_host(e)->history_open = false;
    std::puts(
        "PASS history drawer, vertical resizing, confirmations, hiding and 125-250% display bounds");
}

extern "C" void test_export_dropdown(Editor *e, int target) {
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    auto *window = ImGui::FindWindowByName("Export");
    assert(window && window->Active);
    ImGuiID id = window->GetID("Export format");
    float x = window->Pos.x + 70, y = 0;
    bool found = false;

    for (float probe = window->WorkRect.Min.y; probe < window->WorkRect.Min.y + 160; probe += 4) {
        frame(e, x, probe, false);

        if (ImGui::GetCurrentContext()->HoveredId == id) {
            y = probe;
            found = true;
            break;
        }
    }

    assert(found);
    frame(e, x, y, true);
    frame(e, x, y, false);
    frame(e, x, y, false);
    auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
    assert(stack.Size);
    auto *popup = stack.back().Window;
    assert(popup && popup->Active);
    SDL_Surface *capture = SDL_RenderReadPixels(e->renderer, nullptr);
    assert(capture);
    SDL_SaveBMP(capture, "agents/reports/export-plugins/plugin-dropdown.bmp");
    SDL_DestroySurface(capture);
    float px = popup->WorkRect.Min.x + 20;
    int row = target > 0 ? target + 1 : target == 0 ? 0 : 1;
    float py = popup->DC.CursorStartPos.y + row * ImGui::GetTextLineHeightWithSpacing() +
               ImGui::GetTextLineHeight() * .5f;
    frame(e, px, py, false);
    frame(e, px, py, true);
    frame(e, px, py, false);
    assert(e->export_type == target);
}

extern "C" void test_export_mapping_row(Editor *e, int field_index, int row) {
    frame(e, -100, -100, false);
    ImGuiWindow *sources = nullptr;

    for (auto *w : ImGui::GetCurrentContext()->Windows) {
        if (w->Active && std::strstr(w->Name, "/sources_")) {
            sources = w;
        }
    }

    assert(sources);
    float x = sources->WorkRect.Min.x + 20, y = sources->DC.CursorStartPos.y +
                                                row * ImGui::GetTextLineHeightWithSpacing() +
                                                ImGui::GetTextLineHeight() * .5f;
    frame(e, x, y, false);
    frame(e, x, y, true);
    frame(e, x, y, false);
    auto *format = ed_export_selected(e);
    assert(format && format->selected_rows[field_index] == row);
}

extern "C" void test_m64_sequence_dropdown(Editor *e, int target) {
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    auto *w = ImGui::FindWindowByName("Export");
    assert(w && w->Active);
    auto *format = ed_export_selected(e);
    DmeUi ui = {};
    assert(format && format->api->describe(format->instance, &ui) == DME_OK);
    int field = 0;

    while (field < (int)ui.count && std::strcmp(ui.fields[field].id, "sequence")) {
        field++;
    }

    assert(field < (int)ui.count);
    ImGuiID scope = ImHashData(&field, sizeof(field), w->IDStack.back());
    ImGuiID id = ImHashStr("Sequence name", 0, scope);
    float x = w->WorkRect.Min.x + 70, y = 0;
    bool found = false;

    for (float probe = w->WorkRect.Min.y; probe < w->WorkRect.Min.y + 240; probe += 4) {
        frame(e, x, probe, false);

        if (ImGui::GetCurrentContext()->HoveredId == id) {
            y = probe;
            found = true;
            break;
        }
    }

    assert(found);
    frame(e, x, y, true);
    frame(e, x, y, false);
    frame(e, x, y, false);
    auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
    assert(stack.Size);
    auto *popup = stack.back().Window;
    assert(popup && popup->Active);
    ImGui::SetScrollY(popup, target == 35 ? popup->ScrollMax.y : 0);
    frame(e, x, y, false);
    frame(e, x, y, false);
    float px = popup->WorkRect.Min.x + 20, py = popup->DC.CursorStartPos.y +
                                                target * ImGui::GetTextLineHeightWithSpacing() +
                                                ImGui::GetTextLineHeight() * .5f;
    assert(py >= popup->InnerRect.Min.y && py < popup->InnerRect.Max.y);
    frame(e, px, py, false);
    frame(e, px, py, true);
    frame(e, px, py, false);
    frame(e, -100, -100, false);
    assert(format->api->describe(format->instance, &ui) == DME_OK);
    assert(!std::strcmp(ui.fields[field].id, "sequence") &&
           ui.fields[field].value.integer == target);
    SDL_Surface *capture = SDL_RenderReadPixels(e->renderer, nullptr);
    assert(capture);
    SDL_SaveBMP(capture, target == 35 ? "agents/reports/m64-export-ui/custom.bmp"
                                      : "agents/reports/m64-export-ui/preset.bmp");
    SDL_DestroySurface(capture);

    if (target != 35) {
        return;
    }

    /* A similar instrument picked for the first row shows in both dropdowns. */
    const DmeField *mapping = nullptr;

    for (uint32_t i = 0; i < ui.count; i++) {
        if (!std::strcmp(ui.fields[i].id, "mapping")) {
            mapping = &ui.fields[i];
        }
    }

    assert(mapping && mapping->row_count && mapping->rows[0].similar_count);
    char row[64];
    SDL_snprintf(row, sizeof(row), "%s", mapping->rows[0].id);
    int before = mapping->rows[0].value, pick = mapping->rows[0].similar[0].value;
    size_t index = (size_t)(e->export_type - 1);
    assert(ed_export_set(e, index, row, DmeValue{DME_CHOICE, pick, nullptr}));

    for (int i = 0; i < 6; i++) {
        frame(e, -100, -100, false);
    }
    capture = SDL_RenderReadPixels(e->renderer, nullptr);
    assert(capture);
    SDL_SaveBMP(capture, "agents/reports/m64-export-ui/similar-picked.bmp");
    SDL_DestroySurface(capture);
    assert(ed_export_set(e, index, row, DmeValue{DME_CHOICE, before, nullptr}));
}

extern "C" void test_export_warning(Editor *e) {
    assert(ed_export_warning_open(e));
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    auto *w = ImGui::FindWindowByName("Export warnings");
    assert(w && w->Active && (w->Flags & ImGuiWindowFlags_Modal));
    assert(ImGui::GetCurrentContext()->OpenPopupStack.Size);
    SDL_Surface *capture = SDL_RenderReadPixels(e->renderer, nullptr);
    assert(capture);
    SDL_SaveBMP(capture, "agents/reports/m64-pitch-range/export-warnings.bmp");
    SDL_DestroySurface(capture);
    static int close_method = 0;

    if ((close_method++ % 3) == 1) {
        SDL_Event key = {};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.key = SDLK_ESCAPE;
        key.key.scancode = SDL_SCANCODE_ESCAPE;
        editor_event(e, &key);
        frame(e, -100, -100, false);
        assert(!ed_export_warning_open(e));
        return;
    }

    if ((close_method % 3) == 0) {
        float cx = w->Pos.x + w->Size.x - ImGui::GetFrameHeight() * .5f,
              cy = w->Pos.y + ImGui::GetFrameHeight() * .5f;
        frame(e, cx, cy, false);
        frame(e, cx, cy, true);
        frame(e, cx, cy, false);
        frame(e, cx, cy, false);
        assert(!ed_export_warning_open(e));
        return;
    }

    float x = w->WorkRect.Min.x + 30, y = w->WorkRect.Max.y - 20;
    // The OK button follows the scrollable warning body at the bottom.
    bool found = false;
    ImGuiID id = w->GetID("OK");

    for (float probe = w->WorkRect.Max.y - 50; probe < w->WorkRect.Max.y; probe += 3) {
        frame(e, x, probe, false);

        if (ImGui::GetCurrentContext()->HoveredId == id) {
            y = probe;
            found = true;
            break;
        }
    }

    assert(found);
    frame(e, x, y, true);
    frame(e, x, y, false);
    frame(e, x, y, false);
    assert(!ed_export_warning_open(e));
}
