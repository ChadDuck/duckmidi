#ifndef DM_PATTERN_GENERATION_H
#define DM_PATTERN_GENERATION_H
#include "../src/core/duck.h"

typedef struct {
    size_t note_patterns, automation_patterns, placements, linked_placements, notes;
} DmGenerationStats;

typedef struct {
    void *user;
    bool (*cancelled)(void *);
    void (*progress)(void *, unsigned);
} DmGenerationHooks;

/* What a run regenerates. */
typedef struct {
    int channel; /* 0..15, or -1 for every channel */
} DmGenerationScope;

/* Source and initialized output are independent; failure leaves output intact. A NULL scope
   regenerates every channel. With one channel, only its note placements are replaced; the other
   channels keep their placements, patterns, automation and subrow names, and a source pattern
   still placed on another channel is kept whole. */
bool dm_generate_patterns(const DmProject *source, DmProject *output,
                          const DmGenerationScope *scope, const DmGenerationHooks *hooks,
                          DmGenerationStats *stats, char *error, size_t cap);
/* True when the project has duckmidi edits a plain MIDI import never has, after which the
   generated patterns may sound different (the Generate Patterns window warns). */
bool dm_generation_changes_sound(const DmProject *p);
#endif
