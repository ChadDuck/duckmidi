#include "recording_feedback_audio.c"

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
    queue_put(&a->midi, (Command){0x90, 60, 100});
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

    while (queue_get(&a->midi, &command, NULL)) {
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
