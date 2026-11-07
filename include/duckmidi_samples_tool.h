#ifndef DUCKMIDI_SAMPLES_TOOL_H
#define DUCKMIDI_SAMPLES_TOOL_H

#include "duckmidi_export_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DME_SAMPLES_API 5u

enum { DME_SAMPLE_SCAN, DME_SAMPLE_DECODE, DME_SAMPLE_ANALYZE };

enum { DME_ROOT_DEFAULT, DME_ROOT_SIGNAL, DME_ROOT_FILENAME, DME_ROOT_EMBEDDED };

typedef struct {
    void *user;
    int32_t(DME_CALL *cancelled)(void *user);
    void(DME_CALL *progress)(void *user, uint32_t stage, DmeString path, uint32_t index,
                             uint32_t total, uint64_t done, uint64_t units);
} DmeSampleImportHost;

typedef struct {
    uint32_t size, drums;
    int32_t program, drum_key;               /* GM suggestion or choice, -1 when unspecified. */
    uint32_t melodic_key_lo, melodic_key_hi; /* Retained when switching a draft to drums. */
    DmeString name;
    DmeSampleZoneParams zone;
} DmeSampleEdit;

typedef struct {
    uint32_t size, channels, rate, root_source, pitch_conflict, peak_count, preset_drums,
        preset_zone;
    uint64_t frames, token; /* Token remains stable through undo/redo and set switching. */
    DmeString path, format, codec, error;
    DmeString asset_id, preset_id, source_name,
        asset_name; /* Saved-set identity; empty for new imports. */
    double frequency, confidence;
    float absolute_peak;
    DmeSampleEdit edit;
    int32_t embedded_root;
    double embedded_cents;
    uint64_t embedded_loop_start, embedded_loop_end;
} DmeSampleDraft;

typedef struct {
    float minimum[2], maximum[2];
} DmeSamplePeak;

typedef struct {
    uint32_t size, version;
    /* Main thread. The job owns copies of paths; no path is retained by the host. */
    void *(DME_CALL *create_import)(const void *instance, const DmeString *paths, uint32_t count,
                                    char *error, uint64_t capacity);
    /* Worker. Job and callbacks stay valid until return. The worker never uses the instance. */
    int32_t(DME_CALL *run_import)(void *job, const DmeSampleImportHost *, char *error,
                                  uint64_t capacity);
    /* Main thread after joining. A successful finish transfers the results to the instance.
       Destroy the job after any finish attempt or cancellation. */
    int32_t(DME_CALL *finish_import)(void *instance, void *job, char *error, uint64_t capacity);
    void(DME_CALL *destroy_import)(void *job);
    uint32_t(DME_CALL *count)(const void *instance);
    /* Main thread. Views remain valid until the next edit, reset, finish, or destruction. */
    int32_t(DME_CALL *draft)(const void *instance, uint32_t index, DmeSampleDraft *out);
    int32_t(DME_CALL *edit)(void *instance, uint32_t index, const DmeSampleEdit *edit);
    int32_t(DME_CALL *read_pcm)(const void *instance, uint32_t index, uint64_t start,
                                uint64_t frames, float *pcm, uint64_t capacity);
    int32_t(DME_CALL *read_peaks)(const void *instance, uint32_t index, uint32_t first,
                                  uint32_t count, DmeSamplePeak *out);
    int32_t(DME_CALL *snap_loop)(const void *instance, uint32_t index, uint64_t *start,
                                 uint64_t *end);
    int32_t(DME_CALL *remove)(void *instance, uint32_t index);
    void(DME_CALL *reset)(void *instance);
    /* Same worker/finish/destroy lifecycle as an import. Empty root/id selects a new draft.
       Previously selected sets retain unsaved drafts until reset or destruction. */
    void *(DME_CALL *create_load)(const void *instance, DmeString root, DmeString set_id,
                                  char *error, uint64_t capacity);
    /* Main thread after successful publication: give the current workspace its saved identity. */
    int32_t(DME_CALL *published)(void *instance, DmeString root, DmeString set_id);
    /* Restore detection/defaults from the original imported audio metadata and filename. */
    int32_t(DME_CALL *reset_edit)(void *instance, uint32_t index);
    void(DME_CALL *history_group)(void *instance, uint32_t active);
    uint32_t(DME_CALL *can_undo)(const void *instance, uint32_t redo);
    int32_t(DME_CALL *undo)(void *instance, uint32_t redo);

} DmeSamplesApi;

#ifdef __cplusplus
}
#endif
#endif
