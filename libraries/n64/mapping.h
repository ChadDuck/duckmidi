#ifndef DUCK_N64_MAPPING_H
#define DUCK_N64_MAPPING_H

#include "../../include/duckmidi_export_plugin.h"
#include "../common/common.h"

typedef struct EsMappings EsMappings;
typedef struct EsMappingJob EsMappingJob;

typedef struct {
    DmeMappingSource source;
    uint32_t native_channel;
    char id[64], label[256];
} EsSourceRow;

EsMappings *es_mappings_create(uint32_t targets, uint32_t custom_targets);
void es_mappings_destroy(EsMappings *);
void es_mappings_reset(EsMappings *);
/* Native channel layout is [source channel][route], or NULL for unchanged routing. */
bool es_mappings_inspect(EsMappings *, const DmeContext *, const uint8_t *native_channels,
                         uint32_t routes, char *, size_t);
size_t es_mappings_count(const EsMappings *);
const EsSourceRow *es_mappings_source(const EsMappings *, size_t index);
const DmeCustomMapping *es_mappings_describe(EsMappings *, uint32_t target, size_t index);
int32_t es_mappings_set(EsMappings *, uint32_t target, const char *row, const DmeCustomSelection *,
                        char *, size_t);
bool es_mappings_targets(EsMappings *, uint32_t target, const DmeSampleTarget *, size_t, char *,
                         size_t);
bool es_mappings_stock_id(const EsMappings *, const char *, char out[64]);
bool es_mappings_set_stock(EsMappings *, uint32_t, const char *, int32_t);
int32_t es_mappings_stock(EsMappings *, uint32_t, DmeMappingSource, int32_t);
bool es_mapping_job_values(const EsMappingJob *, uint32_t, const DmeContext *, int32_t *values,
                           char *, size_t);
bool es_mapping_row_id(const DmeContext *, uint64_t, char out[64]);
bool es_mapping_job_set_stock(EsMappingJob *, uint32_t, const char *, int32_t);
EsMappingJob *es_mappings_freeze(const EsMappings *, char *, size_t);
void es_mapping_job_free(EsMappingJob *);
const DmeCustomSelection *es_mapping_job_selection(const EsMappingJob *, uint32_t target,
                                                   DmeMappingSource source);
bool es_mapping_job_custom(const EsMappingJob *, uint32_t target);

#endif
