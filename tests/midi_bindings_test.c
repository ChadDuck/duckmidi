#include "editor/internal.h"
#include "midi_input_fixture.h"

void recording_feedback_audio_pause(DmAudio *audio);
void recording_feedback_audio_render(DmAudio *audio, int frames);
int recording_feedback_audio_port_cc(DmAudio *audio, unsigned port, int channel, int cc);
void test_midi_binding_controls(Editor *e, EdMidiTarget target);
void test_midi_mixer_learn(Editor *e);
void test_midi_binding_clear_all(Editor *e);

static MidiFixture fixture;

static const EdMidiTarget volume = {ED_MIDI_VOLUME, 0, 0};
static const EdMidiTarget pan = {ED_MIDI_PAN, 0, 2};
static const EdMidiTarget metronome = {.kind = ED_MIDI_METRONOME};

static void emit(Editor *e, unsigned device, uint8_t status, uint8_t a, uint8_t b) {
    fixture_emit(&fixture, device, status, a, b, SDL_GetTicksNS());
    ed_midi_input_frame(e);
}

static void bind(Editor *e, EdMidiTarget target, bool any, uint8_t status, uint8_t a, uint8_t b) {
    ed_midi_binding_learn(e, target, any);
    emit(e, 0, status, a, b);
    assert(!e->midi_learn.started && ed_midi_binding_find(e, target));
    assert(fixture_drain(e->midi_input, 0, NULL) == 0);
}

static uint32_t transport(Editor *e) {
    DmMidiClockValue clock;
    assert(dm_midi_clock_read(dm_midi_input_clock(e->midi_input), &clock));
    return clock.transport;
}

static void buttons(Editor *e) {
    assert(!e->active);
    test_midi_binding_controls(e, metronome);
    assert(ed_midi_binding_learning(e, metronome));
    emit(e, 0, MIDI_CONTROL | 15, 75, 0);
    assert(ed_midi_binding_find(e, metronome));
    assert(fixture_drain(e->midi_input, 0, NULL) == 0);
    bool initial = e->metronome;
    emit(e, 0, MIDI_CONTROL | 15, 75, 127);
    assert(e->metronome != initial);
    emit(e, 0, MIDI_CONTROL | 15, 75, 120);
    emit(e, 0, MIDI_CONTROL | 15, 75, 0);
    assert(e->metronome != initial);
    emit(e, 0, MIDI_CONTROL | 15, 75, 127);
    assert(e->metronome == initial);
    ed_midi_binding_find(e, metronome)->toggle = true;
    emit(e, 0, MIDI_CONTROL | 15, 75, 0);
    assert(e->metronome != initial);
    emit(e, 0, MIDI_CONTROL | 15, 75, 0);
    assert(e->metronome != initial);
    emit(e, 0, MIDI_CONTROL | 15, 75, 127);
    assert(e->metronome == initial);

    EdMidiTarget play = {.kind = ED_MIDI_PLAY};
    ed_midi_binding_learn(e, play, false);
    SDL_Event escape = {.key = {.type = SDL_EVENT_KEY_DOWN, .scancode = SDL_SCANCODE_ESCAPE}};
    editor_event(e, &escape);
    assert(!e->midi_learn.started && !ed_midi_binding_find(e, play));
    ed_midi_binding_learn(e, play, false);
    ed_midi_bindings_tick(e, e->midi_learn.started + 10 * SDL_NS_PER_SECOND);
    assert(!e->midi_learn.started && !ed_midi_binding_find(e, play));

    /* The same source moves to a new target, even when it changes to any device. */
    bind(e, (EdMidiTarget){.kind = ED_MIDI_CHANNEL_NEXT}, true, MIDI_CONTROL | 15, 75, 127);
    assert(!ed_midi_binding_find(e, metronome));
}

static void mixer(Editor *e) {
    ed_new_project(e);
    e->context = 0;
    assert(e->project.channels[0].enabled);
    test_midi_mixer_learn(e);
    assert(ed_midi_binding_learning(e, volume));
    emit(e, 0, MIDI_NOTE_ON | 15, 74, 100);
    emit(e, 0, MIDI_CONTROL | 15, MIDI_CC_RPN_MSB, 0);
    assert(!ed_midi_binding_find(e, volume) && ed_midi_binding_learning(e, volume));
    emit(e, 0, MIDI_NOTE_OFF | 15, 74, 64);
    assert(fixture_drain(e->midi_input, 0, NULL) == 3);
    emit(e, 0, MIDI_CONTROL | 15, 74, 11);
    EdMidiBinding *binding = ed_midi_binding_find(e, volume);
    assert(binding && !strcmp(binding->source.device, "Fixture 0") &&
           binding->source.channel == 15 && binding->source.number == 74);
    assert(fixture_drain(e->midi_input, 0, NULL) == 0);
    int original = e->project.channels[0].volume;
    int undo = e->undo_count;
    uint32_t generation = transport(e);
    emit(e, 0, MIDI_CONTROL | 15, 74, 20);
    assert(e->project.channels[0].volume == original && e->undo_count == undo);
    emit(e, 0, MIDI_CONTROL | 15, 74, 120);
    assert(e->project.channels[0].volume == 120 && e->undo_count == undo + 1);

    for (unsigned i = 0; i < 50; ++i) {
        fixture_emit(&fixture, 0, MIDI_CONTROL | 15, 74, 70 + i, SDL_GetTicksNS());
    }

    ed_midi_input_frame(e);
    assert(e->project.channels[0].volume == 119 && e->undo_count == undo + 1);
    assert(fixture_drain(e->midi_input, 0, NULL) == 0);
    recording_feedback_audio_render(e->audio, 64);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 0, MIDI_CC_VOLUME) == 119);
    assert(transport(e) == generation);
    ed_midi_bindings_flush(e, e->midi_mix_last + 250 * SDL_NS_PER_MS);
    assert(transport(e) == generation + 1);
    ed_midi_bindings_flush(e, e->midi_mix_last + 500 * SDL_NS_PER_MS);
    assert(transport(e) == generation + 1);

    bind(e, (EdMidiTarget){.kind = ED_MIDI_UNDO}, false, MIDI_CONTROL, 76, 0);
    emit(e, 0, MIDI_CONTROL, 76, 127);
    assert(e->project.channels[0].volume == original && e->undo_count == undo);
    emit(e, 0, MIDI_CONTROL | 15, 74, 127);
    assert(e->project.channels[0].volume == original);
    emit(e, 0, MIDI_CONTROL | 15, 74, 90);
    assert(e->project.channels[0].volume == 90);
    e->project.channels[0].volume = 50;
    emit(e, 0, MIDI_CONTROL | 15, 74, 95);
    assert(e->project.channels[0].volume == 50);
    emit(e, 0, MIDI_CONTROL | 15, 74, 40);
    assert(e->project.channels[0].volume == 40);

    bind(e, pan, false, MIDI_CONTROL | 15, 71, 0);
    e->input_pickup = false;
    emit(e, 0, MIDI_CONTROL | 15, 71, 10);
    assert(dm_port_mix(&e->project, 2, 0).pan == 10);
    EdMidiTarget mute = {ED_MIDI_MUTE, 0, 0}, solo = {ED_MIDI_SOLO, 0, 0};
    bind(e, mute, false, MIDI_NOTE_ON | 15, 60, 99);
    emit(e, 0, MIDI_NOTE_OFF | 15, 60, 64);
    assert(!e->project.channels[0].mute);
    emit(e, 0, MIDI_NOTE_ON | 15, 60, 60);
    assert(e->project.channels[0].mute);
    bind(e, solo, false, MIDI_CONTROL | 15, 73, 0);
    emit(e, 0, MIDI_CONTROL | 15, 73, 127);
    assert(e->project.channels[0].solo);
}

static void recording(Editor *e) {
    e->context = 2;
    e->pattern_mode = true;
    e->wait_input = false;
    e->overwrite = true;
    ed_toggle_record(e);
    ed_play(e);
    assert(e->record && dm_audio_playing(e->audio));
    uint64_t ns = SDL_GetTicksNS();
    dm_midi_clock_write(dm_midi_input_clock(e->midi_input),
                        (DmMidiClockValue){.ticks_ns = ns, .position = .173, .playing = true});
    fixture_emit(&fixture, 0, MIDI_CONTROL | 15, 74, 81, ns);
    fixture_emit(&fixture, 0, MIDI_CONTROL, 74, 52, ns);
    ed_midi_input_frame(e);
    ed_midi_input_flush(e);
    assert(fixture_drain(e->midi_input, 0, NULL) == 1);
    assert(e->project.automation_count == 1);
    DmAutomation recorded = e->project.automation[0];
    assert(recorded.type == MIDI_CONTROL && recorded.a == 74 && recorded.b == 52);
    assert(dm_graph_resolution(ed_pattern(e), 74) == 16);

    /* A transport binding can finish a take during the drain without entering it again. */
    bind(e, (EdMidiTarget){.kind = ED_MIDI_STOP}, false, MIDI_CONTROL, 116, 0);
    emit(e, 0, MIDI_CONTROL, 116, 127);
    assert(!dm_audio_playing(e->audio) && e->cursor_beat[1] == e->start_beat[1]);
    assert(e->project.automation_count == 1);

    /* Stop returns to the set playhead first, then to the beginning. */
    ed_toggle_record(e);
    ed_seek(e, 2);
    double set = e->start_beat[1];
    assert(set > 0);
    e->cursor_beat[1] = set + .25; /* Where a held MIDI pause left the playhead. */
    emit(e, 0, MIDI_CONTROL, 116, 0);
    emit(e, 0, MIDI_CONTROL, 116, 127);
    assert(e->cursor_beat[1] == set && e->start_beat[1] == set);
    emit(e, 0, MIDI_CONTROL, 116, 0);
    emit(e, 0, MIDI_CONTROL, 116, 127);
    assert(e->cursor_beat[1] == 0 && e->start_beat[1] == 0);

    /* Held MIDI keys light the piano keys whether or not a take is recording. */
    emit(e, 0, MIDI_NOTE_ON | 2, 61, 90);
    assert(ed_midi_input_lit(e, 61) && !ed_midi_input_lit(e, 62));
    emit(e, 0, MIDI_NOTE_OFF | 2, 61, 0);
    assert(!ed_midi_input_lit(e, 61));
    emit(e, 0, MIDI_NOTE_ON | 2, 61, 90);
    emit(e, 0, MIDI_NOTE_ON | 2, 61, 0);
    assert(!ed_midi_input_lit(e, 61));
    ed_toggle_record(e);
    ed_toggle_record(e);
}

static void preferences(Editor *e) {
    snprintf(e->settings_path, sizeof(e->settings_path),
             "agents/reports/midi-bindings-fixture.ini");
    DmMidiBindingSource *source = &ed_midi_binding_find(e, pan)->source;
    snprintf(source->device, sizeof(source->device), "Knob, \"quoted\" name");
    ed_midi_binding_find(e, (EdMidiTarget){.kind = ED_MIDI_CHANNEL_NEXT})->toggle = true;
    size_t count = ed_midi_binding_count(e);
    assert(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names));
    ed_midi_binding_clear(e, (EdMidiTarget){.kind = ED_MIDI_TARGETS});
    assert(!ed_midi_binding_count(e));
    assert(dm_preferences_load(e, ed_shortcut_schema()) && ed_midi_binding_count(e) == count);
    assert(!strcmp(ed_midi_binding_find(e, pan)->source.device, "Knob, \"quoted\" name"));
    EdMidiBinding *next = ed_midi_binding_find(e, (EdMidiTarget){.kind = ED_MIDI_CHANNEL_NEXT});
    assert(next && next->toggle && !*next->source.device);
    SDL_RemovePath(e->settings_path);
    snprintf(e->settings_path, sizeof(e->settings_path),
             "agents/reports/midi-invalid-bindings-fixture.ini");
    static const char invalid[] = "[application]\nschema=3\n[midi_bindings]\n"
                                  "action.play=\"\",16,cc,75,momentary\n"
                                  "action.stop=\"Fixture 0\",16,cc,75,toggle\n";
    assert(SDL_SaveFile(e->settings_path, invalid, sizeof(invalid) - 1));
    assert(!dm_preferences_load(e, ed_shortcut_schema()) && ed_midi_binding_count(e) == count);
    SDL_RemovePath(e->settings_path);
}

static void reconnect(Editor *e) {
    atomic_store(&fixture.available, 2);
    dm_midi_input_refresh(e->midi_input);
    fixture_wait(e->midi_input, 0, DM_MIDI_MISSING);
    dm_midi_input_close(e->midi_input, "Fixture 0");
    fixture_wait(e->midi_input, 0, DM_MIDI_CLOSED);
    ed_midi_input_frame(e);
    fixture_drain(e->midi_input, 0, NULL);
    assert(dm_midi_input_open(e->midi_input, "Fixture 1"));
    fixture_wait(e->midi_input, 0, DM_MIDI_OPEN);
    atomic_store(&fixture.available, 3);
    assert(dm_midi_input_open(e->midi_input, "Fixture 0"));
    fixture_wait(e->midi_input, 1, DM_MIDI_OPEN);

    int volume_before = e->project.channels[0].volume;
    emit(e, 1, MIDI_CONTROL | 15, 74, 13);
    assert(e->project.channels[0].volume == volume_before);
    assert(fixture_drain(e->midi_input, 0, NULL) == 1);
    fixture_emit(&fixture, 0, MIDI_CONTROL | 15, 74, 33, SDL_GetTicksNS());
    assert(fixture_drain(e->midi_input, 0, NULL) == 0);
    ed_midi_input_frame(e);
    assert(e->project.channels[0].volume == 33);

    e->project.channels[3].enabled = true;
    e->selected_channel = 0;
    emit(e, 1, MIDI_CONTROL | 15, 75, 127);
    assert(e->selected_channel == 3 && e->instrument_channel < 0);
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO));
    SDL_Window *window = SDL_CreateWindow("MIDI binding validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    assert(window && renderer && ui_init(window, renderer, NULL));
    char error[256] = {0};
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    assert(audio);
    recording_feedback_audio_pause(audio);
    Editor *e = calloc(1, sizeof(*e));
    assert(e);
    editor_init(e, window, renderer, audio, "", "", true);
    ed_midi_input_attach(e, fixture_create(&fixture));
    assert(dm_midi_input_open(e->midi_input, "Fixture 0"));
    fixture_wait(e->midi_input, 0, DM_MIDI_OPEN);
    buttons(e);
    mixer(e);
    recording(e);
    preferences(e);
    reconnect(e);
    test_midi_binding_clear_all(e);
    assert(!ed_midi_binding_count(e));
    emit(e, 0, MIDI_CONTROL | 15, 74, 15);
    assert(fixture_drain(e->midi_input, 0, NULL) == 1);
    editor_free(e);
    free(e);
    fixture_free(&fixture);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    puts("MIDI learn, pickup, mixer gestures, recording isolation and reconnection passed");
    return 0;
}
