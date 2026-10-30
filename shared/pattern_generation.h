#ifndef DM_PATTERN_GENERATION_H
#define DM_PATTERN_GENERATION_H
#include "../src/duck.h"
typedef struct {size_t note_patterns,automation_patterns,placements,linked_placements,notes;} DmGenerationStats;
typedef struct {void *user;bool (*cancelled)(void *);void (*progress)(void *,unsigned);} DmGenerationHooks;
/* Source and initialized output are independent; failure leaves output intact. */
bool dm_generate_patterns(const DmProject *source,DmProject *output,const DmGenerationHooks *hooks,DmGenerationStats *stats,char *error,size_t cap);
#endif
