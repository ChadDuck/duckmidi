#ifndef DUCK_EXPORT_SUPPORT_PORTS_H
#define DUCK_EXPORT_SUPPORT_PORTS_H

#include "../../include/duckmidi_export_plugin.h"
#include "export_support.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ES_PORTS 128

/* Where each colored port's channel messages play in a native export; see ports.c. */
typedef struct {
    uint8_t out[16][ES_PORTS]; /* Output channel by source channel and port. */
    bool merged, interference;
    unsigned moved_count, conflict_count;
    char notice[768];
    char merges[768]; /* One "where: control" line per interfering merge. */
} EsPortLayout;

DES_API bool es_port_layout(const DmeEvent *events, uint64_t count, uint32_t percussion,
                            EsPortLayout *layout);

/* The loop the context's markers ask for, in ticks. A missing start is the beginning; an end of 0
   means the end of the song. Fails with a message when the start is not before the end. */
DES_API bool es_loop_region(const DmeContext *context, uint64_t *start, uint64_t *end, char *error,
                            size_t cap);

#ifdef __cplusplus
}
#endif

#endif
