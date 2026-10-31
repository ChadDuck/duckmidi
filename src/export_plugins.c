#include "export_plugins.h"
#include "text/en.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool text(const char *s, size_t max) {
    return s && *s && SDL_strnlen(s, max + 1) <= max;
}

static bool identifier(const char *s) {
    if (!text(s, 96)) {
        return false;
    }

    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              strchr("._/-", *s))) {
            return false;
        }
    }

    return true;
}

static bool choices(const DmeField *f, int value) {
    for (uint32_t i = 0; i < f->choice_count; i++) {
        if (f->choices[i].value == value) {
            return true;
        }
    }

    return false;
}

bool dm_export_ui_valid(const DmeUi *ui, char *error, size_t cap) {
    const char *why = DM_TEXT_INVALID_PLUGIN_SETTINGS_SCHEMA;

    if (!ui || ui->count > 64 || (ui->count && !ui->fields)) {
        goto fail;
    }

    for (uint32_t i = 0; i < ui->count; i++) {
        const DmeField *f = &ui->fields[i];

        if (!identifier(f->id) || !text(f->label, 512) || f->type > DME_ACTION) {
            goto fail;
        }

        for (uint32_t j = 0; j < i; j++) {
            if (!strcmp(f->id, ui->fields[j].id)) {
                why = DM_TEXT_DUPLICATE_PLUGIN_FIELD_ID;
                goto fail;
            }
        }

        if (f->type == DME_BOOL &&
            (f->value.type != DME_BOOL || f->value.integer < 0 || f->value.integer > 1)) {
            goto fail;
        }

        if (f->type == DME_INT &&
            (f->value.type != DME_INT || f->minimum > f->maximum || f->value.integer < f->minimum ||
             f->value.integer > f->maximum)) {
            goto fail;
        }

        if (f->type == DME_TEXT &&
            (f->value.type != DME_TEXT || !f->text_capacity || f->text_capacity > 4096 ||
             !f->value.text || SDL_strnlen(f->value.text, f->text_capacity) >= f->text_capacity)) {
            goto fail;
        }

        if (f->type == DME_CHOICE || f->type == DME_MAPPING) {
            if (!f->choice_count || f->choice_count > 512 || !f->choices) {
                goto fail;
            }

            for (uint32_t j = 0; j < f->choice_count; j++) {
                if (!text(f->choices[j].label, 512)) {
                    goto fail;
                }

                for (uint32_t k = 0; k < j; k++) {
                    if (f->choices[j].value == f->choices[k].value) {
                        goto fail;
                    }
                }
            }

            if (f->type == DME_CHOICE &&
                (f->value.type != DME_CHOICE || !choices(f, f->value.integer))) {
                goto fail;
            }
        }

        if (f->type == DME_MAPPING) {
            if (f->row_count > 4096 || (f->row_count && !f->rows)) {
                goto fail;
            }

            for (uint32_t j = 0; j < f->row_count; j++) {
                const DmeMappingRow *r = &f->rows[j];

                if (!identifier(r->id) || !text(r->label, 512) || !choices(f, r->value) ||
                    !choices(f, r->default_value)) {
                    goto fail;
                }

                for (uint32_t k = 0; k < j; k++) {
                    if (!strcmp(r->id, f->rows[k].id)) {
                        goto fail;
                    }
                }

                for (uint32_t k = 0; k < ui->count; k++) {
                    if (!strcmp(r->id, ui->fields[k].id)) {
                        goto fail;
                    }
                }
            }
        }
    }

    return true;
fail:
    snprintf(error, cap, "%s", why);
    return false;
}

bool dm_export_review_valid(const DmeReview *r, char *error, size_t cap) {
    if (!r || r->count > 2048 || (r->count && (!r->rows || !text(r->description, 4096)))) {
        goto fail;
    }

    for (uint32_t i = 0; i < r->count; i++) {
        const DmeReviewRow *row = &r->rows[i];

        if (!identifier(row->id) || !text(row->label, 512) || !text(row->description, 4096) ||
            !text(row->current_mapping, 512) || row->supported < 0 || row->supported > 1 ||
            !row->choices || !row->choice_count || row->choice_count > 512 ||
            row->recommended_count > row->choice_count ||
            (row->recommended_count && !row->recommended)) {
            goto fail;
        }

        for (uint32_t j = 0; j < i; j++) {
            if (!strcmp(row->id, r->rows[j].id)) {
                goto fail;
            }
        }

        bool found = false;

        for (uint32_t j = 0; j < row->choice_count; j++) {
            if (!text(row->choices[j].label, 512)) {
                goto fail;
            }

            if (row->choices[j].value == row->value) {
                found = true;
            }

            for (uint32_t k = 0; k < j; k++) {
                if (row->choices[j].value == row->choices[k].value) {
                    goto fail;
                }
            }
        }

        if (!found) {
            goto fail;
        }

        for (uint32_t j = 0; j < row->recommended_count; j++) {
            found = false;

            if (!text(row->recommended[j].label, 512)) {
                goto fail;
            }

            for (uint32_t k = 0; k < row->choice_count; k++) {
                if (row->recommended[j].value == row->choices[k].value) {
                    found = true;
                }
            }

            if (!found) {
                goto fail;
            }
        }
    }

    return true;
fail:
    snprintf(error, cap, DM_TEXT_EXPORTER_RETURNED_INVALID_REVIEW);
    return false;
}

static bool exporter_valid(const DmeExporter *a) {
    if (!a || !identifier(a->id) || !text(a->name, 128) || !text(a->extension, 16) ||
        !text(a->filter_name, 128) || !text(a->save_label, 128) || a->container > DME_ZIP) {
        return false;
    }

    for (const char *s = a->extension; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9'))) {
            return false;
        }
    }

    bool install = (a->install_label != NULL) == (a->install != NULL) &&
                   (!a->install_label || text(a->install_label, 128));
    return install && a->create && a->destroy && a->inspect && a->describe && a->set && a->reset &&
           a->create_job && a->run && a->destroy_job && a->preflight && a->review &&
           a->review_set && a->review_apply;
}

static int compare(const void *a, const void *b) {
    return SDL_strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static void error(DmExportRegistry *r, const char *file, const char *reason) {
    size_t used = strlen(r->errors);
    snprintf(r->errors + used, sizeof(r->errors) - used, "%s%.180s: %.500s", used ? "\n" : "", file,
             reason);
}

void dm_export_registry_load(DmExportRegistry *r, const char *directory) {
    memset(r, 0, sizeof(*r));
    SDL_PathInfo info;

    if (!SDL_GetPathInfo(directory, &info)) {
        return;
    }

    if (info.type != SDL_PATHTYPE_DIRECTORY) {
        error(r, directory, DM_TEXT_EXPORTER_PATH_NOT_DIRECTORY);
        return;
    }
#if defined(_WIN32)
    const char *pattern = "*.dll";
#elif defined(__APPLE__)
    const char *pattern = "*.dylib";
#else
    const char *pattern = "*.so";
#endif
    int count = 0;
    char **files = SDL_GlobDirectory(directory, pattern, SDL_GLOB_CASEINSENSITIVE, &count);

    if (!files) {
        error(r, directory, SDL_GetError());
        return;
    }

    qsort(files, (size_t)count, sizeof(*files), compare);

    for (int i = 0; i < count; i++) {
        if (r->module_count == 64) {
            error(r, files[i], DM_TEXT_TOO_MANY_EXPORTER_MODULES);
            break;
        }

        char path[4096];

        if (snprintf(path, sizeof(path), "%s/%s", directory, files[i]) >= (int)sizeof(path)) {
            error(r, files[i], DM_TEXT_PATH_TOO_LONG);
            continue;
        }

        SDL_SharedObject *module = SDL_LoadObject(path);

        if (!module) {
            error(r, files[i], SDL_GetError());
            continue;
        }

        DmePluginEntry entry = (DmePluginEntry)SDL_LoadFunction(module, "duckmidi_export_plugin");
        const DmePlugin *plugin = entry ? entry(DME_ABI) : NULL;
        const char *why = NULL;

        if (!entry) {
            why = DM_TEXT_MISSING_DUCKMIDI_EXPORT_PLUGIN;
        } else if (!plugin || plugin->abi != DME_ABI || plugin->size != sizeof(*plugin)) {
            why = DM_TEXT_UNSUPPORTED_EXPORTER_API_ABI;
        } else if (!identifier(plugin->id) || !text(plugin->name, 128) || !plugin->exporter ||
                   !plugin->exporter_count || plugin->exporter_count > 32) {
            why = DM_TEXT_INVALID_PLUGIN_REGISTRATION;
        }

        size_t first = r->count;

        for (uint32_t j = 0; !why && j < plugin->exporter_count; j++) {
            const DmeExporter *a = plugin->exporter(j);

            if (!exporter_valid(a)) {
                why = DM_TEXT_INVALID_EXPORTER_DESCRIPTOR;
                break;
            }

            if (r->count == DM_EXPORT_MAX_FORMATS) {
                why = DM_TEXT_TOO_MANY_REGISTERED_EXPORTERS;
                break;
            }

            for (size_t k = 0; k < r->count; k++) {
                if (!strcmp(a->id, r->formats[k].api->id)) {
                    why = DM_TEXT_DUPLICATE_EXPORTER_ID;
                    break;
                }
            }

            if (why) {
                break;
            }

            void *instance = a->create();

            if (!instance) {
                why = DM_TEXT_CANNOT_ALLOCATE_EXPORTER_SETTINGS;
                break;
            }

            DmeUi ui = {0};
            char schema_error[256];

            if (a->describe(instance, &ui) != DME_OK ||
                !dm_export_ui_valid(&ui, schema_error, sizeof(schema_error))) {
                a->destroy(instance);
                why = DM_TEXT_INVALID_EXPORTER_SETTINGS_SCHEMA;
                break;
            }

            DmExportFormat *f = &r->formats[r->count++];
            f->api = a;
            f->instance = instance;
        }

        if (why) {
            while (r->count > first) {
                DmExportFormat *f = &r->formats[--r->count];
                f->api->destroy(f->instance);
                memset(f, 0, sizeof(*f));
            }

            error(r, files[i], why);
            SDL_UnloadObject(module);
        } else {
            r->modules[r->module_count++] = module;
        }
    }

    SDL_free(files);
}

void dm_export_registry_free(DmExportRegistry *r) {
    for (size_t i = 0; i < r->count; i++) {
        r->formats[i].api->destroy(r->formats[i].instance);
    }

    for (size_t i = 0; i < r->module_count; i++) {
        SDL_UnloadObject(r->modules[i]);
    }

    memset(r, 0, sizeof(*r));
}
