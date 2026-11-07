#ifndef M64_CUSTOM_H
#define M64_CUSTOM_H

#include "m64export/m64export.h"
#include "../../export_support/sample.h"

#define M64_CUSTOM_STOCK_BASE (4096 + ES_NATIVE_REGIONS * 256)

/* Bundled plugin extension; the independently released converter ABI remains version 3. */
typedef struct {
    const EsNativeSamples *samples;
    const M64Instrument *definitions;
    const uint16_t *source_programs; /* Parallel source events; UINT16_MAX keeps MIDI state. */
    const int32_t *stock_values;     /* Per original source row, -1 retains the request mapping. */
    const DmeContext *source_context;
    const char *sample_bank;
    /* Optional, one per region: when set, notes above the 4x rate limit record how far over
       they are here instead of failing. The output is then not valid until recompiled. */
    double *overspeed;
    bool *capacity; /* Optional, per region: bank capacity requires an ordinary-row fallback. */
} M64Custom;

int32_t m64_compile_custom(const M64Request *, const M64Hooks *, const M64Custom *, bool analyze,
                           M64Result **);
bool m64_range_origin(const M64Result *, uint64_t, uint64_t *);

#endif
