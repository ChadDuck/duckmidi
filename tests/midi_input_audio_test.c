#include "../src/audio.c"
#include "midi_input_fixture.h"

static void block(DmAudio *a) {
    float out[BLOCK * 2];
    SDL_LockAudioStream(a->stream);
    run_block(a, out, BLOCK);
    SDL_UnlockAudioStream(a->stream);
}

static int control(Synth *s, unsigned channel, unsigned cc) {
    int value = -1;
    assert(fluid_synth_get_cc(s->synth, channel, cc, &value) == FLUID_OK);
    return value;
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_AUDIO));
    char error[512] = {0};
    DmAudio *a = dm_audio_create(error, sizeof(error));
    assert(a && a->stream);
    SDL_PauseAudioStreamDevice(a->stream);
    DmProject project;
    dm_project_init(&project);
    project.channels[4].enabled = true;
    assert(dm_audio_publish(a, &project, 0, false, error, sizeof(error)));
    dm_project_free(&project);
    MidiFixture f;
    DmMidiInput *input = fixture_create(&f);
    dm_audio_attach_midi_input(a, input);
    dm_audio_input_route(a, 4, true);
    assert(dm_midi_input_open(input, "Fixture 0"));
    fixture_wait(input, 0, DM_MIDI_OPEN);
    a->monitor = true;
    assert(fluid_synth_sfload(a->keyboard.synth, "agents/reports/generated-sine.sf2", 1) >= 0);
    keyboard_configure(a);
    a->keyboard_parts[4] = 4;
    fixture_emit(&f, 0, MIDI_NOTE_ON | 9, 60, 97, SDL_GetTicksNS());
    fixture_emit(&f, 0, MIDI_CONTROL | 9, MIDI_CC_MODULATION, 70, SDL_GetTicksNS());
    block(a);
    assert(a->input_notes[4][60] == 1);
    assert(fluid_synth_get_active_voice_count(a->keyboard.synth) > 0);
    assert(SDL_GetAtomicInt(&a->peaks[4]) > 0);
    dm_audio_preview(a, 4, 60, 100, true);
    block(a);
    fixture_emit(&f, 0, MIDI_NOTE_OFF | 9, 60, 64, SDL_GetTicksNS());
    block(a);
    assert(a->piano_held[4][60] && a->input_notes[4][60] == 0);
    dm_audio_preview(a, 4, 60, 0, false);
    block(a);
    assert(control(&a->keyboard, 4, MIDI_CC_MODULATION) == 70);
    assert(control(&a->live, 4, MIDI_CC_MODULATION) == 0);
    dm_audio_input_controllers(a, DM_INPUT_CONTROLLERS_SONG);
    fixture_emit(&f, 0, MIDI_CONTROL | 9, MIDI_CC_VOLUME, 41, SDL_GetTicksNS());
    block(a);
    assert(control(&a->live, 4, MIDI_CC_VOLUME) == 41 && a->mix_volume[4] == 41);
    dm_audio_input_controllers(a, DM_INPUT_CONTROLLERS_KEYBOARD);
    fixture_emit(&f, 0, MIDI_PITCH_BEND | 9, 0, 100, SDL_GetTicksNS());
    block(a);
    int bend;
    fluid_synth_get_pitch_bend(a->keyboard.synth, 4, &bend);
    assert(bend == 12800);
    a->keyboard_panic = true;
    block(a);
    fluid_synth_get_pitch_bend(a->keyboard.synth, 4, &bend);
    assert(bend == MIDI_BEND_CENTER && control(&a->keyboard, 4, MIDI_CC_MODULATION) == 0);

    /* Stop the real output consumer; inspect the same queue the worker reads. */
    SDL_SetAtomicInt(&a->quit, 1);
    SDL_WaitThread(a->midi_thread, NULL);
    a->midi_thread = NULL;
    SDL_SetAtomicInt(&a->midi.read, SDL_GetAtomicInt(&a->midi.write));
    fixture_emit(&f, 0, MIDI_CONTROL | 9, MIDI_CC_EXPRESSION, 55, SDL_GetTicksNS());
    block(a);
    Command cmd;
    assert(queue_get(&a->midi, &cmd, NULL));
    assert(cmd.status == (MIDI_CONTROL | 4) && cmd.a == MIDI_CC_EXPRESSION && cmd.b == 55);
    dm_midi_echo_write(dm_midi_input_echo(input), cmd.status, cmd.a, cmd.b, SDL_GetTicksNS());
    fixture_emit(&f, 0, cmd.status, cmd.a, cmd.b, SDL_GetTicksNS());
    DmMidiInputStats stats;
    dm_midi_input_stats(input, &stats);
    assert(stats.echoes == 1);
    dm_midi_input_set_thru(input, false);
    fixture_emit(&f, 0, MIDI_CONTROL, MIDI_CC_MODULATION, 20, SDL_GetTicksNS());
    block(a);
    assert(!queue_get(&a->midi, &cmd, NULL));
    dm_audio_input_offset(a, .01);
    a->position = 10;
    a->playing = true;
    a->recording = true;
    block(a);
    DmMidiClockValue value;
    assert(dm_midi_clock_read(dm_midi_input_clock(input), &value));
    DmMidiInputEvent event = {.ticks_ns = value.ticks_ns + 20000000};
    dm_midi_clock_stamp(value, &event);
    assert(fabs(event.song_seconds - 10.01) < .000001);
    a->keyboard_parts[4] = 20;
    a->port_live[1] = calloc(1, sizeof(Synth));
    assert(synth_create(a->port_live[1], NULL));
    dm_audio_input_controllers(a, DM_INPUT_CONTROLLERS_SONG);
    fixture_emit(&f, 0, MIDI_CONTROL, MIDI_CC_PAN, 27, SDL_GetTicksNS());
    block(a);
    assert(control(a->port_live[1], 4, MIDI_CC_PAN) == 27 && a->mix_pan[20] == 27);
    dm_audio_attach_midi_input(a, NULL);
    dm_midi_input_destroy(input);
    fixture_free(&f);
    dm_audio_destroy(a);
    SDL_Quit();
    puts(
        "MIDI audio: voices, controller policies, colored route, thru, echo, clock and panic passed");
    return 0;
}
