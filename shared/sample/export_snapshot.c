#include "export_snapshot.h"
#include "library.h"
#include "../../src/text/en.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SNAPSHOT_MAX_VALUES = 64 * 1024 * 1024,
    SNAPSHOT_MAX_SETS = 256,
    SNAPSHOT_MAX_ASSETS = 2048,
    SNAPSHOT_MAX_ZONES = 8192,
};

typedef struct {
    char locator[DM_PATH], id[DM_SAMPLE_ID_CAP];
    DmSampleManifest manifest;
    DmSampleLibraryReader *reader;
    uint32_t *asset_map, *preset_map;
} FrozenSet;

struct DmSampleExportSnapshot {
    FrozenSet **sets;
    size_t set_count, set_capacity;
    DmeEffectivePatch *patches;
    DmeSampleAsset *assets;
    DmSampleAudio *audio;
    DmeSamplePreset *presets;
    DmeSampleZone *zones;
    size_t asset_count, asset_capacity, audio_capacity, preset_count, preset_capacity, zone_count,
        zone_capacity, values;
};

typedef struct {
    size_t first, last;
} HeldNote;

static bool canceled(void *user) {
    const DmeHost *host = user;
    return host && host->cancelled && host->cancelled(host->user);
}

static DmeString string_view(const char *text) {
    return (DmeString){text, strlen(text)};
}

static FrozenSet *get_set(DmSampleExportSnapshot *s, const DmPatchReplacement *r) {
    for (size_t i = 0; i < s->set_count; ++i) {
        FrozenSet *set = s->sets[i];

        if (!strcmp(set->id, r->set_id) && !strcmp(set->locator, r->locator)) {
            return set;
        }
    }

    if (s->set_count >= SNAPSHOT_MAX_SETS ||
        !dm_reserve((void **)&s->sets, &s->set_capacity, s->set_count + 1, sizeof(*s->sets))) {
        return NULL;
    }

    FrozenSet *set = calloc(1, sizeof(*set));

    if (!set) {
        return NULL;
    }

    s->sets[s->set_count++] = set;
    memcpy(set->id, r->set_id, sizeof(set->id));
    memcpy(set->locator, r->locator, sizeof(set->locator));

    set->reader = dm_sample_library_reader_open(set->locator, set->id, &set->manifest, NULL, 0);

    if (set->reader) {
        set->asset_map = calloc(set->manifest.asset_count, sizeof(*set->asset_map));
        set->preset_map = calloc(set->manifest.preset_count, sizeof(*set->preset_map));

        if (!set->asset_map || !set->preset_map) {
            return NULL;
        }
    }

    return set;
}

static uint32_t get_asset(DmSampleExportSnapshot *s, FrozenSet *set, uint32_t id,
                          const DmeHost *host) {
    uint32_t *mapped = &set->asset_map[id];

    if (*mapped) {
        return *mapped;
    }

    const DmSampleAsset *asset = &set->manifest.assets[id];
    size_t values = (size_t)asset->frames * asset->channels;

    if (values > SNAPSHOT_MAX_VALUES - s->values || s->asset_count >= SNAPSHOT_MAX_ASSETS) {
        return 0;
    }

    size_t index = s->asset_count;

    if (!dm_reserve((void **)&s->assets, &s->asset_capacity, index + 1, sizeof(*s->assets)) ||
        !dm_reserve((void **)&s->audio, &s->audio_capacity, index + 1, sizeof(*s->audio))) {
        return 0;
    }

    DmSampleImportCallbacks cb = {(void *)host, canceled, NULL};

    if (!dm_sample_library_reader_audio(set->reader, &set->manifest, id, &cb, &s->audio[index],
                                        NULL, 0)) {
        *mapped = UINT32_MAX;
        return *mapped;
    }

    s->assets[index] = (DmeSampleAsset){.size = sizeof(DmeSampleAsset),
                                        .rate = asset->rate,
                                        .channels = asset->channels,
                                        .frames = asset->frames,
                                        .id = string_view(asset->id)};
    s->values += values;
    ++s->asset_count;
    *mapped = (uint32_t)index + 1;
    return *mapped;
}

static DmeSampleZone zone_view(const DmSampleZone *z, uint32_t asset) {
    return (DmeSampleZone){
        .size = sizeof(DmeSampleZone), .asset = asset, .params = dm_sample_zone_params(z)};
}

static uint32_t get_preset(DmSampleExportSnapshot *s, const DmPatchReplacement *r,
                           const DmeHost *host) {
    FrozenSet *set = get_set(s, r);

    if (!set) {
        return 0;
    }

    uint32_t index = 0;
    DmSampleManifest *m = &set->manifest;

    while (index < m->preset_count && strcmp(m->presets[index].id, r->preset_id)) {
        ++index;
    }

    if (index == m->preset_count || m->presets[index].drums != r->key.drums) {
        return UINT32_MAX;
    }

    uint32_t *mapped = &set->preset_map[index];

    if (*mapped) {
        return *mapped;
    }

    const DmSamplePreset *p = &m->presets[index];
    size_t first = s->zone_count, next = s->preset_count;

    if (p->zone_count > SNAPSHOT_MAX_ZONES - first ||
        !dm_reserve((void **)&s->presets, &s->preset_capacity, next + 1, sizeof(*s->presets)) ||
        !dm_reserve((void **)&s->zones, &s->zone_capacity, first + p->zone_count,
                    sizeof(*s->zones))) {
        return 0;
    }

    for (uint32_t i = 0; i < p->zone_count; ++i) {
        const DmSampleZone *zone = &m->zones[p->first_zone + i];
        uint32_t asset = get_asset(s, set, zone->asset, host);

        if (!asset || asset == UINT32_MAX) {
            *mapped = asset;
            return asset;
        }

        s->zones[first + i] = zone_view(zone, asset - 1);
    }

    s->presets[next] = (DmeSamplePreset){.size = sizeof(DmeSamplePreset),
                                         .generation = set->manifest.generation,
                                         .flags = p->drums ? DME_SAMPLE_PERCUSSION : 0,
                                         .first_zone = (uint32_t)first,
                                         .zone_count = p->zone_count,
                                         .locator = string_view(set->locator),
                                         .set_id = string_view(m->id),
                                         .preset_id = string_view(p->id),
                                         .name = string_view(p->name)};
    ++s->preset_count;
    s->zone_count += p->zone_count;
    *mapped = (uint32_t)next + 1;
    return *mapped;
}

static void warn_missing(const DmeHost *host, const DmPatchReplacement *r) {
    if (host && host->diagnostic) {
        char message[256];
        snprintf(message, sizeof(message), DM_TEXT_SAMPLE_EXPORT_MISSING, r->set_id, r->preset_id);
        host->diagnostic(host->user, DME_WARNING, DME_DISPLAY, message);
    }
}

void dm_sample_patch_state_init(const DmProject *p, DmSamplePatchState parts[DM_PARTS]) {
    for (uint32_t port = 0; port < DM_NOTE_PORTS; ++port) {
        for (uint32_t channel = 0; channel < 16; ++channel) {
            DmPortMix mix = dm_port_mix(p, (int)port, (int)channel);
            int program = mix.program ? mix.program - 1 : p->channels[channel].program;
            parts[port * 16 + channel] =
                (DmSamplePatchState){.program = (uint8_t)(program < 0 ? 0 : program),
                                     .drums = dm_channel_drums(p, (int)channel)};
        }
    }
}

void dm_sample_patch_state_event(DmSamplePatchState *state, unsigned status, unsigned a,
                                 unsigned b) {
    unsigned type = status & MIDI_STATUS_MASK;

    if (type == MIDI_CONTROL && a == MIDI_CC_BANK) {
        state->pending = (uint16_t)((state->pending & 127) | (b & 127u) << 7);
    } else if (type == MIDI_CONTROL && a == MIDI_CC_BANK_LSB) {
        state->pending = (uint16_t)((state->pending & (127 << 7)) | (b & 127u));
    } else if (type == MIDI_PROGRAM) {
        state->selected = state->pending;
        state->program = (uint8_t)a;
    }
}

static bool resolve_patches(DmSampleExportSnapshot *s, const DmProject *p, const DmeContext *c,
                            const DmeHost *host, HeldNote *held, size_t *links,
                            uint32_t *replacements, bool audio) {
    DmSamplePatchState parts[DM_PARTS];
    dm_sample_patch_state_init(p, parts);

    for (size_t i = 0; i < c->event_count; ++i) {
        if (canceled((void *)host)) {
            return false;
        }

        const DmeEvent *e = &c->events[i];

        if (e->status >= MIDI_SYSTEM || e->port >= DM_NOTE_PORTS || e->a >= 128) {
            continue;
        }

        uint8_t channel = (uint8_t)(e->status & MIDI_CHANNEL_MASK);
        uint32_t part = e->port * 16 + channel;
        DmSamplePatchState *state = &parts[part];
        uint32_t status = e->status & MIDI_STATUS_MASK;
        dm_sample_patch_state_event(state, e->status, e->a, e->b);

        DmeEffectivePatch *patch = &s->patches[i];
        *patch = (DmeEffectivePatch){.bank = state->selected,
                                     .program = state->program,
                                     .flags = state->drums ? DME_SAMPLE_PERCUSSION : 0};
        HeldNote *note = &held[part * 128 + e->a];

        if (status == MIDI_NOTE_OFF || (status == MIDI_NOTE_ON && !e->b)) {
            if (note->first) {
                *patch = s->patches[note->first - 1];
                note->first = links[note->first - 1];

                if (!note->first) {
                    note->last = 0;
                }
            }
        } else if (status == MIDI_NOTE_ON) {
            patch->note_velocity = e->b;
            patch->note_source_id = e->source_id;
            DmPatchKey key = {state->selected, (uint8_t)e->port, channel, state->program,
                              state->drums};
            const DmPatchReplacement *r = dm_patch_replacement(p, key);

            if (audio && r && r->generation) {
                uint32_t *mapped = &replacements[r - p->patch_replacements];

                if (!*mapped) {
                    *mapped = get_preset(s, r, host);

                    if (!*mapped) {
                        return false;
                    }

                    if (*mapped == UINT32_MAX) {
                        warn_missing(host, r);
                    }
                }

                if (*mapped == UINT32_MAX) {
                    patch->flags |= DME_SAMPLE_MISSING;
                } else {
                    patch->sample_preset = *mapped;
                }
            }

            if (note->last) {
                links[note->last - 1] = i + 1;
            } else {
                note->first = i + 1;
            }

            note->last = i + 1;
        }
    }

    return !canceled((void *)host);
}

static void expose_snapshot(const DmSampleExportSnapshot *s, DmeContext *context) {
    context->effective_patches = s->patches;
    context->sample_assets = s->assets;
    context->sample_zones = s->zones;
    context->sample_presets = s->presets;
    context->sample_asset_count = (uint32_t)s->asset_count;
    context->sample_zone_count = (uint32_t)s->zone_count;
    context->sample_preset_count = (uint32_t)s->preset_count;
}

static void close_readers(DmSampleExportSnapshot *s) {
    for (size_t i = 0; i < s->set_count; ++i) {
        dm_sample_library_reader_close(s->sets[i]->reader);
        s->sets[i]->reader = NULL;
    }
}

bool dm_sample_playback_snapshot(const DmProject *p, DmeContext *context, const DmeHost *host,
                                 uint32_t *indices, DmSampleExportSnapshot **out) {
    DmSampleExportSnapshot *s = calloc(1, sizeof(*s));

    if (!s) {
        return false;
    }

    for (size_t i = 0; i < p->patch_replacement_count; ++i) {
        const DmPatchReplacement *r = &p->patch_replacements[i];
        uint32_t preset = r->generation ? get_preset(s, r, host) : UINT32_MAX;

        if (!preset || canceled((void *)host)) {
            dm_sample_export_free(s);
            return false;
        }

        if (preset == UINT32_MAX && r->generation) {
            warn_missing(host, r);
        }

        indices[i] = preset == UINT32_MAX ? 0 : preset;
    }

    close_readers(s);
    expose_snapshot(s, context);
    *out = s;
    return true;
}

static bool source_snapshot(const DmProject *p, DmeContext *context, const DmeHost *host,
                            DmSampleExportSnapshot **out, bool audio) {
    if (!context->event_count) {
        return true;
    }

    if (context->event_count > SIZE_MAX / sizeof(DmeEffectivePatch) ||
        context->event_count > SIZE_MAX / sizeof(size_t)) {
        return false;
    }

    DmSampleExportSnapshot *s = calloc(1, sizeof(*s));
    HeldNote *held = calloc(DM_PARTS * 128, sizeof(*held));
    size_t *links = calloc((size_t)context->event_count, sizeof(*links));
    size_t replacement_count = p->patch_replacement_count ? p->patch_replacement_count : 1;
    uint32_t *replacements = calloc(replacement_count, sizeof(*replacements));

    if (s) {
        s->patches = calloc((size_t)context->event_count, sizeof(*s->patches));
    }

    bool ok = s && s->patches && held && links && replacements &&
              resolve_patches(s, p, context, host, held, links, replacements, audio);
    free(replacements);
    free(links);
    free(held);

    if (!ok) {
        dm_sample_export_free(s);
        return false;
    }

    close_readers(s);
    expose_snapshot(s, context);
    *out = s;
    return true;
}

bool dm_sample_export_snapshot(const DmProject *p, DmeContext *context, const DmeHost *host,
                               DmSampleExportSnapshot **out) {
    return source_snapshot(p, context, host, out, true);
}

bool dm_sample_export_identities(const DmProject *p, DmeContext *context, const DmeHost *host,
                                 DmSampleExportSnapshot **out) {
    return source_snapshot(p, context, host, out, false);
}

bool dm_sample_export_selection_snapshot(const DmPatchReplacement *selections, size_t count,
                                         DmeContext *context, const DmeHost *host,
                                         uint32_t *indices, DmSampleExportSnapshot **out,
                                         char *error, size_t cap) {
    if (!context || !out || !count || count > SNAPSHOT_MAX_ZONES || !selections || !indices) {
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_MANIFEST_INVALID);
        return false;
    }

    DmSampleExportSnapshot *s = calloc(1, sizeof(*s));
    uint32_t *mapped = calloc(count, sizeof(*mapped));
    bool ok = s && mapped;

    for (size_t i = 0; ok && i < count; ++i) {
        mapped[i] = selections[i].generation ? get_preset(s, &selections[i], host) : UINT32_MAX;
        ok = mapped[i] && mapped[i] != UINT32_MAX && !canceled((void *)host);
    }

    if (!ok) {
        dm_sample_export_free(s);
        free(mapped);
        snprintf(error, cap, "%s",
                 canceled((void *)host) ? DM_TEXT_SAMPLE_IMPORT_CANCELED
                                        : DM_TEXT_SAMPLE_LIBRARY_ASSET);
        return false;
    }

    close_readers(s);
    expose_snapshot(s, context);
    memcpy(indices, mapped, count * sizeof(*indices));
    *out = s;
    free(mapped);
    return true;
}

int32_t dm_sample_export_read(const DmSampleExportSnapshot *s, uint32_t asset, uint64_t start,
                              uint64_t frames, float *pcm, uint64_t capacity) {
    if (!s || !pcm || asset >= s->asset_count) {
        return DME_INVALID;
    }

    const DmSampleAudio *audio = &s->audio[asset];
    uint64_t channels = (uint64_t)audio->channels;

    if (start > audio->frames || frames > audio->frames - start || frames * channels > capacity) {
        return DME_INVALID;
    }

    memcpy(pcm, audio->pcm + start * channels, (size_t)(frames * channels) * sizeof(*pcm));
    return DME_OK;
}

void dm_sample_export_free(DmSampleExportSnapshot *s) {
    if (!s) {
        return;
    }

    for (size_t i = 0; i < s->set_count; ++i) {
        FrozenSet *set = s->sets[i];
        dm_sample_library_reader_close(set->reader);
        dm_sample_manifest_free(&set->manifest);
        free(set->asset_map);
        free(set->preset_map);
        free(set);
    }

    for (size_t i = 0; i < s->asset_count; ++i) {
        dm_sample_audio_free(&s->audio[i]);
    }

    free(s->sets);
    free(s->patches);
    free(s->assets);
    free(s->audio);
    free(s->presets);
    free(s->zones);
    free(s);
}
