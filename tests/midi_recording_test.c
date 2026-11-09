#include "editor_internal.h"
#include "midi_input_fixture.h"

void recording_feedback_audio_pause(DmAudio *audio);
bool recording_feedback_audio_event(DmAudio *audio, int type, int key, int value);

static MidiFixture fixture;
static uint32_t generation = 100;
static uint32_t loop_id;

static void send_at(Editor *e, double seconds, uint8_t status, uint8_t a, uint8_t b) {
    uint64_t ns = SDL_GetTicksNS();
    dm_midi_clock_write(dm_midi_input_clock(e->midi_input),
                        (DmMidiClockValue){ns, seconds + .01, .01, generation, true, loop_id});
    fixture_emit(&fixture, 0, status, a, b, ns);
}

static void reset(Editor *e, bool overlap) {
    ed_release_piano(e);
    dm_audio_transport(e->audio, false);
    e->record = e->take_started = e->take_snapshot = e->recording_take = false;
    e->wait_input = e->wait_armed = false;
    ed_new_project(e);
    assert(ed_pattern(e));
    ed_pattern(e)->count = 0;
    e->project.automation_count = 0;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->pattern_mode = true;
    e->piano = true;
    e->overwrite = overlap;
    e->start_beat[1] = e->cursor_beat[1] = 0;
    e->loop_enabled[1] = false;
    e->project.bpm = 120;
    ed_clear_history(e->undo, &e->undo_count);
    ed_clear_history(e->redo, &e->redo_count);
    ed_toggle_record(e);
    ed_play(e);
    ++generation;
    loop_id = 0;
}

static void point(Editor *e, double beat, int cc, int value) {
    assert(dm_automation_add(&e->project, (DmAutomation){.beat = beat,
                                                         .type = MIDI_CONTROL,
                                                         .channel = e->pattern_channel,
                                                         .a = cc,
                                                         .b = value,
                                                         .pattern = e->selected_pattern}));
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO));
    SDL_Window *window =
        SDL_CreateWindow("MIDI recording validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    assert(window && renderer && ui_init(window, renderer, NULL));
    char error[512] = {0};
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    assert(audio);
    recording_feedback_audio_pause(audio);
    Editor *e = calloc(1, sizeof(*e));
    assert(e);
    editor_init(e, window, renderer, audio, "", "", true);
    ed_midi_input_attach(e, fixture_create(&fixture));
    ed_midi_input_attach(e, e->midi_input);
    assert(dm_midi_input_open(e->midi_input, "Fixture 0"));
    fixture_wait(e->midi_input, 0, DM_MIDI_OPEN);

    reset(e, false);
    send_at(e, .125, MIDI_NOTE_ON | 9, 60, 97);
    send_at(e, .375, MIDI_NOTE_OFF | 9, 60, 64);
    ed_midi_input_frame(e);
    assert(ed_pattern(e)->count == 0);
    ed_midi_input_flush(e);
    assert(ed_pattern(e)->count == 1);
    DmNote note = ed_pattern(e)->notes[0];
    assert(note.key == 60 && note.velocity == 97);
    assert(fabs(note.beat - .25) < 1e-7 && fabs(note.length - .5) < 1e-7);
    assert(e->undo_count == 1);
    /* Recorded notes lie behind the playhead: no rebuild until recording stops while playing. */
    assert(!recording_feedback_audio_event(audio, MIDI_NOTE_ON, 60, 97));
    ed_toggle_record(e);
    assert(recording_feedback_audio_event(audio, MIDI_NOTE_ON, 60, 97));

    reset(e, false);
    ed_piano_press(e, SDL_SCANCODE_Q, 0, false);
    send_at(e, .125, MIDI_NOTE_ON, 60, 41);
    ed_midi_input_frame(e);
    ed_record_note(e, 60);
    assert(ed_pattern(e)->count == 1 && ed_pattern(e)->notes[0].velocity == ED_DEFAULT_VELOCITY);
    send_at(e, .375, MIDI_NOTE_OFF, 60, 0);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(ed_pattern(e)->count == 2);
    bool midi = false;

    for (size_t i = 0; i < ed_pattern(e)->count; ++i) {
        midi |= ed_pattern(e)->notes[i].velocity == 41 &&
                fabs(ed_pattern(e)->notes[i].length - .5) < 1e-7;
    }

    assert(midi && e->undo_count == 1);

    reset(e, false);
    send_at(e, 1.75, MIDI_NOTE_ON, 65, 92);
    ed_midi_input_frame(e);
    ++generation;
    send_at(e, 1.749, MIDI_CONTROL, MIDI_CC_MODULATION, 40);
    send_at(e, 2.0, MIDI_NOTE_OFF, 65, 64);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(ed_pattern(e)->count == 1 && fabs(ed_pattern(e)->notes[0].beat - 3.5) < 1e-7 &&
           fabs(ed_pattern(e)->notes[0].length - .5) < 1e-7);
    assert(e->project.automation_count == 1 && fabs(e->project.automation[0].beat - 3.498) < 1e-7);

    reset(e, false);
    ed_play(e);
    e->wait_input = true;
    ed_play(e);
    assert(e->wait_armed && !dm_audio_playing(audio));
    uint64_t ns = SDL_GetTicksNS();
    dm_midi_clock_write(dm_midi_input_clock(e->midi_input),
                        (DmMidiClockValue){ns, 0, 0, ++generation, false});
    fixture_emit(&fixture, 0, MIDI_NOTE_ON, 64, 88, ns);
    ed_midi_input_frame(e);
    assert(!e->wait_armed && dm_audio_playing(audio));
    send_at(e, .25, MIDI_NOTE_OFF, 64, 0);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(ed_pattern(e)->count == 1 && ed_pattern(e)->notes[0].velocity == 88);

    for (int overlap = 0; overlap <= 1; ++overlap) {
        reset(e, overlap);
        point(e, .1, MIDI_CC_MODULATION, 1);
        point(e, .5, MIDI_CC_MODULATION, 2);
        point(e, 1.5, MIDI_CC_MODULATION, 3);
        point(e, .5, MIDI_CC_EXPRESSION, 77);
        send_at(e, .2, MIDI_CONTROL | 12, MIDI_CC_MODULATION, 35);
        send_at(e, .4, MIDI_CONTROL | 12, MIDI_CC_MODULATION, 36);
        send_at(e, .3, MIDI_PITCH_BEND, 7, 72);
        send_at(e, .3, MIDI_CHANNEL_PRESSURE, 31, 0);
        send_at(e, .3, MIDI_KEY_PRESSURE, 60, 44);
        ed_midi_input_frame(e);
        ed_midi_input_flush(e);
        unsigned old = 0, fresh = 0, untouched = 0, bend = 0, pressure = 0;

        for (size_t i = 0; i < e->project.automation_count; ++i) {
            DmAutomation a = e->project.automation[i];
            old += a.type == MIDI_CONTROL && a.a == MIDI_CC_MODULATION && a.b <= 3;
            fresh += a.type == MIDI_CONTROL && a.a == MIDI_CC_MODULATION && a.b >= 35;
            untouched += a.type == MIDI_CONTROL && a.a == MIDI_CC_EXPRESSION && a.b == 77;
            bend += a.type == MIDI_PITCH_BEND && a.a == 7 && a.b == 72;
            pressure += (a.type == MIDI_CHANNEL_PRESSURE && a.a == 31 && a.b == 0) ||
                        (a.type == MIDI_KEY_PRESSURE && a.a == 60 && a.b == 44);
        }

        assert(old == (overlap ? 2u : 0u) && fresh == 2 && untouched == 1 && bend == 1 &&
               pressure == 2);
        assert(e->undo_count == 1);
        DmBytes packed = {0};
        DmProject round_trip;
        dm_project_init(&round_trip);
        assert(dm_pack(&e->project, &packed));
        assert(dm_unpack(&round_trip, packed.data, packed.size, error, sizeof(error)));
        assert(round_trip.automation_count == e->project.automation_count);
        dm_project_free(&round_trip);
        dm_bytes_free(&packed);
    }

    reset(e, false);
    e->loop_enabled[1] = true;
    e->loop_start[1] = 0;
    e->loop_end[1] = 4;
    send_at(e, 1.5, MIDI_NOTE_ON, 67, 91);
    ed_midi_input_frame(e);
    ++generation;
    ++loop_id;
    send_at(e, .1, MIDI_NOTE_OFF, 67, 64);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(ed_pattern(e)->count == 1 && fabs(ed_pattern(e)->notes[0].beat - 3) < 1e-7 &&
           fabs(ed_pattern(e)->notes[0].length - 1) < 1e-7);

    reset(e, false);
    unsigned long long revision = e->export_revision;

    for (unsigned i = 0; i < 2000; ++i) {
        send_at(e, i / 10000.0, MIDI_CONTROL, MIDI_CC_MODULATION, i % 128);
    }

    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(e->project.automation_count == 2000);
    assert(e->export_revision - revision <= 2 && e->undo_count == 1);
    ed_toggle_record(e);
    ed_undo(e, false);
    assert(e->project.automation_count == 0);

    reset(e, false);
    e->project.clip_count = 0;
    e->preview_override = false;
    e->selected_channel = e->pattern_channel = 0;
    e->project.channels[3].enabled = true;
    e->project.channels[3].program = 0;
    e->note_ports[3] = 1;
    assert(dm_note_add(ed_pattern(e), (DmNote){0, 4, 60, 100, 0, 1}));
    assert(dm_automation_add(&e->project, (DmAutomation){.beat = 1,
                                                         .type = MIDI_CONTROL,
                                                         .channel = 3,
                                                         .a = MIDI_CC_MODULATION,
                                                         .b = 5,
                                                         .pattern = e->selected_pattern,
                                                         .port = 1}));
    send_at(e, .1, MIDI_CONTROL, MIDI_CC_MODULATION, 40);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    e->selected_channel = 3;
    send_at(e, .2, MIDI_CONTROL, MIDI_CC_MODULATION, 99);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(e->pattern_channel == 3 && ed_pattern(e)->count == 0 && e->undo_count == 1);
    unsigned first_route = 0, second_route = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        first_route += a.channel == 0 && a.port == 0 && a.b == 40;
        second_route += a.channel == 3 && a.port == 1 && a.b == 99;
        assert(a.b != 5);
    }

    assert(first_route == 1 && second_route == 1);

    reset(e, false);
    int survivor = dm_pattern_add(&e->project, "Surviving pattern");
    assert(survivor >= 0);
    assert(dm_note_add(&e->project.patterns[survivor], (DmNote){0, 2, 72, 100, 0, 0}));
    send_at(e, .1, MIDI_NOTE_ON, 65, 90);
    send_at(e, .1, MIDI_CONTROL, MIDI_CC_MODULATION, 55);
    ed_midi_input_frame(e);
    dm_pattern_remove(&e->project, e->selected_pattern);
    e->selected_pattern = 0;
    ed_bind_pattern_channel(e);
    ed_commit(e);
    send_at(e, .2, MIDI_NOTE_OFF, 65, 64);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(ed_pattern(e)->count == 1 && ed_pattern(e)->notes[0].key == 72);
    assert(e->project.automation_count == 0 && !e->take_started);

    reset(e, false);
    send_at(e, .25, MIDI_NOTE_ON, 70, 73);
    ed_midi_input_frame(e);
    ed_toggle_record(e);
    assert(ed_pattern(e)->count == 1 && ed_pattern(e)->notes[0].velocity == 73);
    assert(!e->record);
    ed_pattern(e)->kind = 1;
    ed_toggle_record(e);
    assert(!e->record);

    /* Computer-key releases (focus changes, opening windows) leave held MIDI notes alone. */
    reset(e, false);
    send_at(e, .25, MIDI_NOTE_ON, 62, 80);
    ed_midi_input_frame(e);
    SDL_Event lost = {.type = SDL_EVENT_WINDOW_FOCUS_LOST};
    editor_event(e, &lost);
    send_at(e, .75, MIDI_NOTE_OFF, 62, 64);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(ed_pattern(e)->count == 1 && fabs(ed_pattern(e)->notes[0].length - 1) < 1e-7);

    /* Controller-only batches skip the playback rebuild; a loop pass replaces the previous
       pass's points and rebuilds playback for them. */
    reset(e, false);
    e->loop_enabled[1] = true;
    e->loop_start[1] = 0;
    e->loop_end[1] = 4;
    send_at(e, .2, MIDI_CONTROL, MIDI_CC_EXPRESSION, 70);
    send_at(e, .4, MIDI_CONTROL, MIDI_CC_EXPRESSION, 71);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(e->project.automation_count == 2);
    assert(!recording_feedback_audio_event(audio, MIDI_CONTROL, MIDI_CC_EXPRESSION, 70));
    ++loop_id;
    send_at(e, .3, MIDI_CONTROL, MIDI_CC_EXPRESSION, 90);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    unsigned first_pass = 0, second_pass = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        first_pass += a.a == MIDI_CC_EXPRESSION && a.b < 90;
        second_pass += a.a == MIDI_CC_EXPRESSION && a.b == 90;
    }

    assert(first_pass == 0 && second_pass == 1 && e->undo_count == 1);
    assert(!recording_feedback_audio_event(audio, MIDI_CONTROL, MIDI_CC_EXPRESSION, 70));
    assert(recording_feedback_audio_event(audio, MIDI_CONTROL, MIDI_CC_EXPRESSION, 90));

    /* Isolated preference round trip, including separators, quotes and missing devices. */
    snprintf(e->settings_path, sizeof(e->settings_path),
             "build/development/midi_recording_test.ini");
    snprintf(e->input_names[0], DM_MIDI_NAME, "Name, with \"quotes\"");
    snprintf(e->input_names[1], DM_MIDI_NAME, "Missing device");
    assert(dm_preferences_save(e, 0, ed_action_names));
    memset(e->input_names, 0, sizeof(e->input_names));
    assert(dm_preferences_load(e, 0));
    assert(!strcmp(e->input_names[0], "Name, with \"quotes\""));
    assert(!strcmp(e->input_names[1], "Missing device"));
    SDL_RemovePath(e->settings_path);
    e->settings_path[0] = 0;
    reset(e, false);
    ed_toggle_record(e);
    e->automation_cc = MIDI_CC_MODULATION;
    e->automation_all = false;
    e->automation_node = 500;

    for (unsigned i = 0; i < 1000; ++i) {
        point(e, i / 100000.0, MIDI_CC_MODULATION, i % 128);
    }

    UiRect lane = {20, 20, 800, 300};
    UiMouse mouse = {0};
    mouse.pos = ed_automation_point(e, e->project.automation[500], lane, &e->roll_view, 80);
    int expected = -1;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        UiPoint at = ed_automation_point(e, e->project.automation[i], lane, &e->roll_view, 80);

        if (fabsf(at.x - mouse.pos.x) < 9 && fabsf(at.y - mouse.pos.y) < 9) {
            expected = (int)i;
        }
    }

    ui_frame();
    ui_root(0, 360);
    ui_canvas("Dense MIDI validation", 840, 340);
    ui_clip(lane);
    assert(ed_automation_plot(e, lane, &e->roll_view, 80, mouse) == expected);
    ed_curve_handles(e, lane, &e->roll_view, 80, mouse, false, true);
    ui_unclip();
    ui_end();
    SDL_RenderClear(renderer);
    ui_render(renderer);
    assert(e->project.automation_count == 1000);
    e->settings_path[0] = 0;
    editor_free(e);
    free(e);
    fixture_free(&fixture);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    puts(
        "MIDI recording: timing, velocity, independent sources, wait, lanes, loop, batch, undo and preferences passed");
    return 0;
}
