#include "repository_install.h"
#include "../libraries/export_support/export_support.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int32_t action(const DmeExporter *exporter, void *settings, void *job,
                      const DmeContext *context, const DmeHost *host, uint32_t kind, bool untracked,
                      const char *plugins, const char *checkout, char *error, size_t capacity) {
    const DmeHistoryRuntime *runtime = duckmidi_history_runtime(DME_REPOSITORY_API);
    const DmeHistoryAdapter *adapter = exporter->history(DME_REPOSITORY_API);
    assert(runtime && adapter);
    DmeRepositoryRequest request = {.size = sizeof(request),
                                    .action = kind,
                                    .keep_originals = untracked,
                                    .plugins_directory = {plugins, strlen(plugins)},
                                    .checkout = {checkout, strlen(checkout)},
                                    .logical_key = {"", 0},
                                    .untracked = untracked};
    void *history =
        runtime->create_job(adapter, exporter->id, settings, job, &request, error, capacity);

    if (!history) {
        return DME_FAILED;
    }

    int32_t status = runtime->prepare(history, context, host, error, capacity);
    status = status == DME_OK ? runtime->run(history, context, host, error, capacity) : status;

    if (status == DME_OK) {
        status = runtime->finish(history, error, capacity);
    }

    runtime->destroy_job(history);
    return status;
}

static int32_t install(const DmeExporter *exporter, void *settings, const DmeContext *context,
                       const DmeHost *host, bool untracked, const char *plugins,
                       const char *checkout, char *error, size_t capacity) {
    void *job = exporter->create_job(settings, error, capacity);

    if (!job) {
        return DME_FAILED;
    }

    int32_t status = action(exporter, settings, job, context, host, DME_REPOSITORY_INSTALL,
                            untracked, plugins, checkout, error, capacity);
    exporter->destroy_job(job);
    return status;
}

int32_t repository_install(const DmeExporter *exporter, void *settings, const DmeContext *context,
                           const DmeHost *host, const char *plugins, const char *checkout,
                           char *error, size_t capacity) {
    return install(exporter, settings, context, host, false, plugins, checkout, error, capacity);
}

int32_t repository_install_untracked(const DmeExporter *exporter, void *settings,
                                     const DmeContext *context, const DmeHost *host,
                                     const char *plugins, const char *checkout, char *error,
                                     size_t capacity) {
    return install(exporter, settings, context, host, true, plugins, checkout, error, capacity);
}

int32_t repository_revert_all(const DmeExporter *exporter, void *settings, const DmeHost *host,
                              const char *plugins, const char *checkout, char *error,
                              size_t capacity) {
    return action(exporter, settings, NULL, NULL, host, DME_REPOSITORY_REVERT_ALL, false, plugins,
                  checkout, error, capacity);
}

char *checkout_read(const char *checkout, const char *path) {
    char full[4096];
    size_t size = 0;
    assert(SDL_snprintf(full, sizeof(full), "%s/%s", checkout, path) < (int)sizeof(full));
    return SDL_LoadFile(full, &size);
}

void checkout_write(const char *checkout, const char *path, const char *text) {
    char full[4096];
    assert(SDL_snprintf(full, sizeof(full), "%s/%s", checkout, path) < (int)sizeof(full));
    char *slash = strrchr(full, '/');
    *slash = 0;
    assert(SDL_CreateDirectory(full));
    *slash = '/';
    assert(SDL_SaveFile(full, text, strlen(text)));
}

void scenario_folder(char *out, size_t capacity, const char *group, const char *name) {
    static unsigned serial;
    SDL_Time now;
    assert(SDL_GetCurrentTime(&now));
    assert(SDL_snprintf(out, capacity, "agents/reports/%s/%llu-%u-%s", group,
                        (unsigned long long)now, serial++, name) < (int)capacity);
    assert(SDL_CreateDirectory(out));
}
