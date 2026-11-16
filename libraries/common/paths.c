#include "common.h"
#include <stdlib.h>
#include <string.h>

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    return copy ? memcpy(copy, text, size) : NULL;
}

static DmeString string(const char *s) {
    return (DmeString){s ? s : "", s ? strlen(s) : 0};
}

bool es_relative(const char *path) {
    if (!path || !*path || strlen(path) >= ES_PATH_LIMIT || strchr(path, '\\') ||
        strchr(path, ':') || path[0] == '/') {
        return false;
    }

    for (const char *p = path; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);

        if (!n || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.') ||
            p[n - 1] == '.' || p[n - 1] == ' ') {
            return false;
        }

        for (size_t i = 0; i < n; ++i) {
            if ((unsigned char)p[i] < 32 || strchr("*?\"<>|", p[i])) {
                return false;
            }
        }

        if (end && !end[1]) {
            return false;
        }

        p += n + (end != NULL);
    }

    return true;
}

bool es_paths_has(const EsPaths *paths, const char *path) {
    return es_map_get(&paths->index, path, NULL);
}

bool es_strings_add(EsPaths *paths, const char *path) {
    if (es_paths_has(paths, path)) {
        return true;
    }

    if (paths->count >= ES_MAX_PATHS ||
        !es_reserve((void **)&paths->items, &paths->capacity, paths->count + 1, sizeof(char *))) {
        return false;
    }

    char *copy = copy_text(path);

    if (!copy || !es_map_put(&paths->index, copy, paths->count)) {
        free(copy);
        return false;
    }

    paths->items[paths->count++] = copy;
    return true;
}

bool es_paths_add(EsPaths *paths, const char *path) {
    return es_relative(path) && es_strings_add(paths, path);
}

void es_paths_free(EsPaths *paths) {
    for (size_t i = 0; i < paths->count; ++i) {
        free(paths->items[i]);
    }

    free(paths->items);
    es_map_free(&paths->index);
    *paths = (EsPaths){0};
}

bool es_targets_add(EsTargets *t, const char *id, const char *label, const char *path) {
    if (!id || !label || !path || t->count >= 20000 ||
        !es_reserve((void **)&t->items, &t->capacity, t->count + 1, sizeof(*t->items))) {
        return false;
    }

    char *a = copy_text(id), *b = copy_text(label), *c = copy_text(path);

    if (!a || !b || !c) {
        free(a);
        free(b);
        free(c);
        return false;
    }

    t->items[t->count++] = (DmeSampleTarget){string(a), string(b), string(c)};
    return true;
}

void es_targets_free(EsTargets *t) {
    for (size_t i = 0; i < t->count; i++) {
        free((void *)t->items[i].id.data);
        free((void *)t->items[i].label.data);
        free((void *)t->items[i].path.data);
    }

    free(t->items);
    *t = (EsTargets){0};
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
