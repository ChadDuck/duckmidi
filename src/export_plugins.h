#ifndef DM_EXPORT_PLUGINS_H
#define DM_EXPORT_PLUGINS_H
#include "../include/duckmidi_export_plugin.h"
#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL.h>
#define DM_EXPORT_MAX_FORMATS 64

typedef struct {
    const DmeExporter *api;
    void *instance;
    int selected_rows[64];
} DmExportFormat;

typedef struct {
    SDL_SharedObject *modules[64];
    size_t module_count;
    DmExportFormat formats[DM_EXPORT_MAX_FORMATS];
    size_t count;
    char errors[4096];
} DmExportRegistry;

void dm_export_registry_load(DmExportRegistry *registry, const char *directory);
void dm_export_registry_free(DmExportRegistry *registry);
bool dm_export_ui_valid(const DmeUi *ui, char *error, size_t capacity);
bool dm_export_review_valid(const DmeReview *review, char *error, size_t capacity);
#endif
