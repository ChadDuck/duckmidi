#ifndef DUCK_ADAPTER_CLIENT_H
#define DUCK_ADAPTER_CLIENT_H

/* The bundled exporters' side of repository history. Everything stateful goes through the
   DmeHistoryApi the runtime (plugins/db) hands to adapter callbacks or to
   DmeHistoryAdapter.attach, so an exporter never links db and still loads without it. Files
   returned here are the plugin's own copies: free them with es_file_free. */
#include "../common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An open history scope: the runtime's table and its handle. */
typedef struct {
    const DmeHistoryApi *api;
    DmeHistory *history;
} EsHistory;

/* The desired write set being rendered. */
typedef struct {
    const DmeHistoryApi *api;
    DmeHistoryPlan *plan;
} EsHistoryPlan;

/* Detection: current content of a checkout file; absent files succeed with exists false. */
bool es_history_root_read(const DmeHistoryApi *, DmeHistoryRoot *, const char *path, EsFile *,
                          char *error, size_t cap);
bool es_history_baseline(EsHistory *, const char *path, EsFile *, char *error, size_t cap);
bool es_history_baseline_exists(EsHistory *, const char *path, bool *exists, char *error,
                                size_t cap);
bool es_history_read(EsHistory *, const char *path, EsFile *, char *error, size_t cap);
const char *es_history_identity(EsHistory *);
bool es_history_untracked(EsHistory *);
/* Adds every baseline path, or every current file below directory, to out. */
bool es_history_baseline_paths(EsHistory *, EsPaths *out, char *error, size_t cap);
bool es_history_paths(EsHistory *, const char *directory, bool recursive, EsPaths *out, char *error,
                      size_t cap);

/* mode: POSIX permission bits, 0 for 0644. */
bool es_history_plan_put(EsHistoryPlan *, const char *path, const void *data, size_t size,
                         uint32_t mode, bool exists, char *error, size_t cap);
bool es_history_plan_backup(EsHistoryPlan *, const char *path);
bool es_history_plan_affect(EsHistoryPlan *, const char *key, const char *path,
                            const char *selector);
bool es_history_plan_depend(EsHistoryPlan *, const char *consumer, const char *provider,
                            const char *role);
/* The desired state of a path already in the plan. *view borrows the plan's bytes until the
   plan changes; do not free it. False when the plan does not hold the path. */
bool es_history_plan_find(EsHistoryPlan *, const char *path, EsFile *view);
/* Adds every path put in the plan so far to out. */
bool es_history_plan_paths(EsHistoryPlan *, EsPaths *out);
/* Whether the path exists in the plan, or else in the baseline. plan may be NULL. */
bool es_history_plan_exists(EsHistory *, EsHistoryPlan *, const char *path, bool *exists,
                            char *error, size_t cap);
bool es_history_keep_originals(EsHistory *, EsHistoryPlan *, const char *key, char *error,
                               size_t cap);

/* Install: upserts the intents and applies the rendered state. */
bool es_history_apply(EsHistory *, const EsModification *, size_t count, char *error, size_t cap);

/* Outside history jobs. probe fails without an error when api is NULL (no db plugin). */
bool es_history_probe(EsHistory *out, const DmeHistoryApi *, const DmeHistoryAdapter *,
                      const char *checkout, char *error, size_t cap);
bool es_history_package(EsHistory *probe, const EsModifications *, const DmeHost *, char *error,
                        size_t cap);
void es_history_close(EsHistory *probe);

/* Callback results: hands collected paths or targets to the runtime's list. */
bool es_history_list(const DmeHistoryApi *, DmeHistoryList *, const EsPaths *);
bool es_history_targets(const DmeHistoryApi *, DmeHistoryTargets *, const EsTargets *);

#ifdef __cplusplus
}
#endif

#endif
