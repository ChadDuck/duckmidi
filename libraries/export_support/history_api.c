#include "export_support.h"
#include "internal.h"
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
                                 uint64_t size, uint32_t exists, char *error, uint64_t cap) {
    return size <= SIZE_MAX && es_plan_put((EsPlan *)plan, path, data, (size_t)size, 0, exists != 0,
                                           error, (size_t)cap);
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

bool es_intents_view(const DmeHistoryIntent *intents, uint32_t count, EsModifications *out) {
    *out = (EsModifications){calloc(count + 1, sizeof(EsModification)), count};

    for (uint32_t i = 0; out->items && i < count; i++) {
        const DmeHistoryIntent *intent = &intents[i];

        /* Strings must be terminated where their length says; the runtime compares keys as C
           strings. */
        if (!intent->key.data || strlen(intent->key.data) != intent->key.length ||
            !intent->label.data || strlen(intent->label.data) != intent->label.length ||
            intent->recipe_size > SIZE_MAX) {
            es_intents_release(out);
            return false;
        }

        out->items[i] = (EsModification){.key = (char *)intent->key.data,
                                         .label = (char *)intent->label.data,
                                         .data = (uint8_t *)intent->recipe,
                                         .size = (size_t)intent->recipe_size};
    }

    return out->items != NULL;
}

void es_intents_release(EsModifications *view) {
    free(view->items);
    *view = (EsModifications){0};
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

static const DmeHistoryApi api = {.size = sizeof(DmeHistoryApi),
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
                                  .targets_add = targets_add};

const DmeHistoryApi *es_history_api(void) {
    return &api;
}
