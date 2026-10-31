#include "editor_internal.h"
static int checks;

static void check(bool ok, const char *what) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL automation_transfer: %s\n", what);
        exit(1);
    }
}

extern void test_automation_transfer_ui(Editor *, void (*)(bool, const char *));

static int control_at(const DmSchedule *s, int channel, int cc, double seconds) {
    int value = -1;

    for (size_t i = 0; i < s->count; ++i) {
        if (s->items[i].seconds <= seconds + 1e-8 && s->items[i].status == (0xb0 | channel) &&
            s->items[i].a == cc) {
            value = s->items[i].b;
        }
    }

    return value;
}

static void modular_schedule(void) {
    DmProject p;
    dm_project_init(&p);
    p.channels[0].program = 0;
    int note = dm_pattern_add(&p, "Notes"), a = dm_pattern_add(&p, "First"),
        b = dm_pattern_add(&p, "Second");
    p.patterns[a].kind = p.patterns[b].kind = 1;
    p.patterns[a].event = p.patterns[b].event = 11;
    p.lane_count[0] = 2;
    snprintf(p.lane_names[0][0], 64, "Expression %% ## literal");
    dm_note_add(&p.patterns[note], (DmNote){0, 4, 60, 100, 0});
    dm_clip_add(&p, (DmClip){0, note, 0, 0, 0, 0});
    dm_clip_add(&p, (DmClip){0, a, 0, 0, 8, 1});
    dm_clip_add(&p, (DmClip){2, b, 0, 0, 2, 2});
    dm_automation_add(&p, (DmAutomation){0, 0xb0, 0, 11, 40, a, 0, 0, 0, false});
    dm_automation_add(&p, (DmAutomation){0, 0xb0, 0, 11, 90, b, 0, 0, 0, false});
    dm_automation_add(&p, (DmAutomation){1, 0xb0, 0, 11, 100, note, 7, 0, 0, false});
    dm_automation_add(&p, (DmAutomation){1.5, 0xb0, 0, 11, 100, note, 7, 0, 0, false});
    dm_automation_add(&p, (DmAutomation){1.501, 0xb0, 0, 11, 127, note, -7, 0, 0, false});
    DmSchedule schedule = {0};
    check(dm_schedule(&p, &schedule), "modular schedule");
    check(control_at(&schedule, 0, 11, .1) == 40, "automation begins on its assigned channel");
    check(control_at(&schedule, 0, 11, .6) == 100, "note-pattern events take priority");
    check(control_at(&schedule, 0, 11, .9) == 40, "note event end reveals active automation value");
    check(control_at(&schedule, 0, 11, 1.2) == 90,
          "most recent overlapping automation placement wins");
    check(control_at(&schedule, 0, 11, 2.2) == 40,
          "shorter automation end reveals older active placement");
    check(control_at(&schedule, 0, 11, 4.1) == 127,
          "last automation end restores underlying default");
    dm_schedule_free(&schedule);
    DmBytes midi = {0}, packed = {0};
    char error[256];
    DmProject next;
    dm_project_init(&next);
    check(dm_midi(&p, &midi, error, sizeof(error)), "modular MIDI export");
    check(dm_import(&next, midi.data, midi.size, 0, 0, error, sizeof(error)),
          "exported automation reimports");
    check(dm_schedule(&next, &schedule), "reimported automation schedule");
    check(control_at(&schedule, 0, 11, 1.2) == 90 && control_at(&schedule, 0, 11, 4.1) == 127,
          "MIDI retains overlap and restoration behavior");
    dm_schedule_free(&schedule);
    check(dm_pack(&p, &packed) && dm_unpack(&next, packed.data, packed.size, error, sizeof(error)),
          "schema 3 modular roundtrip");
    check(next.patterns[a].kind == 1 && next.clips[2].length == 2 && next.clips[2].lane == 2 &&
              next.lane_count[0] == 2 && !strcmp(next.lane_names[0][0], p.lane_names[0][0]),
          "pattern kinds, placement lengths and literal lane names persist");
    packed.data[8] = 2;
    check(!dm_unpack(&next, packed.data, packed.size, error, sizeof(error)),
          "obsolete schema rejected clearly");
    p.patterns[a].mode = 1;
    p.patterns[a].event = 1;
    p.patterns[a].speed = 2;
    p.patterns[a].amount = .8;
    check(dm_schedule(&p, &schedule), "LFO schedule");
    check(control_at(&schedule, 0, 1, .005) > 100 && control_at(&schedule, 0, 1, .125) < 30,
          "LFO produces positive and negative half cycles");
    check(control_at(&schedule, 0, 1, 4.1) == 0, "LFO end restores controller default");
    dm_schedule_free(&schedule);
    double normal = dm_lfo(&p.patterns[a], .08);
    p.patterns[a].skew = .5;
    check(fabs(dm_lfo(&p.patterns[a], .08) - normal) > .01, "skew changes waveform timing");
    p.patterns[a].pulse = .25;
    check(fabs(dm_lfo(&p.patterns[a], .08) - normal) > .01, "pulse changes waveform shape");
    p.patterns[a].tension = .5;
    check(isfinite(dm_lfo(&p.patterns[a], .08)), "tension remains bounded");
    p.patterns[a].amount = 0;
    check(dm_lfo(&p.patterns[a], .08) == 0, "zero amount is flat");
    check(!dm_note_add(&p.patterns[a], (DmNote){0, 1, 60, 100, 0}),
          "automation definitions reject note insertion");
    dm_bytes_free(&midi);
    dm_bytes_free(&packed);
    dm_project_free(&next);
    dm_project_free(&p);
}

static void bend_and_density(void) {
    DmProject p;
    dm_project_init(&p);
    int note = dm_pattern_add(&p, "Bend notes"), mod = dm_pattern_add(&p, "LFO");
    dm_note_add(&p.patterns[note], (DmNote){0, 4, 60, 100, 0});
    p.patterns[mod].kind = 1;
    p.patterns[mod].mode = 1;
    p.patterns[mod].amount = 1;
    p.lane_count[0] = 1;
    dm_clip_add(&p, (DmClip){0, note, 0, 0, 0, 0});
    dm_clip_add(&p, (DmClip){0, mod, 0, 0, 4, 1});
    dm_automation_add(&p, (DmAutomation){0, 0xb0, 0, 101, 0, note, 1, 60, 0, false});
    dm_automation_add(&p, (DmAutomation){0, 0xb0, 0, 100, 0, note, 1, 60, 0, false});
    dm_automation_add(&p, (DmAutomation){0, 0xb0, 0, 6, 24, note, 1, 60, 0, false});
    dm_automation_add(&p, (DmAutomation){0, 0xe0, 0, 0, 96, note, 1, 60, 0, false});
    dm_automation_add(&p, (DmAutomation){1, 0xe0, 0, 0, 96, note, 1, 60, 0, false});
    dm_automation_add(&p, (DmAutomation){1.001, 0xe0, 0, 0, 64, note, -1, 60, 0, false});
    dm_automation_add(&p, (DmAutomation){0, 0xb0, 0, 38, 0, note, 1, 60, 0, false});
    dm_automation_add(&p, (DmAutomation){0, 0xb0, 0, 101, 127, note, 1, 60, 0, false});
    dm_automation_add(&p, (DmAutomation){0, 0xb0, 0, 100, 127, note, 1, 60, 0, false});
    const int setup_cc[] = {101, 100, 6, 38, 101, 100, 99, 98},
              setup_values[] = {0, 0, 12, 50, 127, 127, 1, 2};

    for (int i = 0; i < 8; ++i) {
        uint8_t bytes[] = {0xb0, setup_cc[i], setup_values[i]};
        DmBytes ed_message = {bytes, 3, 3};
        dm_event_add(&p, 0, 0, 0, &ed_message);
    }

    const int restore_cc[] = {101, 100, 6, 38, 101, 100},
              restore_values[] = {0, 0, 12, 50, 127, 127};

    for (int i = 0; i < 6; ++i) {
        dm_automation_add(&p, (DmAutomation){1.001, 0xb0, 0, restore_cc[i], restore_values[i], note,
                                             -1, 60, 0, false});
    }

    DmSchedule schedule = {0};
    check(dm_schedule(&p, &schedule), "pitch bend and setup schedule");
    int selector = -1, entry = -1, bend = -1;
    bool bounded = true;

    for (size_t i = 0; i < schedule.count; ++i) {
        DmScheduled a = schedule.items[i];

        if (a.seconds == 0 && a.status == 0xb0 && a.a == 101 && a.b == 0) {
            selector = (int)i;
        }

        if (a.seconds == 0 && a.status == 0xb0 && a.a == 6 && a.b == 24) {
            entry = (int)i;
        }

        if (a.status == 0xe0 && a.seconds <= .2) {
            bend = a.a + (a.b << 7);
        }

        if (a.status == 0xe0) {
            bounded &= a.a < 128 && a.b < 128;
        }
    }

    check(selector >= 0 && entry > selector, "pitch sensitivity selects its RPN before data entry");
    check(bend == 12288 && bounded, "note bend overrides LFO and emitted bends stay 14-bit");
    check(control_at(&schedule, 0, 6, .6) == 2,
          "active LFO restores its own bend range after a higher-priority note section ends");
    check(control_at(&schedule, 0, 6, 2.1) == 12 && control_at(&schedule, 0, 38, 2.1) == 50,
          "automation end restores source bend sensitivity including cents");
    check(control_at(&schedule, 0, 99, 2.1) == 1 && control_at(&schedule, 0, 98, 2.1) == 2,
          "pitch transactions preserve the source NRPN selection");
    dm_schedule_free(&schedule);
    const int later_cc[] = {101, 100, 6, 38, 101, 100}, later_values[] = {0, 0, 7, 25, 127, 127};

    for (int i = 0; i < 6; ++i) {
        uint8_t bytes[] = {0xb0, later_cc[i], later_values[i]};
        DmBytes ed_message = {bytes, 3, 3};
        dm_event_add(&p, 2.5, 0, 0, &ed_message);
    }

    check(dm_schedule(&p, &schedule), "source bend-range change under active automation");
    check(control_at(&schedule, 0, 6, 1.3) == 2 && control_at(&schedule, 0, 6, 2.1) == 7,
          "active LFO overrides a later source range change, then restores it");
    dm_schedule_free(&schedule);
    p.clip_count = 0;
    p.automation_count = 0;

    for (int c = 0; c < 8; ++c) {
        p.lane_count[c] = 1;
        p.channels[c].enabled = true;

        for (int i = 0; i < 16; ++i) {
            dm_clip_add(&p, (DmClip){i * 16, mod, c, 0, 16, 1});
        }
    }

    uint64_t start = SDL_GetPerformanceCounter();
    check(dm_schedule(&p, &schedule), "dense LFO arrangement schedules successfully");
    double ms = (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
    printf("Dense LFO schedule: 128 placements, 8 channels, %zu messages, %.2f ms\n",
           schedule.count, ms);
    dm_schedule_free(&schedule);
    dm_project_free(&p);
}

static void schema_validation(void) {
    DmProject p, next;
    dm_project_init(&p);
    dm_project_init(&next);
    DmBytes packed = {0};
    char error[256];
    int id = dm_pattern_add(&p, "Validation");
    p.patterns[id].kind = 1;
    p.lane_count[0] = 1;
    dm_clip_add(&p, (DmClip){0, id, 0, 0, 4, 1});
    check(dm_pack(&p, &packed) && dm_unpack(&next, packed.data, packed.size, error, sizeof(error)),
          "valid automation fixture roundtrips");
    dm_bytes_free(&packed);
    p.patterns[id].speed = 0;
    dm_pack(&p, &packed);
    check(!dm_unpack(&next, packed.data, packed.size, error, sizeof(error)) &&
              next.patterns[0].speed == 1,
          "invalid LFO speed is rejected transactionally");
    dm_bytes_free(&packed);
    p.patterns[id].speed = 1;
    p.clips[0].lane = 2;
    dm_pack(&p, &packed);
    check(!dm_unpack(&next, packed.data, packed.size, error, sizeof(error)),
          "placement referencing an absent subrow is rejected");
    dm_bytes_free(&packed);
    p.clips[0].lane = 1;
    p.patterns[id].amount = NAN;
    dm_pack(&p, &packed);
    check(!dm_unpack(&next, packed.data, packed.size, error, sizeof(error)),
          "non-finite LFO amount is rejected");
    dm_bytes_free(&packed);
    p.patterns[id].amount = .5;
    p.patterns[id].mode = 1;
    p.clips[0].length = 10000000;
    DmSchedule schedule = {0};
    check(!dm_schedule(&p, &schedule), "unbounded LFO message expansion fails cleanly");
    dm_schedule_free(&schedule);
    dm_project_free(&p);
    dm_project_free(&next);
}

static void transfer_checks(Editor *e) {
    editor_smoke(e);
    size_t clips = e->project.clip_count;
    int notes = (int)ed_pattern(e)->count;
    dm_automation_add(&e->project, (DmAutomation){0, 0xb0, 0, 11, 80, 0, 9, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){2, 0xb0, 0, 11, 100, 0, 9, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){2.001, 0xb0, 0, 11, 127, 0, -9, 0, 0, false});
    check(ed_transfer_automation(e), "transfer succeeds");
    int id = e->selected_pattern;
    check(ed_pattern(e)->kind && ed_pattern(e)->event == 11 && !ed_pattern(e)->count,
          "transfer opens a standalone event pattern");
    check(e->project.clip_count == clips + 4 && e->project.lane_count[0] == 1,
          "transfer adds matching copies beneath all four source placements");
    bool moved = true;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        moved &= e->project.automation[i].pattern == id;
    }

    check(moved && e->project.patterns[0].count == (size_t)notes,
          "events move while source notes remain intact");
    int copy = dm_pattern_copy(&e->project, id);
    check(copy >= 0 && e->project.patterns[copy].kind == 1, "automation duplicate retains kind");
    e->project.patterns[copy].speed = 3;
    check(e->project.patterns[id].speed == 1, "duplicate definition is independent");
    ed_undo(e, false);
    check(e->project.pattern_count == 2 && e->project.clip_count == clips &&
              e->project.automation[0].pattern == 0,
          "one undo restores transfer as a single edit");
    editor_smoke(e);
    ed_choose_pattern_card(e, 0, false);
    check(e->linked_highlight, "browser selection highlights all linked references");
    ed_playlist_hit_click(e, (UiMouse){0}, 0, 0);
    check(!e->linked_highlight && e->project.clips[0].selected && !e->project.clips[2].selected,
          "playlist selection highlights only the clicked placement");
    e->project.patterns[e->project.clips[0].pattern].notes[0].key = 61;
    check(e->project.patterns[e->project.clips[2].pattern].notes[0].key == 61,
          "independent placement selection retains shared note content");
    e->drag = 0;
    e->selected_channel = 3;
    e->piano = true;
    ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
    check(e->piano_channel == 3, "non-recording keyboard follows selected channel");
    ed_release_piano(e);
    e->record = true;
    e->wait_input = true;
    ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
    check(e->piano_channel == 3, "playlist-focused recording keyboard follows selected channel");
    ed_release_piano(e);
    e->project.clip_count = 0;
    ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
    check(e->piano_channel == 3, "recording unplaced pattern follows selected channel");
    ed_release_piano(e);
    e->record = false;
    e->piano = false;
    e->loop_enabled[0] = true;
    e->loop_start[0] = 1;
    e->loop_end[0] = 2;
    e->edit_focus = FOCUS_PLAYLIST;
    check(ed_clear_loop(e) && !e->loop_enabled[0],
          "playlist loop can be dismissed after selections");
    e->loop_enabled[1] = true;
    e->pattern_mode = true;
    e->edit_focus = FOCUS_EVENTS;
    check(ed_clear_loop(e) && !e->pattern_mode,
          "pattern Escape clears its loop and returns Song playback");
    ed_seek(e, 1.5);
    check(e->start_beat[0] == 1.5 && e->cursor_beat[0] == 1.5,
          "keyboard seek updates set position");
}

int main(void) {
    modular_schedule();
    bend_and_density();
    schema_validation();
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("automation_transfer checks", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", true);
    transfer_checks(e);
    editor_smoke(e);
    test_automation_transfer_ui(e, check);
    printf("PASS automation_transfer: %d focused checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
