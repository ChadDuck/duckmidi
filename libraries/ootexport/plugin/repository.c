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
    size_t n = strlen(s), b = strlen(end);
    return n >= b && !strcmp(s + n - b, end);
}

static bool version_from(const EsFile *makefile, const EsFile *options, char out[64], char *error,
                         size_t cap) {
    bool found = false;
    return es_make_value(options, "VERSION", out, &found, error, cap) &&
           es_make_value(makefile, "VERSION", out, &found, error, cap) &&
           (found || fail(error, cap, DM_TEXT_OOT_THE_SELECTED_BUILD_VERSION_CANNOT_BE_RESOLVED,
                          "Makefile VERSION"));
}

static bool detect(EsRoot *root, DmeHistoryTarget *target, char *error, size_t cap) {
    char *version = target->version;
    EsFile makefile = {0}, compiler = {0}, options = {0};
    bool ok = es_fs_read(root, "Makefile", &makefile, error, cap) &&
              es_fs_read(root, ".make_options.mk", &options, error, cap) &&
              es_fs_read(root, "tools/audio/soundfont_compiler.c", &compiler, error, cap) &&
              compiler.exists && version_from(&makefile, &options, version, error, cap);
    snprintf(target->id, sizeof(target->id), "oot");
    snprintf(target->name, sizeof(target->name), "OoT");
    target->capabilities = DME_REPOSITORY_CUSTOM;
    es_file_free(&makefile);
    es_file_free(&compiler);
    es_file_free(&options);
    return ok || (*error ? false
                         : fail(error, cap, DM_TEXT_NATIVE_SELECT_OOT,
                                "tools/audio/soundfont_compiler.c"));
}

static bool allowed(const char *path) {
    const char *fixed[] = {OOT_SEQUENCE_TABLE, OOT_SPEC,   OOT_SESSION_CONFIG, OOT_AUDIO_HEAP_C,
                           OOT_BUFFERS_H,      "Makefile", ".make_options.mk"};

    for (size_t i = 0; i < sizeof(fixed) / sizeof(*fixed); i++) {
        if (!strcmp(path, fixed[i])) {
            return true;
        }
    }

    if (suffix(path, ".bkp")) {
        char original[DME_PATH_LIMIT];
        size_t n = strlen(path);

        if (n >= sizeof(original)) {
            return false;
        }

        memcpy(original, path, n - 4);
        original[n - 4] = 0;
        return allowed(original);
    }

    const char *audio = !strncmp(path, "assets/audio/", 13) ? path
                        : !strncmp(path, "extracted/", 10)  ? strchr(path + 10, '/')
                                                            : NULL;
    audio = audio && *audio == '/' ? audio + 1 : audio;
    return audio && !strncmp(audio, "assets/audio/", 13) &&
           (suffix(path, ".wav") || suffix(path, ".xml") || suffix(path, ".seq"));
}

static bool inventory(EsRepository *repo, EsPaths *paths, char *error, size_t cap) {
    EsFile makefile = {0}, options = {0};
    char version[64], extracted[256];
    bool ok = es_repository_read(repo, "Makefile", &makefile, error, cap) &&
              es_repository_read(repo, ".make_options.mk", &options, error, cap) &&
              version_from(&makefile, &options, version, error, cap) &&
              es_paths_add(paths, ".make_options.mk");
    es_file_free(&makefile);
    es_file_free(&options);
    snprintf(extracted, sizeof(extracted), "extracted/%s/assets/audio", version);
    const char *folders[] = {"assets/audio", extracted};

    for (unsigned f = 0; ok && f < 2; f++) {
        EsPaths all = {0};
        ok = es_repository_paths(repo, folders[f], true, &all, error, cap);

        for (size_t i = 0; ok && i < all.count; i++) {
            if (allowed(all.items[i])) {
                ok = es_paths_add(paths, all.items[i]);
            }
        }

        es_paths_free(&all);
    }

    const char *fixed[] = {OOT_SEQUENCE_TABLE, OOT_SPEC,      OOT_SESSION_CONFIG,
                           OOT_AUDIO_HEAP_C,   OOT_BUFFERS_H, "Makefile"};

    for (size_t i = 0; ok && i < sizeof(fixed) / sizeof(*fixed); i++) {
        EsFile file = {0};
        ok = es_repository_read(repo, fixed[i], &file, error, cap);

        if (ok && !file.exists) {
            snprintf(error, cap, "%s: %s", DM_TEXT_OOT_MISSING_NATIVE_SOURCE_INPUT, fixed[i]);
            ok = false;
        }

        ok = ok && es_paths_add(paths, fixed[i]);
        es_file_free(&file);
        char backup[256];
        snprintf(backup, sizeof(backup), "%s.bkp", fixed[i]);
        ok = ok && es_repository_read(repo, backup, &file, error, cap);

        if (ok && file.exists) {
            ok = es_paths_add(paths, backup);
        }

        es_file_free(&file);
    }

    return ok;
}

static bool current(EsRepository *repo, const EsPlan *plan, const char *path, EsFile *file,
                    char *error, size_t cap) {
    const EsFile *desired = plan ? es_plan_find(plan, path) : NULL;

    if (!desired) {
        return es_repository_baseline(repo, path, file, error, cap);
    }

    file->data = malloc(desired->size + 1);

    if (!file->data) {
        return false;
    }

    memcpy(file->data, desired->data, desired->size);
    file->data[desired->size] = 0;
    file->size = desired->size;
    file->mode = desired->mode;
    file->exists = desired->exists;
    return true;
}

static bool get_version(EsRepository *repo, char version[64], char *error, size_t cap) {
    EsFile file = {0}, options = {0};
    bool ok = es_repository_baseline(repo, "Makefile", &file, error, cap) &&
              es_repository_baseline(repo, ".make_options.mk", &options, error, cap) &&
              version_from(&file, &options, version, error, cap);
    es_file_free(&file);
    es_file_free(&options);
    return ok;
}

static bool effective(EsRepository *repo, const EsPlan *plan, const char *logical,
                      const char *version, char physical[DME_PATH_LIMIT], EsFile *file, char *error,
                      size_t cap) {
    char extracted[DME_PATH_LIMIT];

    if (snprintf(extracted, sizeof(extracted), "extracted/%s/%s", version, logical) >=
        (int)sizeof(extracted)) {
        return false;
    }

    EsFile authored = {0}, fallback = {0};
    bool ok = current(repo, plan, logical, &authored, error, cap) &&
              current(repo, plan, extracted, &fallback, error, cap);

    if (ok && authored.exists && fallback.exists && suffix(logical, ".xml")) {
        ok = fail(error, cap,
                  DM_TEXT_OOT_DUPLICATE_NATIVE_XML_INPUT_REMOVE_THE_AUTHORED_EXTRACTED_AMBIGUITY,
                  logical);
    }

    if (ok && !authored.exists && !fallback.exists) {
        ok = fail(error, cap, DM_TEXT_OOT_MISSING_NATIVE_SOURCE_INPUT, logical);
    }

    if (ok) {
        snprintf(physical, DME_PATH_LIMIT, "%s", authored.exists ? logical : extracted);
        *file = authored.exists ? authored : fallback;

        if (authored.exists) {
            authored = (EsFile){0};
        } else {
            fallback = (EsFile){0};
        }
    }

    es_file_free(&authored);
    es_file_free(&fallback);
    return ok;
}

/* Resolves the effective physical input for a logical path without reading it: an authored
   source takes precedence over the extracted copy. */
static bool effective_path(EsRepository *repo, const EsPlan *plan, const char *logical,
                           const char *version, char physical[DME_PATH_LIMIT], char *error,
                           size_t cap) {
    char extracted[DME_PATH_LIMIT];
    bool authored = false, fallback = false;

    if (snprintf(extracted, sizeof(extracted), "extracted/%s/%s", version, logical) >=
            (int)sizeof(extracted) ||
        !es_plan_exists(repo, plan, logical, &authored, error, cap) ||
        !es_plan_exists(repo, plan, extracted, &fallback, error, cap)) {
        return false;
    }

    if (!authored && !fallback) {
        return fail(error, cap, DM_TEXT_OOT_MISSING_NATIVE_SOURCE_INPUT, logical);
    }

    snprintf(physical, DME_PATH_LIMIT, "%s", authored ? logical : extracted);
    return true;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

typedef struct {
    double rate;
    int root, fine;
} Pitch;

static bool wav_pitch(const EsFile *file, Pitch *pitch) {
    *pitch = (Pitch){.root = 60};

    if (!file->exists || file->size < 12 || memcmp(file->data, "RIFF", 4) ||
        memcmp(file->data + 8, "WAVE", 4)) {
        return false;
    }

    bool inst = false;

    for (size_t i = 12; i + 8 <= file->size;) {
        size_t n = le32(file->data + i + 4);
        const uint8_t *data = file->data + i + 8;

        if (n > file->size - i - 8) {
            return false;
        }

        if (!memcmp(file->data + i, "fmt ", 4) && n >= 16) {
            pitch->rate = le32(data + 4);
        } else if (!memcmp(file->data + i, "inst", 4) && n >= 7) {
            pitch->root = data[0];
            pitch->fine = (int8_t)data[1];
            inst = true;
        } else if (!inst && !memcmp(file->data + i, "smpl", 4) && n >= 36) {
            pitch->root = (int)le32(data + 12);
            pitch->fine = (data[19] * 50 + 64) / 128;

            if (pitch->fine > 50) {
                pitch->root = (pitch->root - 1) & 127;
                pitch->fine -= 100;
            }
        }

        i += 8 + n + (n & 1);
    }

    return pitch->rate > 0 && pitch->root >= 0 && pitch->root < 100;
}

static int note(const char *s) {
    if (!s || !strchr("CDEFGAB", *s)) {
        return -1;
    }

    const int semitones[] = {9, 11, 0, 2, 4, 5, 7};
    int pitch = semitones[*s++ - 'A'];

    if (*s == 'F') {
        pitch--;
        s++;
    } else if (*s == 'S') {
        pitch++;
        s++;
    }

    bool negative = !strncmp(s, "NEG", 3);
    s += negative ? 3 : 0;
    char *end;
    long octave = strtol(s, &end, 10);
    return *s && !*end ? (int)((negative ? -octave : octave) + 1) * 12 + pitch : -1;
}

static bool pitch_override(Pitch *p, const EsXmlNode *node, const char *ending) {
    char key[32];
    snprintf(key, sizeof(key), "SampleRate%s", ending);
    const char *rate = es_xml_attribute(node, key);
    snprintf(key, sizeof(key), "BaseNote%s", ending);
    const char *base = es_xml_attribute(node, key);
    snprintf(key, sizeof(key), "FineTune%s", ending);
    const char *fine = es_xml_attribute(node, key);

    if (rate) {
        char *end;
        p->rate = strtod(rate, &end);

        if (*end) {
            return false;
        }
    }

    if (base) {
        p->root = note(base);
    }

    if (fine) {
        char *end;
        long value = strtol(fine, &end, 10);

        if (*end || value < -100 || value > 100) {
            return false;
        }

        p->fine = (int)value;
    }

    return isfinite(p->rate) && p->rate > 0 && p->root >= 0 && p->root < 100;
}

static double tuning(Pitch p) {
    return p.rate / 32000 * pow(2, (60.0 - p.root) / 12) * pow(2, p.fine / 1200.0);
}

typedef struct {
    char *bank, *name, *path, *key; /* key joins bank and name for lookup. */
} Sample;

typedef struct {
    EsRepository *repo;
    EsPlan *plan;
    EsTargets *targets;
    char version[64];
    Sample *samples;
    size_t count, capacity;
    char *bank_indices[256];
    EsMap sample_index;
    EsPaths target_paths;
    const EsModification *replacement;
    const char *replace_path;
    double replacement_tuning;
    int replacement_pan; /* Optional replacement override; -1 preserves initialized drum pan. */
} Graph;

static char *duplicate(const char *s) {
    char *p = s ? malloc(strlen(s) + 1) : NULL;

    if (p) {
        strcpy(p, s);
    }

    return p;
}

static bool sample_key(const char *bank, const char *name, char *out, size_t capacity) {
    /* A newline cannot occur in either part, so it separates them unambiguously. */
    return snprintf(out, capacity, "%s\n%s", bank, name) < (int)capacity;
}

static bool graph_sample(Graph *g, const char *bank, const char *name, const char *path) {
    char key[16384];

    if (g->count >= 20000 || !sample_key(bank, name, key, sizeof(key))) {
        return false;
    }

    if (g->count == g->capacity) {
        size_t capacity = g->capacity ? g->capacity * 2 : 256;
        Sample *grown = realloc(g->samples, capacity * sizeof(*grown));

        if (!grown) {
            return false;
        }

        g->samples = grown;
        g->capacity = capacity;
    }

    Sample *s = &g->samples[g->count];
    *s = (Sample){duplicate(bank), duplicate(name), duplicate(path), duplicate(key)};

    if (!s->bank || !s->name || !s->path || !s->key) {
        free(s->bank);
        free(s->name);
        free(s->path);
        free(s->key);
        return false;
    }

    /* Lookups hold an index; the key string is owned by the sample. */
    return es_map_put(&g->sample_index, s->key, g->count++);
}

static void graph_free(Graph *g) {
    for (size_t i = 0; i < g->count; i++) {
        free(g->samples[i].bank);
        free(g->samples[i].name);
        free(g->samples[i].path);
        free(g->samples[i].key);
    }

    for (unsigned i = 0; i < 256; i++) {
        free(g->bank_indices[i]);
    }

    free(g->samples);
    es_map_free(&g->sample_index);
    es_paths_free(&g->target_paths);
}

static bool native_path(const char *build, const char *folder, const char *extension,
                        char logical[DME_PATH_LIMIT]) {
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "$(BUILD_DIR)/assets/audio/%s/", folder);

    if (!build || strncmp(build, prefix, strlen(prefix)) || !suffix(build, extension) ||
        strlen(build) >= DME_PATH_LIMIT || strstr(build, "..") || strchr(build, '\\')) {
        return false;
    }

    snprintf(logical, DME_PATH_LIMIT, "assets/audio/%s/%s", folder, build + strlen(prefix));
    return true;
}

static bool index_value(const char *s, unsigned *number) {
    char *end;

    if (!s || !*s) {
        return false;
    }

    unsigned long value = strtoul(s, &end, 0);
    *number = (unsigned)value;
    return !*end && value <= 255;
}

static const Sample *find_sample(const Graph *, const char *, const char *);

static bool scan_bank(Graph *g, const char *logical, char *error, size_t cap) {
    char path[DME_PATH_LIMIT];
    EsFile source = {0};
    bool ok = effective(g->repo, g->plan, logical, g->version, path, &source, error, cap);
    EsXml *xml = ok ? es_xml_parse(source.data, source.size, error, cap) : NULL;
    const EsXmlNode *root = xml ? es_xml_node(xml, 0) : NULL;
    unsigned index;
    ok = root && !strcmp(root->name, "SampleBank") &&
         index_value(es_xml_attribute(root, "Index"), &index);

    for (size_t i = 0; ok && i < es_xml_count(xml); i++) {
        const EsXmlNode *n = es_xml_node(xml, i);

        if (!i || !strcmp(n->name, "Pointer")) {
            unsigned alias = index;
            ok = !i || index_value(es_xml_attribute(n, "Index"), &alias);

            if (ok && g->bank_indices[alias]) {
                ok = fail(error, cap, DM_TEXT_OOT_DUPLICATE_NATIVE_SAMPLEBANK_INDEX_OR_POINTER,
                          path);
            }

            if (ok) {
                g->bank_indices[alias] = duplicate(path);
                ok = g->bank_indices[alias] != NULL;
            }

            continue;
        }

        if (strcmp(n->name, "Sample")) {
            ok = fail(error, cap, DM_TEXT_OOT_UNSUPPORTED_SAMPLEBANK_ELEMENT, n->name);
            break;
        }

        const char *name = es_xml_attribute(n, "Name");
        char wav_logical[DME_PATH_LIMIT], physical[DME_PATH_LIMIT];
        ok = name && *name &&
             native_path(es_xml_attribute(n, "Path"), "samples", ".aifc", wav_logical);

        if (!ok) {
            break;
        }

        strcpy(wav_logical + strlen(wav_logical) - 5, ".wav");
        ok = effective_path(g->repo, g->plan, wav_logical, g->version, physical, error, cap);

        if (ok && find_sample(g, logical, name)) {
            ok = fail(error, cap, DM_TEXT_OOT_DUPLICATE_NATIVE_SAMPLE_NAME, name);
        }

        ok = ok && graph_sample(g, logical, name, physical) &&
             (!g->plan || es_plan_depend(g->plan, path, physical, name));
    }

    es_xml_free(xml);
    es_file_free(&source);
    return ok ||
           (*error ? false : fail(error, cap, DM_TEXT_OOT_UNSUPPORTED_NATIVE_SAMPLEBANK, path));
}

static const Sample *find_sample(const Graph *g, const char *bank, const char *name) {
    char key[16384];
    size_t index;
    return bank && name && sample_key(bank, name, key, sizeof(key)) &&
                   es_map_get(&g->sample_index, key, &index)
               ? &g->samples[index]
               : NULL;
}

static bool patch_pitch(EsXml *xml, size_t node, const char *ending, double rate, EsEdits *edits,
                        char *error, size_t cap) {
    char attr[32], value[96];
    snprintf(value, sizeof(value), "%.17g", rate * 32000);
    snprintf(attr, sizeof(attr), "SampleRate%s", ending);
    bool ok = es_xml_set_attribute(xml, node, attr, value, edits, error, cap);
    snprintf(attr, sizeof(attr), "BaseNote%s", ending);
    ok = ok && es_xml_set_attribute(xml, node, attr, "C4", edits, error, cap);
    snprintf(attr, sizeof(attr), "FineTune%s", ending);
    return ok && es_xml_set_attribute(xml, node, attr, "0", edits, error, cap);
}

static bool scan_font(Graph *g, const char *logical, bool font_indices[256], char *error,
                      size_t cap) {
    char path[DME_PATH_LIMIT], banks[2][DME_PATH_LIMIT] = {{0}};
    EsFile source = {0};
    bool ok = effective(g->repo, g->plan, logical, g->version, path, &source, error, cap);
    EsXml *xml = ok ? es_xml_parse(source.data, source.size, error, cap) : NULL;
    const EsXmlNode *root = xml ? es_xml_node(xml, 0) : NULL;
    unsigned index;
    ok = root && !strcmp(root->name, "Soundfont") &&
         index_value(es_xml_attribute(root, "Index"), &index) && index < 48;

    if (ok && font_indices[index]) {
        ok = fail(error, cap, DM_TEXT_OOT_DUPLICATE_NATIVE_SOUNDFONT_INDEX, path);
    }

    if (ok) {
        font_indices[index] = true;
    }

    const char *bank_attrs[] = {"SampleBank", "SampleBankDD"};

    for (unsigned b = 0; ok && b < 2; b++) {
        const char *attr = es_xml_attribute(root, bank_attrs[b]);

        if (!attr && b == 1) {
            continue;
        }

        ok = native_path(attr, "samplebanks", ".xml", banks[b]);
    }

    /* Sample defaults by name; a name defined twice is an error only when referenced. */
    EsMap definitions = {0};
    EsPaths duplicates = {0};

    for (size_t d = 0; ok && d < es_xml_count(xml); d++) {
        const EsXmlNode *candidate = es_xml_node(xml, d);
        const char *id = es_xml_attribute(candidate, "Name");

        if (strcmp(candidate->name, "Sample") || !id) {
            continue;
        }

        ok = es_map_get(&definitions, id, NULL) ? es_paths_add(&duplicates, id)
                                                : es_map_put(&definitions, id, d);
    }

    EsEdits edits = {0};

    for (size_t i = 0; ok && i < es_xml_count(xml); i++) {
        const EsXmlNode *n = es_xml_node(xml, i);
        const char *refs[] = {"Sample", "SampleLo", "SampleHi"};
        bool pan_patched = false;

        for (unsigned r = 0; ok && r < 3; r++) {
            const char *name = es_xml_attribute(n, refs[r]);
            bool defaults = !strcmp(n->name, "Sample");

            if (defaults && !r) {
                name = es_xml_attribute(n, "Name");
            }

            if (!name) {
                continue;
            }

            const EsXmlNode *definition = NULL;
            size_t definition_index = 0;

            if (es_paths_has(&duplicates, name)) {
                ok = fail(error, cap, DM_TEXT_OOT_DUPLICATE_SOUNDFONT_SAMPLE_DEFAULT, name);
            } else if (es_map_get(&definitions, name, &definition_index)) {
                definition = es_xml_node(xml, definition_index);
            }

            if (!ok || !definition) {
                ok = ok && fail(error, cap, DM_TEXT_OOT_MISSING_SOUNDFONT_SAMPLE_DEFAULT, name);
                break;
            }

            const char *dd = es_xml_attribute(definition, "IsDD");

            if (!dd && definition->parent != SIZE_MAX) {
                dd = es_xml_attribute(es_xml_node(xml, definition->parent), "IsDD");
            }

            if (dd && strcmp(dd, "true") && strcmp(dd, "false")) {
                ok = fail(error, cap, DM_TEXT_OOT_INVALID_SAMPLE_ISDD, path);
                break;
            }

            const Sample *sample = find_sample(g, banks[dd && !strcmp(dd, "true")], name);

            if (!sample) {
                ok = fail(error, cap, DM_TEXT_OOT_UNRESOLVED_SOUNDFONT_SAMPLE, name);
                break;
            }

            if (g->targets && !es_paths_has(&g->target_paths, sample->path)) {
                char id[16384], label[16384];
                snprintf(id, sizeof(id), "%s|%s", es_repository_identity(g->repo), sample->path);
                snprintf(label, sizeof(label), "%s / %s / %s — %s", logical, n->name, name,
                         sample->path);
                ok = es_targets_add(g->targets, id, label, sample->path) &&
                     es_paths_add(&g->target_paths, sample->path);
            }

            if (ok && g->plan) {
                ok = es_plan_depend(g->plan, path, sample->path, refs[r]);
            }

            if (!ok || !g->replace_path || strcmp(sample->path, g->replace_path)) {
                continue;
            }

            EsFile wav = {0};
            Pitch natural = {0}, configured;
            ok = es_repository_baseline(g->repo, sample->path, &wav, error, cap) &&
                 wav_pitch(&wav, &natural);
            es_file_free(&wav);
            configured = natural;
            const char *ending = r == 1 ? "Lo" : r == 2 ? "Hi" : "";
            ok = ok && pitch_override(&configured, definition, "") &&
                 (defaults || pitch_override(&configured, n, ending));

            if (ok) {
                double desired = tuning(configured) / tuning(natural) * g->replacement_tuning;
                ok = patch_pitch(xml, defaults ? definition_index : i, ending, desired, &edits,
                                 error, cap) &&
                     es_plan_affect(g->plan, g->replacement->key, path, refs[r]);

                if (ok && !pan_patched && g->replacement_pan >= 0 && !strcmp(n->name, "Drum")) {
                    char pan[16];
                    snprintf(pan, sizeof(pan), "%d", g->replacement_pan);
                    ok = es_xml_set_attribute(xml, i, "Pan", pan, &edits, error, cap);
                    pan_patched = true;
                }
            }
        }
    }

    if (ok && edits.count) {
        EsFile edited = {0};
        ok = es_edits_apply(&edits, source.data, source.size, &edited, error, cap) &&
             es_plan_put(g->plan, path, edited.data, edited.size, source.mode, true, error, cap);
        es_file_free(&edited);
    }

    es_edits_free(&edits);
    es_map_free(&definitions);
    es_paths_free(&duplicates);
    es_xml_free(xml);
    es_file_free(&source);
    return ok ||
           (*error ? false
                   : fail(error, cap, DM_TEXT_OOT_UNSUPPORTED_NATIVE_SOUNDFONT_OR_PITCH, logical));
}

static bool scan_graph(EsRepository *repo, EsPlan *plan, EsTargets *targets,
                       const EsModification *replacement, char *error, size_t cap) {
    Graph g = {.repo = repo,
               .plan = plan,
               .targets = targets,
               .replacement = replacement,
               .replacement_pan = -1};
    bool ok = get_version(repo, g.version, error, cap);
    EsPaths paths = {0}, banks = {0}, fonts = {0};
    ok = ok && es_repository_baseline_paths(repo, &paths, error, cap);

    for (size_t i = 0; ok && plan && i < plan->count; i++) {
        ok = es_paths_add(&paths, plan->writes[i].path);
    }

    for (size_t i = 0; ok && i < paths.count; i++) {
        const char *logical = paths.items[i];

        if (!strncmp(logical, "extracted/", 10)) {
            logical = strchr(logical + 10, '/');
            logical = logical ? logical + 1 : "";
        }

        if (!suffix(logical, ".xml")) {
            continue;
        }

        if (!strncmp(logical, "assets/audio/samplebanks/", 25)) {
            ok = es_paths_add(&banks, logical);
        } else if (!strncmp(logical, "assets/audio/soundfonts/", 24)) {
            ok = es_paths_add(&fonts, logical);
        }
    }

    if (replacement) {
        g.replace_path = es_recipe_text(replacement->data, replacement->size, "path");
        const char *value = es_recipe_text(replacement->data, replacement->size, "tuning");
        g.replacement_tuning = value ? strtod(value, NULL) : 0;
        ok = ok && g.replace_path && isfinite(g.replacement_tuning) && g.replacement_tuning > 0;
        const char *pan = es_recipe_text(replacement->data, replacement->size, "drum-pan");

        if (pan) {
            char *end;
            long number = strtol(pan, &end, 10);
            ok = ok && *pan && !*end && number >= 0 && number <= 127;
            g.replacement_pan = (int)number;

            if (!ok && !*error) {
                fail(error, cap, DM_TEXT_NATIVE_INVALID_DRUM_PAN, g.replace_path);
            }
        }
    }

    for (size_t i = 0; ok && i < banks.count; i++) {
        ok = scan_bank(&g, banks.items[i], error, cap);
    }

    bool font_indices[256] = {0};

    for (size_t i = 0; ok && i < fonts.count; i++) {
        ok = scan_font(&g, fonts.items[i], font_indices, error, cap);
    }

    ok = ok && banks.count && fonts.count && g.count;

    graph_free(&g);
    es_paths_free(&paths);
    es_paths_free(&banks);
    es_paths_free(&fonts);
    return ok;
}

bool oot_repository_targets(EsRepository *repo, EsTargets *targets, char *error, size_t cap) {
    return scan_graph(repo, NULL, targets, NULL, error, cap);
}

bool oot_repository_sample_targets(OotPluginJob *job, EsRepository *repo, const DmeContext *context,
                                   const DmeHost *host, char *error, size_t cap) {
    Graph g = {.repo = repo, .replacement_pan = -1};
    EsPaths paths = {0}, banks = {0};
    job->sample_bank[0] = 0;
    bool ok = get_version(repo, g.version, error, cap) &&
              es_repository_baseline_paths(repo, &paths, error, cap);

    for (size_t i = 0; ok && i < paths.count; i++) {
        const char *logical = paths.items[i];

        if (!strncmp(logical, "extracted/", 10)) {
            logical = strchr(logical + 10, '/');
            logical = logical ? logical + 1 : "";
        }

        if (!strncmp(logical, "assets/audio/samplebanks/", 25) && suffix(logical, ".xml")) {
            ok = es_paths_add(&banks, logical);
        }
    }

    for (size_t i = 0; ok && i < banks.count; i++) {
        ok = scan_bank(&g, banks.items[i], error, cap);
    }

    /* First decide which Replace regions can apply. A Replace that cannot is added as a new
       sample instead, so the song still plays it. */
    static const char default_bank[] = "assets/audio/samplebanks/SampleBank_0.xml";
    const Sample *selections[ES_NATIVE_REGIONS] = {0};
    double tunings[ES_NATIVE_REGIONS] = {0};

    for (size_t i = 0; ok && i < job->samples.count; i++) {
        EsNativeRegion *r = &job->samples.regions[i];

        if (!r->target) {
            continue;
        }

        const Sample *selected = NULL;

        for (size_t s = 0; s < g.count; s++) {
            char id[16384];
            snprintf(id, sizeof(id), "%s|%s", es_repository_identity(repo), g.samples[s].path);

            if (!strcmp(id, r->target) || !strcmp(g.samples[s].path, r->target)) {
                selected = &g.samples[s];
                break;
            }
        }

        if (!selected || strlen(selected->name) >= sizeof(r->name) ||
            strpbrk(selected->name, "\"&<>")) {
            es_native_add_instead(&job->samples, r);
            continue;
        }

        EsFile wav = {0};
        Pitch pitch = {0};
        ok = es_repository_baseline(repo, selected->path, &wav, error, cap);
        bool pitched = ok && wav_pitch(&wav, &pitch);
        es_file_free(&wav);

        if (ok && (!pitched || !es_native_resample_fits(r, tuning(pitch)))) {
            es_native_add_instead(&job->samples, r);
            continue;
        }

        selections[i] = selected;
        tunings[i] = tuning(pitch);
    }

    /* One generated soundfont reads one samplebank. Replaces outside SampleBank_0 apply only when
       every played note is a Replace from that same bank; otherwise they become Adds. */
    const char *other = NULL;
    bool single = true;

    for (size_t i = 0; ok && i < job->samples.count; i++) {
        if (job->samples.regions[i].target && strcmp(selections[i]->bank, default_bank)) {
            single &= !other || !strcmp(other, selections[i]->bank);
            other = other ? other : selections[i]->bank;
        }
    }

    if (ok && other) {
        for (size_t i = 0; single && i < job->samples.count; i++) {
            single = job->samples.regions[i].target && !strcmp(selections[i]->bank, other);
        }

        for (uint64_t i = 0; single && i < context->event_count; i++) {
            const DmeEvent *e = &context->events[i];

            if ((e->status & MIDI_STATUS_MASK) == MIDI_NOTE_ON && e->b) {
                single = job->samples.notes && job->samples.notes[i].custom;
            }
        }

        for (size_t i = 0; !single && i < job->samples.count; i++) {
            if (job->samples.regions[i].target && strcmp(selections[i]->bank, default_bank)) {
                es_native_add_instead(&job->samples, &job->samples.regions[i]);
            }
        }
    }

    for (size_t i = 0; ok && i < job->samples.count; i++) {
        EsNativeRegion *r = &job->samples.regions[i];

        if (!r->target) {
            continue;
        }

        const Sample *selected = selections[i];
        snprintf(job->sample_bank, sizeof(job->sample_bank), "%s", selected->bank);
        ok = es_native_resample(r, tunings[i], ES_NATIVE_OOT, host, error, cap) == ES_RESAMPLE_OK;
        char *physical = ok ? duplicate(selected->path) : NULL;
        ok = physical != NULL;

        if (ok) {
            free(r->target);
            r->target = physical;
            strcpy(r->name, selected->name);
            r->reference_tuning = tunings[i];
        }
    }

    graph_free(&g);
    es_paths_free(&paths);
    es_paths_free(&banks);
    return ok;
}

static bool append_sample(EsRepository *repo, EsPlan *plan, const char *version, const char *name,
                          char *error, size_t cap) {
    char path[DME_PATH_LIMIT];
    EsFile source = {0};
    bool ok = effective(repo, plan, "assets/audio/samplebanks/SampleBank_0.xml", version, path,
                        &source, error, cap);
    EsXml *xml = ok ? es_xml_parse(source.data, source.size, error, cap) : NULL;
    char native[1024];
    snprintf(native, sizeof(native), "$(BUILD_DIR)/assets/audio/samples/SampleBank_0/%s.aifc",
             name);
    bool found = false;

    for (size_t i = 0; xml && ok && i < es_xml_count(xml); i++) {
        const EsXmlNode *n = es_xml_node(xml, i);
        const char *id = es_xml_attribute(n, "Name");

        if (!strcmp(n->name, "Sample") && id && !strcmp(id, name)) {
            const char *existing = es_xml_attribute(n, "Path");
            ok = existing && !strcmp(existing, native);
            found = true;
        }
    }

    if (ok && xml && !found) {
        char fragment[2048];
        snprintf(fragment, sizeof(fragment), "\n    <Sample Name=\"%s\" Path=\"%s\"/>\n", name,
                 native);
        EsEdits edits = {0};
        EsFile edited = {0};
        ok = es_xml_append(xml, 0, fragment, &edits, error, cap) &&
             es_edits_apply(&edits, source.data, source.size, &edited, error, cap) &&
             es_plan_put(plan, path, edited.data, edited.size, source.mode, true, error, cap);
        es_edits_free(&edits);
        es_file_free(&edited);
    }

    es_xml_free(xml);
    es_file_free(&source);
    return ok && xml;
}

static bool memory_plan(EsRepository *repo, const EsModifications *mods, EsPlan *plan,
                        uint32_t sequence, uint32_t font, char *error, size_t cap) {
    EsFile session = {0};
    OotText edited = {0};
    uint32_t growth = 0;
    bool ok = es_repository_baseline(repo, OOT_SESSION_CONFIG, &session, error, cap) &&
              oot_patch_session_config(session.data, session.size, sequence, font, &edited, &growth,
                                       error, cap);

    if (ok && growth) {
        ok = es_plan_put(plan, OOT_SESSION_CONFIG, edited.data, edited.size, session.mode, true,
                         error, cap);
        const char *heaps[] = {OOT_AUDIO_HEAP_C, OOT_BUFFERS_H};

        for (unsigned i = 0; ok && i < 2; i++) {
            EsFile original = {0};
            OotText heap = {0};
            ok = es_repository_baseline(repo, heaps[i], &original, error, cap) &&
                 oot_patch_audio_heap(original.data, original.size, growth, &heap, error, cap) &&
                 es_plan_put(plan, heaps[i], heap.data, heap.size, original.mode, true, error, cap);
            oot_text_free(&heap);
            es_file_free(&original);
        }

        const char *paths[] = {OOT_SESSION_CONFIG, OOT_AUDIO_HEAP_C, OOT_BUFFERS_H};

        for (size_t i = 0; ok && i < mods->count; i++) {
            const char *enlarge =
                es_recipe_text(mods->items[i].data, mods->items[i].size, "enlarge");

            for (unsigned p = 0; enlarge && !strcmp(enlarge, "1") && ok && p < 3; p++) {
                ok = es_plan_affect(plan, mods->items[i].key, paths[p], "audio memory");
            }
        }
    }

    oot_text_free(&edited);
    es_file_free(&session);
    return ok;
}

static bool render(EsRepository *repo, const EsModifications *mods, EsPlan *plan, char *error,
                   size_t cap) {
    char version[64];
    bool ok = get_version(repo, version, error, cap);
    EsFile spec = {0}, table = {0};
    ok = ok && es_repository_baseline(repo, OOT_SPEC, &spec, error, cap) &&
         es_repository_baseline(repo, OOT_SEQUENCE_TABLE, &table, error, cap);
    uint32_t max_sequence = 0, max_font = 0;
    /* Sequence files are named after the song, so two installed songs cannot share a name. */
    EsPaths names = {0};

    for (size_t i = 0; ok && i < mods->count; i++) {
        const EsModification *m = &mods->items[i];
        const char *kind = es_recipe_text(m->data, m->size, "kind");

        if (!kind) {
            ok = fail(error, cap, DM_TEXT_OOT_UNSUPPORTED_OOT_HISTORY_RECIPE, m->key);
            break;
        }

        if (!strcmp(kind, "oot-sample-1")) {
            const char *path = es_recipe_text(m->data, m->size, "path");
            size_t size = 0;
            const uint8_t *bytes = es_recipe_get(m->data, m->size, "audio", &size);
            ok = path && bytes && allowed(path) && suffix(path, ".wav") &&
                 es_plan_put(plan, path, bytes, size, 0, true, error, cap) &&
                 es_plan_backup(plan, path) && es_plan_affect(plan, m->key, path, "sample");
            continue;
        }

        if (strcmp(kind, "oot-song-1")) {
            ok = fail(error, cap, DM_TEXT_OOT_UNSUPPORTED_OOT_HISTORY_RECIPE_VERSION, kind);
            break;
        }

        const char *name = es_recipe_text(m->data, m->size, "name");
        const char *song = es_recipe_text(m->data, m->size, "song");
        const char *enlarge = es_recipe_text(m->data, m->size, "enlarge");
        const char *seq_bytes = es_recipe_text(m->data, m->size, "sequence-size");
        const char *font_bytes = es_recipe_text(m->data, m->size, "font-size");
        size_t sequence_size = 0, font_size = 0;
        const uint8_t *sequence = es_recipe_get(m->data, m->size, "sequence", &sequence_size);
        const uint8_t *font = es_recipe_get(m->data, m->size, "font", &font_size);
        ok = name && oot_name_valid(name) && song && enlarge && seq_bytes && font_bytes &&
             sequence && sequence_size;

        if (!ok) {
            break;
        }

        if (es_paths_has(&names, name)) {
            ok = fail(error, cap, DM_TEXT_OOT_SEQUENCE_NAME_IN_USE, name);
            break;
        }

        ok = es_paths_add(&names, name);

        bool has_font = font && font_size;
        int index = has_font ? oot_next_font_index(spec.data, spec.size, name) : -1;

        if (has_font && (index < 0 || index >= 48)) {
            ok = fail(error, cap,
                      DM_TEXT_OOT_THE_NATIVE_48_SOUNDFONT_LOAD_STATUS_CAPACITY_IS_EXHAUSTED, name);
            break;
        }

        OotText new_spec = {0}, new_table = {0};
        char notice[512] = {0};
        ok = oot_patch_spec(spec.data, spec.size, name, has_font, &new_spec, error, cap) &&
             oot_patch_sequence_table(table.data, table.size, name, *song ? song : NULL,
                                      strtoull(seq_bytes, NULL, 10), &new_table, notice,
                                      sizeof(notice), error, cap);

        if (ok) {
            es_file_free(&spec);
            spec =
                (EsFile){.data = (uint8_t *)new_spec.data, .size = new_spec.size, .exists = true};
            new_spec = (OotText){0};
            es_file_free(&table);
            table =
                (EsFile){.data = (uint8_t *)new_table.data, .size = new_table.size, .exists = true};
            new_table = (OotText){0};
        }

        oot_text_free(&new_spec);
        oot_text_free(&new_table);
        char path[512];
        snprintf(path, sizeof(path), "assets/audio/sequences/%s.seq", name);
        ok = ok && es_plan_put(plan, path, sequence, sequence_size, 0, true, error, cap) &&
             es_plan_affect(plan, m->key, path, "sequence") &&
             es_plan_affect(plan, m->key, OOT_SPEC, name) &&
             es_plan_affect(plan, m->key, OOT_SEQUENCE_TABLE, *song ? song : name);

        if (ok && has_font) {
            EsXml *xml = es_xml_parse(font, font_size, error, cap);
            EsEdits edits = {0};
            EsFile adjusted = {0};
            char value[32];
            snprintf(value, sizeof(value), "%d", index);
            ok = xml && es_xml_set_attribute(xml, 0, "Index", value, &edits, error, cap) &&
                 es_edits_apply(&edits, font, font_size, &adjusted, error, cap);
            char font_path[512];
            snprintf(font_path, sizeof(font_path), "assets/audio/soundfonts/Soundfont_%s.xml",
                     name);
            ok = ok &&
                 es_plan_put(plan, font_path, adjusted.data, adjusted.size, 0, true, error, cap) &&
                 es_plan_affect(plan, m->key, font_path, "soundfont") &&
                 es_plan_depend(plan, path, font_path, "soundfont");
            es_file_free(&adjusted);
            es_edits_free(&edits);
            es_xml_free(xml);
        }

        for (unsigned a = 0; ok; a++) {
            char field[64], sample_path[512];
            snprintf(field, sizeof(field), "asset%u.name", a);
            const char *asset_name = es_recipe_text(m->data, m->size, field);

            if (!asset_name) {
                break;
            }

            snprintf(sample_path, sizeof(sample_path), "assets/audio/samples/SampleBank_0/%s.wav",
                     asset_name);
            snprintf(field, sizeof(field), "asset%u.bytes", a);
            size_t bytes = 0;
            const uint8_t *data = es_recipe_get(m->data, m->size, field, &bytes);
            const EsFile *other = es_plan_find(plan, sample_path);

            if (other && (other->size != bytes || !data || memcmp(other->data, data, bytes))) {
                ok = fail(error, cap,
                          DM_TEXT_M64_ACTIVE_SONGS_REQUEST_DIFFERENT_REVISIONS_OF_A_SHARED_CUSTOM,
                          sample_path);
            }

            ok = ok && data && es_plan_put(plan, sample_path, data, bytes, 0, true, error, cap) &&
                 es_plan_affect(plan, m->key, sample_path, "added sample") &&
                 append_sample(repo, plan, version, asset_name, error, cap);
            char physical[DME_PATH_LIMIT];
            EsFile bank = {0};
            ok = ok &&
                 effective(repo, plan, "assets/audio/samplebanks/SampleBank_0.xml", version,
                           physical, &bank, error, cap) &&
                 es_plan_affect(plan, m->key, physical, asset_name);
            es_file_free(&bank);
        }

        if (!strcmp(enlarge, "1")) {
            uint64_t seq_need = strtoull(seq_bytes, NULL, 10);
            uint64_t font_need = strtoull(font_bytes, NULL, 10);

            if (seq_need > UINT32_MAX || font_need > UINT32_MAX) {
                ok = false;
            } else {
                max_sequence = seq_need > max_sequence ? (uint32_t)seq_need : max_sequence;
                max_font = font_need > max_font ? (uint32_t)font_need : max_font;
            }
        }
    }

    if (ok && mods->count) {
        ok = es_plan_put(plan, OOT_SPEC, spec.data, spec.size, 0, true, error, cap) &&
             es_plan_put(plan, OOT_SEQUENCE_TABLE, table.data, table.size, 0, true, error, cap);
    }

    es_paths_free(&names);
    es_file_free(&spec);
    es_file_free(&table);

    if (ok && (max_sequence || max_font)) {
        ok = memory_plan(repo, mods, plan, max_sequence, max_font, error, cap);
    }

    for (size_t i = 0; ok && i < mods->count; i++) {
        const EsModification *m = &mods->items[i];
        const char *kind = es_recipe_text(m->data, m->size, "kind");

        if (kind && !strcmp(kind, "oot-sample-1")) {
            ok = scan_graph(repo, plan, NULL, m, error, cap);
        }
    }

    ok = ok && scan_graph(repo, plan, NULL, NULL, error, cap);

    for (size_t i = 0; ok && i < mods->count; i++) {
        const char *keep = es_recipe_text(mods->items[i].data, mods->items[i].size, "keep");

        if (keep && !strcmp(keep, "1")) {
            ok = es_plan_keep_originals(repo, plan, mods->items[i].key, error, cap);
        }
    }

    return ok;
}

int32_t DME_CALL oot_history_detect(const DmeHistoryApi *api, DmeHistoryRoot *root,
                                    DmeHistoryTarget *target, char *error, uint64_t cap) {
    (void)api;
    return detect((EsRoot *)root, target, error, (size_t)cap);
}

int32_t DME_CALL oot_history_inventory(const DmeHistoryApi *api, DmeHistory *history,
                                       DmeHistoryList *list, char *error, uint64_t cap) {
    (void)api;
    return inventory((EsRepository *)history, (EsPaths *)list, error, (size_t)cap);
}

int32_t DME_CALL oot_history_allow(const char *path) {
    return allowed(path);
}

int32_t DME_CALL oot_history_render(const DmeHistoryApi *api, DmeHistory *history,
                                    const DmeHistoryIntent *intents, uint32_t count,
                                    DmeHistoryPlan *plan, char *error, uint64_t cap) {
    (void)api;
    EsModifications mods;
    bool ok = es_intents_view(intents, count, &mods) &&
              render((EsRepository *)history, &mods, (EsPlan *)plan, error, (size_t)cap);
    es_intents_release(&mods);
    return ok;
}

int32_t DME_CALL oot_history_targets(const DmeHistoryApi *api, DmeHistory *history,
                                     DmeHistoryTargets *targets, char *error, uint64_t cap) {
    (void)api;
    return oot_repository_targets((EsRepository *)history, (EsTargets *)targets, error,
                                  (size_t)cap);
}

static bool compose(OotPluginJob *j, EsRepository *repo, const OotResult *result,
                    const DmeHost *package_host, char *error, size_t cap) {
    EsRecipe recipes[ES_NATIVE_REGIONS + 1] = {0};
    EsModification mods[ES_NATIVE_REGIONS + 1] = {0};
    char keys[ES_NATIVE_REGIONS + 1][512], labels[ES_NATIVE_REGIONS + 1][256];
    snprintf(keys[0], sizeof(keys[0]), "song:%s", j->song ? j->song : j->name);
    snprintf(labels[0], sizeof(labels[0]), DM_TEXT_M64_EXPORTED_S_SEQUENCE, j->name);
    char seq_bytes[32], font_bytes[32];
    snprintf(seq_bytes, sizeof(seq_bytes), "%llu", (unsigned long long)result->sequence_bytes);
    snprintf(font_bytes, sizeof(font_bytes), "%llu", (unsigned long long)result->soundfont_bytes);
    bool ok =
        es_recipe_string(&recipes[0], "kind", "oot-song-1") &&
        es_recipe_string(&recipes[0], "name", j->name) &&
        es_recipe_string(&recipes[0], "song", j->song ? j->song : "") &&
        es_recipe_string(&recipes[0], "enlarge", j->enlarge ? "1" : "0") &&
        es_recipe_string(&recipes[0], "keep", j->keep_originals ? "1" : "0") &&
        es_recipe_string(&recipes[0], "sequence-size", seq_bytes) &&
        es_recipe_string(&recipes[0], "font-size", font_bytes) &&
        es_recipe_add(&recipes[0], "sequence", result->sequence.data, result->sequence.size) &&
        es_recipe_add(&recipes[0], "font", result->soundfont.data, result->soundfont.size);
    size_t count = 1;
    unsigned asset = 0;

    for (size_t i = 0; ok && i < j->samples.count; i++) {
        const EsNativeRegion *r = &j->samples.regions[i];

        if (r->target) {
            size_t n = count++;
            snprintf(keys[n], sizeof(keys[n]), "sample:%s", r->target);
            snprintf(labels[n], sizeof(labels[n]), DM_TEXT_M64_REPLACED_S_SAMPLE, r->name);
            char value[96], pan[16];
            snprintf(value, sizeof(value), "%.17g", r->tuning);
            snprintf(pan, sizeof(pan), "%u", es_native_pan(r));
            ok = es_recipe_string(&recipes[n], "kind", "oot-sample-1") &&
                 es_recipe_string(&recipes[n], "path", r->target) &&
                 es_recipe_string(&recipes[n], "keep", j->keep_originals ? "1" : "0") &&
                 es_recipe_string(&recipes[n], "tuning", value) &&
                 es_recipe_add(&recipes[n], "audio", r->audio.data, r->audio.size);

            /* A centred zone sets no pan, so every stock drum keeps its own. */
            if (ok && r->params.pan != 0) {
                ok = es_recipe_string(&recipes[n], "drum-pan", pan);
            }
        } else {
            char field[64];
            snprintf(field, sizeof(field), "asset%u.name", asset);
            ok = es_recipe_string(&recipes[0], field, r->name);
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

bool oot_repository_apply(OotPluginJob *job, EsRepository *repo, const OotResult *result,
                          char *error, size_t cap) {
    return compose(job, repo, result, NULL, error, cap);
}

bool oot_repository_package(OotPluginJob *job, EsRepository *probe, const OotResult *result,
                            const DmeHost *host, char *error, size_t cap) {
    return compose(job, probe, result, host, error, cap);
}
