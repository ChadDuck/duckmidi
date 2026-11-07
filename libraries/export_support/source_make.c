#include "source.h"
#include "../../src/text/en.h"
#include "internal.h"
#include <ctype.h>
#include <string.h>

bool es_make_value(const EsFile *file, const char *key, char out[64], bool *found, char *error,
                   size_t cap) {
    if (!file->exists) {
        return true;
    }

    unsigned depth = 0;
    const char *p = (const char *)file->data, *limit = p + file->size;
    size_t key_length = strlen(key);

    while (p < limit) {
        const char *end = memchr(p, '\n', (size_t)(limit - p));
        end = end ? end : limit;

        if (*p == '\t') {
            p = end < limit ? end + 1 : limit;
            continue;
        }

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        if ((size_t)(end - p) >= 5 && !memcmp(p, "endif", 5)) {
            depth -= depth != 0;
        } else if ((size_t)(end - p) >= 4 &&
                   (!memcmp(p, "ifeq", 4) || !memcmp(p, "ifne", 4) || !memcmp(p, "ifde", 4))) {
            depth++;
        } else if ((size_t)(end - p) >= 6 && !memcmp(p, "ifndef", 6)) {
            depth++;
        } else if ((size_t)(end - p) > key_length && !memcmp(p, key, key_length) &&
                   (isspace((unsigned char)p[key_length]) || strchr("=:?+!", p[key_length]))) {
            p += key_length;

            while (p < end && (*p == ' ' || *p == '\t')) {
                p++;
            }

            bool conditional = p < end && *p == '?';

            if (p < end && strchr(":?", *p)) {
                p++;
            }

            if (depth || p == end || *p++ != '=') {
                return es_fail(
                    error, cap,
                    DM_TEXT_REPOSITORY_RESOLVE_THE_CONDITIONAL_OR_COMPUTED_S_BUILD_SETTING_TO, key);
            }

            while (p < end && (*p == ' ' || *p == '\t')) {
                p++;
            }

            const char *value = p;

            while (p < end && !isspace((unsigned char)*p) && *p != '#') {
                if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-",
                            *p)) {
                    return es_fail(error, cap,
                                   DM_TEXT_REPOSITORY_UNSUPPORTED_COMPUTED_S_BUILD_SETTING, key);
                }

                p++;
            }

            size_t length = (size_t)(p - value);

            while (p < end && isspace((unsigned char)*p)) {
                p++;
            }

            if (p < end && *p != '#') {
                return es_fail(error, cap, DM_TEXT_REPOSITORY_UNSUPPORTED_COMPUTED_S_BUILD_SETTING,
                               key);
            }

            if (!length || length >= 64) {
                return es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_S_BUILD_SETTING, key);
            }

            if (!conditional || !*found) {
                memcpy(out, value, length);
                out[length] = 0;
                *found = true;
            }
        }

        p = end < limit ? end + 1 : limit;
    }

    return true;
}
