#include "../libraries/export_support/export_support.h"
#include "../libraries/export_support/source.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    uint32_t kind, value, memory;
} Recipe;

typedef struct {
    bool cancel;
    unsigned crash;
    const char *deny;
    void *lock;
} Boundary;

static int32_t DME_CALL canceled(void *user) {
    return ((Boundary *)user)->cancel;
}

static void DME_CALL boundary(void *user, uint32_t percent) {
    Boundary *state = user;

    if (percent == 20 && state->deny && !state->lock) {
#ifdef _WIN32
        wchar_t path[4096];
        assert(MultiByteToWideChar(CP_UTF8, 0, state->deny, -1, path, SDL_arraysize(path)));
        state->lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        assert(state->lock != INVALID_HANDLE_VALUE);
#else
        assert(!chmod(state->deny, 0555));
        state->lock = state;
#endif
    }

    if (state->crash == percent) {
        _Exit(73);
    }

    if (state->crash == 999 && percent >= 20) {
        state->cancel = true;
    }
}

static int32_t DME_CALL allow(const char *path) {
    return path && !strncmp(path, "audio/", 6);
}

static int32_t DME_CALL inventory(const DmeHistoryApi *api, DmeHistory *history,
                                  DmeHistoryList *list, char *error, uint64_t cap) {
    return api->list_directory(history, list, "audio", 1, error, cap);
}

static bool compose(EsRepository *r, const EsModifications *mods, EsPlan *plan, char *error,
                    size_t cap) {
    unsigned memory = 10;
    bool sample = false;

    for (size_t i = 0; i < mods->count; ++i) {
        const EsModification *m = &mods->items[i];
        assert(m->size == sizeof(Recipe));
        Recipe recipe;
        memcpy(&recipe, m->data, sizeof(recipe));
        char path[256], value[128];

        if (recipe.kind == 2) {
            EsFile original = {0};
            bool ok = es_repository_baseline(r, "audio/shared.wav", &original, error, cap) &&
                      es_plan_put(plan, "audio/shared.wav", original.data, original.size,
                                  original.mode, original.exists, error, cap) &&
                      es_plan_affect(plan, m->key, "audio/shared.wav", "baseline");
            es_file_free(&original);

            if (!ok) {
                return false;
            }

            continue;
        }

        if (recipe.kind == 3 || recipe.kind == 4) {
            if (!es_plan_depend(plan, "audio/shared.wav",
                                recipe.kind == 3 ? "audio/bank.cfg" : "audio/missing.wav",
                                "fixture") ||
                (recipe.kind == 3 &&
                 !es_plan_depend(plan, "audio/bank.cfg", "audio/shared.wav", "cycle"))) {
                return false;
            }
        }

        if (recipe.kind == 5) {
            for (unsigned f = 0; f < 80; f++) {
                snprintf(path, sizeof(path), "audio/bulk/%u.seq", f);

                if (!es_plan_put(plan, path, "native fixture", 14, 0, true, error, cap) ||
                    !es_plan_affect(plan, m->key, path, "sequence") ||
                    !es_plan_depend(plan, path, "audio/shared.wav", "sample")) {
                    return false;
                }
            }

            continue;
        }

        if (!recipe.kind) {
            snprintf(value, sizeof(value), "sample %u", recipe.value);
            sample = true;

            if (!es_plan_put(plan, "audio/shared.wav", value, strlen(value), 0, true, error, cap) ||
                !es_plan_backup(plan, "audio/shared.wav")) {
                return false;
            }
        } else {
            snprintf(path, sizeof(path), "audio/songs/%s.seq", m->key);
            snprintf(value, sizeof(value), "latest %u", recipe.value);

            if (!es_plan_put(plan, path, value, strlen(value), 0, true, error, cap)) {
                return false;
            }

            memory = SDL_max(memory, recipe.memory);
        }
    }

    if (mods->count) {
        char bank[128], bytes[32];
        snprintf(bank, sizeof(bank), "shared source %s", sample ? "replacement" : "original");
        snprintf(bytes, sizeof(bytes), "%u", memory);
        return es_plan_put(plan, "audio/bank.cfg", bank, strlen(bank), 0, true, error, cap) &&
               es_plan_put(plan, "audio/memory.cfg", bytes, strlen(bytes), 0, true, error, cap);
    }

    return true;
}

static int32_t DME_CALL render(const DmeHistoryApi *api, DmeHistory *history,
                               const DmeHistoryIntent *intents, uint32_t count,
                               DmeHistoryPlan *plan, char *error, uint64_t cap) {
    (void)api;
    EsModifications mods;
    bool ok = es_intents_view(intents, count, &mods) &&
              compose((EsRepository *)history, &mods, (EsPlan *)plan, error, (size_t)cap);
    es_intents_release(&mods);
    return ok;
}

static int32_t DME_CALL detect(const DmeHistoryApi *api, DmeHistoryRoot *root,
                               DmeHistoryTarget *target, char *error, uint64_t cap) {
    (void)api;
    (void)root;
    (void)error;
    (void)cap;
    *target = (DmeHistoryTarget){"sm64", "Fixture", "us", 0};
    return 1;
}

static int32_t DME_CALL install(void *job, const DmeContext *context, const DmeHost *host,
                                const DmeHistoryApi *api, DmeHistory *history, char *error,
                                uint64_t cap) {
    (void)context;
    (void)host;
    DmeHistoryIntent intent = {{"prepared", 8}, {"Prepared export", 15}, job, sizeof(Recipe)};
    return api->apply(history, &intent, 1, error, cap) ? DME_OK : DME_FAILED;
}

static const DmeHistoryAdapter adapter = {.size = sizeof(DmeHistoryAdapter),
                                          .version = DME_REPOSITORY_API,
                                          .detect = detect,
                                          .inventory = inventory,
                                          .allow = allow,
                                          .render = render,
                                          .install = install};

static void put(const char *root, const char *name, const char *value) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    assert(SDL_SaveFile(path, value, strlen(value)));
}

static void expect(EsRepository *r, const char *path, const char *value) {
    char error[512] = {0};
    EsFile file = {0};
    assert(es_repository_read(r, path, &file, error, sizeof(error)));
    bool matches = file.exists == (value != NULL) &&
                   (!value || (file.size == strlen(value) && !memcmp(file.data, value, file.size)));

    if (!matches) {
        fprintf(stderr, "%s: expected %s, found %s\n", path, value ? value : "(absent)",
                file.exists ? (const char *)file.data : "(absent)");
    }

    assert(matches);

    es_file_free(&file);
}

static EsRepository *open_repository(const char *plugins, const char *checkout, DmeHost *host) {
    char error[2048] = {0};
    EsOpen options = {.plugins = plugins,
                      .checkout = checkout,
                      .exporter = "fixture",
                      .target = "sm64",
                      .version = "us",
                      .host = host,
                      .adapter = &adapter};
    EsRepository *r = es_repository_open(&options, error, sizeof(error));

    if (!r) {
        fprintf(stderr, "%s\n", error);
    }

    assert(r);
    return r;
}

static void apply(EsRepository *r, const char *key, Recipe recipe) {
    char error[2048] = {0};
    bool ok = es_repository_apply(r, key, key, &recipe, sizeof(recipe), error, sizeof(error));

    if (!ok) {
        fprintf(stderr, "%s\n", error);
    }

    assert(ok);
}

static void revert(EsRepository *r, const char *key) {
    char error[2048] = {0};
    bool ok = es_repository_revert(r, key, error, sizeof(error));

    if (!ok) {
        fprintf(stderr, "%s\n", error);
    }

    assert(ok);
}

static void source_edits(void) {
    const char json[] =
        "{/* retain this comment */\n\"sound\":{\"sample\":\"shared\",\"tuning\":1.25},\"pan\":37}";
    char error[512] = {0};
    EsJson *doc = es_json_parse(json, sizeof(json) - 1, error, sizeof(error));
    assert(doc);
    yyjson_val *root = es_json_root(doc);
    EsEdits edits = {0};
    assert(es_json_set(doc, yyjson_obj_get(root, "sound"), "tuning", "0.625", &edits, error,
                       sizeof(error)));
    assert(es_json_set(doc, root, "song_A", "[\"bankA\"]", &edits, error, sizeof(error)));
    assert(es_json_set(doc, root, "song_B", "[\"bankB\"]", &edits, error, sizeof(error)));
    assert(es_json_rename(doc, root, "pan", "independent_pan", &edits, error, sizeof(error)));
    EsFile output = {0};
    assert(es_edits_apply(&edits, json, sizeof(json) - 1, &output, error, sizeof(error)));
    assert(strstr((const char *)output.data, "/* retain this comment */") &&
           strstr((const char *)output.data, "\"independent_pan\":37"));
    EsJson *parsed = es_json_parse(output.data, output.size, error, sizeof(error));
    assert(parsed && yyjson_get_real(yyjson_obj_get(yyjson_obj_get(es_json_root(parsed), "sound"),
                                                    "tuning")) == .625);
    es_json_free(parsed);
    es_json_free(doc);
    es_file_free(&output);
    es_edits_free(&edits);
    doc = es_json_parse("{}", 2, error, sizeof(error));
    assert(doc && es_json_set(doc, es_json_root(doc), "A", "1", &edits, error, sizeof(error)) &&
           es_json_set(doc, es_json_root(doc), "B", "2", &edits, error, sizeof(error)) &&
           es_edits_apply(&edits, "{}", 2, &output, error, sizeof(error)));
    parsed = es_json_parse(output.data, output.size, error, sizeof(error));
    assert(parsed && yyjson_obj_size(es_json_root(parsed)) == 2);
    es_json_free(parsed);
    es_json_free(doc);
    es_file_free(&output);
    es_edits_free(&edits);
    const char duplicate[] = "{\"a\":1,\"a\":2}";
    assert(!es_json_parse(duplicate, sizeof(duplicate) - 1, error, sizeof(error)));

    const char xml[] =
        "<?xml version=\"1.0\"?>\n<!-- retain XML comment -->\n<Soundfont Index='0' Pan=\"37\"><Samples /></Soundfont>";
    EsXml *tree = es_xml_parse(xml, sizeof(xml) - 1, error, sizeof(error));
    assert(tree && es_xml_count(tree) == 2);
    assert(es_xml_set_attribute(tree, 0, "Index", "38", &edits, error, sizeof(error)) &&
           es_xml_set_attribute(tree, 0, "SampleBank", "a&b", &edits, error, sizeof(error)) &&
           es_xml_append(tree, 1, "<Sample Name=\"tone\"/>", &edits, error, sizeof(error)) &&
           es_edits_apply(&edits, xml, sizeof(xml) - 1, &output, error, sizeof(error)));
    assert(strstr((char *)output.data, "Index='38'") && strstr((char *)output.data, "Pan=\"37\"") &&
           strstr((char *)output.data, "<!-- retain XML comment -->"));
    EsXml *again = es_xml_parse(output.data, output.size, error, sizeof(error));
    assert(again && es_xml_count(again) == 3 &&
           !strcmp(es_xml_attribute(es_xml_node(again, 0), "SampleBank"), "a&b"));
    es_xml_free(again);
    es_xml_free(tree);
    es_file_free(&output);
    es_edits_free(&edits);
    const char dtd[] = "<!DOCTYPE X><X/>";
    assert(!es_xml_parse(dtd, sizeof(dtd) - 1, error, sizeof(error)));
    puts("PASS source-preserving JSON/XML edits, comments, independent insertions and validation");

    const char make[] = "VERSION ?= ntsc-1.0\nrecipe:\n\tVERSION=unsupported command\n";
    EsFile makefile = {.data = (uint8_t *)make, .size = sizeof(make) - 1, .exists = true};
    char version[64] = "";
    bool found = false;
    assert(es_make_value(&makefile, "VERSION", version, &found, error, sizeof(error)) && found &&
           !strcmp(version, "ntsc-1.0"));
    const char options[] = "VERSION := pal-1.0 # explicit override\n";
    makefile.data = (uint8_t *)options;
    makefile.size = sizeof(options) - 1;
    assert(es_make_value(&makefile, "VERSION", version, &found, error, sizeof(error)) &&
           !strcmp(version, "pal-1.0"));
    const char conditional[] = "ifeq ($(REGION),us)\nVERSION=ntsc-1.0\nendif\n";
    makefile.data = (uint8_t *)conditional;
    makefile.size = sizeof(conditional) - 1;
    assert(!es_make_value(&makefile, "VERSION", version, &found, error, sizeof(error)));
    puts("PASS literal Make settings, explicit options, ignored recipes and conditional rejection");
}

/* An exporter written only against the public interface: no inventory, no namespace limit and
   a recipe that is "<path>\n<content>". Each path's baseline is captured when it is first
   written, so it can install into any folder. */
static int32_t DME_CALL overlay_detect(const DmeHistoryApi *api, DmeHistoryRoot *root,
                                       DmeHistoryTarget *target, char *error, uint64_t cap) {
    DmeHistoryFile marker;
    int32_t ok = api->root_read(root, "overlay.marker", &marker, error, cap);
    bool exists = ok && marker.exists;
    api->file_free(&marker);

    if (!exists) {
        snprintf(error, (size_t)cap, "Not an overlay folder");
        return 0;
    }

    *target = (DmeHistoryTarget){"overlay", "Overlay", "1", 0};
    return 1;
}

static int32_t DME_CALL overlay_render(const DmeHistoryApi *api, DmeHistory *history,
                                       const DmeHistoryIntent *intents, uint32_t count,
                                       DmeHistoryPlan *plan, char *error, uint64_t cap) {
    (void)history;

    for (uint32_t i = 0; i < count; i++) {
        char path[256];
        const char *text = (const char *)intents[i].recipe,
                   *newline = memchr(text, '\n', intents[i].recipe_size);

        if (!newline || newline - text >= (ptrdiff_t)sizeof(path)) {
            return 0;
        }

        memcpy(path, text, (size_t)(newline - text));
        path[newline - text] = 0;
        uint64_t size = intents[i].recipe_size - (uint64_t)(newline + 1 - text);

        if (!api->plan_put(plan, path, (const uint8_t *)newline + 1, size, 1, error, cap) ||
            !api->plan_affect(plan, intents[i].key.data, path, "file")) {
            return 0;
        }
    }

    return 1;
}

static int32_t DME_CALL overlay_install(void *job, const DmeContext *context, const DmeHost *host,
                                        const DmeHistoryApi *api, DmeHistory *history, char *error,
                                        uint64_t cap) {
    (void)context;
    (void)host;
    /* The export job is "<path>\n<key>\n<content>"; the stored recipe drops the key line. */
    const char *path = job, *key = strchr(path, '\n') + 1, *body = strchr(key, '\n') + 1;
    char name[64], recipe[512];
    snprintf(name, sizeof(name), "%.*s", (int)(body - key - 1), key);
    snprintf(recipe, sizeof(recipe), "%.*s\n%s", (int)(key - 1 - path), path, body);
    DmeHistoryIntent intent = {
        {name, strlen(name)}, {name, strlen(name)}, (const uint8_t *)recipe, strlen(recipe)};
    return api->apply(history, &intent, 1, error, cap) ? DME_OK : DME_FAILED;
}

static const DmeHistoryAdapter overlay = {.size = sizeof(DmeHistoryAdapter),
                                          .version = DME_REPOSITORY_API,
                                          .detect = overlay_detect,
                                          .render = overlay_render,
                                          .install = overlay_install};

/* Checks a file through a short-lived probe; its cached directory handles close at once. */
static void expect_in(const char *checkout, const char *path, const char *value) {
    char error[512] = {0};
    EsRepository *probe = es_repository_probe(checkout, &overlay, error, sizeof(error));
    assert(probe);
    expect(probe, path, value);
    es_repository_close(probe);
}

static int32_t overlay_action(const char *plugins, const char *checkout, uint32_t action,
                              const char *key, const char *recipe, const DmeHost *host, char *error,
                              size_t cap) {
    const DmeHistoryRuntime *runtime = duckmidi_history_runtime(DME_REPOSITORY_API);
    DmeRepositoryRequest request = {.size = sizeof(request),
                                    .action = action,
                                    .plugins_directory = {plugins, strlen(plugins)},
                                    .checkout = {checkout, strlen(checkout)},
                                    .logical_key = {key ? key : "", key ? strlen(key) : 0}};
    void *job = runtime->create_job(&overlay, "org.example.overlay", NULL, (void *)recipe, &request,
                                    error, cap);
    int32_t status = job ? runtime->prepare(job, NULL, host, error, cap) : DME_FAILED;
    status = status == DME_OK ? runtime->run(job, NULL, host, error, cap) : status;
    DmeRepositoryView view = {0};

    if (status == DME_OK && action != DME_REPOSITORY_INSTALL) {
        runtime->describe(job, &view);
        assert(view.scope_count && !strcmp(view.scopes[0].target_name.data, "Overlay"));
    }

    runtime->destroy_job(job);
    return status;
}

static void overlay_plugin(const char *root, unsigned long long now, DmeHost *host) {
    char checkout[4096], plugins[4096], path[4096], error[2048] = {0};
    snprintf(checkout, sizeof(checkout), "%s/overlay-%llu", root, now);
    snprintf(plugins, sizeof(plugins), "%s/overlay-plugins-%llu", root, now);
    snprintf(path, sizeof(path), "%s/config", checkout);
    assert(SDL_CreateDirectory(path) && SDL_CreateDirectory(plugins));
    put(checkout, "overlay.marker", "");
    put(checkout, "config/settings.txt", "original");
    assert(overlay_action(plugins, checkout, DME_REPOSITORY_INSTALL, NULL,
                          "config/settings.txt\nA\nfrom A", host, error, sizeof(error)) == DME_OK);
    assert(overlay_action(plugins, checkout, DME_REPOSITORY_INSTALL, NULL, "extra/b.txt\nB\nfrom B",
                          host, error, sizeof(error)) == DME_OK);
    expect_in(checkout, "config/settings.txt", "from A");
    expect_in(checkout, "extra/b.txt", "from B");
    assert(overlay_action(plugins, checkout, DME_REPOSITORY_REVERT, "A", NULL, host, error,
                          sizeof(error)) == DME_OK);
    expect_in(checkout, "config/settings.txt", "original");
    expect_in(checkout, "extra/b.txt", "from B");
    put(checkout, "extra/b.txt", "edited outside");
    assert(overlay_action(plugins, checkout, DME_REPOSITORY_INSTALL, NULL,
                          "config/settings.txt\nA\nagain", host, error, sizeof(error)) != DME_OK &&
           strstr(error, "extra/b.txt"));
    put(checkout, "extra/b.txt", "from B");
    int32_t reverted = overlay_action(plugins, checkout, DME_REPOSITORY_REVERT_ALL, NULL, NULL,
                                      host, error, sizeof(error));

    if (reverted != DME_OK) {
        fprintf(stderr, "overlay revert all: %s\n", error);
    }

    assert(reverted == DME_OK);
    expect_in(checkout, "extra/b.txt", NULL);
    snprintf(path, sizeof(path), "%s/extra", checkout);
    assert(!SDL_GetPathInfo(path, NULL));
    puts("PASS a public-interface exporter without an inventory gets first-touch history");
}

int main(int argc, char **argv) {
    assert(SDL_Init(0));

    if (argc == 4 && !strcmp(argv[1], "--fault")) {
        char directory[4096], error[2048] = {0};
        snprintf(directory, sizeof(directory), "%s/audio", argv[3]);
        assert(SDL_CreateDirectory(directory) && SDL_CreateDirectory(argv[2]));
        put(argv[3], "audio/shared.wav", "initialized sample");
        put(argv[3], "audio/bank.cfg", "initialized bank");
        put(argv[3], "audio/memory.cfg", "10");
        EsRepository *r = open_repository(argv[2], argv[3], NULL);
        Recipe recipe = {1, 19, 400};
        assert(!es_repository_apply(r, "disk-full", "Disk full", &recipe, sizeof(recipe), error,
                                    sizeof(error)));
        expect(r, "audio/bank.cfg", "initialized bank");
        expect(r, "audio/songs/disk-full.seq", NULL);
        es_repository_close(r);
        r = open_repository(argv[2], argv[3], NULL);
        apply(r, "disk-full", recipe);
        revert(r, NULL);
        es_repository_close(r);
        puts("PASS injected disk-full after a completed write, rollback, restart and retry");
        SDL_Quit();
        return 0;
    }

    if (argc == 5 && !strcmp(argv[1], "--crash")) {
        Boundary state = {.crash = (unsigned)atoi(argv[4])};
        DmeHost host = {.user = &state, .cancelled = canceled, .progress = boundary};
        EsRepository *r = open_repository(argv[2], argv[3], &host);
        apply(r, "interrupted", (Recipe){1, 99, 999});
        es_repository_close(r);
        return 1;
    }

    if (argc == 4 && !strcmp(argv[1], "--busy")) {
        char error[2048] = {0};
        EsOpen options = {.plugins = argv[2],
                          .checkout = argv[3],
                          .exporter = "fixture",
                          .target = "sm64",
                          .version = "us",
                          .host = NULL,
                          .adapter = &adapter};
        EsRepository *r = es_repository_open(&options, error, sizeof(error));
        bool blocked = !r && strstr(error, "physical checkout");
        es_repository_close(r);
        return blocked ? 0 : 2;
    }

    if (argc == 5 && !strcmp(argv[1], "--crash-revert")) {
        Boundary state = {.crash = (unsigned)atoi(argv[4])};
        DmeHost host = {.user = &state, .cancelled = canceled, .progress = boundary};
        EsRepository *r = open_repository(argv[2], argv[3], &host);
        revert(r, NULL);
        es_repository_close(r);
        return 1;
    }

    assert(argc == 2);
    char protected_error[512];
#ifdef _WIN32
    const char *base = SDL_GetBasePath();
    char protected_root[4] = {base[0], ':', '/', 0};
#else
    const char *protected_root = "/";
#endif
    assert(!es_root_open(protected_root, protected_error, sizeof(protected_error)));
    source_edits();
    SDL_Time now;
    assert(SDL_GetCurrentTime(&now));
    char checkout[4096], plugins[4096], path[4096], error[2048] = {0};
    snprintf(checkout, sizeof(checkout), "%s/checkout-%llu", argv[1], (unsigned long long)now);
    snprintf(plugins, sizeof(plugins), "%s/plugins-%llu", argv[1], (unsigned long long)now);
    snprintf(path, sizeof(path), "%s/audio", checkout);
    assert(SDL_CreateDirectory(path) && SDL_CreateDirectory(plugins));
    put(checkout, "audio/shared.wav", "initialized sample");
    put(checkout, "audio/bank.cfg", "initialized bank");
    put(checkout, "audio/memory.cfg", "10");
    put(checkout, "unrelated.sentinel", "outside namespace");
    Boundary state = {0};
    DmeHost host = {.user = &state, .cancelled = canceled, .progress = boundary};
    EsRepository *r = open_repository(plugins, checkout, &host);
    uint64_t generation = es_repository_generation(r);
    assert(generation);
    apply(r, "sample-X", (Recipe){0, 1, 0});
    assert(es_repository_generation(r) == generation + 1);
    apply(r, "A", (Recipe){1, 1, 50});
    apply(r, "B", (Recipe){1, 2, 80});
    apply(r, "A", (Recipe){1, 3, 40});
    apply(r, "sample-X", (Recipe){0, 2, 0});
    expect(r, "audio/shared.wav.bkp", "initialized sample");
    EsModifications mods = {0};
    assert(es_repository_modifications(r, &mods, error, sizeof(error)) && mods.count == 3);
    es_modifications_free(&mods);
    es_repository_close(r);
    r = open_repository(plugins, checkout, &host);
    revert(r, "sample-X");
    expect(r, "audio/shared.wav", "initialized sample");
    expect(r, "audio/shared.wav.bkp", NULL);
    expect(r, "audio/songs/A.seq", "latest 3");
    expect(r, "audio/songs/B.seq", "latest 2");
    expect(r, "audio/memory.cfg", "80");
    revert(r, "A");
    expect(r, "audio/songs/A.seq", NULL);
    expect(r, "audio/songs/B.seq", "latest 2");
    expect(r, "audio/memory.cfg", "80");
    revert(r, NULL);
    expect(r, "audio/bank.cfg", "initialized bank");
    expect(r, "audio/memory.cfg", "10");
    expect(r, "audio/songs/B.seq", NULL);
    snprintf(path, sizeof(path), "%s/audio/songs", checkout);
    assert(!SDL_GetPathInfo(path, NULL));
    puts("PASS current logical composition, repeated replacement, reopen and selective reverts");

    apply(r, "baseline", (Recipe){2, 0, 0});
    assert(es_repository_modifications(r, &mods, error, sizeof(error)) && !mods.count);
    es_modifications_free(&mods);
    expect(r, "audio/bank.cfg", "initialized bank");
    Recipe invalid = {3, 1, 0};
    assert(!es_repository_apply(r, "cycle", "Cycle", &invalid, sizeof(invalid), error,
                                sizeof(error)) &&
           strstr(error, "cycle"));
    invalid.kind = 4;
    assert(!es_repository_apply(r, "missing", "Missing", &invalid, sizeof(invalid), error,
                                sizeof(error)) &&
           strstr(error, "audio/missing.wav"));
    expect(r, "audio/shared.wav", "initialized sample");
    apply(r, "bulk", (Recipe){5, 0, 0});
    assert(es_repository_modifications(r, &mods, error, sizeof(error)) && mods.count == 1 &&
           mods.items[0].paths.count == 80);
    es_modifications_free(&mods);
    revert(r, "bulk");
    snprintf(path, sizeof(path), "%s/audio/bulk", checkout);
    assert(!SDL_GetPathInfo(path, NULL));
    puts("PASS baseline pruning, graph preflight, 80-file closure and created-directory cleanup");

    char second_plugins[4096];
    snprintf(second_plugins, sizeof(second_plugins), "%s/other-plugins-%llu", argv[1],
             (unsigned long long)now);
    assert(SDL_CreateDirectory(second_plugins));
    const char *busy_args[] = {argv[0], "--busy", second_plugins, checkout, NULL};
    SDL_Process *busy_child = SDL_CreateProcess(busy_args, false);
    int busy_status = -1;
    assert(busy_child && SDL_WaitProcess(busy_child, true, &busy_status) && busy_status == 0);
    SDL_DestroyProcess(busy_child);
    puts("PASS physical checkout serialization across separate plugin directories and processes");

    state.crash = 999;
    Recipe recipe = {1, 7, 900};
    assert(!es_repository_apply(r, "canceled", "Canceled", &recipe, sizeof(recipe), error,
                                sizeof(error)));
    state = (Boundary){0};
    expect(r, "audio/songs/canceled.seq", NULL);
    expect(r, "audio/bank.cfg", "initialized bank");
    assert(es_repository_modifications(r, &mods, error, sizeof(error)) && !mods.count);
    es_modifications_free(&mods);
    es_repository_close(r);
    puts(
        "PASS cancellation after durable preparation restores files and retains no committed intent");

    const unsigned crashes[] = {20, 40, 60, 80, 90, 100};

    for (size_t i = 0; i < SDL_arraysize(crashes); ++i) {
        char percent[16];
        snprintf(percent, sizeof(percent), "%u", crashes[i]);
        const char *args[] = {argv[0], "--crash", plugins, checkout, percent, NULL};
        SDL_Process *child = SDL_CreateProcess(args, false);
        int status = 0;
        assert(child && SDL_WaitProcess(child, true, &status) && status == 73);
        SDL_DestroyProcess(child);
        r = open_repository(plugins, checkout, &host);

        if (crashes[i] == 100) {
            expect(r, "audio/songs/interrupted.seq", "latest 99");
            revert(r, NULL);
        } else {
            expect(r, "audio/bank.cfg", "initialized bank");
            expect(r, "audio/songs/interrupted.seq", NULL);
        }

        es_repository_close(r);
    }

    puts(
        "PASS killed worker recovery before writes, between replacements and before ledger commit");

    const char *conflict_args[] = {argv[0], "--crash", plugins, checkout, "40", NULL};
    SDL_Process *conflict_child = SDL_CreateProcess(conflict_args, false);
    int conflict_status = 0;
    assert(conflict_child && SDL_WaitProcess(conflict_child, true, &conflict_status) &&
           conflict_status == 73);
    SDL_DestroyProcess(conflict_child);
    put(checkout, "audio/songs/interrupted.seq", "outside pending edit");
    EsOpen conflict_options = {.plugins = plugins,
                               .checkout = checkout,
                               .exporter = "fixture",
                               .target = "sm64",
                               .version = "us",
                               .host = &host,
                               .adapter = &adapter};
    assert(!es_repository_open(&conflict_options, error, sizeof(error)) &&
           strstr(error, "Recovery conflict"));
    snprintf(path, sizeof(path), "%s/audio/songs/interrupted.seq", checkout);
    size_t conflict_size = 0;
    char *conflict_bytes = SDL_LoadFile(path, &conflict_size);
    assert(conflict_bytes && conflict_size == 20 &&
           !memcmp(conflict_bytes, "outside pending edit", 20));
    SDL_free(conflict_bytes);
    put(checkout, "audio/songs/interrupted.seq", "latest 99");
    r = open_repository(plugins, checkout, &host);
    expect(r, "audio/songs/interrupted.seq", NULL);
    es_repository_close(r);
    puts(
        "PASS interrupted recovery retains unknown outside edits and succeeds after conflict resolution");

#ifdef _WIN32
    bool permissions_enforced = true;
#else
    /* Root ignores directory permissions, so a read-only directory cannot deny the write. */
    bool permissions_enforced = geteuid() != 0;
#endif

    if (permissions_enforced) {
        r = open_repository(plugins, checkout, &host);
#ifdef _WIN32
        snprintf(path, sizeof(path), "%s/audio/bank.cfg", checkout);
#else
        snprintf(path, sizeof(path), "%s/audio", checkout);
#endif
        state.deny = path;
        recipe = (Recipe){1, 13, 900};
        assert(!es_repository_apply(r, "denied", "Denied", &recipe, sizeof(recipe), error,
                                    sizeof(error)));
#ifdef _WIN32
        assert(CloseHandle(state.lock));
#else
        assert(!chmod(path, 0755));
#endif
        state = (Boundary){0};
        expect(r, "audio/bank.cfg", "initialized bank");
        expect(r, "audio/songs/denied.seq", NULL);
        es_repository_close(r);
        r = open_repository(plugins, checkout, &host);
        apply(r, "denied", recipe);
        revert(r, NULL);
        es_repository_close(r);
        puts("PASS denied native write rolls back completed files and a later retry succeeds");
    } else {
        puts("SKIP denied native write rollback: permissions are not enforced for root");
    }

    r = open_repository(plugins, checkout, &host);
    apply(r, "bulk", (Recipe){5, 0, 0});
    state.crash = 999;
    assert(!es_repository_revert(r, NULL, error, sizeof(error)));
    state = (Boundary){0};
    expect(r, "audio/bulk/79.seq", "native fixture");
    es_repository_close(r);
    const char *revert_args[] = {argv[0], "--crash-revert", plugins, checkout, "90", NULL};
    SDL_Process *revert_child = SDL_CreateProcess(revert_args, false);
    int revert_status = 0;
    assert(revert_child && SDL_WaitProcess(revert_child, true, &revert_status) &&
           revert_status == 73);
    SDL_DestroyProcess(revert_child);
    r = open_repository(plugins, checkout, &host);
    expect(r, "audio/bulk/79.seq", "native fixture");
    assert(es_repository_modifications(r, &mods, error, sizeof(error)) && mods.count == 1);
    es_modifications_free(&mods);
    revert(r, NULL);
    snprintf(path, sizeof(path), "%s/audio/bulk", checkout);
    assert(!SDL_GetPathInfo(path, NULL));
    puts(
        "PASS cancelled revert and killed revert after directory removal recover the active graph");

    put(checkout, "audio/shared.wav.bkp", "initialized sample");
    assert(es_repository_initialize(r, error, sizeof(error)));
    apply(r, "sample-X", (Recipe){0, 4, 0});
    revert(r, NULL);
    expect(r, "audio/shared.wav.bkp", "initialized sample");
    put(checkout, "audio/shared.wav.bkp", "unrelated backup");
    assert(es_repository_initialize(r, error, sizeof(error)));
    recipe = (Recipe){0, 5, 0};
    assert(!es_repository_apply(r, "conflicting-backup", "Sample", &recipe, sizeof(recipe), error,
                                sizeof(error)));
    expect(r, "audio/shared.wav", "initialized sample");
    expect(r, "audio/shared.wav.bkp", "unrelated backup");
    snprintf(path, sizeof(path), "%s/audio/shared.wav.bkp", checkout);
    assert(SDL_RemovePath(path) && es_repository_initialize(r, error, sizeof(error)));
    puts(
        "PASS matching preexisting backups survive; conflicting backups block the whole operation");

    put(checkout, "audio/shared.wav", "outside edit");
    assert(!es_repository_apply(r, "blocked", "Blocked", &recipe, sizeof(recipe), error,
                                sizeof(error)));
    assert(strstr(error, "audio/shared.wav"));
    expect(r, "audio/shared.wav", "outside edit");
    assert(es_repository_initialize(r, error, sizeof(error)));
    apply(r, "sample-X", (Recipe){0, 8, 0});
    revert(r, NULL);
    expect(r, "audio/shared.wav", "outside edit");
    es_repository_close(r);
    puts("PASS outside drift blocks mutation; explicit initialization adopts current audio");

    Recipe prepared_recipe = {1, 11, 99};
    const DmeHistoryRuntime *runtime = duckmidi_history_runtime(DME_REPOSITORY_API);
    DmeRepositoryRequest request = {.size = sizeof(request),
                                    .action = DME_REPOSITORY_INSTALL,
                                    .plugins_directory = {plugins, strlen(plugins)},
                                    .checkout = {checkout, strlen(checkout)}};
    void *bridge = runtime->create_job(&adapter, "fixture", NULL, &prepared_recipe, &request, error,
                                       sizeof(error));
    assert(bridge && runtime->prepare(bridge, NULL, &host, error, sizeof(error)) == DME_OK);
    r = open_repository(plugins, checkout, &host);
    apply(r, "concurrent", (Recipe){1, 12, 100});
    es_repository_close(r);
    assert(runtime->run(bridge, NULL, &host, error, sizeof(error)) == DME_FAILED &&
           strstr(error, "preflight"));
    runtime->destroy_job(bridge);
    r = open_repository(plugins, checkout, &host);
    expect(r, "audio/songs/prepared.seq", NULL);
    expect(r, "audio/songs/concurrent.seq", "latest 12");
    revert(r, NULL);
    es_repository_close(r);
    puts("PASS concurrent preflight generation rejection preserves the newer committed graph");
    overlay_plugin(argv[1], (unsigned long long)now, &host);
    uint8_t hash[32];
    es_sha256("abc", 3, hash);
    static const uint8_t expected[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                       0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                       0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                       0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    assert(!memcmp(hash, expected, sizeof(expected)));
    SDL_Quit();
    return 0;
}
