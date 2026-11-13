/* The audio engine compiled with accessors the desktop tests use to render blocks and read
   voices, controllers and scheduled events. The accessors change nothing by themselves. */
#include "audio_engine.h"
#include "wave_fixture.h"

uint64_t recording_feedback_worker_thread(DmAudio *a) {
    return a->worker ? SDL_GetThreadID(a->worker) : 0;
}

void recording_feedback_audio_pause(DmAudio *a) {
    SDL_PauseAudioStreamDevice(a->stream);
}

/* Sets the transport clock that input is timed against, as a rendered block would. */
void recording_feedback_audio_clock(DmAudio *a, uint64_t ns, double position, double latency) {
    dm_midi_clock_write(&a->clock, (DmMidiClockValue){ns, position, latency, a->input_transport,
                                                      true, a->input_loop});
}

void recording_feedback_audio_render(DmAudio *a, int frames) {
    SDL_LockAudioStream(a->stream);

    while (frames > 0) {
        float buffer[BLOCK * 2];
        int n = frames > BLOCK ? BLOCK : frames;
        run_block(a, buffer, n);
        frames -= n;
    }

    SDL_UnlockAudioStream(a->stream);
}

unsigned recording_feedback_audio_voice(DmAudio *a, int channel, int key) {
    fluid_voice_t *voices[512] = {0};
    fluid_synth_get_voicelist(a->keyboard.synth, voices, 512, -1);

    for (int i = 0; i < 512 && voices[i]; ++i) {
        if (fluid_voice_is_on(voices[i]) && fluid_voice_get_channel(voices[i]) == channel &&
            fluid_voice_get_key(voices[i]) == key) {
            return fluid_voice_get_id(voices[i]);
        }
    }

    return UINT32_MAX;
}

int recording_feedback_audio_count(DmAudio *a) {
    fluid_voice_t *voices[512] = {0};
    int count = 0;
    fluid_synth_get_voicelist(a->keyboard.synth, voices, 512, -1);

    for (int i = 0; i < 512 && voices[i]; ++i) {
        count += fluid_voice_is_on(voices[i]) != 0;
    }

    return count;
}

bool recording_feedback_audio_live_voice(DmAudio *a, int channel, int key) {
    fluid_voice_t *voices[512] = {0};
    fluid_synth_get_voicelist(a->live.synth, voices, 512, -1);

    for (int i = 0; i < 512 && voices[i]; i++) {
        if (fluid_voice_is_on(voices[i]) && fluid_voice_get_channel(voices[i]) == channel &&
            fluid_voice_get_key(voices[i]) == key) {
            return true;
        }
    }

    return false;
}

int recording_feedback_audio_override(DmAudio *a, int channel) {
    return a->keyboard_override[channel];
}

int recording_feedback_audio_preset(DmAudio *a, int channel, bool keyboard) {
    fluid_preset_t *preset =
        fluid_synth_get_channel_preset(keyboard ? a->keyboard.synth : a->live.synth, channel);
    return preset ? fluid_preset_get_num(preset) : -1;
}

int recording_feedback_audio_bank(DmAudio *a, int channel, bool keyboard) {
    fluid_preset_t *preset =
        fluid_synth_get_channel_preset(keyboard ? a->keyboard.synth : a->live.synth, channel);
    return preset ? fluid_preset_get_banknum(preset) : -1;
}

int recording_feedback_audio_port_cc(DmAudio *a, unsigned port, int channel, int cc) {
    Synth *s = live_port(a, port);
    int value = -1;

    if (s) {
        fluid_synth_get_cc(s->synth, channel, cc, &value);
    }

    return value;
}

int recording_feedback_audio_port_bend(DmAudio *a, unsigned port, int channel) {
    Synth *s = live_port(a, port);
    int value = -1;

    if (s) {
        fluid_synth_get_pitch_bend(s->synth, channel, &value);
    }

    return value;
}

bool recording_feedback_audio_port_voice(DmAudio *a, unsigned port, int channel, int key) {
    Synth *s = live_port(a, port);

    if (!s) {
        return false;
    }

    fluid_voice_t *voices[512] = {0};
    fluid_synth_get_voicelist(s->synth, voices, 512, -1);

    for (int i = 0; i < 512 && voices[i]; i++) {
        if (fluid_voice_is_on(voices[i]) && fluid_voice_get_channel(voices[i]) == channel &&
            fluid_voice_get_key(voices[i]) == key) {
            return true;
        }
    }

    return false;
}

bool recording_feedback_audio_event(DmAudio *a, int type, int key, int value) {
    bool found = false;
    SDL_LockAudioStream(a->stream);

    for (size_t i = 0; i < a->schedule.count; ++i) {
        DmScheduled event = a->schedule.items[i];
        found |= (event.status & MIDI_STATUS_MASK) == type && event.a == key && event.b == value;
    }

    SDL_UnlockAudioStream(a->stream);
    return found;
}

int visualizers_audio_voices(DmAudio *a) {
    Synth *synths[] = {&a->live, &a->keyboard, &a->feedback};
    int count = 0;

    for (int s = 0; s < 3; ++s) {
        fluid_voice_t *voices[512] = {0};
        fluid_synth_get_voicelist(synths[s]->synth, voices, 512, -1);

        for (int i = 0; i < 512 && voices[i]; ++i) {
            count += fluid_voice_is_playing(voices[i]) != 0;
        }
    }

    return count;
}

void visualizers_audio_panic_checks(DmAudio *a, void (*check)(bool, const char *)) {
    Synth *synths[] = {&a->live, &a->keyboard, &a->feedback};

    for (int s = 0; s < 3; ++s) {
        fluid_synth_program_change(synths[s]->synth, 0, 0);
        fluid_synth_cc(synths[s]->synth, 0, 64, 127);
        fluid_synth_cc(synths[s]->synth, 0, 66, 127);
        fluid_synth_cc(synths[s]->synth, 0, 69, 127);
        fluid_synth_noteon(synths[s]->synth, 0, 60, 100);
    }

    recording_feedback_audio_render(a, 512);
    check(visualizers_audio_voices(a) >= 3, "panic fixture sounds in all three synths");
    dm_audio_preview(a, 0, 62, 100, true);
    dm_audio_edit_preview(a, 0, 65, true);
    dm_audio_transport(a, true);
    SDL_LockMutex(a->device_lock);
    dm_audio_queue_put(&a->midi, (Command){0x90, 60, 100});
    dm_audio_panic(a);
    check(!dm_audio_playing(a), "panic stops transport");
    check(SDL_GetAtomicInt(&a->preview.read) == SDL_GetAtomicInt(&a->preview.write) &&
              SDL_GetAtomicInt(&a->feedback_queue.read) ==
                  SDL_GetAtomicInt(&a->feedback_queue.write),
          "panic discards pending preview and feedback note-ons");
    bool seen[16][7] = {{0}};
    const int controls[] = {64, 66, 69, 123, 120, 121};
    Command command;
    int messages = 0;

    while (dm_audio_queue_get(&a->midi, &command, NULL)) {
        int c = command.status & 15;
        check((command.status & 0xf0) != 0x90, "panic discards queued external note-ons");

        for (int i = 0; i < 6; ++i) {
            if (command.status == (0xb0 | c) && command.a == controls[i] && command.b == 0) {
                seen[c][i] = true;
            }
        }

        if (command.status == (0xe0 | c) && command.a == 0 && command.b == 64) {
            seen[c][6] = true;
        }

        ++messages;
    }

    check(messages == 112,
          "panic queues pedal release, note/controller resets and centered pitch on all channels");

    for (int c = 0; c < 16; ++c) {
        for (int i = 0; i < 7; ++i) {
            check(seen[c][i], "every MIDI channel receives the complete panic sequence");
        }
    }

    SDL_UnlockMutex(a->device_lock);
    recording_feedback_audio_render(a, 512);
    check(visualizers_audio_voices(a) == 0, "panic silences all synth voices and pending attacks");

    for (int s = 0; s < 3; ++s) {
        int value = -1;
        fluid_synth_get_cc(synths[s]->synth, 0, 64, &value);
        check(value == 0, "panic releases sustain");
        fluid_synth_get_cc(synths[s]->synth, 0, 66, &value);
        check(value == 0, "panic releases sostenuto");
    }

    dm_audio_preview(a, 0, 60, 100, true);
    recording_feedback_audio_render(a, 512);
    check(visualizers_audio_voices(a) > 0, "new keyboard notes still work after panic");
    dm_audio_panic(a);
}

static void controls_process(DmAudio *a, int frames) {
    SDL_LockAudioStream(a->stream);

    while (frames > 0) {
        float out[BLOCK * 2];
        int n = frames > BLOCK ? BLOCK : frames;
        run_block(a, out, n);
        frames -= n;
    }

    SDL_UnlockAudioStream(a->stream);
}

static bool controls_voice(DmAudio *a, int key, bool sostenuto) {
    fluid_voice_t *voices[512] = {0};
    fluid_synth_get_voicelist(a->live.synth, voices, 512, -1);

    for (int i = 0; i < 512 && voices[i]; ++i) {
        if (fluid_voice_get_channel(voices[i]) == 0 && fluid_voice_get_key(voices[i]) == key &&
            (sostenuto ? fluid_voice_is_sostenuto(voices[i]) : fluid_voice_is_on(voices[i]))) {
            return true;
        }
    }

    return false;
}

static void controls_wait(DmAudio *a, void (*check)(bool, const char *)) {
    uint64_t start = SDL_GetTicks();

    while (dm_audio_busy(a)) {
        char error[512];
        bool success = false;

        if (dm_audio_poll(a, error, sizeof(error), &success, NULL)) {
            check(success, error);
        }

        check(SDL_GetTicks() - start < 30000, "worker timeout");
        SDL_Delay(5);
    }
}

static double controls_energy(DmBytes *wave, double start, double end, int channel) {
    double energy = 0;
    size_t size, offset = wave_data_offset(wave, &size);

    for (size_t f = (size_t)(start * RATE); f < (size_t)(end * RATE); ++f) {
        size_t i = offset + f * 4 + channel * 2;

        if (i + 1 >= offset + size) {
            break;
        }

        double sample = (int16_t)(wave->data[i] | wave->data[i + 1] << 8);
        energy += sample * sample;
    }

    return energy;
}

static int controls_crossings(DmBytes *wave, double start, double end) {
    int crossings = 0, previous = 0;
    size_t size, offset = wave_data_offset(wave, &size);

    for (size_t f = (size_t)(start * RATE); f < (size_t)(end * RATE); ++f) {
        size_t i = offset + f * 4;

        if (i + 1 >= offset + size) {
            break;
        }

        int sample = (int16_t)(wave->data[i] | wave->data[i + 1] << 8);
        crossings += previous < 0 && sample >= 0;
        previous = sample;
    }

    return crossings;
}

void test_event_controls_audio(DmAudio *a, void (*check)(bool, const char *)) {
    char error[512];
    check(dm_audio_soundfont(a, "agents/reports/generated-sine.sf2", error, sizeof(error)),
          "load isolated audio fixture");
    controls_wait(a, check);
    check(SDL_PauseAudioStreamDevice(a->stream), "pause device for deterministic controls test");
    DmProject p;
    dm_project_init(&p);
    snprintf(p.soundfont, sizeof(p.soundfont), "agents/reports/generated-sine.sf2");
    p.bpm = 120;
    int notes = dm_pattern_add(&p, "Sostenuto voices");
    dm_note_add(&p.patterns[notes], (DmNote){0, 1, 60, 100, 0});
    dm_note_add(&p.patterns[notes], (DmNote){1.5, 1, 64, 100, 0});
    dm_clip_add(&p, (DmClip){.pattern = notes, .length = 4});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = .5, .type = 0xb0, .a = 66, .b = 127, .pattern = notes, .curve = 1});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = 3, .type = 0xb0, .a = 66, .b = 127, .pattern = notes, .curve = 1});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = 3, .type = 0xb0, .a = 66, .b = 0, .pattern = notes, .curve = -1});
    dm_audio_monitor(a, true);
    check(dm_audio_publish(a, &p, 1, true, error, sizeof(error)), "seek into sostenuto interval");
    controls_process(a, BLOCK * 4);
    check(controls_voice(a, 60, true), "seek restores captured sostenuto voice");
    check(controls_voice(a, 64, false) && !controls_voice(a, 64, true),
          "seek does not capture notes played after pedal down");
    controls_process(a, RATE / 3);
    check(controls_voice(a, 60, true) && !controls_voice(a, 64, true) &&
              !controls_voice(a, 64, false),
          "later note releases while captured note remains held");
    controls_process(a, RATE / 3);
    check(!controls_voice(a, 60, true), "sostenuto endpoint releases captured voice");
    /* Both pedals cooperate when chasing a seek, regardless of release order. */
    reset_chase_channel(a, 0);
    cache_event(a, &(DmScheduled){.status = 0x90, .a = 60, .b = 100});
    cache_controller(a, 0, 66, 127);
    cache_controller(a, 0, 64, 127);
    chase_note_off(a, 0, 60);
    cache_controller(a, 0, 64, 0);
    check(a->state.notes[0][60] == 100, "sustain release preserves sostenuto capture");
    cache_controller(a, 0, 66, 0);
    check(a->state.notes[0][60] == 0, "last pedal release clears chased voice");

    p.automation_count = 0;
    p.patterns[notes].count = 0;
    dm_note_add(&p.patterns[notes], (DmNote){0, 4, 69, 110, 0});
    int controls = dm_pattern_add(&p, "Volume section");
    p.patterns[controls].kind = 1;
    p.patterns[controls].event = 7;
    p.lane_count[0] = 1;
    dm_clip_add(&p, (DmClip){.pattern = controls, .length = 4, .lane = 1});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = 1, .type = 0xb0, .a = 7, .b = 0, .pattern = controls, .curve = 1});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = 3, .type = 0xb0, .a = 7, .b = 0, .pattern = controls, .curve = 1});
    dm_automation_add(
        &p, (DmAutomation){
                .beat = 3, .type = 0xb0, .a = 7, .b = 100, .pattern = controls, .curve = -1});
    dm_audio_transport(a, false);
    check(dm_audio_export(a, &p, "agents/reports/event-controls-volume.wav", NULL, error,
                          sizeof(error)),
          "render volume section");
    controls_wait(a, check);
    DmBytes wave = {0};
    check(dm_read("agents/reports/event-controls-volume.wav", &wave, error, sizeof(error)),
          "read rendered volume fixture");
    double before = controls_energy(&wave, .2, .4, 0);
    double during = controls_energy(&wave, 1, 1.2, 0);
    double after = controls_energy(&wave, 1.7, 1.9, 0);
    check(before > 100000 && during < before * .1 && after > before * .5,
          "volume section audibly mutes and restores the held note");
    dm_bytes_free(&wave);
    p.patterns[controls].event = 10;

    for (size_t i = 0; i < p.automation_count; ++i) {
        p.automation[i].a = 10;
        p.automation[i].b = p.automation[i].curve < 0 ? 64 : 0;
    }

    check(
        dm_audio_export(a, &p, "agents/reports/event-controls-pan.wav", NULL, error, sizeof(error)),
        "render pan section");
    controls_wait(a, check);
    check(dm_read("agents/reports/event-controls-pan.wav", &wave, error, sizeof(error)),
          "read rendered pan fixture");
    double left = controls_energy(&wave, 1, 1.2, 0), right = controls_energy(&wave, 1, 1.2, 1);
    double restored_left = controls_energy(&wave, 1.7, 1.9, 0);
    double restored_right = controls_energy(&wave, 1.7, 1.9, 1);
    check(left > right * 10 && restored_left > restored_right * .8 &&
              restored_left < restored_right * 1.2,
          "pan section audibly moves left and restores center");
    dm_bytes_free(&wave);
    p.patterns[notes].count = 0;
    dm_note_add(&p.patterns[notes], (DmNote){0, 1, 69, 110, 0});
    dm_note_add(&p.patterns[notes], (DmNote){1, 5, 81, 110, 0});
    p.patterns[controls].event = 5;

    for (size_t i = 0; i < p.clip_count; ++i) {
        p.clips[i].length = 8;
    }

    for (size_t i = 0; i < p.automation_count; ++i) {
        p.automation[i].a = 5;
        p.automation[i].beat = i == 0 ? 0 : 6;
        p.automation[i].b = p.automation[i].curve < 0 ? 0 : 5;
    }

    check(dm_audio_export(a, &p, "agents/reports/event-controls-glide-fast.wav", NULL, error,
                          sizeof(error)),
          "render fast automatic glide");
    controls_wait(a, check);
    check(dm_read("agents/reports/event-controls-glide-fast.wav", &wave, error, sizeof(error)),
          "read fast glide fixture");
    int fast = controls_crossings(&wave, .8, 1);
    dm_bytes_free(&wave);

    for (size_t i = 0; i < p.automation_count; ++i) {
        if (p.automation[i].curve > 0) {
            p.automation[i].b = 100;
        }
    }

    check(dm_audio_export(a, &p, "agents/reports/event-controls-glide-slow.wav", NULL, error,
                          sizeof(error)),
          "render slow automatic glide");
    controls_wait(a, check);
    check(dm_read("agents/reports/event-controls-glide-slow.wav", &wave, error, sizeof(error)),
          "read slow glide fixture");
    int slow = controls_crossings(&wave, .8, 1);
    check(fast > slow * 1.2 && slow > 20, "portamento values audibly change glide duration");
    dm_bytes_free(&wave);
    dm_project_free(&p);
}

size_t audio_probe_engine_size(void) {
    return sizeof(DmAudio);
}
