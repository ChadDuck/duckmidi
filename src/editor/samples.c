#include "editor/export_internal.h"
#include "../../include/duckmidi_samples_tool.h"
#include "../../shared/sample/batch.h"
#include "../../shared/sample/library.h"
#include "../../shared/sample/export_catalog.h"
#include "text/en.h"

typedef struct {
    DmeString paths[DM_SAMPLE_BATCH_MAX];
    uint32_t count;
} SamplePaths;

typedef struct {
    uint32_t draft, zone;
    uint64_t token;
    bool drums;
    char preset_id[DM_SAMPLE_ID_CAP];
    char locator[DM_PATH], set_id[DM_SAMPLE_ID_CAP], asset_id[DM_SAMPLE_ID_CAP];
} PublishedDraft;

typedef struct {
    const DmeSamplesApi *api;
    void *import;
    SDL_Thread *thread;
    SDL_AtomicInt cancel, done, progress;
    int32_t result;
    char error[512], root[DM_PATH], fallback[DM_PATH];
    DmSampleManifest manifest;
    DmSampleAudio *audio;
    PublishedDraft *links;
    size_t link_count;
    bool loading;
    char set_id[DM_SAMPLE_ID_CAP], set_name[DM_SAMPLE_NAME_CAP];
} SampleWork;

struct DmEditorSamples {
    char program[DM_PATH], writable[DM_PATH];
    DmSampleCatalog catalog;
    DmSampleManifest browse;
    uint64_t bank_revision;
    double bank_beat;
    int bank_index, bank_port, bank_channel;
    bool bank_cached, bank_automation, bank_adding;
    uint16_t program_bank;
    char browse_root[DM_PATH], browse_set[DM_SAMPLE_ID_CAP];
    uint32_t browse_generation;
    bool scanned, open, initialized, picker, held_preview, loop_warning, drop_claimed;
    bool delete_sample;
    int remove;
    char current_root[DM_PATH];
    UiRect rect;
    float split, drag_start, drag_width;
    double drag_value;
    int envelope_handle, loop_handle;
    int history_request; /* 1 undo, 2 redo: applied after the editor panel is drawn. */
    uint32_t selected;
    char set_name[DM_SAMPLE_NAME_CAP], update_id[DM_SAMPLE_ID_CAP];
    SamplePaths *drop;
    SampleWork work;
    PublishedDraft *links;
    size_t link_count, link_capacity;
    DmPatchReplacement label_refs[16];
    bool label_ready[16];
    uint64_t label_revision[16];
    char label_names[16][DM_SAMPLE_NAME_CAP];
};

typedef struct DmEditorSamples DmEditorSamples;

static DmNativeTool *tool_for(Editor *e) {
    DmExportPlugins *host = ed_export_host(e);
    return host ? dm_export_registry_tool(&host->registry, DME_SAMPLES_TOOL_ID) : NULL;
}

static const DmeSamplesApi *samples_api(const DmNativeTool *tool) {
    const DmeSamplesApi *api = tool ? tool->api->interface_at(DME_SAMPLES_API) : NULL;
    return api && api->size == sizeof(*api) && api->version == DME_SAMPLES_API ? api : NULL;
}

static DmEditorSamples *state(Editor *e) {
    if (e->samples) {
        return e->samples;
    }

    DmEditorSamples *s = calloc(1, sizeof(*s));

    if (s) {
        if (!dm_sample_library_roots(e->base_path, e->settings_path, s->program, sizeof(s->program),
                                     s->writable, sizeof(s->writable))) {
            free(s);
            return NULL;
        }

        s->envelope_handle = s->loop_handle = s->remove = -1;
        e->samples = s;
    }

    return s;
}

bool ed_samples_available(Editor *e) {
    return samples_api(tool_for(e)) != NULL;
}

void ed_samples_open(Editor *e) {
    DmExportPlugins *host = ed_export_host(e);

    if (!host || !state(e) || !ed_samples_available(e) ||
        dm_export_registry_request_tool(&host->registry, DME_SAMPLES_TOOL_ID) != DME_OK) {
        ed_message(e, DM_TEXT_SAMPLE_TOOL_UNAVAILABLE);
    }
}

static void free_paths(SamplePaths *paths) {
    if (paths) {
        for (uint32_t i = 0; i < paths->count; ++i) {
            SDL_free((void *)paths->paths[i].data);
        }

        free(paths);
    }
}

static bool append_path(SamplePaths **paths, const char *name) {
    if (!*paths) {
        *paths = calloc(1, sizeof(**paths));
    }

    if (!*paths || (*paths)->count == DM_SAMPLE_BATCH_MAX) {
        return false;
    }

    char *copy = SDL_strdup(name);

    if (!copy) {
        return false;
    }

    (*paths)->paths[(*paths)->count++] = (DmeString){copy, strlen(copy)};
    return true;
}

static void SDLCALL pick_samples(void *user, const char *const *files, int filter) {
    (void)filter;
    SamplePaths *paths = NULL;
    char *error = NULL;

    if (!files) {
        error = SDL_strdup(SDL_GetError());
    } else {
        for (const char *const *path = files; *path; ++path) {
            if (!append_path(&paths, *path)) {
                free_paths(paths);
                paths = NULL;
                error = SDL_strdup(DM_TEXT_SAMPLE_IMPORT_BATCH_LIMIT);
                break;
            }
        }
    }

    SDL_Event event = {.type = ed_dialog_event};
    event.user.code = (int)(intptr_t)user;
    event.user.data1 = paths;
    event.user.data2 = error;

    if (!SDL_PushEvent(&event)) {
        free_paths(paths);
        SDL_free(error);
    }
}

static void choose_samples(Editor *e, bool folder) {
    if (e->dialog_busy) {
        return;
    }

    static const SDL_DialogFileFilter filters[] = {
        {DM_TEXT_SAMPLE_AUDIO_FILES, "wav;mp3;m4a;ogg;oga;opus;flac;aiff;aif;aifc"},
        {DM_TEXT_FILES, "*"}};
    e->dialog_busy = true;
    e->samples->picker = true;
    int kind = folder ? DIALOG_SAMPLE_FOLDER : DIALOG_SAMPLE_FILES;
    void *token = (void *)(intptr_t)kind;

    if (folder) {
        SDL_ShowOpenFolderDialog(pick_samples, token, e->window, NULL, false);
    } else {
        SDL_ShowOpenFileDialog(pick_samples, token, e->window, filters, SDL_arraysize(filters),
                               NULL, true);
    }
}

static int32_t DME_CALL work_canceled(void *user) {
    SampleWork *w = user;
    return SDL_GetAtomicInt(&w->cancel);
}

static bool publish_canceled(void *user) {
    return work_canceled(user) != 0;
}

static void DME_CALL import_progress(void *user, uint32_t stage, DmeString path, uint32_t index,
                                     uint32_t total, uint64_t done, uint64_t units) {
    (void)path;
    SampleWork *w = user;
    unsigned base = total ? index * 100u / total : 0;
    unsigned span = total ? 100u / total : 0;
    unsigned fraction = units ? (unsigned)(done * span / units) : 0;
    SDL_SetAtomicInt(&w->progress, (int)(base + (stage == DME_SAMPLE_ANALYZE ? span : fraction)));
}

static int SDLCALL sample_worker(void *user) {
    SampleWork *w = user;

    if (w->import) {
        DmeSampleImportHost host = {w, work_canceled, import_progress};
        w->result = w->api->run_import(w->import, &host, w->error, sizeof(w->error));
    } else {
        DmSampleImportCallbacks callbacks = {w, publish_canceled, NULL};
        bool ready = !work_canceled(w) && SDL_CreateDirectory(w->root);

        if (*w->fallback && (!ready || !dm_sample_library_writable(w->root)) && !work_canceled(w)) {
            snprintf(w->root, sizeof(w->root), "%s", w->fallback);
            ready = SDL_CreateDirectory(w->root);
        }

        bool ok = ready && dm_sample_library_publish(w->root, &w->manifest, w->audio, &callbacks,
                                                     w->error, sizeof(w->error));
        w->result = ok ? DME_OK : work_canceled(w) ? DME_CANCELLED : DME_FAILED;

        if (!ok && !*w->error) {
            snprintf(w->error, sizeof(w->error), "%s", SDL_GetError());
        }
    }

    SDL_SetAtomicInt(&w->done, 1);
    return 0;
}

static void work_free(SampleWork *w) {
    if (w->thread) {
        SDL_SetAtomicInt(&w->cancel, 1);
        SDL_WaitThread(w->thread, NULL);
    }

    if (w->import) {
        w->api->destroy_import(w->import);
    }

    if (w->audio) {
        for (uint32_t i = 0; i < w->manifest.asset_count; ++i) {
            dm_sample_audio_free(&w->audio[i]);
        }
    }

    free(w->audio);
    free(w->links);
    dm_sample_manifest_free(&w->manifest);
    *w = (SampleWork){0};
}

static void begin_import(Editor *e, SamplePaths *paths) {
    DmEditorSamples *s = e->samples;
    DmNativeTool *tool = tool_for(e);
    const DmeSamplesApi *api = samples_api(tool);

    if (s && s->open && !s->work.thread && api && paths && paths->count) {
        SampleWork *w = &s->work;
        w->api = api;
        w->import = api->create_import(tool->instance, paths->paths, paths->count, w->error,
                                       sizeof(w->error));

        if (w->import) {
            w->thread = SDL_CreateThread(sample_worker, "Sample import", w);
        }

        if (!w->thread) {
            ed_message(e, *w->error ? w->error : DM_TEXT_SAMPLE_IMPORT_MEMORY);
            work_free(w);
        }
    }

    free_paths(paths);
}

void ed_samples_picker_result(Editor *e, const SDL_Event *event) {
    if (e->samples) {
        e->samples->picker = false;
    }

    if (event->user.data2) {
        ed_message(e, event->user.data2);
    }

    begin_import(e, event->user.data1);
    SDL_free(event->user.data2);
}

bool ed_samples_drop_target(Editor *e, UiPoint at, UiRect *rect) {
    DmEditorSamples *s = e->samples;

    if (!s || !s->open || s->delete_sample || s->work.thread ||
        (!ed_inside(at, s->rect) || !ui_window_front_at(DM_TEXT_SAMPLE_TOOL_NAME, at))) {
        return false;
    }

    if (rect) {
        *rect = s->rect;
    }

    return true;
}

bool ed_samples_drop(Editor *e, const SDL_DropEvent *event) {
    DmEditorSamples *s = e->samples;

    if (!s) {
        return false;
    }

    if (event->type == SDL_EVENT_DROP_BEGIN) {
        free_paths(s->drop);
        s->drop = NULL;
        s->drop_claimed = false;
    } else if (event->type == SDL_EVENT_DROP_FILE && event->data) {
        UiPoint at = {event->x / ui_scale(), event->y / ui_scale()};

        if (s->drop_claimed || (!e->drop_handled && ed_samples_drop_target(e, at, NULL))) {
            s->drop_claimed = true;

            if (!append_path(&s->drop, event->data)) {
                ed_message(e, DM_TEXT_SAMPLE_IMPORT_BATCH_LIMIT);
            }

            if (!e->drop_active) {
                begin_import(e, s->drop);
                s->drop = NULL;
                s->drop_claimed = false;
            }

            return true;
        }
    } else if (event->type == SDL_EVENT_DROP_COMPLETE && s->drop_claimed) {
        begin_import(e, s->drop);
        s->drop = NULL;
        s->drop_claimed = false;
    }

    return false;
}

void ed_samples_close(Editor *e) {
    DmEditorSamples *s = e->samples;

    if (s && s->open) {
        dm_audio_sample_audition_stop(e->audio);
        s->open = s->held_preview = s->delete_sample = false;
        if (s->work.thread) {
            SDL_SetAtomicInt(&s->work.cancel, 1);
        }
    }
}

void ed_samples_free(Editor *e) {
    DmEditorSamples *s = e->samples;

    if (s) {
        ed_samples_close(e);
        work_free(&s->work);
        free_paths(s->drop);
        dm_sample_catalog_free(&s->catalog);
        free(s->links);
        dm_sample_manifest_free(&s->browse);
        free(s);
        e->samples = NULL;
    }
}

void ed_sample_catalog_refresh(Editor *e) {
    ed_export_catalog_changed(e);
    DmEditorSamples *s = state(e);

    if (s) {
        DmSampleCatalog next = {0};
        char error[256];
        s->scanned = true;

        if (dm_sample_library_catalog(s->program, s->writable, NULL, &next, error, sizeof(error))) {
            dm_sample_catalog_free(&s->catalog);
            s->catalog = next;
        } else {
            ed_message(e, error);
        }
    }
}

void ed_samples_poll(Editor *e) {
    DmEditorSamples *s = e->samples;

    if (!s || !s->work.thread || !SDL_GetAtomicInt(&s->work.done)) {
        return;
    }

    SampleWork *w = &s->work;
    SDL_WaitThread(w->thread, NULL);
    w->thread = NULL;
    int32_t result = w->result;

    if (w->import && result == DME_OK) {
        DmNativeTool *tool = tool_for(e);

        if (w->loading && !dm_reserve((void **)&s->links, &s->link_capacity,
                                      s->link_count + DM_SAMPLE_BATCH_MAX, sizeof(*s->links))) {
            result = DME_NOMEM;
        }

        result = !s->open || work_canceled(w) ? DME_CANCELLED
                 : result != DME_OK
                     ? result
                     : w->api->finish_import(tool->instance, w->import, w->error, sizeof(w->error));

        if (result == DME_OK && w->loading) {
            snprintf(s->update_id, sizeof(s->update_id), "%s", w->set_id);
            snprintf(s->current_root, sizeof(s->current_root), "%s", w->root);
            snprintf(s->set_name, sizeof(s->set_name), "%s", w->set_name);
            s->selected = 0;
            s->delete_sample = false;
            s->envelope_handle = s->loop_handle = -1;

            for (uint32_t i = 0; i < w->api->count(tool->instance); ++i) {
                DmeSampleDraft draft;

                if (w->api->draft(tool->instance, i, &draft) != DME_OK || !draft.asset_id.length) {
                    continue;
                }

                size_t at = 0;

                while (at < s->link_count && (s->links[at].token != draft.token ||
                                              strcmp(s->links[at].set_id, w->set_id) ||
                                              strcmp(s->links[at].locator, w->root))) {
                    ++at;
                }

                if (at < s->link_count) {
                    continue;
                }

                PublishedDraft *link = &s->links[at];
                *link = (PublishedDraft){.draft = i,
                                         .zone = draft.preset_zone,
                                         .token = draft.token,
                                         .drums = draft.edit.drums != 0};
                snprintf(link->locator, sizeof(link->locator), "%s", w->root);
                snprintf(link->set_id, sizeof(link->set_id), "%s", w->set_id);
                snprintf(link->asset_id, sizeof(link->asset_id), "%.*s", (int)draft.asset_id.length,
                         draft.asset_id.data);
                snprintf(link->preset_id, sizeof(link->preset_id), "%.*s",
                         (int)draft.preset_id.length, draft.preset_id.data);
                s->link_count += at == s->link_count;
            }
        }
    } else if (!w->import && result == DME_OK) {
        snprintf(s->writable, sizeof(s->writable), "%s", w->root);
        ed_sample_catalog_refresh(e);
        /* Reserve before starting the worker, so successful publication cannot lose its links. */
        for (size_t i = 0; i < w->link_count; ++i) {
            PublishedDraft link = w->links[i];
            snprintf(link.locator, sizeof(link.locator), "%s", w->root);
            size_t at = 0;

            while (at < s->link_count &&
                   (s->links[at].token != link.token || strcmp(s->links[at].set_id, link.set_id) ||
                    strcmp(s->links[at].locator, link.locator))) {
                ++at;
            }

            s->links[at] = link;
            s->link_count += at == s->link_count;
        }

        bool assigned = false;

        for (size_t i = 0; i < e->project.patch_replacement_count; ++i) {
            const DmPatchReplacement *r = &e->project.patch_replacements[i];
            assigned |= !strcmp(r->locator, *s->current_root ? s->current_root : w->root) &&
                        !strcmp(r->set_id, w->manifest.id) &&
                        r->generation != w->manifest.generation;
        }

        if (assigned) {
            for (size_t i = 0; i < e->project.patch_replacement_count; ++i) {
                DmPatchReplacement *r = &e->project.patch_replacements[i];

                if (!strcmp(r->locator, *s->current_root ? s->current_root : w->root) &&
                    !strcmp(r->set_id, w->manifest.id)) {
                    for (uint32_t j = 0; j < w->manifest.preset_count; ++j) {
                        const DmSamplePreset *p = &w->manifest.presets[j];

                        if (!strcmp(r->preset_id, p->id) && r->key.drums == p->drums) {
                            r->generation = w->manifest.generation;
                            snprintf(r->locator, sizeof(r->locator), "%s", w->root);
                            break;
                        }
                    }
                }
            }

            ed_publish(e);
        }

        DmNativeTool *tool = tool_for(e);
        const DmeSamplesApi *api = samples_api(tool);
        api->published(tool->instance, (DmeString){w->root, strlen(w->root)},
                       (DmeString){w->manifest.id, strlen(w->manifest.id)});
        snprintf(s->current_root, sizeof(s->current_root), "%s", w->root);
        snprintf(s->update_id, sizeof(s->update_id), "%s", w->manifest.id);
        ed_message(e, DM_TEXT_SAMPLE_SAVED);
    }

    if (result != DME_OK && result != DME_CANCELLED) {
        ed_message(e, *w->error ? w->error : DM_TEXT_SAMPLE_TOOL_UNAVAILABLE);
    }

    work_free(w);
}

static const DmSampleCatalogEntry *find_set(const DmEditorSamples *s, const char *id) {
    for (size_t i = 0; i < s->catalog.count; ++i) {
        if (!strcmp(id, s->catalog.entries[i].id)) {
            return &s->catalog.entries[i];
        }
    }

    return NULL;
}

static const DmSampleManifest *browse_set(DmEditorSamples *s, const char *root, const char *id,
                                          uint32_t generation) {
    if (s->browse_generation != generation || strcmp(s->browse_root, root) ||
        strcmp(s->browse_set, id)) {
        dm_sample_manifest_free(&s->browse);
        s->browse_generation = generation;
        snprintf(s->browse_root, sizeof(s->browse_root), "%s", root);
        snprintf(s->browse_set, sizeof(s->browse_set), "%s", id);
        dm_sample_library_manifest(root, id, &s->browse, NULL, 0);
    }

    return &s->browse;
}

/* The name given to the channel's sample in its set, for the playlist label. */
const char *ed_sample_channel_name(Editor *e, int channel) {
    if (channel < 0 || channel >= 16 || !e->project.patch_replacement_count) {
        return NULL;
    }

    DmEditorSamples *s = state(e);

    if (!s) {
        return NULL;
    }

    if (s->label_ready[channel] && s->label_revision[channel] == e->export_revision) {
        return *s->label_names[channel] ? s->label_names[channel] : NULL;
    }

    s->label_revision[channel] = e->export_revision;
    int program = e->project.channels[channel].program;
    DmPatchKey key = {(uint16_t)ed_initial_bank(&e->project, 0, channel), 0, (uint8_t)channel,
                      (uint8_t)(program < 0 ? 0 : program), dm_channel_drums(&e->project, channel)};
    const DmPatchReplacement *r = dm_patch_replacement(&e->project, key);

    if (!r || !r->generation) {
        s->label_ready[channel] = true;
        s->label_refs[channel] = (DmPatchReplacement){0};
        *s->label_names[channel] = 0;
        return NULL;
    }

    if (!s->label_ready[channel] || memcmp(&s->label_refs[channel], r, sizeof(*r))) {
        s->label_refs[channel] = *r;
        s->label_ready[channel] = true;
        *s->label_names[channel] = 0;
        const DmSampleManifest *set = browse_set(s, r->locator, r->set_id, r->generation);

        for (uint32_t i = 0; i < set->preset_count; ++i) {
            const DmSamplePreset *p = &set->presets[i];

            if (!strcmp(p->id, r->preset_id) && p->zone_count) {
                const DmSampleAsset *a = &set->assets[set->zones[p->first_zone].asset];
                snprintf(s->label_names[channel], sizeof(s->label_names[channel]), "%s",
                         *p->name          ? p->name
                         : *a->source_name ? a->source_name
                                           : a->file);
                break;
            }
        }
    }

    if (!*s->label_names[channel]) {
        snprintf(s->label_names[channel], sizeof(s->label_names[channel]), "%s", r->preset_id);
    }

    return s->label_names[channel];
}

static DmPatchReplacement replacement_for(DmPatchKey key, const char *root,
                                          const DmSampleManifest *set, const DmSamplePreset *p) {
    DmPatchReplacement value = {.key = key, .generation = set->generation};
    snprintf(value.locator, sizeof(value.locator), "%s", root);
    snprintf(value.set_id, sizeof(value.set_id), "%s", set->id);
    snprintf(value.preset_id, sizeof(value.preset_id), "%s", p->id);
    return value;
}

bool ed_sample_source_picker(Editor *e, DmPatchKey key, const DmPatchReplacement *current,
                             DmPatchReplacement *selected, bool *inherit) {
    DmEditorSamples *s = state(e);

    if (!s) {
        return false;
    }

    if (!s->scanned) {
        ed_sample_catalog_refresh(e);
    }

    const char *label = current && current->generation ? current->set_id
                        : !current && key.port         ? DM_TEXT_SAMPLE_INHERIT
                                                       : DM_TEXT_SOUNDFONT;
    const DmSampleManifest *set = NULL;
    bool changed = false;
    char preset_name[DM_SAMPLE_NAME_CAP];

    if (current && current->generation) {
        set = browse_set(s, current->locator, current->set_id, current->generation);
        label = *set->name ? set->name : current->set_id;
    }

    ui_width(ui_available().x - ui_text_width(DM_TEXT_SAMPLE_SOURCE) - 2 * ui_item_spacing_x());

    if (ui_combo(DM_TEXT_SAMPLE_SOURCE, label)) {
        if (key.port && ui_select(DM_TEXT_SAMPLE_INHERIT, !current, 0, 0)) {
            *inherit = true;
            changed = true;
        }

        if (ui_select(DM_TEXT_SOUNDFONT,
                      (!current && !key.port) || (current && !current->generation), 0, 0)) {
            *selected = (DmPatchReplacement){.key = key};
            *inherit = !key.port;
            changed = true;
        }

        for (size_t i = 0; i < s->catalog.count; ++i) {
            const DmSampleCatalogEntry *entry = &s->catalog.entries[i];
            const char *root = entry->writable_root ? s->writable : s->program;
            ui_id((int)i);
            bool same = current && current->generation == entry->generation &&
                        !strcmp(entry->id, current->set_id);

            if (ui_select(entry->name, same, 0, 0)) {
                set = browse_set(s, root, entry->id, entry->generation);

                for (uint32_t j = 0; j < set->preset_count; ++j) {
                    if (set->presets[j].drums == key.drums) {
                        *selected = replacement_for(key, root, set, &set->presets[j]);
                        *inherit = false;
                        changed = true;
                        break;
                    }
                }

                if (!changed) {
                    ed_message(e, DM_TEXT_SAMPLE_NO_COMPATIBLE_PRESET);
                }
            }

            ui_pop_id();
        }

        ui_combo_end();
    }

    if (changed || !current || !current->generation) {
        return changed;
    }

    set = browse_set(s, current->locator, current->set_id, current->generation);
    snprintf(preset_name, sizeof(preset_name), "%s", current->preset_id);

    for (uint32_t i = 0; i < set->preset_count; ++i) {
        if (!strcmp(current->preset_id, set->presets[i].id)) {
            snprintf(preset_name, sizeof(preset_name), "%s", set->presets[i].name);
            break;
        }
    }

    if (!set->preset_count) {
        ui_dim(DM_TEXT_SAMPLE_UNAVAILABLE);
    }

    ui_width(ui_available().x - ui_text_width(DM_TEXT_SAMPLE_PRESET) - 2 * ui_item_spacing_x());

    if (ui_combo(DM_TEXT_SAMPLE_PRESET, preset_name)) {
        for (uint32_t i = 0; i < set->preset_count; ++i) {
            const DmSamplePreset *preset = &set->presets[i];

            if (preset->drums == key.drums) {
                ui_id((int)i);

                if (ui_select(preset->name, !strcmp(current->preset_id, preset->id), 0, 0)) {
                    *selected = replacement_for(key, current->locator, set, preset);
                    *inherit = false;
                    changed = true;
                }

                ui_pop_id();
            }
        }

        ui_combo_end();
    }

    return changed;
}

bool ed_sample_store_replacement(Editor *e, const DmPatchReplacement *selection, bool inherit) {
    if (inherit) {
        dm_patch_replacement_clear(&e->project, selection->key);
        return true;
    }

    if (!dm_patch_replacement_set(&e->project, selection)) {
        ed_message(e, DM_TEXT_SAMPLE_ASSIGN_FAILED);
        return false;
    }

    dm_audio_reload_samples(e->audio);
    return true;
}

void ed_sample_clear_route(DmProject *p, int port, int channel) {
    size_t write = 0;

    for (size_t i = 0; i < p->patch_replacement_count; ++i) {
        DmPatchReplacement *r = &p->patch_replacements[i];

        if ((port < 0 || r->key.port == port) && (channel < 0 || r->key.channel == channel)) {
            continue;
        }

        if (i != write) {
            p->patch_replacements[write] = *r;
        }

        ++write;
    }

    p->patch_replacement_count = write;
}

static void bank_controller(double beat, int controller, int value, double latest[2],
                            unsigned bank[2]) {
    int byte = controller == MIDI_CC_BANK ? 0 : controller == MIDI_CC_BANK_LSB ? 1 : -1;

    if (byte >= 0 && beat >= latest[byte]) {
        latest[byte] = beat;
        bank[byte] = (unsigned)value;
    }
}

static uint16_t program_bank(const Editor *e, DmEditorSamples *s, int port, int channel,
                             bool adding) {
    if (s->bank_cached && s->bank_revision == e->export_revision && s->bank_beat == e->event_beat &&
        s->bank_index == e->midi_event_index && s->bank_port == port &&
        s->bank_channel == channel && s->bank_automation == !!e->midi_event_kind &&
        s->bank_adding == adding) {
        return s->program_bank;
    }

    s->bank_cached = true;
    s->bank_revision = e->export_revision;
    s->bank_beat = e->event_beat;
    s->bank_index = e->midi_event_index;
    s->bank_port = port;
    s->bank_channel = channel;
    s->bank_automation = e->midi_event_kind != 0;
    s->bank_adding = adding;
    double latest[2] = {-1, -1};
    unsigned bank[2] = {0, 0};
    bool raw = !adding && !e->midi_event_kind;
    uint64_t tick = isfinite(e->event_beat) && e->event_beat > 0
                        ? (uint64_t)llround(e->event_beat * DM_PPQ)
                        : 0;
    DmExportEvent *timeline = NULL;
    size_t count = 0;
    uint64_t duration;

    if (dm_export_timeline(&e->project, &timeline, &count, &duration, NULL, 0)) {
        for (size_t i = 0; i < count && timeline[i].tick < tick; ++i) {
            DmExportEvent ev = timeline[i];

            if (ev.port == (unsigned)port && ev.status == (unsigned)(MIDI_CONTROL | channel)) {
                bank_controller((double)ev.tick, (int)ev.a, (int)ev.b, latest, bank);
            }
        }
    }

    free(timeline);

    for (size_t i = 0; i < e->project.event_count; ++i) {
        const DmEvent *event = &e->project.events[i];
        const DmBytes *message = &event->message;

        if (event->port == port && message->size == 3 &&
            message->data[0] == (MIDI_CONTROL | channel) &&
            (uint64_t)llround(event->beat * DM_PPQ) == tick &&
            (!raw || i < (size_t)e->midi_event_index)) {
            bank_controller((double)tick, message->data[1], message->data[2], latest, bank);
        }
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.port == port && a.channel == channel && a.pattern < 0 && a.type == MIDI_CONTROL &&
            (uint64_t)llround(a.beat * DM_PPQ) == tick &&
            (adding || (!raw && i < (size_t)e->midi_event_index))) {
            bank_controller((double)tick, a.a, a.b, latest, bank);
        }
    }

    s->program_bank = (uint16_t)((bank[0] << 7) | bank[1]);
    return s->program_bank;
}

static bool same_patch(DmPatchKey a, DmPatchKey b) {
    return a.port == b.port && a.channel == b.channel && a.bank == b.bank &&
           a.program == b.program && a.drums == b.drums;
}

void ed_sample_program_source(Editor *e) {
    bool adding = e->song_event && e->song_event_add;
    int port = adding ? 0 : e->midi_source_port;
    int channel = e->midi_draft.data[0] & MIDI_CHANNEL_MASK;
    DmEditorSamples *s = state(e);

    if (!s || port < 0 || port >= DM_NOTE_PORTS) {
        return;
    }

    DmPatchKey key = {.bank = program_bank(e, s, port, channel, adding),
                      .port = (uint8_t)port,
                      .channel = (uint8_t)channel,
                      .program = e->midi_draft.data[1],
                      .drums = dm_channel_drums(&e->project, channel)};
    char label[96];
    snprintf(label, sizeof(label), DM_TEXT_SAMPLE_TARGET_BANK, key.bank, key.port);
    ui_dim(label);

    if (e->midi_sample_dirty && !same_patch(key, e->midi_sample_source.key)) {
        e->midi_sample_dirty = false;
    }

    const DmPatchReplacement *current =
        e->midi_sample_dirty ? (e->midi_sample_inherit ? NULL : &e->midi_sample_source)
                             : dm_patch_replacement_exact(&e->project, key);
    DmPatchReplacement next = {.key = key};
    bool inherit = false;

    if (ed_sample_source_picker(e, key, current, &next, &inherit)) {
        e->midi_sample_source = next;
        e->midi_sample_inherit = inherit;
        e->midi_sample_dirty = true;
    }

    if (e->midi_sample_dirty && ui_button(DM_TEXT_SAMPLE_APPLY_SOURCE, 0, 0)) {
        ed_checkpoint(e);

        if (ed_sample_store_replacement(e, &e->midi_sample_source, e->midi_sample_inherit)) {
            ed_commit(e);
            e->midi_selection_revision = e->export_revision;
            e->midi_sample_dirty = false;
        }
    }
}

static bool new_set_id(DmEditorSamples *s, char id[DM_SAMPLE_ID_CAP]) {
    char stem[DM_SAMPLE_ID_CAP - 6];
    size_t n = 0;
    bool space = false;

    for (const unsigned char *c = (const unsigned char *)s->set_name; *c && n + 2 < sizeof(stem);
         ++c) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) {
            if (space && n) {
                stem[n++] = '-';
            }

            stem[n++] = (char)tolower(*c);
            space = false;
        } else {
            space = true;
        }
    }

    stem[n] = 0;
    const char *base = n ? stem : "sample-set";
    snprintf(id, DM_SAMPLE_ID_CAP, "%s", base);

    for (unsigned suffix = 2; find_set(s, id) && suffix <= 9999; ++suffix) {
        snprintf(id, DM_SAMPLE_ID_CAP, "%s-%u", base, suffix);
    }

    return !find_set(s, id);
}

static bool prepare_publication(DmEditorSamples *s, const DmNativeTool *tool,
                                const DmeSamplesApi *api) {
    SampleWork *w = &s->work;
    DmSampleManifest *m = &w->manifest;
    uint32_t count = api->count(tool->instance);
    const DmSampleCatalogEntry *existing = find_set(s, s->update_id);

    for (size_t i = 0; i < s->catalog.count; ++i) {
        const DmSampleCatalogEntry *other = &s->catalog.entries[i];

        if (other != existing && !SDL_strcasecmp(s->set_name, other->name)) {
            snprintf(w->error, sizeof(w->error), "%s", DM_TEXT_SAMPLE_SET_NAME_COLLISION);
            return false;
        }
    }

    if (existing) {
        snprintf(m->id, sizeof(m->id), "%s", existing->id);
    } else {
        if (!new_set_id(s, m->id)) {
            snprintf(w->error, sizeof(w->error), "%s", DM_TEXT_SAMPLE_SET_NAME_COLLISION);
            return false;
        }
    }

    if (!dm_sample_name_valid(s->set_name)) {
        snprintf(w->error, sizeof(w->error), "%s", DM_TEXT_SAMPLE_STORAGE_NAME);
        return false;
    }

    snprintf(m->name, sizeof(m->name), "%s", s->set_name);
    snprintf(w->root, sizeof(w->root), "%s", s->writable);
#ifdef _WIN32
    if (!existing && !strcmp(s->writable, s->program)) {
        char *prefs = SDL_GetPrefPath(NULL, "duckmidi");

        if (prefs && snprintf(w->fallback, sizeof(w->fallback), "%ssamples", prefs) >=
                         (int)sizeof(w->fallback)) {
            *w->fallback = 0;
        }

        SDL_free(prefs);
    }
#endif
    m->assets = calloc(count, sizeof(*m->assets));
    m->presets = calloc(count, sizeof(*m->presets));
    m->zones = calloc(count, sizeof(*m->zones));
    w->audio = calloc(count, sizeof(*w->audio));
    w->links = calloc(count, sizeof(*w->links));

    if (!dm_reserve((void **)&s->links, &s->link_capacity, s->link_count + count,
                    sizeof(*s->links))) {
        return false;
    }

    if (!m->assets || !m->presets || !m->zones || !w->audio || !w->links) {
        return false;
    }

    typedef struct {
        DmeSampleDraft view;
        uint32_t asset, draft, link;
        char preset[DM_SAMPLE_ID_CAP];
        bool used;
    } Prepared;

    Prepared *prepared = calloc(count ? count : 1, sizeof(*prepared));

    if (!prepared) {
        return false;
    }

    uint32_t total = 0;
    bool ok = true;
    SDL_Time now;

    if (!SDL_GetCurrentTime(&now)) {
        free(prepared);
        return false;
    }

    for (uint32_t i = 0; ok && i < count; ++i) {
        DmeSampleDraft draft;

        if (api->draft(tool->instance, i, &draft) != DME_OK || draft.error.length) {
            continue;
        }

        char asset_id[DM_SAMPLE_ID_CAP] = "", preset_id[DM_SAMPLE_ID_CAP] = "";

        if (draft.asset_id.length) {
            snprintf(asset_id, sizeof(asset_id), "%.*s", (int)draft.asset_id.length,
                     draft.asset_id.data);
        }

        if (draft.preset_id.length && draft.preset_drums == draft.edit.drums) {
            snprintf(preset_id, sizeof(preset_id), "%.*s", (int)draft.preset_id.length,
                     draft.preset_id.data);
        }

        for (size_t link_index = 0; link_index < s->link_count; ++link_index) {
            const PublishedDraft *link = &s->links[link_index];

            if (link->token == draft.token && !strcmp(link->set_id, s->update_id) &&
                !strcmp(link->locator, s->current_root)) {
                snprintf(asset_id, sizeof(asset_id), "%s", link->asset_id);

                if (link->drums == (draft.edit.drums != 0)) {
                    snprintf(preset_id, sizeof(preset_id), "%s", link->preset_id);
                }

                break;
            }
        }

        if (!*asset_id) {
            snprintf(asset_id, sizeof(asset_id), "sample-%llx-%u", (unsigned long long)now, i + 1);
        }

        if (!*preset_id) {
            snprintf(preset_id, sizeof(preset_id), "%s", draft.edit.drums ? "drums" : asset_id);
        }

        uint32_t index = 0;

        while (index < m->asset_count && strcmp(m->assets[index].id, asset_id)) {
            ++index;
        }

        if (index == m->asset_count) {
            DmSampleAsset *asset = &m->assets[m->asset_count++];
            snprintf(asset->id, sizeof(asset->id), "%s", asset_id);
            char sample_name[DM_SAMPLE_NAME_CAP];
            snprintf(sample_name, sizeof(sample_name), "%.*s", (int)draft.edit.name.length,
                     draft.edit.name.data);

            if (draft.preset_id.length && draft.asset_name.length) {
                for (uint32_t other = 0; other < count; ++other) {
                    DmeSampleDraft sibling;

                    if (api->draft(tool->instance, other, &sibling) == DME_OK &&
                        sibling.preset_id.length == draft.preset_id.length &&
                        !memcmp(sibling.preset_id.data, draft.preset_id.data,
                                draft.preset_id.length) &&
                        (sibling.asset_id.length != draft.asset_id.length ||
                         memcmp(sibling.asset_id.data, draft.asset_id.data,
                                draft.asset_id.length))) {
                        snprintf(sample_name, sizeof(sample_name), "%.*s",
                                 (int)draft.asset_name.length, draft.asset_name.data);
                        break;
                    }
                }
            }

            if (!dm_sample_name_valid(sample_name)) {
                snprintf(w->error, sizeof(w->error), "%s", DM_TEXT_SAMPLE_STORAGE_NAME);
                ok = false;
                break;
            }

            snprintf(asset->file, sizeof(asset->file), "%s.wav", sample_name);

            for (uint32_t previous = 0; previous < index; ++previous) {
                if (!SDL_strcasecmp(m->assets[previous].file, asset->file)) {
                    snprintf(w->error, sizeof(w->error), "%s", DM_TEXT_SAMPLE_STORAGE_COLLISION);
                    ok = false;
                    break;
                }
            }

            if (!ok) {
                break;
            }

            const char *leaf = draft.path.data;

            for (const char *c = leaf; *c; ++c) {
                if (*c == '/' || *c == '\\') {
                    leaf = c + 1;
                }
            }

            SDL_utf8strlcpy(asset->source_name,
                            draft.source_name.length ? draft.source_name.data : leaf,
                            sizeof(asset->source_name));
            asset->frames = (uint32_t)draft.frames;
            asset->rate = draft.rate;
            asset->channels = (uint8_t)draft.channels;
            size_t values = (size_t)draft.frames * draft.channels;
            w->audio[index] = (DmSampleAudio){.pcm = malloc(values * sizeof(float)),
                                              .frames = (size_t)draft.frames,
                                              .rate = (int)draft.rate,
                                              .channels = (int)draft.channels,
                                              .embedded_root = draft.embedded_root,
                                              .embedded_cents = draft.embedded_cents,
                                              .embedded_loop_start = draft.embedded_loop_start,
                                              .embedded_loop_end = draft.embedded_loop_end};
            ok = w->audio[index].pcm && api->read_pcm(tool->instance, i, 0, draft.frames,
                                                      w->audio[index].pcm, values) == DME_OK;
        }

        PublishedDraft *link = &w->links[w->link_count++];
        *link = (PublishedDraft){.draft = i,
                                 .zone = draft.preset_zone,
                                 .token = draft.token,
                                 .drums = draft.edit.drums != 0};
        snprintf(link->set_id, sizeof(link->set_id), "%s", m->id);
        snprintf(link->asset_id, sizeof(link->asset_id), "%s", asset_id);
        snprintf(link->preset_id, sizeof(link->preset_id), "%s", preset_id);
        prepared[total] = (Prepared){
            .view = draft, .asset = index, .draft = i, .link = (uint32_t)w->link_count - 1};
        snprintf(prepared[total].preset, sizeof(prepared[total].preset), "%s", preset_id);
        ++total;
    }

    for (uint32_t i = 0; ok && i < total; ++i) {
        if (prepared[i].used) {
            continue;
        }

        DmeSampleDraft first = prepared[i].view;
        DmSamplePreset *preset = &m->presets[m->preset_count++];
        snprintf(preset->id, sizeof(preset->id), "%s", prepared[i].preset);
        snprintf(preset->name, sizeof(preset->name), "%.*s", (int)first.edit.name.length,
                 first.edit.name.data);
        preset->drums = first.edit.drums != 0;
        preset->first_zone = m->zone_count;
        preset->suggested_program = preset->drums ? 0 : (uint32_t)(first.edit.program + 1);
        bool drum_keys[128] = {0};

        if (preset->drums && !first.preset_id.length) {
            snprintf(preset->name, sizeof(preset->name), "%s", DM_TEXT_DRUM_KIT);
        }

        for (uint32_t j = i; j < total; ++j) {
            if (prepared[j].used || strcmp(prepared[j].preset, preset->id) ||
                (prepared[j].view.edit.drums != 0) != preset->drums) {
                continue;
            }

            const DmeSampleZoneParams *v = &prepared[j].view.edit.zone;

            if (preset->drums && !prepared[j].view.preset_id.length && drum_keys[v->key_lo]) {
                snprintf(w->error, sizeof(w->error), "%s", DM_TEXT_SAMPLE_DUPLICATE_DRUM_KEY);
                ok = false;
                break;
            }

            drum_keys[v->key_lo] |= preset->drums && !prepared[j].view.preset_id.length;
            uint32_t at = m->zone_count++;
            m->zones[at] =
                (DmSampleZone){.asset = prepared[j].asset,
                               .root = (uint8_t)v->root,
                               .key_lo = (uint8_t)v->key_lo,
                               .key_hi = (uint8_t)v->key_hi,
                               .velocity_lo = (uint8_t)v->velocity_lo,
                               .velocity_hi = (uint8_t)v->velocity_hi,
                               .cut_group = (uint8_t)v->cut_group,
                               .cut_by = (uint8_t)v->cut_by,
                               .cents = v->cents,
                               .gain_db = v->gain_db,
                               .pan = v->pan,
                               .envelope = {.delay = v->delay,
                                            .attack = v->attack,
                                            .hold = v->hold,
                                            .decay = v->decay,
                                            .release = v->release,
                                            .sustain = v->sustain},
                               .loop_mode = (DmSampleLoopMode)v->loop_mode,
                               .loop = {(size_t)v->loop_start, (size_t)v->loop_end},
                               .tone = v->tone,
                               .playback = {(size_t)v->playback_start, (size_t)v->playback_end},
                               .playback_set = v->playback_set != 0};
            w->links[prepared[j].link].zone = preset->zone_count;
            ++preset->zone_count;
            prepared[j].used = true;
        }
    }

    free(prepared);

    if (!ok) {
        return false;
    }

    if (!m->preset_count) {
        snprintf(w->error, sizeof(w->error), "%s", DM_TEXT_SAMPLE_SAVE_EMPTY);
        return false;
    }

    return true;
}

static void begin_load(Editor *e, const DmSampleCatalogEntry *entry) {
    DmEditorSamples *s = e->samples;
    DmNativeTool *tool = tool_for(e);
    const DmeSamplesApi *api = samples_api(tool);
    SampleWork *w = &s->work;

    if (w->thread) {
        return;
    }

    const char *root = entry ? (entry->writable_root ? s->writable : s->program) : "";
    const char *id = entry ? entry->id : "";

    if (!strcmp(root, s->current_root) && !strcmp(id, s->update_id)) {
        return;
    }

    w->api = api;
    w->loading = true;
    snprintf(w->root, sizeof(w->root), "%s", root);
    snprintf(w->set_id, sizeof(w->set_id), "%s", id);
    snprintf(w->set_name, sizeof(w->set_name), "%s", entry ? entry->name : "");
    w->import = api->create_load(tool->instance, (DmeString){root, strlen(root)},
                                 (DmeString){id, strlen(id)}, w->error, sizeof(w->error));

    if (w->import) {
        w->thread = SDL_CreateThread(sample_worker, DM_TEXT_SAMPLE_LOADING, w);
    }

    if (!w->thread) {
        ed_message(e, *w->error ? w->error : DM_TEXT_SAMPLE_IMPORT_MEMORY);
        work_free(w);
    } else {
        dm_audio_sample_audition_stop(e->audio);
        s->held_preview = false;
    }
}

static void save_set(Editor *e, DmEditorSamples *s, const DmNativeTool *tool,
                     const DmeSamplesApi *api) {
    if (s->work.thread || !*s->set_name) {
        return;
    }

    ed_sample_catalog_refresh(e);
    SampleWork *w = &s->work;

    if (prepare_publication(s, tool, api)) {
        w->thread = SDL_CreateThread(sample_worker, DM_TEXT_SAMPLE_SAVE_SET, w);
    }

    if (!w->thread) {
        ed_message(e, *w->error ? w->error : DM_TEXT_SAMPLE_LIBRARY_WRITE);
        work_free(w);
    }
}

static void set_controls(Editor *e, DmEditorSamples *s, const DmNativeTool *tool,
                         const DmeSamplesApi *api) {
    float width = ui_available().x;
    float apply = ui_button_row_width((const char *const[]){DM_TEXT_APPLY}, 1);
    float combo = fminf(180, width * .3f);
    const DmSampleCatalogEntry *current = find_set(s, s->update_id);
    ui_width(combo);

    /* An unsaved draft, including the first one before any set exists, is a new set. */
    if (ui_combo(DM_TEXT_SAMPLE_SET_SELECT, current ? current->name : DM_TEXT_SAMPLE_NEW_SET)) {
        if (ui_select(DM_TEXT_SAMPLE_NEW_SET, !*s->update_id, 0, 0)) {
            begin_load(e, NULL);
        }

        for (size_t i = 0; i < s->catalog.count; ++i) {
            const DmSampleCatalogEntry *entry = &s->catalog.entries[i];
            ui_id((int)i);

            if (ui_select(entry->name, entry == current, 0, 0)) {
                begin_load(e, entry);
            }

            ui_pop_id();
        }

        ui_combo_end();
    }

    ui_tip(DM_TEXT_SAMPLE_SET_SELECT_HELP);
    ui_same();
    ui_width(fmaxf(30, width - combo - apply - 2 * ui_item_spacing_x()));
    bool submit = ui_input_submit(DM_TEXT_SAMPLE_SET_NAME, s->set_name, sizeof(s->set_name));
    ui_same();
    ui_disabled(!*s->set_name || !api->count(tool->instance));

    if (ui_button(DM_TEXT_APPLY, apply, 0) || submit) {
        save_set(e, s, tool, api);
    }

    ui_disabled_end();
}

typedef struct {
    const DmeSamplesApi *api;
    const void *instance;
    uint32_t draft;
} AuditionSource;

static int32_t DME_CALL audition_read(void *user, uint32_t asset, uint64_t start, uint64_t frames,
                                      float *pcm, uint64_t capacity) {
    const AuditionSource *s = user;
    return asset ? DME_INVALID
                 : s->api->read_pcm(s->instance, s->draft, start, frames, pcm, capacity);
}

static bool audition(Editor *e, bool sustained) {
    DmEditorSamples *s = e->samples;
    DmNativeTool *tool = tool_for(e);
    const DmeSamplesApi *api = samples_api(tool);
    DmeSampleDraft draft;

    if (!api || api->draft(tool->instance, s->selected, &draft) != DME_OK || draft.error.length) {
        return false;
    }

    DmeSampleAsset asset = {.size = sizeof(asset),
                            .rate = draft.rate,
                            .channels = draft.channels,
                            .frames = draft.frames};
    DmeSampleZone zone = {.size = sizeof(zone), .params = draft.edit.zone};
    DmeSamplePreset preset = {.size = sizeof(preset),
                              .zone_count = 1,
                              .flags = draft.edit.drums ? DME_SAMPLE_PERCUSSION : 0,
                              .name = draft.edit.name};
    DmeContext context = {.sample_asset_count = 1,
                          .sample_zone_count = 1,
                          .sample_preset_count = 1,
                          .sample_assets = &asset,
                          .sample_zones = &zone,
                          .sample_presets = &preset};
    AuditionSource source = {api, tool->instance, s->selected};
    DmeHost host = {.user = &source, .read_sample = audition_read};
    int key = (int)fmin(zone.params.key_hi, fmax(zone.params.key_lo, zone.params.root));
    char error[512];

    if (!dm_audio_sample_audition(e->audio, &context, &host, key, sustained,
                                  zone.params.loop_mode != DME_SAMPLE_NO_LOOP, error,
                                  sizeof(error))) {
        ed_message(e, error);
        return false;
    }

    s->held_preview = sustained;
    return true;
}

bool ed_samples_history(Editor *e, bool redo, bool apply) {
    DmEditorSamples *s = e->samples;
    DmNativeTool *tool = tool_for(e);
    const DmeSamplesApi *api = samples_api(tool);

    if (!s || !s->open || !api || !ui_window_focused(DM_TEXT_SAMPLE_TOOL_NAME)) {
        return false;
    }

    DmeSampleDraft before = {0};
    api->draft(tool->instance, s->selected, &before);

    if (apply && !s->work.thread && api->undo(tool->instance, redo) == DME_OK) {
        uint32_t count = api->count(tool->instance);
        s->selected = count ? SDL_min(s->selected, count - 1) : 0;
        s->envelope_handle = s->loop_handle = -1;
        s->loop_warning = false;

        if (!count) {
            dm_audio_sample_audition_stop(e->audio);
            s->held_preview = false;
        }

        for (uint32_t i = 0; i < count; ++i) {
            DmeSampleDraft draft;

            if (api->draft(tool->instance, i, &draft) != DME_OK || draft.error.length) {
                continue;
            }

            if (i == s->selected) {
                if (before.token == draft.token) {
                    dm_audio_sample_audition_edit(e->audio, &draft.edit.zone);
                } else {
                    dm_audio_sample_audition_stop(e->audio);
                    s->held_preview = false;
                }
            }

            for (size_t j = 0; j < s->link_count; ++j) {
                const PublishedDraft *link = &s->links[j];

                if (link->token == draft.token && !strcmp(link->set_id, s->update_id) &&
                    !strcmp(link->locator, s->current_root)) {
                    dm_audio_sample_edit(e->audio, link->locator, link->set_id, link->preset_id,
                                         link->zone, link->asset_id, &draft.edit.zone);
                }
            }
        }
    }

    return apply || (!s->work.thread && api->can_undo(tool->instance, redo));
}

bool ed_samples_key(Editor *e, const SDL_KeyboardEvent *key) {
    DmEditorSamples *s = e->samples;

    if (!s || !s->open) {
        return false;
    }

    if (s->picker || s->delete_sample) {
        if (s->delete_sample && key->scancode == SDL_SCANCODE_ESCAPE) {
            s->delete_sample = false;
            return true;
        }

        return key->scancode == SDL_SCANCODE_SPACE;
    }

    if (!ui_window_focused(DM_TEXT_SAMPLE_TOOL_NAME)) {
        return false;
    }

    if (ui_popup_open() || ui_text_active()) {
        return key->scancode == SDL_SCANCODE_SPACE || key->scancode == SDL_SCANCODE_RETURN ||
               key->scancode == SDL_SCANCODE_KP_ENTER;
    }

    if ((key->mod & SDL_KMOD_CTRL) &&
        (key->scancode == SDL_SCANCODE_Z || key->scancode == SDL_SCANCODE_Y)) {
        ed_samples_history(e, key->scancode == SDL_SCANCODE_Y || (key->mod & SDL_KMOD_SHIFT), true);
        return true;
    }

    if (key->scancode == SDL_SCANCODE_ESCAPE) {
        ed_samples_close(e);
        return true;
    }

    if (key->scancode != SDL_SCANCODE_SPACE ||
        (key->mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
        return (key->mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0;
    }

    if (!key->repeat && !s->work.thread) {
        bool sustained = (key->mod & SDL_KMOD_SHIFT) != 0;

        if (sustained && s->held_preview) {
            dm_audio_sample_audition_stop(e->audio);
            s->held_preview = false;
        } else {
            audition(e, sustained);
        }
    }

    return true;
}

static bool number(const char *label, uint32_t *value, int lo, int hi) {
    int next = (int)*value;
    bool changed = ui_int(label, &next, lo, hi);
    *value = (uint32_t)next;
    return changed;
}

static bool decimal_step(const char *label, double *value, float lo, float hi, float step) {
    float next = (float)*value;

    if (!ui_float_step(label, &next, lo, hi, step)) {
        return false;
    }

    *value = next;
    return true;
}

static bool decimal(const char *label, double *value, float lo, float hi) {
    return decimal_step(label, value, lo, hi, .1f);
}

static const char *file_name(const char *path) {
    const char *leaf = path;

    for (const char *c = path; *c; ++c) {
        if (*c == '/' || *c == '\\') {
            leaf = c + 1;
        }
    }

    return leaf;
}

static bool category_row(DmeSampleEdit *edit) {
    float width = ui_available().x;
    float category = fminf(160, fmaxf(128, width * .60f));
    category = fminf(category, fmaxf(0, width - 60 - ui_item_spacing_x()));
    int *value = edit->drums ? &edit->drum_key : &edit->program;
    const char *name = *value < 0    ? DM_TEXT_SAMPLE_NO_SUGGESTION
                       : edit->drums ? dm_drum_name(*value)
                                     : dm_program_name(*value);
    bool changed = false;
    ui_width(fmaxf(36, width - category - ui_item_spacing_x()));

    if (ui_combo(DM_TEXT_SAMPLE_MAPPING, name)) {
        int first = edit->drums ? 0 : -1;

        for (int i = first; i < 128; ++i) {
            char label[128];
            name = i < 0         ? DM_TEXT_SAMPLE_NO_SUGGESTION
                   : edit->drums ? dm_drum_name(i)
                                 : dm_program_name(i);
            snprintf(label, sizeof(label), DM_TEXT_PROGRAM_NUMBERED, name, i);

            if (ui_select(i < 0 ? name : label, *value == i, 0, 0)) {
                *value = i;

                if (edit->drums) {
                    edit->zone.key_lo = edit->zone.key_hi = (uint32_t)i;
                }

                changed = true;
            }
        }

        ui_combo_end();
    }

    ui_same();
    unsigned drums =
        (unsigned)ui_button_pair(DM_TEXT_MELODY, DM_TEXT_DRUMS, (int)edit->drums, category);

    if (drums != edit->drums) {
        edit->drums = drums;

        if (drums) {
            edit->melodic_key_lo = edit->zone.key_lo;
            edit->melodic_key_hi = edit->zone.key_hi;

            if (edit->drum_key < 0) {
                edit->drum_key = 36;
            }

            edit->zone.key_lo = edit->zone.key_hi = (uint32_t)edit->drum_key;
        } else {
            edit->zone.key_lo = edit->melodic_key_lo;
            edit->zone.key_hi = edit->melodic_key_hi;
        }

        changed = true;
    }

    return changed;
}

static bool waveform(Editor *e, DmEditorSamples *s, const DmNativeTool *tool,
                     const DmeSamplesApi *api, const DmeSampleDraft *draft,
                     DmeSampleZoneParams *z) {
    DmeSamplePeak peaks[DM_SAMPLE_BATCH_PEAKS];
    uint32_t count = draft->peak_count;
    UiPoint at = ui_cursor();
    UiRect rect = {at.x, at.y, ui_available().x, 106};
    ui_canvas("##sample-waveform", rect.w, rect.h);
    bool hovered = ui_hover();

    /* The tip would cover the waveform while a boundary is being dragged. */
    if (!ui_mouse().left) {
        ui_tip(DM_TEXT_SAMPLE_PLAYBACK_HELP);
    }
    ui_rect(rect, BG, 3);
    double frames = (double)draft->frames;
    bool loop = z->loop_mode != DME_SAMPLE_NO_LOOP;
    float lo = rect.x + (float)(rect.w * (double)z->loop_start / frames);
    float hi = rect.x + (float)(rect.w * (double)z->loop_end / frames);

    if (loop) {
        ui_rect((UiRect){lo, rect.y, hi - lo, rect.h}, (e->accent & 0xffffffu) | 0x28000000u, 0);
    }

    if (count && count <= SDL_arraysize(peaks) &&
        api->read_peaks(tool->instance, s->selected, 0, count, peaks) == DME_OK) {
        float band = rect.h / (float)draft->channels;

        for (uint32_t channel = 0; channel < draft->channels; ++channel) {
            float mid = rect.y + band * ((float)channel + .5f);
            ui_line((UiPoint){rect.x, mid}, (UiPoint){rect.x + rect.w, mid}, GRID, 1);
            float scale = draft->absolute_peak > 0 ? .46f * band / draft->absolute_peak : 0;

            uint32_t columns = (uint32_t)fmaxf(1, ceilf(rect.w * ui_scale()));
            columns = columns < count ? columns : count;

            for (uint32_t i = 0; i < columns; ++i) {
                uint32_t first = (uint32_t)((uint64_t)i * count / columns);
                uint32_t end = (uint32_t)((uint64_t)(i + 1) * count / columns);
                float maximum = peaks[first].maximum[channel],
                      minimum = peaks[first].minimum[channel];

                for (uint32_t bin = first + 1; bin < end; ++bin) {
                    maximum = fmaxf(maximum, peaks[bin].maximum[channel]);
                    minimum = fminf(minimum, peaks[bin].minimum[channel]);
                }

                float x = rect.x + rect.w * ((float)i + .5f) / (float)columns;
                UiPoint top = {x, mid - scale * maximum};
                UiPoint bottom = {x, mid - scale * minimum};
                bottom.y = fmaxf(top.y + 1 / ui_scale(), bottom.y);
                ui_line(top, bottom, e->accent, 1 / ui_scale());
            }
        }
    }

    bool changed = false;
    UiMouse mouse = ui_mouse();

    if (loop) {
        ui_line((UiPoint){lo, rect.y}, (UiPoint){lo, rect.y + rect.h}, GOLD, 2);
        ui_line((UiPoint){hi, rect.y}, (UiPoint){hi, rect.y + rect.h}, GOLD, 2);

        if (s->loop_warning) {
            ui_dim(DM_TEXT_SAMPLE_LOOP_WARNING);
        }
    }

    uint64_t playback_end = z->playback_end ? z->playback_end : draft->frames;
    float play_lo = rect.x + (float)(rect.w * (double)z->playback_start / frames);
    float play_hi = rect.x + (float)(rect.w * (double)playback_end / frames);

    if (z->playback_set || z->playback_start || playback_end != draft->frames) {
        const uint32_t color = 0xffe0b060u;
        ui_line((UiPoint){play_lo, rect.y}, (UiPoint){play_lo, rect.y + rect.h}, color, 2);
        ui_line((UiPoint){play_hi, rect.y}, (UiPoint){play_hi, rect.y + rect.h}, color, 2);
    }

    if (hovered && mouse.clicked && draft->frames >= 2) {
        if (ed_gesture_held(e, GESTURE_PLAY_RANGE, mouse)) {
            s->loop_handle = fabsf(mouse.pos.x - play_lo) <= fabsf(mouse.pos.x - play_hi) ? 2 : 3;
        } else if (loop) {
            s->loop_handle = fabsf(mouse.pos.x - lo) <= fabsf(mouse.pos.x - hi) ? 0 : 1;
        }
    }

    if (s->loop_handle >= 0 && mouse.left && rect.w > 0) {
        bool playback = s->loop_handle >= 2;
        uint64_t start = playback ? z->playback_start : z->loop_start;
        uint64_t end = playback ? playback_end : z->loop_end;
        double fraction = fmax(0, fmin(1, (mouse.pos.x - rect.x) / rect.w));
        uint64_t frame = (uint64_t)llround(fraction * frames);

        if (!(s->loop_handle & 1)) {
            start = frame < end - 2 ? frame : end - 2;
        } else {
            end = frame > start + 2 ? frame : start + 2;
        }

        if (!playback || ed_gesture_held(e, GESTURE_PLAY_RANGE, mouse)) {
            s->loop_warning = !ed_gesture_held(e, GESTURE_FREE, mouse) &&
                              api->snap_loop(tool->instance, s->selected, &start, &end) != DME_OK;

            if (playback) {
                if (loop) {
                    start = SDL_min(start, z->loop_start);
                    end = SDL_max(end, z->loop_end);
                }

                changed |= !z->playback_set || z->playback_start != start || playback_end != end;
                z->playback_start = start;
                z->playback_end = end;
                z->playback_set = 1;
            } else {
                start = SDL_max(start, z->playback_start);
                end = SDL_min(end, playback_end);
                changed |= z->loop_start != start || z->loop_end != end;
                z->loop_start = start;
                z->loop_end = end;
            }
        }
    }

    if (!mouse.left) {
        s->loop_handle = -1;
    }

    static const char *const labels[] = {DM_TEXT_SAMPLE_LOOP_OFF, DM_TEXT_SAMPLE_LOOP_CONTINUOUS,
                                         DM_TEXT_SAMPLE_LOOP_SUSTAIN};
    static const char *const tips[] = {DM_TEXT_SAMPLE_LOOP_OFF_HELP,
                                       DM_TEXT_SAMPLE_LOOP_CONTINUOUS_HELP,
                                       DM_TEXT_SAMPLE_LOOP_SUSTAIN_HELP};
    float width = (ui_available().x - 2 * ui_item_spacing_x()) / 3;

    for (unsigned mode = 0; mode < SDL_arraysize(labels); ++mode) {
        if (mode) {
            ui_same();
        }

        ui_disabled(mode && draft->frames < 2);
        ui_accent(z->loop_mode == mode);

        if (ui_button(labels[mode], width, 0) && mode != z->loop_mode) {
            z->loop_mode = mode;
            s->loop_warning = false;

            if (mode && (z->loop_end < z->loop_start + 2 || z->loop_start < z->playback_start ||
                         z->loop_end > playback_end)) {
                z->loop_start = z->playback_start;
                z->loop_end = playback_end;
            }

            changed = true;
        }

        ui_accent_end();
        ui_disabled_end();
        ui_tip(tips[mode]);
    }

    return changed;
}

static bool envelope(Editor *e, DmEditorSamples *s, DmeSampleZoneParams *z) {
    static const char *const labels[] = {DM_TEXT_SAMPLE_DELAY, DM_TEXT_SAMPLE_ATTACK,
                                         DM_TEXT_SAMPLE_HOLD, DM_TEXT_SAMPLE_DECAY,
                                         DM_TEXT_SAMPLE_RELEASE};
    double *phases[] = {&z->delay, &z->attack, &z->hold, &z->decay, &z->release};
    static const float limits[] = {20, 100, 20, 100, 100};
    UiPoint at = ui_cursor();
    UiRect rect = {at.x, at.y, ui_available().x, 114};
    ui_canvas("##sample-envelope", rect.w, rect.h);
    ui_rect(rect, BG, 3);

    for (int grid = 1; grid < 5; ++grid) {
        float x = rect.x + rect.w * grid / 5, y = rect.y + rect.h * grid / 5;
        ui_line((UiPoint){x, rect.y}, (UiPoint){x, rect.y + rect.h}, GRID, 1);
        ui_line((UiPoint){rect.x, y}, (UiPoint){rect.x + rect.w, y}, GRID, 1);
    }

    float left = rect.x + 5, right = rect.x + rect.w - 5;
    float top = rect.y + 8, bottom = rect.y + rect.h - 8;
    float sustain = bottom - (float)z->sustain * (bottom - top);
    float gap = fmaxf(1, (right - left) * .08f);
    /* Fixed logarithmic spacing for each duration keeps earlier handles stationary.
       The sum of the maximum widths fits the canvas without value-dependent rescaling. */
    double extent = 0;

    for (unsigned i = 0; i < SDL_arraysize(phases); ++i) {
        extent += log1p(limits[i] / .02);
    }

    double scale = fmax(1, right - left - 5 * gap) / extent;
    UiPoint handles[5];
    float heights[] = {bottom, top, top, sustain, bottom};
    float x = left;

    for (unsigned i = 0; i < SDL_arraysize(handles); ++i) {
        x += (i ? gap : 0) + (float)(log1p(*phases[i] / .02) * scale);

        if (i == 4) {
            ui_line(handles[3], (UiPoint){handles[3].x + gap, sustain}, e->accent, 2);
            x += gap;
        }

        handles[i] = (UiPoint){x, heights[i]};
        UiPoint previous = i ? handles[i - 1] : (UiPoint){left, bottom};

        if (i == 4) {
            previous.x += gap;
        }

        ui_line(previous, handles[i], e->accent, 2);
        ui_circle(handles[i], 5, BG);
        ui_circle_outline(handles[i], 5, e->accent);
    }

    bool changed = false;
    UiMouse mouse = ui_mouse();

    if (ui_hover() && mouse.clicked) {
        for (int i = 0; i < 5; ++i) {
            if (hypotf(mouse.pos.x - handles[i].x, mouse.pos.y - handles[i].y) <= 9) {
                s->envelope_handle = i;
                s->drag_start = mouse.pos.x;
                s->drag_value = *phases[i];
                s->drag_width = (float)scale;
                break;
            }
        }
    }

    if (mouse.left && s->envelope_handle >= 0) {
        int i = s->envelope_handle;
        double value =
            fmax(0, fmin(limits[i], .02 * expm1(log1p(s->drag_value / .02) +
                                                (mouse.pos.x - s->drag_start) / s->drag_width)));
        changed = value != *phases[i];
        *phases[i] = value;
    } else if (!mouse.left) {
        s->envelope_handle = -1;
    }

    for (unsigned i = 0; i < SDL_arraysize(phases); ++i) {
        changed |= decimal(labels[i], phases[i], 0, limits[i]);
    }

    changed |= decimal(DM_TEXT_SAMPLE_SUSTAIN, &z->sustain, 0, 1);
    return changed;
}

/* Undo and Redo beside Reset to import values; the request runs after the panel is drawn,
   since the panel works on a copy of the current edit. */
static void history_buttons(DmEditorSamples *s, const DmNativeTool *tool,
                            const DmeSamplesApi *api) {
    static const char *const labels[] = {DM_TEXT_SAMPLE_RESET_IMPORT, DM_TEXT_SAMPLE_UNDO,
                                         DM_TEXT_SAMPLE_REDO};

    /* A narrow panel puts them on the next line instead of past its edge. */
    if (ui_button_row_width(labels, 3) <= ui_available().x) {
        ui_same();
    }

    ui_disabled(!api->can_undo(tool->instance, false));

    if (ui_button(DM_TEXT_SAMPLE_UNDO, 0, 0)) {
        s->history_request = 1;
    }

    ui_disabled_end();
    ui_same();
    ui_disabled(!api->can_undo(tool->instance, true));

    if (ui_button(DM_TEXT_SAMPLE_REDO, 0, 0)) {
        s->history_request = 2;
    }

    ui_disabled_end();
}

static void sample_details(Editor *e, DmEditorSamples *s, const DmNativeTool *tool,
                           const DmeSamplesApi *api) {
    DmeSampleDraft draft;

    if (api->draft(tool->instance, s->selected, &draft) != DME_OK) {
        return;
    }

    ui_wrap(draft.path.data);

    if (draft.error.length) {
        ui_wrap(draft.error.data);
        return;
    }

    char name[DM_SAMPLE_NAME_CAP], geometry[128];
    DmeSampleEdit edit = draft.edit;
    DmeSampleZoneParams *z = &edit.zone;
    snprintf(name, sizeof(name), "%.*s", (int)edit.name.length, edit.name.data);
    snprintf(geometry, sizeof(geometry), DM_TEXT_SAMPLE_GEOMETRY, draft.format.data,
             draft.codec.data, draft.rate, draft.channels, (double)draft.frames / draft.rate);
    ui_dim(geometry);
    ui_width(ui_available().x);
    bool changed = ui_input(DM_TEXT_SAMPLE_NAME_EDIT, name, sizeof(name));
    changed |= category_row(&edit);

    if (ui_button(DM_TEXT_SAMPLE_RESET_IMPORT, 0, 0) &&
        api->reset_edit(tool->instance, s->selected) == DME_OK &&
        api->draft(tool->instance, s->selected, &draft) == DME_OK) {
        edit = draft.edit;
        snprintf(name, sizeof(name), "%.*s", (int)edit.name.length, edit.name.data);
        s->envelope_handle = s->loop_handle = -1;
        s->loop_warning = false;
        changed = true;
    }

    history_buttons(s, tool, api);
    ui_text(DM_TEXT_SAMPLE_LOOP_TITLE);
    ui_separator();
    bool loop_changed = waveform(e, s, tool, api, &draft, z);
    ui_text(DM_TEXT_SAMPLE_ENVELOPE_TITLE);
    ui_separator();
    changed |= envelope(e, s, z);

    static const char *const roots[] = {DM_TEXT_SAMPLE_ROOT_DEFAULT, DM_TEXT_SAMPLE_ROOT_SIGNAL,
                                        DM_TEXT_SAMPLE_ROOT_FILENAME, DM_TEXT_SAMPLE_ROOT_EMBEDDED};
    const char *source = roots[draft.root_source < SDL_arraysize(roots) ? draft.root_source : 0];
    if (draft.root_source == DME_ROOT_SIGNAL) {
        snprintf(geometry, sizeof(geometry), DM_TEXT_SAMPLE_PITCH_INFO, source,
                 (unsigned)lround(draft.confidence * 100));
    } else {
        snprintf(geometry, sizeof(geometry), DM_TEXT_SAMPLE_PITCH_SOURCE, source);
    }
    ui_dim(geometry);

    if (draft.pitch_conflict) {
        ui_wrap(DM_TEXT_SAMPLE_PITCH_CONFLICT);
    }

    changed |= number(DM_TEXT_SAMPLE_ROOT, &z->root, 0, 127);
    char root_name[32];
    ed_pitch_text(root_name, sizeof(root_name), (float)z->root);
    ui_same();
    ui_dim(root_name);
    changed |= ui_knob(DM_TEXT_SAMPLE_FINE, &z->cents, -100, 100, 0);
    ui_tip(DM_TEXT_SAMPLE_FINE_HELP);
    ui_same();
    changed |= ui_knob(DM_TEXT_SAMPLE_COARSE_TUNE, &z->tone.coarse, -48, 48, 0);
    ui_tip(DM_TEXT_SAMPLE_COARSE_HELP);
    z->tone.coarse = round(z->tone.coarse);
    ui_same();
    changed |= ui_knob(DM_TEXT_SAMPLE_FINE_TUNE, &z->tone.fine, -100, 100, 0);
    ui_tip(DM_TEXT_SAMPLE_FINE_TUNE_HELP);
    bool tracking = z->tone.custom_tracking != 0;

    if (ui_check(DM_TEXT_SAMPLE_CUSTOM_TRACKING, &tracking)) {
        z->tone.custom_tracking = tracking;

        changed = true;
    }

    if (tracking) {
        changed |=
            ui_knob(DM_TEXT_SAMPLE_TRACKING, &z->tone.tracking, 0, 200, edit.drums ? 0 : 100);
    }

    changed |= number(DM_TEXT_SAMPLE_CUT, &z->cut_group, 0, 127);
    ui_tip(DM_TEXT_SAMPLE_CUT_HELP);
    changed |= number(DM_TEXT_SAMPLE_CUT_BY, &z->cut_by, 0, 127);
    ui_tip(DM_TEXT_SAMPLE_CUT_BY_HELP);

    if (ui_section(DM_TEXT_SAMPLE_RANGES)) {
        if (!edit.drums) {
            changed |= number(DM_TEXT_SAMPLE_KEY_LOW, &z->key_lo, 0, (int)z->key_hi);
            changed |= number(DM_TEXT_SAMPLE_KEY_HIGH, &z->key_hi, (int)z->key_lo, 127);
            edit.melodic_key_lo = z->key_lo;
            edit.melodic_key_hi = z->key_hi;
        }

        changed |= number(DM_TEXT_SAMPLE_VELOCITY_LOW, &z->velocity_lo, 1, (int)z->velocity_hi);
        changed |= number(DM_TEXT_SAMPLE_VELOCITY_HIGH, &z->velocity_hi, (int)z->velocity_lo, 127);
        changed |= decimal(DM_TEXT_SAMPLE_GAIN, &z->gain_db, -96, 12);
        changed |= decimal(DM_TEXT_SAMPLE_PAN, &z->pan, -1, 1);
    }

    if (ui_section(DM_TEXT_SAMPLE_FILTER)) {
        if (decimal_step(DM_TEXT_SAMPLE_CUTOFF, &z->tone.cutoff, 0, 20000, 100)) {
            z->tone.cutoff = z->tone.cutoff > 0 ? fmax(20, z->tone.cutoff) : 0;
            changed = true;
        }

        changed |= decimal(DM_TEXT_SAMPLE_RESONANCE, &z->tone.resonance, 0, 24);
        changed |= decimal(DM_TEXT_SAMPLE_FILTER_DEPTH, &z->tone.filter_depth, -4800, 4800);
    }

    if (ui_section(DM_TEXT_SAMPLE_VIBRATO_EFFECTS)) {
        if (decimal(DM_TEXT_SAMPLE_VIBRATO_RATE, &z->tone.vibrato_rate, 0, 20)) {
            z->tone.vibrato_rate = z->tone.vibrato_rate > 0 ? fmax(.1, z->tone.vibrato_rate) : 0;
            changed = true;
        }

        changed |= decimal(DM_TEXT_SAMPLE_VIBRATO_DEPTH, &z->tone.vibrato_depth, 0, 200);
        changed |= decimal(DM_TEXT_SAMPLE_CHORUS, &z->tone.chorus, 0, 1);
        changed |= decimal(DM_TEXT_SAMPLE_REVERB, &z->tone.reverb, 0, 1);
    }

    if ((changed || loop_changed) && *name) {
        edit.name = (DmeString){name, strlen(name)};

        if (api->edit(tool->instance, s->selected, &edit) == DME_OK) {
            dm_audio_sample_audition_edit(e->audio, z);

            for (size_t i = 0; i < s->link_count; ++i) {
                const PublishedDraft *link = &s->links[i];

                if (link->token == draft.token && !strcmp(link->set_id, s->update_id) &&
                    !strcmp(link->locator, s->current_root)) {
                    dm_audio_sample_edit(e->audio, link->locator, link->set_id, link->preset_id,
                                         link->zone, link->asset_id, z);
                }
            }
        }
    }
}

static void sample_rows(Editor *e, DmEditorSamples *s, const DmNativeTool *tool,
                        const DmeSamplesApi *api, uint32_t count) {
    float available = ui_available().x;
    float footer = ui_frame_height() + ui_item_spacing_y();

    if (s->work.thread) {
        footer += 2 * (ui_frame_height() + ui_item_spacing_y());
    }

    float height = fmaxf(40, ui_available().y - footer);
    UiPoint panels = ui_cursor();
    float minimum = fminf(110, available * .30f), maximum = available * .58f;

    if (!s->split) {
        s->split = available * .30f;
    }

    s->split = fmaxf(minimum, fminf(s->split, maximum));

    if (ui_child("##sample-list", s->split,
                 fmaxf(40, height - 2 * (ui_frame_height() + ui_item_spacing_y())), true)) {
        for (uint32_t i = 0; i < count; ++i) {
            DmeSampleDraft draft;

            if (api->draft(tool->instance, i, &draft) != DME_OK) {
                continue;
            }

            char label[DM_PATH];
            snprintf(label, sizeof(label), DM_TEXT_SAMPLE_LIST_ROW, i + 1,
                     draft.source_name.length ? draft.source_name.data : file_name(draft.path.data),
                     draft.error.length ? DM_TEXT_SAMPLE_ROW_ERROR : "");

            ui_id((int)i);

            if (ui_select(label, i == s->selected, 0, 0) && i != s->selected) {
                dm_audio_sample_audition_stop(e->audio);
                s->selected = i;
                s->held_preview = s->loop_warning = false;
                s->envelope_handle = s->loop_handle = -1;
            }

            ui_tip(draft.path.data);

            if (ui_context(DM_TEXT_SAMPLE_ACTIONS)) {
                if (ui_menu_item(DM_TEXT_SAMPLE_OPEN_LOCATION, NULL, true)) {
                    ed_open_file_location(e, draft.path.data);
                }

                if (ui_menu_item(DM_TEXT_DELETE, NULL, true)) {
                    s->remove = (int)i;
                    s->delete_sample = true;
                }

                ui_popup_end();
            }

            ui_pop_id();
        }
    }

    ui_child_end();

    if (ui_button(DM_TEXT_SAMPLE_CHOOSE_FILES, s->split, 0)) {
        choose_samples(e, false);
    }

    if (ui_button(DM_TEXT_SAMPLE_CHOOSE_FOLDER, s->split, 0)) {
        choose_samples(e, true);
    }

    bool confirmed = false;

    if (s->delete_sample) {
        static const char *const buttons[] = {DM_TEXT_CANCEL, DM_TEXT_DELETE};
        char text[DM_PATH + 256];
        DmeSampleDraft draft;
        const char *name = "";

        if (s->remove >= 0 && api->draft(tool->instance, (uint32_t)s->remove, &draft) == DME_OK) {
            name = draft.source_name.length ? draft.source_name.data : file_name(draft.path.data);
        }

        snprintf(text, sizeof(text), "%s\n\n%s", DM_TEXT_SAMPLE_DELETE_WARNING, name);
        confirmed =
            ui_confirm(DM_TEXT_SAMPLE_DELETE_TITLE, &s->delete_sample, text, buttons, 2) == 1;
    }

    int removed = confirmed ? s->remove : -1;

    if (removed >= 0 && api->remove(tool->instance, (uint32_t)removed) == DME_OK) {
        if ((uint32_t)removed == s->selected) {
            dm_audio_sample_audition_stop(e->audio);
            s->held_preview = s->loop_warning = false;
            s->envelope_handle = s->loop_handle = -1;
        } else if ((uint32_t)removed < s->selected) {
            --s->selected;
        }

        uint32_t remaining = api->count(tool->instance);

        if (s->selected >= remaining) {
            s->selected = remaining ? remaining - 1 : 0;
        }
    }

    ui_position(panels.x + s->split + ui_item_spacing_x(), panels.y);
    ui_splitter(&s->split, minimum, maximum, height);
    ui_same();

    if (ui_child("##sample-details", 0, height, true)) {
        sample_details(e, s, tool, api);
    }

    ui_child_end();
    ui_position(panels.x, panels.y + height + ui_item_spacing_y());
}

void ed_samples_draw(Editor *e) {
    if (!e->export_plugins) {
        return;
    }

    DmNativeTool *tool = tool_for(e);
    const DmeSamplesApi *api = samples_api(tool);

    if (!api) {
        return;
    }

    if (tool->open_requested) {
        tool->open_requested = false;
        DmEditorSamples *s = state(e);

        if (s) {
            s->open = true;
            ui_dialog_raise(DM_TEXT_SAMPLE_TOOL_NAME);
        }
    }

    DmEditorSamples *s = e->samples;

    if (!s || !s->open) {
        return;
    }

    if (!s->initialized) {
        s->initialized = true;
        ed_sample_catalog_refresh(e);
        const DmSampleCatalogEntry *latest = NULL;
        SDL_Time modified = 0;

        for (size_t i = 0; i < s->catalog.count; ++i) {
            const DmSampleCatalogEntry *entry = &s->catalog.entries[i];
            if (!latest || entry->modified > (uint64_t)modified) {
                latest = entry;
                modified = (SDL_Time)entry->modified;
            }
        }

        if (latest && !api->count(tool->instance)) {
            begin_load(e, latest);
        }
    }

    api->history_group(tool->instance, ui_mouse().left);
    bool open = true;
    uint32_t count = api->count(tool->instance);

    /* Empty, the window fits its contents; with samples it opens larger and resizes. */
    UiDialog dialog = count ? (UiDialog){.width = 1000,
                                         .height = 580,
                                         .size = UI_RESIZABLE,
                                         .tool = true,
                                         .layout = 1}
                            : (UiDialog){.width = 760, .tool = true};

    if (ui_dialog(DM_TEXT_SAMPLE_TOOL_NAME, &open, dialog)) {
        s->rect = ui_window_rect();
        ui_disabled(s->work.thread != NULL);

        if (count || s->catalog.count) {
            set_controls(e, s, tool, api);
        }

        if (!count) {
            if (ui_button(DM_TEXT_SAMPLE_DROP, ui_available().x, 140)) {
                choose_samples(e, false);
            }

            float half = (ui_available().x - ui_item_spacing_x()) * .5f;

            if (ui_button(DM_TEXT_SAMPLE_CHOOSE_FILES, half, 0)) {
                choose_samples(e, false);
            }

            ui_same();

            if (ui_button(DM_TEXT_SAMPLE_CHOOSE_FOLDER, half, 0)) {
                choose_samples(e, true);
            }
        } else {
            sample_rows(e, s, tool, api, count);
            ui_disabled(!*s->set_name);

            if (ui_button(DM_TEXT_SAVE, ui_available().x, 0)) {
                save_set(e, s, tool, api);
            }

            ui_disabled_end();
        }

        /* With no sample shown, Undo and Redo stay reachable here (after deleting the last one). */
        if (!count &&
            (api->can_undo(tool->instance, false) || api->can_undo(tool->instance, true))) {
            ui_disabled(!api->can_undo(tool->instance, false));

            if (ui_button(DM_TEXT_SAMPLE_UNDO, 0, 0)) {
                s->history_request = 1;
            }

            ui_disabled_end();
            ui_same();
            ui_disabled(!api->can_undo(tool->instance, true));

            if (ui_button(DM_TEXT_SAMPLE_REDO, 0, 0)) {
                s->history_request = 2;
            }

            ui_disabled_end();
        }

        if (s->history_request) {
            ed_samples_history(e, s->history_request == 2, true);
            s->history_request = 0;
        }

        ui_disabled_end();

        if (s->work.thread) {
            ui_progress(SDL_GetAtomicInt(&s->work.progress) / 100.0f, ui_available().x, 0,
                        s->work.loading ? DM_TEXT_SAMPLE_LOADING : DM_TEXT_SAMPLE_IMPORTING);

            if (ui_button(DM_TEXT_SAMPLE_CANCEL_WORK, ui_available().x, 0)) {
                SDL_SetAtomicInt(&s->work.cancel, 1);
            }
        }
    }

    ui_dialog_end();

    if (!open) {
        ed_samples_close(e);
    }
}
