#ifndef DUCK_EXPORT_SUPPORT_H
#define DUCK_EXPORT_SUPPORT_H

#include "../../include/duckmidi_repository_plugin.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef DES_BUILD
#define DES_API __declspec(dllexport)
#else
#define DES_API __declspec(dllimport)
#endif
#else
#define DES_API __attribute__((visibility("default")))
#endif

typedef struct EsRepository EsRepository;
typedef struct EsRoot EsRoot;

/* String-keyed hash index. Keys are borrowed and must outlive the map. */
typedef struct {
    const char **keys;
    size_t *values;
    size_t capacity, count;
} EsMap;

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t mode;
    bool exists;
} EsFile;

/* Directory-entry metadata. The signature (size, mtime, mode) detects outside edits without
   reading contents; a changed signature is confirmed by hashing before it counts as drift. */
typedef struct {
    int64_t size, mtime; /* Bytes; nanoseconds since an arbitrary platform epoch. */
    uint32_t mode;
    bool exists, directory;
} EsStat;

typedef struct {
    char **items;
    size_t count, capacity;
    EsMap index;
} EsPaths;

typedef struct {
    char *path;
    EsFile file;
} EsWrite;

typedef struct {
    char *key, *path, *selector;
} EsEffect;

typedef struct {
    char *consumer, *provider, *role;
} EsDependency;

/* The complete desired write set for one operation. Dependencies are validated in memory
   (missing providers, cycles) and are not stored. */
typedef struct {
    EsWrite *writes;
    size_t count, capacity;
    EsMap index;
    EsPaths backups;
    EsEffect *effects;
    size_t effect_count, effect_capacity;
    EsDependency *dependencies;
    size_t dependency_count, dependency_capacity;
} EsPlan;

typedef struct {
    char *key, *label;
    uint8_t *data;
    size_t size;
    uint64_t order;
    EsPaths paths;
} EsModification;

typedef struct {
    EsModification *items;
    size_t count;
} EsModifications;

/* target_name and capabilities (DME_REPOSITORY_* flags) describe the scope in history views. */
typedef struct {
    const char *plugins, *checkout, *exporter, *target, *version, *target_name;
    uint32_t capabilities;
    const DmeHost *host;
    const DmeHistoryAdapter *adapter;
} EsOpen;

typedef struct {
    uint64_t id, generation;
    uint32_t capabilities;
    char *exporter, *target, *target_name, *checkout, *version;
} EsScope;

typedef struct {
    EsScope *items;
    size_t count;
} EsScopes;

typedef struct {
    DmeSampleTarget *items;
    size_t count, capacity;
} EsTargets;

DES_API bool es_map_put(EsMap *, const char *key, size_t value);
DES_API bool es_map_get(const EsMap *, const char *key, size_t *value);
DES_API void es_map_free(EsMap *);

DES_API EsRepository *es_repository_open(const EsOpen *, char *error, size_t cap);
/* Read-only native inspection for packages; creates no history directory or database. */
DES_API EsRepository *es_repository_probe(const char *checkout, const DmeHistoryAdapter *, char *,
                                          size_t);
/* History off: no database; installs render over the current files and write them directly.
   With keep_originals, each file's previous content is kept as "<path>.bkp" unless one already
   exists; without it, files are simply overwritten. */
DES_API EsRepository *es_repository_open_untracked(const char *checkout, const DmeHistoryAdapter *,
                                                   const DmeHost *, bool keep_originals, char *,
                                                   size_t);
DES_API bool es_repository_untracked(const EsRepository *);
/* Renders intents through the repository.s adapter without writing anything. */
DES_API bool es_render(EsRepository *, const EsModifications *, EsPlan *, char *, size_t);
/* The callback table given to adapters; DmeHistory, DmeHistoryPlan and DmeHistoryList are
   EsRepository, EsPlan and EsPaths. */
DES_API const DmeHistoryApi *es_history_api(void);
/* A borrowed EsModifications view of intents; free it with es_intents_release. */
DES_API bool es_intents_view(const DmeHistoryIntent *, uint32_t, EsModifications *);
DES_API void es_intents_release(EsModifications *);
DES_API void es_repository_close(EsRepository *);
DES_API bool es_repository_initialize(EsRepository *, char *error, size_t cap);
DES_API bool es_repository_apply(EsRepository *, const char *key, const char *label,
                                 const void *recipe, size_t size, char *error, size_t cap);
DES_API bool es_repository_apply_batch(EsRepository *, const EsModification *, size_t, char *,
                                       size_t);
/* NULL key reverts the complete displayed scope. */
DES_API bool es_repository_revert(EsRepository *, const char *key, char *error, size_t cap);
DES_API bool es_repository_modifications(EsRepository *, EsModifications *, char *, size_t);
DES_API void es_modifications_free(EsModifications *);
DES_API bool es_history_scopes(const char *plugins, const char *exporter, EsScopes *, char *,
                               size_t);
DES_API void es_scopes_free(EsScopes *);
/* Initialized content of a path. Cached for the repository's lifetime. */
DES_API bool es_repository_baseline(EsRepository *, const char *path, EsFile *, char *, size_t);
/* Whether a path existed at initialization, without reading its content. */
DES_API bool es_repository_baseline_exists(EsRepository *, const char *path, bool *exists, char *,
                                           size_t);
/* Current checkout content. */
DES_API bool es_repository_read(EsRepository *, const char *path, EsFile *, char *, size_t);
DES_API const char *es_repository_checkout(const EsRepository *);
DES_API const char *es_repository_identity(const EsRepository *);
DES_API const DmeHost *es_repository_host(const EsRepository *);
DES_API bool es_repository_paths(EsRepository *, const char *directory, bool recursive, EsPaths *,
                                 char *, size_t);
DES_API bool es_repository_baseline_paths(EsRepository *, EsPaths *, char *, size_t);
DES_API uint64_t es_repository_scope_id(const EsRepository *);
DES_API uint64_t es_repository_generation(const EsRepository *);

/* Read-only anchored access for target detection before a history scope is opened. */
DES_API EsRoot *es_root_open(const char *, char *, size_t);
DES_API void es_root_close(EsRoot *);
DES_API const char *es_root_name(const EsRoot *);
DES_API const char *es_root_identity(const EsRoot *);
DES_API bool es_fs_read(EsRoot *, const char *, EsFile *, char *, size_t);
DES_API bool es_fs_paths(EsRoot *, const char *, bool recursive, EsPaths *, char *, size_t);

DES_API bool es_paths_add(EsPaths *, const char *);
DES_API bool es_paths_has(const EsPaths *, const char *);
DES_API void es_paths_free(EsPaths *);
DES_API void es_file_free(EsFile *);
DES_API bool es_plan_put(EsPlan *, const char *path, const void *, size_t, uint32_t mode,
                         bool exists, char *, size_t);
DES_API bool es_plan_backup(EsPlan *, const char *path);
DES_API bool es_plan_keep_originals(EsRepository *, EsPlan *, const char *key, char *, size_t);
DES_API bool es_plan_affect(EsPlan *, const char *key, const char *path, const char *selector);
DES_API bool es_plan_depend(EsPlan *, const char *consumer, const char *provider, const char *role);
DES_API const EsFile *es_plan_find(const EsPlan *, const char *);
DES_API void es_plan_free(EsPlan *);
DES_API void es_sha256(const void *, size_t, uint8_t hash[32]);
DES_API bool es_targets_add(EsTargets *, const char *id, const char *label, const char *path);
DES_API void es_targets_free(EsTargets *);
/* The host-facing job table, resolved by name from plugins/db. */
DES_API const DmeHistoryRuntime *DME_CALL duckmidi_history_runtime(uint32_t version);

#ifdef __cplusplus
}
#endif

#endif
