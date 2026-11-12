/* Auditioning one sample outside the song, with live zone edits. */
#include "audio/internal.h"

bool dm_audio_sample_audition_edit(DmAudio *a, const DmeSampleZoneParams *params) {
    if (!a || !a->stream) {
        return false;
    }

    SDL_LockAudioStream(a->stream);
    SampleAudition *p = &a->sample_audition;
    bool ok = p->active && !p->keyboard &&
              dm_sample_synth_edit(p->bank, p->voice.synth, p->preset, NULL, params);
    if (ok) {
        bool looping = params->loop_mode != DME_SAMPLE_NO_LOOP;
        p->repeating = p->sustained && !looping;

        if (p->looping != looping) {
            p->release_in = !p->sustained && looping ? RATE : 0;
            p->looping = looping;
        }
    }

    SDL_UnlockAudioStream(a->stream);

    for (bool pending = ok; pending;) {
        SDL_LockAudioStream(a->stream);
        pending = dm_sample_synth_rescale(a->sample_audition.bank, GAIN_SLICE_FRAMES);
        SDL_UnlockAudioStream(a->stream);
    }

    return ok;
}

void dm_audio_sample_edit(DmAudio *a, const char *locator, const char *set_id,
                          const char *preset_id, uint32_t region, const char *asset,
                          const DmeSampleZoneParams *params) {
    if (!a || !a->stream) {
        return;
    }

    SDL_LockAudioStream(a->stream);

    for (int bank = 0; bank < LIVE_SAMPLE_BANKS; ++bank) {
        DmLiveSampleBank *b = a->sample_banks[bank];
        dm_live_sample_bank_edit(b, a->live.synth, locator, set_id, preset_id, region, asset,
                                 params);
        dm_live_sample_bank_edit(b, a->keyboard.synth, locator, set_id, preset_id, region, asset,
                                 params);
        dm_live_sample_bank_edit(b, a->feedback.synth, locator, set_id, preset_id, region, asset,
                                 params);

        for (unsigned port = 1; port < DM_NOTE_PORTS; ++port) {
            if (a->port_live[port]) {
                dm_live_sample_bank_edit(b, a->port_live[port]->synth, locator, set_id, preset_id,
                                         region, asset, params);
            }
        }
    }

    SDL_UnlockAudioStream(a->stream);

    for (bool pending = true; pending;) {
        pending = false;
        SDL_LockAudioStream(a->stream);

        for (int bank = 0; bank < LIVE_SAMPLE_BANKS; ++bank) {
            pending |= dm_live_sample_bank_rescale(a->sample_banks[bank], GAIN_SLICE_FRAMES);
        }

        SDL_UnlockAudioStream(a->stream);
    }
}

bool dm_audio_sample_audition_loop(DmAudio *a, uint64_t start, uint64_t end) {
    if (!a || !a->stream) {
        return false;
    }

    SDL_LockAudioStream(a->stream);
    SampleAudition *preview = &a->sample_audition;
    bool ok = preview->active && !preview->keyboard &&
              dm_sample_synth_loop(preview->bank, preview->voice.synth, start, end);
    SDL_UnlockAudioStream(a->stream);
    return ok;
}

static void audition_swap(DmAudio *a, SampleAudition next) {
    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    SampleAudition old = a->sample_audition;
    a->sample_audition = next;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    dm_audio_synth_free(&old.voice);
    dm_sample_synth_free(old.bank);
}

void dm_audio_sample_audition_stop(DmAudio *a) {
    if (a) {
        audition_swap(a, (SampleAudition){0});
    }
}

bool dm_audio_instrument_preview(DmAudio *a, const DmPatchReplacement *r, int route, char *error,
                                 size_t cap) {
    if (!a || !a->stream || !r || route < 0 || route >= 16 || r->key.port >= DM_NOTE_PORTS) {
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_AUDITION);
        return false;
    }

    SampleAudition next = {.active = true,
                           .keyboard = true,
                           .keyboard_route = route,
                           .song_part = r->key.port * 16 + route,
                           .key = r->key.drums ? 36 : 60,
                           .release_in = RATE / 3};
    DmSampleExportSnapshot *snapshot = NULL;
    DmeContext context = {0};
    uint32_t preset = 0;
    DmProject project = {.patch_replacements = (DmPatchReplacement *)r,
                         .patch_replacement_count = 1};
    bool prepared =
        !r->generation || dm_sample_playback_snapshot(&project, &context, NULL, &preset, &snapshot);

    if (prepared && preset) {
        next.preset = preset;
        const DmeSamplePreset *view = &context.sample_presets[preset - 1];
        const DmeSampleZoneParams *zone = &context.sample_zones[view->first_zone].params;
        next.key = r->key.drums ? (int)zone->key_lo : (int)zone->root;
        next.key = (int)fmin(zone->key_hi, fmax(zone->key_lo, next.key));
        next.bank = dm_sample_synth_create(&context, snapshot, NULL);
        prepared = next.bank != NULL;
    }

    dm_sample_export_free(snapshot);

    if (!prepared || !dm_audio_synth_create(&next.voice, next.bank ? NULL : a->font_path)) {
        dm_sample_synth_free(next.bank);
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_AUDITION);
        return false;
    }

    if (!next.bank) {
        fluid_synth_set_channel_type(next.voice.synth, 0,
                                     r->key.drums ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        fluid_synth_bank_select(next.voice.synth, 0,
                                r->key.drums && !r->key.bank ? 128 : r->key.bank);
        fluid_synth_program_change(next.voice.synth, 0, r->key.program);
        fluid_synth_noteon(next.voice.synth, 0, next.key, 100);
    } else {
        dm_audio_sample_audition_start(&next);
    }

    if (!next.bank) {
        fluid_synth_set_channel_type(next.voice.synth, 1,
                                     r->key.drums ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        fluid_synth_bank_select(next.voice.synth, 1,
                                r->key.drums && !r->key.bank ? 128 : r->key.bank);
        fluid_synth_program_change(next.voice.synth, 1, r->key.program);
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    int part = next.song_part;
    fluid_synth_cc(next.voice.synth, 1, MIDI_CC_VOLUME,
                   a->mix_volume[part] < 128 ? a->mix_volume[part] : 100);
    fluid_synth_cc(next.voice.synth, 1, MIDI_CC_PAN,
                   a->mix_pan[part] < 128 ? a->mix_pan[part] : 64);
    static const unsigned controls[] = {MIDI_CC_MODULATION, MIDI_CC_EXPRESSION, MIDI_CC_SUSTAIN,
                                        MIDI_CC_SOSTENUTO};

    for (unsigned i = 0; i < SDL_arraysize(controls); ++i) {
        unsigned value = a->state.cc[part][controls[i]];

        if (value < 128) {
            dm_audio_send_route(&next.voice, MIDI_CONTROL | 1, (uint8_t)controls[i],
                                (uint8_t)value);
        }
    }

    if (a->state.bend[part][0] < 128 && a->state.bend[part][1] < 128) {
        fluid_synth_pitch_bend(next.voice.synth, 1,
                               a->state.bend[part][0] | a->state.bend[part][1] << 7);
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    audition_swap(a, next);
    return true;
}

static bool audition_begin(DmAudio *a, DmSampleSynth *bank, unsigned preset, int key,
                           bool sustained, bool looping, char *error, size_t cap) {
    SampleAudition next = {.bank = bank,
                           .preset = preset,
                           .key = key,
                           .active = true,
                           .release_in = !sustained && looping ? RATE : 0,
                           .repeating = sustained && !looping,
                           .sustained = sustained,
                           .looping = looping};

    if (!a || !a->stream || !bank || !dm_audio_synth_create(&next.voice, NULL)) {
        dm_sample_synth_free(bank);
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_AUDITION);
        return false;
    }

    dm_audio_sample_audition_start(&next);
    audition_swap(a, next);
    return true;
}

bool dm_audio_sample_audition(DmAudio *a, const DmeContext *context, const DmeHost *host, int key,
                              bool sustained, bool looping, char *error, size_t cap) {
    DmSampleSynth *bank = dm_sample_synth_create(context, NULL, host);
    return audition_begin(a, bank, 1, key, sustained, looping, error, cap);
}
