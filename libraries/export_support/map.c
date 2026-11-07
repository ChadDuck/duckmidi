#include "export_support.h"
#include <stdlib.h>
#include <string.h>

/* FNV-1a; repository paths are short ASCII-heavy strings. */
static uint64_t hash_key(const char *key) {
    uint64_t hash = 1469598103934665603ull;

    for (const unsigned char *p = (const unsigned char *)key; *p; ++p) {
        hash = (hash ^ *p) * 1099511628211ull;
    }

    return hash;
}

static bool grow(EsMap *map) {
    size_t capacity = map->capacity ? map->capacity * 2 : 64;
    const char **keys = calloc(capacity, sizeof(*keys));
    size_t *values = malloc(capacity * sizeof(*values));

    if (!keys || !values) {
        free(keys);
        free(values);
        return false;
    }

    for (size_t i = 0; i < map->capacity; ++i) {
        if (!map->keys[i]) {
            continue;
        }

        size_t slot = (size_t)hash_key(map->keys[i]) & (capacity - 1);

        while (keys[slot]) {
            slot = (slot + 1) & (capacity - 1);
        }

        keys[slot] = map->keys[i];
        values[slot] = map->values[i];
    }

    free(map->keys);
    free(map->values);
    map->keys = keys;
    map->values = values;
    map->capacity = capacity;
    return true;
}

bool es_map_put(EsMap *map, const char *key, size_t value) {
    if ((map->count + 1) * 4 > map->capacity * 3 && !grow(map)) {
        return false;
    }

    size_t slot = (size_t)hash_key(key) & (map->capacity - 1);

    while (map->keys[slot] && strcmp(map->keys[slot], key)) {
        slot = (slot + 1) & (map->capacity - 1);
    }

    if (!map->keys[slot]) {
        map->keys[slot] = key;
        map->count++;
    }

    map->values[slot] = value;
    return true;
}

bool es_map_get(const EsMap *map, const char *key, size_t *value) {
    if (!map->capacity || !key) {
        return false;
    }

    size_t slot = (size_t)hash_key(key) & (map->capacity - 1);

    while (map->keys[slot]) {
        if (!strcmp(map->keys[slot], key)) {
            if (value) {
                *value = map->values[slot];
            }

            return true;
        }

        slot = (slot + 1) & (map->capacity - 1);
    }

    return false;
}

void es_map_free(EsMap *map) {
    free(map->keys);
    free(map->values);
    *map = (EsMap){0};
}
