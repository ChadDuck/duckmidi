// Exercise the production editor's internal boundaries, without adding a second app entry point.
#include "editor_internal.h"
static int checks;

static void check(bool ok, const char *label) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL transport_shortcuts: %s\n", label);
        exit(1);
    }
}

static void key(Editor *e, SDL_Scancode code, SDL_Keymod mods) {
    ed_shortcut(e, code, mods, false);
}

static void playback(Editor *e) {
    editor_smoke(e);
    bool visible;
    check(fabs(ed_view_playhead(e, 1, true, 1.5, &visible) - 1.5) < 1e-8 && visible,
          "arrangement drives local pattern playhead");
    check(fabs(ed_view_playhead(e, 1, true, 4.25, &visible) - .25) < 1e-8 && visible,
          "next copy restarts local playhead");
    check(fabs(ed_view_playhead(e, 0, true, 4.25, &visible) - 4.25) < 1e-8,
          "arrangement keeps absolute song position");
    ed_view_playhead(e, 1, true, 80, &visible);
    check(!visible, "hide local playhead outside all placements");
    e->project.patterns[0].length = 100;
    check(fabs(ed_view_playhead(e, 1, true, 70, &visible) - 58) < 1e-8,
          "long patterns use full local position, latest overlapping copy wins");
    e->project.clip_count = 1;
    check(fabs(ed_view_playhead(e, 1, true, 70, &visible) - 70) < 1e-8,
          "one long imported pattern tracks entire song");
    e->pattern_mode = true;
    check(fabs(ed_view_playhead(e, 1, true, 2.5, &visible) - 2.5) < 1e-8,
          "standalone pattern playback keeps local position");
}

static void shortcuts_added(Editor *e) {
    editor_smoke(e);
    e->context = 1;
    e->edit_focus = FOCUS_PLAYLIST;
    key(e, SDL_SCANCODE_M, 0);
    check(e->project.channels[0].mute && !e->project.channels[0].solo,
          "M mutes selected playlist channel");
    key(e, SDL_SCANCODE_SLASH, 0);
    check(!e->project.channels[0].mute && e->project.channels[0].solo, "/ replaces mute with solo");
    key(e, SDL_SCANCODE_M, 0);
    check(e->project.channels[0].mute && !e->project.channels[0].solo, "M replaces solo with mute");
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    key(e, SDL_SCANCODE_M, 0);
    check(e->project.channels[0].mute, "mute shortcut requires playlist focus");
    e->piano = true;
    key(e, SDL_SCANCODE_S, 0);
    check(!e->project.channels[0].solo && e->held[SDL_SCANCODE_S] == 49,
          "piano S plays lower C sharp without soloing");
    ed_release_piano(e);
    key(e, SDL_SCANCODE_F2, 0);
    key(e, SDL_SCANCODE_F3, 0);
    key(e, SDL_SCANCODE_F4, 0);
    check(e->metronome && e->wait_input && e->overwrite, "F2-F4 work with piano enabled");
    key(e, SDL_SCANCODE_R, 0);
    check(!e->record && e->held[SDL_SCANCODE_R] == 65, "R plays F4 while piano enabled");
    ed_release_piano(e);
    key(e, SDL_SCANCODE_F5, 0);
    check(e->record, "F5 records with piano enabled");
    key(e, SDL_SCANCODE_F5, 0);
    e->piano = false;
    key(e, SDL_SCANCODE_R, 0);
    check(e->record, "R records with piano disabled");
    key(e, SDL_SCANCODE_R, 0);
    bool roll = e->roll;
    int selected = e->selected_pattern;
    size_t patterns = e->project.pattern_count;
    check(ed_pattern(e) && !ed_pattern(e)->kind, "a note pattern is selected");
    e->roll = true;
    e->automation_open = false;
    key(e, SDL_SCANCODE_E, SDL_KMOD_SHIFT);
    check(e->automation_open && !e->song_event,
          "Shift E opens the pattern editor's Events section");
    key(e, SDL_SCANCODE_E, SDL_KMOD_SHIFT);
    check(!e->automation_open, "Shift E hides the Events section again");
    e->roll = false;
    key(e, SDL_SCANCODE_E, SDL_KMOD_SHIFT);
    check(e->roll && ed_pattern(e) && ed_pattern(e)->kind && !e->song_event,
          "Shift E opens the Event editor when the pattern editor is hidden");
    key(e, SDL_SCANCODE_E, SDL_KMOD_SHIFT);
    check(!e->roll, "Shift E hides the Event editor again");

    if (e->project.pattern_count > patterns) {
        dm_pattern_remove(&e->project, (int)patterns);
    }

    e->selected_pattern = selected;
    e->roll = roll;

    /* I opens the instrument window for the focused channel; the piano keyboard keeps I. */
    e->piano = false;
    e->selected_channel = 1;
    e->pattern_channel = 0;
    ed_focus_edit(e, FOCUS_PLAYLIST);
    key(e, SDL_SCANCODE_I, 0);
    check(e->instrument_channel == 1 && e->instrument_preview,
          "I opens the playlist channel's instrument window");
    e->instrument_channel = -1;
    e->instrument_preview = false;
    ed_focus_edit(e, FOCUS_EVENTS);
    key(e, SDL_SCANCODE_I, 0);
    check(e->instrument_channel == 0, "I opens the pattern editor channel's instrument window");
    e->instrument_channel = -1;
    e->instrument_preview = false;
    ed_focus_edit(e, FOCUS_PATTERNS);
    key(e, SDL_SCANCODE_I, 0);
    check(e->instrument_channel == -1, "I does nothing in the pattern section");
    ed_focus_edit(e, FOCUS_PLAYLIST);
    e->piano = true;
    key(e, SDL_SCANCODE_I, 0);
    check(e->instrument_channel == -1, "I plays the keyboard piano while it is on");
    ed_release_piano(e);
    e->piano = false;

    /* P opens Preferences in front of an open window, from any section; S opens Project
       settings. Both belong to the keyboard piano while it is on. */
    e->shortcuts = true;
    ed_focus_edit(e, FOCUS_OTHER);
    key(e, SDL_SCANCODE_P, 0);
    check(e->preferences && e->preferences_raise, "P opens Preferences over another window");
    e->preferences = e->preferences_raise = e->shortcuts = false;
    key(e, SDL_SCANCODE_S, 0);
    check(e->config, "S opens Project settings");
    e->config = false;
    e->piano = true;
    key(e, SDL_SCANCODE_P, 0);
    check(!e->preferences, "P plays the keyboard piano while it is on");
    ed_release_piano(e);
    e->piano = false;
    e->bindings[ACT_METRONOME] = (Binding){SDL_SCANCODE_F8, SDL_KMOD_ALT};
    key(e, SDL_SCANCODE_F8, SDL_KMOD_ALT);
    check(!e->metronome, "new shortcuts are customizable");
    ed_defaults(e);
    check(ed_piano_key(SDL_SCANCODE_Z) == 48 && ed_piano_key(SDL_SCANCODE_Q) == 60 &&
              ed_piano_key(SDL_SCANCODE_P) == 76,
          "two octaves plus four upper notes");
}

static void channels_and_events(Editor *e) {
    editor_smoke(e);
    DmNote before = ed_pattern(e)->notes[0];
    int list[] = {0, 1};
    UiMouse m = {.pos = {30, 180}, .clicked = true};
    ui_root(0, 900);
    ed_playlist_label(e, (UiRect){0, 0, 500, 300}, 154, 64, list, 2, 160, m);
    check(e->selected_channel == 1 && e->pattern_channel == 0,
          "playlist selection keeps open pattern channel");
    check(!memcmp(&before, &ed_pattern(e)->notes[0], sizeof(before)),
          "channel selection leaves note data unchanged");
    check(!strcmp(dm_drum_name(42), "Closed Hi-Hat") &&
              !strcmp(dm_drum_name(35), "Acoustic Bass Drum"),
          "percussion keys carry GM names");
    ed_new_song_event(e);
    e->event_kind = 0;
    e->event_bpm = 90;
    e->event_beat = 2;
    size_t count = e->project.event_count;
    ed_song_event_add(e);
    check(e->project.event_count == count + 1 && !e->song_event, "manual tempo event is committed");
    ed_new_song_event(e);
    check(e->event_edit == -1 && e->event_auto_edit == -1 && !e->event_source_kind,
          "new event clears prior edit target");
    e->song_event = false;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->project.automation_count = 0;
    e->automation_cc = -1;
    UiRect lane = {100, 100, 500, 200};
    e->drag = 4;
    e->marquee_start = (UiPoint){180, 200};
    UiMouse release = {.pos = {180, 200}, .released = true};
    ed_automation_drag(e, lane, &e->roll_view, 80, release);
    check(e->project.automation_count == 1 && e->project.automation[0].type == 0xc0 &&
              e->project.automation[0].curve == 0 && e->project.automation[0].pattern == 0,
          "program grid authors a discrete instrument change");
    DmSchedule schedule = {0};
    char error[256];
    check(dm_schedule(&e->project, &schedule), "program event schedule compiles");
    bool found = false;

    for (size_t i = 0; i < schedule.count; ++i) {
        if (schedule.items[i].status == 0xc0 && schedule.items[i].seconds > 0) {
            found = true;
        }
    }

    check(found, "program change reaches MIDI schedule");
    dm_schedule_free(&schedule);
    ui_end();
}

static void bend_edits(Editor *e) {
    ui_root(0, 900);
    editor_smoke(e);
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->automation_cc = -2;
    e->project.automation_count = 0;
    e->roll_view = (View){1, 0, (127 - 70) * 20, 0};
    e->drag_key = 64;
    e->bend_range = 12;
    ed_automation_line(e, 0, 2, 8192, 10923, -2);
    UiRect grid = {100, 100, 500, 400};
    UiMouse mouse = {.pos = {180, 190}, .clicked = true, .alt = true};
    size_t notes = ed_pattern(e)->count, old = e->project.automation_count;
    check(ed_bend_mouse(e, grid, &e->roll_view, 80, mouse),
          "Alt grabs bend line between endpoints");
    check(e->project.automation_count == old + 1 && e->drag == 7,
          "line gesture inserts one connected node");
    mouse.clicked = false;
    mouse.left = true;
    mouse.pos = (UiPoint){190, 170};
    ed_bend_mouse(e, grid, &e->roll_view, 80, mouse);
    DmAutomation moved = e->project.automation[e->automation_node];
    check(fabs(moved.beat - 1.25) < 1e-8 && fabs(ed_automation_pitch(e, moved) - 67) < .01,
          "Alt dragging snaps inserted point in time and note pitch");
    mouse.left = false;
    mouse.released = true;
    ed_bend_mouse(e, grid, &e->roll_view, 80, mouse);
    mouse = (UiMouse){
        .pos = ed_bend_point(e, moved, grid, &e->roll_view, 80), .clicked = true, .shift = true};
    old = e->project.automation_count;
    check(ed_bend_mouse(e, grid, &e->roll_view, 80, mouse) && e->automation_node >= 0 &&
              e->drag == 0,
          "Shift click selects overlay point without dragging");
    key(e, SDL_SCANCODE_DELETE, 0);
    check(e->project.automation_count == old - 1 && ed_pattern(e)->count == notes,
          "Delete removes selected overlay point and preserves notes");
    int endpoint = -1;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].type == 0xe0 && e->project.automation[i].curve > 0) {
            endpoint = (int)i;
        }
    }

    DmAutomation a = e->project.automation[endpoint];
    mouse = (UiMouse){
        .pos = ed_bend_point(e, a, grid, &e->roll_view, 80), .clicked = true, .alt = true};
    old = e->project.automation_count;
    ed_bend_mouse(e, grid, &e->roll_view, 80, mouse);
    check(e->drag == 7 && e->project.automation_count == old,
          "Alt on existing node moves it without inserting");
    mouse.clicked = false;
    mouse.released = true;
    ed_bend_mouse(e, grid, &e->roll_view, 80, mouse);
    UiRect lane = {100, 100, 500, 200};
    check(ed_lane_step((UiRect){0, 0, 500, 500}) == ed_lane_step(lane) && ed_lane_step(lane) == 20,
          "lane axis retains the same readable pitch spacing at all heights");
    e->project.automation_count = 0;
    e->automation_center = 64;
    e->drag = 4;
    e->marquee_start = (UiPoint){180, 200};
    mouse = (UiMouse){.pos = {180, 200}, .released = true};
    ed_automation_drag(e, lane, &e->roll_view, 80, mouse);
    int discrete = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        discrete += e->project.automation[i].type == 0xe0 && !e->project.automation[i].curve;
    }

    check(discrete == 1, "Shift lane click creates one unconnected point");
    mouse = (UiMouse){.pos = {260, 182}, .clicked = true};
    ed_automation_lane_click(e, lane, &e->roll_view, 80, mouse, -1);
    int positives = 0, negative = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation b = e->project.automation[i];
        positives += b.type == 0xe0 && b.curve > 0;
        negative += b.type == 0xe0 && b.curve < 0 && b.beat == 2;
    }

    check(positives == 2 && negative == 1,
          "left click connects new point and moves end restoration");
    mouse = (UiMouse){.right_clicked = true, .shift = true};
    ed_automation_lane_click(e, lane, &e->roll_view, 80, mouse, 0);
    check(e->drag == 0, "right click on an existing point cannot strand a left-button drag");
    // A new forward extension follows the user's last placement, even when another section
    // has a closer point. A backward insertion instead picks the nearest point in time.
    e->project.automation_count = 0;
    e->automation_cc = 11;
    DmAutomation first = {1, 0xb0, 0, 11, 90, 0, 1, 64, 0, false};
    DmAutomation other = {3, 0xb0, 0, 11, 70, 0, 2, 64, 0, false};
    dm_automation_add(&e->project, first);
    dm_automation_add(&e->project, other);
    e->last_automation = first;
    e->last_automation_valid = true;
    check(e->project.automation[ed_closest_node(e, 4)].curve == 1,
          "forward extension follows the last placed point");
    e->last_automation = other;
    check(e->project.automation[ed_closest_node(e, .5)].curve == 1,
          "backward extension finds the nearest point");
    ui_end();
}

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL initializes");
    SDL_Window *window =
        SDL_CreateWindow("transport_shortcuts validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "direct3d11");
    check(window && renderer && ui_init(window, renderer, NULL), "real ImGui renderer initializes");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, window, renderer, NULL, "", "", true);
    ui_frame();
    playback(e);
    shortcuts_added(e);
    channels_and_events(e);
    bend_edits(e);
    ui_render(renderer);
    ui_frame();
    e->automation_open = true;
    e->automation_cc = -2;
    e->project.automation_count = 0;
    e->drag_key = 64;
    e->bend_range = 12;
    ed_automation_line(e, 0, 2, 8192, 9557, -2);
    e->roll_height = 450;
    editor_frame(e, 1400, 900);
    ui_render(renderer);
    SDL_Surface *capture = SDL_RenderReadPixels(renderer, NULL);
    check(capture && SDL_SaveBMP(capture, "agents/reports/transport_shortcuts-editor.bmp"),
          "capture updated editor");
    SDL_DestroySurface(capture);
    SDL_RenderPresent(renderer);
    SDL_SetWindowSize(window, 900, 1600);
    SDL_PumpEvents();
    e->roll_height = 1000;
    e->automation_height = 450;

    for (int frame = 0; frame < 2; ++frame) {
        ui_frame();
        editor_frame(e, 900, 1600);
        SDL_RenderClear(renderer);
        ui_render(renderer);

        if (frame == 1) {
            SDL_Surface *tall = SDL_RenderReadPixels(renderer, NULL);

            if (tall) {
                SDL_SaveBMP(tall, "agents/reports/transport_shortcuts-tall.bmp");
            }

            SDL_DestroySurface(tall);
        }

        SDL_RenderPresent(renderer);
    }

    printf("PASS transport_shortcuts: %d targeted checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
