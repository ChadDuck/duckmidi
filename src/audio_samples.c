#include "audio_samples.h"
#include "../shared/sample_manifest.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { COPY_PADDING = 8, SYNTH_SAMPLE_VALUES_MAX = 64 * 1024 * 1024 };

static const unsigned voice_generators[] = {
    GEN_SAMPLEMODE,    GEN_COARSETUNE,  GEN_FINETUNE,         GEN_ATTENUATION,   GEN_VOLENVDELAY,
    GEN_VOLENVATTACK,  GEN_VOLENVHOLD,  GEN_VOLENVDECAY,      GEN_VOLENVSUSTAIN, GEN_VOLENVRELEASE,
    GEN_FILTERFC,      GEN_FILTERQ,     GEN_MODENVTOFILTERFC, GEN_MODENVDELAY,   GEN_MODENVATTACK,
    GEN_MODENVHOLD,    GEN_MODENVDECAY, GEN_MODENVSUSTAIN,    GEN_MODENVRELEASE, GEN_VIBLFOFREQ,
    GEN_VIBLFOTOPITCH, GEN_CHORUSSEND,  GEN_REVERBSEND,
};

typedef struct {
    DmeSampleZoneParams params;
    fluid_sample_t *sample[2];
    float *source[2];
    short *pcm[2];
    uint32_t channels;
    uint64_t frames, original_loop_start, original_loop_end;
    unsigned cut, cut_by;
    char asset_id[DM_SAMPLE_ID_CAP];
    /* A gain edit sets target_gain; dm_sample_synth_rescale then rewrites PCM in slices. Frames
       below gain_frame use target_gain and the rest still use pcm_gain. */
    double pcm_gain, target_gain;
    uint64_t gain_frame;
    float generators[sizeof(voice_generators) / sizeof(*voice_generators)], pan[2];
} SampleVoiceZone;

typedef struct {
    char name[DM_SAMPLE_NAME_CAP];
    SampleVoiceZone *zones;
    uint32_t count, index;
    bool drums;
    bool one_shot_pass;
    struct DmSampleSynth *owner;
    fluid_preset_t *preset;
} SampleVoicePreset;

typedef struct {
    uint64_t key;
    uint32_t preset;
} PatchBinding;

struct DmLiveSampleBank {
    DmSampleSynth *synth;
    DmPatchReplacement *replacements;
    PatchBinding *bindings;
    size_t count;
};

typedef struct {
    fluid_voice_t *voice;
    SampleVoiceZone *zone;
    unsigned id, side;
} VoiceBinding;

typedef struct VoiceMap {
    fluid_synth_t *synth;
    VoiceBinding bindings[1024]; /* Twice the desktop synth's 512-voice polyphony. */
    struct VoiceMap *next;
} VoiceMap;

/* Synths a bank usually serves (ports, keyboard, feedback) get maps without allocating on the
   audio thread; further synths allocate one on first use. */
#define DM_SAMPLE_VOICE_MAPS 8

struct DmSampleSynth {
    fluid_sfont_t *font;
    SampleVoiceZone *zones;
    SampleVoicePreset *presets;
    uint32_t zone_count, preset_count;
    VoiceMap *maps, *active_map;
    VoiceMap pool[DM_SAMPLE_VOICE_MAPS];
    unsigned pooled;
};

static VoiceBinding *voice_binding(VoiceMap *map, fluid_voice_t *voice) {
    size_t at = ((uintptr_t)voice >> 4) % 1024;

    for (size_t i = 0; i < 1024; ++i) {
        VoiceBinding *entry = &map->bindings[(at + i) % 1024];

        if (!entry->voice || entry->voice == voice) {
            return entry;
        }
    }

    return NULL;
}

static VoiceMap *voice_map(DmSampleSynth *bank, fluid_synth_t *synth, bool create) {
    for (VoiceMap *map = bank->maps; map; map = map->next) {
        if (map->synth == synth) {
            return map;
        }
    }

    VoiceMap *map = !create                               ? NULL
                    : bank->pooled < DM_SAMPLE_VOICE_MAPS ? &bank->pool[bank->pooled++]
                                                          : calloc(1, sizeof(*map));

    if (map) {
        map->synth = synth;
        map->next = bank->maps;
        bank->maps = map;
    }

    return map;
}

static const char *font_name(fluid_sfont_t *font) {
    (void)font;
    return "duckmidi sample voices";
}

static fluid_preset_t *get_preset(fluid_sfont_t *font, int bank, int program) {
    const DmSampleSynth *s = fluid_sfont_get_data(font);

    if (bank < 0 || program < 0 || program > 127) {
        return NULL;
    }

    uint32_t index = (uint32_t)bank * 128 + (uint32_t)program;
    return index < s->preset_count ? s->presets[index].preset : NULL;
}

static const char *preset_name(fluid_preset_t *preset) {
    const SampleVoicePreset *p = fluid_preset_get_data(preset);
    return p->name;
}

static int preset_bank(fluid_preset_t *preset) {
    const SampleVoicePreset *p = fluid_preset_get_data(preset);
    return (int)(p->index / 128);
}

static int preset_program(fluid_preset_t *preset) {
    const SampleVoicePreset *p = fluid_preset_get_data(preset);
    return (int)(p->index % 128);
}

static float timecents(double seconds) {
    return seconds > 0 ? (float)(1200 * log2(seconds)) : -32768;
}

static float frequency_cents(double hz) {
    return (float)(1200 * log2(hz / 8.175798915643707));
}

static void prepare_generators(SampleVoiceZone *zone) {
    const DmeSampleZoneParams *v = &zone->params;
    const DmeSampleToneParams *t = &v->tone;
    float delay = timecents(v->delay), attack = timecents(v->attack), hold = timecents(v->hold),
          decay = timecents(v->decay), release = timecents(v->release);
    const float values[] = {
        v->loop_mode == DME_SAMPLE_LOOP_SUSTAIN      ? 3
        : v->loop_mode == DME_SAMPLE_LOOP_CONTINUOUS ? 1
                                                     : 0,
        (float)t->coarse,
        (float)(t->fine - v->cents),
        (float)fmax(0, -10 * v->gain_db),
        delay,
        attack,
        hold,
        decay,
        v->sustain > 0 ? (float)(-200 * log10(v->sustain)) : 1440,
        release,
        t->cutoff > 0 ? frequency_cents(t->cutoff) : 13500,
        (float)(10 * t->resonance),
        (float)t->filter_depth,
        delay,
        attack,
        hold,
        decay,
        (float)(1000 * (1 - v->sustain)),
        release,
        frequency_cents(t->vibrato_rate > 0 ? t->vibrato_rate : 5),
        (float)t->vibrato_depth,
        (float)(1000 * t->chorus),
        (float)(1000 * t->reverb),
    };
    _Static_assert(sizeof(values) == sizeof(zone->generators), "Every generator has a value");
    memcpy(zone->generators, values, sizeof(values));

    for (unsigned side = 0; side < zone->channels; ++side) {
        double pan =
            v->pan * 500 + (zone->channels == 2 ? (side ? 500 : -500) * (1 - fabs(v->pan)) : 0);
        zone->pan[side] = (float)fmax(-500, fmin(500, pan));
    }
}

static bool zone_matches(const SampleVoiceZone *z, int key, int velocity) {
    return (uint32_t)key >= z->params.key_lo && (uint32_t)key <= z->params.key_hi &&
           (uint32_t)velocity >= z->params.velocity_lo &&
           (uint32_t)velocity <= z->params.velocity_hi;
}

static int DME_CALL preset_note(fluid_preset_t *preset, fluid_synth_t *synth, int channel, int key,
                                int velocity) {
    const SampleVoicePreset *p = fluid_preset_get_data(preset);

    for (uint32_t i = 0; i < p->count; ++i) {
        const SampleVoiceZone *z = &p->zones[i];
        const DmeSampleZoneParams *v = &z->params;

        bool one_shot = p->drums && v->loop_mode == DME_SAMPLE_NO_LOOP;

        if (!zone_matches(z, key, velocity) || one_shot != p->one_shot_pass) {
            continue;
        }

        for (uint32_t side = 0; side < z->channels; ++side) {
            fluid_voice_t *voice =
                fluid_synth_alloc_voice(synth, z->sample[side], channel, key, velocity);

            if (!voice) {
                return FLUID_FAILED;
            }

            for (size_t gen = 0; gen < sizeof(voice_generators) / sizeof(*voice_generators);
                 ++gen) {
                fluid_voice_gen_set(voice, (int)voice_generators[gen], z->generators[gen]);
            }

            fluid_voice_gen_set(voice, GEN_STARTADDROFS, (float)(COPY_PADDING + v->playback_start));
            fluid_voice_gen_set(voice, GEN_ENDADDROFS,
                                (float)((int64_t)(v->playback_end ? v->playback_end : z->frames) -
                                        (int64_t)fmax(48, z->frames) - COPY_PADDING));
            fluid_voice_gen_set(voice, GEN_EXCLUSIVECLASS, (float)z->cut);
            fluid_voice_gen_set(voice, GEN_SCALETUNE,
                                v->tone.custom_tracking ? (float)v->tone.tracking
                                : p->drums              ? 0
                                                        : 100);
            fluid_voice_gen_set(voice, GEN_PAN, z->pan[side]);
            fluid_voice_gen_set(voice, GEN_STARTLOOPADDROFS,
                                (float)((int64_t)v->loop_start - (int64_t)z->original_loop_start));
            fluid_voice_gen_set(voice, GEN_ENDLOOPADDROFS,
                                (float)((int64_t)v->loop_end - (int64_t)z->original_loop_end));
            VoiceBinding *binding = voice_binding(p->owner->active_map, voice);

            if (binding) {
                *binding =
                    (VoiceBinding){voice, (SampleVoiceZone *)z, fluid_voice_get_id(voice), side};
            }

            fluid_synth_start_voice(synth, voice);
            /* Starting uses Cut; subsequent attacks compare this voice's Cut by. */
            fluid_voice_gen_set(voice, GEN_EXCLUSIVECLASS, (float)z->cut_by);
        }
    }

    return FLUID_OK;
}

static bool prepare_zone(SampleVoiceZone *zone, const DmeSampleZone *view,
                         const DmeSampleAsset *asset, const DmSampleExportSnapshot *snapshot,
                         const DmeHost *host) {
    size_t frames = (size_t)asset->frames;
    uint64_t end = view->params.playback_end ? view->params.playback_end : frames;

    if (view->params.playback_start >= end || end > frames ||
        (view->params.loop_mode != DME_SAMPLE_NO_LOOP &&
         (view->params.loop_start < view->params.playback_start || view->params.loop_end > end))) {
        return false;
    }

    zone->channels = asset->channels;
    zone->params = view->params;
    zone->frames = asset->frames;
    snprintf(zone->asset_id, sizeof(zone->asset_id), "%.*s", (int)asset->id.length,
             asset->id.data ? asset->id.data : "");
    zone->pcm_gain = pow(10, fmax(0, view->params.gain_db) / 20);
    zone->target_gain = zone->pcm_gain;
    zone->gain_frame = asset->frames;
    zone->original_loop_start = view->params.loop_mode ? view->params.loop_start : 0;
    zone->original_loop_end = view->params.loop_mode ? view->params.loop_end : frames;
    prepare_generators(zone);
    bool ok = true;

    for (uint32_t side = 0; side < asset->channels && ok; ++side) {
        float block[2048];
        size_t first = 0;
        size_t padded = (frames < 48 ? 48 : frames) + 2 * COPY_PADDING;
        zone->source[side] = malloc(frames * sizeof(float));
        zone->pcm[side] = calloc(padded, sizeof(short));
        ok = zone->source[side] && zone->pcm[side];

        while (first < frames && ok) {
            size_t count = frames - first;

            if (count > 1024) {
                count = 1024;
            }

            uint64_t capacity = sizeof(block) / sizeof(*block);
            int32_t result =
                snapshot
                    ? dm_sample_export_read(snapshot, view->asset, first, count, block, capacity)
                    : host->read_sample(host->user, view->asset, first, count, block, capacity);
            ok = result == DME_OK;

            for (size_t i = 0; ok && i < count; ++i) {
                float source = block[i * asset->channels + side];
                double value = source * zone->pcm_gain * 32768;
                zone->source[side][first + i] = source;
                zone->pcm[side][COPY_PADDING + first + i] =
                    (short)lround(fmax(-32768, fmin(32767, value)));
            }

            first += count;
        }

        fluid_sample_t *sample = zone->sample[side] = new_fluid_sample();
        ok = ok && sample &&
             fluid_sample_set_sound_data(sample, zone->pcm[side], NULL, (unsigned)padded,
                                         asset->rate, 0) == FLUID_OK &&
             fluid_sample_set_pitch(sample, (int)view->params.root, 0) == FLUID_OK;

        if (ok) {
            ok =
                fluid_sample_set_loop(sample, (unsigned)zone->original_loop_start + COPY_PADDING,
                                      (unsigned)zone->original_loop_end + COPY_PADDING) == FLUID_OK;
        }
    }

    return ok;
}

bool dm_sample_synth_loop(DmSampleSynth *s, fluid_synth_t *synth, uint64_t start, uint64_t end) {
    if (!s || s->zone_count != 1 || start >= end || end - start < 2 || end > s->zones[0].frames ||
        s->zones[0].params.loop_mode == DME_SAMPLE_NO_LOOP) {
        return false;
    }

    SampleVoiceZone *z = &s->zones[0];
    z->params.loop_start = start;
    z->params.loop_end = end;
    float lo = (float)((int64_t)start - (int64_t)z->original_loop_start);
    float hi = (float)((int64_t)end - (int64_t)z->original_loop_end);
    fluid_voice_t *voices[512] = {0};
    fluid_synth_get_voicelist(synth, voices, 512, -1);

    for (unsigned i = 0; i < sizeof(voices) / sizeof(*voices) && voices[i]; ++i) {
        fluid_voice_gen_set(voices[i], GEN_STARTLOOPADDROFS, lo);
        fluid_voice_gen_set(voices[i], GEN_ENDLOOPADDROFS, hi);
        fluid_voice_update_param(voices[i], GEN_STARTLOOPADDROFS);
        fluid_voice_update_param(voices[i], GEN_ENDLOOPADDROFS);
    }

    return true;
}

bool dm_sample_synth_rescale(DmSampleSynth *s, uint64_t budget) {
    bool pending = false;

    for (uint32_t i = 0; s && i < s->zone_count; ++i) {
        SampleVoiceZone *z = &s->zones[i];

        if (z->gain_frame >= z->frames) {
            continue;
        }

        uint64_t end = z->frames - z->gain_frame > budget ? z->gain_frame + budget : z->frames;

        for (unsigned side = 0; side < z->channels; ++side) {
            for (uint64_t frame = z->gain_frame; frame < end; ++frame) {
                double value = z->source[side][frame] * z->target_gain * 32768;
                z->pcm[side][COPY_PADDING + frame] =
                    (short)lround(fmax(-32768, fmin(32767, value)));
            }
        }

        budget -= end - z->gain_frame;
        z->gain_frame = end;

        if (end == z->frames) {
            z->pcm_gain = z->target_gain;
        } else {
            pending = true;
        }

        if (!budget) {
            /* Report later zones too, so the caller comes back for them. */
            for (uint32_t j = i + 1; j < s->zone_count && !pending; ++j) {
                pending = s->zones[j].gain_frame < s->zones[j].frames;
            }

            break;
        }
    }

    return pending;
}

bool dm_sample_synth_edit(DmSampleSynth *s, fluid_synth_t *synth, uint32_t preset,
                          const char *asset, const DmeSampleZoneParams *params) {
    return dm_sample_synth_edit_region(s, synth, preset, UINT32_MAX, asset, params);
}

bool dm_sample_synth_edit_region(DmSampleSynth *s, fluid_synth_t *synth, uint32_t preset,
                                 uint32_t region, const char *asset,
                                 const DmeSampleZoneParams *params) {
    if (!s || !synth || !params || params->size != sizeof(*params) || !preset ||
        preset > s->preset_count) {
        return false;
    }

    SampleVoicePreset *p = &s->presets[preset - 1];
    bool changed = false;
    VoiceMap *map = voice_map(s, synth, false);
    fluid_voice_t *voices[512] = {0};
    fluid_synth_get_voicelist(synth, voices, 512, -1);

    for (uint32_t i = 0; i < p->count; ++i) {
        SampleVoiceZone *z = &p->zones[i];

        if ((region != UINT32_MAX && region != i) || (asset && strcmp(asset, z->asset_id)) ||
            params->loop_end > z->frames ||
            params->playback_start >= (params->playback_end ? params->playback_end : z->frames) ||
            params->playback_end > z->frames) {
            continue;
        }

        z->params = *params;
        z->cut = params->cut_group ? preset * 128 + params->cut_group : 0;
        z->cut_by = params->cut_by ? preset * 128 + params->cut_by : 0;
        prepare_generators(z);
        double gain = pow(10, fmax(0, params->gain_db) / 20);

        if (gain != z->target_gain) {
            z->target_gain = gain;
            z->gain_frame = 0;
        }

        changed = true;

        for (unsigned side = 0; side < z->channels; ++side) {
            fluid_sample_set_pitch(z->sample[side], (int)params->root, 0);
        }

        for (unsigned v = 0; map && v < 512 && voices[v]; ++v) {
            VoiceBinding *entry = voice_binding(map, voices[v]);

            if (!entry || entry->voice != voices[v] || entry->zone != z ||
                entry->id != fluid_voice_get_id(voices[v])) {
                continue;
            }

            for (size_t gen = 0; gen < sizeof(voice_generators) / sizeof(*voice_generators);
                 ++gen) {
                fluid_voice_gen_set(voices[v], (int)voice_generators[gen], z->generators[gen]);
                fluid_voice_update_param(voices[v], (int)voice_generators[gen]);
            }

            const unsigned extra[] = {GEN_STARTADDROFS,   GEN_ENDADDROFS,      GEN_PAN,
                                      GEN_SCALETUNE,      GEN_OVERRIDEROOTKEY, GEN_STARTLOOPADDROFS,
                                      GEN_ENDLOOPADDROFS, GEN_EXCLUSIVECLASS};
            const float values[] = {
                (float)(COPY_PADDING + params->playback_start),
                (float)((int64_t)(params->playback_end ? params->playback_end : z->frames) -
                        (int64_t)fmax(48, z->frames) - COPY_PADDING),
                z->pan[entry->side],
                params->tone.custom_tracking ? (float)params->tone.tracking
                : p->drums                   ? 0
                                             : 100,
                (float)params->root,
                (float)((int64_t)params->loop_start - (int64_t)z->original_loop_start),
                (float)((int64_t)params->loop_end - (int64_t)z->original_loop_end),
                (float)z->cut_by};

            for (unsigned j = 0; j < sizeof(extra) / sizeof(*extra); ++j) {
                fluid_voice_gen_set(voices[v], (int)extra[j], values[j]);
                fluid_voice_update_param(voices[v], (int)extra[j]);
            }
        }
    }

    /* Voice generator writes queue DSP updates; commit them before the next render block. */
    (void)fluid_synth_get_active_voice_count(synth);
    return changed;
}

DmSampleSynth *dm_sample_synth_create(const DmeContext *context,
                                      const DmSampleExportSnapshot *snapshot, const DmeHost *host) {
    if (!snapshot && (!host || !host->read_sample)) {
        return NULL;
    }

    DmSampleSynth *s = calloc(1, sizeof(*s));

    if (!s) {
        return NULL;
    }

    s->presets = calloc(context->sample_preset_count, sizeof(*s->presets));
    s->zones = calloc(context->sample_zone_count, sizeof(*s->zones));
    s->font = new_fluid_sfont(font_name, get_preset, NULL, NULL, delete_fluid_sfont);
    bool ok = s->presets && s->zones && s->font && fluid_sfont_set_data(s->font, s) == FLUID_OK;
    size_t values = 0;

    for (uint32_t i = 0; ok && i < context->sample_zone_count; ++i) {
        const DmeSampleZone *zone = &context->sample_zones[i];
        const DmeSampleAsset *asset = &context->sample_assets[zone->asset];
        size_t count = (size_t)asset->frames * asset->channels;
        ++s->zone_count;
        ok = (!host || !host->cancelled || !host->cancelled(host->user)) &&
             count <= SYNTH_SAMPLE_VALUES_MAX - values &&
             prepare_zone(&s->zones[i], zone, asset, snapshot, host);
        values += count;
    }

    for (uint32_t i = 0; ok && i < context->sample_preset_count; ++i) {
        const DmeSamplePreset *p = &context->sample_presets[i];
        SampleVoicePreset *preset = &s->presets[s->preset_count++];
        *preset = (SampleVoicePreset){.zones = s->zones + p->first_zone,
                                      .count = p->zone_count,
                                      .index = i,
                                      .drums = (p->flags & DME_SAMPLE_PERCUSSION) != 0,
                                      .owner = s};
        memcpy(preset->name, p->name.data, (size_t)p->name.length);

        for (uint32_t j = 0; j < preset->count; ++j) {
            SampleVoiceZone *z = &preset->zones[j];
            z->cut = z->params.cut_group ? (i + 1) * 128 + z->params.cut_group : 0;
            z->cut_by = z->params.cut_by ? (i + 1) * 128 + z->params.cut_by : 0;
        }

        preset->preset = new_fluid_preset(s->font, preset_name, preset_bank, preset_program,
                                          preset_note, delete_fluid_preset);
        ok = preset->preset && fluid_preset_set_data(preset->preset, preset) == FLUID_OK;
    }

    if (!ok) {
        dm_sample_synth_free(s);
        return NULL;
    }

    return s;
}

void dm_sample_synth_free(DmSampleSynth *s) {
    if (!s) {
        return;
    }

    for (uint32_t i = 0; i < s->preset_count; ++i) {
        if (s->presets[i].preset) {
            delete_fluid_preset(s->presets[i].preset);
        }
    }

    for (uint32_t i = 0; i < s->zone_count; ++i) {
        for (uint32_t side = 0; side < s->zones[i].channels; ++side) {
            if (s->zones[i].sample[side]) {
                delete_fluid_sample(s->zones[i].sample[side]);
            }

            free(s->zones[i].source[side]);
            free(s->zones[i].pcm[side]);
        }
    }

    if (s->font) {
        delete_fluid_sfont(s->font);
    }

    while (s->maps) {
        VoiceMap *next = s->maps->next;

        bool pooled = false;

        for (unsigned i = 0; i < s->pooled; i++) {
            pooled |= s->maps == &s->pool[i];
        }

        if (!pooled) {
            free(s->maps);
        }

        s->maps = next;
    }

    free(s->presets);
    free(s->zones);
    free(s);
}

static bool needs_release(const SampleVoicePreset *preset, int key, int velocity) {
    if (!preset->drums) {
        return true;
    }

    for (uint32_t i = 0; i < preset->count; ++i) {
        const SampleVoiceZone *z = &preset->zones[i];

        if (z->params.loop_mode != DME_SAMPLE_NO_LOOP && zone_matches(z, key, velocity)) {
            return true;
        }
    }

    return false;
}

void dm_sample_pedal_on(DmSamplePedals *pedals, unsigned id, unsigned channel) {
    size_t count = sizeof(pedals->notes) / sizeof(*pedals->notes);
    unsigned slot = pedals->next;

    for (size_t i = 0; i < count; ++i) {
        unsigned candidate = (pedals->next + (unsigned)i) % (unsigned)count;

        if (!pedals->notes[candidate].id) {
            slot = candidate;
            break;
        }
    }

    pedals->notes[slot].id = id;
    pedals->notes[slot].channel = (uint8_t)channel;
    pedals->notes[slot].held = true;
    pedals->notes[slot].captured = false;
    pedals->next = (slot + 1) % (unsigned)count;
}

void dm_sample_pedal_off(DmSamplePedals *pedals, fluid_synth_t *synth, unsigned id) {
    for (size_t i = 0; i < sizeof(pedals->notes) / sizeof(*pedals->notes); ++i) {
        if (pedals->notes[i].id == id) {
            pedals->notes[i].held = false;

            if (pedals->notes[i].captured && pedals->down[pedals->notes[i].channel]) {
                return;
            }

            pedals->notes[i].id = 0;
            break;
        }
    }

    fluid_synth_stop(synth, id);
}

void dm_sample_pedal_cc(DmSamplePedals *pedals, fluid_synth_t *synth, unsigned channel,
                        unsigned controller, unsigned value) {
    if (controller == MIDI_CC_NOTES_OFF && pedals->down[channel]) {
        fluid_voice_t *voices[512] = {0};
        fluid_synth_get_voicelist(synth, voices, 512, -1);

        for (size_t i = 0; i < 512 && voices[i]; ++i) {
            if (fluid_voice_get_channel(voices[i]) == (int)channel) {
                dm_sample_pedal_off(pedals, synth, fluid_voice_get_id(voices[i]));
            }
        }

        return;
    }

    /* Arbitrary FluidSynth voice IDs do not participate in its MIDI note-ID capture order. */
    bool down = controller == MIDI_CC_SOSTENUTO && value >= 64;
    bool reset = controller == MIDI_CC_RESET || controller == MIDI_CC_SOUND_OFF;

    if (controller == MIDI_CC_SOSTENUTO || reset) {
        bool edge = down && !pedals->down[channel];
        pedals->down[channel] = down;

        for (size_t i = 0; i < sizeof(pedals->notes) / sizeof(*pedals->notes); ++i) {
            if (!pedals->notes[i].id || pedals->notes[i].channel != channel) {
                continue;
            }

            if (edge) {
                pedals->notes[i].captured = pedals->notes[i].held;
            } else if (!down) {
                if (!pedals->notes[i].held && pedals->notes[i].captured) {
                    fluid_synth_stop(synth, pedals->notes[i].id);
                    pedals->notes[i].id = 0;
                }

                pedals->notes[i].captured = false;
            }

            if (controller == MIDI_CC_SOUND_OFF) {
                pedals->notes[i].id = 0;
            }
        }
    }

    fluid_synth_cc(synth, (int)channel, (int)controller, (int)value);
}

DmSampleStart dm_sample_synth_start(DmSampleSynth *s, fluid_synth_t *synth, uint32_t preset,
                                    unsigned id, int channel, int key, int velocity) {
    if (!s || !preset || preset > s->preset_count) {
        return DM_SAMPLE_STOCK;
    }

    s->active_map = voice_map(s, synth, true);

    if (!s->active_map) {
        return DM_SAMPLE_STOCK;
    }

    SampleVoicePreset *p = &s->presets[preset - 1];
    bool release = needs_release(p, key, velocity);
    bool one_shot = false;

    if (p->drums) {
        for (uint32_t i = 0; i < p->count; ++i) {
            one_shot |= p->zones[i].params.loop_mode == DME_SAMPLE_NO_LOOP &&
                        zone_matches(&p->zones[i], key, velocity);
        }
    }

    p->one_shot_pass = false;

    if (release) {
        fluid_synth_start(synth, id, p->preset, 0, channel, key, velocity);
    }

    if (one_shot) {
        fluid_voice_t *voices[512] = {0};
        float classes[512];

        if (release) {
            fluid_synth_get_voicelist(synth, voices, 512, -1);

            for (size_t i = 0; i < 512 && voices[i]; ++i) {
                classes[i] = -1;

                if (fluid_voice_get_id(voices[i]) == id) {
                    classes[i] = fluid_voice_gen_get(voices[i], GEN_EXCLUSIVECLASS);
                    fluid_voice_gen_set(voices[i], GEN_EXCLUSIVECLASS, 0);
                }
            }
        }

        /* Both layers are one attack. The second group must not choke its new looping layer. */
        p->one_shot_pass = true;
        fluid_synth_start(synth, id | UINT32_C(0x40000000), p->preset, 0, channel, key, velocity);

        for (size_t i = 0; i < 512 && voices[i]; ++i) {
            if (classes[i] >= 0 && fluid_voice_get_id(voices[i]) == id) {
                fluid_voice_gen_set(voices[i], GEN_EXCLUSIVECLASS, classes[i]);
            }
        }
    }

    return release ? DM_SAMPLE_RELEASE : DM_SAMPLE_ONE_SHOT;
}

bool dm_sample_synth_note(DmSampleSynth *s, fluid_synth_t *synth, DmSamplePedals *pedals,
                          const DmScheduled *event, DmeEffectivePatch patch) {
    if (!s || !patch.sample_preset || patch.sample_preset > s->preset_count ||
        !patch.note_source_id || patch.note_source_id >= UINT32_C(0x80000000)) {
        return false;
    }

    unsigned id = UINT32_C(0x80000000) | (unsigned)patch.note_source_id;
    const SampleVoicePreset *p = &s->presets[patch.sample_preset - 1];
    int status = event->status & MIDI_STATUS_MASK;

    if (status == MIDI_NOTE_ON && event->b) {
        unsigned channel = event->status & MIDI_CHANNEL_MASK;

        if (dm_sample_synth_start(s, synth, patch.sample_preset, id, (int)channel, event->a,
                                  event->b) == DM_SAMPLE_RELEASE) {
            dm_sample_pedal_on(pedals, id, channel);
        }
        return true;
    }

    if (status != MIDI_NOTE_OFF && status != MIDI_NOTE_ON) {
        return false;
    }

    if (needs_release(p, event->a, (int)patch.note_velocity)) {
        dm_sample_pedal_off(pedals, synth, id);
    }

    return true;
}

static uint64_t binding_key(DmPatchKey key) {
    return (uint64_t)key.port << 26 | (uint64_t)key.channel << 22 | (uint64_t)key.drums << 21 |
           (uint64_t)key.bank << 7 | key.program;
}

static int compare_bindings(const void *a, const void *b) {
    const PatchBinding *x = a, *y = b;
    return (x->key > y->key) - (x->key < y->key);
}

DmLiveSampleBank *dm_live_sample_bank_create(const DmProject *p, const DmeHost *host) {
    DmLiveSampleBank *bank = calloc(1, sizeof(*bank));
    size_t count = p->patch_replacement_count;
    uint32_t *indices = calloc(count, sizeof(*indices));
    DmSampleExportSnapshot *snapshot = NULL;
    DmeContext context = {0};
    bool ok = bank && indices;

    if (ok) {
        bank->bindings = calloc(count, sizeof(*bank->bindings));
        bank->replacements = malloc(count * sizeof(*bank->replacements));
        ok = bank->bindings && bank->replacements &&
             dm_sample_playback_snapshot(p, &context, host, indices, &snapshot);
    }

    if (ok && context.sample_preset_count) {
        bank->synth = dm_sample_synth_create(&context, snapshot, host);
        ok = bank->synth != NULL;
    }

    if (ok) {
        bank->count = count;
        memcpy(bank->replacements, p->patch_replacements, count * sizeof(*bank->replacements));

        for (size_t i = 0; i < count; ++i) {
            bank->bindings[i] = (PatchBinding){binding_key(bank->replacements[i].key), indices[i]};
        }

        qsort(bank->bindings, count, sizeof(*bank->bindings), compare_bindings);
    } else {
        dm_live_sample_bank_free(bank);
        bank = NULL;
    }

    dm_sample_export_free(snapshot);
    free(indices);
    return bank;
}

void dm_live_sample_bank_free(DmLiveSampleBank *bank) {
    if (bank) {
        dm_sample_synth_free(bank->synth);
        free(bank->bindings);
        free(bank->replacements);
        free(bank);
    }
}

bool dm_live_sample_bank_matches(const DmLiveSampleBank *bank, const DmProject *p) {
    size_t count = bank ? bank->count : 0;

    if (count != p->patch_replacement_count) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        const DmPatchReplacement *a = &bank->replacements[i], *b = &p->patch_replacements[i];

        if (binding_key(a->key) != binding_key(b->key) || a->generation != b->generation ||
            strcmp(a->locator, b->locator) || strcmp(a->set_id, b->set_id) ||
            strcmp(a->preset_id, b->preset_id)) {
            return false;
        }
    }

    return true;
}

uint32_t dm_live_sample_bank_patch(const DmLiveSampleBank *bank, DmPatchKey key) {
    if (!bank) {
        return 0;
    }

    PatchBinding search = {binding_key(key), 0};
    const PatchBinding *found =
        bsearch(&search, bank->bindings, bank->count, sizeof(*bank->bindings), compare_bindings);

    if (!found && key.port) {
        key.port = 0;
        search.key = binding_key(key);
        found = bsearch(&search, bank->bindings, bank->count, sizeof(*bank->bindings),
                        compare_bindings);
    }

    return found ? found->preset : 0;
}

void dm_live_sample_bank_edit(DmLiveSampleBank *bank, fluid_synth_t *synth, const char *locator,
                              const char *set_id, const char *preset_id, uint32_t region,
                              const char *asset, const DmeSampleZoneParams *params) {
    if (!bank || !bank->synth || !synth) {
        return;
    }

    for (size_t i = 0; i < bank->count; ++i) {
        const DmPatchReplacement *r = &bank->replacements[i];

        if (!strcmp(locator, r->locator) && !strcmp(set_id, r->set_id) &&
            !strcmp(preset_id, r->preset_id)) {
            uint32_t preset = dm_live_sample_bank_patch(bank, r->key);
            dm_sample_synth_edit_region(bank->synth, synth, preset, region, asset, params);
        }
    }
}

bool dm_live_sample_bank_rescale(DmLiveSampleBank *bank, uint64_t budget) {
    return bank && dm_sample_synth_rescale(bank->synth, budget);
}

DmSampleStart dm_live_sample_bank_start(const DmLiveSampleBank *bank, fluid_synth_t *synth,
                                        uint32_t preset, unsigned id, int channel, int key,
                                        int velocity) {
    return dm_sample_synth_start(bank ? bank->synth : NULL, synth, preset, id, channel, key,
                                 velocity);
}
