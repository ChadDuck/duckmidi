#ifndef DM_PROJECT_INTERNAL_H
#define DM_PROJECT_INTERNAL_H

#include "core/duck.h"

/* Real numbers that are exact multiples of 1/DM_REAL_STEPS encode as compact varints. */
#define DM_REAL_STEPS 3840.0

typedef struct {
    const uint8_t *p;
    size_t left;
    bool ok;
    uint32_t schema; /* Schema of the document being read. */
} Input;

/* Shared codec primitives (project_codec.c). */
bool dm_put_fixed32(DmBytes *b, uint32_t n);
bool dm_put_varint(DmBytes *b, uint64_t n);
bool dm_put_real(DmBytes *b, double n);
bool dm_exact_steps(double n, int64_t *steps);
bool dm_put_time_after(DmBytes *b, double n, double previous);
bool dm_put_block(DmBytes *b, const void *data, size_t size);
bool dm_put_string(DmBytes *b, const char *s);

uint32_t dm_get_fixed32(Input *in);
uint64_t dm_get_varint(Input *in);
double dm_get_real10(Input *in);
double dm_get_time_after10(Input *in, double previous);

uint32_t dm_crc32(const uint8_t *p, size_t size);

/* Shared single-record validation checks (project_checks.c). */
bool dm_point_only_event(int event);
bool dm_graph_resolution_valid(uint32_t graph);
bool dm_pattern_settings_valid(const DmPattern *p);
bool dm_note_fields_valid(uint32_t key, uint32_t velocity, uint32_t selected, uint32_t port);
bool dm_channel_settings_valid(const DmChannel *c);
bool dm_note_rows_valid(int rows);
bool dm_lane_count_valid(int count);
bool dm_metadata_valid(const DmProject *p);
bool dm_percussion_valid(const DmProject *p);
bool dm_port_mix_valid(DmPortMix mix);
bool dm_automation_fields_valid(DmAutomation a);

/* The tagged body of schema 20 and later (project_fields.c). dm_read_body sets *newer when it
   met a tag this build does not know. */
bool dm_write_body(const DmProject *p, DmBytes *out);
bool dm_read_body(DmProject *p, Input *in, bool allow_views, bool *newer);

/* dm_automation_add without its pattern requirement, for readers of schemas before 21 (core.c). */
bool dm_automation_insert(DmProject *p, DmAutomation a);

/* Frozen legacy reader for schemas 9-19 (project_legacy.c). */
bool dm_read_body_legacy(DmProject *p, Input *in, bool allow_views);

/* Document unpacking and view lookup (project.c). */
bool dm_unpack_document(DmProject *p, const unsigned char *data, size_t size, char *error,
                        size_t cap, bool allow_views);
int dm_view_index(const DmProject *p, int port, int sequence);
bool dm_valid_view_key(const DmProject *p, int port, int sequence);
/* Whether a stored view snapshot unpacks and matches its key and the parent's source layout. */
bool dm_validate_view(const DmProject *parent, const DmView *view);

#endif
