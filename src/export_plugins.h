#ifndef DM_EXPORT_PLUGINS_H
#define DM_EXPORT_PLUGINS_H
#include "../include/duckmidi_export_plugin.h"
#include "../include/duckmidi_repository_plugin.h"
#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL.h>
#define DM_EXPORT_MAX_FORMATS 64
#define DM_EXPORT_MAX_TOOLS 32
/* File name, without extension, of the history runtime beside the plugins. */
#define DM_HISTORY_RUNTIME_NAME "db"

typedef struct {
    const DmeExporter *api;
    const DmeHistoryAdapter *history; /* NULL without repository installs. */
    void *instance;
    void *history_job;
    uint64_t history_scope;
    int selected_rows[64];
} DmExportFormat;

typedef struct {
    const DmeTool *api;
    void *instance;
    bool open_requested, opening;
} DmNativeTool;

typedef struct {
    SDL_SharedObject *modules[64];
    SDL_SharedObject *dependencies[64];
    const DmePlugin *plugins[64];
    size_t module_count;
    size_t dependency_count;
    DmExportFormat formats[DM_EXPORT_MAX_FORMATS];
    size_t count;
    DmNativeTool tools[DM_EXPORT_MAX_TOOLS];
    size_t tool_count;
    SDL_ThreadID main_thread;
    DmeMainHost main_host;
    const DmeHistoryRuntime *history_runtime; /* From plugins/db; NULL when absent. */
    char directory[4096];
    char errors[4096];
} DmExportRegistry;

void dm_export_registry_load(DmExportRegistry *registry, const char *directory);
void dm_export_registry_free(DmExportRegistry *registry);
DmNativeTool *dm_export_registry_tool(DmExportRegistry *registry, const char *id);
int32_t dm_export_registry_request_tool(DmExportRegistry *registry, const char *id);
bool dm_export_ui_valid(const DmeUi *ui, char *error, size_t capacity);
bool dm_export_review_valid(const DmeReview *review, char *error, size_t capacity);
#endif
