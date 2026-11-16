#include "client.h"
#include "../../src/text/en.h"
#include <stdlib.h>
#include <string.h>

/* Copies a runtime file into plugin memory and releases the runtime's copy. */
static bool take(const DmeHistoryApi *api, DmeHistoryFile *file, bool ok, EsFile *out, char *error,
                 size_t cap) {
    *out = (EsFile){0};

    if (ok && file->size > SIZE_MAX - 1) {
        ok = es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_REPOSITORY_PATH);
    }

    if (ok) {
        out->data = malloc((size_t)file->size + 1);
        ok = out->data != NULL ||
             es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_REPOSITORY_PATH);
    }

    if (ok) {
        if (file->size) {
            memcpy(out->data, file->data, (size_t)file->size);
        }

        out->data[file->size] = 0;
        out->size = (size_t)file->size;
        out->mode = file->mode;
        out->exists = file->exists != 0;
    }

    api->file_free(file);
    return ok;
}

bool es_history_root_read(const DmeHistoryApi *api, DmeHistoryRoot *root, const char *path,
                          EsFile *out, char *error, size_t cap) {
    DmeHistoryFile file = {0};
    bool ok = api->root_read(root, path, &file, error, cap) != 0;
    return take(api, &file, ok, out, error, cap);
}

bool es_history_baseline(EsHistory *h, const char *path, EsFile *out, char *error, size_t cap) {
    DmeHistoryFile file = {0};
    bool ok = h->api->baseline(h->history, path, &file, error, cap) != 0;
    return take(h->api, &file, ok, out, error, cap);
}

bool es_history_baseline_exists(EsHistory *h, const char *path, bool *exists, char *error,
                                size_t cap) {
    uint32_t found = 0;
    bool ok = h->api->baseline_exists(h->history, path, &found, error, cap) != 0;
    *exists = found != 0;
    return ok;
}

bool es_history_read(EsHistory *h, const char *path, EsFile *out, char *error, size_t cap) {
    DmeHistoryFile file = {0};
    bool ok = h->api->read(h->history, path, &file, error, cap) != 0;
    return take(h->api, &file, ok, out, error, cap);
}

const char *es_history_identity(EsHistory *h) {
    return h->api->identity(h->history);
}

bool es_history_untracked(EsHistory *h) {
    return h->api->untracked(h->history) != 0;
}

static int32_t DME_CALL collect(void *user, const char *path) {
    return es_paths_add(user, path);
}

bool es_history_baseline_paths(EsHistory *h, EsPaths *out, char *error, size_t cap) {
    return h->api->baseline_paths(h->history, collect, out, error, cap) != 0;
}

bool es_history_paths(EsHistory *h, const char *directory, bool recursive, EsPaths *out,
                      char *error, size_t cap) {
    return h->api->paths(h->history, directory, recursive, collect, out, error, cap) != 0;
}

bool es_history_plan_put(EsHistoryPlan *p, const char *path, const void *data, size_t size,
                         uint32_t mode, bool exists, char *error, size_t cap) {
    return p->api->plan_put(p->plan, path, data, size, exists, mode, error, cap) != 0;
}

bool es_history_plan_backup(EsHistoryPlan *p, const char *path) {
    return p->api->plan_backup(p->plan, path) != 0;
}

bool es_history_plan_affect(EsHistoryPlan *p, const char *key, const char *path,
                            const char *selector) {
    return p->api->plan_affect(p->plan, key, path, selector) != 0;
}

bool es_history_plan_depend(EsHistoryPlan *p, const char *consumer, const char *provider,
                            const char *role) {
    return p->api->plan_depend(p->plan, consumer, provider, role) != 0;
}

bool es_history_plan_find(EsHistoryPlan *p, const char *path, EsFile *view) {
    DmeHistoryFile file = {0};
    bool found = p->api->plan_find(p->plan, path, &file) != 0 && file.size <= SIZE_MAX;
    *view =
        found ? (EsFile){file.data, (size_t)file.size, file.mode, file.exists != 0} : (EsFile){0};
    return found;
}

bool es_history_plan_paths(EsHistoryPlan *p, EsPaths *out) {
    return p->api->plan_paths(p->plan, collect, out) != 0;
}

bool es_history_plan_exists(EsHistory *h, EsHistoryPlan *p, const char *path, bool *exists,
                            char *error, size_t cap) {
    EsFile planned;

    if (p && es_history_plan_find(p, path, &planned)) {
        *exists = planned.exists;
        return true;
    }

    return es_history_baseline_exists(h, path, exists, error, cap);
}

bool es_history_keep_originals(EsHistory *h, EsHistoryPlan *p, const char *key, char *error,
                               size_t cap) {
    return h->api->plan_keep_originals(h->history, p->plan, key, error, cap) != 0;
}

/* The intents a list of modifications describes; the strings and recipes stay borrowed. */
static DmeHistoryIntent *intents(const EsModification *mods, size_t count) {
    DmeHistoryIntent *out = count <= UINT32_MAX ? calloc(count + 1, sizeof(*out)) : NULL;

    for (size_t i = 0; out && i < count; i++) {
        out[i] = (DmeHistoryIntent){{mods[i].key, strlen(mods[i].key)},
                                    {mods[i].label, strlen(mods[i].label)},
                                    mods[i].data,
                                    mods[i].size};
    }

    return out;
}

bool es_history_apply(EsHistory *h, const EsModification *mods, size_t count, char *error,
                      size_t cap) {
    DmeHistoryIntent *list = intents(mods, count);
    bool ok = list ? h->api->apply(h->history, list, (uint32_t)count, error, cap) != 0
                   : es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_REPOSITORY_PATH);
    free(list);
    return ok;
}

bool es_history_probe(EsHistory *out, const DmeHistoryApi *api, const DmeHistoryAdapter *adapter,
                      const char *checkout, char *error, size_t cap) {
    *out = (EsHistory){api, NULL};
    return api && api->probe(adapter, checkout, &out->history, error, cap) && out->history;
}

bool es_history_package(EsHistory *probe, const EsModifications *mods, const DmeHost *host,
                        char *error, size_t cap) {
    DmeHistoryIntent *list = intents(mods->items, mods->count);
    bool ok =
        list ? probe->api->package(probe->history, list, (uint32_t)mods->count, host, error, cap)
             : es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_REPOSITORY_PATH);
    free(list);
    return ok;
}

void es_history_close(EsHistory *probe) {
    if (probe->api && probe->history) {
        probe->api->close(probe->history);
    }

    probe->history = NULL;
}

bool es_history_list(const DmeHistoryApi *api, DmeHistoryList *list, const EsPaths *paths) {
    bool ok = true;

    for (size_t i = 0; ok && i < paths->count; i++) {
        ok = api->list_add(list, paths->items[i]) != 0;
    }

    return ok;
}

bool es_history_targets(const DmeHistoryApi *api, DmeHistoryTargets *out,
                        const EsTargets *targets) {
    bool ok = true;

    for (size_t i = 0; ok && i < targets->count; i++) {
        const DmeSampleTarget *t = &targets->items[i];
        ok = api->targets_add(out, t->id.data, t->label.data, t->path.data) != 0;
    }

    return ok;
}
