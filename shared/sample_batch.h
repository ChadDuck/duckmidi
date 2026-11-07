#ifndef DM_SAMPLE_BATCH_H
#define DM_SAMPLE_BATCH_H

#include "sample_analysis.h"

enum { DM_SAMPLE_BATCH_MAX = 256, DM_SAMPLE_BATCH_PEAKS = 4096 };

typedef enum {
    DM_SAMPLE_SCAN_PATHS,
    DM_SAMPLE_DECODE_FILE,
    DM_SAMPLE_ANALYZE_FILE,
} DmSampleBatchStage;

typedef struct {
    void *user;
    bool (*canceled)(void *user);
    void (*progress)(void *user, DmSampleBatchStage stage, const char *path, size_t index,
                     size_t total, uint64_t done, uint64_t units);
    /* Applies to directory children only, before decoding or charging the batch PCM budget. */
    bool (*include_file)(void *user, const char *path);
    void *file_user;
} DmSampleBatchCallbacks;

typedef struct {
    char *path;
    DmSampleAudio audio;
    DmSampleAnalysis analysis;
    DmSamplePeak *peaks;
    float absolute_peak;
    bool from_directory;
    char error[256];
} DmSampleBatchItem;

typedef struct {
    DmSampleBatchItem *items;
    size_t count;
} DmSampleBatch;

/* Directory imports scan direct children with supported extensions in filename order.
   Individual failures are retained. Cancellation leaves out untouched. */
bool dm_sample_batch_import(const char *const *paths, size_t count,
                            const DmSampleBatchCallbacks *callbacks, DmSampleBatch *out,
                            char *error, size_t cap);
void dm_sample_batch_free(DmSampleBatch *batch);

#endif
