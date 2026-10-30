#ifndef DM_EDITOR_EXPORT_INTERNAL_H
#define DM_EDITOR_EXPORT_INTERNAL_H
#include "editor_internal.h"
#include "export_plugins.h"
typedef struct {
    char name[256];
    DmBytes data;
} DmPluginArtifact;
typedef struct {
    SDL_Thread *thread;
    SDL_AtomicInt cancel,done,progress;
    bool inspection,success,review,prepared;
    uint64_t revision,session;
    int format_index;
    const DmeExporter *api;
    void *job;
    DmBytes snapshot;
    DmeEvent *events;
    DmeContext context;
    char project_name[128],source_name[DM_PATH],destination[DM_PATH];
    char message[8192],summary[4096];
    char popup[16384];
    bool popup_truncated;
    unsigned warnings;
    DmPluginArtifact artifacts[64];
    size_t artifact_count,total_bytes;
} DmPluginWork;
typedef struct DmExportPlugins {
    DmExportRegistry registry;
    DmPluginWork work;
    DmeContext context;
    DmeEvent *events;
    bool context_valid;
    bool inspection_failed;
    uint64_t failed_revision,failed_session;
    uint64_t revision,session;
    char project_name[128],source_name[DM_PATH],message[8192];
    bool warning_open;
    bool review_pending;
    int review_selected;
    char warning[24576];
    uint64_t inspected[DM_EXPORT_MAX_FORMATS];
    int pending_format;
    int message_format;
    SDL_DialogFileFilter filter;
    char suggested[DM_PATH];
} DmExportPlugins;
DmExportPlugins *ed_export_host(Editor *e);
bool ed_export_context(Editor *e);
DmExportFormat *ed_export_selected(Editor *e);
void ed_export_render_fields(Editor *e,DmExportFormat *format,const DmeUi *ui);
#endif
