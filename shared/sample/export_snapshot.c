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

/* What a preset lookup returns besides a one-based preset; zero is a failure. */
#define PRESET_MISSING UINT32_MAX      /* The set, preset or sample is unavailable. */
#define PRESET_STOCK (UINT32_MAX - 1u) /* The SoundFont plays, as assigned. */

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

/* The zone of a drum key entry's preset that plays its sample, and whether that preset is a
   kit; NULL when the set, preset or sample is gone. */
static const DmSampleZone *sample_zone(const DmSampleManifest *m, const DmPatchReplacement *r,
                                       bool *kit) {
    for (uint32_t i = 0; i < m->preset_count; ++i) {
        const DmSamplePreset *preset = &m->presets[i];

        for (uint32_t z = 0; !strcmp(preset->id, r->preset_id) && z < preset->zone_count; ++z) {
            const DmSampleZone *zone = &m->zones[preset->first_zone + z];

            if (!strcmp(m->assets[zone->asset].id, r->sample_id)) {
                *kit = preset->drums;
                return zone;
            }
        }
    }

    return NULL;
}

/* The preset a percussion patch plays on one port. With entries for single drum keys it is a
   combined kit: the whole patch's kit, with each key's entry in place of the kit's zones on
   that key, so cut groups keep working across them. A port without a whole-patch entry of its
   own takes the channel default's kit and key entries first. Returns the one-based preset,
   PRESET_STOCK or PRESET_MISSING when the SoundFont plays, or 0 on failure. */
static uint32_t get_kit(DmSampleExportSnapshot *s, const DmProject *p, DmPatchKey key,
                        const DmeHost *host) {
    const DmPatchReplacement *over[128] = {0};
    DmPatchKey wide = key;
    key.drum_key = wide.drum_key = wide.port = 0;
    const DmPatchReplacement *base = dm_patch_replacement_exact(p, key);
    bool own = base != NULL, keyed = false;

    if (!own) {
        base = dm_patch_replacement_exact(p, wide);
    }

    for (int pass = own || !key.port; pass < 2; ++pass) {
        for (size_t i = 0; i < p->patch_replacement_count; ++i) {
            const DmPatchReplacement *r = &p->patch_replacements[i];

            if (r->key.drum_key && dm_patch_same(r->key, pass ? key : wide)) {
                over[r->key.drum_key - 1] = r;
                keyed = true;
            }
        }
    }

    uint32_t kit = base && base->generation ? get_preset(s, base, host) : PRESET_STOCK;
    bool missing = kit == PRESET_MISSING;

    if (missing) {
        warn_missing(host, base);
    }

    if (!kit || !keyed) {
        return kit;
    }

    size_t base_first = kit >= PRESET_STOCK ? 0 : s->presets[kit - 1].first_zone;
    size_t base_count = kit >= PRESET_STOCK ? 0 : s->presets[kit - 1].zone_count;
    size_t first = s->zone_count, next = s->preset_count, count = 0;

    if (base_count + 128 > SNAPSHOT_MAX_ZONES - first ||
        !dm_reserve((void **)&s->presets, &s->preset_capacity, next + 1, sizeof(*s->presets)) ||
        !dm_reserve((void **)&s->zones, &s->zone_capacity, first + base_count + 128,
                    sizeof(*s->zones))) {
        return 0;
    }

    for (size_t i = 0; i < base_count; ++i) {
        DmeSampleZone zone = s->zones[base_first + i];
        bool replaced = false;

        for (uint32_t k = zone.params.key_lo; k <= zone.params.key_hi && k < 128; ++k) {
            replaced |= over[k] != NULL;
        }

        if (!replaced) {
            s->zones[first + count++] = zone;
        }
    }

    for (uint32_t k = 0; k < 128; ++k) {
        const DmPatchReplacement *r = over[k];

        if (!r || !r->generation) {
            continue;
        }

        FrozenSet *set = get_set(s, r);

        if (!set) {
            return 0;
        }

        bool drum_zone = false;
        const DmSampleZone *zone = sample_zone(&set->manifest, r, &drum_zone);
        uint32_t asset = zone ? get_asset(s, set, zone->asset, host) : UINT32_MAX;

        if (!asset) {
            return 0;
        }

        if (asset == UINT32_MAX) {
            warn_missing(host, r);
            missing = true;
            continue;
        }

        /* The sample answers its drum key at any velocity, whatever range its own zone has. A
           kit ignores the key's pitch and plays every zone as key 60, so a melody sample is
           rooted there to sound at its recorded pitch. */
        DmeSampleZone view = zone_view(zone, asset - 1);
        view.params.key_lo = view.params.key_hi = k;
        view.params.root = drum_zone ? view.params.root : 60;
        view.params.velocity_lo = 1;
        view.params.velocity_hi = 127;
        s->zones[first + count++] = view;
    }

    if (!count) {
        return missing ? PRESET_MISSING : PRESET_STOCK;
    }

    s->presets[next] = (DmeSamplePreset){.size = sizeof(DmeSamplePreset),
                                         .flags = DME_SAMPLE_PERCUSSION,
                                         .first_zone = (uint32_t)first,
                                         .zone_count = (uint32_t)count,
                                         .locator = string_view(""),
                                         .set_id = string_view(""),
                                         .preset_id = string_view(""),
                                         .name = string_view(DM_TEXT_DRUM_KIT)};
    ++s->preset_count;
    s->zone_count += count;
    return (uint32_t)next + 1;
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

/* The first entry of a note's patch on its port, or else on the channel default. Every note of
   the patch there shares that entry's preset: a percussion patch's kit and drum key entries all
   play one kit. */
static const DmPatchReplacement *patch_entry(const DmProject *p, DmPatchKey key) {
    for (int pass = key.port ? 0 : 1; pass < 2; ++pass) {
        for (size_t i = 0; i < p->patch_replacement_count; ++i) {
            if (dm_patch_same(p->patch_replacements[i].key, key)) {
                return &p->patch_replacements[i];
            }
        }

        key.port = 0;
    }

    return NULL;
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
            const DmPatchReplacement *r = patch_entry(p, key);

            if (audio && r) {
                uint32_t *mapped = &replacements[r - p->patch_replacements];

                if (!*mapped) {
                    *mapped = key.drums       ? get_kit(s, p, r->key, host)
                              : r->generation ? get_preset(s, r, host)
                                              : PRESET_STOCK;

                    if (!*mapped) {
                        return false;
                    }

                    if (*mapped == PRESET_MISSING && !key.drums) {
                        warn_missing(host, r);
                    }
                }

                if (*mapped == PRESET_MISSING) {
                    patch->flags |= DME_SAMPLE_MISSING;
                } else if (*mapped != PRESET_STOCK) {
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
        uint32_t preset;

        if (r->key.drums) {
            /* Every entry of a percussion patch on one port shares that port's kit. */
            size_t same = 0;

            while (same < i && !dm_patch_same(p->patch_replacements[same].key, r->key)) {
                ++same;
            }

            preset = same < i ? indices[same] ? indices[same] : PRESET_STOCK
                              : get_kit(s, p, r->key, host);
        } else {
            preset = r->generation ? get_preset(s, r, host) : PRESET_STOCK;

            if (preset == PRESET_MISSING) {
                warn_missing(host, r);
            }
        }

        if (!preset || canceled((void *)host)) {
            dm_sample_export_free(s);
            return false;
        }

        indices[i] = preset >= PRESET_STOCK ? 0 : preset;
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
