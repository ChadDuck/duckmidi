#ifndef DM_SAMPLE_EXPORT_SNAPSHOT_H
#define DM_SAMPLE_EXPORT_SNAPSHOT_H

#include "../include/duckmidi_export_plugin.h"
#include "../src/duck.h"

typedef struct DmSampleExportSnapshot DmSampleExportSnapshot;

typedef struct {
    uint16_t selected, pending;
    uint8_t program;
    bool drums;
} DmSamplePatchState;

void dm_sample_patch_state_init(const DmProject *project, DmSamplePatchState parts[DM_PARTS]);
void dm_sample_patch_state_event(DmSamplePatchState *state, unsigned status, unsigned a,
                                 unsigned b);

/* Prepares all assigned presets for interactive playback, including assignments with no
   scheduled notes. Indices are parallel to project replacements. Zero selects SoundFont. */
bool dm_sample_playback_snapshot(const DmProject *project, DmeContext *context, const DmeHost *host,
                                 uint32_t *indices, DmSampleExportSnapshot **out);

/* Resolves sample replacements against ordered source events, borrowing project and host only
   for this call. Context sample arrays and PCM remain valid while the snapshot is retained. */
bool dm_sample_export_snapshot(const DmProject *project, DmeContext *context, const DmeHost *host,
                               DmSampleExportSnapshot **out);
/* Original route/patch and paired note identity only; never opens sample assets. */
bool dm_sample_export_identities(const DmProject *, DmeContext *, const DmeHost *,
                                 DmSampleExportSnapshot **);
/* Freezes explicitly selected saved presets without assigning them to a project or opening
   the Samples UI. References and output indices are parallel; indices are one-based in the
   returned context. Fails atomically if any selected preset or asset is unavailable. */
bool dm_sample_export_selection_snapshot(const DmPatchReplacement *selections, size_t count,
                                         DmeContext *context, const DmeHost *host,
                                         uint32_t *indices, DmSampleExportSnapshot **out,
                                         char *error, size_t cap);
int32_t dm_sample_export_read(const DmSampleExportSnapshot *snapshot, uint32_t asset,
                              uint64_t start, uint64_t frames, float *pcm, uint64_t capacity);
void dm_sample_export_free(DmSampleExportSnapshot *snapshot);

#endif
