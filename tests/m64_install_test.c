/* Export to Repository, saved packages for each Target and the sequence memory guides, checked
   against generated SM64, HackerSM64 and sm64coopdx checkouts made of the excerpts the plugin
   reads and edits. */
#include "../libraries/m64export/plugin/m64_plugin.h"
#include "native_repository_fixture.h"
#include "repository_install.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi);

static const char *data_c =
    "#if defined(VERSION_JP) || defined(VERSION_US)\n"
    "struct AudioSessionSettings gAudioSessionPresets[18] = {\n"
    "#ifdef VERSION_JP\n"
    "    { 32000, 16, 1, 0x0800, 0x2FFF, 0x7FFF, 0x3900, 0x6000, 0x4400, 0x2A00 },\n"
    "#else\n"
    "    { 32000, 16, 1, 0x0C00, 0x2FFF, 0x7FFF, 0x3A00, 0x6D00, 0x4400, 0x2A00 },\n"
    "    { 32000, 16, 1, 0x0A00, 0x47FF, 0x7FFF, 0x3A00, 0x6D00, 0x4400, 0x2A00 },\n"
    "    { 32000, 16, 1, 0x1000, 0x2FFF, 0x7FFF, 0x3A00, 0x6D00, 0x4400, 0x2A00 },\n"
    "    { 32000, 16, 1, 0x0E00, 0x3FFF, 0x7FFF, 0x3A00, 0x6D00, 0x4400, 0x2A00 },\n"
    "    { 32000, 16, 1, 0x0C00, 0x4FFF, 0x7FFF, 0x3A00, 0x6D00, 0x4400, 0x2A00 },\n"
    "    { 32000, 16, 1, 0x0C00, 0x2FFF, 0x7FFF, 0x4000, 0x6E00, 0x3F00, 0x2A00 },\n"
    "    { 32000, 16, 1, 0x0A00, 0x47FF, 0x7FFF, 0x4100, 0x6E00, 0x4400, 0x2A80 },\n"
    "    { 32000, 20, 1, 0x0800, 0x37FF, 0x7FFF, 0x34C0, 0x6280, 0x4000, 0x1B00 },\n"
    "#endif\n"
    "    { 27000, 16, 1, 0x0800, 0x2FFF, 0x7FFF, 0x2500, 0x5500, 0x7400, 0x2400 },\n"
    "    { 27000, 16, 1, 0x0800, 0x3FFF, 0x7FFF, 0x2500, 0x5500, 0x7400, 0x2400 },\n"
    "    { 27000, 16, 1, 0x1000, 0x2FFF, 0x7FFF, 0x2500, 0x5500, 0x7400, 0x2400 },\n"
    "    { 27000, 16, 1, 0x1000, 0x3FFF, 0x7FFF, 0x2500, 0x5500, 0x7400, 0x2400 },\n"
    "    { 27000, 16, 1, 0x0C00, 0x4FFF, 0x7FFF, 0x2500, 0x5500, 0x7400, 0x2400 },\n"
    "    { 32000, 14, 1, 0x0800, 0x2FFF, 0x7FFF, 0x2500, 0x5500, 0x7400, 0x2400 },\n"
    "    { 32000, 12, 1, 0x0800, 0x2FFF, 0x7FFF, 0x2500, 0x5500, 0x7400, 0x2400 },\n"
    "    { 32000, 10, 1, 0x0800, 0x2FFF, 0x7FFF, 0x2500, 0x5500, 0x7400, 0x2400 },\n"
    "    { 32000, 8, 1, 0x0800, 0x2FFF, 0x7FFF, 0x2500, 0x5500, 0x7400, 0x2400 },\n"
    "    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }\n"
    "};\n"
    "#endif\n";

static const char *data_h = "#if defined(VERSION_EU)\n#define AUDIO_HEAP_SIZE 0x2c500\n#else\n"
                            "#define AUDIO_HEAP_SIZE 0x31150\n#endif\n";

static const char *buffers_c = "#if defined(VERSION_EU)\nALIGNED16 u8 gAudioHeap[0x3000];\n#else\n"
                               "ALIGNED16 u8 gAudioHeap[DOUBLE_SIZE_ON_64_BIT(0x31200)];\n#endif\n";

/* Registrations matching the generated native fixture's sequences and banks. */
static const char *sequences =
    "{\n    \"00_unused\": null,\n    \"01_unused\": null,\n"
    "    \"02_menu_title_screen\": [\"11\"],\n    \"03_custom_worker\": [\"22\"],\n"
    "    \"04_level_inside_castle\": [\"0E\"]\n}\n";

typedef struct {
    const char *path, *data;
} File;

static File sm64[] = {{"sound/sequences.json", NULL},
                      {"src/audio/data.c", NULL},
                      {"src/audio/data.h", NULL},
                      {"src/buffers/buffers.c", NULL}};
static File hacker[] = {{"sound/sequences.json", NULL},
                        {"include/config/config_audio.h", "#define PERSISTENT_CAP_MUSIC\n"},
                        {"src/audio/data.h", NULL}};
/* sm64coopdx keeps a stock-style sequences.json that its build does not use. */
static File coopdx[] = {
    {"sound/sequences.json", NULL},
    {"src/pc/lua/utils/smlua_audio_utils.c", "void smlua_audio_utils_reset_all(void);\n"}};
static char artifacts[8][128];
static char *artifact_data[8];
static int artifact_count;
static char warning[512];
static char pool_warning[512]; /* The stock sequence pool warning, when shown. */

static int32_t DME_CALL cancelled(void *user) {
    (void)user;
    return 0;
}

static void DME_CALL progress(void *user, uint32_t percent) {
    (void)user;
    (void)percent;
}

static void DME_CALL diagnostic(void *user, uint32_t severity, uint32_t f, const char *message) {
    (void)user;

    if (severity == DME_WARNING && (f & DME_POPUP) && strstr(message, "checkout")) {
        snprintf(warning, sizeof(warning), "%s", message);
    }

    if (strstr(message, "temporary sequence memory")) {
        snprintf(pool_warning, sizeof(pool_warning), "%s", message);
    }
}

static const DmeField *ui_field(const DmeUi *ui, const char *id, int *index) {
    for (uint32_t i = 0; i < ui->count; i++) {
        if (!strcmp(ui->fields[i].id, id)) {
            *index = (int)i;
            return &ui->fields[i];
        }
    }

    return NULL;
}

static int32_t DME_CALL artifact(void *user, const char *name, const uint8_t *data, uint64_t size) {
    (void)user;
    assert(artifact_count < 8);
    snprintf(artifacts[artifact_count], 128, "%s", name);
    artifact_data[artifact_count] = calloc(size + 1, 1);
    memcpy(artifact_data[artifact_count++], data, size);
    return 1;
}

static const char *artifact_named(const char *name) {
    for (int i = 0; i < artifact_count; ++i) {
        if (!strcmp(artifacts[i], name)) {
            return artifact_data[i];
        }
    }

    return NULL;
}

static void reset_output(void) {
    for (int i = 0; i < artifact_count; ++i) {
        free(artifact_data[i]);
    }

    artifact_count = 0;
    warning[0] = 0;
}

/* A generated checkout of its own, with the native fixture under the listed excerpts, and an
   empty plugins folder for its history. */
static void make_checkout(char checkout[512], char plugins[600], const char *name,
                          const File *files, size_t count) {
    scenario_folder(checkout, 512, "m64-install", name);
    native_repository_fixture(checkout, false);

    for (size_t i = 0; i < count; i++) {
        checkout_write(checkout, files[i].path, files[i].data);
    }

    snprintf(plugins, 600, "%s-plugins", checkout);
    assert(SDL_CreateDirectory(plugins));
}

static bool same_file(const char *checkout, const char *path, const char *expected) {
    char *actual = checkout_read(checkout, path);
    bool same = actual && expected && !strcmp(actual, expected);
    SDL_free(actual);
    return same || (!actual && !expected);
}

static bool present(const char *checkout, const char *path) {
    char *data = checkout_read(checkout, path);
    SDL_free(data);
    return data != NULL;
}

static void install(const DmeExporter *exporter, void *settings, const DmeContext *context,
                    const DmeHost *host, const char *plugins, const char *checkout) {
    char error[2048] = {0};
    int32_t status = repository_install(exporter, settings, context, host, plugins, checkout, error,
                                        sizeof(error));

    if (status != DME_OK) {
        fprintf(stderr, "install: %s\n", error);
    }

    assert(status == DME_OK);
}

int main(void) {
    assert(SDL_Init(0));
    assert(SDL_CreateDirectory("agents/reports/m64-install"));
    sm64[0].data = sequences;
    sm64[1].data = data_c;
    sm64[2].data = data_h;
    sm64[3].data = buffers_c;
    hacker[0].data = sequences;
    coopdx[0].data = sequences;

    /* Memory plans: a small song needs nothing; a larger one grows the heap by the difference
       from the smallest stock pool; one beyond the free RAM does not fit. */
    M64MemoryPlan small = m64_memory_plan(0x3000), large = m64_memory_plan(0xC350);
    M64MemoryPlan larger = m64_memory_plan(0xDF80), huge = m64_memory_plan(0x20000);
    assert(!small.needed && small.fits);
    assert(large.needed && large.pool == 0xC400 && large.growth == 0xC400 - 0x3F00 &&
           large.heap == 0x31150 + large.growth && large.buffer == 0x31200 + large.growth &&
           large.low_rate == 0xC400);
    assert(larger.pool == 0xE000 && larger.growth == 0xE000 - 0x3F00);
    assert(huge.needed && !huge.fits);
    char *guide = m64_memory_guide(0xC350, "03_level_grass");
    assert(guide && strstr(guide, "Enlarge: the smallest change") && strstr(guide, "console") &&
           strstr(guide, "#define AUDIO_HEAP_SIZE 0x39650") && !strstr(guide, "HackerSM64"));
    free(guide);
    guide = m64_hacker_memory_guide(0xC350, "03_level_grass");
    assert(guide && strstr(guide, "TEMPORARY_SEQ_MEM to at least 0xC400") &&
           !strstr(guide, "AUDIO_HEAP_SIZE 0x"));
    free(guide);

    /* Patching is repeatable: a second pass with another size rewrites the same lines. */
    char error[512] = {0};
    M64Text first = {0}, second = {0};
    assert(m64_patch_data_c((const uint8_t *)data_c, strlen(data_c), &large, &first, error,
                            sizeof(error)));
    assert(strstr(first.data, "0x6D00, 0xC400, 0x2A00") &&
           strstr(first.data, "#define DUCKMIDI_US_LOW_RATE_SEQ_MEM 0xC400") &&
           strstr(first.data, "0x5500, DUCKMIDI_US_LOW_RATE_SEQ_MEM, 0x2400") &&
           strstr(first.data, "0x3900, 0x6000, 0x4400, 0x2A00"));
    assert(m64_patch_data_c((const uint8_t *)first.data, first.size, &larger, &second, error,
                            sizeof(error)));
    assert(strstr(second.data, "#define DUCKMIDI_US_LOW_RATE_SEQ_MEM 0xE000") &&
           !strstr(second.data, "0xC400"));
    assert(strstr(second.data, "{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }"));
    m64_text_free(&first);
    m64_text_free(&second);
    assert(m64_patch_data_h((const uint8_t *)data_h, strlen(data_h), &large, &first, error,
                            sizeof(error)));
    assert(m64_patch_data_h((const uint8_t *)first.data, first.size, &larger, &second, error,
                            sizeof(error)));
    assert(strstr(second.data, "#define AUDIO_HEAP_SIZE 0x3B250") &&
           strstr(second.data, "#define AUDIO_HEAP_SIZE 0x31150") &&
           strstr(second.data, "#define AUDIO_HEAP_SIZE 0x2c500"));
    m64_text_free(&first);
    m64_text_free(&second);
    assert(m64_patch_buffers_c((const uint8_t *)buffers_c, strlen(buffers_c), &large, &first, error,
                               sizeof(error)));
    assert(m64_patch_buffers_c((const uint8_t *)first.data, first.size, &larger, &second, error,
                               sizeof(error)));
    assert(strstr(second.data, "DOUBLE_SIZE_ON_64_BIT(0x3B300)") &&
           strstr(second.data, "DOUBLE_SIZE_ON_64_BIT(0x31200)"));
    m64_text_free(&first);
    m64_text_free(&second);
    assert(
        !m64_patch_data_h((const uint8_t *)"nothing\n", 8, &large, &first, error, sizeof(error)) &&
        strstr(error, "sequence-memory.txt"));
    m64_text_free(&first);

    /* HackerSM64: object-form sequence entries and one TEMPORARY_SEQ_MEM per branch. */
    const char *hacker_sequences =
        "{\n    \"22_cutscene_lakitu\": {\"ifdef\": [\"VERSION_US\"], \"banks\": [\"1B\"]},\n}\n";
    assert(m64_patch_sequences((const uint8_t *)hacker_sequences, strlen(hacker_sequences),
                               "22_cutscene_lakitu", "26_custom", &first, error, sizeof(error)));
    assert(strstr(first.data, "{\"ifdef\": [\"VERSION_US\"], \"banks\": [\"26_custom\"]},"));
    m64_text_free(&first);
    const char *hacker_h = "#ifdef EXPAND_AUDIO_HEAP\n#define TEMPORARY_SEQ_MEM 0xE800\n#else\n"
                           "#define TEMPORARY_SEQ_MEM 0x7400\n#endif\n";
    const char *config = "#define PERSISTENT_CAP_MUSIC\n// #define BETTER_REVERB\n";
    bool changed = true;
    assert(!m64_hacker_expanded((const uint8_t *)config, strlen(config)));
    assert(m64_patch_hacker_data_h((const uint8_t *)hacker_h, strlen(hacker_h), 0x9010, true,
                                   &first, &changed, error, sizeof(error)) &&
           !changed);
    m64_text_free(&first);
    assert(m64_patch_hacker_data_h((const uint8_t *)hacker_h, strlen(hacker_h), 0x9010, false,
                                   &first, &changed, error, sizeof(error)) &&
           changed && strstr(first.data, "#define TEMPORARY_SEQ_MEM 0x9100") &&
           strstr(first.data, "#define TEMPORARY_SEQ_MEM 0xE800"));
    m64_text_free(&first);

    /* Colored ports: a melodic port moves to a free channel; drum ports stay on channel 10 and
       interfere only when they send different values for the same control. */
    DmeEvent ported[] = {{0, 1, 0x90, 60, 100, 0}, {0, 2, 0x91, 60, 100, 2},
                         {0, 3, 0x99, 36, 100, 0}, {0, 4, 0x99, 38, 100, 1},
                         {10, 5, 0xE9, 0, 64, 0},  {10, 6, 0xE9, 0, 64, 1},
                         {20, 7, 0xB9, 7, 100, 0}, {20, 8, 0xB9, 7, 90, 1}};
    M64PortLayout layout;
    assert(m64_port_layout(ported, 8, 1u << MIDI_DRUM_CHANNEL, &layout));
    assert(layout.out[0][0] == 0 && layout.out[1][2] == 1 && layout.out[9][1] == 9);
    assert(layout.merged && layout.interference && layout.conflict_count == 1 &&
           strstr(layout.notice, "controller 7") && !strstr(layout.notice, "pitch bend"));
    DmeEvent moved[] = {{0, 1, 0x90, 60, 100, 0}, {0, 2, 0x90, 64, 100, 1}};
    assert(m64_port_layout(moved, 2, 1u << MIDI_DRUM_CHANNEL, &layout) && !layout.merged &&
           layout.out[0][1] == 1 &&
           strstr(layout.notice, "Port 1 of channel 1 plays on channel 2."));

    /* Percussion parts: channel 11 plays on the drum channel with channel 10 without a notice;
       a melodic channel 10 moves to a free channel. */
    DmeEvent kits[] = {{0, 1, 0xC9, 0, 0, 0},
                       {0, 2, 0xCA, 25, 0, 0},
                       {0, 3, 0x99, 36, 100, 0},
                       {0, 4, 0x9A, 38, 100, 0}};
    uint32_t percussion = 1u << MIDI_DRUM_CHANNEL | 1u << 10;
    assert(m64_port_layout(kits, 4, percussion, &layout) && layout.out[10][0] == 9 &&
           layout.out[9][0] == 9 && !layout.interference && !layout.merged && !*layout.notice);
    assert(m64_port_layout(kits, 4, 1u << 10, &layout) && layout.out[10][0] == 9 &&
           layout.out[9][0] == 0 &&
           strstr(layout.notice, "Port 0 of channel 10 plays on channel 1."));

    /* Export to Repository through the history runtime: the song replaces 03_custom_worker and
       its bank entry, appending a bank after the initialized ones; audio memory stays. */
    const DmeExporter *exporter = duckmidi_export_plugin(DME_ABI)->exporter(0);
    assert(exporter->history && !strcmp(exporter->install_label, "Export to Repository"));
    void *settings = exporter->create();
    DmeEvent events[] = {{0, 1, 0x90, 60, 100, 0}, {960, 2, 0x80, 60, 0, 0}};
    DmeContext context = {.events = events,
                          .event_count = 2,
                          .duration = 960,
                          .ppq = 960,
                          .percussion_channels = 1u << MIDI_DRUM_CHANNEL};
    DmeHost host = {.cancelled = cancelled, .progress = progress, .diagnostic = diagnostic};
    char checkout[512], plugins[600];
    make_checkout(checkout, plugins, "sm64", sm64, 4);
    install(exporter, settings, &context, &host, plugins, checkout);
    assert(!*warning);
    char *entry = checkout_read(checkout, "sound/sequences.json");
    assert(entry && strstr(entry, "\"03_level_grass\": [\"26_custom_duckmidi\"]") &&
           strstr(entry, "\"02_menu_title_screen\": [\"11\"],"));
    SDL_free(entry);
    assert(present(checkout, "sound/sequences/us/03_level_grass.m64") &&
           !present(checkout, "sound/sequences/us/03_custom_worker.m64"));
    assert(same_file(checkout, "src/audio/data.c", data_c));

    /* Revert all restores every initialized file and removes what the export added. */
    char error_text[2048] = {0};
    assert(repository_revert_all(exporter, settings, &host, plugins, checkout, error_text,
                                 sizeof(error_text)) == DME_OK);
    assert(same_file(checkout, "sound/sequences.json", sequences) &&
           present(checkout, "sound/sequences/us/03_custom_worker.m64") &&
           !present(checkout, "sound/sequences/us/03_level_grass.m64"));

    /* History off: a repeated export rewrites its own entry and bank instead of adding more, and
       the first originals stay in .bkp files. */
    char direct[512], direct_plugins[600];
    make_checkout(direct, direct_plugins, "sm64-untracked", sm64, 4);
    char *paths[2][3] = {{0}};

    for (int pass = 0; pass < 2; pass++) {
        int32_t status =
            repository_install_untracked(exporter, settings, &context, &host, direct_plugins,
                                         direct, error_text, sizeof(error_text));

        if (status != DME_OK) {
            fprintf(stderr, "untracked install: %s\n", error_text);
        }

        assert(status == DME_OK);
        paths[pass][0] = checkout_read(direct, "sound/sequences.json");
        paths[pass][1] = checkout_read(direct, "sound/sound_banks/26_custom_duckmidi.json");
        paths[pass][2] = checkout_read(direct, "sound/sequences.json.bkp");
        assert(paths[pass][0] && paths[pass][1] && paths[pass][2] &&
               !strcmp(paths[pass][2], sequences));
    }

    assert(!strcmp(paths[0][0], paths[1][0]) && !strcmp(paths[0][1], paths[1][1]) &&
           strstr(paths[1][0], "\"03_level_grass\": [\"26_custom_duckmidi\"]") &&
           !present(direct, "sound/sound_banks/26_custom_duckmidi_duckmidi.json") &&
           !present(direct, "sound/sound_banks/duckmidi.json") &&
           present(direct, "sound/sequences/us/03_level_grass.m64") &&
           present(direct, "sound/sequences/us/03_custom_worker.m64.bkp"));

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 3; i++) {
            SDL_free(paths[pass][i]);
        }
    }

    char database[700];
    snprintf(database, sizeof(database), "%s/history/repositories.sqlite3", direct_plugins);
    assert(!SDL_GetPathInfo(database, NULL));

    /* A custom song name takes the next free sequence ID. */
    DmeValue custom = {DME_CHOICE, 35, NULL}, name = {DME_TEXT, 0, "my_song"};
    assert(exporter->set(settings, "sequence", &custom, error, sizeof(error)) == DME_OK &&
           exporter->set(settings, "sequence_name", &name, error, sizeof(error)) == DME_OK);
    install(exporter, settings, &context, &host, plugins, checkout);
    entry = checkout_read(checkout, "sound/sequences.json");
    assert(entry && strstr(entry, "\"05_my_song\"") &&
           present(checkout, "sound/sequences/us/05_my_song.m64"));
    SDL_free(entry);

    /* sm64coopdx checkouts are detected by their Lua audio utilities. The song becomes a mod
       in mods/, which the build copies; the unused sequences.json and audio memory stay as they
       are. A Target other than the checkout's game is reported, and the checkout wins. */
    DmeValue grass = {DME_CHOICE, 3, NULL};
    assert(exporter->set(settings, "sequence", &grass, error, sizeof(error)) == DME_OK);
    make_checkout(checkout, plugins, "coopdx", coopdx, 2);
    reset_output();
    install(exporter, settings, &context, &host, plugins, checkout);
    assert(strstr(warning, "sm64coopdx checkout"));
    char *lua = checkout_read(checkout, "mods/duckmidi_03_level_grass/main.lua");
    assert(lua && strstr(lua, "-- name: duckmidi 03_level_grass\n") &&
           strstr(lua, "smlua_audio_utils_replace_sequence(0x03, 0x2A, 75, \"03_level_grass\")"));
    SDL_free(lua);
    assert(present(checkout, "mods/duckmidi_03_level_grass/sound/03_level_grass.m64") &&
           same_file(checkout, "sound/sequences.json", sequences));

    /* A song that replaces no stock sequence gets a sequence allocated by the mod. */
    reset_output();
    DmeValue sm64coopdx = {DME_CHOICE, 2, NULL}, coop_name = {DME_TEXT, 0, "99_custom_song"};
    assert(exporter->set(settings, "target", &sm64coopdx, error, sizeof(error)) == DME_OK &&
           exporter->set(settings, "sequence", &custom, error, sizeof(error)) == DME_OK &&
           exporter->set(settings, "sequence_name", &coop_name, error, sizeof(error)) == DME_OK);
    install(exporter, settings, &context, &host, plugins, checkout);
    assert(!*warning);
    lua = checkout_read(checkout, "mods/duckmidi_99_custom_song/main.lua");
    assert(lua && strstr(lua, "SEQ_99_CUSTOM_SONG = smlua_audio_utils_allocate_sequence()") &&
           strstr(lua, "replace_sequence(SEQ_99_CUSTOM_SONG, 0x2A, 75, \"99_custom_song\")"));
    SDL_free(lua);

    /* HackerSM64 is still detected, ahead of the stock layout; a small song keeps its pool. */
    assert(exporter->set(settings, "sequence", &grass, error, sizeof(error)) == DME_OK);
    hacker[2].data = hacker_h;
    make_checkout(checkout, plugins, "hacker", hacker, 3);
    reset_output();
    install(exporter, settings, &context, &host, plugins, checkout);
    assert(strstr(warning, "HackerSM64 checkout"));
    assert(present(checkout, "sound/sequences/us/03_level_grass.m64") &&
           same_file(checkout, "src/audio/data.h", hacker_h));

    /* Saved packages follow Target: file layout, names and the manifest's game. */
    const char *const sm64_files[] = {"03_level_grass.m64", "26_custom_duckmidi.json",
                                      "sequences-entry.json", "manifest.json"};
    const char *const coopdx_files[] = {"duckmidi_03_level_grass/main.lua",
                                        "duckmidi_03_level_grass/sound/03_level_grass.m64",
                                        "manifest.json"};
    const char *const games[] = {"\"target\": \"n64decomp/sm64 US\"", "\"target\": \"HackerSM64\"",
                                 "\"target\": \"sm64coopdx\""};
    host.artifact = artifact;

    for (int game = 0; game < 3; ++game) {
        DmeValue target = {DME_CHOICE, game, NULL};
        assert(exporter->set(settings, "target", &target, error, sizeof(error)) == DME_OK);
        reset_output();
        void *job = exporter->create_job(settings, error, sizeof(error));
        assert(job && exporter->run(job, &context, &host, error, sizeof(error)) == DME_OK);
        int expected = game == 2 ? 3 : 4;
        assert(artifact_count == expected);

        for (int i = 0; i < expected; ++i) {
            assert(artifact_named(game == 2 ? coopdx_files[i] : sm64_files[i]));
        }

        assert(strstr(artifact_named("manifest.json"), games[game]));

        if (game < 2) {
            assert(!strcmp(artifact_named("sequences-entry.json"),
                           "{\"03_level_grass\": [\"26_custom_duckmidi\"]}\n"));
        } else {
            assert(strstr(artifact_named(coopdx_files[0]), "replace_sequence(0x03, 0x2A, 75"));
        }

        exporter->destroy_job(job);
    }

    /* Sequence memory: Enlarge and Stock, then Loop song; HackerSM64 offers the same choices. */
    DmeUi ui;
    int at_memory = -1, at_loop = -1;
    DmeValue sm64_target = {DME_CHOICE, 0, NULL}, hacker_target = {DME_CHOICE, 1, NULL};
    assert(exporter->set(settings, "target", &sm64_target, error, sizeof(error)) == DME_OK &&
           exporter->describe(settings, &ui) == DME_OK);
    const DmeField *memory = ui_field(&ui, "memory", &at_memory);
    assert(memory && memory->choice_count == 2 && !strcmp(memory->choices[0].label, "Enlarge") &&
           !strcmp(memory->choices[1].label, "Stock"));
    assert(!strcmp(ui_field(&ui, "loop", &at_loop)->label, "Loop song") &&
           at_loop == at_memory + 1);
    assert(exporter->set(settings, "target", &hacker_target, error, sizeof(error)) == DME_OK &&
           exporter->describe(settings, &ui) == DME_OK);
    memory = ui_field(&ui, "memory", &at_memory);
    assert(memory && memory->choice_count == 2 && !strcmp(memory->choices[0].label, "Enlarge"));
    assert(exporter->set(settings, "target", &sm64_target, error, sizeof(error)) == DME_OK);

    /* A large song: Enlarge edits the memory files without the pool warning; Stock writes only
       the song and its entries, and warns. */
    enum { NOTES = 8000 };

    DmeEvent *big = calloc(NOTES * 2, sizeof(*big));
    uint32_t seed = 1;
    assert(big);

    for (uint32_t i = 0; i < NOTES; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t key = 40 + (seed >> 16) % 40, ch = (seed >> 8) % 8;
        big[i * 2] = (DmeEvent){(uint64_t)i * 37, i * 2 + 1, 0x90 | ch, key, 100, 0};
        big[i * 2 + 1] = (DmeEvent){(uint64_t)i * 37 + 30, i * 2 + 2, 0x80 | ch, key, 0, 0};
    }

    DmeContext large_song = {.events = big,
                             .event_count = NOTES * 2,
                             .duration = NOTES * 37,
                             .ppq = 96,
                             .percussion_channels = 1u << MIDI_DRUM_CHANNEL};
    make_checkout(checkout, plugins, "memory", sm64, 4);
    host.artifact = NULL;
    reset_output();
    pool_warning[0] = 0;
    assert(exporter->inspect(settings, &large_song, error, sizeof(error)) == DME_OK);
    install(exporter, settings, &large_song, &host, plugins, checkout);
    assert(!same_file(checkout, "src/audio/data.c", data_c) && !*pool_warning);

    /* Re-exporting with Stock composes the song again, and the memory files return to their
       initialized text because no current export needs more. */
    DmeValue stock = {DME_CHOICE, M64_MEMORY_STOCK, NULL};
    assert(exporter->set(settings, "memory", &stock, error, sizeof(error)) == DME_OK);
    reset_output();
    install(exporter, settings, &large_song, &host, plugins, checkout);
    assert(same_file(checkout, "src/audio/data.c", data_c) &&
           same_file(checkout, "src/audio/data.h", data_h) &&
           same_file(checkout, "src/buffers/buffers.c", buffers_c) &&
           present(checkout, "sound/sequences/us/03_level_grass.m64") && *pool_warning);

    /* Similar instruments: other stock samples, each one a choice of the mapping. */
    assert(exporter->describe(settings, &ui) == DME_OK);
    const DmeField *mapping = ui_field(&ui, "mapping", &at_memory);
    assert(mapping && mapping->row_count);

    for (uint32_t r = 0; r < mapping->row_count; r++) {
        const DmeMappingRow *row = &mapping->rows[r];
        assert(row->similar_count <= 8);

        for (uint32_t i = 0; i < row->similar_count; i++) {
            bool listed = false;

            for (uint32_t j = 0; j < mapping->choice_count; j++) {
                listed |= mapping->choices[j].value == row->similar[i].value;
            }

            assert(listed && row->similar[i].value != row->default_value);
        }
    }

    /* The list follows the default mapping, so choosing from it leaves it unchanged. */
    const DmeMappingRow *top = &mapping->rows[0];
    uint32_t similar_count = top->similar_count;
    int similar[8];
    assert(similar_count > 0);

    for (uint32_t i = 0; i < similar_count; i++) {
        similar[i] = top->similar[i].value;
    }

    DmeValue pick = {DME_CHOICE, similar[similar_count - 1], NULL};
    assert(exporter->set(settings, top->id, &pick, error, sizeof(error)) == DME_OK &&
           exporter->describe(settings, &ui) == DME_OK);
    top = &ui_field(&ui, "mapping", &at_memory)->rows[0];
    assert(top->value == pick.integer && top->similar_count == similar_count);

    for (uint32_t i = 0; i < similar_count; i++) {
        assert(top->similar[i].value == similar[i]);
    }
    free(big);
    reset_output();
    exporter->destroy(settings);

    /* Custom offers the project's name, or the MIDI file's name without a project name. */
    settings = exporter->create();
    DmeContext named = {.events = events,
                        .event_count = 2,
                        .duration = 960,
                        .ppq = 960,
                        .project_session = 1,
                        .percussion_channels = 1u << MIDI_DRUM_CHANNEL,
                        .project_name = "My Tune!",
                        .source_name = ""};
    DmeValue custom_choice = {DME_CHOICE, 35, NULL};
    assert(exporter->inspect(settings, &named, error, sizeof(error)) == DME_OK &&
           exporter->set(settings, "sequence", &custom_choice, error, sizeof(error)) == DME_OK &&
           exporter->describe(settings, &ui) == DME_OK);
    assert(!strcmp(ui_field(&ui, "sequence_name", &at_memory)->value.text, "my_tune"));
    named.project_name = "";
    named.source_name = "C:/midi/Boss Theme.mid";
    named.project_session = 2;
    assert(exporter->inspect(settings, &named, error, sizeof(error)) == DME_OK &&
           exporter->describe(settings, &ui) == DME_OK);
    assert(!strcmp(ui_field(&ui, "sequence_name", &at_memory)->value.text, "boss_theme"));
    exporter->destroy(settings);
    puts("PASS M64 repository install and sequence memory guide");
    return 0;
}
