#ifndef DES_RECIPE_H
#define DES_RECIPE_H

#include "../common/common.h"

typedef struct {
    uint8_t *data;
    size_t size, capacity;
} EsRecipe;

/* Versioned length-delimited fields. Unknown/malformed formats are rejected, never repaired. */
bool es_recipe_add(EsRecipe *, const char *field, const void *, size_t);
const uint8_t *es_recipe_get(const void *, size_t, const char *, size_t *);
const char *es_recipe_text(const void *, size_t, const char *);
bool es_recipe_string(EsRecipe *, const char *, const char *);
void es_recipe_free(EsRecipe *);

#endif
