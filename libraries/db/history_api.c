#include "db.h"
#include "internal.h"
#include "package.h"
#include "../../src/text/en.h"
#include <stdlib.h>
#include <string.h>

/* The public DmeHistoryApi over the runtime's own types. The opaque public handles are the
   runtime structures themselves: DmeHistory is EsRepository, DmeHistoryRoot is EsRoot,
   DmeHistoryPlan is EsPlan, DmeHistoryList is EsPaths and DmeHistoryTargets is EsTargets. */

static int32_t give(EsFile *file, DmeHistoryFile *out, bool ok) {
    *out = (DmeHistoryFile){file->data, file->size, file->mode, file->exists};

    if (!ok) {
        free(file->data);
        *out = (DmeHistoryFile){0};
    }

    return ok;
}

static int32_t DME_CALL root_read(DmeHistoryRoot *root, const char *path, DmeHistoryFile *out,
                                  char *error, uint64_t cap) {
    EsFile file = {0};
    return give(&file, out, es_fs_read((EsRoot *)root, path, &file, error, (size_t)cap));
}

static int32_t DME_CALL baseline(DmeHistory *history, const char *path, DmeHistoryFile *out,
                                 char *error, uint64_t cap) {
    EsFile file = {0};
    return give(&file, out,
                es_repository_baseline((EsRepository *)history, path, &file, error, (size_t)cap));
}

static int32_t DME_CALL baseline_exists(DmeHistory *history, const char *path, uint32_t *exists,
                                        char *error, uint64_t cap) {
    bool found = false;
    bool ok =
        es_repository_baseline_exists((EsRepository *)history, path, &found, error, (size_t)cap);
    *exists = found;
    return ok;
}

static int32_t DME_CALL read_current(DmeHistory *history, const char *path, DmeHistoryFile *out,
                                     char *error, uint64_t cap) {
    EsFile file = {0};
    return give(&file, out,
                es_repository_read((EsRepository *)history, path, &file, error, (size_t)cap));
}

static void DME_CALL file_free(DmeHistoryFile *file) {
    free(file->data);
    *file = (DmeHistoryFile){0};
}

static int32_t DME_CALL list_directory(DmeHistory *history, DmeHistoryList *list,
                                       const char *directory, uint32_t recursive, char *error,
                                       uint64_t cap) {
    return es_repository_paths((EsRepository *)history, directory, recursive != 0, (EsPaths *)list,
                               error, (size_t)cap);
}

static int32_t DME_CALL list_add(DmeHistoryList *list, const char *path) {
    return es_paths_add((EsPaths *)list, path);
}

static int32_t DME_CALL plan_put(DmeHistoryPlan *plan, const char *path, const uint8_t *data,
                                 uint64_t size, uint32_t exists, uint32_t mode, char *error,
                                 uint64_t cap) {
    return size <= SIZE_MAX && es_plan_put((EsPlan *)plan, path, data, (size_t)size, mode,
                                           exists != 0, error, (size_t)cap);
}

static int32_t DME_CALL plan_backup(DmeHistoryPlan *plan, const char *path) {
    return es_plan_backup((EsPlan *)plan, path);
}

static int32_t DME_CALL plan_affect(DmeHistoryPlan *plan, const char *key, const char *path,
                                    const char *selector) {
    return es_plan_affect((EsPlan *)plan, key, path, selector);
}

static int32_t DME_CALL plan_depend(DmeHistoryPlan *plan, const char *consumer,
                                    const char *provider, const char *role) {
    return es_plan_depend((EsPlan *)plan, consumer, provider, role);
}

static int32_t DME_CALL apply(DmeHistory *history, const DmeHistoryIntent *intents, uint32_t count,
                              char *error, uint64_t cap) {
    EsModifications view;

    if (!es_intents_view(intents, count, &view)) {
        return es_fail(error, (size_t)cap, DM_TEXT_REPOSITORY_INVALID_REPOSITORY_WRITE_S, "");
    }

    bool ok = es_repository_apply_batch((EsRepository *)history, view.items, view.count, error,
                                        (size_t)cap);
    es_intents_release(&view);
    return ok;
}

static int32_t DME_CALL targets_add(DmeHistoryTargets *targets, const char *id, const char *label,
                                    const char *path) {
    return es_targets_add((EsTargets *)targets, id, label, path);
}

static const char *DME_CALL identity(DmeHistory *history) {
    return es_repository_identity((EsRepository *)history);
}

static int32_t DME_CALL untracked(DmeHistory *history) {
    return es_repository_untracked((EsRepository *)history);
}

/* Hands each listed path to the plugin, then frees the list. */
static int32_t visit_all(EsPaths *paths, bool ok, DmeHistoryVisit visit, void *user, char *error,
                         uint64_t cap) {
    for (size_t i = 0; ok && i < paths->count; i++) {
        ok = visit(user, paths->items[i]) ||
             es_fail(error, (size_t)cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_REPOSITORY_PATH);
    }

    es_paths_free(paths);
    return ok;
}

static int32_t DME_CALL baseline_paths(DmeHistory *history, DmeHistoryVisit visit, void *user,
                                       char *error, uint64_t cap) {
    EsPaths paths = {0};
    bool ok = es_repository_baseline_paths((EsRepository *)history, &paths, error, (size_t)cap);
    return visit_all(&paths, ok, visit, user, error, cap);
}

static int32_t DME_CALL paths(DmeHistory *history, const char *directory, uint32_t recursive,
                              DmeHistoryVisit visit, void *user, char *error, uint64_t cap) {
    EsPaths list = {0};
    bool ok = es_repository_paths((EsRepository *)history, directory, recursive != 0, &list, error,
                                  (size_t)cap);
    return visit_all(&list, ok, visit, user, error, cap);
}

static int32_t DME_CALL plan_find(DmeHistoryPlan *plan, const char *path, DmeHistoryFile *view) {
    const EsFile *file = es_plan_find((EsPlan *)plan, path);
    *view = file ? (DmeHistoryFile){file->data, file->size, file->mode, file->exists}
                 : (DmeHistoryFile){0};
    return file != NULL;
}

static int32_t DME_CALL plan_paths(DmeHistoryPlan *handle, DmeHistoryVisit visit, void *user) {
    const EsPlan *plan = (const EsPlan *)handle;
    bool ok = true;

    for (size_t i = 0; ok && i < plan->count; i++) {
        ok = visit(user, plan->writes[i].path) != 0;
    }

    return ok;
}

static int32_t DME_CALL plan_keep_originals(DmeHistory *history, DmeHistoryPlan *plan,
                                            const char *key, char *error, uint64_t cap) {
    return es_plan_keep_originals((EsRepository *)history, (EsPlan *)plan, key, error, (size_t)cap);
}

static int32_t DME_CALL probe(const DmeHistoryAdapter *adapter, const char *checkout,
                              DmeHistory **out, char *error, uint64_t cap) {
    *out = (DmeHistory *)es_repository_probe(checkout, adapter, error, (size_t)cap);
    return *out != NULL;
}

static int32_t DME_CALL package(DmeHistory *history, const DmeHistoryIntent *intents,
                                uint32_t count, const DmeHost *host, char *error, uint64_t cap) {
    EsModifications view;

    if (!es_intents_view(intents, count, &view)) {
        return es_fail(error, (size_t)cap, DM_TEXT_REPOSITORY_INVALID_REPOSITORY_WRITE_S, "");
    }

    bool ok = es_package_plan((EsRepository *)history, &view, host, error, (size_t)cap);
    es_intents_release(&view);
    return ok;
}

static void DME_CALL close_probe(DmeHistory *history) {
    es_repository_close((EsRepository *)history);
}

const DmeHistoryApi es_history_table = {.size = sizeof(DmeHistoryApi),
                                        .version = DME_REPOSITORY_API,
                                        .root_read = root_read,
                                        .baseline = baseline,
                                        .baseline_exists = baseline_exists,
                                        .read = read_current,
                                        .file_free = file_free,
                                        .list_directory = list_directory,
                                        .list_add = list_add,
                                        .plan_put = plan_put,
                                        .plan_backup = plan_backup,
                                        .plan_affect = plan_affect,
                                        .plan_depend = plan_depend,
                                        .apply = apply,
                                        .targets_add = targets_add,
                                        .identity = identity,
                                        .untracked = untracked,
                                        .baseline_paths = baseline_paths,
                                        .paths = paths,
                                        .plan_find = plan_find,
                                        .plan_paths = plan_paths,
                                        .plan_keep_originals = plan_keep_originals,
                                        .probe = probe,
                                        .package = package,
                                        .close = close_probe};

const DmeHistoryApi *es_history_api(void) {
    return &es_history_table;
}
