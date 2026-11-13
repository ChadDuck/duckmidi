#include "editor/internal.h"
#include "text/en.h"
#include <inttypes.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

typedef struct DmSaveJob {
    DmProject project;
    SDL_Thread *thread;
    SDL_AtomicInt done, cancel;
    uint64_t session, revision;
    bool success;
    char path[DM_PATH], original[DM_PATH], prefix[256], error[512];
} DmSaveJob;

typedef struct DmAutosave {
    DmSaveJob *job;
    uint64_t due, session, revision;
    SDL_Time started;
    bool owns_marker, restore_pending;
    char folder[DM_PATH], marker[DM_PATH], latest[DM_PATH], original[DM_PATH];
    char recovery[DM_PATH], recovery_original[DM_PATH], question[DM_PATH * 3];
} DmAutosave;

static void autosave_poll(Editor *e);

static void free_job(DmSaveJob *job) {
    if (!job) {
        return;
    }

    SDL_SetAtomicInt(&job->cancel, 1);
    SDL_WaitThread(job->thread, NULL);
    dm_project_free(&job->project);
    free(job);
}

static int save_worker(void *user) {
    DmSaveJob *job = user;
    DmBytes data = {0};
    DmOutput output = {0};
    bool ok = !SDL_GetAtomicInt(&job->cancel) && dm_pack(&job->project, &data);

    if (!ok && !SDL_GetAtomicInt(&job->cancel)) {
        snprintf(job->error, sizeof(job->error), DM_TEXT_CANNOT_SERIALIZE_PROJECT_SNAPSHOT);
    }

    if (ok) {
        ok = dm_output_begin(&output, job->path, job->error, sizeof(job->error));
    }

    if (output.file) {
        for (size_t i = 0; i < data.size && ok; i += 65536) {
            size_t n = data.size - i;

            if (n > 65536) {
                n = 65536;
            }

            ok = !SDL_GetAtomicInt(&job->cancel) &&
                 dm_output_write(&output, data.data + i, n, job->error, sizeof(job->error));
        }

        ok = dm_output_finish(&output, ok && !SDL_GetAtomicInt(&job->cancel), job->error,
                              sizeof(job->error));
    }

    job->success = ok;
    dm_bytes_free(&data);
    SDL_SetAtomicInt(&job->done, 1);
    return 0;
}

bool ed_save_busy(Editor *e) {
    return e->save_job != NULL;
}

void ed_save_cancel(Editor *e) {
    if (e->save_job) {
        SDL_SetAtomicInt(&e->save_job->cancel, 1);
    }
}

void ed_save_free(Editor *e) {
    free_job(e->save_job);
    e->save_job = NULL;

    if (e->autosave) {
        free_job(e->autosave->job);
        e->autosave->job = NULL;
    }
}

static DmSaveJob *start_save(Editor *e, const char *path, char *error, size_t cap) {
    if (!path || !*path || strlen(path) >= DM_PATH) {
        snprintf(error, cap, "%s", DM_TEXT_INVALID_PROJECT_SAVE_PATH);
        return NULL;
    }

    DmSaveJob *job = calloc(1, sizeof(*job));

    if (!job) {
        snprintf(error, cap, "%s", DM_TEXT_CANNOT_ALLOCATE_PROJECT_SAVE);
        return NULL;
    }

    dm_project_init(&job->project);

    if (!dm_project_copy(&job->project, &e->project)) {
        dm_project_free(&job->project);
        free(job);
        snprintf(error, cap, "%s", DM_TEXT_CANNOT_SNAPSHOT_PROJECT_SAVING);
        return NULL;
    }

    snprintf(job->path, sizeof(job->path), "%s", path);
    job->session = e->project_session;
    job->revision = e->export_revision;
    job->thread = SDL_CreateThread(save_worker, DM_TEXT_PROJECT_SAVE, job);

    if (!job->thread) {
        dm_project_free(&job->project);
        free(job);
        snprintf(error, cap, "%s", DM_TEXT_CANNOT_START_PROJECT_SAVE);
        return NULL;
    }

    return job;
}

bool ed_save_to(Editor *e, const char *path) {
    if (ed_save_busy(e)) {
        ed_message(e, DM_TEXT_PROJECT_SAVE_ALREADY_RUNNING);
        return false;
    }

    char error[512];
    e->save_job = start_save(e, path, error, sizeof(error));

    if (!e->save_job) {
        ed_message(e, error);
        return false;
    }

    ed_message(e, DM_TEXT_SAVING_PROJECT);
    return true;
}

void ed_save_poll(Editor *e) {
    autosave_poll(e);
    ed_autosave_step(e, SDL_GetTicks());
    DmSaveJob *job = e->save_job;

    if (!job || !SDL_GetAtomicInt(&job->done)) {
        return;
    }

    SDL_WaitThread(job->thread, NULL);
    e->save_job = NULL;
    bool current = job->session == e->project_session,
         unchanged = current && job->revision == e->export_revision;

    if (job->success && current) {
        snprintf(e->path, sizeof(e->path), "%s", job->path);

        if (unchanged) {
            e->dirty = false;
        }

        ed_add_recent(e, job->path);
        ed_preference_save(e);
        ed_message(e, unchanged ? DM_TEXT_PROJECT_SAVED : DM_TEXT_PROJECT_SNAPSHOT_SAVED_NEWER);
    } else if (current) {
        char message[640];
        snprintf(message, sizeof(message), DM_TEXT_PROJECT_SAVE_FAILED,
                 SDL_GetAtomicInt(&job->cancel) ? "Cancelled." : job->error);
        ed_message(e, message);
    }

    bool proceed = job->success && unchanged && e->pending_action;

    if (current && e->pending_action && !proceed) {
        e->confirm = true;
    }

    dm_project_free(&job->project);
    free(job);

    if (proceed) {
        ed_pending_execute(e);
    }
}

static bool process_running(uint64_t pid) {
    if (!pid || pid > UINT32_MAX) {
        return false;
    }

#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);

    if (!process) {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }

    DWORD code;
    bool alive = !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    if (pid > INT32_MAX) {
        return false;
    }

    return kill((pid_t)pid, 0) == 0 || errno != ESRCH;
#endif
}

static uint64_t process_id(void) {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

static bool write_marker(DmAutosave *a) {
    if (!a->owns_marker) {
        return true;
    }

    char data[DM_PATH * 2 + 128], error[512];
    int n = snprintf(data, sizeof(data), "%" PRIu64 "\n%" PRId64 "\n%s\n%s\n", process_id(),
                     (int64_t)a->started, a->latest, a->original);
    DmOutput output = {0};

    if (n < 0 || n >= (int)sizeof(data) ||
        !dm_output_begin(&output, a->marker, error, sizeof(error))) {
        return false;
    }

    bool ok = dm_output_write(&output, data, (size_t)n, error, sizeof(error));
    return dm_output_finish(&output, ok, error, sizeof(error));
}

/* Claim a marker exclusively before using the transactional writer to fill it. A simultaneous
   second launch cannot replace a marker owned by a live process. */
static bool claim_marker(const char *path) {
#ifdef _WIN32
    wchar_t wide[DM_PATH];

    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, DM_PATH)) {
        return false;
    }

    HANDLE file = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    CloseHandle(file);
#else
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

    if (file < 0) {
        return false;
    }

    close(file);
#endif
    return true;
}

static const char *file_name(const char *path) {
    const char *name = path;

    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }

    return name;
}

static void recovery_question(DmAutosave *a, const char *settings, SDL_Time started) {
    const char *name = file_name(a->recovery);
    size_t length = strlen(name);
    char time[20] = {0};

    if (length >= 24) {
        memcpy(time, name + length - 24, 19);
    }

    snprintf(a->question, sizeof(a->question), DM_TEXT_AUTOSAVE_RESTORE_QUESTION,
             *a->recovery_original ? file_name(a->recovery_original) : DM_TEXT_UNTITLED, time);
    char logs[DM_PATH];

    if (snprintf(logs, sizeof(logs), "%s%slogs", settings,
                 *settings && settings[strlen(settings) - 1] != '/' &&
                         settings[strlen(settings) - 1] != '\\'
                     ? "/"
                     : "") >= (int)sizeof(logs)) {
        return;
    }

    int count = 0;
    char **files = SDL_GlobDirectory(logs, "crash-*.txt", 0, &count);
    SDL_Time newest = started;
    char crash[DM_PATH] = {0};

    for (int i = 0; files && i < count; ++i) {
        char path[DM_PATH];
        SDL_PathInfo info;

        if (snprintf(path, sizeof(path), "%s/%s", logs, files[i]) < (int)sizeof(path) &&
            SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE &&
            info.modify_time >= newest) {
            newest = info.modify_time;
            snprintf(crash, sizeof(crash), "%s", path);
        }
    }

    SDL_free(files);

    if (*crash) {
        size_t used = strlen(a->question);
        snprintf(a->question + used, sizeof(a->question) - used, DM_TEXT_AUTOSAVE_CRASH_REPORT,
                 crash);
    }
}

void ed_autosave_init(Editor *e, const char *settings_folder) {
    ed_autosave_free(e);
    DmAutosave *a = calloc(1, sizeof(*a));

    if (!a) {
        ed_message(e, DM_TEXT_AUTOSAVE_INIT_FAILED);
        return;
    }

    a->due = SDL_GetTicks() + ED_AUTOSAVE_INTERVAL_MS;
    SDL_GetCurrentTime(&a->started);
    int n = snprintf(a->folder, sizeof(a->folder), "%s%sprojects/auto", settings_folder,
                     *settings_folder && settings_folder[strlen(settings_folder) - 1] != '/' &&
                             settings_folder[strlen(settings_folder) - 1] != '\\'
                         ? "/"
                         : "");

    if (n < 0 || n >= (int)sizeof(a->folder) ||
        snprintf(a->marker, sizeof(a->marker), "%s/session.lock", a->folder) >=
            (int)sizeof(a->marker) ||
        !SDL_CreateDirectory(a->folder)) {
        free(a);
        ed_message(e, DM_TEXT_AUTOSAVE_INIT_FAILED);
        return;
    }

    SDL_PathInfo info;
    bool exists = SDL_GetPathInfo(a->marker, &info);

    if (exists) {
        DmBytes data = {0};
        char error[512];
        uint64_t pid = 0;
        int64_t started = 0;

        if (dm_read(a->marker, &data, error, sizeof(error)) && dm_bytes(&data, "", 1)) {
            char *text = (char *)data.data, *end;
            pid = strtoull(text, &end, 10);

            if (*end == '\n') {
                started = strtoll(end + 1, &end, 10);

                if (*end == '\n') {
                    char *latest = end + 1, *original = strchr(latest, '\n');

                    if (original) {
                        *original++ = 0;
                        char *tail = strchr(original, '\n');

                        if (tail) {
                            *tail = 0;
                        }

                        if (strlen(latest) < DM_PATH && strlen(original) < DM_PATH &&
                            SDL_GetPathInfo(latest, &info) && info.type == SDL_PATHTYPE_FILE) {
                            snprintf(a->recovery, sizeof(a->recovery), "%s", latest);
                            snprintf(a->recovery_original, sizeof(a->recovery_original), "%s",
                                     original);
                        }
                    }
                }
            }
        }

        dm_bytes_free(&data);

        /* An empty/unreadable marker may belong to an instance still claiming it. */
        if (!pid || process_running(pid)) {
            e->autosave = a;
            return;
        }

        a->restore_pending = *a->recovery != 0;

        if (a->restore_pending) {
            recovery_question(a, settings_folder, started);
        }

        SDL_RemovePath(a->marker);
    }

    a->owns_marker = claim_marker(a->marker);
    e->autosave = a;

    if (a->owns_marker && !write_marker(a)) {
        SDL_RemovePath(a->marker);
        a->owns_marker = false;
        ed_message(e, DM_TEXT_AUTOSAVE_INIT_FAILED);
    }
}

static int autosave_newest_first(const void *left, const void *right) {
    return strcmp(*(const char *const *)right, *(const char *const *)left);
}

static void prune_autosaves(DmAutosave *a, const char *prefix) {
    char pattern[300];
    snprintf(pattern, sizeof(pattern), "%s_*.duck", prefix);
    int count = 0;
    char **files = SDL_GlobDirectory(a->folder, pattern, 0, &count);

    if (!files) {
        return;
    }

    qsort(files, count, sizeof(*files), autosave_newest_first);

    for (int i = ED_AUTOSAVE_KEEP; i < count; ++i) {
        char path[DM_PATH];

        if (snprintf(path, sizeof(path), "%s/%s", a->folder, files[i]) < (int)sizeof(path)) {
            SDL_RemovePath(path);
        }
    }

    SDL_free(files);
}

static void autosave_poll(Editor *e) {
    DmAutosave *a = e->autosave;
    DmSaveJob *job = a ? a->job : NULL;

    if (!job || !SDL_GetAtomicInt(&job->done)) {
        return;
    }

    SDL_WaitThread(job->thread, NULL);
    a->job = NULL;

    if (job->success) {
        a->session = job->session;
        a->revision = job->revision;
        snprintf(a->latest, sizeof(a->latest), "%s", job->path);
        snprintf(a->original, sizeof(a->original), "%s", job->original);
        prune_autosaves(a, job->prefix);

        if (!write_marker(a)) {
            ed_message(e, DM_TEXT_AUTOSAVE_MARKER_FAILED);
        }
    } else {
        char message[640];
        snprintf(message, sizeof(message), DM_TEXT_AUTOSAVE_FAILED, job->error);
        ed_message(e, message);
    }

    dm_project_free(&job->project);
    free(job);
}

void ed_autosave_step(Editor *e, uint64_t now) {
    DmAutosave *a = e->autosave;

    if (!a || now < a->due || a->job || a->restore_pending) {
        return;
    }

    a->due = now + ED_AUTOSAVE_INTERVAL_MS;

    if (!e->active || !e->dirty ||
        (a->session == e->project_session && a->revision == e->export_revision)) {
        return;
    }

    char full_path[DM_PATH];
    const char *identity = *e->path ? e->path : "untitled";

    if (*e->path) {
        bool absolute =
            e->path[0] == '/' || e->path[0] == '\\' || (e->path[0] && e->path[1] == ':');
        char *cwd = absolute ? NULL : SDL_GetCurrentDirectory();
        int size = snprintf(full_path, sizeof(full_path), "%s%s", cwd ? cwd : "", e->path);
        bool valid = (absolute || cwd) && size >= 0 && size < (int)sizeof(full_path);
        SDL_free(cwd);

        if (!valid) {
            ed_message(e, DM_TEXT_AUTOSAVE_INIT_FAILED);
            return;
        }

        identity = full_path;
    }

    const char *name = file_name(identity), *dot = strrchr(name, '.');
    size_t length = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    char stem[200];
    size_t n = 0;

    for (size_t i = 0; i < length && n + 1 < sizeof(stem); ++i) {
        unsigned char c = (unsigned char)name[i];
        stem[n++] = c >= 128 || isalnum(c) || c == '-' || c == '_' ? (char)c : '_';
    }

    /* Do not cut a UTF-8 project name in the middle of its last character. */
    if (n < length) {
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) {
            --n;
        }
    }

    stem[n] = 0;
    uint32_t hash = 2166136261u;

    for (const unsigned char *p = (const unsigned char *)identity; *p; ++p) {
        hash = (hash ^ *p) * 16777619u;
    }

    SDL_Time time;
    SDL_DateTime date;
    char path[DM_PATH], prefix[256], error[512];
    snprintf(prefix, sizeof(prefix), "%s_%08" PRIx32, *stem ? stem : "untitled", hash);

    if (!SDL_GetCurrentTime(&time) || !SDL_TimeToDateTime(time, &date, true) ||
        snprintf(path, sizeof(path), "%s/%s_%04d-%02d-%02d_%02d-%02d-%02d.duck", a->folder, prefix,
                 date.year, date.month, date.day, date.hour, date.minute,
                 date.second) >= (int)sizeof(path)) {
        ed_message(e, DM_TEXT_AUTOSAVE_INIT_FAILED);
        return;
    }

    a->job = start_save(e, path, error, sizeof(error));

    if (!a->job) {
        char message[640];
        snprintf(message, sizeof(message), DM_TEXT_AUTOSAVE_FAILED, error);
        ed_message(e, message);
        return;
    }

    snprintf(a->job->original, sizeof(a->job->original), "%s", e->path);
    snprintf(a->job->prefix, sizeof(a->job->prefix), "%s", prefix);
}

bool ed_autosave_busy(const Editor *e) {
    return e->autosave && e->autosave->job;
}

const char *ed_autosave_question(const Editor *e) {
    return e->autosave && e->autosave->restore_pending ? e->autosave->question : NULL;
}

bool ed_autosave_restore(Editor *e) {
    DmAutosave *a = e->autosave;

    if (!a || !a->restore_pending) {
        return false;
    }

    a->restore_pending = false;
    return ed_load_autosave(e, a->recovery, a->recovery_original);
}

void ed_autosave_dialog(Editor *e) {
    const char *question = ed_autosave_question(e);

    if (!question || e->confirm || e->dialog_busy) {
        return;
    }

    static const char *const buttons[] = {DM_TEXT_AUTOSAVE_RESTORE, DM_TEXT_CANCEL};
    bool open = true;
    int choice = ui_confirm(DM_TEXT_AUTOSAVE_RECOVERY, &open, question, buttons, 2);

    if (choice == 0) {
        ed_autosave_restore(e);
    } else if (!open) {
        e->autosave->restore_pending = false;
    }
}

void ed_autosave_free(Editor *e) {
    DmAutosave *a = e->autosave;

    if (!a) {
        return;
    }

    free_job(a->job);

    if (a->owns_marker) {
        SDL_RemovePath(a->marker);
    }

    free(a);
    e->autosave = NULL;
}
