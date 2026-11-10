#include "sample_export_catalog.h"
#include "../src/duck.h"
#include "../src/text/en.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DmSampleManifest manifest;
    char root[DM_PATH], error[256];
} CatalogSet;

struct DmSampleExportCatalog {
    CatalogSet **retained;
    DmeSampleCatalogSet *sets;
    DmeSampleCatalogPreset *presets;
    DmeSampleZoneParams *zones;
    size_t preset_capacity, zone_capacity;
    DmeSampleCatalog view;
};

static DmeString view(const char *text) {
    return (DmeString){text, strlen(text)};
}

static bool canceled(void *user) {
    const DmeHost *host = user;
    return host && host->cancelled && host->cancelled(host->user);
}

bool dm_sample_library_roots(const char *base, const char *settings, char *program,
                              size_t program_cap, char *writable, size_t writable_cap) {
    const char *leaf = settings + strlen(settings);

    while (leaf > settings && leaf[-1] != '/' && leaf[-1] != '\\') {
        --leaf;
    }

    size_t folder = (size_t)(leaf - settings);
    int a = snprintf(program, program_cap, "%ssamples", base);
    int b = snprintf(writable, writable_cap, "%.*ssamples", (int)folder, settings);

    if (!folder || a < 0 || (size_t)a >= program_cap || b < 0 || (size_t)b >= writable_cap) {
        return false;
    }

#ifdef _WIN32
    if (!strcmp(program, writable)) {
        char *prefs = SDL_GetPrefPath(NULL, "duckmidi");
        char fallback[DM_PATH];
        SDL_PathInfo info;

        if (prefs && snprintf(fallback, sizeof(fallback), "%ssamples", prefs) < (int)sizeof(fallback) &&
            SDL_GetPathInfo(fallback, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
            b = snprintf(writable, writable_cap, "%s", fallback);
        }

        SDL_free(prefs);
        return b >= 0 && (size_t)b < writable_cap;
    }
#endif
    return true;
}

bool dm_sample_export_catalog(const char *program, const char *writable, const DmeHost *host,
                               DmSampleExportCatalog **out, char *error, size_t cap) {
    DmSampleCatalog catalog = {0};
    DmSampleImportCallbacks callbacks = {(void *)host, canceled, NULL};

    if (!dm_sample_library_catalog(program, writable, &callbacks, &catalog, error, cap)) {
        return false;
    }

    DmSampleExportCatalog *result = calloc(1, sizeof(*result));

    if (result) {
        result->sets = calloc(catalog.count ? catalog.count : 1, sizeof(*result->sets));
        result->retained = calloc(catalog.count ? catalog.count : 1, sizeof(*result->retained));
        result->view.size = sizeof(result->view);
    }

    bool ok = result && result->sets && result->retained;

    for (size_t i = 0; ok && i < catalog.count; ++i) {
        const DmSampleCatalogEntry *entry = &catalog.entries[i];
        CatalogSet *set = calloc(1, sizeof(*set));

        if (!set) {
            ok = false;
            break;
        }

        result->retained[i] = set;
        result->view.set_count++;
        snprintf(set->root, sizeof(set->root), "%s", entry->writable_root ? writable : program);
        DmSampleLibraryReader *reader = dm_sample_library_reader_open_in(
            set->root, entry->directory, entry->id, &set->manifest, set->error, sizeof(set->error));
        bool available = reader != NULL;
        dm_sample_library_reader_close(reader);
        DmSampleManifest *m = &set->manifest;
        available = available && m->generation == entry->generation;

        if (!available && !*set->error) {
            snprintf(set->error, sizeof(set->error), "%s", DM_TEXT_SAMPLE_MANIFEST_INVALID);
        }

        /* Unavailable entries retain their stable catalog identity and an explicit error. */
        if (!available) {
            dm_sample_manifest_free(m);
            snprintf(m->id, sizeof(m->id), "%s", entry->id);
            snprintf(m->name, sizeof(m->name), "%s", entry->name);
        }

        result->sets[i] = (DmeSampleCatalogSet){.size = sizeof(DmeSampleCatalogSet),
                                                .generation = entry->generation,
                                                .available = available,
                                                .modified = entry->modified,
                                                .locator = view(set->root),
                                                .id = view(m->id),
                                                .name = view(m->name),
                                                .error = view(set->error),
                                                .first_preset = result->view.preset_count,
                                                .preset_count = m->preset_count};

        if (m->preset_count > 8192 - result->view.preset_count ||
            m->zone_count > 32768 - result->view.zone_count ||
            !dm_reserve((void **)&result->presets, &result->preset_capacity,
                         result->view.preset_count + m->preset_count, sizeof(*result->presets)) ||
            !dm_reserve((void **)&result->zones, &result->zone_capacity,
                         result->view.zone_count + m->zone_count, sizeof(*result->zones))) {
            ok = false;
            break;
        }

        for (uint32_t p = 0; p < m->preset_count; ++p) {
            const DmSamplePreset *preset = &m->presets[p];
            result->presets[result->view.preset_count++] = (DmeSampleCatalogPreset){
                .size = sizeof(DmeSampleCatalogPreset), .set = (uint32_t)i,
                .flags = preset->drums ? DME_SAMPLE_PERCUSSION : 0,
                .suggested_program = preset->suggested_program,
                .first_zone = result->view.zone_count + preset->first_zone,
                .zone_count = preset->zone_count, .id = view(preset->id), .name = view(preset->name)};
        }

        for (uint32_t z = 0; z < m->zone_count; ++z) {
            result->zones[result->view.zone_count++] = dm_sample_zone_params(&m->zones[z]);
        }

        ok = !canceled((void *)host);
    }

    dm_sample_catalog_free(&catalog);

    if (!ok) {
        dm_sample_export_catalog_free(result);
        snprintf(error, cap, "%s", canceled((void *)host) ? DM_TEXT_SAMPLE_IMPORT_CANCELED
                                                         : DM_TEXT_SAMPLE_IMPORT_MEMORY);
        return false;
    }

    result->view.sets = result->sets;
    result->view.presets = result->presets;
    result->view.zones = result->zones;
    *out = result;
    return true;
}

const DmeSampleCatalog *dm_sample_export_catalog_view(const DmSampleExportCatalog *catalog) {
    return catalog ? &catalog->view : NULL;
}

static bool string_equal(DmeString a, DmeString b) {
    return a.length == b.length && (!a.length || !memcmp(a.data, b.data, (size_t)a.length));
}

bool dm_sample_export_catalog_equal(const DmSampleExportCatalog *a, const DmSampleExportCatalog *b) {
    const DmeSampleCatalog *left = dm_sample_export_catalog_view(a);
    const DmeSampleCatalog *right = dm_sample_export_catalog_view(b);

    if (!left || !right || left->set_count != right->set_count ||
        left->preset_count != right->preset_count || left->zone_count != right->zone_count) {
        return left == right;
    }

    for (uint32_t i = 0; i < left->set_count; i++) {
        const DmeSampleCatalogSet *x = &left->sets[i], *y = &right->sets[i];

        if (x->generation != y->generation || x->modified != y->modified ||
            x->available != y->available || x->preset_count != y->preset_count ||
            !string_equal(x->id, y->id) || !string_equal(x->locator, y->locator) ||
            !string_equal(x->name, y->name) || !string_equal(x->error, y->error)) {
            return false;
        }
    }

    return true;
}

void dm_sample_export_catalog_free(DmSampleExportCatalog *catalog) {
    if (catalog) {
        for (uint32_t i = 0; i < catalog->view.set_count; ++i) {
            dm_sample_manifest_free(&catalog->retained[i]->manifest);
            free(catalog->retained[i]);
        }

        free(catalog->retained);
        free(catalog->sets);
        free(catalog->presets);
        free(catalog->zones);
        free(catalog);
    }
}
