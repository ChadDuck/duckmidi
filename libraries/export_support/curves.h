#ifndef ES_CURVES_H
#define ES_CURVES_H

/* Controller curve simplification shared by the game exporters. Songs that pass a game's
   sequence limit lose controller points before any note changes: first within audible error
   bounds, densest curves first, then by spacing changes apart. */
#include "export_support.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ES_CURVE_FIXED,  /* Never removed (instruments, reverb and anything else). */
    ES_CURVE_VOLUME, /* Volume or expression, 0-127: error measured in decibels. */
    ES_CURVE_PAN,    /* Pan, 0-127: error in pan steps. */
    ES_CURVE_BEND,   /* Pitch bend in cents. */
} EsCurveKind;

typedef struct {
    uint64_t tick;  /* Game ticks; points are in tick order. */
    int32_t value;  /* In the kind's units. */
    uint32_t curve; /* Points with the same curve form one controller of one channel. */
    uint8_t kind;   /* EsCurveKind */
} EsCurvePoint;

enum { ES_CURVE_STEPS = 7, ES_CURVE_SPACINGS = 6 };

/* Marks in keep[] the points to write so that at most `budget` changes remain, raising the
   error tolerance of the densest curves first, up to audible caps (1 dB, 6 pan steps, 10 cents).
   `spacing` (index 0-5: 1, 2, 3, 4, 6, 8 ticks) also keeps changes that far apart. The last
   value before a pause of `settle` ticks or more is always kept, so ramps land exactly. Fixed
   points are always kept. Returns the number of changes kept, or SIZE_MAX without memory. */
DES_API size_t es_thin_curves(const EsCurvePoint *, size_t count, size_t budget, unsigned spacing,
                              uint64_t settle, bool *keep);

/* Renders the song keeping the points marked in keep[]. Returns 1 when it fits, 0 when it does
   not, or -1 to stop (no memory or cancelled); *bytes receives its size either way. */
typedef int (*EsCurveRender)(void *user, const bool *keep, uint64_t *bytes);

/* Finds the most points that still fit: estimates a budget from the bytes each removed change
   saves, then narrows between the best fitting and the smallest failing budget. `bytes` is the
   size with every point and `limit` the size to aim for. On 1 the last render used keep[].
   Returns 1 when it fits, 0 when even the widest spacing does not, -1 when stopped. */
DES_API int es_fit_curves(const EsCurvePoint *, size_t count, uint64_t bytes, uint64_t limit,
                          uint64_t settle, EsCurveRender, void *user, bool *keep);

#endif
