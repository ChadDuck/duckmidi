extern "C" {
#include "editor/export_internal.h"
#include "../include/duckmidi_samples_tool.h"
#include "../shared/sample/library.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static void frame(Editor *editor, int width = 1400, int height = 900) {
    ui_frame();
    editor_frame(editor, (float)width, (float)height);
    SDL_SetRenderDrawColor(editor->renderer, 0, 0, 0, 255);
    SDL_RenderClear(editor->renderer);
    ui_render(editor->renderer);
}

static void activate(Editor *editor, ImGuiWindow *window, const char *label) {
    ImGui::ActivateItemByID(window->GetID(label));
    frame(editor);
}

static void enter_set_name(Editor *editor, ImGuiWindow *window, const char *name,
                           bool enter = false) {
    ImGui::ActivateItemByID(window->GetID("##sample-set-name"));
    frame(editor);
    auto &io = ImGui::GetIO();
    io.AddInputCharactersUTF8(name);
    frame(editor);

    if (enter) {
        io.AddKeyEvent(ImGuiKey_Enter, true);
        frame(editor);
        io.AddKeyEvent(ImGuiKey_Enter, false);
        frame(editor);
    }
}

static void select_set(Editor *editor, ImGuiWindow *window, const char *name, int index) {
    activate(editor, window, "##sample-set-select");
    auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
    auto *combo = popups.back().Window;
    ImGuiID parent = combo->IDStack.back();

    if (index >= 0) {
        parent = ImHashData(&index, sizeof(index), parent);
    }

    ImGui::ActivateItemByID(ImHashStr(name, 0, parent));
    frame(editor);
}

static void await_count(Editor *editor, const DmeSamplesApi *api, const DmNativeTool *tool,
                        uint32_t count) {
    uint64_t end = SDL_GetTicks() + 8000;

    do {
        frame(editor);
        SDL_Delay(2);
    } while (api->count(tool->instance) != count && SDL_GetTicks() < end);

    frame(editor);
    frame(editor);
}

static ImGuiWindow *details_window() {
    for (auto *window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && std::strstr(window->Name, "/##sample-details_")) {
            return window;
        }
    }

    return nullptr;
}

static UiRect graph_rect(ImGuiWindow *window, float height) {
    auto &vertices = window->DrawList->VtxBuffer;
    ImU32 background = ui_background_shade(12);

    for (int first = 0; first < vertices.Size;) {
        int end = first + 1;

        while (end < vertices.Size &&
               (vertices[end].col & 0xffffffu) == (vertices[first].col & 0xffffffu)) {
            ++end;
        }

        if ((vertices[first].col & 0xffffffu) == (background & 0xffffffu) && end - first >= 4) {
            ImVec2 lo = vertices[first].pos, hi = lo;

            for (int i = first + 1; i < end; ++i) {
                lo = ImMin(lo, vertices[i].pos);
                hi = ImMax(hi, vertices[i].pos);
            }

            if (std::fabs(hi.y - lo.y - height) < 2 && hi.x - lo.x > 100) {
                return {lo.x + .5f, lo.y + .5f, hi.x - lo.x - 1, hi.y - lo.y - 1};
            }
        }

        first = end;
    }

    return {};
}

static std::vector<ImDrawVert> graph_prefix(ImGuiWindow *window, UiRect rect, float end) {
    std::vector<ImDrawVert> result;

    for (const auto &vertex : window->DrawList->VtxBuffer) {
        if (vertex.pos.x >= rect.x - 1 && vertex.pos.x < end && vertex.pos.y >= rect.y - 1 &&
            vertex.pos.y <= rect.y + rect.h + 1) {
            result.push_back(vertex);
        }
    }

    return result;
}

static void mouse(Editor *e, float x, float y, bool down) {
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down);
    frame(e);
}

static void drop(Editor *editor, Uint32 type, UiRect rect, const char *path = nullptr) {
    SDL_Event event = {};
    event.type = type;
    event.drop.x = (rect.x + rect.w / 2) * ui_scale();
    event.drop.y = (rect.y + rect.h / 2) * ui_scale();
    event.drop.data = path;
    editor_event(editor, &event);
}

static void key(Editor *editor, SDL_Scancode code, SDL_Keymod mods = SDL_KMOD_NONE) {
    SDL_Event event = {};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.down = true;
    event.key.scancode = code;
    event.key.mod = mods;
    editor_event(editor, &event);
}

static void capture(Editor *editor, const char *root, const char *leaf,
                    void (*check)(bool, const char *)) {
    SDL_Surface *surface = SDL_RenderReadPixels(editor->renderer, nullptr);
    char path[DM_PATH];
    std::snprintf(path, sizeof(path), "%s%s.bmp", root, leaf);
    check(surface && SDL_SaveBMP(surface, path), "save layout capture");
    SDL_DestroySurface(surface);
}

extern "C" void test_samples_ui(Editor *e, const char *root, void (*check)(bool, const char *)) {
    ed_samples_open(e);
    frame(e);
    frame(e);
    auto *window = ImGui::FindWindowByName("Samples");
    check(window && window->Active, "Edit opens the registered Samples window");
    frame(e);
    frame(e);
    check(window->Size.y < 280 && window->Size.x <= 761,
          "empty Samples window has the compact initial dimensions");
    check(window->DC.CursorMaxPos.y - window->DC.CursorStartPos.y < 190,
          "first empty window omits the sample-set controls");
    capture(e, root, "empty", check);
    auto *host = ed_export_host(e);
    auto *tool = dm_export_registry_tool(&host->registry, DME_SAMPLES_TOOL_ID);
    check(tool, "registry holds the instance drawn by the desktop");
    auto *api = static_cast<const DmeSamplesApi *>(tool->api->interface_at(DME_SAMPLES_API));
    UiRect rect = {window->Pos.x, window->Pos.y, window->Size.x, window->Size.y};
    e->preferences = e->preferences_raise = true;
    frame(e);
    frame(e);
    UiPoint overlap = {std::fmax(rect.x, e->preferences_rect.x) + 20,
                       std::fmax(rect.y, e->preferences_rect.y) + 20};
    check(ui_window_front_at("Preferences", overlap) &&
              !ed_samples_drop_target(e, overlap, nullptr),
          "frontmost Preferences wins over the overlapping Samples drop target");
    e->preferences = false;
    ImGui::FocusWindow(window);
    frame(e);
    SDL_Event enter = {};
    enter.type = SDL_EVENT_DROP_BEGIN;
    editor_event(e, &enter);
    check(e->drop_position.x < 0 && e->drop_position.y < 0,
          "a drag without a position cannot show a premature generic overlay");
    drop(e, SDL_EVENT_DROP_BEGIN, rect);
    check(ed_samples_drop_target(e, e->drop_position, nullptr),
          "the initial valid drag position resolves the Samples target immediately");
    drop(e, SDL_EVENT_DROP_POSITION, rect);
    drop(e, SDL_EVENT_DROP_FILE, rect, "agents/reports/audio-export/codec-0-0.wav");
    drop(e, SDL_EVENT_DROP_FILE, rect, "agents/reports/audio-export/tone_B4.aiff");
    drop(e, SDL_EVENT_DROP_FILE, rect, "agents/reports/audio-export/broken.aiff");
    drop(e, SDL_EVENT_DROP_COMPLETE, rect);
    uint64_t end = SDL_GetTicks() + 8000;

    while (api->count(tool->instance) < 3 && SDL_GetTicks() < end) {
        frame(e);
        SDL_Delay(2);
    }

    check(api->count(tool->instance) == 3, "collect the whole file-drop batch");
    DmeSampleDraft failed = {};
    check(api->draft(tool->instance, 2, &failed) == DME_OK && failed.error.length,
          "corrupt item remains alongside successful imports");
    frame(e);
    frame(e);
    frame(e);
    capture(e, root, "populated", check);
    auto *details = details_window();
    check(details, "details pane is rendered");
    float panel_height = details->Size.y;
    float window_height = window->Size.y;
    float resize_x = window->Pos.x + window->Size.x - 4;
    float resize_y = window->Pos.y + window->Size.y - 4;
    mouse(e, resize_x, resize_y, false);
    mouse(e, resize_x, resize_y, true);
    mouse(e, resize_x, resize_y + 140, true);
    mouse(e, resize_x, resize_y + 140, false);
    frame(e);
    frame(e);
    std::printf("Resize window %.1f -> %.1f, panel %.1f -> %.1f\n", window_height, window->Size.y,
                panel_height, details->Size.y);
    check(details->Size.y > panel_height + 130 && window->Size.y >= window_height + 139,
          "both panels consume additional vertical window space");
    capture(e, root, "resized", check);
    DmeSampleDraft edited = {};
    check(api->draft(tool->instance, 0, &edited) == DME_OK, "read initial draft");
    DmeSampleEdit initial = edited.edit;
    edited.edit.zone.root = 2;
    edited.edit.zone.tone.reverb = .9;
    check(api->edit(tool->instance, 0, &edited.edit) == DME_OK, "prepare reset changes");
    activate(e, details, "Reset to import values");
    check(api->draft(tool->instance, 0, &edited) == DME_OK &&
              edited.edit.zone.root == initial.zone.root && !edited.edit.zone.tone.reverb,
          "Reset restores the selected sample's import values through the actual button");
    edited.edit.zone.key_lo = 22;
    edited.edit.zone.key_hi = 100;
    edited.edit.melodic_key_lo = 22;
    edited.edit.melodic_key_hi = 100;
    edited.edit.zone.root = 70;
    check(api->edit(tool->instance, 0, &edited.edit) == DME_OK, "set manual draft fields");
    activate(e, details, "Drums");
    activate(e, details, "Melody");
    check(api->draft(tool->instance, 0, &edited) == DME_OK && edited.edit.zone.key_lo == 22 &&
              edited.edit.zone.key_hi == 100 && edited.edit.zone.root == 70,
          "category switches preserve manual pitch and melodic range");
    activate(e, details, "Continuous");
    key(e, SDL_SCANCODE_SPACE, SDL_KMOD_SHIFT);
    UiRect waveform = graph_rect(details, 106);
    check(waveform.w > 100, "actual waveform geometry is present");
    mouse(e, waveform.x + 1, waveform.y + 15, false);
    mouse(e, waveform.x + 1, waveform.y + 15, true);
    mouse(e, waveform.x + waveform.w * .25f, waveform.y + 15, true);
    mouse(e, waveform.x + waveform.w * .25f, waveform.y + 15, false);
    check(api->draft(tool->instance, 0, &edited) == DME_OK && edited.edit.zone.loop_start > 0 &&
              edited.edit.zone.loop_end <= edited.frames &&
              edited.edit.zone.loop_start + 2 <= edited.edit.zone.loop_end,
          "waveform drag sets a valid half-open loop");
    check(dm_audio_sample_audition_loop(e->audio, edited.edit.zone.loop_start,
                                        edited.edit.zone.loop_end),
          "waveform drag keeps the active audition available");
    activate(e, details, "No loop");
    auto &play_io = ImGui::GetIO();
    play_io.AddKeyEvent(ImGuiMod_Shift, true);
    mouse(e, waveform.x + 1, waveform.y + 15, false);
    mouse(e, waveform.x + 1, waveform.y + 15, true);
    mouse(e, waveform.x + waveform.w * .1f, waveform.y + 15, true);
    mouse(e, waveform.x + waveform.w * .1f, waveform.y + 15, false);
    play_io.AddKeyEvent(ImGuiMod_Shift, false);
    frame(e);
    check(api->draft(tool->instance, 0, &edited) == DME_OK && edited.edit.zone.playback_set &&
              edited.edit.zone.playback_start > 0 && edited.edit.zone.playback_end <= edited.frames,
          "Shift-drag edits playback boundaries separately from the disabled loop");
    capture(e, root, "playback-boundaries", check);
    ImGui::SetScrollY(details, 180);
    frame(e);
    frame(e);
    UiRect env = graph_rect(details, 114);
    check(env.w > 100, "envelope geometry is present");
    float gap = (env.w - 10) * .08f;
    double extent = 2 * std::log1p(20 / .02) + 3 * std::log1p(100 / .02);
    double scale = (env.w - 10 - 5 * gap) / extent;
    float attack_x = env.x + 5 + gap +
                     (float)((std::log1p(edited.edit.zone.delay / .02) +
                              std::log1p(edited.edit.zone.attack / .02)) *
                             scale);
    float attack_y = env.y + 8;
    mouse(e, attack_x, attack_y, false);
    mouse(e, attack_x, attack_y, true);
    mouse(e, attack_x + 25, attack_y, true);
    mouse(e, attack_x + 25, attack_y, false);
    DmeSampleDraft after_drag = {};
    check(api->draft(tool->instance, 0, &after_drag) == DME_OK &&
              after_drag.edit.zone.attack > edited.edit.zone.attack,
          "horizontal envelope handle changes attack time");
    check(dm_audio_sample_audition_edit(e->audio, &after_drag.edit.zone),
          "envelope dragging preserves the playing audition and accepts live parameters");
    double times[] = {after_drag.edit.zone.delay, after_drag.edit.zone.attack,
                      after_drag.edit.zone.hold, after_drag.edit.zone.decay,
                      after_drag.edit.zone.release};
    float release_x = env.x + 5 + 5 * gap;

    for (double value : times) {
        release_x += (float)(std::log1p(value / .02) * scale);
    }

    float decay_x = release_x - 2 * gap - (float)(std::log1p(times[4] / .02) * scale);
    auto earlier = graph_prefix(details, env, decay_x + 7);
    mouse(e, release_x, env.y + env.h - 8, false);
    mouse(e, release_x, env.y + env.h - 8, true);
    mouse(e, release_x + 12, env.y + env.h - 8, true);
    mouse(e, release_x + 12, env.y + env.h - 8, false);
    DmeSampleDraft release_drag = {};
    check(api->draft(tool->instance, 0, &release_drag) == DME_OK &&
              release_drag.edit.zone.release > after_drag.edit.zone.release &&
              release_drag.edit.zone.decay == after_drag.edit.zone.decay &&
              release_drag.edit.zone.attack == after_drag.edit.zone.attack,
          "Release drag changes release independently of all earlier durations");
    auto later = graph_prefix(details, env, decay_x + 7);
    bool stationary = !earlier.empty() && earlier.size() == later.size();

    for (size_t i = 0; stationary && i < earlier.size(); ++i) {
        stationary = earlier[i].pos.x == later[i].pos.x && earlier[i].pos.y == later[i].pos.y &&
                     earlier[i].col == later[i].col;
    }

    check(stationary, "Release dragging leaves the earlier rendered hollow points stationary");
    capture(e, root, "envelope-release", check);
    key(e, SDL_SCANCODE_SPACE, SDL_KMOD_SHIFT);
    ImGui::SetScrollY(details, 0);
    frame(e);
    key(e, SDL_SCANCODE_SPACE);
    key(e, SDL_SCANCODE_SPACE, SDL_KMOD_SHIFT);
    key(e, SDL_SCANCODE_SPACE, SDL_KMOD_SHIFT);
    check(!dm_audio_playing(e->audio), "Samples audition leaves the transport stopped");
    enter_set_name(e, window, "First set");
    activate(e, window, "Apply");
    frame(e);
    check(!ImGui::FindWindowByName("Save sample set"),
          "Apply saves directly without a naming popup");
    char library[DM_PATH];
    std::snprintf(library, sizeof(library), "%ssamples", root);
    DmSampleCatalog catalog = {};
    char error[512] = {};
    end = SDL_GetTicks() + 8000;

    while (!catalog.count && SDL_GetTicks() < end) {
        frame(e);
        dm_sample_catalog_free(&catalog);
        dm_sample_library_catalog(nullptr, library, nullptr, &catalog, error, sizeof(error));
        SDL_Delay(2);
    }

    check(catalog.count == 1 && catalog.entries[0].generation != 0, "publish and discover a set");
    DmSampleManifest manifest = {};
    check(dm_sample_library_manifest(library, catalog.entries[0].id, &manifest, error,
                                     sizeof(error)) &&
              manifest.asset_count == 2 && manifest.preset_count == 2,
          "publication preserves both good samples and skips the failed import");
    dm_sample_manifest_free(&manifest);
    frame(e);
    ed_samples_close(e);
    frame(e);
    ed_open_instrument(e, 0, false);
    frame(e);
    auto *instrument = ImGui::FindWindowByName("Instrument");
    check(instrument && instrument->Active, "open the ordinary Instrument dialog");
    activate(e, instrument, "Source");
    auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
    check(popups.Size && popups.back().Window, "source dropdown opens");
    ImGuiWindow *combo = popups.back().Window;
    int index = 0;
    ImGuiID row = ImHashData(&index, sizeof(index), combo->IDStack.back());
    ImGui::ActivateItemByID(ImHashStr(catalog.entries[0].name, 0, row));
    frame(e);
    check(e->instrument_source_dirty && e->instrument_source.generation != 0,
          "select the newly published set");
    activate(e, instrument, "Apply");
    check(e->project.patch_replacement_count == 1 &&
              e->project.patch_replacements[0].generation != 0,
          "apply sample source to the original MIDI program");
    const char *label = ed_sample_channel_name(e, 0);
    check(label && !std::strcmp(label, "codec-0-0"),
          "playlist sample label uses the sample's name");
    int automation = dm_pattern_add(&e->project, "Label automation");
    check(automation >= 0, "create populated subrow fixture");
    e->project.patterns[automation].kind = 1;
    e->project.lane_count[0] = 1;
    check(dm_clip_add(&e->project, {.pattern = automation, .channel = 0, .lane = 1}),
          "place an automation pattern in the channel subrow");
    frame(e);
    frame(e);
    capture(e, root, "assigned-playlist", check);
    dm_pattern_remove(&e->project, automation);
    e->project.lane_count[0] = 0;
    frame(e);
    uint8_t bank_msb[] = {MIDI_CONTROL | 4, MIDI_CC_BANK, 2};
    uint8_t bank_lsb[] = {MIDI_CONTROL | 4, MIDI_CC_BANK_LSB, 5};
    uint8_t program[] = {MIDI_PROGRAM | 4, 6};
    DmBytes message = {bank_msb, sizeof(bank_msb), sizeof(bank_msb)};
    check(dm_event_add(&e->project, 1, 0, 3, &message), "add routed bank MSB");
    message = {bank_lsb, sizeof(bank_lsb), sizeof(bank_lsb)};
    check(dm_event_add(&e->project, 2, 0, 3, &message), "add preceding same-tick bank LSB");
    size_t program_index = e->project.event_count;
    message = {program, sizeof(program), sizeof(program)};
    check(dm_event_add(&e->project, 2, 0, 3, &message), "add Program Change target");
    bank_lsb[2] = 7;
    message = {bank_lsb, sizeof(bank_lsb), sizeof(bank_lsb)};
    check(dm_event_add(&e->project, 2, 0, 3, &message), "add following same-tick bank LSB");
    ed_commit(e);
    e->midi_events = true;
    e->midi_values = false;
    ed_midi_select(e, {2, (int)program_index, false});
    frame(e);
    frame(e);
    ImGuiWindow *event_details = nullptr;

    for (auto *candidate : ImGui::GetCurrentContext()->Windows) {
        if (candidate->Active && std::strstr(candidate->Name, "/MIDI event details_")) {
            event_details = candidate;
        }
    }

    check(event_details, "Program Change renders its source picker");
    activate(e, event_details, "Source");
    check(popups.Size && popups.back().Window, "Program Change source dropdown opens");
    combo = popups.back().Window;
    row = ImHashData(&index, sizeof(index), combo->IDStack.back());
    ImGui::ActivateItemByID(ImHashStr(catalog.entries[0].name, 0, row));
    frame(e);
    check(e->midi_sample_dirty && e->midi_sample_source.key.bank == 261 &&
              e->midi_sample_source.key.port == 3 && e->midi_sample_source.key.channel == 4 &&
              e->midi_sample_source.key.program == 6,
          "Program Change source uses preceding bank messages and its original route");
    activate(e, event_details, "Apply source");
    check(e->project.patch_replacement_count == 2 && !e->midi_sample_dirty &&
              e->project.events[program_index].message.size == sizeof(program) &&
              !std::memcmp(e->project.events[program_index].message.data, program, sizeof(program)),
          "source assignment preserves the Program Change bytes");
    ed_undo(e, false);
    check(e->project.patch_replacement_count == 1, "undo removes only the new source assignment");
    ed_undo(e, true);
    check(e->project.patch_replacement_count == 2, "redo restores the source assignment");
    e->midi_events = false;
    frame(e);
    char set_id[DM_SAMPLE_ID_CAP], set_name[DM_SAMPLE_NAME_CAP];
    std::snprintf(set_id, sizeof(set_id), "%s", catalog.entries[0].id);
    std::snprintf(set_name, sizeof(set_name), "%s", catalog.entries[0].name);
    dm_sample_catalog_free(&catalog);
    ed_samples_open(e);
    frame(e);
    frame(e);
    check(api->draft(tool->instance, 0, &edited) == DME_OK, "read saved draft for history");
    double saved_gain = edited.edit.zone.gain_db;
    uint64_t saved_token = edited.token;
    DmeSampleEdit gain_edit = edited.edit;
    gain_edit.zone.gain_db = -7;
    check(api->edit(tool->instance, 0, &gain_edit) == DME_OK, "edit the current sample");
    ImGui::FocusWindow(window);
    frame(e);
    key(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    check(api->draft(tool->instance, 0, &edited) == DME_OK &&
              edited.edit.zone.gain_db == saved_gain && edited.token == saved_token,
          "Ctrl+Z restores sample settings within Samples");
    key(e, SDL_SCANCODE_Y, SDL_KMOD_CTRL);
    check(api->draft(tool->instance, 0, &edited) == DME_OK && edited.edit.zone.gain_db == -7,
          "Ctrl+Y restores the sample edit");
    check(dm_sample_library_manifest(library, set_id, &manifest, error, sizeof(error)),
          "read current stored settings before replacement");
    uint32_t previous_generation = manifest.generation;
    activate(e, window, "Save");
    frame(e);
    check(popups.Size == 0, "Save replaces the selected set without a naming popup");
    end = SDL_GetTicks() + 8000;

    do {
        frame(e);
        dm_sample_manifest_free(&manifest);
        dm_sample_library_manifest(library, set_id, &manifest, error, sizeof(error));
        SDL_Delay(2);
    } while ((!manifest.generation || manifest.generation == previous_generation) &&
             SDL_GetTicks() < end);

    check(manifest.generation && manifest.generation != previous_generation &&
              manifest.zones[0].gain_db == -7,
          "Save replaces the sole stored set with its latest settings");
    frame(e);
    frame(e);
    check(e->project.patch_replacement_count == 2 &&
              e->project.patch_replacements[0].generation == manifest.generation &&
              e->project.patch_replacements[1].generation == manifest.generation,
          "saving a set refreshes its assigned routes without instrument reselection");
    key(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    check(api->draft(tool->instance, 0, &edited) == DME_OK &&
              edited.edit.zone.gain_db == saved_gain,
          "saving retains the in-memory sample undo history");
    dm_sample_manifest_free(&manifest);
    check(dm_sample_library_manifest(library, set_id, &manifest, error, sizeof(error)) &&
              manifest.zones[0].gain_db == -7,
          "undo leaves the saved sample unchanged until the next Save");
    key(e, SDL_SCANCODE_Y, SDL_KMOD_CTRL);
    dm_sample_manifest_free(&manifest);
    frame(e);
    frame(e);
    select_set(e, window, "New set", -1);
    await_count(e, api, tool, 0);
    check(api->count(tool->instance) == 0 && window->Size.y < 350,
          "new set has only its own empty draft and compact layout");
    rect = {window->Pos.x, window->Pos.y, window->Size.x, window->Size.y};
    drop(e, SDL_EVENT_DROP_BEGIN, rect);
    drop(e, SDL_EVENT_DROP_FILE, rect, "agents/reports/audio-export/tone_B4.aiff");
    drop(e, SDL_EVENT_DROP_COMPLETE, rect);
    await_count(e, api, tool, 1);
    check(api->count(tool->instance) == 1, "import into an isolated new set");
    enter_set_name(e, window, "First set");
    activate(e, window, "Apply");
    check(std::strstr(e->status, "already uses that name"),
          "set-name collisions preserve the draft");
    enter_set_name(e, window, "Second set", true);
    end = SDL_GetTicks() + 8000;

    do {
        frame(e);
        dm_sample_catalog_free(&catalog);
        dm_sample_library_catalog(nullptr, library, nullptr, &catalog, error, sizeof(error));
        SDL_Delay(2);
    } while (catalog.count != 2 && SDL_GetTicks() < end);

    check(catalog.count == 2, "Enter in the name field creates the second set");
    select_set(e, window, "First set", 0);
    await_count(e, api, tool, 3);
    check(api->count(tool->instance) == 3 && api->draft(tool->instance, 0, &edited) == DME_OK &&
              edited.edit.zone.root == 70,
          "selecting the first set restores its own draft without second-set samples");
    dm_sample_catalog_free(&catalog);
    frame(e);
    check(window->ScrollMax.y < 1, "sample Save and history buttons fit below the panels");
    capture(e, root, "set-controls", check);
    char batch[DM_PATH], destination[DM_PATH];
    std::snprintf(batch, sizeof(batch), "%sbatch", root);
    check(SDL_CreateDirectory(batch), "create isolated directory import");
    const char *sources[] = {"agents/reports/audio-export/codec-0-0.wav",
                             "agents/reports/audio-export/tone_B4.aiff"};
    const char *names[] = {"b.wav", "a.aiff"};

    for (int file = 0; file < 2; ++file) {
        DmBytes bytes = {};
        check(dm_read(sources[file], &bytes, error, sizeof(error)), "read generated audio fixture");
        std::snprintf(destination, sizeof(destination), "%s/%s", batch, names[file]);
        check(dm_write(destination, bytes.data, bytes.size, error, sizeof(error)),
              "copy generated directory fixture");
        dm_bytes_free(&bytes);
    }

    rect = {window->Pos.x, window->Pos.y, window->Size.x, window->Size.y};
    drop(e, SDL_EVENT_DROP_BEGIN, rect);
    drop(e, SDL_EVENT_DROP_FILE, rect, destination);
    drop(e, SDL_EVENT_DROP_COMPLETE, rect);
    end = SDL_GetTicks() + 8000;

    while (api->count(tool->instance) < 4 && SDL_GetTicks() < end) {
        frame(e);
        SDL_Delay(2);
    }

    check(api->count(tool->instance) == 4, "import an explicit file before its containing folder");
    drop(e, SDL_EVENT_DROP_BEGIN, rect);
    drop(e, SDL_EVENT_DROP_FILE, rect, batch);
    drop(e, SDL_EVENT_DROP_COMPLETE, rect);
    end = SDL_GetTicks() + 8000;

    while (api->count(tool->instance) < 5 && SDL_GetTicks() < end) {
        frame(e);
        SDL_Delay(2);
    }

    DmeSampleDraft directory_item = {};
    check(api->count(tool->instance) == 5 &&
              api->draft(tool->instance, 3, &directory_item) == DME_OK &&
              std::strstr(directory_item.path.data, "a.aiff"),
          "folder skips the existing explicit file and appends the other child");
    drop(e, SDL_EVENT_DROP_BEGIN, rect);
    drop(e, SDL_EVENT_DROP_FILE, rect, destination);
    drop(e, SDL_EVENT_DROP_FILE, rect, destination);
    drop(e, SDL_EVENT_DROP_COMPLETE, rect);
    end = SDL_GetTicks() + 8000;

    while (api->count(tool->instance) < 7 && SDL_GetTicks() < end) {
        frame(e);
        SDL_Delay(2);
    }

    check(api->count(tool->instance) == 7,
          "explicit duplicates remain allowed after a folder and within one file batch");
    ImGuiWindow *list = nullptr;

    for (auto *candidate : ImGui::GetCurrentContext()->Windows) {
        if (candidate->Active && std::strstr(candidate->Name, "/##sample-list_")) {
            list = candidate;
        }
    }

    check(list, "sample list context target exists");
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(list->Pos.x + 50,
                        list->DC.CursorStartPos.y + 2 * ImGui::GetTextLineHeightWithSpacing() + 5);
    io.AddMouseButtonEvent(1, false);
    frame(e);
    io.AddMouseButtonEvent(1, true);
    frame(e);
    io.AddMouseButtonEvent(1, false);
    frame(e);
    check(popups.Size && popups.back().Window, "right click opens sample actions");
    capture(e, root, "sample-actions", check);
    activate(e, popups.back().Window, "Delete");
    frame(e);
    auto *confirmation = ImGui::FindWindowByName("Delete sample?");
    check(confirmation && confirmation->Active && api->count(tool->instance) == 7,
          "deletion waits for confirmation");
    capture(e, root, "delete-confirmation", check);
    activate(e, confirmation, "Cancel");
    check(api->count(tool->instance) == 7, "cancelling deletion preserves the selected sample");
    io.AddMousePosEvent(list->Pos.x + 50,
                        list->DC.CursorStartPos.y + 2 * ImGui::GetTextLineHeightWithSpacing() + 5);
    io.AddMouseButtonEvent(1, true);
    frame(e);
    io.AddMouseButtonEvent(1, false);
    frame(e);
    activate(e, popups.back().Window, "Delete");
    frame(e);
    activate(e, confirmation, "Delete");
    check(api->count(tool->instance) == 6, "context deletion removes exactly the clicked draft");
    check(api->draft(tool->instance, 2, &directory_item) == DME_OK &&
              !directory_item.error.length && directory_item.edit.name.length,
          "drafts after the removed row retain valid names and ownership");

    ImGui::FocusWindow(window);
    key(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    check(api->count(tool->instance) == 7, "Ctrl+Z restores the context-deleted draft");
    key(e, SDL_SCANCODE_Z, SDL_Keymod(SDL_KMOD_CTRL | SDL_KMOD_SHIFT));
    check(api->count(tool->instance) == 6, "Ctrl+Shift+Z reapplies the sample deletion");

    SDL_SetWindowSize(e->window, 780, 520);
    SDL_SyncWindow(e->window);
    ui_set_scale(1.5f);
    ed_samples_open(e);
    frame(e, 780, 520);
    frame(e, 780, 520);
    frame(e, 780, 520);
    auto work = ImGui::GetMainViewport()->WorkSize;
    check(window->Size.x <= work.x + 1 && window->Size.y <= work.y + 1,
          "populated dialog fits the constrained work area");
    capture(e, root, "constrained", check);
    check(ImGui::GetIO().DisplayFramebufferScale.x >= 1.5f, "capture uses 150 percent UI scale");
    check(!ImGui::FindWindowByName("Save sample set"),
          "constrained layout uses the same inline save controls");
    ed_samples_close(e);

    /* Reopen through a fresh tool instance so this exercises disk loading rather than a cache. */
    DmSampleAudio audio = {};
    check(dm_sample_decode("agents/reports/audio-export/codec-0-0.wav", nullptr, &audio, error,
                           sizeof(error)),
          "decode layered-set fixture");
    DmSampleAsset shared = {.id = "shared",
                            .file = "shared.wav",
                            .frames = (uint32_t)audio.frames,
                            .rate = (uint32_t)audio.rate,
                            .channels = (uint8_t)audio.channels,
                            .source_name = "Shared original.wav"};
    DmSampleAsset assets[] = {shared, shared};
    std::snprintf(assets[1].id, sizeof(assets[1].id), "other");
    std::snprintf(assets[1].file, sizeof(assets[1].file), "Other.wav");
    DmSampleAudio layered_audio[] = {audio, audio};
    DmSampleZone zones[3] = {{.root = 60,
                              .key_hi = 63,
                              .velocity_lo = 1,
                              .velocity_hi = 127,
                              .envelope = {.release = .2, .sustain = .8}},
                             {.root = 72,
                              .key_lo = 64,
                              .key_hi = 127,
                              .velocity_lo = 1,
                              .velocity_hi = 127,
                              .envelope = {.release = .3, .sustain = .6}}};
    zones[2] = zones[1];
    zones[2].asset = 1;
    DmSamplePreset layered = {
        .id = "stable-preset", .name = "Layered preset", .zone_count = 3, .suggested_program = 9};
    DmSampleManifest saved_set = {.id = "layered-set",
                                  .name = "Layered set",
                                  .generation = 1,
                                  .asset_count = 2,
                                  .preset_count = 1,
                                  .zone_count = 3,
                                  .assets = assets,
                                  .presets = &layered,
                                  .zones = zones};
    SDL_Delay(2);
    check(dm_sample_library_publish(library, &saved_set, layered_audio, nullptr, error,
                                    sizeof(error)),
          "publish a layered preset with a shared source asset");
    dm_sample_audio_free(&audio);
    ui_set_scale(1);
    SDL_SetWindowSize(e->window, 1400, 900);
    SDL_SyncWindow(e->window);
    Editor *reopened = static_cast<Editor *>(std::calloc(1, sizeof(Editor)));
    check(reopened, "allocate a fresh editor for saved-set loading");
    editor_init(reopened, e->window, e->renderer, e->audio, "agents/reports/", root, true);
    editor_smoke(reopened);
    ed_samples_open(reopened);
    frame(reopened);
    auto *reopened_host = ed_export_host(reopened);
    auto *reopened_tool = dm_export_registry_tool(&reopened_host->registry, DME_SAMPLES_TOOL_ID);
    await_count(reopened, api, reopened_tool, 3);
    DmeSampleDraft loaded = {};
    check(api->count(reopened_tool->instance) == 3 &&
              api->draft(reopened_tool->instance, 0, &loaded) == DME_OK &&
              !std::strcmp(loaded.preset_id.data, "stable-preset"),
          "opening Samples selects and loads the most recently modified set");
    window = ImGui::FindWindowByName("Samples");
    reopened->status[0] = 0;
    activate(reopened, window, "Save");
    end = SDL_GetTicks() + 8000;
    DmSampleManifest round_trip = {};

    do {
        frame(reopened);
        dm_sample_manifest_free(&round_trip);
        dm_sample_library_manifest(library, "layered-set", &round_trip, error, sizeof(error));
        SDL_Delay(2);
    } while (!std::strstr(reopened->status, "Sample set saved") && SDL_GetTicks() < end);

    check(
        round_trip.generation && round_trip.asset_count == 2 && round_trip.preset_count == 1 &&
            round_trip.zone_count == 3 && !std::strcmp(round_trip.presets[0].id, "stable-preset") &&
            !std::strcmp(round_trip.assets[0].id, "shared") && round_trip.zones[0].root == 60 &&
            round_trip.zones[1].root == 72 && round_trip.zones[2].asset == 1 &&
            !std::strcmp(round_trip.assets[0].file, "shared.wav") &&
            !std::strcmp(round_trip.assets[1].file, "Other.wav"),
        "resaving a disk-loaded set preserves preset identity, shared assets and layered regions");
    dm_sample_manifest_free(&round_trip);
    capture(reopened, root, "loaded-set", check);
    editor_free(reopened);
    std::free(reopened);
}
