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

static bool view_string(DmeString s, size_t max) {
    return s.length <= max &&
           (!s.length || (s.data && SDL_strnlen(s.data, (size_t)s.length + 1) == s.length));
}

static bool custom_mapping(const DmeMappingRow *row, const DmeCustomMapping *previous) {
    const DmeCustomMapping *m = row->custom;

    if (!m) {
        return true;
    }

    const DmeCustomSelection *s = &m->selection;

    if (m->size != sizeof(*m) || m->available > 1 || s->size != sizeof(*s) || s->enabled > 1 ||
        s->show_all > 1 || s->region_set > 1 || s->operation > DME_SAMPLE_REPLACE ||
        s->sample.size != sizeof(s->sample) || (s->sample.flags & ~DME_SAMPLE_PERCUSSION) ||
        !view_string(s->sample.locator, 8192) || !view_string(s->sample.set_id, 128) ||
        !view_string(s->sample.preset_id, 128) || !view_string(s->physical_target, 16384) ||
        !view_string(m->unavailable_reason, 4096) || m->target_count > 20000 ||
        (m->target_count && !m->targets) || row->source.channel >= 16 || row->source.port >= 128 ||
        row->source.bank >= 16384 || row->source.program >= 128 || row->source.drum_key >= 128 ||
        (row->source.flags & ~DME_SAMPLE_PERCUSSION)) {
        return false;
    }

    bool same_targets =
        previous && previous->targets == m->targets && previous->target_count == m->target_count;

    for (uint32_t i = 0; !same_targets && i < m->target_count; i++) {
        if (!m->targets[i].id.length || !m->targets[i].path.length ||
            !view_string(m->targets[i].id, 16384) || !view_string(m->targets[i].label, 16384) ||
            !view_string(m->targets[i].path, 8192)) {
            return false;
        }
    }

    const DmeSampleCatalog *c = m->catalog;

    if (!c) {
        return !m->available;
    }

    if (previous && previous->catalog == c) {
        return true;
    }

    if (c->size != sizeof(*c) || c->set_count > 1024 || c->preset_count > 8192 ||
        c->zone_count > 32768 || (c->set_count && !c->sets) || (c->preset_count && !c->presets) ||
        (c->zone_count && !c->zones)) {
        return false;
    }

    for (uint32_t i = 0; i < c->set_count; i++) {
        const DmeSampleCatalogSet *set = &c->sets[i];

        if (set->size != sizeof(*set) || set->available > 1 ||
            set->first_preset > c->preset_count ||
            set->preset_count > c->preset_count - set->first_preset || !set->id.length ||
            !view_string(set->id, 128) || !view_string(set->name, 512) ||
            !view_string(set->locator, 8192) || !view_string(set->error, 4096)) {
            return false;
        }
    }

    for (uint32_t i = 0; i < c->preset_count; i++) {
        const DmeSampleCatalogPreset *p = &c->presets[i];

        if (p->size != sizeof(*p) || p->set >= c->set_count || p->suggested_program > 128 ||
            (p->flags & ~DME_SAMPLE_PERCUSSION) || p->first_zone > c->zone_count ||
            p->zone_count > c->zone_count - p->first_zone || !p->id.length ||
            !view_string(p->id, 128) || !view_string(p->name, 512)) {
            return false;
        }
    }

    return true;
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
                    !choices(f, r->default_value) || r->similar_count > 512 ||
                    (r->similar_count && !r->similar) ||
                    !custom_mapping(r, j ? f->rows[j - 1].custom : NULL)) {
                    goto fail;
                }

                for (uint32_t k = 0; k < r->similar_count; k++) {
                    if (!text(r->similar[k].label, 512) || !choices(f, r->similar[k].value)) {
                        goto fail;
                    }
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

    bool install = (a->install_label != NULL) == (a->history != NULL) &&
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

DmNativeTool *dm_export_registry_tool(DmExportRegistry *r, const char *id) {
    for (size_t i = 0; i < r->tool_count; ++i) {
        if (!strcmp(id, r->tools[i].api->id)) {
            return &r->tools[i];
        }
    }

    return NULL;
}

int32_t dm_export_registry_request_tool(DmExportRegistry *r, const char *id) {
    if (SDL_GetCurrentThreadID() != r->main_thread) {
        return DME_INVALID;
    }

    DmNativeTool *tool = dm_export_registry_tool(r, id);

    if (!tool) {
        return DME_UNAVAILABLE;
    }

    if (tool->opening) {
        return DME_FAILED;
    }

    tool->opening = true;
    int32_t status = tool->api->open(tool->instance);
    tool->opening = false;
    tool->open_requested |= status == DME_OK;
    return status;
}

static int32_t DME_CALL request_tool(void *user, const char *id, uint64_t length) {
    char name[128];

    if (!id || !length || length >= sizeof(name) || memchr(id, 0, (size_t)length)) {
        return DME_INVALID;
    }

    memcpy(name, id, (size_t)length);
    name[length] = 0;
    return identifier(name) ? dm_export_registry_request_tool(user, name) : DME_INVALID;
}

static const char *register_tool(DmExportRegistry *r, const DmeTool *api) {
    if (!api || api->size != sizeof(*api) || !identifier(api->id) || !text(api->name, 128) ||
        !api->create || !api->destroy || !api->open || !api->interface_at) {
        return DM_TEXT_INVALID_TOOL_DESCRIPTOR;
    }

    if (r->tool_count == DM_EXPORT_MAX_TOOLS) {
        return DM_TEXT_TOO_MANY_REGISTERED_TOOLS;
    }

    if (dm_export_registry_tool(r, api->id)) {
        return DM_TEXT_DUPLICATE_TOOL_ID;
    }

    void *instance = api->create();

    if (!instance) {
        return DM_TEXT_CANNOT_ALLOCATE_TOOL;
    }

    r->tools[r->tool_count++] = (DmNativeTool){.api = api, .instance = instance};
    return NULL;
}

void dm_export_registry_load(DmExportRegistry *r, const char *directory) {
    memset(r, 0, sizeof(*r));
    r->main_thread = SDL_GetCurrentThreadID();
    snprintf(r->directory, sizeof(r->directory), "%s", directory);
    r->main_host =
        (DmeMainHost){sizeof(DmeMainHost), r, request_tool, {r->directory, strlen(r->directory)}};
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

    /* The history runtime is a dependency of exporters, not a plugin entry point. Load it first so
       exporters resolve it, and retain it until all modules unload. */
    char runtime_file[32];
    snprintf(runtime_file, sizeof(runtime_file), "%s%s", DM_HISTORY_RUNTIME_NAME, pattern + 1);

    for (int i = 0; i < count; ++i) {
        if (SDL_strcasecmp(files[i], runtime_file)) {
            continue;
        }

        char path[4096];

        if (snprintf(path, sizeof(path), "%s/%s", directory, files[i]) >= (int)sizeof(path)) {
            error(r, files[i], DM_TEXT_PATH_TOO_LONG);
            break;
        }

        SDL_SharedObject *helper = SDL_LoadObject(path);

        if (!helper) {
            error(r, files[i], SDL_GetError());
            break;
        }

        r->dependencies[r->dependency_count++] = helper;
        DmeHistoryRuntimeEntry entry =
            (DmeHistoryRuntimeEntry)SDL_LoadFunction(helper, "duckmidi_history_runtime");
        const DmeHistoryRuntime *runtime = entry ? entry(DME_REPOSITORY_API) : NULL;

        if (runtime && runtime->size == sizeof(*runtime) &&
            runtime->version == DME_REPOSITORY_API) {
            r->history_runtime = runtime;
        }

        break;
    }

    for (int i = 0; i < count; i++) {
        if (r->module_count == 64) {
            error(r, files[i], DM_TEXT_TOO_MANY_EXPORTER_MODULES);
            break;
        }

        if (!SDL_strcasecmp(files[i], runtime_file)) {
            continue;
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
        } else if (!identifier(plugin->id) || !text(plugin->name, 128) ||
                   (!plugin->exporter_count && !plugin->tool_count) ||
                   plugin->exporter_count > 32 || plugin->tool_count > 32 ||
                   (plugin->exporter_count != 0) != (plugin->exporter != NULL) ||
                   (plugin->tool_count != 0) != (plugin->tool != NULL)) {
            why = DM_TEXT_INVALID_PLUGIN_REGISTRATION;
        }

        size_t first = r->count;
        size_t first_tool = r->tool_count;

        for (uint32_t j = 0; !why && j < plugin->exporter_count; j++) {
            const DmeExporter *a = plugin->exporter(j);

            if (!exporter_valid(a)) {
                why = DM_TEXT_INVALID_EXPORTER_DESCRIPTOR;
                break;
            }

            const DmeHistoryAdapter *history = a->history ? a->history(DME_REPOSITORY_API) : NULL;

            if (a->history && (!history || history->size != sizeof(*history) ||
                               history->version != DME_REPOSITORY_API || !history->detect ||
                               !history->render || !history->install)) {
                why = DM_TEXT_INVALID_EXPORTER_DESCRIPTOR;
                break;
            }

            if (history && !r->history_runtime) {
                why = DM_TEXT_HISTORY_RUNTIME_MISSING;
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
            f->history = history;
            f->instance = instance;
        }

        for (uint32_t j = 0; !why && j < plugin->tool_count; ++j) {
            why = register_tool(r, plugin->tool(j));
        }

        if (why) {
            while (r->tool_count > first_tool) {
                DmNativeTool *tool = &r->tools[--r->tool_count];
                tool->api->destroy(tool->instance);
                memset(tool, 0, sizeof(*tool));
            }

            while (r->count > first) {
                DmExportFormat *f = &r->formats[--r->count];
                f->api->destroy(f->instance);
                memset(f, 0, sizeof(*f));
            }

            error(r, files[i], why);
            SDL_UnloadObject(module);
        } else {
            r->plugins[r->module_count] = plugin;
            r->modules[r->module_count++] = module;
        }
    }

    SDL_free(files);

    for (size_t i = 0; i < r->module_count; ++i) {
        if (r->plugins[i]->bind_main) {
            r->plugins[i]->bind_main(&r->main_host);
        }
    }
}

void dm_export_registry_free(DmExportRegistry *r) {
    for (size_t i = 0; i < r->module_count; ++i) {
        if (r->plugins[i]->bind_main) {
            r->plugins[i]->bind_main(NULL);
        }
    }

    for (size_t i = 0; i < r->count; i++) {
        if (r->formats[i].history_job) {
            r->history_runtime->destroy_job(r->formats[i].history_job);
        }

        r->formats[i].api->destroy(r->formats[i].instance);
    }

    for (size_t i = 0; i < r->tool_count; ++i) {
        r->tools[i].api->destroy(r->tools[i].instance);
    }

    for (size_t i = 0; i < r->module_count; i++) {
        SDL_UnloadObject(r->modules[i]);
    }

    for (size_t i = r->dependency_count; i > 0; --i) {
        SDL_UnloadObject(r->dependencies[i - 1]);
    }

    memset(r, 0, sizeof(*r));
}
