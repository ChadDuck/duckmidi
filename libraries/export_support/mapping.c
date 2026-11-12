#include "mapping.h"
#include "../../src/text/en.h"
#include "internal.h"
#include "../../include/duckmidi_midi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t target;
    DmeMappingSource source;
    DmeCustomSelection selected;
    bool stock_set;
    int32_t stock;
} Selection;

typedef struct {
    EsSourceRow source;
    DmeCustomMapping descriptor;
} Row;

typedef struct {
    DmeSampleTarget *items;
    size_t count;
} Targets;

struct EsMappings {
    uint32_t target_count, custom_targets;
    Row *rows;
    size_t count, capacity;
    Selection *selections;
    size_t selection_count, selection_capacity;
    Targets *targets;
    DmeSampleCatalog catalog;
};

struct EsMappingJob {
    Selection *selections;
    size_t count;
    uint32_t target_count;
};

static bool same_source(DmeMappingSource a, DmeMappingSource b) {
    return a.channel == b.channel && a.port == b.port && a.bank == b.bank &&
           a.program == b.program && a.drum_key == b.drum_key && a.flags == b.flags;
}

static uint32_t source_hash(DmeMappingSource s) {
    return s.port * 16777619u ^ s.channel * 65537u ^ s.bank * 257u ^ s.program * 17u ^
           s.drum_key * 3u ^ s.flags;
}

static DmeString copy_string(DmeString value) {
    DmeString result = {0};

    if (!value.length) {
        result.data = SDL_strdup("");
        return result;
    }

    if (value.length > DME_PATH_LIMIT || !value.data ||
        memchr(value.data, 0, (size_t)value.length)) {
        return result;
    }

    char *copy = SDL_malloc((size_t)value.length + 1);

    if (copy) {
        memcpy(copy, value.data, (size_t)value.length);
        copy[value.length] = 0;
        result = (DmeString){copy, value.length};
    }

    return result;
}

static void free_string(DmeString value) {
    SDL_free((void *)value.data);
}

static void selection_free(Selection *selection) {
    free_string(selection->selected.sample.locator);
    free_string(selection->selected.sample.set_id);
    free_string(selection->selected.sample.preset_id);
    free_string(selection->selected.physical_target);
    *selection = (Selection){0};
}

static bool selection_copy(Selection *out, const Selection *source) {
    *out = *source;
    out->selected.sample.locator = copy_string(source->selected.sample.locator);
    out->selected.sample.set_id = copy_string(source->selected.sample.set_id);
    out->selected.sample.preset_id = copy_string(source->selected.sample.preset_id);
    out->selected.physical_target = copy_string(source->selected.physical_target);

    if (!out->selected.sample.locator.data || !out->selected.sample.set_id.data ||
        !out->selected.sample.preset_id.data || !out->selected.physical_target.data) {
        selection_free(out);
        return false;
    }

    return true;
}

static void catalog_free(DmeSampleCatalog *catalog) {
    for (uint32_t i = 0; i < catalog->set_count; ++i) {
        const DmeSampleCatalogSet *set = &catalog->sets[i];
        free_string(set->locator);
        free_string(set->id);
        free_string(set->name);
        free_string(set->error);
    }

    for (uint32_t i = 0; i < catalog->preset_count; ++i) {
        free_string(catalog->presets[i].id);
        free_string(catalog->presets[i].name);
    }

    free((void *)catalog->sets);
    free((void *)catalog->presets);
    free((void *)catalog->zones);
    *catalog = (DmeSampleCatalog){.size = sizeof(*catalog)};
}

static bool catalog_copy(DmeSampleCatalog *out, const DmeSampleCatalog *source) {
    *out = (DmeSampleCatalog){.size = sizeof(*out)};

    if (!source) {
        return true;
    }

    if (source->size != sizeof(*source) || source->set_count > 1024 ||
        source->preset_count > DME_PRESET_LIMIT || source->zone_count > 32768 ||
        (source->set_count && !source->sets) || (source->preset_count && !source->presets) ||
        (source->zone_count && !source->zones)) {
        return false;
    }

    DmeSampleCatalogSet *sets = calloc(source->set_count ? source->set_count : 1, sizeof(*sets));
    DmeSampleCatalogPreset *presets =
        calloc(source->preset_count ? source->preset_count : 1, sizeof(*presets));
    DmeSampleZoneParams *zones =
        calloc(source->zone_count ? source->zone_count : 1, sizeof(*zones));
    out->sets = sets;
    out->presets = presets;
    out->zones = zones;
    bool ok = sets && presets && zones;

    for (uint32_t i = 0; ok && i < source->set_count; ++i) {
        const DmeSampleCatalogSet *s = &source->sets[i];
        sets[i] = *s;
        out->set_count++;
        sets[i].locator = copy_string(s->locator);
        sets[i].id = copy_string(s->id);
        sets[i].name = copy_string(s->name);
        sets[i].error = copy_string(s->error);
        ok = sets[i].locator.data && sets[i].id.data && sets[i].name.data && sets[i].error.data &&
             s->first_preset <= source->preset_count &&
             s->preset_count <= source->preset_count - s->first_preset;
    }

    for (uint32_t i = 0; ok && i < source->preset_count; ++i) {
        const DmeSampleCatalogPreset *p = &source->presets[i];
        presets[i] = *p;
        out->preset_count++;
        presets[i].id = copy_string(p->id);
        presets[i].name = copy_string(p->name);
        ok = presets[i].id.data && presets[i].name.data && p->set < source->set_count &&
             p->first_zone <= source->zone_count &&
             p->zone_count <= source->zone_count - p->first_zone;
    }

    if (ok && source->zone_count) {
        memcpy(zones, source->zones, source->zone_count * sizeof(*zones));
    }

    out->zone_count = source->zone_count;

    if (!ok) {
        catalog_free(out);
    }

    return ok;
}

EsMappings *es_mappings_create(uint32_t targets, uint32_t custom_targets) {
    if (!targets || targets > 32) {
        return NULL;
    }

    EsMappings *s = calloc(1, sizeof(*s));

    if (s) {
        s->targets = calloc(targets, sizeof(*s->targets));
        s->target_count = targets;
        s->custom_targets = custom_targets;
        s->catalog.size = sizeof(s->catalog);

        if (!s->targets) {
            free(s);
            return NULL;
        }
    }

    return s;
}

void es_mappings_reset(EsMappings *s) {
    for (size_t i = 0; i < s->selection_count; ++i) {
        selection_free(&s->selections[i]);
    }

    s->selection_count = 0;
}

void es_mappings_destroy(EsMappings *s) {
    if (s) {
        es_mappings_reset(s);
        catalog_free(&s->catalog);

        for (uint32_t i = 0; i < s->target_count; ++i) {
            for (size_t j = 0; j < s->targets[i].count; ++j) {
                free_string(s->targets[i].items[j].id);
                free_string(s->targets[i].items[j].label);
                free_string(s->targets[i].items[j].path);
            }

            free(s->targets[i].items);
        }

        free(s->targets);
        free(s->rows);
        free(s->selections);
        free(s);
    }
}

static int source_order(const void *left, const void *right) {
    const Row *a = left, *b = right;
    const uint32_t aa[] = {a->source.source.flags,   a->source.source.port,
                           a->source.source.channel, a->source.source.bank,
                           a->source.source.program, a->source.source.drum_key};
    const uint32_t bb[] = {b->source.source.flags,   b->source.source.port,
                           b->source.source.channel, b->source.source.bank,
                           b->source.source.program, b->source.source.drum_key};

    for (size_t i = 0; i < SDL_arraysize(aa); ++i) {
        if (aa[i] != bb[i]) {
            return aa[i] < bb[i] ? -1 : 1;
        }
    }

    return 0;
}

bool es_mappings_inspect(EsMappings *s, const DmeContext *context, const uint8_t *native,
                         uint32_t routes, char *error, size_t cap) {
    if (!context || routes > 128 || !routes || (context->event_count && !context->events)) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_SOURCE_MAPPING_CONTEXT);
    }

    DmeSampleCatalog catalog;

    if (!catalog_copy(&catalog, context->library)) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_RETAIN_THE_SAVED_SAMPLE_CATALOG);
    }

    catalog_free(&s->catalog);
    s->catalog = catalog;
    s->count = 0;

    struct {
        uint32_t pending, bank, program;
    } parts[128][16] = {0};

    int hash[4096];
    memset(hash, -1, sizeof(hash));

    for (uint64_t i = 0; i < context->event_count; ++i) {
        const DmeEvent *event = &context->events[i];

        if (event->status < MIDI_STATUS || event->status >= MIDI_SYSTEM || event->port >= routes ||
            event->a > 127 || event->b > 127) {
            continue;
        }

        uint32_t channel = event->status & MIDI_CHANNEL_MASK;
        uint32_t type = event->status & MIDI_STATUS_MASK;
        uint32_t port = event->port;

        if (type == MIDI_CONTROL && event->a == MIDI_CC_BANK) {
            parts[port][channel].pending = (parts[port][channel].pending & 127u) | event->b << 7;
        } else if (type == MIDI_CONTROL && event->a == MIDI_CC_BANK_LSB) {
            parts[port][channel].pending = (parts[port][channel].pending & (127u << 7)) | event->b;
        } else if (type == MIDI_PROGRAM) {
            parts[port][channel].program = event->a;
            parts[port][channel].bank = parts[port][channel].pending;
        }

        if (type != MIDI_NOTE_ON || !event->b) {
            continue;
        }

        bool drums = (context->percussion_channels & (1u << channel)) != 0;
        uint32_t program = parts[port][channel].program;
        uint32_t bank = parts[port][channel].bank;

        if (context->effective_patches) {
            const DmeEffectivePatch *patch = &context->effective_patches[i];
            program = patch->program;
            bank = patch->bank;
            drums = (patch->flags & DME_SAMPLE_PERCUSSION) != 0;
        }

        if (program > 127 || bank > 16383) {
            return es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_ORIGINAL_MIDI_PATCH_IDENTITY);
        }

        DmeMappingSource source = {channel,
                                   port,
                                   bank,
                                   drums ? 0 : program,
                                   drums ? event->a : 0,
                                   drums ? DME_SAMPLE_PERCUSSION : 0};
        uint32_t h = (port * 16777619u ^ channel * 65537u ^ bank * 257u ^ source.program * 17u ^
                      source.drum_key * 3u ^ source.flags) &
                     4095;

        while (hash[h] >= 0 && !same_source(s->rows[hash[h]].source.source, source)) {
            h = (h + 1) & 4095;
        }

        if (hash[h] >= 0) {
            continue;
        }

        if (s->count == DME_ROW_LIMIT ||
            !es_reserve((void **)&s->rows, &s->capacity, s->count + 1, sizeof(*s->rows))) {
            return es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_MORE_THAN_2048_SOURCE_MAPPINGS_CANNOT_BE_REPRESENTED_BY);
        }

        hash[h] = (int)s->count;
        Row *row = &s->rows[s->count++];
        *row =
            (Row){.source = {.source = source,
                             .native_channel = native ? native[channel * routes + port] : channel}};
        const char *name = drums ? context->drum_names[event->a] : context->program_names[program];
        int name_length = -1;
        uint32_t preset =
            context->effective_patches ? context->effective_patches[i].sample_preset : 0;

        /* A custom sample on this route is named instead of the instrument it replaces. */
        if (preset && preset <= context->sample_preset_count && context->sample_presets &&
            context->sample_presets[preset - 1].name.data &&
            context->sample_presets[preset - 1].name.length) {
            const DmeString *sample = &context->sample_presets[preset - 1].name;
            name = sample->data;
            name_length = sample->length > 200 ? 200 : (int)sample->length;
        }

        if (drums && channel == MIDI_DRUM_CHANNEL && !port && !bank) {
            snprintf(row->source.id, sizeof(row->source.id), "drum.%u", event->a);
        } else if (drums) {
            snprintf(row->source.id, sizeof(row->source.id), "drum.%u.%u.%u.%u", channel, event->a,
                     port, bank);
        } else if (!port && !bank) {
            snprintf(row->source.id, sizeof(row->source.id), "melodic.%u.%u", channel, program);
        } else {
            snprintf(row->source.id, sizeof(row->source.id), "melodic.%u.%u.%u.%u", channel,
                     program, port, bank);
        }

        snprintf(row->source.label, sizeof(row->source.label), "Ch %u: %.*s / %s %u", channel + 1,
                 name ? name_length : 0, name ? name : "", drums ? "note" : "GM",
                 drums ? event->a : program + 1);
    }

    qsort(s->rows, s->count, sizeof(*s->rows), source_order);
    return true;
}

size_t es_mappings_count(const EsMappings *s) {
    return s->count;
}

const EsSourceRow *es_mappings_source(const EsMappings *s, size_t index) {
    return index < s->count ? &s->rows[index].source : NULL;
}

static Selection *choice(EsMappings *s, uint32_t target, DmeMappingSource source) {
    for (size_t i = 0; i < s->selection_count; ++i) {
        if (s->selections[i].target == target && same_source(s->selections[i].source, source)) {
            return &s->selections[i];
        }
    }

    if (target >= s->target_count || s->selection_count >= 16384 ||
        !es_reserve((void **)&s->selections, &s->selection_capacity, s->selection_count + 1,
                    sizeof(*s->selections))) {
        return NULL;
    }

    Selection *selected = &s->selections[s->selection_count++];
    *selected = (Selection){.target = target,
                            .source = source,
                            .selected = {.size = sizeof(DmeCustomSelection),
                                         .sample = {.size = sizeof(DmeSampleSelection)}}};
    return selected;
}

const DmeCustomMapping *es_mappings_describe(EsMappings *s, uint32_t target, size_t index) {
    if (index >= s->count || target >= s->target_count) {
        return NULL;
    }

    Row *row = &s->rows[index];
    Selection *selected = choice(s, target, row->source.source);

    if (!selected) {
        return NULL;
    }

    if (!selected->selected.sample.set_id.length) {
        const DmeSampleCatalogSet *latest = NULL;

        for (uint32_t i = 0; i < s->catalog.set_count; ++i) {
            const DmeSampleCatalogSet *set = &s->catalog.sets[i];

            if (set->available && (!latest || set->modified > latest->modified ||
                                   (set->modified == latest->modified &&
                                    strcmp(set->id.data, latest->id.data) < 0))) {
                latest = set;
            }
        }

        if (latest) {
            free_string(selected->selected.sample.set_id);
            free_string(selected->selected.sample.locator);
            selected->selected.sample.set_id = copy_string(latest->id);
            selected->selected.sample.locator = copy_string(latest->locator);
            selected->selected.sample.generation = latest->generation;
        }
    }

    for (uint32_t i = 0; i < s->catalog.set_count; ++i) {
        const DmeSampleCatalogSet *set = &s->catalog.sets[i];

        if (selected->selected.sample.set_id.data &&
            !strcmp(set->id.data, selected->selected.sample.set_id.data)) {
            selected->selected.sample.generation = set->generation;

            if (!selected->selected.sample.locator.data ||
                strcmp(selected->selected.sample.locator.data, set->locator.data)) {
                DmeString locator = copy_string(set->locator);

                if (locator.data) {
                    free_string(selected->selected.sample.locator);
                    selected->selected.sample.locator = locator;
                }
            }

            break;
        }
    }

    row->descriptor = (DmeCustomMapping){.size = sizeof(DmeCustomMapping),
                                         .available = (s->custom_targets & (1u << target)) != 0,
                                         .selection = selected->selected,
                                         .catalog = &s->catalog,
                                         .targets = s->targets[target].items,
                                         .target_count = (uint32_t)s->targets[target].count};
    static const char unavailable[] =
        DM_TEXT_REPOSITORY_CUSTOM_INSTRUMENT_SAMPLES_ARE_UNAVAILABLE_FOR_THIS_TARGET;

    if (!row->descriptor.available) {
        row->descriptor.unavailable_reason = (DmeString){unavailable, sizeof(unavailable) - 1};
    }

    return &row->descriptor;
}

int32_t es_mappings_set(EsMappings *s, uint32_t target, const char *id,
                        const DmeCustomSelection *value, char *error, size_t cap) {
    if (target >= s->target_count || !(s->custom_targets & (1u << target)) || !value ||
        value->size != sizeof(*value) || value->enabled > 1 || value->show_all > 1 ||
        value->operation > DME_SAMPLE_REPLACE || value->region_set > 1 ||
        value->sample.size != sizeof(value->sample)) {
        es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_CUSTOM_SAMPLE_MAPPING_FOR_THIS_TARGET);
        return DME_INVALID;
    }

    for (size_t i = 0; i < s->count; ++i) {
        if (strcmp(s->rows[i].source.id, id)) {
            continue;
        }

        Selection *selected = choice(s, target, s->rows[i].source.source);
        Selection incoming = selected ? *selected : (Selection){0};
        incoming.selected = *value;
        Selection copied;

        if (!selected || !selection_copy(&copied, &incoming)) {
            es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_RETAIN_CUSTOM_SAMPLE_MAPPING);
            return DME_NOMEM;
        }

        selection_free(selected);
        *selected = copied;
        return DME_OK;
    }

    es_fail(error, cap, DM_TEXT_REPOSITORY_THE_SOURCE_MAPPING_IS_NO_LONGER_PRESENT);
    return DME_UNAVAILABLE;
}

bool es_mappings_targets(EsMappings *s, uint32_t target, const DmeSampleTarget *items, size_t count,
                         char *error, size_t cap) {
    if (target >= s->target_count || count > 20000 || (count && !items)) {
        return es_fail(error, cap,
                       DM_TEXT_REPOSITORY_INVALID_PHYSICAL_REPLACEMENT_TARGET_INVENTORY);
    }

    Targets replacement = {.items = calloc(count ? count : 1, sizeof(*items))};
    bool ok = replacement.items != NULL;

    for (size_t i = 0; ok && i < count; ++i) {
        replacement.count++;
        replacement.items[i] = (DmeSampleTarget){
            copy_string(items[i].id), copy_string(items[i].label), copy_string(items[i].path)};
        ok = replacement.items[i].id.data && replacement.items[i].label.data &&
             replacement.items[i].path.data;
    }

    Targets old = ok ? s->targets[target] : replacement;

    for (size_t i = 0; i < old.count; ++i) {
        free_string(old.items[i].id);
        free_string(old.items[i].label);
        free_string(old.items[i].path);
    }

    free(old.items);

    if (ok) {
        s->targets[target] = replacement;
    }

    return ok || es_fail(error, cap,
                         DM_TEXT_REPOSITORY_CANNOT_RETAIN_PHYSICAL_REPLACEMENT_TARGET_INVENTORY);
}

bool es_mappings_stock_id(const EsMappings *s, const char *id, char out[64]) {
    for (size_t i = 0; i < s->count; ++i) {
        const EsSourceRow *row = &s->rows[i].source;

        if (!strcmp(row->id, id)) {
            if (row->source.flags & DME_SAMPLE_PERCUSSION) {
                snprintf(out, 64, "drum.%u", row->source.drum_key);
            } else {
                snprintf(out, 64, "melodic.%u.%u", row->native_channel, row->source.program);
            }

            return true;
        }
    }

    return false;
}

EsMappingJob *es_mappings_freeze(const EsMappings *s, char *error, size_t cap) {
    EsMappingJob *job = calloc(1, sizeof(*job));

    if (job) {
        job->target_count = s->target_count;
        job->selections =
            calloc(s->selection_count ? s->selection_count : 1, sizeof(*job->selections));
    }

    bool ok = job && job->selections;

    for (size_t i = 0; ok && i < s->selection_count; ++i) {
        if (!s->selections[i].selected.enabled && !s->selections[i].stock_set) {
            continue;
        }

        bool used = false;

        for (size_t j = 0; j < s->count; ++j) {
            used |= same_source(s->rows[j].source.source, s->selections[i].source);
        }

        if (!used) {
            continue;
        }

        ok = selection_copy(&job->selections[job->count], &s->selections[i]);

        if (ok) {
            job->count++;
        }
    }

    if (!ok) {
        es_mapping_job_free(job);
        es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_FREEZE_CUSTOM_SAMPLE_SELECTIONS);
        return NULL;
    }

    return job;
}

void es_mapping_job_free(EsMappingJob *job) {
    if (job) {
        for (size_t i = 0; i < job->count; ++i) {
            selection_free(&job->selections[i]);
        }

        free(job->selections);
        free(job);
    }
}

const DmeCustomSelection *es_mapping_job_selection(const EsMappingJob *job, uint32_t target,
                                                   DmeMappingSource source) {
    if (job) {
        for (size_t i = 0; i < job->count; ++i) {
            if (job->selections[i].target == target &&
                same_source(job->selections[i].source, source)) {
                return &job->selections[i].selected;
            }
        }
    }

    return NULL;
}

bool es_mapping_job_custom(const EsMappingJob *job, uint32_t target) {
    if (job) {
        for (size_t i = 0; i < job->count; ++i) {
            if (job->selections[i].target == target && job->selections[i].selected.enabled) {
                return true;
            }
        }
    }

    return false;
}

bool es_mappings_set_stock(EsMappings *s, uint32_t target, const char *id, int32_t value) {
    for (size_t i = 0; i < s->count; i++) {
        if (!strcmp(s->rows[i].source.id, id)) {
            Selection *selected = choice(s, target, s->rows[i].source.source);

            if (!selected) {
                return false;
            }

            selected->stock_set = true;
            selected->stock = value;
            return true;
        }
    }

    return false;
}

int32_t es_mappings_stock(EsMappings *s, uint32_t target, DmeMappingSource source,
                          int32_t fallback) {
    Selection *selected = choice(s, target, source);
    return selected && selected->stock_set ? selected->stock : fallback;
}

bool es_mapping_job_values(const EsMappingJob *job, uint32_t target, const DmeContext *context,
                           int32_t *values, char *error, size_t cap) {
    bool ok = job && context && (!context->event_count || context->events);

    if (!ok) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_SOURCE_MAPPING_CONTEXT);
    }

    uint32_t pending[128][16] = {{0}}, banks[128][16] = {{0}}, programs[128][16] = {{0}};
    int lookup[32768];
    memset(lookup, -1, sizeof(lookup));

    for (size_t i = 0; i < job->count; i++) {
        const Selection *selection = &job->selections[i];

        if (selection->target != target || !selection->stock_set) {
            continue;
        }

        uint32_t hash = source_hash(selection->source) & 32767;

        while (lookup[hash] >= 0) {
            hash = (hash + 1) & 32767;
        }

        lookup[hash] = (int)i;
    }

    for (uint64_t i = 0; ok && i < context->event_count; i++) {
        values[i] = -1;
        const DmeEvent *e = &context->events[i];

        if (e->status < MIDI_STATUS || e->status >= MIDI_SYSTEM || e->port >= 128) {
            continue;
        }

        uint32_t ch = e->status & MIDI_CHANNEL_MASK, type = e->status & MIDI_STATUS_MASK;

        if (type == MIDI_PROGRAM) {
            programs[e->port][ch] = e->a;
            banks[e->port][ch] = pending[e->port][ch];
        } else if (type == MIDI_CONTROL && e->a == MIDI_CC_BANK) {
            pending[e->port][ch] = (pending[e->port][ch] & 127) | (e->b << 7);
        } else if (type == MIDI_CONTROL && e->a == MIDI_CC_BANK_LSB) {
            pending[e->port][ch] = (pending[e->port][ch] & ~127u) | e->b;
        }

        if (type != MIDI_NOTE_ON || !e->b) {
            continue;
        }

        const DmeEffectivePatch *p =
            context->effective_patches ? &context->effective_patches[i] : NULL;
        bool drum = p ? (p->flags & DME_SAMPLE_PERCUSSION) != 0
                      : (context->percussion_channels & (1u << ch)) != 0;
        DmeMappingSource source = {ch,
                                   e->port,
                                   p ? p->bank : banks[e->port][ch],
                                   drum ? 0
                                   : p  ? p->program
                                        : programs[e->port][ch],
                                   drum ? e->a : 0,
                                   drum ? DME_SAMPLE_PERCUSSION : 0};

        uint32_t hash = source_hash(source) & 32767;

        while (lookup[hash] >= 0) {
            const Selection *selection = &job->selections[lookup[hash]];

            if (same_source(selection->source, source)) {
                values[i] = selection->stock;
                break;
            }

            hash = (hash + 1) & 32767;
        }
    }

    return ok;
}

bool es_mapping_row_id(const DmeContext *context, uint64_t index, char out[64]) {
    if (!context || index >= context->event_count) {
        return false;
    }

    const DmeEvent *note = &context->events[index];
    unsigned ch = note->status & MIDI_CHANNEL_MASK, port = note->port;

    if (port >= 128) {
        return false;
    }

    unsigned bank = 0, pending = 0, program = 0;
    bool drum = (context->percussion_channels & (1u << ch)) != 0;

    for (uint64_t i = 0; i < index; i++) {
        const DmeEvent *e = &context->events[i];

        if (e->port != port || (e->status & MIDI_CHANNEL_MASK) != ch) {
            continue;
        }

        unsigned type = e->status & MIDI_STATUS_MASK;

        if (type == MIDI_PROGRAM) {
            program = e->a;
            bank = pending;
        } else if (type == MIDI_CONTROL && e->a == MIDI_CC_BANK) {
            pending = (pending & 127) | (e->b << 7);
        } else if (type == MIDI_CONTROL && e->a == MIDI_CC_BANK_LSB) {
            pending = (pending & ~127u) | e->b;
        }
    }

    if (context->effective_patches) {
        const DmeEffectivePatch *p = &context->effective_patches[index];
        bank = p->bank;
        program = p->program;
        drum = (p->flags & DME_SAMPLE_PERCUSSION) != 0;
    }

    if (drum && ch == MIDI_DRUM_CHANNEL && !port && !bank) {
        snprintf(out, 64, "drum.%u", note->a);
    } else if (drum) {
        snprintf(out, 64, "drum.%u.%u.%u.%u", ch, note->a, port, bank);
    } else if (!port && !bank) {
        snprintf(out, 64, "melodic.%u.%u", ch, program);
    } else {
        snprintf(out, 64, "melodic.%u.%u.%u.%u", ch, program, port, bank);
    }

    return true;
}

bool es_mapping_job_set_stock(EsMappingJob *job, uint32_t target, const char *id, int32_t value) {
    unsigned ch = 0, patch = 0, port = 0, bank = 0;
    bool drum = !strncmp(id, "drum.", 5);

    if (drum && sscanf(id, "drum.%u.%u.%u.%u", &ch, &patch, &port, &bank) == 1) {
        patch = ch;
        ch = MIDI_DRUM_CHANNEL;
    } else if (!drum && sscanf(id, "melodic.%u.%u.%u.%u", &ch, &patch, &port, &bank) < 2) {
        return false;
    }

    if (ch >= 16 || patch >= 128 || port >= 128 || bank >= 16384 || target >= job->target_count) {
        return false;
    }

    DmeMappingSource source = {
        ch, port, bank, drum ? 0 : patch, drum ? patch : 0, drum ? DME_SAMPLE_PERCUSSION : 0};

    for (size_t i = 0; i < job->count; i++) {
        Selection *s = &job->selections[i];

        if (s->target == target && same_source(s->source, source)) {
            s->stock_set = true;
            s->stock = value;
            return true;
        }
    }

    if (job->count >= 16384) {
        return false;
    }

    Selection *grown = realloc(job->selections, (job->count + 1) * sizeof(*grown));

    if (!grown) {
        return false;
    }

    job->selections = grown;
    job->selections[job->count++] =
        (Selection){.target = target, .source = source, .stock_set = true, .stock = value};
    return true;
}
