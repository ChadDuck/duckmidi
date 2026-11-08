#include "editor_export_internal.h"
#include "text/en.h"
#include <limits.h>

static bool begin(Editor *e, bool inspection, int index, const char *path, bool install);
static bool refresh_catalog(Editor *e);

/* duckmidi-plugins.ini sits beside the preferences file. */
static bool folders_path(const Editor *e, char *out, size_t capacity) {
    const char *slash = strrchr(e->settings_path, '/'),
               *backslash = strrchr(e->settings_path, '\\');
    const char *leaf = backslash > slash ? backslash + 1 : slash ? slash + 1 : e->settings_path;
    int folder = (int)(leaf - e->settings_path);
    return *e->settings_path && snprintf(out, capacity, "%.*s%s", folder, e->settings_path,
                                         DM_TEXT_PLUGIN_SETTINGS_INI) < (int)capacity;
}

static int folder_entry(DmExportPlugins *h, const char *id, bool add) {
    for (size_t i = 0; i < h->folder_count; i++) {
        if (!strcmp(h->folders[i].id, id)) {
            return (int)i;
        }
    }

    if (!add || h->folder_count == DM_EXPORT_MAX_FORMATS ||
        strlen(id) >= sizeof(h->folders[0].id)) {
        return -1;
    }

    snprintf(h->folders[h->folder_count].id, sizeof(h->folders[0].id), "%s", id);
    h->folders[h->folder_count].repository[0] = 0;
    return (int)h->folder_count++;
}

/* Lines are "[exporter id]" and "repository=folder"; anything else is ignored. */
static void load_folders(Editor *e, DmExportPlugins *h) {
    char path[DM_PATH], error[256];
    DmBytes b = {0};

    if (!folders_path(e, path, sizeof(path)) || !dm_read(path, &b, error, sizeof(error))) {
        return;
    }

    int entry = -1;
    char *text = malloc(b.size + 1);

    if (text) {
        memcpy(text, b.data, b.size);
        text[b.size] = 0;

        for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            size_t length = strlen(line);

            if (*line == '[' && length > 2 && line[length - 1] == ']') {
                line[length - 1] = 0;
                entry = folder_entry(h, line + 1, true);
            } else if (entry >= 0 && !strncmp(line, "repository=", 11)) {
                snprintf(h->folders[entry].repository, DM_PATH, "%s", line + 11);
            }
        }
    }

    free(text);
    dm_bytes_free(&b);
}

static void save_folders(Editor *e, DmExportPlugins *h) {
    char path[DM_PATH], error[256];
    DmBytes b = {0};
    bool ok = folders_path(e, path, sizeof(path));

    for (size_t i = 0; ok && i < h->folder_count; i++) {
        const char *id = h->folders[i].id, *folder = h->folders[i].repository;

        if (*folder) {
            ok = dm_bytes(&b, "[", 1) && dm_bytes(&b, id, strlen(id)) &&
                 dm_bytes(&b, "]\nrepository=", 13) && dm_bytes(&b, folder, strlen(folder)) &&
                 dm_bytes(&b, "\n", 1);
        }
    }

    if (ok && !dm_write(path, b.data ? (const void *)b.data : "", b.size, error, sizeof(error))) {
        ed_message(e, error);
    }

    dm_bytes_free(&b);
}

DmExportPlugins *ed_export_host(Editor *e) {
    if (e->export_plugins) {
        return e->export_plugins;
    }

    DmExportPlugins *h = calloc(1, sizeof(*h));

    if (!h) {
        return NULL;
    }

    h->pending_format = -1;
    h->message_format = -1;
    h->folder_format = -1;
    h->keep_originals = true;
    h->history_width = 380;
    const char *base = SDL_GetBasePath();
    char directory[DM_PATH];

    if (!base ||
        snprintf(directory, sizeof(directory), "%splugins", base) >= (int)sizeof(directory)) {
        snprintf(h->registry.errors, sizeof(h->registry.errors),
                 DM_TEXT_CANNOT_LOCATE_EXPORTER_PLUGINS);
    } else {
        dm_export_registry_load(&h->registry, directory);
    }

    e->export_plugins = h;
    load_folders(e, h);
    return h;
}

size_t ed_export_count(Editor *e) {
    DmExportPlugins *h = ed_export_host(e);
    return h ? h->registry.count : 0;
}

const char *ed_export_name(Editor *e, size_t index) {
    DmExportPlugins *h = ed_export_host(e);
    return h && index < h->registry.count ? h->registry.formats[index].api->name
                                          : DM_TEXT_UNAVAILABLE_EXPORTER;
}

DmExportFormat *ed_export_selected(Editor *e) {
    DmExportPlugins *h = ed_export_host(e);
    return h && e->export_type > 0 && (size_t)e->export_type <= h->registry.count
               ? &h->registry.formats[e->export_type - 1]
               : NULL;
}

void ed_export_error(Editor *e, const char *message) {
    DmExportPlugins *h = ed_export_host(e);

    if (h) {
        snprintf(h->message, sizeof(h->message), "%s", message);
        h->message_format = e->export_type - 1;
    }

    ed_message(e, message);
    e->export_window = true;
}

static int32_t DME_CALL cancelled(void *user) {
    return SDL_GetAtomicInt(&((DmPluginWork *)user)->cancel);
}

static void DME_CALL progress(void *user, uint32_t percent) {
    SDL_SetAtomicInt(&((DmPluginWork *)user)->progress, (int)(percent > 100 ? 100 : percent));
}

static void list_item(DmPluginWork *w, const char *heading, size_t length, const char *item);

static void DME_CALL diagnostic(void *user, uint32_t severity, uint32_t flags,
                                const char *message) {
    DmPluginWork *w = user;

    if (!message) {
        return;
    }

    if (severity == DME_ERROR && !*w->message) {
        snprintf(w->message, sizeof(w->message), "%s", message);
    }

    if (severity == DME_WARNING) {
        w->warnings++;
    }

    const char *split = flags & DME_LIST ? strchr(message, '\n') : NULL;

    if ((flags & DME_POPUP) && severity == DME_WARNING) {
        w->altered |= (flags & DME_ALTERED) != 0;
    }

    if ((flags & DME_POPUP) && severity == DME_WARNING && split && split > message) {
        list_item(w, message, (size_t)(split - message), split + 1);
    } else if ((flags & DME_POPUP) && severity == DME_WARNING) {
        size_t used = strlen(w->popup), remaining = sizeof(w->popup) - used;

        if (!w->popup_truncated) {
            int length = snprintf(w->popup + used, remaining, "%s%s", used ? "\n\n" : "", message);

            if (length < 0 || (size_t)length >= remaining) {
                w->popup_truncated = true;
            }
        }
    }

    if (flags & DME_DISPLAY) {
        size_t used = strlen(w->summary);

        if (split) {
            snprintf(w->summary + used, sizeof(w->summary) - used, "%s%.*s: %.1000s",
                     used ? "\n" : "", (int)(split - message), message, split + 1);
        } else {
            snprintf(w->summary + used, sizeof(w->summary) - used, "%s%.1000s", used ? "\n" : "",
                     message);
        }
    }
}

/* Adds one list item under its heading, once. */
static void list_item(DmPluginWork *w, const char *heading, size_t length, const char *item) {
    unsigned g = 0;

    while (g < w->group_count && (strlen(w->groups[g].heading) != length ||
                                  memcmp(w->groups[g].heading, heading, length))) {
        ++g;
    }

    if (g == w->group_count) {
        if (g == SDL_arraysize(w->groups) || length >= sizeof(w->groups[g].heading)) {
            w->popup_truncated = true;
            return;
        }

        memcpy(w->groups[g].heading, heading, length);
        w->groups[g].heading[length] = 0;
        w->groups[g].items[0] = 0;
        ++w->group_count;
    }

    char line[1024];
    snprintf(line, sizeof(line), "- %s\n", item);
    char *items = w->groups[g].items;
    size_t used = strlen(items);

    if (strstr(items, line)) {
        return;
    }

    if (used + strlen(line) >= sizeof(w->groups[g].items)) {
        w->popup_truncated = true;
        return;
    }

    memcpy(items + used, line, strlen(line) + 1);
}

/* The completion warning: the last-resort headline, each heading with its items, other warnings,
   and where the output went. */
static void compose_warning(const DmPluginWork *w, char *out, size_t cap) {
    size_t used = 0;
    out[0] = 0;
#define ADD(...)                                                                                   \
    do {                                                                                           \
        int n = snprintf(out + used, cap - used, __VA_ARGS__);                                     \
        used += n > 0 && (size_t)n < cap - used ? (size_t)n : cap - used - 1;                      \
    } while (0)

    if (w->altered) {
        ADD(DM_TEXT_EXPORT_WARNING_ALTERED, w->api->name);
        ADD("\n\n");
    }

    for (unsigned g = 0; g < w->group_count; ++g) {
        ADD("%s:\n%s\n", w->groups[g].heading, w->groups[g].items);
    }

    if (*w->popup) {
        ADD("%s\n\n", w->popup);
    }

    if (w->popup_truncated) {
        ADD("%s\n\n", DM_TEXT_EXPORT_WARNING_POPUP_TRUNCATED);
    }

    ADD(DM_TEXT_EXPORT_WARNING_SAVED, w->destination);
#undef ADD
}

static int32_t DME_CALL read_sample(void *user, uint32_t asset, uint64_t start, uint64_t frames,
                                   float *pcm, uint64_t capacity) {
    DmPluginWork *w = user;

    if (cancelled(w)) {
        return DME_CANCELLED;
    }

    return dm_sample_export_read(w->samples, asset, start, frames, pcm, capacity);
}

static bool copy_sample_string(char *out, size_t cap, DmeString value) {
    if (!value.data || !value.length || value.length >= cap ||
        memchr(value.data, 0, (size_t)value.length)) {
        return false;
    }

    memcpy(out, value.data, (size_t)value.length);
    out[value.length] = 0;
    return true;
}

static int32_t DME_CALL freeze_sample(void *user, const DmeSampleSelection *selected,
                                      DmeFrozenSample *out, char *error, uint64_t cap) {
    DmPluginWork *w = user;
    DmPatchReplacement reference = {0};

    if (!selected || selected->size != sizeof(*selected) || !out || !selected->generation ||
        !copy_sample_string(reference.locator, sizeof(reference.locator), selected->locator) ||
        !copy_sample_string(reference.set_id, sizeof(reference.set_id), selected->set_id) ||
        !copy_sample_string(reference.preset_id, sizeof(reference.preset_id),
                            selected->preset_id)) {
        snprintf(error, (size_t)cap, "%s", DM_TEXT_SAMPLE_MANIFEST_INVALID);
        return DME_INVALID;
    }

    reference.generation = selected->generation;
    reference.key.drums = (selected->flags & DME_SAMPLE_PERCUSSION) != 0;
    DmFrozenExportSample *frozen = NULL;

    for (size_t i = 0; i < w->frozen_count; ++i) {
        const DmPatchReplacement *r = &w->frozen[i].selection;

        if (r->generation == reference.generation && r->key.drums == reference.key.drums &&
            !strcmp(r->locator, reference.locator) && !strcmp(r->set_id, reference.set_id) &&
            !strcmp(r->preset_id, reference.preset_id)) {
            frozen = &w->frozen[i];
            break;
        }
    }

    if (!frozen) {
        DmFrozenExportSample prepared = {.selection = reference};
        uint32_t index;
        DmeHost callbacks = {.user = w, .cancelled = cancelled};

        if (!dm_sample_export_selection_snapshot(&reference, 1, &prepared.context, &callbacks,
                                                 &index, &prepared.snapshot, error, (size_t)cap)) {
            return cancelled(w) ? DME_CANCELLED : DME_UNAVAILABLE;
        }

        if (prepared.context.sample_presets[0].generation != reference.generation) {
            dm_sample_export_free(prepared.snapshot);
            snprintf(error, (size_t)cap, "%s", DM_TEXT_SAMPLE_EXPORT_CHANGED);
            return DME_UNAVAILABLE;
        }

        uint64_t values = 0;

        for (uint32_t i = 0; i < prepared.context.sample_asset_count; ++i) {
            const DmeSampleAsset *asset = &prepared.context.sample_assets[i];
            values += asset->frames * asset->channels;
        }

        if (values > 64 * 1024 * 1024 - w->frozen_values || w->frozen_count >= 2048 ||
            !dm_reserve((void **)&w->frozen, &w->frozen_capacity, w->frozen_count + 1,
                        sizeof(*w->frozen))) {
            dm_sample_export_free(prepared.snapshot);
            snprintf(error, (size_t)cap, "%s", DM_TEXT_SAMPLE_EXPORT_MEMORY);
            return DME_NOMEM;
        }

        static SDL_SpinLock token_lock;
        static uint64_t next_token;
        SDL_LockSpinlock(&token_lock);
        prepared.handle = ++next_token;
        SDL_UnlockSpinlock(&token_lock);
        w->frozen_values += (size_t)values;
        frozen = &w->frozen[w->frozen_count++];
        *frozen = prepared;
    }

    *out = (DmeFrozenSample){.size = sizeof(*out),
                             .handle = frozen->handle,
                             .preset_count = frozen->context.sample_preset_count,
                             .zone_count = frozen->context.sample_zone_count,
                             .asset_count = frozen->context.sample_asset_count,
                             .presets = frozen->context.sample_presets,
                             .zones = frozen->context.sample_zones,
                             .assets = frozen->context.sample_assets};
    return DME_OK;
}

static int32_t DME_CALL read_frozen_sample(void *user, uint64_t handle, uint32_t asset,
                                           uint64_t start, uint64_t frames, float *pcm,
                                           uint64_t capacity) {
    DmPluginWork *w = user;

    if (cancelled(w)) {
        return DME_CANCELLED;
    }

    for (size_t i = 0; i < w->frozen_count; ++i) {
        if (w->frozen[i].handle == handle) {
            return dm_sample_export_read(w->frozen[i].snapshot, asset, start, frames, pcm,
                                         capacity);
        }
    }

    return DME_INVALID;
}

static bool artifact_name(const char *name) {
    if (!name || !*name || SDL_strnlen(name, 256) >= 256 || !strcmp(name, ".") ||
        !strcmp(name, "..")) {
        return false;
    }

    for (const unsigned char *s = (const unsigned char *)name; *s; s++) {
        if (*s < 32 || strchr("/\\:*?\"<>|", *s)) {
            return false;
        }
    }

    return true;
}

/* ZIP artifacts may place files in folders with "/"; each part follows the name rules. */
static bool artifact_path(const char *name, bool folders) {
    if (!folders) {
        return artifact_name(name);
    }

    if (!name || !*name || SDL_strnlen(name, 8192) >= 8192) {
        return false;
    }

    char part[256];

    for (const char *s = name;;) {
        const char *slash = strchr(s, '/');
        size_t length = slash ? (size_t)(slash - s) : strlen(s);

        if (length >= sizeof(part)) {
            return false;
        }
        memcpy(part, s, length);
        part[length] = 0;

        if (!artifact_name(part)) {
            return false;
        }

        if (!slash) {
            return true;
        }

        s = slash + 1;
    }
}

static int32_t DME_CALL artifact(void *user, const char *name, const uint8_t *data, uint64_t size) {
    DmPluginWork *w = user;

    if (cancelled(w)) {
        return 0;
    }

    if (w->review) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_EXPORTER_ATTEMPTED_OUTPUT_DURING);
        return 0;
    }

    if (!artifact_path(name, w->api->container == DME_ZIP) || (!data && size) ||
        size > 512ULL * 1024 * 1024 || w->artifact_count == 20000 ||
        size > 512ULL * 1024 * 1024 - w->total_bytes) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_EXPORTER_RETURNED_INVALID_ARTIFACT);
        return 0;
    }

    for (size_t i = 0; i < w->artifact_count; i++) {
        if (!SDL_strcasecmp(name, w->artifacts[i].name)) {
            snprintf(w->message, sizeof(w->message), DM_TEXT_EXPORTER_RETURNED_DUPLICATE_ARTIFACT,
                     name);
            return 0;
        }
    }

    if (!dm_reserve((void **)&w->artifacts, &w->artifact_capacity, w->artifact_count + 1,
                    sizeof(*w->artifacts))) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_NOT_ENOUGH_MEMORY_EXPORTED);
        return 0;
    }

    DmPluginArtifact *a = &w->artifacts[w->artifact_count];
    *a = (DmPluginArtifact){.name = SDL_strdup(name)};

    if (!a->name || !dm_bytes(&a->data, data, (size_t)size)) {
        SDL_free(a->name);
        dm_bytes_free(&a->data);
        snprintf(w->message, sizeof(w->message), DM_TEXT_NOT_ENOUGH_MEMORY_EXPORTED);
        return 0;
    }

    w->artifact_count++;
    w->total_bytes += (size_t)size;
    return 1;
}

static bool publish(DmPluginWork *w) {
    SDL_PathInfo info;

    if (SDL_GetPathInfo(w->destination, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_CANNOT_SAVE_EXPORT_DESTINATION);
        return false;
    }

    if (!w->artifact_count || (w->api->container == DME_SINGLE_FILE && w->artifact_count != 1)) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_EXPORTER_RETURNED_INVALID_NUMBER);
        return false;
    }

    DmBytes archive = {0};
    const void *data = w->artifacts[0].data.data;
    size_t size = w->artifacts[0].data.size;
    bool ok = true;

    if (w->api->container == DME_ZIP) {
        DmArchiveEntry *entries = calloc(w->artifact_count, sizeof(*entries));

        if (!entries) {
            snprintf(w->message, sizeof(w->message), DM_TEXT_NOT_ENOUGH_MEMORY_EXPORTED);
            return false;
        }

        for (size_t i = 0; i < w->artifact_count; i++) {
            entries[i] = (DmArchiveEntry){w->artifacts[i].name, w->artifacts[i].data.data,
                                          w->artifacts[i].data.size};
        }

        ok = dm_zip(entries, w->artifact_count, &archive, w->message, sizeof(w->message));
        free(entries);
        data = archive.data;
        size = archive.size;
    }

    DmOutput output = {0};

    if (ok) {
        ok = dm_output_begin(&output, w->destination, w->message, sizeof(w->message));
    }

    if (output.file) {
        for (size_t pos = 0; pos < size && ok; pos += 16384) {
            size_t n = size - pos;

            if (n > 16384) {
                n = 16384;
            }

            ok = !cancelled(w) && dm_output_write(&output, (const uint8_t *)data + pos, n,
                                                  w->message, sizeof(w->message));
        }

        ok = dm_output_finish(&output, ok && !cancelled(w), w->message, sizeof(w->message));
    }

    dm_bytes_free(&archive);

    if (!ok && !cancelled(w)) {
        char reason[2048];
        snprintf(reason, sizeof(reason), "%.2000s",
                 *w->message ? w->message : DM_TEXT_UNKNOWN_OUTPUT_FAILURE);
        snprintf(w->message, sizeof(w->message), "Cannot save export: %s\nDestination: %s", reason,
                 w->destination);
    }

    return ok;
}

static bool resolve(DmPluginWork *w) {
    DmProject p;
    dm_project_init(&p);
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration = 0;
    bool ok = !cancelled(w) &&
              dm_unpack(&p, w->snapshot.data, w->snapshot.size, w->message, sizeof(w->message));

    if (ok) {
        ok = dm_export_timeline(&p, &events, &count, &duration, w->message, sizeof(w->message));
    }

    if (ok && count) {
        w->events = calloc(count, sizeof(*w->events));

        if (!w->events) {
            ok = false;
            snprintf(w->message, sizeof(w->message), DM_TEXT_NOT_ENOUGH_MEMORY_EXPORT);
        }
    }

    if (ok) {
        unsigned authored = dm_export_authored_pan(&p);

        for (size_t i = 0; i < count; i++) {
            const DmExportEvent *e = &events[i];
            bool generated = (e->flags & DM_EXPORT_DEFAULT_CONTROL) &&
                             !(authored & (1u << (e->status & MIDI_CHANNEL_MASK)));
            w->events[i] = (DmeEvent){e->tick,
                                      e->source_id,
                                      e->status,
                                      e->a,
                                      e->b,
                                      e->port,
                                      generated ? DME_EVENT_DEFAULT_CONTROL : 0};
        }

        /* An unnamed project gives no name, so exporters fall back to the source file's. */
        snprintf(w->project_name, sizeof(w->project_name), "%s",
                 strcmp(p.name, DM_TEXT_UNTITLED) ? p.name : "");
        w->context = (DmeContext){.events = w->events,
                                  .event_count = count,
                                  .duration = duration,
                                  .project_session = w->session,
                                  .project_revision = w->revision,
                                  .ppq = DM_PPQ,
                                  .project_name = w->project_name,
                                  .source_name = w->source_name};

        for (int c = 0; c < 16; c++) {
            w->context.percussion_channels |= dm_channel_drums(&p, c) ? 1u << c : 0;
        }

        for (unsigned i = 0; i < 128; i++) {
            w->context.program_names[i] = dm_program_name((int)i);
            w->context.drum_names[i] = dm_drum_name((int)i);
        }

        if (w->inspection) {
            DmeHost host = {.user = w, .cancelled = cancelled};
            ok = dm_sample_export_identities(&p, &w->context, &host, &w->samples);
        } else {
            DmeHost host = {.user = w, .cancelled = cancelled, .diagnostic = diagnostic};
            ok = dm_sample_export_snapshot(&p, &w->context, &host, &w->samples);

            if (!ok && !cancelled(w)) {
                snprintf(w->message, sizeof(w->message), "%s", DM_TEXT_SAMPLE_EXPORT_MEMORY);
            }
        }

        if (w->inspection && ok) {
            DmeHost callbacks = {.user = w, .cancelled = cancelled};
            ok = dm_sample_export_catalog(w->catalog_program, w->catalog_writable, &callbacks,
                                          &w->catalog, w->message, sizeof(w->message));
            w->context.library = dm_sample_export_catalog_view(w->catalog);
        }

        /* Loop points reach exporters in timeline ticks, limited to the exported duration. */
        double start, end;

        if (dm_loop_points(&p, &start, &end)) {
            if (start >= 0) {
                w->context.loop_start = (uint64_t)fmin(duration, llround(start * DM_PPQ));
                w->context.loop_points |= DME_LOOP_START;
            }

            if (end >= 0) {
                w->context.loop_end = (uint64_t)fmin(duration, llround(end * DM_PPQ));
                w->context.loop_points |= DME_LOOP_END;
            }
        }
    }

    free(events);
    dm_project_free(&p);
    return ok && !cancelled(w);
}

static int worker(void *user) {
    DmPluginWork *w = user;
    bool ok;

    if (w->catalog_only) {
        DmeHost callbacks = {.user = w, .cancelled = cancelled};
        ok = dm_sample_export_catalog(w->catalog_program, w->catalog_writable, &callbacks,
                                      &w->catalog, w->message, sizeof(w->message)) &&
             !cancelled(w);
    } else {
        ok = w->prepared || resolve(w);
    }
    w->prepared = ok;

    if (ok && w->inspection) {
        w->success = true;
    } else if (ok) {
        DmeHost host = {.user = w,
                        .cancelled = cancelled,
                        .progress = progress,
                        .diagnostic = diagnostic,
                        .artifact = artifact,
                        .read_sample = read_sample,
                        .freeze_sample = freeze_sample,
                        .read_frozen_sample = read_frozen_sample,
                        .plugins_directory = {w->plugins_directory, strlen(w->plugins_directory)}};
        char error[2048] = {0};
        int32_t status;

        const DmeHistoryRuntime *runtime = w->history_runtime;

        /* Repository jobs own their writes; a saved export is published by the host. */
        if (runtime) {
            status =
                w->review
                    ? runtime->prepare(w->repository_job, &w->context, &host, error, sizeof(error))
                    : runtime->run(w->repository_job, &w->context, &host, error, sizeof(error));
        } else if (w->review) {
            status = w->api->preflight(w->job, &w->context, &host, error, sizeof(error));
        } else {
            status = w->api->run(w->job, &w->context, &host, error, sizeof(error));
        }

        if (status == DME_OK && !*w->message && (runtime || !cancelled(w))) {
            w->success = w->review || runtime || publish(w);
        } else if (!*w->message) {
            snprintf(w->message, sizeof(w->message), "%s",
                     *error ? error : DM_TEXT_EXPORTER_FAILED_WITHOUT_DIAGNOSTIC);
        }

        /* Exports report their result here; history actions report their own outcome. */
        if (w->success && !w->review && (!runtime || w->install)) {
            snprintf(w->message, sizeof(w->message), DM_TEXT_EXPORT_RESULT_FORMAT, w->api->name,
                     w->install ? DM_TEXT_EXPORT_RESULT_INSTALLED : DM_TEXT_EXPORT_RESULT_SAVED,
                     w->warnings, w->destination);
        }
    }

    if (cancelled(w) && !w->success) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_EXPORT_CANCELED);
    }

    dm_bytes_free(&w->snapshot);
    SDL_SetAtomicInt(&w->done, 1);
    return 0;
}

static void clear_work(DmPluginWork *w) {
    if (w->repository_job) {
        w->history_runtime->destroy_job(w->repository_job);
    }

    if (w->job) {
        w->api->destroy_job(w->job);
    }

    for (size_t i = 0; i < w->artifact_count; i++) {
        SDL_free(w->artifacts[i].name);
        dm_bytes_free(&w->artifacts[i].data);
    }

    free(w->artifacts);
    free(w->events);
    dm_sample_export_free(w->samples);
    dm_sample_export_catalog_free(w->catalog);

    for (size_t i = 0; i < w->frozen_count; ++i) {
        dm_sample_export_free(w->frozen[i].snapshot);
    }

    free(w->frozen);
    dm_bytes_free(&w->snapshot);
    memset(w, 0, sizeof(*w));
}

/* The repository folder picker; the export starts once a folder is chosen. */
bool ed_export_repository_picker(Editor *e, const char **start) {
    DmExportPlugins *h = ed_export_host(e);
    DmExportFormat *f = ed_export_selected(e);

    if (!h || !f || !f->history || h->work.thread || h->review_pending) {
        ed_export_error(e, DM_TEXT_SELECTED_EXPORTER_UNAVAILABLE_BUSY);
        return false;
    }

    h->pending_format = e->export_type - 1;
    const char *folder = ed_export_repository(e, (size_t)h->pending_format);
    *start = *folder ? folder : NULL;
    return true;
}

void ed_export_repository_done(Editor *e, const char *folder) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h) {
        return;
    }

    int index = h->pending_format;
    h->pending_format = -1;

    if (folder && index >= 0) {
        ed_export_set_repository(e, (size_t)index, folder);
        begin(e, false, index, folder, true);
    }
}

/* Export to Repository: installs into the exporter's saved folder when it still exists, and
   asks for a folder otherwise. Preferences changes the saved folder. */
void ed_export_to_repository(Editor *e) {
    DmExportFormat *f = ed_export_selected(e);
    const char *saved = f ? ed_export_repository(e, (size_t)(e->export_type - 1)) : "";
    SDL_PathInfo info;

    if (!*saved || !SDL_GetPathInfo(saved, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        ed_dialog(e, DIALOG_REPOSITORY);
        return;
    }

    char folder[DM_PATH];
    const char *start;
    snprintf(folder, sizeof(folder), "%s", saved);

    if (ed_export_repository_picker(e, &start)) {
        ed_export_repository_done(e, folder);
    }
}

bool ed_export_installs(Editor *e, size_t index) {
    DmExportPlugins *h = ed_export_host(e);
    return h && index < h->registry.count && h->registry.formats[index].history;
}

const char *ed_export_repository(Editor *e, size_t index) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h || index >= h->registry.count) {
        return "";
    }

    int entry = folder_entry(h, h->registry.formats[index].api->id, false);
    return entry >= 0 ? h->folders[entry].repository : "";
}

void ed_export_set_repository(Editor *e, size_t index, const char *folder) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h || index >= h->registry.count || strlen(folder) >= DM_PATH) {
        return;
    }

    int entry = folder_entry(h, h->registry.formats[index].api->id, true);

    if (entry >= 0 && strcmp(h->folders[entry].repository, folder)) {
        snprintf(h->folders[entry].repository, DM_PATH, "%s", folder);
        save_folders(e, h);
        DmExportFormat *format = &h->registry.formats[index];

        if (format->history_job) {
            h->registry.history_runtime->destroy_job(format->history_job);
            format->history_job = NULL;
        }

        format->history_scope = 0;
    }
}

void ed_export_history_reset(Editor *e) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h || ed_export_busy(e)) {
        return;
    }

    for (size_t i = 0; i < h->registry.count; i++) {
        DmExportFormat *format = &h->registry.formats[i];

        if (format->history_job) {
            h->registry.history_runtime->destroy_job(format->history_job);
            format->history_job = NULL;
        }

        format->history_scope = 0;
    }

    h->history_key[0] = 0;
}

/* Preferences: choose an exporter's repository folder without exporting. */
bool ed_export_folder_picker(Editor *e, size_t index) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h || !ed_export_installs(e, index)) {
        return false;
    }

    h->folder_format = (int)index;
    return true;
}

const char *ed_export_folder_start(Editor *e) {
    DmExportPlugins *h = e->export_plugins;
    const char *folder =
        h && h->folder_format >= 0 ? ed_export_repository(e, (size_t)h->folder_format) : "";
    return *folder ? folder : NULL;
}

void ed_export_folder_done(Editor *e, const char *folder) {
    DmExportPlugins *h = ed_export_host(e);

    if (h && folder && h->folder_format >= 0) {
        ed_export_set_repository(e, (size_t)h->folder_format, folder);
    }

    if (h) {
        h->folder_format = -1;
    }
}

bool ed_export_review_open(Editor *e) {
    return e->export_plugins && e->export_plugins->review_pending;
}

void ed_export_review_decide(Editor *e, int decision) {
    DmExportPlugins *h = e->export_plugins;

    if (!h || !h->review_pending) {
        return;
    }

    DmPluginWork *w = &h->work;
    h->review_pending = false;

    if (!decision) {
        clear_work(w);
        ed_message(e, DM_TEXT_EXPORT_CANCELED);
        return;
    }

    if (w->revision != e->export_revision || w->session != e->project_session) {
        clear_work(w);
        ed_export_error(e, DM_TEXT_ARRANGEMENT_CHANGED_START_EXPORT);
        return;
    }

    if (decision == 2 && w->api->review_apply(h->registry.formats[w->format_index].instance, w->job,
                                              w->message, sizeof(w->message)) != DME_OK) {
        ed_export_error(e, *w->message ? w->message : DM_TEXT_CANNOT_APPLY_REVIEWED_MAPPINGS);
        clear_work(w);
        return;
    }

    w->review = false;
    w->success = false;
    w->message[0] = w->summary[0] = w->popup[0] = 0;
    w->group_count = 0;
    w->popup_truncated = w->altered = false;
    w->warnings = 0;
    SDL_SetAtomicInt(&w->done, 0);
    SDL_SetAtomicInt(&w->progress, 0);
    w->thread = SDL_CreateThread(worker, "Exporter", w);

    if (!w->thread) {
        ed_export_error(e, DM_TEXT_CANNOT_START_EXPORT_WORKER);
        clear_work(w);
    }
}

static void poll_catalog(Editor *e, DmExportPlugins *h) {
    DmPluginWork *w = &h->catalog_work;

    /* Keep the published catalog alive while a foreground job or its review can use it. */
    if (!w->thread || !SDL_GetAtomicInt(&w->done) || h->work.thread || h->review_pending) {
        return;
    }

    SDL_WaitThread(w->thread, NULL);
    w->thread = NULL;

    if (w->catalog_revision == h->catalog_revision && w->revision == e->export_revision &&
        w->session == e->project_session && h->context_valid) {
        h->catalog_dirty = false;

        if (w->success) {
            if (!dm_sample_export_catalog_equal(h->catalog, w->catalog)) {
                dm_sample_export_catalog_free(h->catalog);
                h->catalog = w->catalog;
                w->catalog = NULL;
                h->context.library = dm_sample_export_catalog_view(h->catalog);
                memset(h->inspected, 0, sizeof(h->inspected));
            }
        } else {
            snprintf(h->message, sizeof(h->message), "%s", w->message);
            h->message_format = -1;
        }
    }

    clear_work(w);
}

void ed_export_poll(Editor *e) {
    DmExportPlugins *h = e->export_plugins;

    if (!h) {
        return;
    }

    poll_catalog(e, h);
    DmPluginWork *w = &h->work;

    if (!w->thread || !SDL_GetAtomicInt(&w->done)) {
        return;
    }

    SDL_WaitThread(w->thread, NULL);
    w->thread = NULL;

    if (w->review && w->success) {
        DmeReview review = {0};

        if (w->api->review(w->job, &review) == DME_OK &&
            dm_export_review_valid(&review, w->message, sizeof(w->message))) {
            h->review_pending = true;
            h->review_selected = 0;

            if (!review.count && !w->warnings) {
                ed_export_review_decide(e, 1);
            }

            return;
        }

        w->success = false;

        if (!*w->message) {
            snprintf(w->message, sizeof(w->message), DM_TEXT_CANNOT_READ_EXPORT_MAPPING);
        }
    }

    if (w->inspection) {
        if (w->success && w->catalog_revision == h->catalog_revision) {
            h->catalog_dirty = false;
        }

        if (w->success && w->revision == e->export_revision && w->session == e->project_session) {
            free(h->events);
            h->events = w->events;
            w->events = NULL;
            h->context = w->context;
            dm_sample_export_free(h->identities);
            h->identities = w->samples;
            w->samples = NULL;
            dm_sample_export_catalog_free(h->catalog);
            h->catalog = w->catalog;
            w->catalog = NULL;
            h->context.library = dm_sample_export_catalog_view(h->catalog);
            h->context.events = h->events;
            snprintf(h->project_name, sizeof(h->project_name), "%s", w->project_name);
            snprintf(h->source_name, sizeof(h->source_name), "%s", w->source_name);
            h->context.project_name = h->project_name;
            h->context.source_name = h->source_name;
            h->revision = w->revision;
            h->session = w->session;
            h->context_valid = true;
            h->inspection_failed = false;
            memset(h->inspected, 0, sizeof(h->inspected));
        } else if (!w->success) {
            snprintf(h->message, sizeof(h->message), DM_TEXT_CANNOT_READ_EXPORT_ARRANGEMENT,
                     w->message);
            h->inspection_failed = true;
            h->failed_revision = w->revision;
            h->failed_session = w->session;
            h->message_format = -1;
        }
    } else {
        DmExportFormat *format = &h->registry.formats[w->format_index];
        const char *folder = ed_export_repository(e, (size_t)w->format_index);

        /* The job becomes the history view unless the folder changed while it ran. */
        if (w->success && w->history_runtime && w->repository_job &&
            (w->install || !strcmp(folder, w->destination))) {
            if (w->history_runtime->finish(w->repository_job, w->message, sizeof(w->message)) ==
                DME_OK) {
                if (format->history_job) {
                    w->history_runtime->destroy_job(format->history_job);
                }

                format->history_job = w->repository_job;
                w->repository_job = NULL;
                DmeRepositoryView view = {0};
                w->history_runtime->describe(format->history_job, &view);
                format->history_scope = view.selected_scope;
            } else {
                w->success = false;
            }
        }

        bool inspection = w->history_runtime && w->repository_action == DME_REPOSITORY_INSPECT;

        /* A routine history refresh keeps the last export result on screen. */
        if (!inspection || !w->success) {
            snprintf(h->message, sizeof(h->message), "%s", w->message);
            h->message_format = w->format_index;
        }

        /* Warnings stay in the export window and popup; the status bar only reports the result. */
        if (w->success && !inspection) {
            char done[512];
            const char *message = DM_TEXT_EXPORT_COMPLETE_FORMAT;

            if (w->history_runtime && w->repository_action == DME_REPOSITORY_INITIALIZE) {
                message = DM_TEXT_REPOSITORY_INITIALIZED_FORMAT;
            } else if (w->history_runtime && (w->repository_action == DME_REPOSITORY_REVERT ||
                                              w->repository_action == DME_REPOSITORY_REVERT_ALL)) {
                message = DM_TEXT_REPOSITORY_REVERTED_FORMAT;
            }

            snprintf(done, sizeof(done), message, w->api->name);
            ed_message(e, done);
        } else if (!w->success) {
            ed_message(e, w->message);
        }

        if (w->success && (*w->popup || w->group_count)) {
            /* Warnings go to the warning window; the status line has the result. */
            compose_warning(w, h->warning, sizeof(h->warning));
            h->warning_open = true;
        }

        if (!w->success && !inspection) {
            e->export_window = true;
            e->export_type = w->format_index + 1;
        }
    }

    clear_work(w);
}

static bool begin(Editor *e, bool inspection, int index, const char *path, bool install) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h) {
        return false;
    }

    DmPluginWork *w = &h->work;

    if (w->thread || h->review_pending) {
        if (!inspection) {
            ed_export_error(e, DM_TEXT_EXPORT_TASK_ALREADY_RUNNING);
        }

        return false;
    }

    memset(w, 0, sizeof(*w));
    w->inspection = inspection;
    w->revision = e->export_revision;
    w->session = e->project_session;
    w->catalog_revision = h->catalog_revision;
    w->format_index = index;
    w->install = install;
    w->keep_originals = h->keep_originals;
    snprintf(w->plugins_directory, sizeof(w->plugins_directory), "%s", h->registry.directory);

    if (!dm_sample_library_roots(e->base_path, e->settings_path, w->catalog_program,
                                 sizeof(w->catalog_program), w->catalog_writable,
                                 sizeof(w->catalog_writable))) {
        ed_export_error(e, DM_TEXT_SAMPLE_LIBRARY_PATH);
        clear_work(w);
        return false;
    }
    snprintf(w->source_name, sizeof(w->source_name), "%s", e->export_source);

    if (!inspection) {
        if (index < 0 || (size_t)index >= h->registry.count) {
            ed_export_error(e, DM_TEXT_SELECTED_EXPORTER_UNAVAILABLE);
            return false;
        }

        if (!path || strlen(path) >= DM_PATH) {
            ed_export_error(e, DM_TEXT_EXPORT_PATH_TOO_LONG);
            return false;
        }

        w->review = true;
        w->api = h->registry.formats[index].api;
        w->job =
            w->api->create_job(h->registry.formats[index].instance, w->message, sizeof(w->message));

        if (!w->job) {
            ed_export_error(e, *w->message ? w->message : DM_TEXT_CANNOT_FREEZE_EXPORTER_SETTINGS);
            clear_work(w);
            return false;
        }

        snprintf(w->destination, sizeof(w->destination), "%s", path);

        DmExportFormat *format = &h->registry.formats[index];

        if (install && format->history) {
            DmeRepositoryRequest request = {
                .size = sizeof(request),
                .action = DME_REPOSITORY_INSTALL,
                .keep_originals = h->keep_originals,
                .untracked = !e->repository_history,
                .plugins_directory = {w->plugins_directory, strlen(w->plugins_directory)},
                .checkout = {w->destination, strlen(w->destination)}};
            w->history_runtime = h->registry.history_runtime;
            w->repository_action = request.action;
            w->repository_job =
                w->history_runtime->create_job(format->history, w->api->id, format->instance,
                                               w->job, &request, w->message, sizeof(w->message));

            if (!w->repository_job) {
                ed_export_error(e, w->message);
                clear_work(w);
                return false;
            }
        }
    }

    if (!dm_pack(&e->project, &w->snapshot)) {
        ed_export_error(e, DM_TEXT_CANNOT_SNAPSHOT_PROJECT_EXPORT);
        clear_work(w);
        return false;
    }

    w->thread = SDL_CreateThread(worker, inspection ? DM_TEXT_EXPORT_INSPECTION : "Exporter", w);

    if (!w->thread) {
        char message[512];
        snprintf(message, sizeof(message), DM_TEXT_CANNOT_START_EXPORT_DETAIL, SDL_GetError());
        ed_export_error(e, message);
        clear_work(w);
        return false;
    }

    if (!inspection) {
        h->message[0] = 0;
        ed_message(e, DM_TEXT_EXPORTING_STATUS);
    }

    return true;
}

bool ed_export_repository_action(Editor *e, uint32_t action, uint64_t scope, const char *key) {
    DmExportPlugins *h = ed_export_host(e);
    DmExportFormat *format = ed_export_selected(e);

    if (!h || !format || !format->history || ed_export_busy(e) || e->dialog_busy ||
        action == DME_REPOSITORY_INSTALL || action > DME_REPOSITORY_REVERT_ALL ||
        (!e->repository_history && action != DME_REPOSITORY_INSPECT)) {
        return false;
    }

    DmPluginWork *w = &h->work;
    memset(w, 0, sizeof(*w));
    w->api = format->api;
    w->format_index = e->export_type - 1;
    w->history_runtime = h->registry.history_runtime;
    w->repository_action = action;
    w->prepared = true;
    snprintf(w->plugins_directory, sizeof(w->plugins_directory), "%s", h->registry.directory);
    snprintf(w->destination, sizeof(w->destination), "%s",
             ed_export_repository(e, (size_t)w->format_index));
    DmeRepositoryRequest request = {
        .size = sizeof(request),
        .action = action,
        .scope = scope,
        .plugins_directory = {w->plugins_directory, strlen(w->plugins_directory)},
        .checkout = {w->destination, strlen(w->destination)},
        .logical_key = {key ? key : "", key ? strlen(key) : 0},
        .untracked = !e->repository_history};
    w->repository_job =
        w->history_runtime->create_job(format->history, format->api->id, format->instance, NULL,
                                       &request, w->message, sizeof(w->message));

    if (!w->repository_job) {
        ed_export_error(e, w->message);
        clear_work(w);
        return false;
    }

    w->thread = SDL_CreateThread(worker, "Repository", w);

    if (!w->thread) {
        ed_export_error(e, DM_TEXT_CANNOT_START_EXPORT_WORKER);
        clear_work(w);
        return false;
    }

    return true;
}

bool ed_export_context(Editor *e) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h) {
        return false;
    }

    ed_export_poll(e);

    if (h->context_valid && h->revision == e->export_revision && h->session == e->project_session &&
        !h->work.thread && !h->review_pending && !h->catalog_work.thread && h->catalog_dirty) {
        refresh_catalog(e);
    }

    if (!h->context_valid || h->revision != e->export_revision ||
        h->session != e->project_session) {
        if (h->inspection_failed && h->failed_revision == e->export_revision &&
            h->failed_session == e->project_session) {
            return false;
        }

        if (!h->work.thread && !h->review_pending && !begin(e, true, -1, NULL, false)) {
            h->inspection_failed = true;
            h->failed_revision = e->export_revision;
            h->failed_session = e->project_session;
        }

        return false;
    }

    return !h->work.thread && !h->review_pending;
}

bool ed_export_settings(Editor *e, size_t index, DmeUi *ui) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h || index >= h->registry.count || !ed_export_context(e)) {
        return false;
    }

    DmExportFormat *f = &h->registry.formats[index];

    if (!h->inspected[index]) {
        struct {
            char field[97], row[97];
        } selected[64] = {0};

        DmeUi before = {0};

        if (f->api->describe(f->instance, &before) == DME_OK && before.count <= 64) {
            for (uint32_t i = 0; i < before.count; i++) {
                const DmeField *field = &before.fields[i];
                int row = f->selected_rows[i];

                if (field->type == DME_MAPPING && row >= 0 && (uint32_t)row < field->row_count) {
                    snprintf(selected[i].field, sizeof(selected[i].field), "%s", field->id);
                    snprintf(selected[i].row, sizeof(selected[i].row), "%s", field->rows[row].id);
                }
            }
        }

        char error[512] = {0};

        if (f->api->inspect(f->instance, &h->context, error, sizeof(error)) != DME_OK) {
            ed_export_error(e, *error ? error : DM_TEXT_EXPORTER_CANNOT_INSPECT_THIS);
            return false;
        }

        h->inspected[index] = 1;
        memset(f->selected_rows, 0, sizeof(f->selected_rows));
        DmeUi after = {0};

        if (f->api->describe(f->instance, &after) == DME_OK && after.count <= 64) {
            for (uint32_t i = 0; i < after.count; i++) {
                const DmeField *field = &after.fields[i];

                for (unsigned old = 0; field->type == DME_MAPPING && old < 64; old++) {
                    if (strcmp(field->id, selected[old].field)) {
                        continue;
                    }

                    for (uint32_t row = 0; row < field->row_count; row++) {
                        if (!strcmp(field->rows[row].id, selected[old].row)) {
                            f->selected_rows[i] = (int)row;
                        }
                    }
                }
            }
        }
    }

    char error[512];

    if (f->api->describe(f->instance, ui) != DME_OK ||
        !dm_export_ui_valid(ui, error, sizeof(error))) {
        ed_export_error(e, DM_TEXT_EXPORTER_RETURNED_INVALID_SETTINGS);
        return false;
    }

    return true;
}

bool ed_export_set(Editor *e, size_t index, const char *id, DmeValue value) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h || index >= h->registry.count || h->work.thread || h->review_pending || e->dialog_busy) {
        return false;
    }

    DmExportFormat *f = &h->registry.formats[index];
    char error[512] = {0};

    if (f->api->set(f->instance, id, &value, error, sizeof(error)) != DME_OK) {
        ed_export_error(e, *error ? error : DM_TEXT_INVALID_EXPORTER_SETTING);
        return false;
    }

    return true;
}

void ed_export_start(Editor *e, size_t index, const char *path) {
    begin(e, false, (int)index, path, false);
}

void ed_export_free(Editor *e) {
    DmExportPlugins *h = e->export_plugins;

    if (!h) {
        return;
    }

    SDL_SetAtomicInt(&h->work.cancel, 1);
    SDL_SetAtomicInt(&h->catalog_work.cancel, 1);

    if (h->work.thread) {
        SDL_WaitThread(h->work.thread, NULL);
    }

    clear_work(&h->work);

    if (h->catalog_work.thread) {
        SDL_WaitThread(h->catalog_work.thread, NULL);
    }

    clear_work(&h->catalog_work);
    free(h->events);
    dm_sample_export_catalog_free(h->catalog);
    dm_sample_export_free(h->identities);
    dm_export_registry_free(&h->registry);
    free(h);
    e->export_plugins = NULL;
}

static const char *leaf(const char *path) {
    const char *s = strrchr(path, '/'), *b = strrchr(path, '\\');

    if (!s || (b && b > s)) {
        s = b;
    }

    return s ? s + 1 : path;
}

bool ed_export_suggest(Editor *e, const char *extension, char *out, size_t cap) {
    char name[DM_PATH];
    bool source = !*e->project.name || !strcmp(e->project.name, DM_TEXT_UNTITLED);
    snprintf(name, sizeof(name), "%s",
             source ? leaf(*e->path ? e->path : e->export_source) : e->project.name);

    if (source) {
        char *dot = strrchr(name, '.');

        if (dot) {
            *dot = 0;
        }
    }

    for (unsigned char *s = (unsigned char *)name; *s; s++) {
        if (*s < 32 || strchr("/\\:*?\"<>|", *s)) {
            *s = '_';
        }
    }

    size_t len = strlen(name);

    while (len && (name[len - 1] == ' ' || name[len - 1] == '.')) {
        name[--len] = 0;
    }

    if (!*name) {
        snprintf(name, sizeof(name), "export");
    }

    const char *devices[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                             "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                             "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    char stem[16];
    size_t n = strcspn(name, ".");

    if (n < sizeof(stem)) {
        memcpy(stem, name, n);
        stem[n] = 0;

        for (size_t i = 0; i < sizeof(devices) / sizeof(*devices); i++) {
            if (!SDL_strcasecmp(stem, devices[i])) {
                if (strlen(name) + 1 >= sizeof(name)) {
                    return false;
                }

                memmove(name + 1, name, strlen(name) + 1);
                name[0] = '_';
                break;
            }
        }
    }

    return snprintf(out, cap, "%s%s.%s", e->export_folder, name, extension) < (int)cap;
}

bool ed_export_picker(Editor *e, const SDL_DialogFileFilter **filter, const char **suggested) {
    DmExportPlugins *h = ed_export_host(e);
    DmExportFormat *f = ed_export_selected(e);

    if (!h || !f || h->work.thread || h->review_pending) {
        ed_export_error(e, DM_TEXT_SELECTED_EXPORTER_UNAVAILABLE_BUSY);
        return false;
    }

    if (!ed_export_suggest(e, f->api->extension, h->suggested, sizeof(h->suggested))) {
        ed_export_error(e, DM_TEXT_REMEMBERED_EXPORT_PATH_TOO);
        return false;
    }

    h->pending_format = e->export_type - 1;
    h->filter = (SDL_DialogFileFilter){f->api->filter_name, f->api->extension};
    *filter = &h->filter;
    *suggested = h->suggested;
    return true;
}

const char *ed_export_extension(Editor *e) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h || h->pending_format < 0 || (size_t)h->pending_format >= h->registry.count) {
        return "";
    }

    return h->registry.formats[h->pending_format].api->extension;
}

void ed_export_picker_done(Editor *e, const char *path) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h) {
        return;
    }

    int index = h->pending_format;
    h->pending_format = -1;

    if (path && index >= 0) {
        begin(e, false, index, path, false);
    }
}

/* Results of a finished export, such as DM_TEXT_OPEN_FILE_LOCATION, belong to that export. They are
   cleared when the export window opens again and when another project is opened or created. */
void ed_export_reset_results(Editor *e) {
    e->audio_exported[0] = 0;
    e->audio_export_message[0] = 0;

    if (e->export_plugins && !ed_export_busy(e)) {
        e->export_plugins->message[0] = 0;
    }

    ed_export_catalog_changed(e);
}

void ed_export_catalog_changed(Editor *e) {
    if (e->export_plugins) {
        e->export_plugins->catalog_revision++;
        e->export_plugins->catalog_dirty = true;
    }
}

static bool refresh_catalog(Editor *e) {
    DmExportPlugins *h = e->export_plugins;
    DmPluginWork *w = &h->catalog_work;
    memset(w, 0, sizeof(*w));
    w->inspection = w->catalog_only = true;
    w->catalog_revision = h->catalog_revision;
    w->revision = e->export_revision;
    w->session = e->project_session;

    if (!dm_sample_library_roots(e->base_path, e->settings_path, w->catalog_program,
                                 sizeof(w->catalog_program), w->catalog_writable,
                                 sizeof(w->catalog_writable))) {
        clear_work(w);
        h->catalog_dirty = false;
        ed_export_error(e, DM_TEXT_SAMPLE_LIBRARY_PATH);
        return false;
    }

    w->thread = SDL_CreateThread(worker, "Sample catalog", w);

    if (!w->thread) {
        clear_work(w);
        h->catalog_dirty = false;
        ed_export_error(e, DM_TEXT_CANNOT_START_EXPORT_WORKER);
        return false;
    }

    return true;
}
