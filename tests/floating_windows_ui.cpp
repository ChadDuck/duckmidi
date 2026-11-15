/* Drives the floating windows: they own the clicks that land on them, open at the size their
   contents need and close on Enter where they are forms. */
#include "test_ui.h"
extern "C" {
#include "devtools/devtools.h"
}
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
    ui_frame();
    ImGui::LogToBuffer();
    ed_shortcuts(e);
    const char *logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
    assert(std::strstr(logged, DM_TEXT_FIXED_MOUSE_GESTURES) &&
           std::strstr(logged, ed_gestures[GESTURE_NOTE_BEND].label));
    ImGui::LogFinish();
    ui_render(e->renderer);
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

#ifdef DM_DEVTOOLS
static void profiler_window(Editor *e, Check check) {
    ed_action(e, ACT_PROFILER);
    frame(e);
    frame(e);
    frame(e);
    auto *window = find_window(DM_TEXT_FRAME_PROFILER);
    check(window && !window->Hidden, "profiler action opens its floating window");
    check(window->Size.x > ImGui::CalcTextSize(DM_TEXT_PROFILER_INCLUSIVE).x &&
              window->ScrollMax.x == 0,
          "profiler contents fit the floating window");
    capture(e, "agents/reports/frame-profiler.bmp");
    check(!ui_dialog_open(), "profiler tool window leaves workspace shortcuts available");
    double average, worst;
    int calls;
    check(dm_profile_stats(DM_PROFILE_UI_BUILD, 2, &average, &worst, &calls) && calls >= 3 &&
              average > 0 && worst >= average,
          "direct editor frames record ui_build without a main-loop frame end");
    int before = calls;
    double average_before = average, worst_before = worst;
    DM_PROFILE_BEGIN(UI_BUILD, nested_build);
    frame(e);
    frame(e);
    dm_profile_stats(DM_PROFILE_UI_BUILD, 2, &average, &worst, &calls);
    check(calls == before && average == average_before && worst == worst_before,
          "nested section intervals do not record separate time or calls");
    DM_PROFILE_END(UI_BUILD, nested_build);
    dm_profile_stats(DM_PROFILE_UI_BUILD, 2, &average, &worst, &calls);
    check(calls == before + 1, "outermost section interval records exactly one call");
    dm_profile_frame_end();
    check(ed_checkpoint(e), "profiler edit checkpoints");
    e->project.channels[0].volume = 90;
    ed_commit(e);
    const int edits[] = {DM_PROFILE_CHECKPOINT, DM_PROFILE_CHANGED, DM_PROFILE_VALIDATE,
                         DM_PROFILE_PUBLISH};

    for (int section : edits) {
        check(dm_profile_stats(section, 2, &average, &worst, &calls) && calls > 0,
              "edit sections report calls in the profiler");
    }

    dm_profile_frame_end();
    check(dm_profile_stats(DM_PROFILE_CHANGED, 2, &average, &worst, &calls) && calls > 0,
          "completed frame retains edit measurements");
    ed_shortcut(e, SDL_SCANCODE_F12, 0, false);
    frame(e);
    check(!e->profiler && !find_window(DM_TEXT_FRAME_PROFILER), "F12 hides the profiler");
}

static void gesture_inspector_window(Editor *e, Check check) {
    ed_action(e, ACT_GESTURE_INSPECTOR);
    frame(e);
    frame(e);
    frame(e);
    auto *window = find_window(DM_TEXT_GESTURE_INSPECTOR);
    check(window && !window->Hidden, "gesture inspector action opens its floating window");
    check(!ui_dialog_open(), "gesture inspector tool window leaves workspace shortcuts available");
    ImGui::SetWindowPos(window, {20, 40});
    e->roll = true;
    e->roll_auto_hidden = false;
    e->automation_open = false;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_NOTES;
    e->tool = TOOL_SELECT;
    e->roll_view = {1, 0, (127 - 65) * 20.0f, 0};
    e->focus_pattern = false;
    frame(e);
    auto *roll = find_window("/Piano roll_");
    check(roll != nullptr, "piano roll renders for gesture inspector");
    float gx = roll->Pos.x + roll->WindowPadding.x + 64;
    float gy = roll->Pos.y + roll->WindowPadding.y + 32 + ImGui::GetStyle().ItemSpacing.y + 28;
    float x = gx + 16 - e->roll_view.scroll_x;
    float y = gy + (127 - 60) * 20 - e->roll_view.scroll_y + 10;
    frame(e, x, y, false, false, false, true);
    check(e->hover_target == TARGET_NOTE, "hovering a note sets TARGET_NOTE");
    check(dm_gesture_inspector_lost_reason(GESTURE_COPY_NOTES) != nullptr,
          "hovering a note records lost reason for copy notes");
    frame(e, x, y, true, false, false, true);
    check(dm_gesture_inspector_queried(GESTURE_NOTE_BEND) &&
              dm_gesture_inspector_won(GESTURE_NOTE_BEND),
          "Shift+press on a note records GESTURE_NOTE_BEND as won");
    check(dm_gesture_inspector_queried(GESTURE_EXTEND) && dm_gesture_inspector_won(GESTURE_EXTEND),
          "Shift+press on a note records GESTURE_EXTEND as won");
    frame(e, x, y, false, false, false, false);
    ed_shortcut(e, SDL_SCANCODE_F11, 0, false);
    frame(e);
    check(!e->gesture_inspector && !find_window(DM_TEXT_GESTURE_INSPECTOR),
          "F11 hides the gesture inspector");
}

static void input_recorder_round_trip(Editor *e, Check check) {
    SDL_CreateDirectory("agents/reports");
    const char *path = "agents/reports/input-log-test.txt";
    dm_input_log_start(1400, 900);
    SDL_Event ev = {};
    /* The release of the F10 that started recording has no recorded press: it is left out. */
    ev.type = SDL_EVENT_KEY_UP;
    ev.key.scancode = SDL_SCANCODE_F10;
    check(!dm_input_log_record(&ev, 0, nullptr, nullptr, 0),
          "a key release without a recorded press is not recorded");

    ev = {};
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.x = 120.5f;
    ev.motion.y = 240.25f;
    check(dm_input_log_record(&ev, 0, nullptr, nullptr, 0), "record mouse motion");

    ev = {};
    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 120.5f;
    ev.button.y = 240.25f;
    check(dm_input_log_record(&ev, 1, nullptr, nullptr, 0), "record mouse down");

    ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
    check(dm_input_log_record(&ev, 2, nullptr, nullptr, 0), "record mouse up");

    ev = {};
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.scancode = SDL_SCANCODE_F1;
    ev.key.mod = 0;
    ev.key.repeat = false;
    check(dm_input_log_record(&ev, 3, nullptr, nullptr, 0), "record key down");

    ev.type = SDL_EVENT_KEY_UP;
    check(dm_input_log_record(&ev, 4, nullptr, nullptr, 0), "record key up");

    ev = {};
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.text = "hello world";
    check(dm_input_log_record(&ev, 5, nullptr, nullptr, 0), "record text input");

    check(dm_input_log_stop_to(path), "save input recording");
    DmInputLog loaded = {};
    check(dm_input_log_load(&loaded, path), "load input recording");
    check(loaded.width == 1400 && loaded.height == 900 && loaded.count == 6,
          "loaded recording preserves header and event count");
    check(loaded.events[0].frame == 0 && loaded.events[0].kind == DM_INPUT_MOTION &&
              loaded.events[0].motion.x == 120.5f && loaded.events[0].motion.y == 240.25f,
          "motion event round-trips");
    check(loaded.events[1].frame == 1 && loaded.events[1].kind == DM_INPUT_BUTTON_DOWN &&
              loaded.events[1].button.button == SDL_BUTTON_LEFT &&
              loaded.events[1].button.x == 120.5f && loaded.events[1].button.y == 240.25f,
          "button_down event round-trips");
    check(loaded.events[2].frame == 2 && loaded.events[2].kind == DM_INPUT_BUTTON_UP &&
              loaded.events[2].button.button == SDL_BUTTON_LEFT,
          "button_up event round-trips");
    check(loaded.events[3].frame == 3 && loaded.events[3].kind == DM_INPUT_KEY_DOWN &&
              loaded.events[3].key.scancode == SDL_SCANCODE_F1 && !loaded.events[3].key.mod &&
              !loaded.events[3].key.repeat,
          "key_down event round-trips");
    check(loaded.events[4].frame == 4 && loaded.events[4].kind == DM_INPUT_KEY_UP &&
              loaded.events[4].key.scancode == SDL_SCANCODE_F1,
          "key_up event round-trips");
    check(loaded.events[5].frame == 5 && loaded.events[5].kind == DM_INPUT_TEXT &&
              !std::strcmp(loaded.events[5].text.text, "hello world"),
          "text event round-trips");

    bool piano_before = e->piano;
    SDL_WindowID window_id = SDL_GetWindowID(e->window);
    size_t cursor = 0;

    for (uint64_t f = 0; f <= 5; ++f) {
        while (cursor < loaded.count && loaded.events[cursor].frame == f) {
            SDL_Event injected = {};
            check(dm_input_log_to_sdl(&loaded.events[cursor], window_id, &injected),
                  "rebuild SDL event for replay");
            ui_event(&injected);
            editor_event(e, &injected);
            ++cursor;
        }

        ui_frame();
        editor_frame(e, 1400, 900);
        SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
        SDL_RenderClear(e->renderer);
        ui_render(e->renderer);
    }

    check(e->piano == !piano_before, "replayed F1 toggles computer keyboard piano");
    e->piano = piano_before;
    dm_input_log_free(&loaded);

    /* Replay waits for each frame in turn: frames that go backwards or lie absurdly far ahead
       would stall it, so such a recording is refused. */
    const char *bad[] = {"duckmidi-input 1 1400 900\n5 motion 1 1\n3 motion 2 2\n",
                         "duckmidi-input 1 1400 900\n99999999999 motion 1 1\n"};

    for (const char *text : bad) {
        SDL_IOStream *file = SDL_IOFromFile(path, "w");
        check(file && SDL_WriteIO(file, text, strlen(text)) == strlen(text) && SDL_CloseIO(file),
              "write a malformed recording");
        check(!dm_input_log_load(&loaded, path) && !loaded.count,
              "a recording whose frames go backwards or too far ahead is refused");
    }

    SDL_RemovePath(path);
}
#endif

extern "C" void test_floating_windows_ui(Editor *e, Check check) {
    covered_clicks(e);
    generate_window(e);
    list_widths(e);
    recent_submenu(e);
    project_settings(e);
    song_event_window(e, check);
#ifdef DM_DEVTOOLS
    profiler_window(e, check);
    gesture_inspector_window(e, check);
    input_recorder_round_trip(e, check);
#endif
}
