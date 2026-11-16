#ifndef DUCK_COMMON_H
#define DUCK_COMMON_H

/* Small utilities shared by the history database (libraries/db), the N64 exporter library
   (libraries/n64) and the exporters. One source, compiled into each library that uses it; never
   exported from a plugin. */
#include "../../include/duckmidi_repository_plugin.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* Formats the message into error and returns false. */
bool es_fail(char *error, size_t cap, const char *format, ...);
/* Grows *data to hold count items of width bytes, doubling; false without memory. */
bool es_reserve(void **data, size_t *capacity, size_t count, size_t width);
void es_file_free(EsFile *);
bool es_map_put(EsMap *, const char *key, size_t value);
bool es_map_get(const EsMap *, const char *key, size_t *value);
void es_map_free(EsMap *);
void es_sha256(const void *, size_t, uint8_t hash[32]);

#define ES_PATH_LIMIT 8192 /* Bytes in a repository path, terminator included. */
#define ES_MAX_PATHS 20000 /* Paths in one list or plan. */

/* A list of distinct strings with an index. */
typedef struct {
    char **items;
    size_t count, capacity;
    EsMap index;
} EsPaths;

/* A safe repository-relative path: '/' separators, no empty, "." or ".." parts, no drive. */
bool es_relative(const char *);
/* Adds any string once; es_paths_add also requires a safe relative path. */
bool es_strings_add(EsPaths *, const char *);
bool es_paths_add(EsPaths *, const char *);
bool es_paths_has(const EsPaths *, const char *);
void es_paths_free(EsPaths *);

/* One current intent: a stable key, a label and its recipe bytes. */
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

/* A borrowed EsModifications view of intents; free it with es_intents_release. */
bool es_intents_view(const DmeHistoryIntent *, uint32_t, EsModifications *);
void es_intents_release(EsModifications *);

/* Physical replace targets an adapter lists. */
typedef struct {
    DmeSampleTarget *items;
    size_t count, capacity;
} EsTargets;

bool es_targets_add(EsTargets *, const char *id, const char *label, const char *path);
void es_targets_free(EsTargets *);

#ifdef __cplusplus
}
#endif

#endif
