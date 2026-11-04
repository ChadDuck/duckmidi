#ifndef DM_EDITOR_EXPORT_INTERNAL_H
#define DM_EDITOR_EXPORT_INTERNAL_H
#include "editor_internal.h"
#include "export_plugins.h"

typedef struct {
    char name[256];
    DmBytes data;
} DmPluginArtifact;

/* A file read from, or queued for, a folder install. Paths are relative and use "/". */
typedef struct {
    char path[256];
    DmBytes data;
    uint32_t flags;
} DmPluginFile;

/* What a folder install changed, so it can be reverted. */
typedef struct {
    char path[DM_PATH];
    char created[DM_PATH]; /* Highest folder the install created for this file, or empty. */
    DmBytes original;
    bool existed, backup_created;
} DmRepositoryChange;

enum { DM_PLUGIN_FILES = 64 }; /* Files an install may read and queue. */

typedef struct {
    SDL_Thread *thread;
    SDL_AtomicInt cancel, done, progress;
    bool inspection, success, review, prepared;
    bool install, keep_originals; /* Folder install, and whether every original is kept. */
    DmPluginFile reads[DM_PLUGIN_FILES], installs[DM_PLUGIN_FILES];
    size_t read_count, install_count;
    DmRepositoryChange *changes;
    size_t change_count;
    uint64_t revision, session;
    int format_index;
    const DmeExporter *api;
    void *job;
    DmBytes snapshot;
    DmeEvent *events;
    DmeContext context;
    char project_name[128], source_name[DM_PATH], destination[DM_PATH];
    char message[8192], summary[4096];
    char popup[16384];
    bool popup_truncated;
    unsigned warnings;
    DmPluginArtifact artifacts[64];
    size_t artifact_count, total_bytes;
} DmPluginWork;

typedef struct DmExportPlugins {
    DmExportRegistry registry;
    DmPluginWork work;
    DmeContext context;
    DmeEvent *events;
    bool context_valid;
    bool inspection_failed;
    uint64_t failed_revision, failed_session;
    uint64_t revision, session;
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
    int folder_format;           /* Exporter whose folder the Preferences picker sets, or -1. */
    bool keep_originals;         /* Keep original files; on until the user turns it off. */
    DmRepositoryChange *changes; /* Changes made to repository since the last revert. */
    size_t change_count;
    char changed_repository[DM_PATH];
    char suggested[DM_PATH];
} DmExportPlugins;

DmExportPlugins *ed_export_host(Editor *e);
bool ed_export_context(Editor *e);
DmExportFormat *ed_export_selected(Editor *e);
void ed_export_render_fields(Editor *e, DmExportFormat *format, const DmeUi *ui);
#endif
