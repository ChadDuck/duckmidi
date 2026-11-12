/* The engine's lifetime and controls: device, MIDI input routing, transport, mixer, metronome,
   analyzer and panic. */
#include "audio/internal.h"

void dm_audio_attach_midi_input(DmAudio *a, DmMidiInput *input) {
    if (!a) {
        return;
    }

    double latency = dm_audio_buffer_ms(a) / 1000;
    SDL_LockMutex(a->device_lock);

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    dm_audio_input_event_stop(a);
    dm_audio_input_thru_stop(a);
    dm_audio_input_silence(a, NULL);
    atomic_store_explicit(&a->input, input, memory_order_release);

    if (a->input_latency_auto) {
        a->input_latency = latency;
    }

    a->input_discard_ns = 0;
    dm_audio_input_clock(a, true);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    SDL_UnlockMutex(a->device_lock);
}

void dm_audio_input_route(DmAudio *a, int channel, bool enabled) {
    if (a) {
        atomic_store_explicit(&a->input_route, (channel & MIDI_CHANNEL_MASK) | (enabled ? 16 : 0),
                              memory_order_relaxed);
    }
}

void dm_audio_input_controllers(DmAudio *a, DmInputControllerPolicy policy) {
    if (a && (policy == DM_INPUT_CONTROLLERS_KEYBOARD || policy == DM_INPUT_CONTROLLERS_SONG)) {
        atomic_store_explicit(&a->input_policy, policy, memory_order_relaxed);
    }
}

void dm_audio_input_thru(DmAudio *a, DmMidiInput *input, bool enabled) {
    if (dm_midi_input_thru(input) == enabled) {
        return;
    }

    if (a && a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    if (a && !enabled) {
        dm_audio_input_thru_stop(a);
    }

    dm_midi_input_set_thru(input, enabled);

    if (a && a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_input_note_map(DmAudio *a, const uint8_t map[MIDI_KEYS]) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    for (unsigned key = 0; key < MIDI_KEYS; ++key) {
        a->input_map[key] = map ? map[key] & MIDI_DATA_MASK : (uint8_t)key;
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_input_offset(DmAudio *a, double seconds) {
    if (!a || isinf(seconds)) {
        return;
    }

    bool automatic = isnan(seconds);
    double latency = automatic ? dm_audio_buffer_ms(a) / 1000 : seconds;

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->input_latency_auto = automatic;
    a->input_latency = latency;
    dm_audio_input_clock(a, false);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

DmAudio *dm_audio_create(char *error, size_t cap) {
    DmAudio *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }

    atomic_init(&a->input, NULL);
    atomic_init(&a->input_route, 0);
    atomic_init(&a->input_policy, DM_INPUT_CONTROLLERS_KEYBOARD);
    atomic_init(&a->input_played, 0);
    atomic_init(&a->input_dropped, 0);
    a->input_latency_auto = true;

    for (unsigned key = 0; key < MIDI_KEYS; ++key) {
        a->input_map[key] = (uint8_t)key;
    }

    memset(a->mix_volume, 255, sizeof(a->mix_volume));
    memset(a->mix_pan, 255, sizeof(a->mix_pan));

    a->device_index = -1;
    a->sample_bank = -1;
    a->preview_mask = UINT16_MAX;
    a->monitor = true;
    a->click_bpm = 120;

    if (!dm_audio_synth_create(&a->live, NULL) || !dm_audio_synth_create(&a->keyboard, NULL) ||
        !dm_audio_synth_create(&a->feedback, NULL)) {
        snprintf(error, cap, DM_TEXT_CANNOT_INITIALIZE_FLUIDSYNTH);
        dm_audio_destroy(a);
        return NULL;
    }

    a->feedback_key = -1;

    for (int c = 0; c < 16; ++c) {
        a->keyboard_channels[c].program = 0;
        a->keyboard_channels[c].volume = 100;
        a->keyboard_channels[c].pan = 64;
        a->keyboard_channels[c].drums = c == MIDI_DRUM_CHANNEL;
        a->keyboard_override[c] = -1;
        a->keyboard_parts[c] = c;
        a->keyboard_applied[c].program = -1;
        a->keyboard_applied[c].volume = -1;
        a->keyboard_applied[c].pan = -1;
    }

    a->device_lock = SDL_CreateMutex();
    a->device = rtmidi_out_create_default();

    if (a->device && !a->device->ok) {
        rtmidi_out_free(a->device);
        a->device = NULL;
    }

    if (!a->device_lock) {
        snprintf(error, cap, DM_TEXT_CANNOT_INITIALIZE_MIDI);
        dm_audio_destroy(a);
        return NULL;
    }

    if (a->device) {
        a->midi_thread = SDL_CreateThread(dm_audio_midi_worker, DM_TEXT_MIDI_OUTPUT, a);
    }

    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    a->stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, dm_audio_callback, a);

    if (!a->stream) {
        snprintf(error, cap, DM_TEXT_AUDIO_UNAVAILABLE_USE, SDL_GetError());
        return a;
    }

    SDL_ResumeAudioStreamDevice(a->stream);
    return a;
}

void dm_audio_destroy(DmAudio *a) {
    if (!a) {
        return;
    }

    SDL_SetAtomicInt(&a->cancel, 1);

    if (a->worker) {
        SDL_WaitThread(a->worker, NULL);
    }

    if (a->stream) {
        SDL_DestroyAudioStream(a->stream);
    }

    SDL_SetAtomicInt(&a->quit, 1);

    if (a->midi_thread) {
        SDL_WaitThread(a->midi_thread, NULL);
    }

    if (a->device) {
        if (a->device_index >= 0) {
            for (int c = 0; c < 16; ++c) {
                uint8_t b[] = {MIDI_CONTROL | c, MIDI_CC_SOUND_OFF, 0};
                rtmidi_out_send_message(a->device, b, 3);
            }
        }

        rtmidi_out_free(a->device);
    }

    if (a->device_lock) {
        SDL_DestroyMutex(a->device_lock);
    }

    dm_audio_synth_free(&a->live);
    dm_audio_synth_free(&a->keyboard);
    dm_audio_synth_free(&a->feedback);
    dm_audio_synth_free(&a->prepared);
    dm_audio_synth_free(&a->prepared_keyboard);
    dm_audio_synth_free(&a->prepared_feedback);
    dm_audio_synth_free(&a->sample_audition.voice);
    dm_sample_synth_free(a->sample_audition.bank);
    dm_audio_free_port_synths(a->port_live);
    dm_audio_free_port_synths(a->prepared_ports);

    for (unsigned i = 0; i < LIVE_SAMPLE_BANKS; ++i) {
        dm_live_sample_bank_free(a->sample_banks[i]);
    }

    dm_schedule_free(&a->schedule);
    dm_schedule_free(&a->render_schedule);
    dm_bytes_free(&a->render_project);
    dm_audio_free_retired(a);
    free(a->releases);
    free(a);
}

void dm_audio_transport(DmAudio *a, bool playing) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->playing = playing;
    dm_audio_input_clock(a, true);
    a->input_reset = true;

    if (!playing) {
        a->reset = true;
    }

    SDL_SetAtomicInt(&a->playing_flag, playing);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

double dm_audio_position(DmAudio *a) {
    return a ? SDL_GetAtomicInt(&a->position_ms) / 1000.0 : 0;
}

void dm_audio_panic(DmAudio *a) {
    if (!a) {
        return;
    }

    dm_audio_sample_audition_stop(a);

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->playing = a->recording = a->reset = a->chase = false;
    dm_audio_input_clock(a, true);
    dm_audio_input_event_stop(a);
    dm_audio_input_thru_stop(a);
    dm_audio_input_silence(a, NULL);
    SDL_SetAtomicInt(&a->playing_flag, 0);
    a->release_count = 0;
    a->release_end = 0;

    if (a->sample_audition.keyboard) {
        fluid_synth_all_sounds_off(a->sample_audition.voice.synth, 1);
        memset(a->sample_audition.song_held, 0, sizeof(a->sample_audition.song_held));
    }

    a->keyboard_tail = a->feedback_tail = a->audition_frames = a->click_frames = 0;
    dm_audio_reset_live_ports(a);
    a->feedback_key = -1;
    a->feedback_sample_id = 0;
    memset(a->note_sample_id, 0, sizeof(a->note_sample_id));
    memset(a->keyboard_sample_id, 0, sizeof(a->keyboard_sample_id));
    memset(a->note_token, 0, sizeof(a->note_token));
    memset(a->state.notes, 0, sizeof(a->state.notes));
    memset(a->state.held, 0, sizeof(a->state.held));
    memset(a->state.sostenuto, 0, sizeof(a->state.sostenuto));
    SDL_SetAtomicInt(&a->preview.read, SDL_GetAtomicInt(&a->preview.write));
    SDL_SetAtomicInt(&a->preview.panic, 0);
    SDL_SetAtomicInt(&a->feedback_queue.read, SDL_GetAtomicInt(&a->feedback_queue.write));
    SDL_SetAtomicInt(&a->feedback_queue.panic, 0);
    // The worker sends these after discarding pending note-ons, without device I/O on the GUI
    // thread.
    SDL_LockMutex(a->device_lock);
    SDL_SetAtomicInt(&a->midi.read, SDL_GetAtomicInt(&a->midi.write));
    SDL_SetAtomicInt(&a->midi.panic, 0);
    const int controls[] = {64, 66, 69, 123, 120, 121};

    for (int c = 0; c < 16; ++c) {
        for (size_t i = 0; i < sizeof(controls) / sizeof(*controls); ++i) {
            int cc = controls[i];
            fluid_synth_cc(a->live.synth, c, cc, 0);
            fluid_synth_cc(a->keyboard.synth, c, cc, 0);
            fluid_synth_cc(a->feedback.synth, c, cc, 0);
            dm_audio_queue_put(&a->midi, (Command){MIDI_CONTROL | c, cc, 0});
        }

        fluid_synth_pitch_bend(a->live.synth, c, MIDI_BEND_CENTER);
        fluid_synth_pitch_bend(a->keyboard.synth, c, MIDI_BEND_CENTER);
        fluid_synth_pitch_bend(a->feedback.synth, c, MIDI_BEND_CENTER);
        dm_audio_queue_put(&a->midi, (Command){MIDI_PITCH_BEND | c, 0, 64});
        a->keyboard_applied[c].volume = a->keyboard_applied[c].pan = -1;
        SDL_SetAtomicInt(&a->peaks[c], 0);
    }

    a->keyboard_config = true;
    dm_audio_keyboard_configure(a);
    SDL_UnlockMutex(a->device_lock);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

bool dm_audio_playing(DmAudio *a) {
    return a && SDL_GetAtomicInt(&a->playing_flag) != 0;
}

void dm_audio_edit_preview(DmAudio *a, int program, float pitch, bool on) {
    dm_audio_edit_preview_part(a, -1, program, pitch, on);
}

void dm_audio_edit_preview_part(DmAudio *a, int part, int program, float pitch, bool on) {
    if (!a) {
        return;
    }

    if (!on) {
        dm_audio_queue_put(&a->feedback_queue, (Command){255, 0, 0});
    } else if (program >= 0 && program <= 128 && isfinite(pitch) && pitch >= 0 && pitch <= 127) {
        int key = (int)floorf(pitch);
        int fraction = (int)roundf((pitch - key) * 128);
        dm_audio_queue_put(&a->feedback_queue,
                           (Command){.status = program,
                                     .a = key,
                                     .b = fraction,
                                     .feedback_part = part >= 0 && part < DM_PARTS ? part + 1 : 0});
    }
}

void dm_audio_preview(DmAudio *a, int channel, int key, int velocity, bool on) {
    if (a && key >= 0 && key <= 127) {
        dm_audio_queue_put(&a->preview, (Command){(on ? MIDI_NOTE_ON : MIDI_NOTE_OFF) |
                                                      (channel & MIDI_CHANNEL_MASK),
                                                  key, velocity});
    }
}

void dm_audio_audition(DmAudio *a, int channel, int key, int velocity, double duration) {
    if (!a || key < 0 || key > 127 || !isfinite(duration) || duration <= 0 || duration > 10) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    if (a->audition_frames > 0) {
        dm_audio_preview(a, a->audition_channel, a->audition_key, 0, false);
    }

    dm_audio_preview(a, channel, key, velocity, true);
    a->audition_frames = (int)(duration * RATE);
    a->audition_channel = channel;
    a->audition_key = key;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_program(DmAudio *a, int channel, int program) {
    if (a) {
        dm_audio_queue_put(&a->preview, (Command){MIDI_PROGRAM | (channel & MIDI_CHANNEL_MASK),
                                                  program & MIDI_DATA_MASK, 0});
    }
}

void dm_audio_keyboard_program(DmAudio *a, int channel, int program, bool drums) {
    if (a && channel >= 0 && channel < 16 && program >= -1 && program <= 127) {
        /* Keyboard override commands use the port byte to carry the drum-bank choice. */
        dm_audio_queue_put(&a->preview, (Command){.status = 0xf1,
                                                  .a = channel,
                                                  .b = program < 0 ? 255 : program,
                                                  .port = drums});
    }
}

void dm_audio_input_event(DmAudio *a, int route, int part, DmMidiInputEvent event) {
    if (a && route >= 0 && route < MIDI_CHANNELS && part >= 0 && part < DM_PARTS &&
        event.device < DM_MIDI_INPUTS) {
        dm_audio_queue_put(&a->preview,
                           (Command){.status = (event.status & MIDI_STATUS_MASK) | route,
                                     .a = event.a,
                                     .b = event.b,
                                     .input_part = part + 1,
                                     .input_device = event.device});
    }
}

void dm_audio_control(DmAudio *a, int channel, int controller, int value) {
    if (a) {
        dm_audio_queue_put(&a->preview,
                           (Command){MIDI_CONTROL | (channel & MIDI_CHANNEL_MASK),
                                     controller & MIDI_DATA_MASK, value & MIDI_DATA_MASK});
    }
}

void dm_audio_keyboard_mix(DmAudio *a, int route, int part, int volume, int pan) {
    if (!a || route < 0 || route > 15 || part < 0 || part >= DM_PARTS) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->keyboard_parts[route] = part;
    a->keyboard_channels[route].volume = volume;
    a->keyboard_channels[route].pan = pan;
    a->preview_mask |= (uint16_t)(1u << route);
    a->keyboard_config = true;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_part_control(DmAudio *a, int port, int channel, int controller, int value) {
    if (a && port >= 0 && port < DM_NOTE_PORTS && channel >= 0 && channel < 16) {
        dm_audio_queue_put(&a->preview, (Command){.status = (uint8_t)(MIDI_CONTROL | channel),
                                                  .a = (uint8_t)controller,
                                                  .b = (uint8_t)value,
                                                  .port = (uint8_t)port,
                                                  .live_only = true});
    }
}

bool dm_audio_mix_state(DmAudio *a, uint8_t volume[DM_PARTS], uint8_t pan[DM_PARTS]) {
    if (!a || !SDL_GetAtomicInt(&a->playing_flag)) {
        return false;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    for (int part = 0; part < DM_PARTS; ++part) {
        volume[part] = a->mix_volume[part];
        pan[part] = a->mix_pan[part];
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    return true;
}

float dm_audio_peak(DmAudio *a, int c) {
    return a && c >= 0 && c < DM_PARTS ? SDL_SetAtomicInt(&a->peaks[c], 0) / 10000.0f : 0;
}

void dm_audio_analyzer_enable(DmAudio *a, bool enabled) {
    if (!a || SDL_GetAtomicInt(&a->analyzer_enabled) == enabled) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->analyzer_cursor = a->analyzer_count = 0;
    SDL_SetAtomicInt(&a->analyzer_enabled, enabled);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

bool dm_audio_analyzer_read(DmAudio *a, float *stereo) {
    if (!a) {
        return false;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    bool ready = SDL_GetAtomicInt(&a->analyzer_enabled) && a->analyzer_count == DM_SPECTRUM_FRAMES;

    if (ready) {
        int tail = DM_SPECTRUM_FRAMES - a->analyzer_cursor;
        memcpy(stereo, a->analyzer_samples + a->analyzer_cursor * 2,
               (size_t)tail * 2 * sizeof(float));
        memcpy(stereo + tail * 2, a->analyzer_samples,
               (size_t)a->analyzer_cursor * 2 * sizeof(float));
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    return ready;
}

const char *dm_audio_driver(DmAudio *a) {
    return a && a->stream ? SDL_GetCurrentAudioDriver() : DM_TEXT_UNAVAILABLE;
}

int dm_audio_buffer_frames(DmAudio *a) {
    int frames = 0;

    if (a && a->stream) {
        SDL_AudioSpec spec;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->stream), &spec, &frames);
    }

    return frames;
}

double dm_audio_buffer_ms(DmAudio *a) {
    int frames = 0;
    SDL_AudioSpec spec = {0};

    if (a && a->stream) {
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->stream), &spec, &frames);
    }

    return spec.freq > 0 ? frames * 1000.0 / spec.freq : 0;
}

bool dm_audio_retry(DmAudio *a, char *error, size_t cap) {
    if (!a) {
        return false;
    }

    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        snprintf(error, cap, DM_TEXT_AUDIO_INITIALIZATION_FAILED, SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    SDL_AudioStream *next =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, dm_audio_callback, a);

    if (!next) {
        snprintf(error, cap, DM_TEXT_AUDIO_RETRY_FAILED, SDL_GetError());
        return false;
    }

    if (a->stream) {
        SDL_DestroyAudioStream(a->stream);
    }

    a->stream = next;

    if (a->input_latency_auto) {
        dm_audio_input_offset(a, NAN);
    }

    SDL_ResumeAudioStreamDevice(next);
    return true;
}

void dm_audio_cancel(DmAudio *a) {
    if (a) {
        SDL_SetAtomicInt(&a->cancel, 1);
    }
}

void dm_audio_metronome(DmAudio *a, bool enabled, double bpm) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->metronome = enabled;
    a->click_bpm = bpm;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_monitor(DmAudio *a, bool enabled) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->monitor = enabled;
    a->reset = true;
    a->keyboard_panic = true;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_recording(DmAudio *a, bool enabled) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->recording = enabled;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

int dm_audio_device_count(DmAudio *a) {
    if (!a || !a->device) {
        return 0;
    }

    SDL_LockMutex(a->device_lock);
    int n = (int)rtmidi_get_port_count(a->device);
    SDL_UnlockMutex(a->device_lock);
    return n;
}

void dm_audio_device_name(DmAudio *a, int i, char *name, size_t cap) {
    if (!a) {
        return;
    }

    SDL_LockMutex(a->device_lock);
    int n = (int)cap;
    rtmidi_get_port_name(a->device, (unsigned)i, name, &n);
    SDL_UnlockMutex(a->device_lock);
    name[cap - 1] = 0;
}

bool dm_audio_device(DmAudio *a, int i, char *error, size_t cap) {
    if (!a || !a->device) {
        return false;
    }

    SDL_LockMutex(a->device_lock);

    if (a->device_index >= 0) {
        for (int c = 0; c < 16; ++c) {
            uint8_t message[] = {(uint8_t)(MIDI_CONTROL | c), MIDI_CC_SOUND_OFF, 0};
            rtmidi_out_send_message(a->device, message, 3);
        }
    }

    rtmidi_close_port(a->device);
    a->device_index = -1;

    if (i >= 0) {
        rtmidi_open_port(a->device, (unsigned)i, DM_TEXT_DUCKMIDI_OUTPUT);
    }

    bool ok = i < 0 || a->device->ok;

    if (ok) {
        a->device_index = i;
    } else {
        snprintf(error, cap, DM_TEXT_MIDI_DEVICE, a->device->msg);
    }

    SDL_UnlockMutex(a->device_lock);
    return ok;
}
