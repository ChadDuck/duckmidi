#include "sample_manifest.h"
#include "../src/text/en.h"
#include "yyjson.h"
#include <math.h>
#include <ctype.h>
#include <SDL3/SDL_stdinc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ASSET_MAX = 256,
    PRESET_MAX = 256,
    ZONE_MAX = 512,
    VALUES_MAX = 16 * 1024 * 1024,
    SET_VALUES_MAX = 32 * 1024 * 1024,
};

static const char *const loop_names[] = {"none", "continuous", "sustain"};
static const char *const tone_keys[] = {
    "coarse",      "fine",         "keyTrack", "cutoff", "resonance",      "filterDepth",
    "vibratoRate", "vibratoDepth", "chorus",   "reverb", "customTracking",
};

static const struct {
    size_t offset;
    double low, high;
} tone_fields[] = {
    {offsetof(DmeSampleToneParams, coarse), -48, 48},
    {offsetof(DmeSampleToneParams, fine), -100, 100},
    {offsetof(DmeSampleToneParams, tracking), 0, 200},
    {offsetof(DmeSampleToneParams, cutoff), 0, 20000},
    {offsetof(DmeSampleToneParams, resonance), 0, 24},
    {offsetof(DmeSampleToneParams, filter_depth), -4800, 4800},
    {offsetof(DmeSampleToneParams, vibrato_rate), 0, 20},
    {offsetof(DmeSampleToneParams, vibrato_depth), 0, 200},
    {offsetof(DmeSampleToneParams, chorus), 0, 1},
    {offsetof(DmeSampleToneParams, reverb), 0, 1},
};

enum { TONE_COUNT = sizeof(tone_fields) / sizeof(*tone_fields) };

/* A field table also rejects unknown and duplicate decoded keys. */
static bool fields(yyjson_val *obj, const char *const *keys, size_t count, yyjson_val **values) {
    if (!yyjson_is_obj(obj) || yyjson_obj_size(obj) > count) {
        return false;
    }

    size_t index, total;
    yyjson_val *key, *value;
    yyjson_obj_foreach(obj, index, total, key, value) {
        size_t field = 0;

        while (field < count && !yyjson_equals_str(key, keys[field])) {
            ++field;
        }

        if (field == count || values[field]) {
            return false;
        }

        values[field] = value;
    }

    return true;
}

static bool string(yyjson_val *value, char *text, size_t cap) {
    size_t length = yyjson_get_len(value);
    const char *source = yyjson_get_str(value);

    if (!source || !length || length >= cap || memchr(source, 0, length)) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)source[i] < 32 || source[i] == 127) {
            return false;
        }
    }

    memcpy(text, source, length + 1);
    return true;
}

static bool id(yyjson_val *value, char *text) {
    return string(value, text, DM_SAMPLE_ID_CAP) && dm_sample_id_valid(text);
}

bool dm_sample_name_valid(const char *name) {
    size_t length = strlen(name);

    if (!length || length >= DM_SAMPLE_NAME_CAP || name[0] == '.' || name[length - 1] == '.' ||
        name[length - 1] == ' ') {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)name[i] < 32 || strchr("<>:\"/\\|?*", name[i])) {
            return false;
        }
    }

    size_t stem = strcspn(name, ".");
    char device[8] = {0};

    if (stem < sizeof(device)) {
        for (size_t i = 0; i < stem; ++i) {
            device[i] = (char)tolower((unsigned char)name[i]);
        }

        if (!strcmp(device, "con") || !strcmp(device, "prn") || !strcmp(device, "aux") ||
            !strcmp(device, "nul") ||
            (stem == 4 && (!strncmp(device, "com", 3) || !strncmp(device, "lpt", 3)) &&
             device[3] >= '0' && device[3] <= '9')) {
            return false;
        }
    }

    return true;
}

/* "name.wav" in the set folder, where name is a valid sample name. */
bool dm_sample_asset_filename_valid(const char *text) {
    size_t total = strlen(text);

    if (total < 5 || total - 4 >= DM_SAMPLE_NAME_CAP || strcmp(text + total - 4, ".wav")) {
        return false;
    }

    char name[DM_SAMPLE_NAME_CAP];
    memcpy(name, text, total - 4);
    name[total - 4] = 0;
    return dm_sample_name_valid(name);
}

static bool natural(yyjson_val *value, uint32_t min, uint32_t max, uint32_t *out) {
    uint64_t number = yyjson_get_uint(value);

    if (!yyjson_is_uint(value) || number < min || number > max) {
        return false;
    }

    *out = (uint32_t)number;
    return true;
}

static bool number(yyjson_val *value, double min, double max, double *out) {
    double n = yyjson_get_num(value);

    if (!yyjson_is_num(value) || !isfinite(n) || n < min || n > max) {
        return false;
    }

    *out = n;
    return true;
}

bool dm_sample_tone_valid(const DmeSampleToneParams *tone) {
    for (size_t i = 0; i < TONE_COUNT; ++i) {
        double value = *(const double *)((const char *)tone + tone_fields[i].offset);

        if (!(value >= tone_fields[i].low && value <= tone_fields[i].high)) {
            return false;
        }
    }

    return tone->custom_tracking <= 1 && tone->coarse == trunc(tone->coarse) &&
           (!tone->cutoff || tone->cutoff >= 20) &&
           (!tone->vibrato_rate || tone->vibrato_rate >= .1);
}

static bool parse_tone(yyjson_val *value, DmeSampleToneParams *tone) {
    if (!value) {
        return true;
    }

    yyjson_val *val[TONE_COUNT + 1] = {0};

    if (!fields(value, tone_keys, TONE_COUNT + 1, val) ||
        (val[TONE_COUNT] && !yyjson_is_bool(val[TONE_COUNT]))) {
        return false;
    }

    for (size_t i = 0; i < TONE_COUNT; ++i) {
        double *field = (double *)((char *)tone + tone_fields[i].offset);

        if (val[i] && !number(val[i], tone_fields[i].low, tone_fields[i].high, field)) {
            return false;
        }
    }

    tone->custom_tracking = yyjson_get_bool(val[TONE_COUNT]);
    return dm_sample_tone_valid(tone);
}

static bool byte(yyjson_val *value, uint32_t min, uint32_t max, uint8_t *out) {
    uint32_t n;

    if (!natural(value, min, max, &n)) {
        return false;
    }

    *out = (uint8_t)n;
    return true;
}

static bool parse_asset(yyjson_val *value, DmSampleAsset *asset) {
    static const char *const keys[] = {"id", "file", "frames", "rate", "channels", "sourceName"};
    yyjson_val *val[6] = {0};
    return fields(value, keys, 6, val) && id(val[0], asset->id) &&
           (!val[5] || string(val[5], asset->source_name, sizeof(asset->source_name))) &&
           string(val[1], asset->file, sizeof(asset->file)) &&
           dm_sample_asset_filename_valid(asset->file) &&
           natural(val[2], 1, VALUES_MAX, &asset->frames) &&
           natural(val[3], 8000, 192000, &asset->rate) && byte(val[4], 1, 2, &asset->channels) &&
           asset->frames <= VALUES_MAX / asset->channels;
}

static bool parse_envelope(yyjson_val *value, DmSampleEnvelope *env) {
    static const char *const keys[] = {"delay", "attack", "hold", "decay", "sustain", "release"};
    yyjson_val *val[6] = {0};
    return fields(value, keys, 6, val) && number(val[0], 0, 20, &env->delay) &&
           number(val[1], 0, 100, &env->attack) && number(val[2], 0, 20, &env->hold) &&
           number(val[3], 0, 100, &env->decay) && number(val[4], 0, 1, &env->sustain) &&
           number(val[5], 0, 100, &env->release);
}

static bool parse_loop(yyjson_val *value, DmSampleZone *zone, uint32_t frames) {
    static const char *const keys[] = {"mode", "start", "end"};
    yyjson_val *val[3] = {0};

    if (!fields(value, keys, 3, val)) {
        return false;
    }

    if (yyjson_equals_str(val[0], loop_names[DM_SAMPLE_UNLOOPED])) {
        return !val[1] && !val[2];
    }

    if (yyjson_equals_str(val[0], loop_names[DM_SAMPLE_CONTINUOUS_LOOP])) {
        zone->loop_mode = DM_SAMPLE_CONTINUOUS_LOOP;
    } else if (yyjson_equals_str(val[0], loop_names[DM_SAMPLE_SUSTAIN_LOOP])) {
        zone->loop_mode = DM_SAMPLE_SUSTAIN_LOOP;
    } else {
        return false;
    }

    uint32_t start, end;

    if (!natural(val[1], 0, frames, &start) || !natural(val[2], 0, frames, &end)) {
        return false;
    }

    zone->loop = (DmSampleLoop){start, end};
    return dm_sample_loop_valid(zone->loop, frames);
}

static bool parse_playback(yyjson_val *value, DmSampleZone *zone, uint32_t frames) {
    if (!value) {
        return true;
    }

    static const char *const keys[] = {"start", "end"};
    yyjson_val *val[2] = {0};
    uint32_t start, end;

    if (!fields(value, keys, 2, val) || !natural(val[0], 0, frames, &start) ||
        !natural(val[1], 1, frames, &end) || start >= end) {
        return false;
    }

    zone->playback = (DmSampleLoop){start, end};
    zone->playback_set = true;
    return true;
}

static bool resolve_asset(yyjson_val *value, const DmSampleManifest *manifest, uint32_t *index) {
    for (uint32_t i = 0; i < manifest->asset_count; ++i) {
        if (yyjson_equals_str(value, manifest->assets[i].id)) {
            *index = i;
            return true;
        }
    }

    return false;
}

static bool parse_zone(yyjson_val *value, const DmSampleManifest *manifest, DmSampleZone *zone,
                       bool drums) {
    static const char *const keys[] = {
        "asset", "root", "cents",    "keyLo", "keyHi", "velocityLo", "velocityHi", "gainDb",
        "pan",   "cut",  "envelope", "loop",  "tone",  "cutBy",      "playback",
    };
    yyjson_val *val[15] = {0};

    if (!fields(value, keys, 15, val) || !resolve_asset(val[0], manifest, &zone->asset) ||
        !byte(val[1], 0, 127, &zone->root) || !number(val[2], -100, 100, &zone->cents) ||
        !byte(val[3], 0, 127, &zone->key_lo) || !byte(val[4], zone->key_lo, 127, &zone->key_hi) ||
        (drums && zone->key_lo != zone->key_hi) || !byte(val[5], 1, 127, &zone->velocity_lo) ||
        !byte(val[6], zone->velocity_lo, 127, &zone->velocity_hi) ||
        !number(val[7], -96, 12, &zone->gain_db) || !number(val[8], -1, 1, &zone->pan) ||
        !byte(val[9], 0, 127, &zone->cut_group) || !byte(val[13], 0, 127, &zone->cut_by) ||
        !parse_envelope(val[10], &zone->envelope)) {
        return false;
    }

    return parse_loop(val[11], zone, manifest->assets[zone->asset].frames) &&
           parse_tone(val[12], &zone->tone) &&
           parse_playback(val[14], zone, manifest->assets[zone->asset].frames) &&
           (zone->loop_mode == DM_SAMPLE_UNLOOPED ||
            (zone->loop.start >= zone->playback.start &&
             zone->loop.end <=
                 (zone->playback.end ? zone->playback.end : manifest->assets[zone->asset].frames)));
}

static const char *parse_presets(yyjson_val *presets, DmSampleManifest *manifest) {
    static const char *const keys[] = {"id", "name", "drums", "zones", "suggestedProgram"};
    size_t index, total;
    yyjson_val *value;
    yyjson_arr_foreach(presets, index, total, value) {
        DmSamplePreset *preset = &manifest->presets[index];
        yyjson_val *val[5] = {0};
        size_t count;

        if (!fields(value, keys, 5, val) || !id(val[0], preset->id) ||
            !string(val[1], preset->name, sizeof(preset->name)) || !yyjson_is_bool(val[2]) ||
            !(count = yyjson_arr_size(val[3])) || count > ZONE_MAX - manifest->zone_count) {
            return DM_TEXT_SAMPLE_MANIFEST_PRESET;
        }

        for (size_t i = 0; i < index; ++i) {
            if (!strcmp(preset->id, manifest->presets[i].id)) {
                return DM_TEXT_SAMPLE_MANIFEST_PRESET;
            }
        }

        preset->drums = yyjson_get_bool(val[2]);

        if (val[4] && (preset->drums || !natural(val[4], 1, 128, &preset->suggested_program))) {
            return DM_TEXT_SAMPLE_MANIFEST_PRESET;
        }

        preset->first_zone = manifest->zone_count;
        preset->zone_count = (uint32_t)count;
        size_t zone_index, zone_total;
        yyjson_val *zone;
        yyjson_arr_foreach(val[3], zone_index, zone_total, zone) {
            if (!parse_zone(zone, manifest, &manifest->zones[manifest->zone_count++],
                            preset->drums)) {
                return DM_TEXT_SAMPLE_MANIFEST_ZONE;
            }
        }
    }

    return NULL;
}

static const char *parse_manifest(yyjson_val *root, DmSampleManifest *manifest) {
    static const char *const keys[] = {"version", "id", "name", "assets", "presets"};
    yyjson_val *val[5] = {0};
    uint32_t version;

    if (!fields(root, keys, 5, val)) {
        return DM_TEXT_SAMPLE_MANIFEST_INVALID;
    }

    if (!natural(val[0], DM_SAMPLE_SET_SCHEMA, DM_SAMPLE_SET_SCHEMA, &version)) {
        return DM_TEXT_SAMPLE_MANIFEST_SCHEMA;
    }

    size_t assets = yyjson_arr_size(val[3]), presets = yyjson_arr_size(val[4]);

    if (!id(val[1], manifest->id) || !string(val[2], manifest->name, sizeof(manifest->name)) ||
        !assets || assets > ASSET_MAX || !presets || presets > PRESET_MAX) {
        return DM_TEXT_SAMPLE_MANIFEST_INVALID;
    }

    manifest->asset_count = (uint32_t)assets;
    manifest->preset_count = (uint32_t)presets;
    manifest->assets = calloc(assets, sizeof(*manifest->assets));
    manifest->presets = calloc(presets, sizeof(*manifest->presets));
    manifest->zones = calloc(ZONE_MAX, sizeof(*manifest->zones));

    if (!manifest->assets || !manifest->presets || !manifest->zones) {
        return DM_TEXT_SAMPLE_IMPORT_MEMORY;
    }

    uint32_t values = 0;
    size_t index, total;
    yyjson_val *value;
    yyjson_arr_foreach(val[3], index, total, value) {
        DmSampleAsset *asset = &manifest->assets[index];

        if (!parse_asset(value, asset) ||
            asset->frames * asset->channels > SET_VALUES_MAX - values) {
            return DM_TEXT_SAMPLE_MANIFEST_ASSET;
        }

        for (size_t i = 0; i < index; ++i) {
            if (!strcmp(asset->id, manifest->assets[i].id) ||
                !SDL_strcasecmp(asset->file, manifest->assets[i].file)) {
                return DM_TEXT_SAMPLE_MANIFEST_ASSET;
            }
        }

        values += asset->frames * asset->channels;
    }

    return parse_presets(val[4], manifest);
}

bool dm_sample_manifest_parse(const char *json, size_t length, DmSampleManifest *out, char *error,
                              size_t cap) {
    yyjson_doc *doc =
        length && length <= DM_SAMPLE_MANIFEST_MAX ? yyjson_read(json, length, 0) : NULL;
    DmSampleManifest manifest = {0};
    const char *failure =
        doc ? parse_manifest(yyjson_doc_get_root(doc), &manifest) : DM_TEXT_SAMPLE_MANIFEST_INVALID;
    yyjson_doc_free(doc);

    if (failure) {
        dm_sample_manifest_free(&manifest);
        snprintf(error, cap, "%s", failure);
        return false;
    }

    *out = manifest;
    return true;
}

DmeSampleZoneParams dm_sample_zone_params(const DmSampleZone *z) {
    return (DmeSampleZoneParams){.size = sizeof(DmeSampleZoneParams),
                                 .root = z->root,
                                 .key_lo = z->key_lo,
                                 .key_hi = z->key_hi,
                                 .velocity_lo = z->velocity_lo,
                                 .velocity_hi = z->velocity_hi,
                                 .cut_group = z->cut_group,
                                 .cut_by = z->cut_by,
                                 .loop_mode = z->loop_mode,
                                 .loop_start = z->loop.start,
                                 .loop_end = z->loop.end,
                                 .cents = z->cents,
                                 .gain_db = z->gain_db,
                                 .pan = z->pan,
                                 .delay = z->envelope.delay,
                                 .attack = z->envelope.attack,
                                 .hold = z->envelope.hold,
                                 .decay = z->envelope.decay,
                                 .sustain = z->envelope.sustain,
                                 .release = z->envelope.release,
                                 .tone = z->tone,
                                 .playback_start = z->playback.start,
                                 .playback_end = z->playback.end,
                                 .playback_set = z->playback_set};
}

static bool write_tone(yyjson_mut_doc *doc, yyjson_mut_val *zone, const DmeSampleToneParams *tone) {
    if (!dm_sample_tone_valid(tone)) {
        return false;
    }

    yyjson_mut_val *obj = NULL;

    for (size_t i = 0; i < TONE_COUNT; ++i) {
        double value = *(const double *)((const char *)tone + tone_fields[i].offset);

        if (value) {
            obj = obj ? obj : yyjson_mut_obj_add_obj(doc, zone, "tone");

            if (!obj || !yyjson_mut_obj_add_real(doc, obj, tone_keys[i], value)) {
                return false;
            }
        }
    }

    if (tone->custom_tracking) {
        obj = obj ? obj : yyjson_mut_obj_add_obj(doc, zone, "tone");
        return obj && yyjson_mut_obj_add_bool(doc, obj, tone_keys[TONE_COUNT], true);
    }

    return true;
}

static bool write_zone(yyjson_mut_doc *doc, yyjson_mut_val *zones, const DmSampleManifest *m,
                       const DmSampleZone *zone) {
    if (zone->asset >= m->asset_count ||
        (unsigned)zone->loop_mode >= sizeof(loop_names) / sizeof(*loop_names)) {
        return false;
    }

    yyjson_mut_val *obj = yyjson_mut_arr_add_obj(doc, zones);
    yyjson_mut_val *env = yyjson_mut_obj_add_obj(doc, obj, "envelope");
    yyjson_mut_val *loop = yyjson_mut_obj_add_obj(doc, obj, "loop");

    if (zone->playback_set || zone->playback.start || zone->playback.end) {
        yyjson_mut_val *playback = yyjson_mut_obj_add_obj(doc, obj, "playback");
        size_t end = zone->playback.end ? zone->playback.end : m->assets[zone->asset].frames;

        if (!playback || !yyjson_mut_obj_add_uint(doc, playback, "start", zone->playback.start) ||
            !yyjson_mut_obj_add_uint(doc, playback, "end", end)) {
            return false;
        }
    }

    return obj && env && loop && yyjson_mut_obj_add_real(doc, env, "delay", zone->envelope.delay) &&
           yyjson_mut_obj_add_real(doc, env, "attack", zone->envelope.attack) &&
           yyjson_mut_obj_add_real(doc, env, "hold", zone->envelope.hold) &&
           yyjson_mut_obj_add_real(doc, env, "decay", zone->envelope.decay) &&
           yyjson_mut_obj_add_real(doc, env, "sustain", zone->envelope.sustain) &&
           yyjson_mut_obj_add_real(doc, env, "release", zone->envelope.release) &&
           yyjson_mut_obj_add_str(doc, loop, "mode", loop_names[zone->loop_mode]) &&
           (zone->loop_mode == DM_SAMPLE_UNLOOPED ||
            (yyjson_mut_obj_add_uint(doc, loop, "start", zone->loop.start) &&
             yyjson_mut_obj_add_uint(doc, loop, "end", zone->loop.end))) &&
           yyjson_mut_obj_add_str(doc, obj, "asset", m->assets[zone->asset].id) &&
           yyjson_mut_obj_add_uint(doc, obj, "root", zone->root) &&
           yyjson_mut_obj_add_real(doc, obj, "cents", zone->cents) &&
           yyjson_mut_obj_add_uint(doc, obj, "keyLo", zone->key_lo) &&
           yyjson_mut_obj_add_uint(doc, obj, "keyHi", zone->key_hi) &&
           yyjson_mut_obj_add_uint(doc, obj, "velocityLo", zone->velocity_lo) &&
           yyjson_mut_obj_add_uint(doc, obj, "velocityHi", zone->velocity_hi) &&
           yyjson_mut_obj_add_real(doc, obj, "gainDb", zone->gain_db) &&
           yyjson_mut_obj_add_real(doc, obj, "pan", zone->pan) &&
           yyjson_mut_obj_add_uint(doc, obj, "cut", zone->cut_group) &&
           yyjson_mut_obj_add_uint(doc, obj, "cutBy", zone->cut_by) &&
           write_tone(doc, obj, &zone->tone);
}

static bool write_manifest(yyjson_mut_doc *doc, const DmSampleManifest *m) {
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *assets = yyjson_mut_obj_add_arr(doc, root, "assets");
    yyjson_mut_val *presets = yyjson_mut_obj_add_arr(doc, root, "presets");

    if (!root || !assets || !presets || !memchr(m->id, 0, sizeof(m->id)) ||
        !memchr(m->name, 0, sizeof(m->name)) ||
        !yyjson_mut_obj_add_uint(doc, root, "version", DM_SAMPLE_SET_SCHEMA) ||
        !yyjson_mut_obj_add_str(doc, root, "id", m->id) ||
        !yyjson_mut_obj_add_str(doc, root, "name", m->name)) {
        return false;
    }

    for (uint32_t i = 0; i < m->asset_count; ++i) {
        const DmSampleAsset *asset = &m->assets[i];
        yyjson_mut_val *obj = yyjson_mut_arr_add_obj(doc, assets);

        if (!obj || !memchr(asset->id, 0, sizeof(asset->id)) ||
            !memchr(asset->file, 0, sizeof(asset->file)) ||
            !memchr(asset->source_name, 0, sizeof(asset->source_name)) ||
            (*asset->source_name &&
             !yyjson_mut_obj_add_str(doc, obj, "sourceName", asset->source_name)) ||
            !yyjson_mut_obj_add_str(doc, obj, "id", asset->id) ||
            !yyjson_mut_obj_add_str(doc, obj, "file", asset->file) ||
            !yyjson_mut_obj_add_uint(doc, obj, "frames", asset->frames) ||
            !yyjson_mut_obj_add_uint(doc, obj, "rate", asset->rate) ||
            !yyjson_mut_obj_add_uint(doc, obj, "channels", asset->channels)) {
            return false;
        }
    }

    uint32_t next = 0;

    for (uint32_t i = 0; i < m->preset_count; ++i) {
        const DmSamplePreset *preset = &m->presets[i];
        yyjson_mut_val *obj = yyjson_mut_arr_add_obj(doc, presets);
        yyjson_mut_val *zones = yyjson_mut_obj_add_arr(doc, obj, "zones");

        if (!obj || !zones || !memchr(preset->id, 0, sizeof(preset->id)) ||
            !memchr(preset->name, 0, sizeof(preset->name)) || preset->first_zone != next ||
            preset->zone_count > m->zone_count - next ||
            !yyjson_mut_obj_add_str(doc, obj, "id", preset->id) ||
            !yyjson_mut_obj_add_str(doc, obj, "name", preset->name) ||
            !yyjson_mut_obj_add_bool(doc, obj, "drums", preset->drums) ||
            (preset->suggested_program &&
             !yyjson_mut_obj_add_uint(doc, obj, "suggestedProgram", preset->suggested_program))) {
            return false;
        }

        for (uint32_t j = 0; j < preset->zone_count; ++j) {
            if (!write_zone(doc, zones, m, &m->zones[next++])) {
                return false;
            }
        }
    }

    return next == m->zone_count;
}

char *dm_sample_manifest_write(const DmSampleManifest *m, size_t *length) {
    *length = 0;

    if (!m->assets || !m->presets || !m->zones || m->asset_count > ASSET_MAX ||
        m->preset_count > PRESET_MAX || m->zone_count > ZONE_MAX) {
        return NULL;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    char *json =
        doc && write_manifest(doc, m) ? yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, length) : NULL;
    yyjson_mut_doc_free(doc);
    DmSampleManifest validated;

    if (json && !dm_sample_manifest_parse(json, *length, &validated, NULL, 0)) {
        free(json);
        *length = 0;
        return NULL;
    }

    if (json) {
        dm_sample_manifest_free(&validated);
    }

    return json;
}

void dm_sample_manifest_free(DmSampleManifest *manifest) {
    free(manifest->assets);
    free(manifest->presets);
    free(manifest->zones);
    *manifest = (DmSampleManifest){0};
}
