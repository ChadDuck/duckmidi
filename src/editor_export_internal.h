#ifndef DM_EDITOR_EXPORT_INTERNAL_H
#define DM_EDITOR_EXPORT_INTERNAL_H
#include "editor_internal.h"
#include "export_plugins.h"
#include "../shared/sample_export_snapshot.h"
#include "../shared/sample_export_catalog.h"

typedef struct {
    char *name;
    DmBytes data;
} DmPluginArtifact;

typedef struct {
    DmPatchReplacement selection;
    DmSampleExportSnapshot *snapshot;
    DmeContext context;
    uint64_t handle;
} DmFrozenExportSample;

typedef struct {
    SDL_Thread *thread;
    SDL_AtomicInt cancel, done, progress;
    bool inspection, catalog_only, success, review, prepared;
    bool install, keep_originals; /* Repository install, and whether originals are kept. */
    uint64_t revision, session;
    uint64_t catalog_revision;
    int format_index;
    const DmeExporter *api;
    void *job;
    const DmeHistoryRuntime *history_runtime; /* Set for repository actions and installs. */
    void *repository_job;
    uint32_t repository_action;
    DmBytes snapshot;
    DmeEvent *events;
    DmeContext context;
    DmSampleExportSnapshot *samples;
    DmSampleExportCatalog *catalog;
    DmFrozenExportSample *frozen;
    size_t frozen_count, frozen_capacity, frozen_values;
    char catalog_program[DM_PATH], catalog_writable[DM_PATH], plugins_directory[DM_PATH];
    char project_name[128], source_name[DM_PATH], destination[DM_PATH];
    char message[8192], summary[4096];
    char popup[8192]; /* Warnings that are not list items. */

    struct {
        char heading[96];
        char items[2048]; /* "- item\n" lines. */
    } groups[12];         /* DME_LIST warnings, in the order their headings first arrived. */

    unsigned group_count;
    bool popup_truncated, altered;
    unsigned warnings;
    DmPluginArtifact *artifacts;
    size_t artifact_count, artifact_capacity, total_bytes;
} DmPluginWork;

typedef struct DmExportPlugins {
    DmExportRegistry registry;
    DmPluginWork work;
    DmPluginWork catalog_work; /* Background discovery does not block export controls or jobs. */
    DmeContext context;
    DmSampleExportCatalog *catalog;
    DmSampleExportSnapshot *identities;
    DmeEvent *events;
    bool context_valid;
    bool inspection_failed;
    uint64_t failed_revision, failed_session;
    uint64_t revision, session;
    uint64_t catalog_revision;
    bool catalog_dirty;
    char project_name[128], source_name[DM_PATH], message[8192];
    bool warning_open;
    bool review_pending;
    int review_selected;
    char warning[24576];
    uint64_t inspected[DM_EXPORT_MAX_FORMATS];
    int pending_format;
    int message_format;
    SDL_DialogFileFilter filter;

    /* Export to Repository folders by exporter ID, kept in duckmidi-plugins.ini. Entries of
       exporters that are not installed are kept too. */
    struct {
        char id[97], repository[DM_PATH];
    } folders[DM_EXPORT_MAX_FORMATS];

    size_t folder_count;
    int folder_format;   /* Exporter whose folder the Preferences picker sets, or -1. */
    bool keep_originals; /* Keep original files; on until the user turns it off. */
    char suggested[DM_PATH];
    bool history_open;
    float history_width;
    char history_key[520]; /* Logical key of the selected history row, or empty. */
    int history_confirm;
} DmExportPlugins;

DmExportPlugins *ed_export_host(Editor *e);
bool ed_export_context(Editor *e);
DmExportFormat *ed_export_selected(Editor *e);
void ed_export_render_fields(Editor *e, DmExportFormat *format, const DmeUi *ui);
void ed_export_history_toggle(Editor *e);
void ed_export_history_panel(Editor *e, UiRect main);
/* Drops every history view so the drawer inspects again, after history is turned on or off. */
void ed_export_history_reset(Editor *e);
bool ed_export_repository_action(Editor *, uint32_t action, uint64_t scope, const char *key);
#endif
