#include "common.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

bool es_fail(char *error, size_t cap, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, cap, format, args);
    va_end(args);
    return false;
}

bool es_reserve(void **data, size_t *capacity, size_t count, size_t width) {
    if (count <= *capacity) {
        return true;
    }

    size_t next = *capacity ? *capacity : 16;

    while (next < count && next <= SIZE_MAX / 2) {
        next *= 2;
    }

    if (next < count || next > SIZE_MAX / width) {
        return false;
    }

    void *grown = realloc(*data, next * width);

    if (!grown) {
        return false;
    }

    *data = grown;
    *capacity = next;
    return true;
}

void es_file_free(EsFile *file) {
    free(file->data);
    *file = (EsFile){0};
}
