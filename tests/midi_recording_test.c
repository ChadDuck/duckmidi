#include "editor/internal.h"
#include "midi_input_fixture.h"

void recording_feedback_audio_pause(DmAudio *audio);
void recording_feedback_audio_clock(DmAudio *audio, uint64_t ns, double position, double latency);
void recording_feedback_audio_render(DmAudio *audio, int frames);
int recording_feedback_audio_port_cc(DmAudio *audio, unsigned port, int channel, int cc);
int recording_feedback_audio_port_bend(DmAudio *audio, unsigned port, int channel);
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

static void recorded_graph(Editor *e) {
    reset(e, true);
    send_at(e, .173, MIDI_CONTROL, MIDI_CC_MODULATION, 48);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(dm_graph_resolution(ed_pattern(e), MIDI_CC_MODULATION) == DM_GRAPH_MAX &&
           e->project.automation_count == 1);
    assert(fabs(e->project.automation[0].beat - .346) < 1e-7);
    ed_toggle_record(e);
    ed_undo(e, false);
    assert(!dm_pattern_has_graph(ed_pattern(e)) && !e->project.automation_count);

    reset(e, true);
    point(e, .5, MIDI_CC_MODULATION, 10);
    point(e, 1, MIDI_CC_MODULATION, 70);
    e->project.automation[0].curve = e->project.automation[1].curve = 1;
    send_at(e, .173, MIDI_PITCH_BEND, 30, 70);
    send_at(e, .225, MIDI_CONTROL, MIDI_CC_MODULATION, 80);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(dm_graph_resolution(ed_pattern(e), DM_EVENT_BEND) == DM_GRAPH_MAX &&
           !dm_graph_resolution(ed_pattern(e), MIDI_CC_MODULATION) &&
           e->project.automation_count == 4);
    assert(e->project.automation[2].curve == 1 && e->project.automation[3].curve == 1);
    assert(strstr(e->status, "Choose Graph"));

    reset(e, true);
    ed_pattern(e)->graph_lanes[MIDI_CC_MODULATION] = DM_GRAPH_POINT;
    send_at(e, .173, MIDI_CONTROL, MIDI_CC_MODULATION, 48);
    send_at(e, .225, MIDI_CONTROL, MIDI_CC_EXPRESSION, 60);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(!dm_graph_resolution(ed_pattern(e), MIDI_CC_MODULATION) &&
           dm_graph_resolution(ed_pattern(e), MIDI_CC_EXPRESSION) == DM_GRAPH_MAX &&
           e->project.automation_count == 2);

    reset(e, true);
    send_at(e, .173, MIDI_CONTROL, MIDI_CC_SUSTAIN, 127);
    send_at(e, .225, MIDI_CONTROL, MIDI_CC_SUSTAIN, 0);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(!dm_pattern_has_graph(ed_pattern(e)) && e->project.automation_count == 2);
    send_at(e, .273, MIDI_CONTROL, MIDI_CC_RPN_MSB, 0);
    send_at(e, .375, MIDI_CHANNEL_PRESSURE, 30, 0);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(!dm_pattern_has_graph(ed_pattern(e)) && e->project.automation_count == 4);
}

static void event_binding(Editor *e, EdMidiTargetKind target, uint8_t status, uint8_t a) {
    DmBytes bindings = {0};
    EdMidiBinding binding = {
        .source = {.kind = status == MIDI_CONTROL ? DM_MIDI_SOURCE_CC : DM_MIDI_SOURCE_BEND,
                   .channel = 2,
                   .number = a},
        .target = {.kind = target},
    };
    assert(ed_midi_binding_append(&bindings, binding));
    assert(ed_midi_bindings_replace(e, &bindings));
}

static void mapped_events(Editor *e) {
    reset(e, true);
    e->project.channels[3].enabled = true;
    e->pattern_channel = 3;
    e->note_ports[3] = 0;
    e->preview_override = true;
    e->preview_program = 0;
    e->selected_channel = 0;
    e->edit_focus = FOCUS_PLAYLIST;
    e->input_controllers = DM_INPUT_CONTROLLERS_SONG;
    ed_midi_input_apply(e);
    fixture_drain(e->midi_input, 0, NULL);
    event_binding(e, ED_MIDI_EVENT_MODULATION, MIDI_CONTROL, 21);
    send_at(e, .173, MIDI_CONTROL | 2, 21, 48);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(e->project.automation_count == 1);
    DmAutomation recorded = e->project.automation[0];
    assert(recorded.type == MIDI_CONTROL && recorded.a == MIDI_CC_MODULATION && recorded.b == 48 &&
           recorded.channel == 3);
    assert(dm_graph_resolution(ed_pattern(e), MIDI_CC_MODULATION) == DM_GRAPH_MAX);
    assert(!dm_graph_resolution(ed_pattern(e), 21));
    assert(!fixture_drain(e->midi_input, 0, NULL));
    recording_feedback_audio_render(e->audio, 64);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 3, MIDI_CC_MODULATION) == 48);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 0, MIDI_CC_MODULATION) != 48);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 3, 21) != 48);

    event_binding(e, ED_MIDI_EVENT_BEND, MIDI_CONTROL, 21);
    send_at(e, .225, MIDI_CONTROL | 2, 21, 64);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    recorded = e->project.automation[1];
    int bend = 64 * MIDI_BEND_MAX / MIDI_DATA_MAX;
    assert(recorded.type == MIDI_PITCH_BEND && (recorded.a | (recorded.b << 7)) == bend &&
           recorded.channel == 3);
    recording_feedback_audio_render(e->audio, 64);
    assert(recording_feedback_audio_port_bend(e->audio, 0, 3) == bend);
    event_binding(e, ED_MIDI_EVENT_EXPRESSION, MIDI_PITCH_BEND, 0);
    send_at(e, .3, MIDI_PITCH_BEND | 2, MIDI_DATA_MAX, MIDI_DATA_MAX);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    recorded = e->project.automation[2];
    assert(recorded.type == MIDI_CONTROL && recorded.a == MIDI_CC_EXPRESSION &&
           recorded.b == MIDI_DATA_MAX && recorded.channel == 3);
    /* Closing the device clears translated live destinations as well as preview routes. */
    dm_midi_input_close(e->midi_input, "Fixture 0");
    fixture_wait(e->midi_input, 0, DM_MIDI_CLOSED);
    recording_feedback_audio_render(e->audio, 64);
    assert(recording_feedback_audio_port_bend(e->audio, 0, 3) == MIDI_BEND_CENTER);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 3, MIDI_CC_EXPRESSION) == MIDI_DATA_MAX);
    assert(dm_midi_input_open(e->midi_input, "Fixture 0"));
    fixture_wait(e->midi_input, 0, DM_MIDI_OPEN);
    ed_midi_input_frame(e);
    DmBytes empty = {0};
    assert(ed_midi_bindings_replace(e, &empty));
    e->input_controllers = DM_INPUT_CONTROLLERS_KEYBOARD;
    ed_midi_input_apply(e);
}

static void automation_recording(Editor *e) {
    reset(e, true);
    ed_toggle_record(e);
    ed_pattern(e)->kind = 1;
    ed_pattern(e)->mode = 0;
    ed_pattern(e)->event = MIDI_CC_MODULATION;
    e->project.clip_count = 0;
    e->selected_channel = 4;
    e->project.channels[4].enabled = true;
    e->preview_override = false;
    e->input_controllers = DM_INPUT_CONTROLLERS_SONG;
    ed_midi_input_apply(e);
    ed_publish(e);
    ed_toggle_record(e);
    e->wait_input = true;
    ed_play(e);
    ed_play(e);
    assert(e->record && e->wait_armed);
    uint64_t ns = SDL_GetTicksNS();
    dm_midi_clock_write(dm_midi_input_clock(e->midi_input),
                        (DmMidiClockValue){.ticks_ns = ns, .transport = ++generation});
    fixture_emit(&fixture, 0, MIDI_CONTROL, MIDI_CC_MODULATION, 55, ns);
    ed_midi_input_frame(e);
    assert(!e->wait_armed && dm_audio_playing(e->audio));
    send_at(e, .175, MIDI_CONTROL, MIDI_CC_EXPRESSION, 22);
    send_at(e, .18, MIDI_NOTE_ON, 60, 100);
    send_at(e, .19, MIDI_NOTE_OFF, 60, 0);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(e->project.automation_count == 1 && !ed_pattern(e)->count);
    DmAutomation recorded = e->project.automation[0];
    assert(recorded.a == MIDI_CC_MODULATION && recorded.b == 55 && recorded.channel == 4);
    assert(!dm_pattern_has_graph(ed_pattern(e)));
    recording_feedback_audio_render(e->audio, 64);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 4, MIDI_CC_MODULATION) == 55);

    event_binding(e, ED_MIDI_EVENT_MODULATION, MIDI_CONTROL, 21);
    send_at(e, .225, MIDI_CONTROL | 2, 21, 70);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(e->project.automation_count == 2);
    recorded = e->project.automation[1];
    assert(recorded.a == MIDI_CC_MODULATION && recorded.b == 70 && recorded.channel == 4);
    assert(dm_clip_add(&e->project, (DmClip){.pattern = e->selected_pattern, .channel = 3}));
    assert(dm_clip_add(&e->project,
                       (DmClip){.pattern = e->selected_pattern, .channel = 5, .beat = 4}));
    e->project.channels[3].enabled = e->project.channels[5].enabled = true;
    e->selected_clip = 1;
    e->note_ports[5] = 0;
    send_at(e, .3, MIDI_CONTROL | 2, 21, 80);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(e->pattern_channel == 5 && e->project.automation_count == 3);
    assert(e->project.automation[2].channel == 5 && e->project.automation[2].b == 80);
    e->selected_clip = -1;
    send_at(e, .4, MIDI_CONTROL | 2, 21, 90);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(e->pattern_channel == 3 && e->project.automation_count == 4);
    assert(e->project.automation[3].channel == 3 && e->project.automation[3].b == 90);
    send_at(e, .45, MIDI_CONTROL | 2, 21, 95);
    ed_toggle_record(e);
    assert(e->project.automation_count == 5 && e->project.automation[4].b == 95);
    ed_undo(e, false);
    assert(!e->project.automation_count && !ed_pattern(e)->count);
    DmBytes empty = {0};
    assert(ed_midi_bindings_replace(e, &empty));
    e->input_controllers = DM_INPUT_CONTROLLERS_KEYBOARD;
    ed_midi_input_apply(e);
}

static DmNote record_pad(Editor *e, int program, uint8_t key, bool unmap_before_release) {
    reset(e, false);
    e->preview_override = true;
    e->preview_program = program;
    send_at(e, .125, MIDI_NOTE_ON | 9, key, 100);
    ed_midi_input_frame(e);

    if (unmap_before_release) {
        memset(e->percussion_midi_global, 0, sizeof(e->percussion_midi_global));
    }

    send_at(e, .375, MIDI_NOTE_OFF | 9, key, 64);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(ed_pattern(e)->count == 1);
    return ed_pattern(e)->notes[0];
}

/* Incoming notes on the drum route play their assigned drum; melodic routes ignore the map. */
static void percussion_mapping(Editor *e) {
    e->percussion_midi_global[38] = 49 + 1;
    assert(record_pad(e, 128, 49, false).key == 38);
    assert(record_pad(e, 0, 49, false).key == 49);
    assert(record_pad(e, 128, 50, false).key == 50);
    assert(record_pad(e, 128, 49, true).key == 38);

    /* Listen for MIDI: controllers keep listening; the next Note On assigns and is not recorded. */
    reset(e, false);
    ed_percussion_open(e);
    e->percussion_note = 42;
    e->percussion_midi_listening = true;
    send_at(e, .125, MIDI_CONTROL | 9, MIDI_CC_MODULATION, 20);
    ed_midi_input_frame(e);
    assert(e->percussion_midi_listening && *e->percussion_feedback);
    send_at(e, .25, MIDI_NOTE_ON | 9, 51, 100);
    ed_midi_input_frame(e);
    send_at(e, .5, MIDI_NOTE_OFF | 9, 51, 64);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(!e->percussion_midi_listening && e->percussion_midi_draft[42] == 52 &&
           ed_pattern(e)->count == 0);
    ed_percussion_save(e, true);
    assert(e->project.percussion_override && e->project.percussion_midi[42] == 52 &&
           !e->project.percussion_midi[38]);
    ed_percussion_close(e);

    /* The global MIDI mapping is saved beside the keyboard mapping. */
    snprintf(e->settings_path, sizeof(e->settings_path),
             "build/development/midi_recording_percussion.ini");
    e->percussion_midi_global[38] = 49 + 1;
    assert(dm_preferences_save(e));
    memset(e->percussion_midi_global, 0, sizeof(e->percussion_midi_global));
    assert(dm_preferences_load(e) && e->percussion_midi_global[38] == 50);
    static const char duplicate[] = "[application]\nschema=3\n[percussion_midi]\n"
                                    "acoustic_snare=49\nbass_drum_1=49\n";
    assert(SDL_SaveFile(e->settings_path, duplicate, sizeof(duplicate) - 1) &&
           !dm_preferences_load(e) && e->percussion_midi_global[38] == 50);
    SDL_RemovePath(e->settings_path);
    memset(e->percussion_midi_global, 0, sizeof(e->percussion_midi_global));
    e->preview_override = false;

    /* Outside the pattern editor, any channel marked as percussion plays the mapping. */
    reset(e, false);
    e->edit_focus = FOCUS_NONE;
    e->project.channels[3].drums = true;
    e->project.channels[MIDI_DRUM_CHANNEL].drums = false;
    assert(ed_route_drums(e, 3) && !ed_route_drums(e, 4) && !ed_route_drums(e, MIDI_DRUM_CHANNEL));
    ed_focus_timeline(e, CONTEXT_ROLL);
    assert(ed_route_drums(e, MIDI_DRUM_CHANNEL) && !ed_route_drums(e, 3));
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

    /* Computer-keyboard notes are timed at their key events against the heard position, like
       device notes: the clock reads 1 s, the output is 50 ms behind it, and the keys go down
       250 ms and up 750 ms after the clock was written, however late the frame handles them. */
    reset(e, false);
    assert(ed_record_start(e)); /* The take's own publish restarts the clock first. */
    uint64_t clock = SDL_GetTicksNS() - 2000000000ULL;
    recording_feedback_audio_clock(audio, clock, 1, .05);
    SDL_Event key = {.key = {.type = SDL_EVENT_KEY_DOWN,
                             .timestamp = clock + 250000000ULL,
                             .scancode = SDL_SCANCODE_Q,
                             .down = true}};
    editor_event(e, &key);
    key.key.type = SDL_EVENT_KEY_UP;
    key.key.timestamp = clock + 750000000ULL;
    key.key.down = false;
    editor_event(e, &key);
    assert(ed_pattern(e)->count == 1);
    note = ed_pattern(e)->notes[0];
    assert(fabs(note.beat - 2.4) < 1e-6 && fabs(note.length - 1) < 1e-6);

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
    ed_pattern(e)->mode = 1;
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
    assert(dm_preferences_save(e));
    memset(e->input_names, 0, sizeof(e->input_names));
    assert(dm_preferences_load(e));
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
    percussion_mapping(e);
    recorded_graph(e);
    mapped_events(e);
    automation_recording(e);
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
