#include "recipe.h"
#include "internal.h"
#include <string.h>
#include <stdlib.h>

static void put64(uint8_t *p, uint64_t n) {
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (uint8_t)(n >> (i * 8));
    }
}

static uint64_t get64(const uint8_t *p) {
    uint64_t n = 0;

    for (unsigned i = 0; i < 8; i++) {
        n |= (uint64_t)p[i] << (i * 8);
    }

    return n;
}

bool es_recipe_add(EsRecipe *b, const char *name, const void *data, size_t size) {
    size_t length = name ? strlen(name) + 1 : 0;

    if (!length || length > 256 || size > 256u * 1024u * 1024u || (size && !data) ||
        size > 256u * 1024u * 1024u - length - 16 ||
        b->size > 256u * 1024u * 1024u - size - length - 16 ||
        !es_reserve((void **)&b->data, &b->capacity, b->size + size + length + 16, 1)) {
        return false;
    }

    put64(b->data + b->size, length);
    put64(b->data + b->size + 8, size);
    memcpy(b->data + b->size + 16, name, length);

    if (size) {
        memcpy(b->data + b->size + 16 + length, data, size);
    }

    b->size += 16 + length + size;
    return true;
}

const uint8_t *es_recipe_get(const void *data, size_t size, const char *name, size_t *length) {
    const uint8_t *p = data, *found = NULL;
    size_t result = 0;

    while (size) {
        if (size < 16) {
            return NULL;
        }

        uint64_t key = get64(p), bytes = get64(p + 8);

        if (!key || key > 256 || key > size - 16 || bytes > size - 16 - key || p[16 + key - 1] ||
            memchr(p + 16, 0, (size_t)key - 1)) {
            return NULL;
        }

        if (!strcmp((const char *)p + 16, name)) {
            if (found) {
                return NULL;
            }

            found = p + 16 + key;
            result = (size_t)bytes;
        }

        size -= (size_t)(16 + key + bytes);
        p += 16 + key + bytes;
    }

    if (length) {
        *length = result;
    }

    return found;
}

const char *es_recipe_text(const void *data, size_t size, const char *name) {
    size_t length = 0;
    const uint8_t *p = es_recipe_get(data, size, name, &length);
    return p && length && !p[length - 1] && !memchr(p, 0, length - 1) ? (const char *)p : NULL;
}

bool es_recipe_string(EsRecipe *b, const char *field, const char *text) {
    return text && es_recipe_add(b, field, text, strlen(text) + 1);
}

void es_recipe_free(EsRecipe *b) {
    free(b->data);
    *b = (EsRecipe){0};
}
