#include "editor/internal.h"
#ifdef _WIN32
// clang-format off: Windows types must precede shell declarations.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#define TEST_NAME "source_workflow"
#include "test_common.h"
// clang-format on
#endif
void test_source_workflow_ui(Editor *, void (*)(bool, const char *));
void test_source_workflow_focus_workspace(void);

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
    e->tool = TOOL_SELECT;
    e->edit_focus = FOCUS_EVENTS;
    e->automation_cc = 11;
    e->automation_node = -1;
    e->automation_all = false;
    e->automation_raw = false;
    e->curve_drag = e->event_marquee = false;
    e->event_selection.size = 0;
    e->drag = DRAG_NONE;
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
    check(dm_preferences_save(e), "INI save");
    e->preferred_font[0] = 0;
    e->ui_size = 1;
    ed_defaults(e);
    check(dm_preferences_load(e), "INI load");
    check(e->ui_size == 2 && e->accent == 0xff123456 && !e->monitor, "scale/color/output persist");
    check(!strcmp(e->preferred_font, " C:\\fónts\\a % [x]=;# \"q\".sf2 ") &&
              !strcmp(e->recent[0], "C:\\a b.mid"),
          "UTF-8 punctuation, percent, backslashes and quoted strings roundtrip");
    check(e->bindings[ACT_PLAY_MODE].key == SDL_SCANCODE_F9, "shortcut restored");
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
        check(!dm_preferences_load(e) && e->ui_size == 1 && e->accent == 0xff123456,
              "invalid syntax/value/version rejected without partial application");
    }

    const char *valid =
        "; comment\r\n[application]\r\nschema=3\r\nsoundfont=\"é🎹.sf2\"\r\n[future]\r\nkey=value";
    check(SDL_SaveFile(e->settings_path, valid, strlen(valid)) && dm_preferences_load(e),
          "CRLF, comments, UTF-8 and unknown sections work without final newline");
    check(!strcmp(e->preferred_font, "é🎹.sf2"), "UTF-8 path decoded");
    memset(e->preferred_font, '%', DM_PATH - 1);
    e->preferred_font[DM_PATH - 1] = 0;
    check(dm_preferences_save(e) && dm_preferences_load(e) &&
              strlen(e->preferred_font) == DM_PATH - 1,
          "maximum encoded preference field roundtrips");
    e->preferred_font[0] = 0;
    e->ui_size = 1;
    check(dm_preferences_save(e), "leave valid example");
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
    ed_focus_timeline(e, CONTEXT_ROLL);
    check(ed_keyboard_channel(e) == 9, "percussion editing routes to channel 10");
    e->project.channels[2].program = 40;
    e->selected_channel = 2;
    int id = dm_pattern_add(&e->project, "Unplaced");
    ed_open_pattern(e, id, true);
    check(e->preview_program == 40 && e->preview_source_channel == -1,
          "new unplaced pattern inherits selected instrument once");
    ed_playlist_label_action(e, 1, 0, (UiMouse){.clicked = true});
    ed_focus_timeline(e, CONTEXT_ROLL);
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
    check(ed_song_event_rows(e, start, end, -1) == 6 && ed_midi_rows(e) == 11,
          "song list keeps composition events and a collapsed channel's program change while "
          "MIDI Events includes system and routing");
    e->channel_expanded[0] = true;
    check(ed_song_event_rows(e, start, end, -1) == 5,
          "an expanded channel's program change leaves the song list for its own Events row");
    e->channel_expanded[0] = false;
    check(ed_song_event_bar(e, change, &start, &end) == 2 && start == change && end == change + 3 &&
              ed_song_event_rows(e, start, end, -1) == 2,
          "meter and following tempo belong to the new bar");
    ed_song_event_open_bar(e, 1, false, -1);
    check(!e->song_event_add && e->midi_draft.data[1] == MIDI_META_TEMPO,
          "first bar opens on its effective initial tempo");
    int us = (int)llround(60000000.0 / 142);
    e->midi_draft.data[3] = us >> 16;
    e->midi_draft.data[4] = us >> 8;
    e->midi_draft.data[5] = us;
    check(ed_midi_apply_draft(e) && fabs(e->project.bpm - 142) < .001,
          "song event editor updates the effective initial tempo");

    size_t count = ed_song_event_rows(e, 0, change, -1);
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
    e->event_beat = 0;
    check(ed_midi_apply_draft(e) && e->project.events[index].message.size == sizeof(updated) + 2 &&
              !memcmp(e->project.events[index].message.data + 3, updated, sizeof(updated) - 1) &&
              e->project.events[index].track == 2 && e->project.events[index].port == 1,
          "text edit preserves event position and source");
    ed_midi_select(e, (MidiRow){.beat = 0, .index = index});
    e->event_beat = change + 1;
    bool moved = ed_midi_apply_draft(e);
    double start_after, end_after;
    ed_song_event_bar(e, e->song_event_anchor, &start_after, &end_after);
    bool found = false;

    for (size_t i = 0; i < e->project.event_count; ++i) {
        const DmEvent *event = &e->project.events[i];
        found |= event->beat == change + 1 && event->message.data[1] == MIDI_META_TRACK_NAME &&
                 event->track == 2 && event->port == 1;
    }

    check(moved && found && start_after == change,
          "editing moves an event into another bar and the window follows it");
    e->event_beat = 0;
    ed_song_event_open_bar(e, change, false, -1);
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
    check(ed_midi_rows(e) == 8 && ed_song_event_rows(e, 0, 4, -1) == 6 &&
              ed_song_event_rows(e, 0, 4, 0) == 5,
          "Channel event folds duplicate programs without changing the underlying project");
    ed_song_event_open_bar(e, 1, false, 0);
    e->midi_draft.data[1] = 12;
    check(ed_midi_apply_draft(e) && e->project.event_count == 5 &&
              e->project.events[0].message.data[1] == 12 &&
              e->project.events[1].message.data[1] == 4 && ed_song_event_rows(e, 0, 4, 0) == 5,
          "editing a folded row replaces its copies and keeps the independent programs");
    e->song_event = false;
}

static void song_event_selection_checks(Editor *e) {
    fixture(e);
    const uint8_t text[] = {MIDI_META, MIDI_META_TEXT, 1, 'A'};
    DmBytes bytes = {(uint8_t *)text, sizeof(text), sizeof(text)};

    for (int i = 0; i < 4; ++i) {
        check(dm_event_add(&e->project, i * .5 + .5, 0, 0, &bytes), "create song selection rows");
    }

    ed_song_event_open_bar(e, 0, false, -1);
    ed_song_event_pick(e, 2, PICK_TOGGLE);
    check(ed_song_event_pick_count(e) == 2 &&
              ed_song_event_picked(e, ((MidiRow *)e->midi_rows.data)[0]),
          "Ctrl click seeds the current single selection");
    ed_song_event_pick(e, 3, PICK_RANGE);
    check(ed_song_event_pick_count(e) == 2 && e->song_event_pick_anchor == 2,
          "Shift replaces with a range and preserves the Ctrl anchor");
    ed_song_event_pick(e, 0, PICK_ADD_RANGE);
    check(ed_song_event_pick_count(e) == 4 && e->song_event_pick_anchor == 2,
          "Ctrl Shift adds a range and preserves its anchor");
    ed_song_event_pick(e, 1, PICK_TOGGLE);
    check(ed_song_event_pick_count(e) == 3 && e->song_event_pick_anchor == 1,
          "Ctrl toggles a picked row off and moves the anchor");
    ed_song_event_pick(e, 3, PICK_RANGE);
    check(ed_song_event_pick_count(e) == 3 && e->song_event_pick_anchor == 1,
          "a toggled-off row remains the range anchor");
    ed_song_event_pick(e, 0, PICK_ONE);
    check(ed_song_event_pick_count(e) == 1 && e->midi_event_index == 0,
          "plain click replaces all picks with the single-event editor");
    ed_song_event_pick(e, 0, PICK_TOGGLE);
    check(!ed_song_event_pick_count(e) && e->midi_event_index == -1, "Ctrl can leave no selection");
    ed_song_event_pick(e, 0, PICK_ONE);
    ed_song_event_rows(e, 1, 4, -1);
    ed_song_event_pick(e, 1, PICK_ADD_RANGE);
    check(ed_song_event_pick_count(e) == 1 && e->midi_event_index == 2 &&
              e->song_event_pick_anchor == 2,
          "range without a visible anchor behaves as a plain click");
    ++e->export_revision;
    check(!ed_song_event_pick_count(e) && e->song_event_pick_anchor == -1,
          "project revision invalidates picks and the anchor");
    ed_song_event_open_bar(e, 0, false, -1);
    ed_song_event_pick(e, 0, PICK_ONE);
    ed_song_event_pick(e, 1, PICK_TOGGLE);
    check(e->song_event_move_to == ((MidiRow *)e->midi_rows.data)[0].beat,
          "Move to starts at the first picked event's beat");
    int undo = e->undo_count;
    check(ed_song_events_move(e, -20, false) && e->project.events[0].beat == 0 &&
              e->project.events[1].beat == .5 && e->undo_count == undo + 1,
          "relative group movement clamps at zero with one undo step");
    check(ed_song_events_move(e, DM_MAX_BEAT * 2.0, false) &&
              e->project.events[2].beat == DM_MAX_BEAT - .5 &&
              e->project.events[3].beat == DM_MAX_BEAT &&
              ed_song_event_picked(e, (MidiRow){DM_MAX_BEAT, 3, false}) &&
              ed_song_event_picked(e, (MidiRow){DM_MAX_BEAT - .5, 2, false}),
          "relative move clamps the latest event and retains picks after sorting");
    check(ed_song_events_move(e, 1.75, true) && e->project.events[1].beat == 1.75 &&
              e->project.events[2].beat == 1.75 && e->song_event_anchor == 1.75 &&
              ed_song_event_pick_count(e) == 2,
          "absolute move puts every pick on the target and follows the earliest move");
    ed_undo(e, false);
    check(e->project.events[2].beat == DM_MAX_BEAT - .5 &&
              e->project.events[3].beat == DM_MAX_BEAT && !ed_song_event_pick_count(e),
          "undo restores the whole move and invalidates picks");
    ed_song_event_open_bar(e, 0, true, -1);
    e->event_beat = DM_MAX_BEAT;
    check(ed_song_event_position_valid(e), "last representable song event beat is accepted");
    e->event_beat += 1;
    check(!ed_song_event_position_valid(e), "song event beats beyond the MIDI limit are rejected");
    ed_song_event_open_bar(e, 0, false, -1);
    check(!ed_song_event_pick_count(e), "opening a bar clears group picks");
    ed_song_event_pick(e, 1, PICK_RANGE);
    check(ed_song_event_pick_count(e) == 2,
          "Shift click ranges from the event the window opened on");
    ed_song_event_open_bar(e, 0, false, -1);
    e->song_event = false;
    ed_song_event_pick(e, 0, PICK_TOGGLE);
    check(!ed_song_event_pick_count(e), "MIDI Events never gains group picks");
}

static void song_event_group_programs(Editor *e) {
    fixture(e);
    const uint8_t patch[] = {MIDI_PROGRAM, 12};
    DmBytes bytes = {(uint8_t *)patch, sizeof(patch), sizeof(patch)};
    check(dm_event_add(&e->project, 1, 0, 0, &bytes) &&
              dm_event_add(&e->project, 1, 0, 0, &bytes) &&
              dm_event_add(&e->project, 2, 0, 0, &bytes) &&
              dm_event_add(&e->project, 2, 0, 0, &bytes) &&
              dm_event_add(&e->project, 3, 0, 0, &bytes),
          "create source program repeats and an independent event");
    DmAutomation a = {.beat = 1, .type = MIDI_PROGRAM, .a = 24, .pattern = -1};
    check(dm_automation_add(&e->project, a) && dm_automation_add(&e->project, a),
          "create automation program repeats");
    ed_song_event_open_bar(e, 0, false, 0);
    ed_song_event_pick(e, 0, PICK_ONE);
    ed_song_event_pick(e, 1, PICK_TOGGLE);
    ed_song_event_pick(e, 2, PICK_TOGGLE);
    check(ed_song_event_pick_count(e) == 3, "pick source and automation program rows");
    check(ed_song_events_move(e, 0, true) && e->project.event_count == 3 &&
              e->project.automation_count == 1 && e->project.events[0].beat == 0 &&
              e->project.events[1].beat == 0 && e->project.events[2].beat == 3 &&
              e->project.automation[0].beat == 0 && e->project.channels[0].program == 24 &&
              ed_song_event_pick_count(e) == 3 && ed_song_event_picked(e, (MidiRow){0, 0, false}) &&
              ed_song_event_picked(e, (MidiRow){0, 1, false}) &&
              ed_song_event_picked(e, (MidiRow){0, 0, true}),
          "moving folded programs removes hidden repeats and applies initial values");
    ed_undo(e, false);
    check(e->project.event_count == 5 && e->project.automation_count == 2 &&
              e->project.events[0].beat == 1 && e->project.events[1].beat == 1 &&
              e->project.events[2].beat == 2 && e->project.events[3].beat == 2 &&
              e->project.automation[0].beat == 1 && e->project.automation[1].beat == 1,
          "one undo restores both program streams including their hidden repeats");
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
    ui_dialog_raise("Preferences");

    for (int frame = 0; frame < 5; ++frame) {
        ui_frame();
        editor_frame(e, 1400, 900);
        ui_render(e->renderer);
    }

    test_source_workflow_focus_workspace();
    check(!ui_window_focused("Preferences"), "the workspace has focus before the SoundFont drop");
    event.drop.x = (e->preferences_rect.x + e->preferences_rect.w * .5f) * ui_scale();
    event.drop.y = (e->preferences_rect.y + e->preferences_rect.h * .5f) * ui_scale();
    event.drop.data = "agents/reports/generated-sine.sf2";
    editor_event(e, &event);
    check(e->font_request, "unfocused Preferences accepts a SoundFont drop");
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
    Editor *e = test_editor_open("source_workflow", (TestDesktop){0});
    SDL_Renderer *r = e->renderer;
    preferences_check(e);
    ui_frame();
    ui_root(0, 900);
    preview_check(e);
    event_checks(e);
    source_checks(e);
    song_program_repeats(e);
    song_event_selection_checks(e);
    song_event_group_programs(e);
    ui_end();
    ui_render(r);
    drop_checks(e);
    fixture(e);
    test_source_workflow_ui(e, check);
    printf("PASS source_workflow: %d focused checks\n", checks);
    test_editor_close(e);
}
