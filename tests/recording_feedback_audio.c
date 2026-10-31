#include "../src/audio.c"

uint64_t recording_feedback_worker_thread(DmAudio *a) {
    return a->worker ? SDL_GetThreadID(a->worker) : 0;
}

void recording_feedback_audio_pause(DmAudio *a) {
    SDL_PauseAudioStreamDevice(a->stream);
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
