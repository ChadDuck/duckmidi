#ifndef OOT_PLUGIN_H
#define OOT_PLUGIN_H
#include "../src/oot.h"
#include "../../export_support/mapping.h"
#include "../../export_support/sample.h"
#include "../../export_support/ports.h"
#include <stdbool.h>
#include <stddef.h>

#define OOT_CUSTOM_CHOICE 1000 /* Soundfont choice value for a custom soundfont */
#define OOT_NAME_CAPACITY 49

typedef struct {
    int song; /* 0 adds a new song; n replaces oot_songs[n - 1]. */
    EsMappings *custom;
    char checkout[DME_PATH_LIMIT], checkout_identity[96];
    int font; /* OOT_CUSTOM_CHOICE or a vanilla soundfont index */
    char name[OOT_NAME_CAPACITY];
    char project_name[OOT_NAME_CAPACITY]; /* Default sequence name from the project. */
    int volume, loop, loop_points, percussion;
    int memory; /* Sequence memory: 0 Enlarge, 1 Stock. */
    bool has_loop_points, session_known;
    bool name_set;      /* The user typed a sequence name; otherwise it follows the project. */
    bool similar_stale; /* similar[] must be rebuilt before the next describe. */
    uint64_t session;
    int melodic[16][128], drums[128];
    bool used[16][128], used_drums[128];
    uint8_t low[16][128], high[16][128]; /* Key range each used source plays. */
    char program_names[128][80], drum_names[128][80];
    DmeField fields[14];
    DmeChoice song_choices[80], font_choices[48], choices[128];
    char song_labels[80][80], font_labels[48][64], choice_labels[128][96];
    DmeMappingRow rows[DME_ROW_LIMIT];
    char row_ids[DME_ROW_LIMIT][40], row_labels[DME_ROW_LIMIT][180];
    DmeChoice similar[DME_ROW_LIMIT][8];
    unsigned similar_count[DME_ROW_LIMIT];
} OotPluginUi;

typedef struct OotReviewUi OotReviewUi;

typedef struct {
    OotRequest request;
    EsMappingJob *custom;
    EsNativeSamples samples;
    EsRepository *repository;
    char checkout[DME_PATH_LIMIT], checkout_identity[96];
    char name[OOT_NAME_CAPACITY];
    char sample_bank[DME_PATH_LIMIT];
    const char *song; /* NA_BGM_* to replace, or NULL to add a song */
    bool loop_points, enlarge;
    bool keep_originals;
    char port_notice[768];
    DmeEvent *events;
    int32_t *stock_values;
    OotReviewUi *review_ui;
    OotText spec_cleaned; /* Install: the spec without removed songs. */
} OotPluginJob;

void *DME_CALL oot_ui_create(void);
void DME_CALL oot_ui_destroy(void *);
int32_t DME_CALL oot_ui_inspect(void *, const DmeContext *, char *, uint64_t);
int32_t DME_CALL oot_ui_describe(void *, DmeUi *);
int32_t DME_CALL oot_ui_set(void *, const char *, const DmeValue *, char *, uint64_t);
void DME_CALL oot_ui_reset(void *);
int oot_ui_font(const OotPluginUi *);

/* Up to capacity choices closest to original that reach the issue's keys, best first. */
unsigned oot_similar(int font, const OotRangeIssue *issue, int original, const DmeChoice *choices,
                     unsigned count, DmeChoice *out, unsigned capacity);
bool oot_review_build(OotPluginJob *, const DmeContext *, const OotResult *);
void oot_review_free(OotPluginJob *);
int32_t DME_CALL oot_review_describe(void *, DmeReview *);
int32_t DME_CALL oot_review_set(void *, const char *, int32_t, char *, uint64_t);
int32_t DME_CALL oot_review_apply(void *, void *, char *, uint64_t);
#endif
