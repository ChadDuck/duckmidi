#include "repository.h"
#include "../../../src/text/en.h"
#include "../../export_support/source.h"
#include "../../export_support/package.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *error, size_t cap, const char *why, const char *path) {
    snprintf(error, cap, "%s: %s", why, path ? path : "");
    return false;
}

static bool suffix(const char *s, const char *end) {
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !strcmp(s + a - b, end);
}

static int sequence_id(const char *name) {
    char *end;
    unsigned long value = strtoul(name, &end, 16);
    return end != name && end - name <= 2 && (!*end || *end == '_') && value <= 255 ? (int)value
                                                                                    : -1;
}

bool m64_repository_sequence(M64PluginJob *job, EsRepository *repo, char *error, size_t cap) {
    if (job->request.target == M64_TARGET_SM64COOPDX || sequence_id(job->sequence) >= 0) {
        return true;
    }

    EsFile source = {0};
    bool ok = es_repository_read(repo, M64_SEQUENCES_JSON, &source, error, cap);
    EsJson *json = ok ? es_json_parse(source.data, source.size, error, cap) : NULL;
    yyjson_val *root = json ? es_json_root(json) : NULL;
    ok = ok && json && yyjson_is_obj(root);
    bool used[256] = {0};
    int maximum = -1;
    const char *existing = NULL;
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, i, count, key, value) {
        const char *name = yyjson_get_str(key);

        if (!strcmp(name, "comment")) {
            continue;
        }

        int id = sequence_id(name);

        if (id < 0 || used[id]) {
            ok = fail(error, cap, DM_TEXT_M64_INVALID_OR_DUPLICATE_NATIVE_SEQUENCE_ID, name);
            break;
        }

        used[id] = true;
        maximum = id > maximum ? id : maximum;
        const char *stem = strchr(name, '_');

        if (stem && !strcmp(stem + 1, job->sequence)) {
            if (existing) {
                ok =
                    fail(error, cap, DM_TEXT_M64_AMBIGUOUS_NATIVE_SEQUENCE_FILENAME, job->sequence);
                break;
            }

            existing = name;
        }
    }

    char name[97];

    if (ok && existing) {
        ok = snprintf(name, sizeof(name), "%s", existing) < (int)sizeof(name);
    } else if (ok && maximum < 255) {
        ok =
            snprintf(name, sizeof(name), "%02X_%s", maximum + 1, job->sequence) < (int)sizeof(name);
    } else {
        ok = false;
    }

    if (ok) {
        snprintf(job->sequence, sizeof(job->sequence), "%s", name);
    } else if (!*error) {
        fail(error, cap, DM_TEXT_M64_NO_NATIVE_SEQUENCE_ID_IS_AVAILABLE_OR_THE_NAME, job->sequence);
    }

    es_json_free(json);
    es_file_free(&source);
    return ok;
}

static bool sequence_path(EsRepository *repo, EsPlan *plan, const char *sequence,
                          const char *old_name, const char *key, char out[512], char *error,
                          size_t cap) {
    EsPaths paths = {0};
    bool ok = es_repository_baseline_paths(repo, &paths, error, cap);
    const char *selected = NULL;
    const char *lookup = old_name ? old_name : sequence;

    for (size_t i = 0; ok && i < paths.count; i++) {
        const char *path = paths.items[i];

        if (strncmp(path, "sound/sequences/", 16) ||
            (!suffix(path, ".m64") && !suffix(path, ".s"))) {
            continue;
        }

        const char *stem = path + 16;

        if (!strncmp(stem, "us/", 3)) {
            stem += 3;
        } else if (strchr(stem, '/')) {
            continue;
        }

        const char *dot = strrchr(stem, '.');

        if ((size_t)(dot - stem) == strlen(lookup) && !strncmp(stem, lookup, strlen(lookup))) {
            if (selected) {
                ok = fail(error, cap, DM_TEXT_M64_DUPLICATE_NATIVE_SEQUENCE_SOURCES, lookup);
            } else {
                selected = path;
            }
        }
    }

    if (ok && selected) {
        const char *slash = strrchr(selected, '/');
        snprintf(out, 512, "%.*s/%s.m64", (int)(slash - selected), selected, sequence);

        if (strcmp(out, selected)) {
            ok = es_plan_put(plan, selected, NULL, 0, 0, false, error, cap) &&
                 es_plan_affect(plan, key, selected, "replaced sequence source");
        }
    } else if (ok) {
        snprintf(out, 512, "sound/sequences/us/%s.m64", sequence);
    }

    es_paths_free(&paths);
    return ok;
}

bool m64_repository_bank(M64PluginJob *job, EsRepository *repo, char *error, size_t cap) {
    if (job->request.target == M64_TARGET_SM64COOPDX) {
        return true;
    }

    EsPaths paths = {0};
    bool ok = es_repository_baseline_paths(repo, &paths, error, cap);
    const char *maximum = "";

    for (size_t i = 0; ok && i < paths.count; i++) {
        const char *path = paths.items[i];

        if (!strncmp(path, "sound/sound_banks/", 18) && suffix(path, ".json") &&
            strcmp(path + 18, maximum) > 0) {
            maximum = path + 18;
        }
    }

    /* Banks take IDs in filename order, so an added bank must sort after every initialized one.
       Keep the chosen name when it already does; otherwise put the last bank's name in front. */
    const char *requested = *job->requested_bank ? job->requested_bank : "duckmidi";
    char file[128];
    snprintf(file, sizeof(file), "%s.json", requested);
    size_t length = strlen(maximum);
    /* With history off the current files are the baseline, so an earlier export's own bank
       may already be the last one; it is reused. */
    int order = strcmp(file, maximum);
    int n = order > 0 || (!order && es_repository_untracked(repo))
                ? snprintf(job->bank, sizeof(job->bank), "%s", requested)
                : snprintf(job->bank, sizeof(job->bank), "%.*s_%s",
                           (int)(length > 5 ? length - 5 : 0), maximum, requested);
    ok = ok && n > 0 && n < (int)sizeof(job->bank);
    es_paths_free(&paths);
    return ok || fail(error, cap,
                      DM_TEXT_M64_CANNOT_ALLOCATE_AN_APPENDED_NATIVE_BANK_NAME_WITHOUT_CHANGING,
                      job->requested_bank);
}

static bool allowed(const char *path) {
    if (!strcmp(path, M64_DATA_C) || !strcmp(path, M64_DATA_H) || !strcmp(path, M64_BUFFERS_C) ||
        !strcmp(path, M64_HACKER_CONFIG) || !strcmp(path, M64_SEQUENCES_JSON) ||
        !strcmp(path, "tools/assemble_sound.py") || !strcmp(path, "Makefile")) {
        return true;
    }

    if (suffix(path, ".bkp")) {
        size_t n = strlen(path);
        char source[8192];

        if (n >= sizeof(source)) {
            return false;
        }

        memcpy(source, path, n - 4);
        source[n - 4] = 0;
        return allowed(source);
    }

    return (!strncmp(path, "sound/samples/", 14) && suffix(path, ".aiff")) ||
           (!strncmp(path, "sound/sound_banks/", 18) && suffix(path, ".json")) ||
           (!strncmp(path, "sound/sequences/", 16) &&
            (suffix(path, ".m64") || suffix(path, ".s"))) ||
           (!strncmp(path, "mods/duckmidi_", 14) &&
            (suffix(path, "/main.lua") || suffix(path, ".m64")));
}

static bool detect(EsRoot *root, DmeHistoryTarget *target, char *error, size_t cap) {
    char *version = target->version;
    EsFile coop = {0}, hacker = {0}, registration = {0};
    bool ok = es_fs_read(root, M64_COOPDX_MARKER, &coop, error, cap) &&
              es_fs_read(root, M64_HACKER_CONFIG, &hacker, error, cap) &&
              es_fs_read(root, M64_SEQUENCES_JSON, &registration, error, cap);

    if (ok && !coop.exists && !registration.exists) {
        ok = fail(error, cap, DM_TEXT_NATIVE_SELECT_M64, "sound/sequences.json");
    }

    const char *id = coop.exists ? "sm64coopdx" : hacker.exists ? "hackersm64" : "sm64";
    snprintf(target->id, sizeof(target->id), "%s", id);
    snprintf(target->name, sizeof(target->name), "%s",
             coop.exists     ? "sm64coopdx"
             : hacker.exists ? "HackerSM64"
                             : "SM64");
    /* sm64coopdx plays sequences with existing banks; custom samples are unavailable. */
    target->capabilities = coop.exists ? 0 : DME_REPOSITORY_CUSTOM;
    EsFile makefile = {0};
    bool found = false;
    ok = ok && es_fs_read(root, "Makefile", &makefile, error, cap) &&
         es_make_value(&makefile, "VERSION", version, &found, error, cap) && found;

    if (ok && strcmp(version, "us")) {
        ok = fail(error, cap, DM_TEXT_NATIVE_M64_US_REQUIRED, version);
    }

    es_file_free(&makefile);
    es_file_free(&coop);
    es_file_free(&hacker);
    es_file_free(&registration);
    return ok;
}

static bool inventory(EsRepository *repo, EsPaths *paths, char *error, size_t cap) {
    EsPaths all = {0};
    EsFile marker = {0};
    bool detected = es_repository_read(repo, M64_COOPDX_MARKER, &marker, error, cap);
    bool coopdx = marker.exists;
    es_file_free(&marker);

    if (!detected) {
        return false;
    }

    if (coopdx) {
        bool ok = es_repository_paths(repo, "mods", true, &all, error, cap);

        for (size_t i = 0; ok && i < all.count; i++) {
            if (allowed(all.items[i])) {
                ok = es_paths_add(paths, all.items[i]);
            }
        }

        es_paths_free(&all);
        return ok;
    }

    bool ok = es_repository_paths(repo, "sound", true, &all, error, cap);
    const char *fixed[] = {
        M64_DATA_C, M64_DATA_H, M64_BUFFERS_C, M64_HACKER_CONFIG, "tools/assemble_sound.py",
        "Makefile"};

    for (size_t i = 0; ok && i < all.count; i++) {
        if (allowed(all.items[i])) {
            ok = es_paths_add(paths, all.items[i]);
        }
    }

    for (size_t i = 0; ok && i < sizeof(fixed) / sizeof(*fixed); i++) {
        EsFile f = {0};
        ok = es_repository_read(repo, fixed[i], &f, error, cap);

        if (ok && f.exists) {
            ok = es_paths_add(paths, fixed[i]);
        }

        es_file_free(&f);
        char backup[256];
        snprintf(backup, sizeof(backup), "%s.bkp", fixed[i]);
        ok = ok && es_repository_read(repo, backup, &f, error, cap);

        if (ok && f.exists) {
            ok = es_paths_add(paths, backup);
        }

        es_file_free(&f);
    }

    es_paths_free(&all);
    all = (EsPaths){0};
    EsFile coop = {0};
    ok = ok && es_repository_read(repo, M64_COOPDX_MARKER, &coop, error, cap);

    if (ok && coop.exists) {
        ok = es_repository_paths(repo, "mods", true, &all, error, cap);

        for (size_t i = 0; ok && i < all.count; i++) {
            if (allowed(all.items[i])) {
                ok = es_paths_add(paths, all.items[i]);
            }
        }
    }

    es_paths_free(&all);
    es_file_free(&coop);
    return ok;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static double natural(const EsFile *file) {
    if (!file->exists || file->size < 12 || memcmp(file->data, "FORM", 4) ||
        memcmp(file->data + 8, "AIFF", 4)) {
        return 0;
    }

    for (size_t p = 12; p + 8 <= file->size;) {
        size_t n = be32(file->data + p + 4);

        if (n > file->size - p - 8) {
            return 0;
        }

        if (!memcmp(file->data + p, "COMM", 4) && n >= 18) {
            const uint8_t *v = file->data + p + 16;
            int exp = (v[0] << 8 | v[1]) - 16383;
            double mantissa = be32(v + 2) / 2147483648.0 + be32(v + 6) / 9223372036854775808.0;
            return ldexp(mantissa, exp) / 32000;
        }

        p += 8 + n + (n & 1);
    }

    return 0;
}

static bool condition(yyjson_val *value, bool *enabled, char *error, size_t cap) {
    *enabled = false;

    if (!yyjson_is_arr(value)) {
        return fail(error, cap, DM_TEXT_M64_UNSUPPORTED_NATIVE_IFDEF, "expected version list");
    }

    size_t i, max;
    yyjson_val *item;
    yyjson_arr_foreach(value, i, max, item) {
        const char *name = yyjson_get_str(item);

        if (!name || (strcmp(name, "VERSION_US") && strcmp(name, "VERSION_JP") &&
                      strcmp(name, "VERSION_EU") && strcmp(name, "VERSION_SH") &&
                      strcmp(name, "VERSION_CN"))) {
            return fail(error, cap, DM_TEXT_M64_UNSUPPORTED_NATIVE_BUILD_CONDITION, name);
        }

        *enabled |= !strcmp(name, "VERSION_US");
    }

    return true;
}

static yyjson_val *select_value(yyjson_val *value, char *error, size_t cap) {
    for (unsigned depth = 0; yyjson_is_obj(value) && yyjson_obj_get(value, "then"); depth++) {
        bool enabled;

        if (depth >= 32 || !condition(yyjson_obj_get(value, "ifdef"), &enabled, error, cap)) {
            return NULL;
        }

        value = yyjson_obj_get(value, enabled ? "then" : "else");
    }

    return value;
}

typedef struct {
    EsRepository *repo;
    const EsPlan *desired;
    EsTargets *targets;
    EsPaths *target_paths;
    EsPlan *changes;
    const EsModification *replacement;
    const char *bank_path, *sample_bank, *target;
    double old_tuning, new_tuning;
    int drum_pan; /* Optional replacement override; -1 preserves each initialized drum pan. */
    EsJson *json;
    EsEdits edits;
    const char *aliases;
} Scan;

static bool current(EsRepository *, const EsPlan *, const char *, EsFile *, char *, size_t);

/* Whether path exists in the desired state, without reading its content. */
static bool present(EsRepository *repo, const EsPlan *plan, const char *path, bool *exists,
                    char *error, size_t cap) {
    const EsFile *file = plan ? es_plan_find(plan, path) : NULL;

    if (file) {
        *exists = file->exists;
        return true;
    }

    return es_repository_baseline_exists(repo, path, exists, error, cap);
}

static bool resolve_sample(Scan *scan, const char *sample, char path[512], bool *exists,
                           char *error, size_t cap) {
    snprintf(path, 512, "sound/samples/%s/%s.aiff", scan->sample_bank, sample);

    if (!present(scan->repo, scan->desired, path, exists, error, cap)) {
        return false;
    }

    if (*exists) {
        return true;
    }

    if (strcmp(scan->sample_bank, "instruments") || !scan->aliases) {
        return true;
    }

    const char *p = strstr(scan->aliases, "VALID_INSTRUMENT_NAMES = [");

    if (!p) {
        return true;
    }

    p += strlen("VALID_INSTRUMENT_NAMES = [");

    while (*p && *p != ']') {
        if (*p == '#') {
            const char *newline = strchr(p, '\n');
            p = newline ? newline + 1 : p + strlen(p);
            continue;
        }

        if (*p++ != '[') {
            continue;
        }

        char names[8][128];
        unsigned count = 0;
        bool matches = false;

        while (*p && *p != ']') {
            if (*p != '"' && *p != '\'') {
                p++;
                continue;
            }

            char quote = *p++;
            const char *end = strchr(p, quote);

            if (!end || count == 8 || end - p >= (ptrdiff_t)sizeof(names[0]) ||
                memchr(p, '\\', (size_t)(end - p))) {
                return fail(error, cap, DM_TEXT_M64_UNSUPPORTED_NATIVE_SAMPLE_ALIAS_TABLE,
                            "tools/assemble_sound.py");
            }

            memcpy(names[count], p, (size_t)(end - p));
            names[count][end - p] = 0;
            matches |= !strcmp(names[count], sample);
            count++;
            p = end + 1;
        }

        if (matches) {
            for (unsigned i = 0; i < count; i++) {
                snprintf(path, 512, "sound/samples/instruments/%s.aiff", names[i]);

                if (!present(scan->repo, scan->desired, path, exists, error, cap)) {
                    return false;
                }

                if (*exists) {
                    return true;
                }
            }

            return true;
        }

        p += *p == ']';
    }

    return true;
}

static bool current(EsRepository *repo, const EsPlan *plan, const char *path, EsFile *out,
                    char *error, size_t cap) {
    const EsFile *file = plan ? es_plan_find(plan, path) : NULL;

    if (!file) {
        return es_repository_baseline(repo, path, out, error, cap);
    }

    out->data = malloc(file->size + 1);

    if (!out->data) {
        return false;
    }

    memcpy(out->data, file->data, file->size);
    out->data[file->size] = 0;
    out->size = file->size;
    out->mode = file->mode;
    out->exists = file->exists;
    return true;
}

static bool sound(Scan *scan, yyjson_val *parent, const char *key, const char *label,
                  bool *replaced, char *error, size_t cap) {
    yyjson_val *value = select_value(yyjson_obj_get(parent, key), error, cap);

    if (!value || yyjson_is_null(value)) {
        return !*error;
    }

    const char *sample = yyjson_get_str(value);

    if (!sample) {
        sample = yyjson_get_str(yyjson_obj_get(value, "sample"));
    }

    if (!sample || !*sample || strchr(sample, '/') || strchr(sample, '\\') || strlen(sample) > 96) {
        return fail(error, cap, DM_TEXT_M64_UNSUPPORTED_NATIVE_SOUND_REFERENCE, scan->bank_path);
    }

    char path[512];
    bool exists = false;
    bool ok = resolve_sample(scan, sample, path, &exists, error, cap);

    if (ok && !exists) {
        ok = fail(error, cap, DM_TEXT_M64_UNRESOLVED_NATIVE_SAMPLE, path);
    }

    if (ok && scan->targets && !es_paths_has(scan->target_paths, path)) {
        char id[8704], text[1024];
        snprintf(id, sizeof(id), "%s|%s", es_repository_identity(scan->repo), path);
        snprintf(text, sizeof(text), "%s / %s / %s — %s", label, key, sample, path);
        ok =
            es_targets_add(scan->targets, id, text, path) && es_paths_add(scan->target_paths, path);
    }

    if (ok && scan->changes) {
        ok = es_plan_depend(scan->changes, scan->bank_path, path, key);
    }

    if (ok && scan->target && !strcmp(scan->target, path)) {
        *replaced = true;
        yyjson_val *tuning = yyjson_is_obj(value) ? yyjson_obj_get(value, "tuning") : NULL;
        double original = tuning ? yyjson_get_num(tuning) : scan->old_tuning;

        if (!isfinite(original) || original <= 0) {
            ok = fail(error, cap, DM_TEXT_M64_INVALID_NATIVE_TUNING, scan->bank_path);
        } else {
            char number[96];
            snprintf(number, sizeof(number), "%.17g",
                     original / scan->old_tuning * scan->new_tuning);

            if (yyjson_is_str(value)) {
                char *quoted = es_json_quote(sample);
                char object[512];
                snprintf(object, sizeof(object), "{\"sample\":%s,\"tuning\":%s}",
                         quoted ? quoted : "null", number);
                ok = quoted &&
                     es_json_set(scan->json, parent, key, object, &scan->edits, error, cap);
                free(quoted);
            } else {
                ok = es_json_set(scan->json, value, "tuning", number, &scan->edits, error, cap);
            }

            ok =
                ok && es_plan_affect(scan->changes, scan->replacement->key, scan->bank_path, label);
        }
    }

    return ok;
}

static bool instrument(Scan *scan, yyjson_val *value, const char *label, unsigned depth,
                       char *error, size_t cap) {
    if (depth > 32) {
        return fail(error, cap, DM_TEXT_M64_NATIVE_INSTRUMENT_NESTING_LIMIT, scan->bank_path);
    }

    if (yyjson_is_null(value)) {
        return true;
    }

    if (yyjson_is_arr(value)) {
        size_t i, max;
        yyjson_val *item;
        yyjson_arr_foreach(value, i, max, item) {
            if (!instrument(scan, item, label, depth + 1, error, cap)) {
                return false;
            }
        }

        return true;
    }

    if (!yyjson_is_obj(value)) {
        return fail(error, cap, DM_TEXT_M64_INVALID_NATIVE_INSTRUMENT, scan->bank_path);
    }

    yyjson_val *ifdef = yyjson_obj_get(value, "ifdef");
    bool enabled;

    if (ifdef && (!condition(ifdef, &enabled, error, cap) || !enabled)) {
        return !*error;
    }

    bool replaced = false;
    bool ok = sound(scan, value, "sound_lo", label, &replaced, error, cap) &&
              sound(scan, value, "sound", label, &replaced, error, cap) &&
              sound(scan, value, "sound_hi", label, &replaced, error, cap);

    if (ok && replaced && scan->drum_pan >= 0 && !strcmp(label, "percussion")) {
        char pan[16];
        snprintf(pan, sizeof(pan), "%d", scan->drum_pan);
        ok = es_json_set(scan->json, value, "pan", pan, &scan->edits, error, cap);
    }

    return ok;
}

static bool scan_banks(EsRepository *repo, EsPlan *plan, EsTargets *targets,
                       const EsModification *replacement, char *error, size_t cap) {
    EsPaths paths = {0}, target_paths = {0};
    bool ok = es_repository_baseline_paths(repo, &paths, error, cap);
    EsFile aliases = {0};
    ok = ok && es_repository_baseline(repo, "tools/assemble_sound.py", &aliases, error, cap);

    for (size_t i = 0; ok && plan && i < plan->count; i++) {
        ok = es_paths_add(&paths, plan->writes[i].path);
    }

    const char *target =
        replacement ? es_recipe_text(replacement->data, replacement->size, "path") : NULL;
    const char *tuning =
        replacement ? es_recipe_text(replacement->data, replacement->size, "tuning") : NULL;
    double old_tuning = 0;
    int drum_pan = -1;

    if (replacement) {
        EsFile baseline = {0};
        ok = ok && target && tuning && es_repository_baseline(repo, target, &baseline, error, cap);
        old_tuning = natural(&baseline);
        es_file_free(&baseline);
        ok = ok && old_tuning > 0;
        const char *value = es_recipe_text(replacement->data, replacement->size, "drum-pan");

        if (value) {
            char *end;
            long number = strtol(value, &end, 10);
            ok = ok && *value && !*end && number >= 0 && number <= 127;
            drum_pan = (int)number;

            if (!ok && !*error) {
                fail(error, cap, DM_TEXT_NATIVE_INVALID_DRUM_PAN, target);
            }
        }
    }

    for (size_t i = 0; ok && i < paths.count; i++) {
        const char *path = paths.items[i];

        if (strncmp(path, "sound/sound_banks/", 18) || !suffix(path, ".json")) {
            continue;
        }

        EsFile source = {0};
        ok = current(repo, plan, path, &source, error, cap);

        if (!ok || !source.exists) {
            es_file_free(&source);
            continue;
        }

        Scan scan = {.repo = repo,
                     .desired = plan,
                     .changes = plan,
                     .targets = targets,
                     .target_paths = &target_paths,
                     .replacement = replacement,
                     .bank_path = path,
                     .target = target,
                     .old_tuning = old_tuning,
                     .new_tuning = tuning ? strtod(tuning, NULL) : 0,
                     .drum_pan = drum_pan,
                     .aliases = (const char *)aliases.data};
        scan.json = es_json_parse(source.data, source.size, error, cap);
        yyjson_val *root = scan.json ? es_json_root(scan.json) : NULL;
        yyjson_val *bank = select_value(yyjson_obj_get(root, "sample_bank"), error, cap);
        scan.sample_bank = yyjson_get_str(bank);
        yyjson_val *instruments = yyjson_obj_get(root, "instruments");
        ok = scan.sample_bank && !strchr(scan.sample_bank, '/') &&
             !strchr(scan.sample_bank, '\\') && yyjson_is_obj(instruments);

        if (!ok && !*error) {
            fail(error, cap, DM_TEXT_M64_UNSUPPORTED_NATIVE_SOUND_BANK, path);
        }

        size_t j, max;
        yyjson_val *key, *value;
        yyjson_obj_foreach(instruments, j, max, key, value) {
            if (ok) {
                ok = instrument(&scan, value, yyjson_get_str(key), 0, error, cap);
            }
        }

        if (ok && scan.edits.count) {
            EsFile edited = {0};
            ok = es_edits_apply(&scan.edits, source.data, source.size, &edited, error, cap) &&
                 es_plan_put(plan, path, edited.data, edited.size, source.mode, true, error, cap);
            es_file_free(&edited);
        }

        es_edits_free(&scan.edits);
        es_json_free(scan.json);
        es_file_free(&source);
    }

    es_paths_free(&paths);
    es_file_free(&aliases);
    es_paths_free(&target_paths);
    return ok;
}

bool m64_repository_targets(EsRepository *repo, EsTargets *targets, char *error, size_t cap) {
    EsFile coop = {0};
    bool ok = es_repository_read(repo, M64_COOPDX_MARKER, &coop, error, cap);

    if (ok && !coop.exists) {
        ok = scan_banks(repo, NULL, targets, NULL, error, cap);
    }

    es_file_free(&coop);
    return ok;
}

bool m64_repository_sample_targets(M64PluginJob *j, EsRepository *repo, const DmeContext *context,
                                   const DmeHost *host, char *error, size_t cap) {
    EsTargets targets = {0};
    EsPaths paths = {0};
    bool ok = m64_repository_targets(repo, &targets, error, cap) &&
              es_repository_baseline_paths(repo, &paths, error, cap);
    j->sample_bank[0] = 0;
    /* Added samples sort after every initialized instrument sample, keeping stock IDs. */
    const char *maximum = "";

    for (size_t p = 0; ok && p < paths.count; p++) {
        const char *path = paths.items[p];

        if (!strncmp(path, "sound/samples/instruments/", 26) && suffix(path, ".aiff") &&
            strcmp(path + 26, maximum) > 0) {
            maximum = path + 26;
        }
    }

    /* First decide which Replace regions can apply. A Replace that cannot is added as a new
       sample instead, so the song still plays it. */
    const char *target_paths[ES_NATIVE_REGIONS] = {0};
    char banks[ES_NATIVE_REGIONS][128] = {{0}};
    double calibrations[ES_NATIVE_REGIONS] = {0};

    for (size_t i = 0; ok && i < j->samples.count; i++) {
        EsNativeRegion *region = &j->samples.regions[i];

        if (!region->target) {
            continue;
        }

        const char *path = NULL;

        for (size_t t = 0; t < targets.count; t++) {
            if (!strcmp(region->target, targets.items[t].id.data) ||
                !strcmp(region->target, targets.items[t].path.data)) {
                path = targets.items[t].path.data;
                break;
            }
        }

        const char *bank = path ? path + strlen("sound/samples/") : NULL;
        const char *end = bank ? strchr(bank, '/') : NULL;
        size_t bank_length = end ? (size_t)(end - bank) : 0;
        const char *stem = path ? strrchr(path, '/') : NULL;

        if (!path || !bank_length || bank_length >= sizeof(banks[i]) || !stem ||
            strlen(stem + 1) <= 5 || strlen(stem + 1) - 5 >= sizeof(region->name)) {
            es_native_add_instead(&j->samples, region);
            continue;
        }

        /* The generated bank uses the initialized physical sample's calibration. The global
           replacement pass applies the new calibration once to every consumer. */
        EsFile original = {0};
        ok = es_repository_baseline(repo, path, &original, error, cap);
        double calibration = ok ? natural(&original) : 0;
        es_file_free(&original);

        if (ok && (calibration <= 0 || !es_native_resample_fits(region, calibration))) {
            es_native_add_instead(&j->samples, region);
            continue;
        }

        target_paths[i] = path;
        calibrations[i] = calibration;
        memcpy(banks[i], bank, bank_length);
    }

    /* One generated bank reads one sample bank. Replaces outside the default bank apply only
       when every played note is a Replace from that same bank; otherwise they become Adds. */
    const char *other = NULL;
    bool single = true;

    for (size_t i = 0; ok && i < j->samples.count; i++) {
        if (j->samples.regions[i].target && strcmp(banks[i], "instruments")) {
            single &= !other || !strcmp(other, banks[i]);
            other = other ? other : banks[i];
        }
    }

    if (ok && other) {
        for (size_t i = 0; single && i < j->samples.count; i++) {
            single = j->samples.regions[i].target && !strcmp(banks[i], other);
        }

        for (uint64_t i = 0; single && i < context->event_count; i++) {
            const DmeEvent *e = &context->events[i];

            if ((e->status & MIDI_STATUS_MASK) == MIDI_NOTE_ON && e->b) {
                single = j->samples.notes && j->samples.notes[i].custom;
            }
        }

        for (size_t i = 0; !single && i < j->samples.count; i++) {
            if (j->samples.regions[i].target && strcmp(banks[i], "instruments")) {
                es_native_add_instead(&j->samples, &j->samples.regions[i]);
            }
        }
    }

    for (size_t i = 0; ok && i < j->samples.count; i++) {
        EsNativeRegion *region = &j->samples.regions[i];

        if (!region->target) {
            if (strcmp(region->name, maximum) <= 0) {
                char appended[128];
                size_t length = strlen(maximum);
                int n = snprintf(appended, sizeof(appended), "%.*s_%s",
                                 (int)(length > 5 ? length - 5 : 0), maximum, region->name);
                ok = n > 0 && n < (int)sizeof(appended);

                if (ok) {
                    strcpy(region->name, appended);
                }
            }

            continue;
        }

        const char *path = target_paths[i];
        ok = es_native_resample(region, calibrations[i], ES_NATIVE_SM64, host, error, cap) ==
             ES_RESAMPLE_OK;

        if (ok) {
            free(region->target);
            region->target = malloc(strlen(path) + 1);
            ok = region->target != NULL;
        }

        if (ok) {
            strcpy(region->target, path);
            const char *stem = strrchr(path, '/') + 1;
            size_t length = strlen(stem) - 5;
            memcpy(region->name, stem, length);
            region->name[length] = 0;
            snprintf(j->sample_bank, sizeof(j->sample_bank), "%s", banks[i]);
            j->custom_definitions[i].sample = region->name;
            j->custom_definitions[i].tuning = calibrations[i];
            region->reference_tuning = calibrations[i];
        }
    }

    es_targets_free(&targets);
    es_paths_free(&paths);
    return ok;
}

static bool validate_registration(EsRepository *repo, EsPlan *plan, char *error, size_t cap) {
    EsPaths paths = {0}, banks = {0};
    EsFile source = {0};
    bool ok = es_repository_baseline_paths(repo, &paths, error, cap) &&
              current(repo, plan, M64_SEQUENCES_JSON, &source, error, cap);

    for (size_t i = 0; ok && i < plan->count; i++) {
        ok = es_paths_add(&paths, plan->writes[i].path);
    }

    for (size_t i = 0; ok && i < paths.count; i++) {
        if (!strncmp(paths.items[i], "sound/sound_banks/", 18) && suffix(paths.items[i], ".json")) {
            bool exists = false;
            ok = present(repo, plan, paths.items[i], &exists, error, cap) &&
                 (!exists || es_paths_add(&banks, paths.items[i]));
        }
    }

    EsFile config = {0};
    ok = ok && es_repository_baseline(repo, M64_HACKER_CONFIG, &config, error, cap);
    unsigned bank_capacity =
        config.exists && m64_hacker_expanded(config.data, config.size) ? 256 : 64;
    es_file_free(&config);

    if (banks.count > bank_capacity) {
        ok = fail(error, cap, DM_TEXT_M64_THE_NATIVE_SOUND_BANK_ID_CAPACITY_IS_EXHAUSTED,
                  "sound/sound_banks");
    }

    EsJson *json = ok ? es_json_parse(source.data, source.size, error, cap) : NULL;
    yyjson_val *root = json ? es_json_root(json) : NULL;
    bool ids[256] = {0};
    int maximum = -1;
    int last_sequence = -1;
    unsigned bank_counts[256] = {0};
    ok = ok && json && yyjson_is_obj(root);
    size_t at, count;
    yyjson_val *key, *entry;
    yyjson_obj_foreach(root, at, count, key, entry) {
        const char *name = yyjson_get_str(key);

        if (!strcmp(name, "comment")) {
            continue;
        }

        int id = sequence_id(name);

        if (!ok || id < 0 || ids[id]) {
            ok = fail(error, cap, DM_TEXT_M64_INVALID_OR_DUPLICATE_NATIVE_SEQUENCE_ID, name);
            break;
        }

        ids[id] = true;
        maximum = id > maximum ? id : maximum;
        yyjson_val *value = select_value(entry, error, cap);

        if (yyjson_is_obj(value)) {
            value = select_value(yyjson_obj_get(value, "banks"), error, cap);
        }

        if (yyjson_is_null(value)) {
            continue;
        }

        if (!yyjson_is_arr(value) || yyjson_arr_size(value) > 255) {
            ok = fail(error, cap, DM_TEXT_M64_UNSUPPORTED_NATIVE_SEQUENCE_BANK_REGISTRATION, name);
            break;
        }

        last_sequence = id > last_sequence ? id : last_sequence;
        bank_counts[id] = (unsigned)yyjson_arr_size(value);

        unsigned sources = 0;

        for (size_t p = 0; ok && p < paths.count; p++) {
            const char *path = paths.items[p], *leaf = strrchr(path, '/');

            if (!leaf || strncmp(path, "sound/sequences/", 16) ||
                (leaf != path + 15 && strncmp(path, "sound/sequences/us/", 19)) ||
                (!suffix(path, ".m64") && !suffix(path, ".s"))) {
                continue;
            }

            size_t length = strlen(leaf + 1) - (suffix(path, ".m64") ? 4 : 2);

            if (strlen(name) == length && !strncmp(leaf + 1, name, length)) {
                bool exists = false;
                ok = present(repo, plan, path, &exists, error, cap);
                sources += exists;

                if (ok && exists) {
                    ok = es_plan_depend(plan, M64_SEQUENCES_JSON, path, "sequence");
                }
            }
        }

        if (sources != 1) {
            ok = fail(error, cap, DM_TEXT_M64_MISSING_OR_AMBIGUOUS_NATIVE_SEQUENCE_SOURCE, name);
            break;
        }

        size_t b, size;
        yyjson_val *bank;
        yyjson_arr_foreach(value, b, size, bank) {
            const char *stem = yyjson_get_str(bank);
            char path[8192];
            bool found = false;

            if (stem && !strchr(stem, '/')) {
                snprintf(path, sizeof(path), "sound/sound_banks/%s.json", stem);

                found = es_paths_has(&banks, path);
            }

            if (!found) {
                ok = fail(error, cap, DM_TEXT_M64_UNRESOLVED_NATIVE_SEQUENCE_BANK, stem);
                break;
            }

            ok = ok && es_plan_depend(plan, M64_SEQUENCES_JSON, path, "bank");
        }
    }

    for (int id = 0; ok && id <= maximum; id++) {
        if (!ids[id]) {
            ok =
                fail(error, cap,
                     DM_TEXT_M64_NATIVE_SEQUENCE_REGISTRATIONS_MUST_HAVE_CONTIGUOUS_HEXADECIMAL_IDS,
                     M64_SEQUENCES_JSON);
        }
    }

    unsigned registration_bytes = 0;

    for (int id = 0; id <= last_sequence; id++) {
        registration_bytes += 3 + bank_counts[id];
    }

    registration_bytes = (registration_bytes + 15u) & ~15u;

    if (ok && registration_bytes > bank_capacity * 4u) {
        ok = fail(error, cap,
                  DM_TEXT_M64_THE_NATIVE_SEQUENCE_BANK_REGISTRATION_BUFFER_IS_FULL_REMOVE,
                  M64_SEQUENCES_JSON);
    }

    for (size_t i = 0; ok && i < paths.count; i++) {
        const char *path = paths.items[i], *leaf = strrchr(path, '/');

        if (!leaf || strncmp(path, "sound/sequences/", 16) ||
            (leaf != path + 15 && strncmp(path, "sound/sequences/us/", 19)) ||
            (!suffix(path, ".m64") && !suffix(path, ".s"))) {
            continue;
        }

        bool exists = false;
        ok = present(repo, plan, path, &exists, error, cap);
        size_t length = strlen(leaf + 1) - (suffix(path, ".m64") ? 4 : 2);
        char name[512];

        if (exists && length < sizeof(name)) {
            memcpy(name, leaf + 1, length);
            name[length] = 0;

            if (!yyjson_obj_get(root, name)) {
                ok = fail(error, cap, DM_TEXT_M64_NATIVE_SEQUENCE_SOURCE_HAS_NO_REGISTRATION, path);
            }
        }
    }

    es_json_free(json);
    es_file_free(&source);
    es_paths_free(&paths);
    es_paths_free(&banks);
    return ok;
}

static bool render(EsRepository *repo, const EsModifications *mods, EsPlan *plan, char *error,
                   size_t cap) {
    EsFile registration = {0};
    EsFile marker = {0};
    bool ok = es_repository_read(repo, M64_COOPDX_MARKER, &marker, error, cap);
    bool coopdx = marker.exists;
    es_file_free(&marker);
    ok = ok &&
         (coopdx || es_repository_baseline(repo, M64_SEQUENCES_JSON, &registration, error, cap));
    EsJson *json = ok && registration.exists
                       ? es_json_parse(registration.data, registration.size, error, cap)
                       : NULL;
    EsEdits entries = {0};
    uint64_t memory = 0;
    bool hacker = false, has_songs = false;
    const char *slot_names[256] = {0};
    yyjson_val *slot_values[256] = {0};
    bool active_ids[256] = {0};
    unsigned maximum = 0;
    yyjson_val *registration_root = json ? es_json_root(json) : NULL;
    size_t entry, entry_count;
    yyjson_val *entry_key, *entry_value;
    yyjson_obj_foreach(registration_root, entry, entry_count, entry_key, entry_value) {
        const char *name = yyjson_get_str(entry_key);

        if (!strcmp(name, "comment")) {
            continue;
        }

        int id = sequence_id(name);

        if (id < 0 || slot_names[id]) {
            ok = fail(error, cap, DM_TEXT_M64_INVALID_OR_DUPLICATE_NATIVE_SEQUENCE_ID, name);
            break;
        }

        slot_names[id] = name;
        slot_values[id] = entry_value;
        maximum = (unsigned)id > maximum ? (unsigned)id : maximum;
    }

    for (size_t i = 0; ok && i < mods->count; i++) {
        const EsModification *m = &mods->items[i];
        const char *kind = es_recipe_text(m->data, m->size, "kind");

        if (!kind) {
            ok = fail(error, cap, DM_TEXT_M64_UNSUPPORTED_M64_HISTORY_RECIPE, m->key);
            break;
        }

        if (!strcmp(kind, "m64-sample-1")) {
            const char *path = es_recipe_text(m->data, m->size, "path");
            size_t size;
            const uint8_t *bytes = es_recipe_get(m->data, m->size, "audio", &size);
            ok = path && bytes && allowed(path) &&
                 es_plan_put(plan, path, bytes, size, 0, true, error, cap) &&
                 es_plan_backup(plan, path) && es_plan_affect(plan, m->key, path, "sample");
            continue;
        }

        if (strcmp(kind, "m64-song-1")) {
            ok = fail(error, cap, DM_TEXT_M64_UNSUPPORTED_M64_HISTORY_RECIPE_VERSION, kind);
            break;
        }

        const char *sequence = es_recipe_text(m->data, m->size, "sequence");
        const char *bank = es_recipe_text(m->data, m->size, "bank");
        const char *target = es_recipe_text(m->data, m->size, "target");
        const char *required = es_recipe_text(m->data, m->size, "memory");
        const char *stock = es_recipe_text(m->data, m->size, "stock");
        size_t seq_size = 0, bank_size = 0;
        const uint8_t *seq = es_recipe_get(m->data, m->size, "sequence-bytes", &seq_size);
        const uint8_t *definition = es_recipe_get(m->data, m->size, "bank-bytes", &bank_size);
        ok = sequence && bank && target && required && stock && seq;

        if (!ok) {
            break;
        }

        char path[512];

        if (!strcmp(target, "sm64coopdx")) {
            char folder[128];
            m64_coopdx_folder(sequence, folder, sizeof(folder));
            snprintf(path, sizeof(path), "mods/%s/sound/%s.m64", folder, sequence);
            ok = es_plan_put(plan, path, seq, seq_size, 0, true, error, cap) &&
                 es_plan_affect(plan, m->key, path, "sequence");
            char *lua = m64_coopdx_main_lua(sequence);
            snprintf(path, sizeof(path), "mods/%s/main.lua", folder);
            ok = ok && lua && es_plan_put(plan, path, lua, strlen(lua), 0, true, error, cap) &&
                 es_plan_affect(plan, m->key, path, "registration");
            free(lua);
            continue;
        }

        has_songs = true;
        int id = sequence_id(sequence);

        if (id < 0 || active_ids[id]) {
            ok = fail(error, cap, DM_TEXT_M64_ACTIVE_EXPORTS_COLLIDE_AT_A_NATIVE_SEQUENCE_ID,
                      sequence);
            break;
        }

        active_ids[id] = true;
        maximum = (unsigned)id > maximum ? (unsigned)id : maximum;
        hacker = !strcmp(target, "hackersm64");
        uint64_t need = !strcmp(stock, "0") ? strtoull(required, NULL, 10) : 0;
        memory = need > memory ? need : memory;
        ok = sequence_path(repo, plan, sequence, slot_names[id], m->key, path, error, cap) &&
             es_plan_put(plan, path, seq, seq_size, 0, true, error, cap) &&
             es_plan_affect(plan, m->key, path, "sequence");
        char bank_path[256];
        snprintf(bank_path, sizeof(bank_path), "sound/sound_banks/%s.json", bank);
        const EsFile *prior = es_plan_find(plan, bank_path);

        if (prior && (prior->size != bank_size || memcmp(prior->data, definition, bank_size))) {
            ok = fail(error, cap, DM_TEXT_NATIVE_M64_SHARED_BANK, bank_path);
        }

        ok = ok && definition &&
             es_plan_put(plan, bank_path, definition, bank_size, 0, true, error, cap) &&
             es_plan_affect(plan, m->key, bank_path, "bank") &&
             es_plan_depend(plan, path, bank_path, "bank") &&
             es_plan_depend(plan, M64_SEQUENCES_JSON, bank_path, "registration");
        yyjson_val *root = registration_root;
        yyjson_val *existing = slot_values[id];
        const char *member = slot_names[id] ? slot_names[id] : sequence;

        if (slot_names[id] && strcmp(slot_names[id], sequence)) {
            ok = ok && es_json_rename(json, root, slot_names[id], sequence, &entries, error, cap);
        }

        char *quoted = es_json_quote(bank);
        char list[512];
        snprintf(list, sizeof(list), "[%s]", quoted ? quoted : "null");
        ok = ok && json && quoted &&
             es_json_set(json, yyjson_is_obj(existing) ? existing : root,
                         yyjson_is_obj(existing) ? "banks" : member, list, &entries, error, cap) &&
             es_plan_affect(plan, m->key, M64_SEQUENCES_JSON, sequence);
        free(quoted);

        for (unsigned asset = 0; ok; asset++) {
            char field[64];
            snprintf(field, sizeof(field), "asset%u.path", asset);
            const char *asset_path = es_recipe_text(m->data, m->size, field);

            if (!asset_path) {
                break;
            }

            snprintf(field, sizeof(field), "asset%u.bytes", asset);
            size_t bytes = 0;
            const uint8_t *data = es_recipe_get(m->data, m->size, field, &bytes);
            const EsFile *other = es_plan_find(plan, asset_path);

            if (other && (other->size != bytes || memcmp(other->data, data, bytes))) {
                ok = fail(error, cap,
                          DM_TEXT_M64_ACTIVE_SONGS_REQUEST_DIFFERENT_REVISIONS_OF_A_SHARED_CUSTOM,
                          asset_path);
            }

            ok = ok && data && allowed(asset_path) &&
                 es_plan_put(plan, asset_path, data, bytes, 0, true, error, cap) &&
                 es_plan_affect(plan, m->key, asset_path, "added sample");
        }
    }

    if (ok && has_songs) {
        for (unsigned id = 0; ok && id <= maximum; id++) {
            if (!slot_names[id] && !active_ids[id]) {
                char name[64];
                snprintf(name, sizeof(name), "%02X_duckmidi_unused", id);
                ok = es_json_set(json, registration_root, name, "null", &entries, error, cap);
            }
        }

        EsFile edited = {0};
        ok = es_edits_apply(&entries, registration.data, registration.size, &edited, error, cap) &&
             es_plan_put(plan, M64_SEQUENCES_JSON, edited.data, edited.size, registration.mode,
                         true, error, cap);
        es_file_free(&edited);
    }

    es_edits_free(&entries);
    es_json_free(json);
    es_file_free(&registration);

    if (ok && memory) {
        M64MemoryPlan memory_plan = m64_memory_plan(memory);
        const char *paths[] = {M64_DATA_C, M64_DATA_H, M64_BUFFERS_C};

        for (unsigned i = 0; ok && i < 3; i++) {
            if ((hacker && i != 1) || (!hacker && !memory_plan.needed)) {
                continue;
            }

            EsFile source = {0};
            M64Text edited = {0};
            ok = es_repository_baseline(repo, paths[i], &source, error, cap);
            bool changed = true;

            if (ok && hacker) {
                EsFile config = {0};
                ok = es_repository_baseline(repo, M64_HACKER_CONFIG, &config, error, cap) &&
                     m64_patch_hacker_data_h(source.data, source.size, memory,
                                             m64_hacker_expanded(config.data, config.size), &edited,
                                             &changed, error, cap);
                es_file_free(&config);
            } else if (ok) {
                ok = i == 0   ? m64_patch_data_c(source.data, source.size, &memory_plan, &edited,
                                                 error, cap)
                     : i == 1 ? m64_patch_data_h(source.data, source.size, &memory_plan, &edited,
                                                 error, cap)
                              : m64_patch_buffers_c(source.data, source.size, &memory_plan, &edited,
                                                    error, cap);
            }

            if (ok && changed) {
                ok = es_plan_put(plan, paths[i], edited.data, edited.size, source.mode, true, error,
                                 cap);

                for (size_t m = 0; ok && m < mods->count; m++) {
                    const char *need =
                        es_recipe_text(mods->items[m].data, mods->items[m].size, "memory");

                    if (need && strtoull(need, NULL, 10) == memory) {
                        ok = es_plan_affect(plan, mods->items[m].key, paths[i], "sequence memory");
                    }
                }
            }

            m64_text_free(&edited);
            es_file_free(&source);
        }
    }

    for (size_t i = 0; ok && i < mods->count; i++) {
        const EsModification *m = &mods->items[i];
        const char *kind = es_recipe_text(m->data, m->size, "kind");

        if (kind && !strcmp(kind, "m64-sample-1")) {
            ok = scan_banks(repo, plan, NULL, m, error, cap);
        }
    }

    ok = ok && (!has_songs || (scan_banks(repo, plan, NULL, NULL, error, cap) &&
                               validate_registration(repo, plan, error, cap)));

    for (size_t i = 0; ok && i < mods->count; i++) {
        const char *keep = es_recipe_text(mods->items[i].data, mods->items[i].size, "keep");

        if (keep && !strcmp(keep, "1")) {
            ok = es_plan_keep_originals(repo, plan, mods->items[i].key, error, cap);
        }
    }

    return ok;
}

int32_t DME_CALL m64_history_detect(const DmeHistoryApi *api, DmeHistoryRoot *root,
                                    DmeHistoryTarget *target, char *error, uint64_t cap) {
    (void)api;
    return detect((EsRoot *)root, target, error, (size_t)cap);
}

int32_t DME_CALL m64_history_inventory(const DmeHistoryApi *api, DmeHistory *history,
                                       DmeHistoryList *list, char *error, uint64_t cap) {
    (void)api;
    return inventory((EsRepository *)history, (EsPaths *)list, error, (size_t)cap);
}

int32_t DME_CALL m64_history_allow(const char *path) {
    return allowed(path);
}

int32_t DME_CALL m64_history_render(const DmeHistoryApi *api, DmeHistory *history,
                                    const DmeHistoryIntent *intents, uint32_t count,
                                    DmeHistoryPlan *plan, char *error, uint64_t cap) {
    (void)api;
    EsModifications mods;
    bool ok = es_intents_view(intents, count, &mods) &&
              render((EsRepository *)history, &mods, (EsPlan *)plan, error, (size_t)cap);
    es_intents_release(&mods);
    return ok;
}

int32_t DME_CALL m64_history_targets(const DmeHistoryApi *api, DmeHistory *history,
                                     DmeHistoryTargets *targets, char *error, uint64_t cap) {
    (void)api;
    return m64_repository_targets((EsRepository *)history, (EsTargets *)targets, error,
                                  (size_t)cap);
}

static bool compose(M64PluginJob *j, EsRepository *repo, const M64Result *result,
                    const DmeHost *package_host, char *error, size_t cap) {
    EsRecipe recipes[ES_NATIVE_REGIONS + 1] = {0};
    EsModification mods[ES_NATIVE_REGIONS + 1] = {0};
    char keys[ES_NATIVE_REGIONS + 1][768], labels[ES_NATIVE_REGIONS + 1][256];
    const char *target = j->request.target == M64_TARGET_SM64COOPDX   ? "sm64coopdx"
                         : j->request.target == M64_TARGET_HACKERSM64 ? "hackersm64"
                                                                      : "sm64";
    int id = sequence_id(j->sequence);

    if (j->request.target == M64_TARGET_SM64COOPDX) {
        snprintf(keys[0], sizeof(keys[0]), "mod:%s", j->sequence);
    } else {
        snprintf(keys[0], sizeof(keys[0]), "song:%02X", id);
    }

    snprintf(labels[0], sizeof(labels[0]), DM_TEXT_M64_EXPORTED_S_SEQUENCE, j->sequence);
    char required[32];
    const M64Api *api = m64_get_api_3();
    M64ArtifactView sequence = {0}, bank = {0}, manifest = {0};
    bool ok = api->artifact_at(result, M64_SEQUENCE, &sequence) == M64_OK &&
              api->artifact_at(result, M64_BANK_JSON, &bank) == M64_OK &&
              api->artifact_at(result, M64_MANIFEST, &manifest) == M64_OK;
    char *text = calloc((size_t)manifest.size + 1, 1);

    if (text) {
        memcpy(text, manifest.data, (size_t)manifest.size);
    }

    const char *need = text ? strstr(text, "\"temporary_sequence_bytes\":") : NULL;
    uint64_t memory =
        need ? strtoull(strchr(need, ':') + 1, NULL, 10) : ((sequence.size + 15) & ~15ull) + 32;
    snprintf(required, sizeof(required), "%llu", (unsigned long long)memory);
    free(text);
    ok = ok && es_recipe_string(&recipes[0], "kind", "m64-song-1") &&
         es_recipe_string(&recipes[0], "sequence", j->sequence) &&
         es_recipe_string(&recipes[0], "bank", j->bank) &&
         es_recipe_string(&recipes[0], "target", target) &&
         es_recipe_string(&recipes[0], "memory", required) &&
         es_recipe_string(&recipes[0], "stock", j->memory_stock ? "1" : "0") &&
         es_recipe_string(&recipes[0], "keep", j->keep_originals ? "1" : "0") &&
         es_recipe_add(&recipes[0], "sequence-bytes", sequence.data, (size_t)sequence.size) &&
         es_recipe_add(&recipes[0], "bank-bytes", bank.data, (size_t)bank.size);
    size_t count = 1;
    unsigned asset = 0;

    for (size_t i = 0; ok && i < j->samples.count; i++) {
        const EsNativeRegion *r = &j->samples.regions[i];

        if (r->target) {
            size_t n = count++;
            snprintf(keys[n], sizeof(keys[n]), "sample:%s", r->target);
            snprintf(labels[n], sizeof(labels[n]), DM_TEXT_M64_REPLACED_S_SAMPLE, r->name);
            char tuning[64], pan[16];
            snprintf(tuning, sizeof(tuning), "%.17g", r->tuning);
            snprintf(pan, sizeof(pan), "%u", es_native_pan(r));
            ok = es_recipe_string(&recipes[n], "kind", "m64-sample-1") &&
                 es_recipe_string(&recipes[n], "path", r->target) &&
                 es_recipe_string(&recipes[n], "keep", j->keep_originals ? "1" : "0") &&
                 es_recipe_string(&recipes[n], "tuning", tuning) &&
                 es_recipe_add(&recipes[n], "audio", r->audio.data, r->audio.size);

            /* A centred zone sets no pan, so every stock drum keeps its own. */
            if (ok && r->params.pan != 0) {
                ok = es_recipe_string(&recipes[n], "drum-pan", pan);
            }
        } else {
            char field[64], path[256];
            snprintf(path, sizeof(path), "sound/samples/instruments/%s.aiff", r->name);
            snprintf(field, sizeof(field), "asset%u.path", asset);
            ok = es_recipe_string(&recipes[0], field, path);
            snprintf(field, sizeof(field), "asset%u.bytes", asset++);
            ok = ok && es_recipe_add(&recipes[0], field, r->audio.data, r->audio.size);
        }
    }

    for (size_t i = 0; i < count; i++) {
        mods[i] = (EsModification){
            .key = keys[i], .label = labels[i], .data = recipes[i].data, .size = recipes[i].size};
    }

    EsModifications state = {.items = mods, .count = count};
    ok = ok && (package_host ? es_package_plan(repo, &state, package_host, error, cap)
                             : es_repository_apply_batch(repo, mods, count, error, cap));

    for (size_t i = 0; i < count; i++) {
        es_recipe_free(&recipes[i]);
    }

    return ok;
}

bool m64_repository_apply(M64PluginJob *job, EsRepository *repo, const M64Result *result,
                          char *error, size_t cap) {
    return compose(job, repo, result, NULL, error, cap);
}

bool m64_repository_package(M64PluginJob *job, EsRepository *probe, const M64Result *result,
                            const DmeHost *host, char *error, size_t cap) {
    return compose(job, probe, result, host, error, cap);
}
