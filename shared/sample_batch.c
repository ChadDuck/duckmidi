#include "sample_batch.h"
#include "../src/duck.h"
#include "../src/text/en.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BATCH_MAX_VALUES = 32 * 1024 * 1024 };

typedef struct {
    DmSampleBatch batch;
    size_t capacity, index, path_count, directory_files;
    const char *failure;
    const DmSampleBatchCallbacks *callbacks;
    bool stopped;
} BatchJob;

static bool canceled(void *user) {
    BatchJob *job = user;
    const DmSampleBatchCallbacks *cb = job->callbacks;
    job->stopped = job->stopped || (cb && cb->canceled && cb->canceled(cb->user));
    return job->stopped;
}

static void progress(const BatchJob *job, DmSampleBatchStage stage, const char *path, size_t total,
                     uint64_t done, uint64_t units) {
    const DmSampleBatchCallbacks *cb = job->callbacks;

    if (cb && cb->progress) {
        cb->progress(cb->user, stage, path, job->index, total, done, units);
    }
}

static void decode_progress(void *user, uint64_t done, uint64_t units) {
    const BatchJob *job = user;
    progress(job, DM_SAMPLE_DECODE_FILE, job->batch.items[job->index].path, job->batch.count, done,
             units);
}

static bool add_path(BatchJob *job, const char *path, const char *error) {
    size_t count = job->batch.count;

    if (count >= DM_SAMPLE_BATCH_MAX) {
        job->failure = DM_TEXT_SAMPLE_IMPORT_BATCH_LIMIT;
        return false;
    }

    size_t length = strlen(path) + 1;
    char *copy = malloc(length);

    if (!copy || !dm_reserve((void **)&job->batch.items, &job->capacity, count + 1,
                             sizeof(*job->batch.items))) {
        free(copy);
        job->failure = DM_TEXT_SAMPLE_IMPORT_MEMORY;
        return false;
    }

    memcpy(copy, path, length);
    DmSampleBatchItem *item = &job->batch.items[job->batch.count++];
    *item = (DmSampleBatchItem){.path = copy, .audio.embedded_root = -1};

    if (error) {
        snprintf(item->error, sizeof(item->error), "%s", error);
    }

    return true;
}

static bool supported_extension(const char *path) {
    static const char *const extensions[] = {
        ".wav", ".mp3", ".m4a", ".ogg", ".opus", ".flac", ".aif", ".aiff", ".aifc",
    };
    const char *extension = strrchr(path, '.');

    if (extension) {
        for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); ++i) {
            if (!SDL_strcasecmp(extension, extensions[i])) {
                return true;
            }
        }
    }

    return false;
}

static SDL_EnumerationResult SDLCALL scan_file(void *user, const char *directory,
                                               const char *name) {
    BatchJob *job = user;

    if (canceled(job)) {
        return SDL_ENUM_FAILURE;
    }

    if (!supported_extension(name)) {
        return SDL_ENUM_CONTINUE;
    }

    char path[DM_PATH];
    size_t directory_length = strlen(directory);
    bool slash = directory_length && (directory[directory_length - 1] == '/' ||
                                      directory[directory_length - 1] == '\\');
    int length = snprintf(path, sizeof(path), "%s%s%s", directory, slash ? "" : "/", name);

    if (length < 0 || (size_t)length >= sizeof(path)) {
        return add_path(job, name, DM_TEXT_SAMPLE_IMPORT_FILE) ? SDL_ENUM_CONTINUE
                                                               : SDL_ENUM_FAILURE;
    }

    SDL_PathInfo info;

    if (SDL_GetPathInfo(path, &info) && info.type != SDL_PATHTYPE_FILE) {
        return SDL_ENUM_CONTINUE;
    }

    progress(job, DM_SAMPLE_SCAN_PATHS, path, job->path_count, 0, 0);
    ++job->directory_files;
    const DmSampleBatchCallbacks *cb = job->callbacks;

    if (cb && cb->include_file && !cb->include_file(cb->file_user, path)) {
        return SDL_ENUM_CONTINUE;
    }

    if (!add_path(job, path, NULL)) {
        return SDL_ENUM_FAILURE;
    }

    job->batch.items[job->batch.count - 1].from_directory = true;
    return SDL_ENUM_CONTINUE;
}

static int compare_files(const void *left, const void *right) {
    const DmSampleBatchItem *a = left, *b = right;
    int order = SDL_strcasecmp(a->path, b->path);
    return order ? order : strcmp(a->path, b->path);
}

static bool scan_path(BatchJob *job, const char *path) {
    progress(job, DM_SAMPLE_SCAN_PATHS, path, job->path_count, 0, 0);
    SDL_PathInfo info;

    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        return add_path(job, path, NULL);
    }

    size_t first = job->batch.count;
    size_t files = job->directory_files;
    bool scanned = SDL_EnumerateDirectory(path, scan_file, job);

    if (job->failure || canceled(job)) {
        return false;
    }

    if (job->batch.count > first + 1) {
        qsort(job->batch.items + first, job->batch.count - first, sizeof(*job->batch.items),
              compare_files);
    }

    if (!scanned || job->directory_files == files) {
        return add_path(job, path,
                        scanned ? DM_TEXT_SAMPLE_IMPORT_NO_FILES : DM_TEXT_SAMPLE_IMPORT_DIRECTORY);
    }

    return true;
}

static void analyze_item(BatchJob *job, DmSampleBatchItem *item, size_t *values) {
    DmSampleImportCallbacks cb = {job, canceled, decode_progress};
    progress(job, DM_SAMPLE_DECODE_FILE, item->path, job->batch.count, 0, 0);

    if (!dm_sample_decode(item->path, &cb, &item->audio, item->error, sizeof(item->error))) {
        return;
    }

    size_t count = item->audio.frames * (size_t)item->audio.channels;

    if (count > BATCH_MAX_VALUES - *values) {
        dm_sample_audio_free(&item->audio);
        snprintf(item->error, sizeof(item->error), "%s", DM_TEXT_SAMPLE_IMPORT_BATCH_BYTES);
        return;
    }

    progress(job, DM_SAMPLE_ANALYZE_FILE, item->path, job->batch.count, 0, 2);
    item->peaks = malloc(DM_SAMPLE_BATCH_PEAKS * sizeof(*item->peaks));

    if (!item->peaks) {
        dm_sample_audio_free(&item->audio);
        snprintf(item->error, sizeof(item->error), "%s", DM_TEXT_SAMPLE_IMPORT_MEMORY);
        return;
    }

    if (dm_sample_analyze(&item->audio, item->path, &cb, &item->analysis)) {
        progress(job, DM_SAMPLE_ANALYZE_FILE, item->path, job->batch.count, 1, 2);

        if (dm_sample_peaks(&item->audio, item->peaks, DM_SAMPLE_BATCH_PEAKS, &item->absolute_peak,
                            &cb)) {
            progress(job, DM_SAMPLE_ANALYZE_FILE, item->path, job->batch.count, 2, 2);
            *values += count;
        }
    }
}

bool dm_sample_batch_import(const char *const *paths, size_t count,
                            const DmSampleBatchCallbacks *cb, DmSampleBatch *out, char *error,
                            size_t cap) {
    BatchJob job = {.path_count = count, .callbacks = cb};

    if (count > DM_SAMPLE_BATCH_MAX) {
        job.failure = DM_TEXT_SAMPLE_IMPORT_BATCH_LIMIT;
    }

    for (job.index = 0; job.index < count && !job.failure && !canceled(&job); ++job.index) {
        if (!scan_path(&job, paths[job.index])) {
            break;
        }
    }

    size_t values = 0;

    for (job.index = 0; job.index < job.batch.count && !job.failure && !canceled(&job);
         ++job.index) {
        DmSampleBatchItem *item = &job.batch.items[job.index];

        if (!item->error[0]) {
            analyze_item(&job, item, &values);
        }
    }

    const char *failure = canceled(&job) ? DM_TEXT_SAMPLE_IMPORT_CANCELED : job.failure;

    if (failure) {
        dm_sample_batch_free(&job.batch);
        snprintf(error, cap, "%s", failure);
        return false;
    }

    *out = job.batch;
    return true;
}

void dm_sample_batch_free(DmSampleBatch *batch) {
    for (size_t i = 0; i < batch->count; ++i) {
        DmSampleBatchItem *item = &batch->items[i];
        free(item->path);
        free(item->peaks);
        dm_sample_audio_free(&item->audio);
    }

    free(batch->items);
    *batch = (DmSampleBatch){0};
}
