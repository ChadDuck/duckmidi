#include "editor_export_internal.h"
#include "text/en.h"
#include <limits.h>

static bool begin(Editor *e, bool inspection, int index, const char *path, bool install);

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
    h->keep_originals = true;
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

    if ((flags & DME_POPUP) && severity == DME_WARNING) {
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
        snprintf(w->summary + used, sizeof(w->summary) - used, "%s%.1000s", used ? "\n" : "",
                 message);
    }
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

static int32_t DME_CALL artifact(void *user, const char *name, const uint8_t *data, uint64_t size) {
    DmPluginWork *w = user;

    if (cancelled(w)) {
        return 0;
    }

    if (w->review) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_EXPORTER_ATTEMPTED_OUTPUT_DURING);
        return 0;
    }

    if (!artifact_name(name) || (!data && size) || size > 512ULL * 1024 * 1024 ||
        w->artifact_count == 64 || size > SIZE_MAX - w->total_bytes) {
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

    DmPluginArtifact *a = &w->artifacts[w->artifact_count];

    if (!dm_bytes(&a->data, data, (size_t)size)) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_NOT_ENOUGH_MEMORY_EXPORTED);
        return 0;
    }

    snprintf(a->name, sizeof(a->name), "%s", name);
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
        DmArchiveEntry entries[64];

        for (size_t i = 0; i < w->artifact_count; i++) {
            entries[i] = (DmArchiveEntry){w->artifacts[i].name, w->artifacts[i].data.data,
                                          w->artifacts[i].data.size};
        }

        ok = dm_zip(entries, w->artifact_count, &archive, w->message, sizeof(w->message));
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
        for (size_t i = 0; i < count; i++) {
            w->events[i] = (DmeEvent){events[i].tick, events[i].source_id, events[i].status,
                                      events[i].a,    events[i].b,         events[i].port};
        }

        snprintf(w->project_name, sizeof(w->project_name), "%s", p.name);
        w->context = (DmeContext){.events = w->events,
                                  .event_count = count,
                                  .duration = duration,
                                  .project_session = w->session,
                                  .ppq = DM_PPQ,
                                  .project_name = w->project_name,
                                  .source_name = w->source_name};

        for (unsigned i = 0; i < 128; i++) {
            w->context.program_names[i] = dm_program_name((int)i);
            w->context.drum_names[i] = dm_drum_name((int)i);
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

/* Folder installs accept relative paths inside the chosen folder only. */
static bool install_path(const char *path) {
    if (!path || !*path || SDL_strnlen(path, 256) >= 256 || path[0] == '/' || strchr(path, ':') ||
        strchr(path, '\\')) {
        return false;
    }

    for (const char *part = path; *part;) {
        const char *slash = strchr(part, '/');
        size_t length = slash ? (size_t)(slash - part) : strlen(part);

        if (!length || (length == 1 && part[0] == '.') ||
            (length == 2 && part[0] == '.' && part[1] == '.')) {
            return false;
        }

        part += length + (slash ? 1 : 0);
    }

    return true;
}

/* Joins the folder and a relative path with the separator the folder already uses. */
static bool repository_file(const char *folder, const char *path, char *out, size_t capacity) {
    bool backslash = strchr(folder, '\\') != NULL;
    size_t length = strlen(folder);
    const char *separator = length && (folder[length - 1] == '/' || folder[length - 1] == '\\') ? ""
                            : backslash ? "\\"
                                        : "/";

    if (snprintf(out, capacity, "%s%s%s", folder, separator, path) >= (int)capacity) {
        return false;
    }

    for (char *c = out + length; backslash && *c; ++c) {
        *c = *c == '/' ? '\\' : *c;
    }

    return true;
}

static int32_t DME_CALL read_file(void *user, const char *path, const uint8_t **data,
                                  uint64_t *size) {
    DmPluginWork *w = user;
    char full[DM_PATH], error[256];

    if (!install_path(path) || w->read_count == 16 ||
        !repository_file(w->destination, path, full, sizeof(full))) {
        return 0;
    }

    DmPluginFile *file = &w->reads[w->read_count];

    if (!dm_read(full, &file->data, error, sizeof(error))) {
        return 0;
    }

    snprintf(file->path, sizeof(file->path), "%s", path);
    ++w->read_count;
    *data = file->data.data;
    *size = file->data.size;
    return 1;
}

static int32_t DME_CALL install_file(void *user, const char *path, const uint8_t *data,
                                     uint64_t size, uint32_t flags) {
    DmPluginWork *w = user;

    if (!install_path(path) || w->install_count == 16 || (!data && size) ||
        size > 64ULL * 1024 * 1024 || flags > DME_INSTALL_BACKUP) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_EXPORTER_QUEUED_INVALID_REPOSITORY);
        return 0;
    }

    DmPluginFile *file = &w->installs[w->install_count];

    if (!dm_bytes(&file->data, data, (size_t)size)) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_NOT_ENOUGH_MEMORY_REPOSITORY);
        return 0;
    }

    snprintf(file->path, sizeof(file->path), "%s", path);
    file->flags = flags;
    ++w->install_count;
    return 1;
}

/* Writes the queued files. Each change is recorded before it is made, so a failure part way can
   still be reverted. Originals are kept beside their files as .bkp when the user keeps originals
   or the exporter asks for it; an existing .bkp is never overwritten. */
static bool commit_install(DmPluginWork *w) {
    w->changes = calloc(w->install_count + 1, sizeof(*w->changes));

    if (!w->changes) {
        snprintf(w->message, sizeof(w->message), DM_TEXT_NOT_ENOUGH_MEMORY_RECORD);
        return false;
    }

    for (size_t i = 0; i < w->install_count; ++i) {
        DmPluginFile *file = &w->installs[i];
        DmRepositoryChange *change = &w->changes[w->change_count];
        char error[1024] = {0}, backup[DM_PATH];

        if (!repository_file(w->destination, file->path, change->path, sizeof(change->path)) ||
            snprintf(backup, sizeof(backup), "%s.bkp", change->path) >= (int)sizeof(backup)) {
            snprintf(w->message, sizeof(w->message), DM_TEXT_REPOSITORY_PATH_TOO_LONG, file->path);
            return false;
        }

        change->existed = dm_read(change->path, &change->original, error, sizeof(error));
        ++w->change_count;
        bool keep = w->keep_originals || (file->flags & DME_INSTALL_BACKUP);

        if (change->existed && keep && !SDL_GetPathInfo(backup, NULL)) {
            if (!dm_write(backup, change->original.data, change->original.size, error,
                          sizeof(error))) {
                snprintf(w->message, sizeof(w->message), DM_TEXT_CANNOT_KEEP_ORIGINAL, file->path,
                         error);
                return false;
            }

            change->backup_created = true;
        }

        /* A new file may go in a folder the checkout has not created yet. */
        if (!change->existed) {
            char folder[DM_PATH];
            snprintf(folder, sizeof(folder), "%s", change->path);
            char *slash = strrchr(folder, '/'), *backslash = strrchr(folder, '\\');
            char *cut = slash > backslash ? slash : backslash;

            if (cut) {
                *cut = 0;
                SDL_CreateDirectory(folder);
            }
        }

        if (!dm_write(change->path, file->data.data, file->data.size, error, sizeof(error))) {
            snprintf(w->message, sizeof(w->message), DM_TEXT_CANNOT_WRITE, file->path, error);
            return false;
        }
    }

    return true;
}

static int worker(void *user) {
    DmPluginWork *w = user;
    bool ok = w->prepared || resolve(w);
    w->prepared = ok;

    if (ok && w->inspection) {
        w->success = true;
    } else if (ok) {
        DmeHost host = {w, cancelled, progress, diagnostic, artifact};
        char error[2048] = {0};
        int32_t status;

        if (w->review) {
            status = w->api->preflight(w->job, &w->context, &host, error, sizeof(error));
        } else if (w->install) {
            host.read_file = read_file;
            host.install_file = install_file;
            status = w->api->install(w->job, &w->context, &host, error, sizeof(error));
        } else {
            status = w->api->run(w->job, &w->context, &host, error, sizeof(error));
        }

        if (status == DME_OK && !*w->message && !cancelled(w)) {
            w->success = w->review || (w->install ? commit_install(w) : publish(w));
        } else if (!*w->message) {
            snprintf(w->message, sizeof(w->message), "%s",
                     *error ? error : DM_TEXT_EXPORTER_FAILED_WITHOUT_DIAGNOSTIC);
        }

        if (w->success && !w->review) {
            snprintf(w->message, sizeof(w->message), "%.128s %s (%u warnings): %.4095s%s%.3700s",
                     w->api->name, w->install ? "exported to repository" : "export complete",
                     w->warnings, w->destination, *w->summary ? "\n" : "", w->summary);
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
    if (w->job) {
        w->api->destroy_job(w->job);
    }

    for (size_t i = 0; i < 64; i++) {
        dm_bytes_free(&w->artifacts[i].data);
    }

    for (size_t i = 0; i < 16; i++) {
        dm_bytes_free(&w->reads[i].data);
        dm_bytes_free(&w->installs[i].data);
    }

    for (size_t i = 0; i < w->change_count; i++) {
        dm_bytes_free(&w->changes[i].original);
    }

    free(w->changes);
    free(w->events);
    dm_bytes_free(&w->snapshot);
    memset(w, 0, sizeof(*w));
}

static void free_changes(DmExportPlugins *h) {
    for (size_t i = 0; i < h->change_count; i++) {
        dm_bytes_free(&h->changes[i].original);
    }

    free(h->changes);
    h->changes = NULL;
    h->change_count = 0;
    h->changed_repository[0] = 0;
}

/* Keeps a revert record for every repository change since the last revert. A file changed again
   keeps its first original, so reverting restores the folder as it was before any export. */
static void keep_changes(DmExportPlugins *h, DmPluginWork *w) {
    if (strcmp(h->changed_repository, w->destination)) {
        free_changes(h);
        snprintf(h->changed_repository, sizeof(h->changed_repository), "%s", w->destination);
    }

    for (size_t i = 0; i < w->change_count; i++) {
        DmRepositoryChange *change = &w->changes[i];
        bool known = false;

        for (size_t j = 0; j < h->change_count && !known; j++) {
            known = !strcmp(h->changes[j].path, change->path);
        }

        DmRepositoryChange *grown =
            known ? h->changes : realloc(h->changes, (h->change_count + 1) * sizeof(*h->changes));

        if (known || !grown) {
            continue;
        }

        h->changes = grown;
        h->changes[h->change_count++] = *change;
        change->original = (DmBytes){0};
    }
}

bool ed_export_has_changes(Editor *e) {
    return e->export_plugins && e->export_plugins->change_count;
}

/* Restores every changed file and removes the backups the exports created. */
void ed_export_revert(Editor *e) {
    DmExportPlugins *h = e->export_plugins;

    if (!h || !h->change_count || ed_export_busy(e)) {
        return;
    }

    char error[1024] = {0};
    size_t failed = 0;

    for (size_t i = h->change_count; i > 0; i--) {
        DmRepositoryChange *change = &h->changes[i - 1];
        char backup[DM_PATH];
        snprintf(backup, sizeof(backup), "%s.bkp", change->path);
        bool ok = change->existed ? dm_write(change->path, change->original.data,
                                             change->original.size, error, sizeof(error))
                                  : SDL_RemovePath(change->path);

        if (ok && change->backup_created) {
            ok = SDL_RemovePath(backup);
        }

        failed += !ok;
    }

    if (failed) {
        snprintf(h->message, sizeof(h->message), DM_TEXT_REVERTED_REPOSITORY_EXPORT_BUT, failed);
    } else {
        snprintf(h->message, sizeof(h->message), DM_TEXT_REPOSITORY_REVERTED_DETAIL,
                 h->changed_repository);
    }

    h->message_format = -1;
    ed_message(e, failed ? h->message : DM_TEXT_REPOSITORY_EXPORT_REVERTED);
    free_changes(h);
}

/* The repository folder picker; the export starts once a folder is chosen. */
bool ed_export_repository_picker(Editor *e, const char **start) {
    DmExportPlugins *h = ed_export_host(e);
    DmExportFormat *f = ed_export_selected(e);

    if (!h || !f || !f->api->install || h->work.thread || h->review_pending) {
        ed_export_error(e, DM_TEXT_SELECTED_EXPORTER_UNAVAILABLE_BUSY);
        return false;
    }

    h->pending_format = e->export_type - 1;
    *start = *h->repository ? h->repository : NULL;
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
        snprintf(h->repository, sizeof(h->repository), "%s", folder);
        begin(e, false, index, folder, true);
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
    w->warnings = 0;
    SDL_SetAtomicInt(&w->done, 0);
    SDL_SetAtomicInt(&w->progress, 0);
    w->thread = SDL_CreateThread(worker, "Exporter", w);

    if (!w->thread) {
        ed_export_error(e, DM_TEXT_CANNOT_START_EXPORT_WORKER);
        clear_work(w);
    }
}

void ed_export_poll(Editor *e) {
    DmExportPlugins *h = e->export_plugins;

    if (!h) {
        return;
    }

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

            if (!review.count) {
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
        if (w->success && w->revision == e->export_revision && w->session == e->project_session) {
            free(h->events);
            h->events = w->events;
            w->events = NULL;
            h->context = w->context;
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
        snprintf(h->message, sizeof(h->message), "%s", w->message);
        h->message_format = w->format_index;

        if (w->install && w->change_count) {
            keep_changes(h, w);
        }

        /* Warnings stay in the export window and popup; the status bar only reports the result. */
        if (w->success) {
            char done[512];
            snprintf(done, sizeof(done), "%.128s export complete", w->api->name);
            ed_message(e, done);
        } else {
            ed_message(e, w->message);
        }

        if (w->success && *w->popup) {
            snprintf(h->warning, sizeof(h->warning),
                     "%s export was saved with audible changes or playback "
                     "limitations.\n\n%s%s\n\nSaved to: %s\n\nYour source project was not changed.",
                     w->api->name, w->popup,
                     w->popup_truncated ? "\n\nAdditional warnings were omitted from this popup; "
                                          "review the exporter's diagnostics."
                                        : "",
                     w->destination);
            h->warning_open = true;
        }

        if (!w->success) {
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
    w->format_index = index;
    w->install = install;
    w->keep_originals = h->keep_originals;
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
        ed_message(e, "Exporting...");
    }

    return true;
}

bool ed_export_context(Editor *e) {
    DmExportPlugins *h = ed_export_host(e);

    if (!h) {
        return false;
    }

    ed_export_poll(e);

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
        char error[512] = {0};

        if (f->api->inspect(f->instance, &h->context, error, sizeof(error)) != DME_OK) {
            ed_export_error(e, *error ? error : DM_TEXT_EXPORTER_CANNOT_INSPECT_THIS);
            return false;
        }

        h->inspected[index] = 1;
        memset(f->selected_rows, 0, sizeof(f->selected_rows));
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

    if (h->work.thread) {
        SDL_WaitThread(h->work.thread, NULL);
    }

    clear_work(&h->work);
    free(h->events);
    free_changes(h);
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

    if (e->export_plugins && !ed_export_busy(e)) {
        e->export_plugins->message[0] = 0;
    }
}
