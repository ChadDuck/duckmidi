#ifndef DM_SAMPLE_MANIFEST_H
#define DM_SAMPLE_MANIFEST_H

#include "analysis.h"
#include "ids.h"
#include "../../include/duckmidi_export_plugin.h"

enum {
    DM_SAMPLE_SET_SCHEMA = 3,
    DM_SAMPLE_NAME_CAP = 128,
    DM_SAMPLE_MANIFEST_MAX = 512 * 1024,
};

typedef enum {
    DM_SAMPLE_UNLOOPED,
    DM_SAMPLE_CONTINUOUS_LOOP,
    DM_SAMPLE_SUSTAIN_LOOP,
} DmSampleLoopMode;

typedef struct {
    char id[DM_SAMPLE_ID_CAP], file[2 * DM_SAMPLE_NAME_CAP + 8];
    uint32_t frames, rate;
    uint8_t channels;
    char source_name[DM_SAMPLE_NAME_CAP];
} DmSampleAsset;

typedef struct {
    uint32_t asset;
    uint8_t root, key_lo, key_hi, velocity_lo, velocity_hi, cut_group, cut_by;
    double cents, gain_db, pan;
    DmSampleEnvelope envelope;
    DmSampleLoopMode loop_mode;
    DmSampleLoop loop;
    DmeSampleToneParams tone;
    DmSampleLoop playback; /* Zero end uses the full asset. */
    bool playback_set;
} DmSampleZone;

typedef struct {
    char id[DM_SAMPLE_ID_CAP], name[DM_SAMPLE_NAME_CAP];
    bool drums;
    uint32_t first_zone, zone_count;
    uint32_t suggested_program; /* One-based GM number, zero when unspecified. */
} DmSamplePreset;

typedef struct {
    char id[DM_SAMPLE_ID_CAP], name[DM_SAMPLE_NAME_CAP];
    uint32_t generation; /* Runtime fingerprint only. */
    uint32_t asset_count, preset_count, zone_count;
    char directory[DM_SAMPLE_NAME_CAP]; /* Resolved set folder; not serialized. */
    DmSampleAsset *assets;
    DmSamplePreset *presets;
    DmSampleZone *zones;
} DmSampleManifest;

/* On success the caller owns the manifest and frees it with dm_sample_manifest_free.
   Parsing validates the metadata. Asset files are opened by the storage layer. */
bool dm_sample_manifest_parse(const char *json, size_t length, DmSampleManifest *out, char *error,
                              size_t cap);
/* Returns malloc-owned UTF-8 JSON or NULL. */
char *dm_sample_manifest_write(const DmSampleManifest *manifest, size_t *length);
void dm_sample_manifest_free(DmSampleManifest *manifest);
bool dm_sample_name_valid(const char *text);
bool dm_sample_asset_filename_valid(const char *text);
DmeSampleZoneParams dm_sample_zone_params(const DmSampleZone *zone);
bool dm_sample_tone_valid(const DmeSampleToneParams *tone);

#endif
