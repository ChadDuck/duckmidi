#include "editor_internal.h"
#ifdef _WIN32
// clang-format off: Windows types must precede shell declarations.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
// clang-format on
#endif
static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL source_workflow: %s\n", text);
        exit(1);
    }
}

void test_source_workflow_ui(Editor *, void (*)(bool, const char *));

static void fixture(Editor *e) {
    editor_smoke(e);
    e->config = e->preferences = e->midi_events = e->song_event = e->confirm = e->about =
        e->shortcuts = e->export_window = false;
    ed_close_instrument(e);
    e->rename_pattern = -1;
    e->selected_pattern = 0;
    e->pattern_channel = e->selected_channel = 0;
    e->channel_selected = true;
    e->project.automation_count = 0;
    e->project.patterns[0].count = 0;
    e->project.patterns[0].length = 4;
    e->roll = true;
    e->roll_auto_hidden = false;
    e->tool = 0;
    e->context = 2;
    e->edit_focus = FOCUS_EVENTS;
    e->automation_cc = 11;
    e->automation_node = -1;
    e->automation_all = false;
    e->automation_raw = false;
    e->curve_drag = e->event_marquee = false;
    e->event_selection.size = 0;
    e->drag = 0;
}

static void preferences_check(Editor *e) {
    snprintf(e->settings_path, DM_PATH, "agents/reports/source_workflow-settings.ini");
    ed_defaults(e);
    e->accent = 0xff123456;
    e->ui_size = 2;
    e->monitor = false;
    snprintf(e->preferred_font, DM_PATH, " C:\\fónts\\a %% [x]=;# \"q\".sf2 ");
    snprintf(e->recent[0], DM_PATH, "C:\\a b.mid");
    e->recent_count = 1;
    e->bindings[ACT_PLAY_MODE].key = SDL_SCANCODE_F9;
    check(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names), "INI save");
    e->preferred_font[0] = 0;
    e->ui_size = 1;
    ed_defaults(e);
    check(dm_preferences_load(e, ed_shortcut_schema()), "INI load");
    check(e->ui_size == 2 && e->accent == 0xff123456 && !e->monitor, "scale/color/output persist");
    check(!strcmp(e->preferred_font, " C:\\fónts\\a % [x]=;# \"q\".sf2 ") &&
              !strcmp(e->recent[0], "C:\\a b.mid"),
          "UTF-8 punctuation, percent, backslashes and quoted strings roundtrip");
    check(e->bindings[ACT_PLAY_MODE].key == SDL_SCANCODE_F9, "shortcut restored");
    ed_defaults(e);
    check(dm_preferences_load(e, ed_shortcut_schema() + 1) &&
              e->bindings[ACT_PLAY_MODE].key == SDL_SCANCODE_F9,
          "changed shortcut list keeps saved shortcuts and other settings");
    const char *bad[] = {"{\"schema\":1}",
                         "[application]\nschema=1",
                         "[application]\nschema=3\nui_size=3",
                         "[application]\nschema=3\nmonitor_soundfont=2",
                         "[application]\nschema=3\nsoundfont=%00",
                         "[application]\nschema=3\nsoundfont=%xx",
                         "[application\nschema=3",
                         "[application]\nschema=3\n[shortcuts]\naction_0=1,0\naction_0=2,0"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        check(SDL_SaveFile(e->settings_path, bad[i], strlen(bad[i])), "write invalid isolated INI");
        e->ui_size = 1;
        check(!dm_preferences_load(e, ed_shortcut_schema()) && e->ui_size == 1 &&
                  e->accent == 0xff123456,
              "invalid syntax/value/version rejected without partial application");
    }

    const char *valid =
        "; comment\r\n[application]\r\nschema=3\r\nsoundfont=\"é🎹.sf2\"\r\n[future]\r\nkey=value";
    check(SDL_SaveFile(e->settings_path, valid, strlen(valid)) &&
              dm_preferences_load(e, ed_shortcut_schema()),
          "CRLF, comments, UTF-8 and unknown sections work without final newline");
    check(!strcmp(e->preferred_font, "é🎹.sf2"), "UTF-8 path decoded");
    memset(e->preferred_font, '%', DM_PATH - 1);
    e->preferred_font[DM_PATH - 1] = 0;
    check(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names) &&
              dm_preferences_load(e, ed_shortcut_schema()) &&
              strlen(e->preferred_font) == DM_PATH - 1,
          "maximum encoded preference field roundtrips");
    e->preferred_font[0] = 0;
    e->ui_size = 1;
    check(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names), "leave valid example");
}

static void preview_check(Editor *e) {
    fixture(e);
    e->project.clips[0].channel = 9;
    ed_open_pattern(e, 0, false);
    ed_bind_pattern_channel(e);
    check(e->preview_program == 128 && e->pattern_channel == 9 &&
              ed_pattern_color(e, 0) == e->accent,
          "percussion preview uses the placed channel; new patterns use the application color");
    ed_playlist_label_action(e, 1, 0, (UiMouse){.clicked = true});
    check(e->preview_program == 128 && e->pattern_channel == 9,
          "unrelated channel selection preserves open preview/channel");
    e->context = 2;
    check(ed_keyboard_channel(e) == 9, "percussion editing routes to channel 10");
    e->project.channels[2].program = 40;
    e->selected_channel = 2;
    int id = dm_pattern_add(&e->project, "Unplaced");
    ed_open_pattern(e, id, true);
    check(e->preview_program == 40 && e->preview_source_channel == -1,
          "new unplaced pattern inherits selected instrument once");
    ed_playlist_label_action(e, 1, 0, (UiMouse){.clicked = true});
    e->context = 2;
    ed_keyboard_channel(e);
    check(e->pattern_channel == 1 && e->preview_program == ed_channel_program(e, 1),
          "unplaced preview follows the selected row's instrument");
    e->preview_program = 12;
    e->preview_override = true;
    ed_keyboard_channel(e);
    check(e->preview_program == 12 && e->project.channels[2].program == 40,
          "manual preview never changes project program");
    e->project.clips[1].pattern = 0;
    e->project.clips[1].channel = 1;
    e->drag_edge = 0;
    ed_playlist_hit_click(e, (UiMouse){0}, 1, 4);
    check(e->preview_program == e->project.channels[1].program && e->pattern_channel == 1,
          "clicked placement overrides first matching placement on open");
    e->preview_program = 16;
    e->preview_override = true;
    ed_choose_pattern_card(e, 0, true);
    check(e->preview_program == 16 && e->preview_override,
          "reselecting the same pattern preserves manual preview override");
    ed_playlist_hit_click(e, (UiMouse){0}, 1, 4);
    check(e->preview_program == 16 && e->preview_override,
          "another placement of the same pattern preserves explicit override");
}

static void event_checks(Editor *e) {
    fixture(e);
    UiRect lane = {100, 100, 600, 200};
    View view = {1, 0, 0, 0};
    dm_automation_add(&e->project, (DmAutomation){0, 0xb0, 0, 11, 30, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){2, 0xb0, 0, 11, 90, 0, 0, 0, 20, true});
    dm_automation_add(&e->project, (DmAutomation){4, 0xb0, 0, 11, 60, 0, 0, 0, 0, false});
    ed_automation_lane_click(e, lane, &view, 80,
                             (UiMouse){.pos = {340, 180}, .clicked = true, .shift = true}, -1);
    ed_automation_press(e, lane, &view, 80, (UiMouse){.pos = {341, 181}, .released = true});
    bool connected = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        connected |= a.beat == 3 && a.type == 0xb0 && a.a == 11 && a.curve > 0;
    }

    check(connected, "Shift + click adds a connected point at the cursor");
    e->project.automation_count = 0;
    dm_automation_add(&e->project, (DmAutomation){0, 0xb0, 0, 11, 30, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){2, 0xb0, 0, 11, 90, 0, 0, 0, 20, true});
    dm_automation_add(&e->project, (DmAutomation){4, 0xb0, 0, 11, 60, 0, 0, 0, 0, false});
    check(ed_insert_lane_node(e, 3) && e->project.automation_count == 4,
          "curve insertion splits a ramp");
    e->event_marquee_start = (UiPoint){90, 90};
    e->event_marquee = true;
    ed_event_box_select(e, lane, &view, 80, (UiMouse){.pos = {500, 310}, .left = true});
    check(e->event_selection.size == 4 * sizeof(DmAutomation),
          "rectangle selects event points without changing content");
    ed_event_box_select(e, lane, &view, 80, (UiMouse){.pos = {500, 310}, .released = true});
    check(!e->event_marquee, "rectangle release ends gesture");
    ed_delete_events(e);
    check(e->project.automation_count == 0, "event selection integrates with deletion");
    e->roll_view.scroll_x = 80000;
    ed_grid_scroll(e, &e->roll_view, lane, 100, 2560, 20, false);
    check(e->roll_view.scroll_x == 80000, "short pattern can scroll arbitrarily beyond content");
    e->roll_view.scroll_x = -10;
    ed_grid_scroll(e, &e->roll_view, lane, 100, 2560, 20, false);
    check(e->roll_view.scroll_x == 0, "timeline cannot scroll before start");
}

static void source_checks(Editor *e) {
    fixture(e);
    uint8_t tempo[] = {0xff, 0x51, 3, 7, 0xa1, 0x20};
    DmBytes bytes = {tempo, sizeof(tempo), sizeof(tempo)};
    dm_event_add(&e->project, 0, 0, 0, &bytes);
    dm_event_add(&e->project, 4, 0, 0, &bytes);
    const uint8_t meter[] = {MIDI_META, MIDI_META_TIME, 4, 3, 2, 36, 8};
    const uint8_t name[] = {MIDI_META, MIDI_META_TRACK_NAME, 1, 'A'};
    const uint8_t text[] = {MIDI_META, MIDI_META_TEXT, 1, 'B'};
    const uint8_t smpte[] = {MIDI_META, MIDI_META_SMPTE, 5, 0x40, 2, 3, 4, 5};
    const uint8_t key[] = {MIDI_META, MIDI_META_KEY, 2, 0, 0};
    const uint8_t sysex[] = {MIDI_SYSEX, 1, MIDI_SYSEX_END};
    const uint8_t port[] = {MIDI_META, MIDI_META_PORT, 1, 0};
    const uint8_t controller[] = {MIDI_CONTROL, MIDI_CC_EXPRESSION, 90};
    const double change = 2.125;

    const struct {
        const uint8_t *data;
        size_t size;
        double beat;
    } fixtures[] = {{name, sizeof(name), 0},
                    {text, sizeof(text), 1},
                    {smpte, sizeof(smpte), 0},
                    {key, sizeof(key), 1.5},
                    {sysex, sizeof(sysex), 1},
                    {port, sizeof(port), 1},
                    {controller, sizeof(controller), 1},
                    {meter, sizeof(meter), change}};

    for (size_t i = 0; i < sizeof(fixtures) / sizeof(*fixtures); ++i) {
        bytes = (DmBytes){(uint8_t *)fixtures[i].data, fixtures[i].size, fixtures[i].size};
        check(dm_event_add(&e->project, fixtures[i].beat, 2, 1, &bytes), "add event fixture");
    }

    check(
        dm_automation_add(
            &e->project, (DmAutomation){.beat = 1.5, .type = MIDI_PROGRAM, .a = 40, .pattern = -1}),
        "add song-level instrument change");
    double start, end;
    check(ed_song_event_bar(e, 1, &start, &end) == 1 && start == 0 && end == change,
          "a meter change ends the preceding bar at its exact beat");
    check(ed_song_event_rows(e, start, end) == 6 && ed_midi_rows(e) == 11,
          "song list keeps composition events while MIDI Events includes system and routing");
    check(ed_song_event_bar(e, change, &start, &end) == 2 && start == change && end == change + 3 &&
              ed_song_event_rows(e, start, end) == 2,
          "meter and following tempo belong to the new bar");
    ed_song_event_open_bar(e, 1, false);
    check(!e->song_event_add && e->midi_draft.data[1] == MIDI_META_TEMPO,
          "first bar opens on its effective initial tempo");
    int us = (int)llround(60000000.0 / 142);
    e->midi_draft.data[3] = us >> 16;
    e->midi_draft.data[4] = us >> 8;
    e->midi_draft.data[5] = us;
    check(ed_midi_apply_draft(e) && fabs(e->project.bpm - 142) < .001,
          "song event editor updates the effective initial tempo");

    size_t count = ed_song_event_rows(e, 0, change);
    MidiRow *rows = (MidiRow *)e->midi_rows.data;
    int index = -1;

    for (size_t i = 0; i < count; ++i) {
        if (!rows[i].automation &&
            e->project.events[rows[i].index].message.data[1] == MIDI_META_TRACK_NAME) {
            index = rows[i].index;
            ed_midi_select(e, rows[i]);
            break;
        }
    }

    check(index >= 0 && e->midi_source_track == 2 && e->midi_source_port == 1,
          "track name editor loads the selected event and its source");
    const uint8_t updated[] = "Renamed track";
    check(ed_midi_replace_payload(e, updated, sizeof(updated) - 1), "edit track-name payload");
    e->event_beat = change;
    int history = e->undo_count;
    check(!ed_midi_apply_draft(e) && e->undo_count == history && *e->midi_error,
          "editing cannot move an event outside its selected bar");
    e->event_beat = 0;
    check(ed_midi_apply_draft(e) && e->project.events[index].message.size == sizeof(updated) + 2 &&
              !memcmp(e->project.events[index].message.data + 3, updated, sizeof(updated) - 1) &&
              e->project.events[index].track == 2 && e->project.events[index].port == 1,
          "text edit preserves event position and source");
    ed_song_event_open_bar(e, change, false);
    check(e->midi_draft.data[1] == MIDI_META_TIME, "meter starts the next bar's event list");
    index = e->midi_event_index;
    e->midi_draft.data[3] = 5;
    check(ed_midi_apply_draft(e) && dm_meter(&e->project, change).bar == 5 &&
              e->project.events[index].message.data[5] == 36,
          "meter edit changes the bar length and preserves metronome ticks");
    e->song_event = false;
    check(dm_automation_add(&e->project, (DmAutomation){.type = MIDI_PROGRAM, .pattern = -1}),
          "add initial instrument fixture");
    ed_midi_select(e, (MidiRow){0, 0, true});
    e->midi_draft.data[1] = 24;
    check(ed_midi_apply_draft(e) && e->project.channels[0].program == 24,
          "initial program editing changes the channel's effective instrument");
}

static void song_program_repeats(Editor *e) {
    fixture(e);
    const uint8_t patch[] = {MIDI_PROGRAM, 4};
    const uint8_t other_channel[] = {MIDI_PROGRAM | 1, 4};
    DmBytes program = {(uint8_t *)patch, sizeof(patch), sizeof(patch)};
    check(dm_event_add(&e->project, 1, 0, 0, &program) &&
              dm_event_add(&e->project, 1, 0, 0, &program) &&
              dm_event_add(&e->project, 1, 1, 0, &program) &&
              dm_event_add(&e->project, 1, 1, 1, &program) &&
              dm_event_add(&e->project, 2, 0, 0, &program),
          "create repeated programs and independent track, port and time fixtures");
    program = (DmBytes){(uint8_t *)other_channel, sizeof(other_channel), sizeof(other_channel)};
    check(dm_event_add(&e->project, 1, 0, 0, &program), "create independent channel fixture");
    DmAutomation a = {.beat = 1, .type = MIDI_PROGRAM, .a = 4, .pattern = -1};
    check(dm_automation_add(&e->project, a) && dm_automation_add(&e->project, a),
          "create repeated song-level program automation");
    check(ed_midi_rows(e) == 8 && ed_song_event_rows(e, 0, 4) == 6,
          "Song event folds duplicate programs without changing the underlying project");
    ed_song_event_open_bar(e, 1, false);
    e->midi_draft.data[1] = 12;
    check(ed_midi_apply_draft(e) && e->project.event_count == 5 &&
              e->project.events[0].message.data[1] == 12 &&
              e->project.events[1].message.data[1] == 4 && ed_song_event_rows(e, 0, 4) == 6,
          "editing a folded row replaces its copies and keeps the independent programs");
    e->song_event = false;
}

static void native_drop(Editor *e, const char *path) {
#ifdef _WIN32
    wchar_t wide[DM_PATH];
    int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, DM_PATH);
    check(length > 0, "native drop UTF-8 path");
    size_t bytes = sizeof(DROPFILES) + (length + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GHND, bytes);
    check(memory != NULL, "native drop allocation");
    DROPFILES *drop = GlobalLock(memory);
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    drop->pt.x = 30;
    drop->pt.y = 30;
    memcpy((char *)drop + sizeof(DROPFILES), wide, length * sizeof(wchar_t));
    GlobalUnlock(memory);
    HWND hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(e->window),
                                       SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    check(hwnd != NULL, "actual native HWND");
    SendMessageW(hwnd, WM_DROPFILES, (WPARAM)memory, 0);
    SDL_Event event;
    bool received = false;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_DROP_FILE) {
            received = true;
        }

        editor_event(e, &event);
    }

    check(received, "Windows native drop message produces an SDL file drop");
#else
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_DROP_FILE;
    event.drop.data = path;
    editor_event(e, &event);
#endif
}

static void drop_checks(Editor *e) {
    fixture(e);

    for (int i = 0; i < 15; ++i) {
        char path[100];
        snprintf(path, sizeof(path), "agents/reports/recent-%d.mid", i);
        ed_add_recent(e, path);
    }

    check(e->recent_count == 10 && strstr(e->recent[0], "14.mid") && strstr(e->recent[9], "5.mid"),
          "recent files are capped at ten, newest first");
    ed_add_recent(e, "agents/reports/recent-8.mid");
    check(e->recent_count == 10 && strstr(e->recent[0], "8.mid") && strstr(e->recent[1], "14.mid"),
          "reopening an existing recent promotes it without duplication");
    DmBytes midi = {0}, project = {0};
    char error[256];
    dm_note_add(ed_pattern(e), (DmNote){0, 1, 60, 100, 0});
    check(dm_midi(&e->project, &midi, error, sizeof(error)) &&
              dm_write("agents/reports/source_workflow-drop.mid", midi.data, midi.size, error,
                       sizeof(error)),
          "generated drop MIDI");
    check(dm_pack(&e->project, &project) &&
              dm_write("agents/reports/source_workflow-drop.duck", project.data, project.size,
                       error, sizeof(error)),
          "generated drop project");
    e->active = e->dirty = false;
    native_drop(e, "agents/reports/source_workflow-drop.mid");
    check(e->active && e->project.pattern_count > 0, "MIDI drop opens from the welcome state");
    e->dirty = false;
    native_drop(e, "agents/reports/source_workflow-drop.duck");
    check(e->active && strstr(e->path, "source_workflow-drop.duck"),
          "project drop opens through the native event path");
    e->dirty = true;
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_DROP_FILE;
    event.drop.data = "agents/reports/source_workflow-drop.mid";
    editor_event(e, &event);
    check(e->confirm && e->pending_action == PENDING_PATH,
          "drop retains unsaved-project confirmation");
    e->confirm = false;
    e->pending_action = 0;
    e->dirty = false;
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio != NULL, "drop audio backend");
    e->audio = audio;
    e->preferences = true;
    e->preferences_raise = true;

    for (int frame = 0; frame < 5; ++frame) {
        ui_frame();
        editor_frame(e, 1400, 900);
        ui_render(e->renderer);
    }

    event.drop.x = (e->preferences_rect.x + e->preferences_rect.w * .5f) * ui_scale();
    event.drop.y = (e->preferences_rect.y + e->preferences_rect.h * .5f) * ui_scale();
    event.drop.data = "agents/reports/generated-sine.sf2";
    editor_event(e, &event);
    check(e->font_request, "SoundFont drop in Preferences starts background loading");
    uint64_t limit = SDL_GetTicks() + 15000;

    while (dm_audio_busy(audio) && SDL_GetTicks() < limit) {
        SDL_Delay(5);
    }

    editor_background(e);
    check(!e->font_request && !strcmp(e->project.soundfont, "agents/reports/generated-sine.sf2"),
          "dropped SoundFont installs through the existing background worker");
    event.drop.x = 20;
    event.drop.y = 20;
    editor_event(e, &event);
    check(strstr(e->status, "onto Preferences") != NULL,
          "SoundFont outside Preferences is rejected with feedback");
    event.type = SDL_EVENT_DROP_BEGIN;
    editor_event(e, &event);
    check(e->drop_active && !e->drop_handled, "drag begin arms visible target feedback");
    event.type = SDL_EVENT_DROP_COMPLETE;
    editor_event(e, &event);
    check(!e->drop_active, "drag leave/completion clears feedback");
    e->audio = NULL;
    dm_audio_destroy(audio);
    e->preferences = false;
    dm_bytes_free(&midi);
    dm_bytes_free(&project);
}

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("source_workflow", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", "", true);
    preferences_check(e);
    ui_frame();
    ui_root(0, 900);
    preview_check(e);
    event_checks(e);
    source_checks(e);
    song_program_repeats(e);
    ui_end();
    ui_render(r);
    drop_checks(e);
    fixture(e);
    test_source_workflow_ui(e, check);
    printf("PASS source_workflow: %d focused checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
