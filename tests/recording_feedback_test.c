#include "editor_internal.h"
static int checks;

static void check(bool ok, const char *what) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL recording_feedback: %s\n", what);
        exit(1);
    }
}

extern void recording_feedback_audio_pause(DmAudio *);
extern void recording_feedback_audio_render(DmAudio *, int);
extern unsigned recording_feedback_audio_voice(DmAudio *, int, int);
extern int recording_feedback_audio_count(DmAudio *);
extern bool recording_feedback_audio_live_voice(DmAudio *, int, int);
extern int recording_feedback_audio_override(DmAudio *, int);
extern int recording_feedback_audio_preset(DmAudio *, int, bool);
extern int recording_feedback_audio_bank(DmAudio *, int, bool);
extern void test_recording_feedback_ui(Editor *, void (*)(bool, const char *));

static void fixture(Editor *e) {
    ed_release_piano(e);
    dm_audio_recording(e->audio, false);
    editor_smoke(e);
    e->selected_pattern = 0;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->record = e->wait_input = e->wait_armed = e->take_started = false;
    e->recording_take = e->take_snapshot = false;
    e->overwrite = false;
    e->project.automation_count = 0;
    e->project.patterns[0].count = 0;
    e->project.patterns[0].length = 4;
    e->piano = true;
    e->pattern_mode = false;
    e->preview_program = 0;
    e->automation_cc = -2;
    e->automation_center = 60;
    e->automation_raw = false;
    e->automation_node = -1;
    e->curve_drag = false;
    memset(e->start_beat, 0, sizeof(e->start_beat));
    memset(e->cursor_beat, 0, sizeof(e->cursor_beat));
    ed_transport_publish(e, 0, false);
    recording_feedback_audio_render(e->audio, 256);
}

static void defaults_and_placement(Editor *e) {
    DmProject p;
    dm_project_init(&p);
    int enabled = 0;

    for (int c = 0; c < 16; ++c) {
        enabled += p.channels[c].enabled;
    }

    check(enabled == 2 && p.channels[0].enabled && p.channels[9].enabled,
          "new projects enable only channels 1 and 10");
    dm_project_free(&p);
    fixture(e);
    e->project.channels[2].program = 36;
    e->project.channels[2].enabled = false;
    int list[16], count = ed_channels(e, list);
    int spare = ed_available_channel(e), disabled = 0;
    bool ordered = true;

    for (int i = 0; i < count; ++i) {
        disabled += !e->project.channels[list[i]].enabled;
        ordered &= !i || list[i - 1] < list[i];
    }

    check(ordered && disabled == (spare >= 0 && !e->project.channels[spare].enabled),
          "spare is ordered by channel and reuses an existing empty unassigned row");
    DmPattern *pat = ed_pattern(e);
    dm_note_add(pat, (DmNote){0, 2, 60, 100, 0});
    dm_note_add(pat, (DmNote){3, .5, 64, 100, 0});
    double original = pat->length;
    DmClip *clip = &e->project.clips[0];
    ed_resize_clip(e, clip, 1);
    check(clip->length == 1 && pat->length == original && e->project.clips[1].length == 0,
          "note placement resizing changes only its own duration");
    DmSchedule s = {0};
    check(dm_schedule(&e->project, &s), "trimmed placements schedule");
    bool cutoff = false, late = false;

    for (size_t i = 0; i < s.count; ++i) {
        DmScheduled a = s.items[i];
        cutoff |= a.status == 0x80 && a.a == 60 && fabs(a.seconds - .5) < 1e-8;
        late |= a.status == 0x90 && a.a == 64 && a.seconds < 2;
    }

    check(cutoff && !late, "trim cuts crossing notes and hides later starts");
    dm_schedule_free(&s);
    ed_resize_clip(e, clip, 8);
    check(dm_schedule(&e->project, &s), "extended placement schedules");
    int starts = 0;

    for (size_t i = 0; i < s.count; ++i) {
        starts += s.items[i].status == 0x90 && s.items[i].b && s.items[i].seconds < 2;
    }

    check(starts == 2, "extending a placement does not repeat its source notes");
    dm_schedule_free(&s);
}

static void recording_and_pause(Editor *e) {
    fixture(e);
    dm_note_add(ed_pattern(e), (DmNote){0, 2, 60, 100, 0});
    dm_note_add(ed_pattern(e), (DmNote){3, .5, 64, 100, 0});
    e->record = e->wait_input = true;
    dm_audio_recording(e->audio, true);
    ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
    check(!dm_audio_playing(e->audio) && !e->wait_armed && ed_pattern(e)->count == 2,
          "piano input cannot start unarmed waiting playback");
    ed_release_piano(e);
    ed_play(e);
    check(e->wait_armed && !dm_audio_playing(e->audio),
          "Play arms Wait without advancing transport");
    ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
    check(dm_audio_playing(e->audio) && !e->wait_armed && ed_pattern(e)->count == 0,
          "armed piano input starts playback and non-overlap take clears all old notes");
    recording_feedback_audio_render(e->audio, 6000);
    ed_release_piano(e);
    check(ed_pattern(e)->count == 1 && ed_pattern(e)->notes[0].key == 48,
          "new recording replaces the old pattern with Overlap disabled");
    ed_play(e);
    check(dm_audio_position(e->audio) == 0, "pattern-focused pause returns to its set position");
    ed_undo(e, false);
    check(ed_pattern(e)->count == 2, "one undo restores the complete pre-take pattern");
    ed_play(e);
    ed_play(e);
    check(!e->wait_armed && !dm_audio_playing(e->audio), "second Play cancels pending Wait");
    fixture(e);
    dm_note_add(ed_pattern(e), (DmNote){0, 2, 60, 100, 0});
    dm_note_add(ed_pattern(e), (DmNote){.1, 1, 64, 100, 0});
    dm_note_add(ed_pattern(e), (DmNote){3, .5, 67, 100, 0});
    e->overwrite = e->record = true;
    dm_audio_recording(e->audio, true);
    ed_play(e);
    ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
    ed_piano_press(e, SDL_SCANCODE_X, 0, false);
    recording_feedback_audio_render(e->audio, 24000);
    ed_release_piano(e);
    bool preserved = false, first = false, second = false;

    for (size_t i = 0; i < ed_pattern(e)->count; ++i) {
        DmNote n = ed_pattern(e)->notes[i];
        preserved |= n.key == 67;
        first |= n.key == 48;
        second |= n.key == 50;
    }

    check(ed_pattern(e)->count == 3 && preserved && first && second,
          "Overlap replaces intersecting old notes of any pitch and retains new "
          "chords/nonoverlapping notes");
    ed_play(e);
    fixture(e);
    e->context = 1;
    ed_seek(e, .5);
    ed_play(e);
    recording_feedback_audio_render(e->audio, 12000);
    ed_play(e);
    check(fabs(dm_audio_position(e->audio) - .5) < .01,
          "playlist-focused pause keeps its current position");
    e->context = 2;
    ed_play(e);
    recording_feedback_audio_render(e->audio, 12000);
    ed_play(e);
    check(fabs(dm_audio_position(e->audio) - .25) < .01,
          "pattern-focused ordinary pause returns to the earlier set position");
}

static void preview_and_feedback(Editor *e) {
    fixture(e);
    e->preview_program = 40;
    int program = e->project.channels[0].program;
    ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
    recording_feedback_audio_render(e->audio, 256);
    unsigned voice = recording_feedback_audio_voice(e->audio, 0, 48);
    check(voice != UINT32_MAX && recording_feedback_audio_override(e->audio, 0) == 40 &&
              e->project.channels[0].program == program,
          "manual preview instrument sounds without changing playlist program");
    dm_note_add(ed_pattern(e), (DmNote){0, 1, 60, 100, 1});
    ed_delete_selection(e);
    recording_feedback_audio_render(e->audio, 256);
    check(recording_feedback_audio_voice(e->audio, 0, 48) == voice,
          "deleting a note does not retrigger the held PC keyboard voice");
    ed_publish(e);
    recording_feedback_audio_render(e->audio, 256);
    check(recording_feedback_audio_voice(e->audio, 0, 48) == voice,
          "ordinary edit publication does not replay held piano note-ons");
    ed_release_piano(e);
    recording_feedback_audio_render(e->audio, 4096);
    check(recording_feedback_audio_count(e->audio) == 0, "key release clears editing preview");
    e->preview_program = 128;
    ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
    recording_feedback_audio_render(e->audio, 128);
    check(e->held_channels[SDL_SCANCODE_Z] == 9, "percussion preview uses percussion mapping");
    ed_release_piano(e);
    recording_feedback_audio_render(e->audio, 128);
}

static void instrument_switching(Editor *e) {
    fixture(e);
    e->pattern_mode = true;
    e->project.channels[0].program = 16;
    e->project.channels[2].enabled = true;
    e->project.channels[2].program = 33;
    dm_note_add(ed_pattern(e), (DmNote){0, 4, 60, 100, 0});
    dm_note_add(ed_pattern(e), (DmNote){.1, 1, 64, 100, 0});
    uint8_t imported_program[] = {0xc0, 73};
    DmBytes event = {imported_program, sizeof(imported_program), sizeof(imported_program)};
    check(dm_event_add(&e->project, .001, 0, 0, &event), "source program change fixture");
    uint8_t imported_bank[] = {0xb0, 0, 2};
    event = (DmBytes){imported_bank, sizeof(imported_bank), sizeof(imported_bank)};
    check(dm_event_add(&e->project, 0, 0, 0, &event), "source bank change fixture");
    int programs[] = {40, 12, 73, 0, 128, 16, 128, 33, 40};

    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); ++i) {
        int program = programs[i], route = program == 128 ? 9 : 0;
        e->context = 1;
        e->edit_focus = FOCUS_PLAYLIST;
        ed_select_preview_instrument(e, program);
        recording_feedback_audio_render(e->audio, 128);
        check(e->context == 2 && e->preview_program == program,
              "selection establishes pattern keyboard focus and visible instrument");
        check(recording_feedback_audio_preset(e->audio, route, true) ==
                  (program == 128 ? 0 : program),
              "keyboard synth has selected preset immediately before a note is played");
        check(recording_feedback_audio_bank(e->audio, route, true) == (program == 128 ? 128 : 0),
              "keyboard preview uses correct melodic or percussion bank");
        ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
        recording_feedback_audio_render(e->audio, 128);
        check(recording_feedback_audio_voice(e->audio, route, 48) != UINT32_MAX,
              "selected keyboard route produces an actual voice");
        ed_transport_publish(e, 0, true);
        recording_feedback_audio_render(e->audio, 128);
        check(recording_feedback_audio_preset(e->audio, route, false) ==
                      (program == 128 ? 0 : program) &&
                  recording_feedback_audio_bank(e->audio, route, false) ==
                      (program == 128 ? 128 : 0),
              "pattern playback synth matches selected instrument and bank");
        ed_release_piano(e);
        check(e->project.channels[0].program == 16 && e->project.channels[2].program == 33,
              "preview instrument never alters playlist instrument assignments");
    }

    ed_select_preview_instrument(e, ed_channel_program(e, 2));
    recording_feedback_audio_render(e->audio, 4800);
    check(recording_feedback_audio_preset(e->audio, 0, true) == 33 &&
              recording_feedback_audio_preset(e->audio, 0, false) == 33,
          "Use selected channel action updates both synths while pattern is playing");

    for (size_t i = 0; i < e->project.event_count; ++i) {
        dm_bytes_free(&e->project.events[i].message);
    }

    e->project.event_count = 0;
    e->pattern_mode = false;
    e->context = 1;
    e->selected_channel = 0;
    ed_keyboard_channel(e);
    ed_transport_publish(e, 0, true);
    recording_feedback_audio_render(e->audio, 128);
    check(recording_feedback_audio_preset(e->audio, 0, true) == 16 &&
              recording_feedback_audio_preset(e->audio, 0, false) == 16,
          "returning to song and playlist restores original channel program");
}

static void curve_backend(Editor *e) {
    fixture(e);
    e->automation_cc = 11;
    DmAutomation a = {0, 0xb0, 0, 11, 30, 0, 0, 0, 25, true};
    DmAutomation b = {2, 0xb0, 0, 11, 90, 0, 0, 0, 0, false};
    check(dm_curve_value(a, b, 0) == 30 && dm_curve_value(a, b, 1) == 90 &&
              dm_curve_value(a, b, .5) == 85,
          "quadratic curve preserves endpoints and moves midpoint");
    dm_automation_add(&e->project, a);
    dm_automation_add(&e->project, b);
    DmSchedule s = {0};
    check(dm_schedule(&e->project, &s), "converted source connection schedules");
    int value = -1;

    for (size_t i = 0; i < s.count; ++i) {
        if (s.items[i].status == 0xb0 && s.items[i].a == 11 && s.items[i].seconds <= .5 + 1e-8) {
            value = s.items[i].b;
        }
    }

    check(value == 85, "audible schedule follows curved midpoint rather than a held step");
    dm_schedule_free(&s);
    DmBytes midi = {0};
    DmProject imported;
    dm_project_init(&imported);
    char export_error[256];
    check(dm_midi(&e->project, &midi, export_error, sizeof(export_error)) &&
              dm_import(&imported, midi.data, midi.size, 0, 0, export_error, sizeof(export_error)),
          "curved events export as standard MIDI and import again");
    DmSchedule baked = {0};
    check(dm_schedule(&imported, &baked), "baked MIDI schedules");
    int baked_value = -1;

    for (size_t i = 0; i < baked.count; ++i) {
        if (baked.items[i].status == 0xb0 && baked.items[i].a == 11 &&
            baked.items[i].seconds <= .5 + 1e-8) {
            baked_value = baked.items[i].b;
        }
    }

    check(baked_value == 85, "MIDI export retains the audible curve midpoint");
    dm_schedule_free(&baked);
    dm_project_free(&imported);
    dm_bytes_free(&midi);
    check(ed_insert_lane_node(e, 1), "Shift insertion splits a curved connection");
    check(e->project.automation_count == 3 && e->project.automation[1].b == 85 &&
              fabs(e->project.automation[0].bend - 6.25) < 1e-8 &&
              fabs(e->project.automation[1].bend - 6.25) < 1e-8,
          "inserting a point preserves the original quadratic trajectory");
    DmBytes packed = {0};
    DmProject loaded;
    dm_project_init(&loaded);
    char error[256];
    check(dm_pack(&e->project, &packed) &&
              dm_unpack(&loaded, packed.data, packed.size, error, sizeof(error)),
          "current curve schema roundtrips");
    check(loaded.automation[0].ramp && loaded.automation[0].bend == 6.25,
          "curve amount and converted-step flag persist");
    packed.data[8] = 4;
    check(!dm_unpack(&loaded, packed.data, packed.size, error, sizeof(error)),
          "prior schema is rejected clearly");
    dm_bytes_free(&packed);
    dm_project_free(&loaded);
}

static void waiting_backing_onset(Editor *e) {
    fixture(e);
    int backing = dm_pattern_add(&e->project, "Backing onset");
    check(backing >= 0, "backing pattern");
    dm_note_add(&e->project.patterns[backing], (DmNote){0, 4, 72, 100, 0});
    e->project.channels[1].enabled = true;
    dm_clip_add(&e->project, (DmClip){.pattern = backing, .channel = 1});
    dm_note_add(ed_pattern(e), (DmNote){0, 4, 60, 100, 0});
    e->record = e->wait_input = true;
    dm_audio_recording(e->audio, true);
    ed_play(e);
    check(e->wait_armed, "waiting recording armed");
    ed_piano_press(e, SDL_SCANCODE_Z, 0, false);
    recording_feedback_audio_render(e->audio, 128);
    check(recording_feedback_audio_live_voice(e->audio, 1, 72),
          "backing beat-zero note survives waiting take preparation");
    check(!recording_feedback_audio_live_voice(e->audio, 0, 60),
          "replaced take does not leak old beat-zero notes");
    ed_publish(e);
    recording_feedback_audio_render(e->audio, 128);
    check(recording_feedback_audio_live_voice(e->audio, 1, 72),
          "first recording edit retains backing voice");
    ed_release_piano(e);
    fixture(e);
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi");
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), "SDL");
    SDL_Window *w = SDL_CreateWindow("recording_feedback", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio && dm_audio_soundfont(audio, "agents/reports/generated-programs.sf2", error,
                                      sizeof(error)),
          "SoundFont job");

    while (dm_audio_busy(audio)) {
        bool success = false;

        if (dm_audio_poll(audio, error, sizeof(error), &success, NULL)) {
            check(success, "SoundFont ready");
        }

        SDL_Delay(5);
    }

    recording_feedback_audio_pause(audio);
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, audio, "", "", true);
    ui_frame();
    defaults_and_placement(e);
    recording_and_pause(e);
    waiting_backing_onset(e);
    preview_and_feedback(e);
    instrument_switching(e);
    curve_backend(e);
    ui_render(r);
    fixture(e);
    test_recording_feedback_ui(e, check);
    printf("PASS recording_feedback: %d focused checks\n", checks);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
