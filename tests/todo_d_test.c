/* Checks for the 2026-10-31 backlog: new-project channels, percussion window lifetime and Enter,
   focus after a recording, and grid line shades. */
#include "editor_internal.h"
#include <assert.h>
extern void recording_feedback_audio_pause(DmAudio *);

static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL todo d: %s\n", text);
        exit(1);
    }
}

static void key(Editor *e, SDL_Scancode scancode) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = scancode;
    event.key.down = true;
    editor_event(e, &event);
}

static void new_project_channels(Editor *e) {
    ed_new_project(e);
    check(e->project.channels[0].enabled && e->project.channels[0].program == 0,
          "new projects start with channel 1 on program 0");
    check(e->project.channels[MIDI_DRUM_CHANNEL].enabled, "new projects keep the drum channel");
    check(ed_available_channel(e) == 1, "the spare instrument row follows channel 1");
}

static void percussion_window(Editor *e) {
    ed_new_project(e);
    e->project.percussion_override = true;
    memset(e->project.percussion_keys, 0, 128);
    ed_percussion_open(e);
    e->percussion_draft[36] = 1;
    key(e, SDL_SCANCODE_RETURN);
    check(!e->percussion_window && e->project.percussion_keys[36] == 1,
          "Enter applies the draft to the project's own mapping and closes the window");

    ed_percussion_open(e);
    e->percussion_draft[38] = 2;
    check(e->percussion_window, "percussion window opens");
    ed_new_project(e);
    check(!e->percussion_window && !e->project.percussion_override &&
              e->project.percussion_keys[36] == 0 &&
              !memcmp(e->percussion_draft, e->percussion_global, 128),
          "a new project closes the window and drops the unsaved draft");
}

static void focus_after_recording(Editor *e) {
    ed_new_project(e);
    e->roll = true;
    e->record = true;
    ed_focus_edit(e, FOCUS_PLAYLIST);
    ed_play(e);

    if (!dm_audio_playing(e->audio)) {
        dm_audio_transport(e->audio, true);
    }

    check(dm_audio_playing(e->audio), "a take is playing");
    ed_focus_edit(e, FOCUS_PLAYLIST);
    ed_play(e);
    check(e->edit_focus == FOCUS_NOTES && e->context == 2,
          "stopping a take focuses the open pattern editor");
    dm_audio_transport(e->audio, false);
    e->record = false;
}

static void grid_shades(Editor *e) {
    ed_new_project(e);
    uint32_t bar = ed_grid_color(e, 0), beat = ed_grid_color(e, 1), quarter = ed_grid_color(e, .25);
    check((bar & 0xff) > (beat & 0xff) && (beat & 0xff) > (quarter & 0xff),
          "beat lines sit between bar and quarter-beat lines");
    check(ed_grid_color(e, 4) == bar, "every bar uses the bar shade");
}

/* Shift + drag joins the drag's start to the nearest point and continues that line to the
   release, all in one curve and one undo step. */
static void shift_drag_line(Editor *e) {
    ed_new_project(e);
    e->automation_cc = 11;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_EVENTS;
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};
    check(dm_automation_add(&e->project,
                            (DmAutomation){.type = MIDI_CONTROL, .a = 11, .b = 30, .pattern = 0}),
          "fixture point");
    int history = e->undo_count;
    ed_automation_lane_click(
        e, lane, &v, 80, (UiMouse){.pos = {260, 180}, .clicked = true, .left = true, .shift = true},
        -1);
    ed_automation_press(e, lane, &v, 80, (UiMouse){.pos = {420, 140}, .left = true, .shift = true});
    check(e->drag == 4, "Shift + drag past the buffer draws a line");
    ed_automation_drag(e, lane, &v, 80, (UiMouse){.pos = {420, 140}, .released = true});
    int curve = 0, nodes = 0, restore = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        curve = a.beat == 0 ? a.curve : curve;
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        nodes += curve > 0 && a.curve == curve;
        restore += curve > 0 && a.curve == -curve;
    }

    check(curve > 0 && nodes == 3 && restore == 1,
          "the neighbour, the drag start and the release form one connected line");
    check(e->undo_count == history + 1, "a Shift + drag line is one undo step");
    e->event_connect = false;
}

/* Drawing a pitch-bend line preview leaves the project untouched, including other lines' bend
   ranges (the preview once borrowed a real line's id and widened its range). */
static void pitch_preview_is_read_only(Editor *e) {
    ed_new_project(e);
    e->automation_cc = -2;
    e->automation_raw = false;
    e->automation_center = 60;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_EVENTS;
    ed_automation_line(e, 0, 1, MIDI_BEND_CENTER, MIDI_BEND_CENTER + 2000, -2);
    size_t count = e->project.automation_count;
    DmAutomation *before = malloc(count * sizeof(*before));
    check(before && count, "fixture pitch line");
    memcpy(before, e->project.automation, count * sizeof(*before));
    UiRect lane = {100, 100, 600, 400};
    View v = {1, 0, 0, 0};
    e->drag = 4;
    e->event_connect = true;
    e->marquee_start = (UiPoint){260, 120};

    for (int y = 110; y < 500; y += 40) {
        ed_automation_drag(e, lane, &v, 80, (UiMouse){.pos = {500, (float)y}, .left = true});
    }

    check(e->project.automation_count == count &&
              !memcmp(before, e->project.automation, count * sizeof(*before)),
          "the line preview does not change any points");
    free(before);
    e->drag = 0;
    e->event_connect = false;
}

/* Program changes are discrete: dragging places one point at the press and never draws a line. */
static void program_change_drag(Editor *e) {
    ed_new_project(e);
    e->automation_cc = -1;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_EVENTS;
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};
    ed_automation_lane_click(
        e, lane, &v, 80, (UiMouse){.pos = {260, 180}, .clicked = true, .left = true, .shift = true},
        -1);
    ed_automation_press(e, lane, &v, 80, (UiMouse){.pos = {420, 120}, .left = true});
    check(e->drag == 12, "dragging a program change draws no line");
    ed_automation_press(e, lane, &v, 80, (UiMouse){.pos = {420, 120}, .released = true});
    check(e->drag == 0 && e->project.automation_count == 1 &&
              e->project.automation[0].type == MIDI_PROGRAM && e->project.automation[0].beat == 2,
          "a program change drag places one point where it was pressed");
}

/* Channel pressure offers only the Point mode, but it still draws lines, and a line plays changing
   pressure values in the first data byte. */
static void channel_pressure_line(Editor *e) {
    ed_new_project(e);
    e->automation_cc = -4;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_EVENTS;
    check(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0}), "fixture placement");
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};
    ed_automation_lane_click(e, lane, &v, 80,
                             (UiMouse){.pos = {100, 292}, .clicked = true, .left = true}, -1);
    ed_automation_press(e, lane, &v, 80, (UiMouse){.pos = {260, 108}, .left = true});
    check(e->drag == 4, "dragging channel pressure draws a line");
    ed_automation_drag(e, lane, &v, 80, (UiMouse){.pos = {260, 108}, .released = true});
    int nodes = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        nodes += e->project.automation[i].type == MIDI_CHANNEL_PRESSURE &&
                 e->project.automation[i].curve > 0;
    }

    check(nodes == 2, "a channel pressure line has two connected points");
    DmSchedule s = {0};
    check(dm_schedule(&e->project, &s), "schedule channel pressure");
    int low = 128, high = -1;

    for (size_t i = 0; i < s.count; ++i) {
        if ((s.items[i].status & MIDI_STATUS_MASK) == MIDI_CHANNEL_PRESSURE &&
            s.items[i].seconds > .05 && s.items[i].seconds < .95) {
            low = s.items[i].a < low ? s.items[i].a : low;
            high = s.items[i].a > high ? s.items[i].a : high;
        }
    }

    check(low < high && high - low > 50, "channel pressure lines play changing values");
    dm_schedule_free(&s);
}

static void pattern_moves(void) {
    DmProject p;
    dm_project_init(&p);

    for (int i = 0; i < 4; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "P%d", i);
        assert(dm_pattern_add(&p, name) == i);
    }

    assert(dm_clip_add(&p, (DmClip){.pattern = 0}) && dm_clip_add(&p, (DmClip){.pattern = 3}));
    check(dm_pattern_move(&p, 0, 3) == 2 && !strcmp(p.patterns[2].name, "P0") &&
              !strcmp(p.patterns[0].name, "P1"),
          "a pattern moves in front of the target pattern");
    check(p.clips[0].pattern == 2 && p.clips[1].pattern == 3, "placements follow moved patterns");
    check(dm_pattern_move(&p, 3, 0) == 0 && !strcmp(p.patterns[0].name, "P3") &&
              p.clips[1].pattern == 0 && p.clips[0].pattern == 3,
          "moving a pattern forward shifts the ones it passes");
    dm_project_free(&p);
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO));
    SDL_Window *window = SDL_CreateWindow("Todo D checks", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    assert(window && renderer && ui_init(window, renderer, NULL));
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    assert(audio);
    recording_feedback_audio_pause(audio);
    Editor *e = calloc(1, sizeof(*e));
    assert(e);
    editor_init(e, window, renderer, audio, "", "", true);
    new_project_channels(e);
    percussion_window(e);
    focus_after_recording(e);
    grid_shades(e);
    ui_frame();
    ui_root(0, 900);
    shift_drag_line(e);
    pitch_preview_is_read_only(e);
    program_change_drag(e);
    channel_pressure_line(e);
    ui_end();
    ui_render(renderer);
    pattern_moves();
    printf("PASS todo d: %d checks\n", checks);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
