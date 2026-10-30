#ifndef M64_PLUGIN_H
#define M64_PLUGIN_H
#include "duckmidi_export_plugin.h"
#include "m64export/m64export.h"
#include <stdbool.h>
typedef struct {
    char sequence[97],bank[97];
    char custom_sequence[97];
    int sequence_choice,loop,percussion;
    int melodic[16][128],drums[128];
    bool used[16][128],used_drums[128],session_known,custom_sequence_set;
    uint64_t session;
    char program_names[128][80],drum_names[128][80];
    DmeField fields[8];
    DmeChoice choices[128];
    char choice_labels[128][220];
    DmeMappingRow rows[2048];
    char row_ids[2048][40],row_labels[2048][180];
} M64PluginUi;
typedef struct M64ReviewUi M64ReviewUi;
typedef struct {
    M64Request request;
    char sequence[97],bank[97];
    M64Instrument melodic[16*128],drums[128];
    int presets[16][128],drum_presets[128];
    M64ReviewUi *review_ui;
} M64PluginJob;
void *DME_CALL m64_ui_create(void);
void DME_CALL m64_ui_destroy(void *);
int32_t DME_CALL m64_ui_inspect(void *,const DmeContext *,char *,uint64_t);
int32_t DME_CALL m64_ui_describe(void *,DmeUi *);
int32_t DME_CALL m64_ui_set(void *,const char *,const DmeValue *,char *,uint64_t);
void DME_CALL m64_ui_reset(void *);
bool m64_review_build(M64PluginJob *,const DmeContext *,const M64Result *);
void m64_review_free(M64PluginJob *);
int32_t DME_CALL m64_review_describe(void *,DmeReview *);
int32_t DME_CALL m64_review_set(void *,const char *,int32_t,char *,uint64_t);
int32_t DME_CALL m64_review_apply(void *,void *,char *,uint64_t);
#endif
