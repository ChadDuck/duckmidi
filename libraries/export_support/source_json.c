#include "source.h"
#include "../../src/text/en.h"
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const yyjson_val *value;
    size_t begin, end;
} Span;

struct EsJson {
    yyjson_doc *doc;
    char *input, *source;
    size_t size, at;
    Span *spans;
    size_t count, capacity;
};

bool es_edits_add(EsEdits *edits, size_t begin, size_t end, const char *data, size_t size,
                  char *error, size_t cap) {
    if (end < begin || (size && !data) || edits->count >= 20000 ||
        !es_reserve((void **)&edits->items, &edits->capacity, edits->count + 1,
                    sizeof(*edits->items))) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_OR_EXCESSIVE_STRUCTURAL_AUDIO_EDITS);
    }

    char *copy = malloc(size + 1);

    if (!copy) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_RETAIN_STRUCTURAL_AUDIO_EDIT);
    }

    if (size) {
        memcpy(copy, data, size);
    }

    copy[size] = 0;
    size_t order = edits->count;
    edits->items[edits->count++] = (EsEdit){begin, end, copy, size, order};
    return true;
}

static int edit_order(const void *left, const void *right) {
    const EsEdit *a = left, *b = right;

    if (a->begin != b->begin) {
        return a->begin < b->begin ? -1 : 1;
    }

    return a->order < b->order ? -1 : a->order > b->order;
}

bool es_edits_apply(EsEdits *edits, const void *source, size_t size, EsFile *out, char *error,
                    size_t cap) {
    qsort(edits->items, edits->count, sizeof(*edits->items), edit_order);
    size_t end = 0, total = size;

    for (size_t i = 0; i < edits->count; ++i) {
        const EsEdit *edit = &edits->items[i];

        if (edit->begin < end || edit->end > size || edit->size > 8 * 1024 * 1024 ||
            total > 16 * 1024 * 1024 - edit->size) {
            return es_fail(error, cap,
                           DM_TEXT_REPOSITORY_CONFLICTING_OR_OVERSIZED_STRUCTURAL_AUDIO_EDITS);
        }

        total = total - (edit->end - edit->begin) + edit->size;
        end = edit->end;
    }

    uint8_t *data = malloc(total + 1);

    if (!data) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_RENDER_STRUCTURAL_AUDIO_EDITS);
    }

    size_t at = 0, written = 0;

    for (size_t i = 0; i < edits->count; ++i) {
        const EsEdit *edit = &edits->items[i];
        size_t gap = edit->begin - at;
        memcpy(data + written, (const uint8_t *)source + at, gap);
        written += gap;
        memcpy(data + written, edit->data, edit->size);
        written += edit->size;
        at = edit->end;
    }

    memcpy(data + written, (const uint8_t *)source + at, size - at);
    data[total] = 0;
    *out = (EsFile){.data = data, .size = total, .exists = true};
    return true;
}

void es_edits_free(EsEdits *edits) {
    for (size_t i = 0; i < edits->count; ++i) {
        free(edits->items[i].data);
    }

    free(edits->items);
    *edits = (EsEdits){0};
}

static bool skip(EsJson *j) {
    while (j->at < j->size) {
        char c = j->source[j->at];

        if (strchr(" \t\n\r", c)) {
            ++j->at;
        } else if (c == '/' && j->at + 1 < j->size && j->source[j->at + 1] == '/') {
            j->at += 2;

            while (j->at < j->size && j->source[j->at] != '\n') {
                ++j->at;
            }
        } else if (c == '/' && j->at + 1 < j->size && j->source[j->at + 1] == '*') {
            j->at += 2;

            while (j->at + 1 < j->size &&
                   !(j->source[j->at] == '*' && j->source[j->at + 1] == '/')) {
                ++j->at;
            }

            if (j->at + 1 >= j->size) {
                return false;
            }

            j->at += 2;
        } else {
            break;
        }
    }

    return true;
}

static bool consume(EsJson *j, char expected) {
    if (!skip(j) || j->at >= j->size || j->source[j->at] != expected) {
        return false;
    }

    ++j->at;
    return true;
}

static bool string_end(EsJson *j) {
    if (!consume(j, '"')) {
        return false;
    }

    while (j->at < j->size) {
        char c = j->source[j->at++];

        if (c == '"') {
            return true;
        }

        if (c == '\\' && j->at < j->size) {
            ++j->at;
        }
    }

    return false;
}

static bool spans(EsJson *j, yyjson_val *value, unsigned depth) {
    if (depth > 64 || !skip(j) || j->count >= 20000 ||
        !es_reserve((void **)&j->spans, &j->capacity, j->count + 1, sizeof(*j->spans))) {
        return false;
    }

    size_t index = j->count++;
    j->spans[index] = (Span){.value = value, .begin = j->at};

    if (yyjson_is_obj(value)) {
        if (!consume(j, '{')) {
            return false;
        }

        yyjson_obj_iter iter = yyjson_obj_iter_with(value);
        yyjson_val *key;
        size_t count = 0;

        while ((key = yyjson_obj_iter_next(&iter))) {
            yyjson_obj_iter prior = yyjson_obj_iter_with(value);

            for (size_t k = 0; k < count; ++k) {
                if (!strcmp(yyjson_get_str(yyjson_obj_iter_next(&prior)), yyjson_get_str(key))) {
                    return false;
                }
            }

            if ((count++ && !consume(j, ',')) || !skip(j)) {
                return false;
            }

            size_t begin = j->at;

            if (!string_end(j) ||
                !es_reserve((void **)&j->spans, &j->capacity, j->count + 1, sizeof(*j->spans))) {
                return false;
            }

            j->spans[j->count++] = (Span){key, begin, j->at};

            if (!consume(j, ':') || !spans(j, yyjson_obj_iter_get_val(key), depth + 1)) {
                return false;
            }
        }

        if (!consume(j, '}')) {
            return false;
        }
    } else if (yyjson_is_arr(value)) {
        if (!consume(j, '[')) {
            return false;
        }

        size_t i, n;
        yyjson_val *item;
        yyjson_arr_foreach(value, i, n, item) {
            if ((i && !consume(j, ',')) || !spans(j, item, depth + 1)) {
                return false;
            }
        }

        if (!consume(j, ']')) {
            return false;
        }
    } else if (yyjson_is_str(value)) {
        if (!string_end(j)) {
            return false;
        }
    } else {
        size_t start = j->at;

        while (j->at < j->size && !strchr(" \t\r\n,]}/", j->source[j->at])) {
            ++j->at;
        }

        if (start == j->at) {
            return false;
        }
    }

    j->spans[index].end = j->at;
    return true;
}

EsJson *es_json_parse(const void *bytes, size_t size, char *error, size_t cap) {
    EsJson *j = size <= 8 * 1024 * 1024 && bytes ? calloc(1, sizeof(*j)) : NULL;

    if (j) {
        j->input = malloc(size + YYJSON_PADDING_SIZE);
        j->source = malloc(size + 1);
    }

    if (!j || !j->input || !j->source) {
        es_json_free(j);
        es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_BOUNDED_NATIVE_JSON_DEFINITION);
        return NULL;
    }

    memcpy(j->input, bytes, size);
    memset(j->input + size, 0, YYJSON_PADDING_SIZE);
    memcpy(j->source, bytes, size);
    j->source[size] = 0;
    j->size = size;
    j->doc = yyjson_read_opts(j->input, size, YYJSON_READ_INSITU | YYJSON_READ_ALLOW_COMMENTS, NULL,
                              NULL);

    if (!j->doc || !spans(j, yyjson_doc_get_root(j->doc), 0) || !skip(j) || j->at != size) {
        es_json_free(j);
        es_fail(error, cap,
                DM_TEXT_REPOSITORY_MALFORMED_DUPLICATE_KEY_OR_UNSUPPORTED_NATIVE_JSON_DEFINITION);
        return NULL;
    }

    return j;
}

yyjson_val *es_json_root(EsJson *j) {
    return yyjson_doc_get_root(j->doc);
}

char *es_json_quote(const char *value) {
    size_t size = strlen(value);
    char *out = size <= (SIZE_MAX - 3) / 6 ? malloc(size * 6 + 3) : NULL;

    if (!out) {
        return NULL;
    }

    size_t n = 0;
    out[n++] = '"';

    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            out[n++] = '\\';
            out[n++] = (char)*p;
        } else if (*p < 32) {
            snprintf(out + n, 7, "\\u%04x", *p);
            n += 6;
        } else {
            out[n++] = (char)*p;
        }
    }

    out[n++] = '"';
    out[n] = 0;
    return out;
}

bool es_json_set(EsJson *j, yyjson_val *object, const char *member, const char *value,
                 EsEdits *edits, char *error, size_t cap) {
    if (!yyjson_is_obj(object) || !member || !value) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_EXPECTED_NATIVE_JSON_OBJECT_MEMBER);
    }

    yyjson_val *existing = yyjson_obj_get(object, member);
    const yyjson_val *find = existing ? existing : object;
    const Span *span = NULL;

    for (size_t i = 0; i < j->count; ++i) {
        if (j->spans[i].value == find) {
            span = &j->spans[i];
            break;
        }
    }

    if (!span) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_LOCATE_STRUCTURAL_NATIVE_JSON_MEMBER);
    }

    if (existing) {
        return es_edits_add(edits, span->begin, span->end, value, strlen(value), error, cap);
    }

    char *key = es_json_quote(member);
    size_t n = key ? strlen(key) + strlen(value) + 4 : 0;
    char *insert = n ? malloc(n) : NULL;
    bool ok = false;

    if (insert) {
        bool comma = yyjson_obj_size(object) != 0;

        for (size_t i = 0; i < edits->count; ++i) {
            comma |= edits->items[i].begin == span->end - 1 && edits->items[i].end == span->end - 1;
        }

        snprintf(insert, n, "%s%s:%s", comma ? "," : "", key, value);
        ok = es_edits_add(edits, span->end - 1, span->end - 1, insert, strlen(insert), error, cap);
    }

    free(insert);
    free(key);
    return ok || es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_INSERT_NATIVE_JSON_MEMBER);
}

void es_json_free(EsJson *j) {
    if (j) {
        yyjson_doc_free(j->doc);
        free(j->input);
        free(j->source);
        free(j->spans);
        free(j);
    }
}

bool es_json_rename(EsJson *j, yyjson_val *object, const char *member, const char *name,
                    EsEdits *edits, char *error, size_t cap) {
    if (!j || !yyjson_is_obj(object) || !member || !name || yyjson_obj_get(object, name)) {
        return es_fail(error, cap,
                       DM_TEXT_REPOSITORY_INVALID_OR_COLLIDING_NATIVE_JSON_MEMBER_RENAME);
    }

    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, i, count, key, value) {
        if (strcmp(yyjson_get_str(key), member)) {
            continue;
        }

        for (size_t s = 0; s < j->count; s++) {
            if (j->spans[s].value == key) {
                char *quoted = es_json_quote(name);
                bool ok = quoted && es_edits_add(edits, j->spans[s].begin, j->spans[s].end, quoted,
                                                 strlen(quoted), error, cap);
                free(quoted);
                return ok;
            }
        }
    }

    return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_LOCATE_NATIVE_JSON_MEMBER_TO_RENAME);
}
