/* Live input on the audio thread: the keyboard piano, edit previews, external MIDI input and
   recording feedback. */
#include "audio/internal.h"

static void keyboard_command(DmAudio *a, Command cmd, int song_part, bool audition_notes) {
    SampleAudition *preview = &a->sample_audition;
    int message_type = cmd.status & MIDI_STATUS_MASK;
    bool controller = message_type != MIDI_NOTE_ON && message_type != MIDI_NOTE_OFF;

    if (preview->keyboard && preview->keyboard_route == (cmd.status & MIDI_CHANNEL_MASK) &&
        (audition_notes || controller)) {
        int type = cmd.status & MIDI_STATUS_MASK;

        if (type == MIDI_NOTE_ON && cmd.b) {
            if (preview->bank) {
                if (preview->keys[cmd.a]) {
                    dm_audio_sample_stop(&preview->voice, preview->keys[cmd.a], 0, cmd.a);
                }

                DmSampleStart start =
                    dm_sample_synth_start(preview->bank, preview->voice.synth, preview->preset,
                                          cmd.a + 2, 0, cmd.a, cmd.b);
                preview->keys[cmd.a] = start == DM_SAMPLE_ONE_SHOT ? SAMPLE_ONE_SHOT : cmd.a + 2;

                if (start == DM_SAMPLE_RELEASE) {
                    dm_sample_pedal_on(&preview->voice.sample_pedals, cmd.a + 2, 0);
                }
            } else {
                dm_audio_send_synth(preview->voice.synth, MIDI_NOTE_ON, cmd.a, cmd.b);
            }
        } else if (type == MIDI_NOTE_OFF || (type == MIDI_NOTE_ON && !cmd.b)) {
            if (!preview->bank || preview->keys[cmd.a]) {
                dm_audio_sample_stop(&preview->voice, preview->keys[cmd.a], 0, cmd.a);
            }
            preview->keys[cmd.a] = 0;
        } else if (type != MIDI_PROGRAM) {
            dm_audio_send_route(&preview->voice, (uint8_t)type, cmd.a, cmd.b);
        }

        if (audition_notes) {
            return;
        }
    }

    if (a->monitor) {
        a->keyboard_tail = RATE * 2;
        int type = cmd.status & MIDI_STATUS_MASK, channel = cmd.status & MIDI_CHANNEL_MASK;
        DmChannel *applied = &a->keyboard_applied[channel];
        DmSamplePatchState *patch = &a->keyboard_patch[channel];
        dm_sample_patch_state_event(patch, cmd.status, cmd.a, cmd.b);

        if (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF) {
            uint32_t *id = &a->keyboard_sample_id[channel][cmd.a];

            if (type == MIDI_NOTE_ON && cmd.b) {
                int part = a->keyboard_parts[channel];
                DmPatchKey route = {patch->selected, part / 16, part & MIDI_CHANNEL_MASK,
                                    patch->program, patch->drums};
                uint32_t preset =
                    a->sample_bank < 0
                        ? 0
                        : dm_live_sample_bank_patch(a->sample_banks[a->sample_bank], route);

                if (*id) {
                    dm_audio_sample_stop(&a->keyboard, *id, channel, cmd.a);
                }

                *id = dm_audio_sample_start(a, &a->keyboard, preset, channel, cmd.a, cmd.b);
            } else {
                dm_audio_sample_stop(&a->keyboard, *id, channel, cmd.a);
                *id = 0;
            }
        } else {
            dm_audio_send_route(&a->keyboard, cmd.status, cmd.a, cmd.b);
        }

        if (type == MIDI_PROGRAM) {
            applied->program = cmd.a;
        } else if (audition_notes && type == MIDI_CONTROL && cmd.a == MIDI_CC_VOLUME) {
            applied->volume = cmd.b;
        } else if (audition_notes && type == MIDI_CONTROL && cmd.a == MIDI_CC_PAN) {
            applied->pan = cmd.b;
        }

        if (song_part >= 0 && type != MIDI_NOTE_OFF && type != MIDI_NOTE_ON) {
            uint8_t status = (cmd.status & MIDI_STATUS_MASK) | (song_part & MIDI_CHANNEL_MASK);
            dm_audio_mix_track(a, song_part, status, cmd.a, cmd.b);
            dm_audio_send_live(a, song_part, status, cmd.a, cmd.b);
        }
    }
}

static void preview_command(DmAudio *a, Command cmd) {
    /* Preserve the computer piano's existing controller destinations. */
    int part =
        (cmd.status & MIDI_STATUS_MASK) == MIDI_KEY_PRESSURE ? -1 : cmd.status & MIDI_CHANNEL_MASK;
    keyboard_command(a, cmd, part, true);
}

static int input_controller_targets(DmAudio *a, unsigned route) {
    return atomic_load_explicit(&a->input_policy, memory_order_relaxed) == DM_INPUT_CONTROLLERS_SONG
               ? a->keyboard_parts[route]
               : -1;
}

static bool input_thru_command(DmAudio *a, Command cmd) {
    unsigned channel = cmd.status & MIDI_CHANNEL_MASK;
    unsigned type = cmd.status & MIDI_STATUS_MASK;

    if (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF) {
        bool *held = &a->input_thru_held[channel][cmd.a];

        if (type == MIDI_NOTE_OFF && !*held) {
            return true;
        }

        *held = type == MIDI_NOTE_ON;
    } else if (type == MIDI_CONTROL) {
        uint16_t bit = (uint16_t)(1u << channel);

        if (cmd.a == MIDI_CC_SUSTAIN || cmd.a == MIDI_CC_RESET) {
            a->input_thru_sustain &= ~bit;
            a->input_thru_sustain |= cmd.a == MIDI_CC_SUSTAIN && cmd.b >= 64 ? bit : 0;
        }

        if (cmd.a == MIDI_CC_SOSTENUTO || cmd.a == MIDI_CC_RESET) {
            a->input_thru_sostenuto &= ~bit;
            a->input_thru_sostenuto |= cmd.a == MIDI_CC_SOSTENUTO && cmd.b >= 64 ? bit : 0;
        }
    }

    return dm_audio_queue_put(&a->midi, cmd);
}

/* Translated controls can target a different project channel from their keyboard preview.
   Remember the destinations until the device's reset/bend release pair has cleared both. */
static void input_event_release(DmAudio *a, unsigned device, Command reset, bool thru) {
    for (unsigned word = 0; word < DM_PARTS / 32; ++word) {
        uint32_t bits = a->input_event_parts[device][word];

        for (unsigned bit = 0; bits && bit < 32; ++bit) {
            if (!(bits & (1u << bit))) {
                continue;
            }

            bits &= ~(1u << bit);
            int part = word * 32 + bit;
            uint8_t status = (reset.status & MIDI_STATUS_MASK) | (part & MIDI_CHANNEL_MASK);
            dm_audio_mix_track(a, part, status, reset.a, reset.b);
            dm_audio_send_live(a, part, status, reset.a, reset.b);
        }
    }

    if (thru) {
        for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
            if (a->input_event_thru[device] & (1u << c)) {
                reset.status = (reset.status & MIDI_STATUS_MASK) | c;
                input_thru_command(a, reset);
            }
        }
    }

    if ((reset.status & MIDI_STATUS_MASK) == MIDI_PITCH_BEND) {
        memset(a->input_event_parts[device], 0, sizeof(a->input_event_parts[device]));
        a->input_event_thru[device] = 0;
    }
}

/* End external MIDI input voices when thru is disabled or the input is detached. */
void dm_audio_input_thru_stop(DmAudio *a) {
    for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
        if (a->input_thru_sustain & (1u << c)) {
            dm_audio_queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_SUSTAIN, 0});
        }

        if (a->input_thru_sostenuto & (1u << c)) {
            dm_audio_queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_SOSTENUTO, 0});
        }

        for (unsigned key = 0; key < MIDI_KEYS; ++key) {
            if (a->input_thru_held[c][key] && !a->piano_held[c][key]) {
                dm_audio_queue_put(&a->midi, (Command){MIDI_NOTE_OFF | c, key, 64});
            }
        }
    }

    memset(a->input_thru_held, 0, sizeof(a->input_thru_held));
    a->input_thru_sustain = a->input_thru_sostenuto = 0;
}

static void input_note_off(DmAudio *a, unsigned route, unsigned key, bool thru) {
    if (a->piano_held[route][key]) {
        a->input_thru_held[route][key] = false;
        return;
    }

    Command cmd = {MIDI_NOTE_OFF | route, key, 64};
    keyboard_command(a, cmd, -1, false);

    if (thru) {
        input_thru_command(a, cmd);
    }
}

/* Ends one held input key; the voice stops when no other input or piano key holds it. */
static void input_release(DmAudio *a, InputHeld *held, bool thru) {
    unsigned route = held->route - 1, key = held->key;
    held->route = 0;

    if (!--a->input_notes[route][key]) {
        input_note_off(a, route, key, thru);
    }
}

void dm_audio_input_event_stop(DmAudio *a) {
    for (unsigned d = 0; d < DM_MIDI_INPUTS; ++d) {
        input_event_release(a, d, (Command){MIDI_CONTROL, MIDI_CC_RESET, 0}, true);
        input_event_release(a, d, (Command){MIDI_PITCH_BEND, 0, MIDI_BEND_CENTER >> 7}, true);
    }
}

void dm_audio_input_silence(DmAudio *a, DmMidiInput *input) {
    bool thru = input && dm_midi_input_thru(input);

    for (unsigned route = 0; route < MIDI_CHANNELS; ++route) {
        for (unsigned key = 0; key < MIDI_KEYS; ++key) {
            if (a->input_notes[route][key]) {
                input_note_off(a, route, key, thru);
            }
        }
    }

    memset(a->input_held, 0, sizeof(a->input_held));
    memset(a->input_notes, 0, sizeof(a->input_notes));
}

void dm_audio_input_commands(DmAudio *a) {
    DmMidiInput *input = atomic_load_explicit(&a->input, memory_order_acquire);

    if (!input || !dm_midi_input_begin(input, 0)) {
        return;
    }

    dm_audio_input_clock(a, false);
    unsigned routing = atomic_load_explicit(&a->input_route, memory_order_relaxed);
    unsigned route = routing & MIDI_CHANNEL_MASK;
    DmMidiInputEvent event;
    bool thru = dm_midi_input_thru(input);

    if (dm_midi_input_overflow(input, 0)) {
        dm_audio_input_silence(a, input);
        dm_audio_keyboard_silence(a);
        a->input_discard_ns = SDL_GetTicksNS();
    }

    if (!(routing & 16)) {
        dm_audio_input_silence(a, input);
    }

    for (unsigned n = 0; n < DM_MIDI_MONITOR_DRAIN && dm_midi_input_pop(input, 0, &event); ++n) {
        if (event.ticks_ns <= a->input_discard_ns) {
            continue;
        }

        int type = event.status & MIDI_STATUS_MASK;
        InputHeld *held = &a->input_held[event.device][event.status & MIDI_CHANNEL_MASK][event.a];
        unsigned target = route, key = event.a;

        if (event.flags & DM_MIDI_EVENT_DEVICE_RELEASE) {
            for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
                for (unsigned k = 0; k < MIDI_KEYS; ++k) {
                    if (a->input_held[event.device][c][k].route) {
                        input_release(a, &a->input_held[event.device][c][k], thru);
                    }
                }
            }

            continue;
        }

        if ((event.flags & DM_MIDI_EVENT_RELEASE) &&
            ((type == MIDI_CONTROL && event.a == MIDI_CC_RESET) || type == MIDI_PITCH_BEND)) {
            for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
                if (a->input_routes[event.device] & (1u << c)) {
                    Command reset = {type | c, event.a, event.b};
                    keyboard_command(a, reset, input_controller_targets(a, c), false);

                    if (thru) {
                        input_thru_command(a, reset);
                    }
                }
            }

            input_event_release(a, event.device, (Command){type, event.a, event.b}, thru);

            if (type == MIDI_PITCH_BEND) {
                a->input_routes[event.device] = 0;
            }

            continue;
        }

        if (type == MIDI_NOTE_OFF) {
            if (!held->route) {
                continue;
            }

            target = held->route - 1;
            key = held->key;
            held->route = 0;

            if (--a->input_notes[target][key]) {
                continue;
            }

            if (a->piano_held[target][key]) {
                a->input_thru_held[target][key] = false;
                continue;
            }
        } else {
            if (!(routing & 16)) {
                continue;
            }

            if (type == MIDI_KEY_PRESSURE) {
                key = held->route ? held->key : a->input_map[event.a];
            }

            if (type == MIDI_NOTE_ON) {
                if (!(a->preview_mask & (1u << route)) && a->keyboard_override[route] < 0) {
                    continue;
                }

                if (held->route) {
                    input_release(a, held, thru);
                }

                key = a->input_map[event.a];
                *held = (InputHeld){(uint8_t)(route + 1), (uint8_t)key};
                ++a->input_notes[route][key];

                if (a->input_notes[route][key] > 1 || a->piano_held[route][key]) {
                    if (thru && a->piano_held[route][key]) {
                        a->input_thru_held[route][key] = true;
                    }

                    continue;
                }
            }
        }

        Command cmd = {(event.status & MIDI_STATUS_MASK) | target, key, event.b};
        a->input_routes[event.device] |= (uint16_t)(1u << target);
        keyboard_command(a, cmd, input_controller_targets(a, target), false);

        if (thru && !input_thru_command(a, cmd)) {
            atomic_fetch_add_explicit(&a->input_dropped, 1, memory_order_relaxed);
        }

        atomic_fetch_add_explicit(&a->input_played, 1, memory_order_relaxed);
    }

    dm_midi_input_end(input, 0);
}

void dm_audio_preview_commands(DmAudio *a) {
    Command cmd;

    while (dm_audio_queue_get(&a->preview, &cmd, NULL)) {
        if (cmd.input_part) {
            int part = cmd.input_part - 1;
            int route = cmd.status & MIDI_CHANNEL_MASK;
            int target = atomic_load_explicit(&a->input_policy, memory_order_relaxed) ==
                                 DM_INPUT_CONTROLLERS_SONG
                             ? part
                             : -1;
            a->input_routes[cmd.input_device] |= (uint16_t)(1u << route);
            keyboard_command(a, cmd, target, false);

            if (target >= 0) {
                a->input_event_parts[cmd.input_device][part / 32] |= 1u << (part % 32);
            }

            DmMidiInput *input = atomic_load_explicit(&a->input, memory_order_acquire);

            if (input && dm_midi_input_thru(input)) {
                a->input_event_thru[cmd.input_device] |=
                    (uint16_t)(1u << (part & MIDI_CHANNEL_MASK));
                cmd.status = (cmd.status & MIDI_STATUS_MASK) | (part & MIDI_CHANNEL_MASK);
                input_thru_command(a, cmd);
            }

            continue;
        }

        if (cmd.live_only) {
            int part = cmd.port * 16 + (cmd.status & MIDI_CHANNEL_MASK);

            /* Mixer volume and pan hold until the song changes them, and the mixer shows them. */
            dm_audio_mix_track(a, part, cmd.status, cmd.a, cmd.b);

            dm_audio_send_live(a, part, cmd.status, cmd.a, cmd.b);
            dm_audio_queue_put(&a->midi, cmd);
            continue;
        }

        if (cmd.status == 0xf1) {
            int channel = cmd.a;
            a->keyboard_override[channel] = cmd.b == 255 ? -1 : cmd.b;
            a->keyboard_override_drums[channel] = cmd.port != 0;
            a->keyboard_config = true;
            dm_audio_keyboard_configure(a);
            continue;
        }

        if ((cmd.status & MIDI_STATUS_MASK) == MIDI_NOTE_ON && cmd.b &&
            !(a->preview_mask & (1u << (cmd.status & MIDI_CHANNEL_MASK))) &&
            a->keyboard_override[cmd.status & MIDI_CHANNEL_MASK] < 0) {
            continue;
        }

        int type = cmd.status & MIDI_STATUS_MASK, channel = cmd.status & MIDI_CHANNEL_MASK;
        bool main_keyboard =
            !a->sample_audition.keyboard || a->sample_audition.keyboard_route != channel;

        if (main_keyboard && (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF)) {
            a->piano_held[channel][cmd.a] = type == MIDI_NOTE_ON && cmd.b;

            if (a->input_notes[channel][cmd.a]) {
                if (!a->input_thru_held[channel][cmd.a]) {
                    dm_audio_queue_put(&a->midi, cmd);
                }

                continue;
            }
        }

        preview_command(a, cmd);
        dm_audio_queue_put(&a->midi, cmd);
    }
}

void dm_audio_keyboard_configure(DmAudio *a) {
    if (!a->keyboard_config) {
        return;
    }

    for (int c = 0; c < 16; ++c) {
        DmChannel next = a->keyboard_channels[c], *old = &a->keyboard_applied[c];
        bool override = a->keyboard_override[c] >= 0;
        int part = a->keyboard_parts[c];
        int program = override ? a->keyboard_override[c] : a->keyboard_programs[part];
        next.drums = override ? a->keyboard_override_drums[c] : next.drums;

        if (!override && !(a->preview_mask & (1u << c))) {
            next.volume = 0;
        }

        bool type_changed = old->program < 0 || old->drums != next.drums;

        if (type_changed) {
            fluid_synth_set_channel_type(a->keyboard.synth, c,
                                         next.drums ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
            fluid_synth_bank_select(a->keyboard.synth, c, next.drums ? 128 : 0);
            a->keyboard_patch[c] = (DmSamplePatchState){.drums = next.drums};
        }

        if (type_changed || old->program != program) {
            dm_audio_send_synth(a->keyboard.synth, MIDI_PROGRAM | c, program, 0);
            dm_sample_patch_state_event(&a->keyboard_patch[c], MIDI_PROGRAM, program, 0);
        }

        if (old->volume != next.volume) {
            dm_audio_send_synth(a->keyboard.synth, MIDI_CONTROL | c, MIDI_CC_VOLUME, next.volume);
        }

        if (old->pan != next.pan) {
            dm_audio_send_synth(a->keyboard.synth, MIDI_CONTROL | c, MIDI_CC_PAN, next.pan);
        }

        *old = next;
        old->program = program;
    }

    a->keyboard_config = false;
}

void dm_audio_keyboard_reset_controllers(DmAudio *a) {
    for (int c = 0; c < 16; ++c) {
        dm_audio_send_route(&a->keyboard, MIDI_CONTROL | c, MIDI_CC_RESET, 0);
        dm_audio_send_route(&a->keyboard, MIDI_PITCH_BEND | c, 0, MIDI_BEND_CENTER >> 7);
    }
}

void dm_audio_keyboard_silence(DmAudio *a) {
    dm_audio_keyboard_reset_controllers(a);

    for (int c = 0; c < 16; ++c) {
        fluid_synth_all_sounds_off(a->keyboard.synth, c);
    }

    a->keyboard_tail = 0;
    dm_audio_input_thru_stop(a);
    memset(&a->keyboard.sample_pedals, 0, sizeof(a->keyboard.sample_pedals));
    memset(a->keyboard_sample_id, 0, sizeof(a->keyboard_sample_id));
    memset(a->piano_held, 0, sizeof(a->piano_held));
    memset(a->input_held, 0, sizeof(a->input_held));
    memset(a->input_notes, 0, sizeof(a->input_notes));
}

void dm_audio_keyboard_render(DmAudio *a, float *out, int frames) {
    if (fluid_synth_get_active_voice_count(a->keyboard.synth)) {
        a->keyboard_tail = RATE * 2;
    }

    if (a->keyboard_tail <= 0) {
        return;
    }

    float keys[BLOCK * 2];
    SDL_AtomicInt peaks[16] = {0};
    dm_audio_render(&a->keyboard, keys, frames, peaks);

    for (int c = 0; c < 16; c++) {
        int peak = SDL_GetAtomicInt(&peaks[c]), part = a->keyboard_parts[c];

        if (peak > SDL_GetAtomicInt(&a->peaks[part])) {
            SDL_SetAtomicInt(&a->peaks[part], peak);
        }
    }

    for (int i = 0; i < frames * 2; ++i) {
        out[i] = fmaxf(-1, fminf(1, out[i] + keys[i]));
    }

    a->keyboard_tail -= frames;
}

void dm_audio_feedback_stop(DmAudio *a) {
    if (a->feedback_key >= 0) {
        dm_audio_sample_stop(&a->feedback, a->feedback_sample_id,
                             a->feedback_program == 128 ? MIDI_DRUM_CHANNEL : 0, a->feedback_key);
        a->feedback_key = -1;
        a->feedback_sample_id = 0;
    }
}

void dm_audio_feedback_commands(DmAudio *a) {
    Command cmd;

    if (SDL_SetAtomicInt(&a->feedback_queue.panic, 0)) {
        dm_audio_feedback_stop(a);
    }

    while (dm_audio_queue_get(&a->feedback_queue, &cmd, NULL)) {
        if (cmd.status == 255 || !a->monitor) {
            dm_audio_feedback_stop(a);
            continue;
        }

        int channel = cmd.status == 128 ? MIDI_DRUM_CHANNEL : 0;
        bool attack = a->feedback_key != cmd.a || a->feedback_program != cmd.status ||
                      a->feedback_part != cmd.feedback_part;

        if (attack) {
            dm_audio_feedback_stop(a);
            a->feedback_program = cmd.status;
            a->feedback_key = cmd.a;
            a->feedback_part = cmd.feedback_part;
            fluid_synth_program_change(a->feedback.synth, channel,
                                       cmd.status == 128 ? 0 : cmd.status);
        }

        fluid_synth_pitch_wheel_sens(a->feedback.synth, channel, 2);
        fluid_synth_pitch_bend(a->feedback.synth, channel,
                               MIDI_BEND_CENTER + (int)cmd.b * MIDI_BEND_CENTER / 256);

        if (attack) {
            uint32_t preset = 0;

            if (a->sample_bank >= 0 && cmd.feedback_part) {
                int part = cmd.feedback_part - 1;
                DmPatchKey route = {0, part / 16, part & MIDI_CHANNEL_MASK,
                                    cmd.status == 128 ? 0 : cmd.status, cmd.status == 128};
                preset = dm_live_sample_bank_patch(a->sample_banks[a->sample_bank], route);
            }

            a->feedback_sample_id =
                dm_audio_sample_start(a, &a->feedback, preset, channel, cmd.a, 100);
        }

        a->feedback_fraction = cmd.b;
        a->feedback_tail = RATE * 2;
    }
}

void dm_audio_feedback_render(DmAudio *a, float *out, int frames) {
    if (fluid_synth_get_active_voice_count(a->feedback.synth)) {
        a->feedback_tail = RATE * 2;
    }

    if (a->feedback_tail > 0) {
        float sound[BLOCK * 2];
        dm_audio_render(&a->feedback, sound, frames, a->peaks);

        for (int i = 0; i < frames * 2; ++i) {
            out[i] = fmaxf(-1, fminf(1, out[i] + sound[i]));
        }

        a->feedback_tail -= frames;
    }
}
