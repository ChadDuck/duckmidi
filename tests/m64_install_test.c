/* Export to Repository and the sequence memory guide, checked against a generated checkout made of
   the stock SM64 US excerpts the plugin edits. */
#include "../libraries/m64export/plugin/m64_plugin.h"
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
    "};\n"
    "#endif\n";

static const char *data_h = "#if defined(VERSION_EU)\n#define AUDIO_HEAP_SIZE 0x2c500\n#else\n"
                            "#define AUDIO_HEAP_SIZE 0x31150\n#endif\n";

static const char *buffers_c = "#if defined(VERSION_EU)\nALIGNED16 u8 gAudioHeap[0x3000];\n#else\n"
                               "ALIGNED16 u8 gAudioHeap[DOUBLE_SIZE_ON_64_BIT(0x31200)];\n#endif\n";

static const char *sequences =
    "{\n    \"02_menu_title_screen\": [\"11\"],\n    \"03_level_grass\": [\"22\"],\n"
    "    \"04_level_inside_castle\": [\"0E\"]\n}\n";

typedef struct {
    const char *path, *data;
} File;

static File checkout[] = {{"sound/sequences.json", NULL},
                          {"src/audio/data.c", NULL},
                          {"src/audio/data.h", NULL},
                          {"src/buffers/buffers.c", NULL}};
static char installed[8][128];
static char *contents[8];
static uint32_t flags[8];
static int installed_count;

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
    (void)severity;
    (void)f;
    (void)message;
}

static int32_t DME_CALL read_file(void *user, const char *path, const uint8_t **data,
                                  uint64_t *size) {
    (void)user;

    for (size_t i = 0; i < sizeof(checkout) / sizeof(*checkout); ++i) {
        if (!strcmp(checkout[i].path, path)) {
            *data = (const uint8_t *)checkout[i].data;
            *size = strlen(checkout[i].data);
            return 1;
        }
    }

    return 0;
}

static int32_t DME_CALL install_file(void *user, const char *path, const uint8_t *data,
                                     uint64_t size, uint32_t f) {
    (void)user;
    assert(installed_count < 8);
    snprintf(installed[installed_count], 128, "%s", path);
    contents[installed_count] = calloc(size + 1, 1);
    memcpy(contents[installed_count], data, size);
    flags[installed_count++] = f;
    return 1;
}

static const char *installed_file(const char *path, uint32_t *f) {
    for (int i = 0; i < installed_count; ++i) {
        if (!strcmp(installed[i], path)) {
            *f = flags[i];
            return contents[i];
        }
    }

    return NULL;
}

int main(void) {
    checkout[0].data = sequences;
    checkout[1].data = data_c;
    checkout[2].data = data_h;
    checkout[3].data = buffers_c;

    /* Memory plans: a small song needs nothing; a larger one grows the heap by the difference
       from the smallest stock pool; Emulator Only uses all the free RAM. */
    M64MemoryPlan small = m64_memory_plan(0x3000, false), large = m64_memory_plan(0xC350, false);
    M64MemoryPlan maximum = m64_memory_plan(0x3000, true), huge = m64_memory_plan(0x20000, false);
    assert(!small.needed && small.fits);
    assert(large.needed && large.pool == 0xC400 && large.growth == 0xC400 - 0x3F00 &&
           large.heap == 0x31150 + large.growth && large.buffer == 0x31200 + large.growth &&
           large.low_rate == 0xC400);
    assert(maximum.pool == 0x3F00 + 0x114A0 && maximum.growth == 0x114A0);
    assert(huge.needed && !huge.fits);
    char *guide = m64_memory_guide(0xC350, "03_level_grass");
    assert(guide && strstr(guide, "Console-Compatible") && strstr(guide, "Emulator Only") &&
           strstr(guide, "#define AUDIO_HEAP_SIZE 0x39650") && strstr(guide, "0x153A0"));
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
    assert(m64_patch_data_c((const uint8_t *)first.data, first.size, &maximum, &second, error,
                            sizeof(error)));
    assert(strstr(second.data, "#define DUCKMIDI_US_LOW_RATE_SEQ_MEM 0x153A0") &&
           !strstr(second.data, "0xC400"));
    m64_text_free(&first);
    m64_text_free(&second);
    assert(m64_patch_data_h((const uint8_t *)data_h, strlen(data_h), &large, &first, error,
                            sizeof(error)));
    assert(m64_patch_data_h((const uint8_t *)first.data, first.size, &maximum, &second, error,
                            sizeof(error)));
    assert(strstr(second.data, "#define AUDIO_HEAP_SIZE 0x425F0") &&
           strstr(second.data, "#define AUDIO_HEAP_SIZE 0x31150") &&
           strstr(second.data, "#define AUDIO_HEAP_SIZE 0x2c500"));
    m64_text_free(&first);
    m64_text_free(&second);
    assert(m64_patch_buffers_c((const uint8_t *)buffers_c, strlen(buffers_c), &large, &first, error,
                               sizeof(error)));
    assert(m64_patch_buffers_c((const uint8_t *)first.data, first.size, &maximum, &second, error,
                               sizeof(error)));
    assert(strstr(second.data, "DOUBLE_SIZE_ON_64_BIT(0x426A0)") &&
           strstr(second.data, "DOUBLE_SIZE_ON_64_BIT(0x31200)"));
    m64_text_free(&first);
    m64_text_free(&second);
    assert(
        !m64_patch_data_h((const uint8_t *)"nothing\n", 8, &large, &first, error, sizeof(error)) &&
        strstr(error, "sequence-memory.txt"));
    m64_text_free(&first);

    /* Install through the plugin: the song replaces 03_level_grass and its bank entry. */
    const DmeExporter *exporter = duckmidi_export_plugin(DME_ABI)->exporter(0);
    assert(exporter->install && !strcmp(exporter->install_label, "Export to Repository"));
    void *settings = exporter->create();
    DmeEvent events[] = {{0, 1, 0x90, 60, 100, 0}, {960, 2, 0x80, 60, 0, 0}};
    DmeContext context = {.events = events, .event_count = 2, .duration = 960, .ppq = 960};
    void *job = exporter->create_job(settings, error, sizeof(error));
    DmeHost host = {NULL, cancelled, progress, diagnostic, NULL, read_file, install_file};
    assert(job && exporter->install(job, &context, &host, error, sizeof(error)) == DME_OK);
    uint32_t f = 0;
    const char *entry = installed_file("sound/sequences.json", &f);
    assert(entry && strstr(entry, "\"03_level_grass\": [\"26_custom_duckmidi\"],") &&
           strstr(entry, "\"02_menu_title_screen\": [\"11\"],"));
    assert(installed_file("sound/sequences/us/03_level_grass.m64", &f) &&
           installed_file("sound/sound_banks/26_custom_duckmidi.json", &f));
    assert(!installed_file("src/audio/data.c", &f));
    exporter->destroy_job(job);

    /* A song name missing from the table is refused before anything is queued. */
    installed_count = 0;
    DmeValue custom = {DME_CHOICE, 35, NULL}, name = {DME_TEXT, 0, "99_custom_song"};
    assert(exporter->set(settings, "sequence", &custom, error, sizeof(error)) == DME_OK &&
           exporter->set(settings, "sequence_name", &name, error, sizeof(error)) == DME_OK);
    job = exporter->create_job(settings, error, sizeof(error));
    assert(exporter->install(job, &context, &host, error, sizeof(error)) == DME_INVALID &&
           strstr(error, "99_custom_song") && !installed_count);
    exporter->destroy_job(job);
    exporter->destroy(settings);
    puts("PASS M64 repository install and sequence memory guide");
    return 0;
}
