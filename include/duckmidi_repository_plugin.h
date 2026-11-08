#ifndef DUCKMIDI_REPOSITORY_PLUGIN_H
#define DUCKMIDI_REPOSITORY_PLUGIN_H

#include "duckmidi_export_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Repository installs with persistent history.

   An exporter that installs into a folder, such as a game source checkout, returns a
   DmeHistoryAdapter from DmeExporter.history. duckmidi's history runtime (plugins/db)
   owns the shared SQLite ledger, the durable write transaction, crash recovery, drift
   detection and the history view; the adapter supplies only what is specific to its format:
   checkout detection, the files that make up its namespace, and how its current intents
   render into files. Every callback receives the runtime's DmeHistoryApi. */

#define DME_REPOSITORY_API 2u

enum {
    DME_REPOSITORY_INSPECT,
    DME_REPOSITORY_INSTALL,
    DME_REPOSITORY_INITIALIZE,
    DME_REPOSITORY_REVERT,
    DME_REPOSITORY_REVERT_ALL
};

/* Scope capabilities. Adapters report DME_REPOSITORY_CUSTOM from detect; the runtime adds the
   state flags. */
enum {
    DME_REPOSITORY_CUSTOM = 1u, /* The target accepts custom sample mappings. */
    DME_REPOSITORY_INITIALIZED = 2u,
    DME_REPOSITORY_WRITABLE = 4u,
    DME_REPOSITORY_MISSING = 8u
};

typedef struct {
    uint32_t size, action, keep_originals;
    uint64_t scope;
    DmeString plugins_directory, checkout, logical_key;
    /* Nonzero when repository history is off: INSPECT and INSTALL run without the ledger,
       and an install writes files directly, keeping "<path>.bkp" originals only when
       keep_originals is nonzero. */
    uint32_t untracked;
} DmeRepositoryRequest;

typedef struct {
    uint32_t size, capabilities;
    uint64_t id;
    DmeString target, target_name, checkout, game_version;
} DmeRepositoryScope;

typedef struct {
    uint32_t size, can_revert, path_count;
    DmeString key, label;
    const DmeString *paths;
} DmeRepositoryRow;

typedef struct {
    uint32_t size, scope_count, row_count, target_count;
    uint64_t selected_scope;
    const DmeRepositoryScope *scopes;
    const DmeRepositoryRow *rows;
    const DmeSampleTarget *targets;
    DmeString notice;
} DmeRepositoryView;

/* Opaque runtime handles, valid only during the callback that receives them. */
typedef struct DmeHistory DmeHistory;               /* An open checkout scope. */
typedef struct DmeHistoryRoot DmeHistoryRoot;       /* A read-only checkout during detection. */
typedef struct DmeHistoryPlan DmeHistoryPlan;       /* The desired write set being rendered. */
typedef struct DmeHistoryList DmeHistoryList;       /* Paths collected by inventory. */
typedef struct DmeHistoryTargets DmeHistoryTargets; /* Physical replace targets. */

/* File contents owned by the caller; release with DmeHistoryApi.file_free. */
typedef struct {
    uint8_t *data;
    uint64_t size;
    uint32_t mode, exists;
} DmeHistoryFile;

/* What detect reports for a checkout. id and version identify the history scope together
   with the exporter and the physical checkout; name titles the history section. */
typedef struct {
    char id[32], name[64], version[64];
    uint32_t capabilities;
} DmeHistoryTarget;

/* One current logical modification: a stable key (such as a song slot), a short label shown
   in the history list and an adapter-defined recipe that renders it. */
typedef struct {
    DmeString key, label;
    const uint8_t *recipe;
    uint64_t recipe_size;
} DmeHistoryIntent;

/* Main thread, after an action finished: the checkout it ran against. */
typedef struct {
    uint32_t size, target_count;
    DmeString checkout, identity;
    const DmeHistoryTarget *target;
    const DmeSampleTarget *targets;
} DmeHistoryResult;

/* Functions the runtime provides to adapter callbacks. Paths are repository-relative with '/'
   separators. Functions return nonzero on success and write a reason to error otherwise. */
typedef struct DmeHistoryApi {
    uint32_t size, version;
    /* Detection: current content of a checkout file. Absent files succeed with exists 0. */
    int32_t(DME_CALL *root_read)(DmeHistoryRoot *, const char *path, DmeHistoryFile *, char *error,
                                 uint64_t capacity);
    /* Initialized content of a path, and whether it existed, without reading it. */
    int32_t(DME_CALL *baseline)(DmeHistory *, const char *path, DmeHistoryFile *, char *error,
                                uint64_t capacity);
    int32_t(DME_CALL *baseline_exists)(DmeHistory *, const char *path, uint32_t *exists,
                                       char *error, uint64_t capacity);
    /* Current checkout content. */
    int32_t(DME_CALL *read)(DmeHistory *, const char *path, DmeHistoryFile *, char *error,
                            uint64_t capacity);
    void(DME_CALL *file_free)(DmeHistoryFile *);
    /* Inventory: adds every regular file below directory (recursively when asked). */
    int32_t(DME_CALL *list_directory)(DmeHistory *, DmeHistoryList *, const char *directory,
                                      uint32_t recursive, char *error, uint64_t capacity);
    int32_t(DME_CALL *list_add)(DmeHistoryList *, const char *path);
    /* Render: the desired complete state of a path (exists 0 removes it), a request to keep
       the path's initialized original as "<path>.bkp", the intent that affects a path, and a
       consumer that requires a provider. */
    int32_t(DME_CALL *plan_put)(DmeHistoryPlan *, const char *path, const uint8_t *data,
                                uint64_t size, uint32_t exists, char *error, uint64_t capacity);
    int32_t(DME_CALL *plan_backup)(DmeHistoryPlan *, const char *path);
    int32_t(DME_CALL *plan_affect)(DmeHistoryPlan *, const char *key, const char *path,
                                   const char *selector);
    int32_t(DME_CALL *plan_depend)(DmeHistoryPlan *, const char *consumer, const char *provider,
                                   const char *role);
    /* Install: upserts intents by key and applies the newly rendered state durably. */
    int32_t(DME_CALL *apply)(DmeHistory *, const DmeHistoryIntent *, uint32_t count, char *error,
                             uint64_t capacity);
    int32_t(DME_CALL *targets_add)(DmeHistoryTargets *, const char *id, const char *label,
                                   const char *path);
} DmeHistoryApi;

typedef struct DmeHistoryAdapter {
    uint32_t size, version; /* sizeof(DmeHistoryAdapter), DME_REPOSITORY_API. */
    /* Worker. Recognizes a checkout or fails with the reason it is not one. */
    int32_t(DME_CALL *detect)(const DmeHistoryApi *, DmeHistoryRoot *, DmeHistoryTarget *,
                              char *error, uint64_t capacity);
    /* Optional, worker. Lists the files snapshotted as the initialized baseline. Without it,
       each path's baseline is captured when an install first writes it. */
    int32_t(DME_CALL *inventory)(const DmeHistoryApi *, DmeHistory *, DmeHistoryList *, char *error,
                                 uint64_t capacity);
    /* Optional, any thread. Limits writes to the adapter's namespace. */
    int32_t(DME_CALL *allow)(const char *path);
    /* Worker. Renders every current intent over the baseline into the complete desired state.
       Paths no longer rendered return to their baseline. */
    int32_t(DME_CALL *render)(const DmeHistoryApi *, DmeHistory *, const DmeHistoryIntent *,
                              uint32_t count, DmeHistoryPlan *, char *error, uint64_t capacity);
    /* Optional, worker. Install preflight with read access; no files change. */
    int32_t(DME_CALL *prepare)(void *export_job, const DmeContext *, const DmeHost *,
                               const DmeHistoryApi *, DmeHistory *, const DmeHistoryTarget *,
                               char *error, uint64_t capacity);
    /* Worker. Converts the export job into intents and calls DmeHistoryApi.apply. */
    int32_t(DME_CALL *install)(void *export_job, const DmeContext *, const DmeHost *,
                               const DmeHistoryApi *, DmeHistory *, char *error, uint64_t capacity);
    /* Optional, worker. Lists physical files a custom sample may replace. */
    int32_t(DME_CALL *targets)(const DmeHistoryApi *, DmeHistory *, DmeHistoryTargets *,
                               char *error, uint64_t capacity);
    /* Optional, main thread. Receives the detected checkout after any action. */
    int32_t(DME_CALL *finish)(void *instance, const DmeHistoryResult *, char *error,
                              uint64_t capacity);
    /* Optional, main thread, install only: copies job settings before the worker starts. */
    void(DME_CALL *begin_install)(void *export_job, const DmeRepositoryRequest *);
} DmeHistoryAdapter;

/* Host-facing jobs exported by the history runtime as duckmidi_history_runtime(version).
   create_job runs on the main thread and borrows export_job (INSTALL only) until destroy_job;
   prepare and run run on a worker; finish and describe run on the main thread after the join. */
typedef struct DmeHistoryRuntime {
    uint32_t size, version;
    void *(DME_CALL *create_job)(const DmeHistoryAdapter *, const char *exporter, void *instance,
                                 void *export_job, const DmeRepositoryRequest *, char *error,
                                 uint64_t capacity);
    int32_t(DME_CALL *prepare)(void *job, const DmeContext *, const DmeHost *, char *error,
                               uint64_t capacity);
    int32_t(DME_CALL *run)(void *job, const DmeContext *, const DmeHost *, char *error,
                           uint64_t capacity);
    int32_t(DME_CALL *finish)(void *job, char *error, uint64_t capacity);
    int32_t(DME_CALL *describe)(const void *job, DmeRepositoryView *);
    void(DME_CALL *destroy_job)(void *job);
} DmeHistoryRuntime;

typedef const DmeHistoryRuntime *(DME_CALL *DmeHistoryRuntimeEntry)(uint32_t version);

#ifdef __cplusplus
}
#endif
#endif
