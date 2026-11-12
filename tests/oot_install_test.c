/* The OoT plugin through its entry point: settings, review, package and Export to Repository
   against a generated excerpt of a zeldaret/oot checkout. */
#include "../libraries/ootexport/plugin/oot_plugin.h"
#include "native_repository_fixture.h"
#include "repository_install.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi);

static const char *table =
    "DEFINE_SEQUENCE    (Sequence_29,        NA_BGM_MARKET,           MEDIUM_CART, "
    "CACHE_LOAD_TEMPORARY,  0          ) // market\n"
    "DEFINE_SEQUENCE    (Sequence_109,       NA_BGM_CUTSCENE_EFFECTS, MEDIUM_CART, "
    "CACHE_LOAD_TEMPORARY,  0          ) // cutscene_effects\n";

static const char *spec =
    "beginseg\n    name \"Audiobank\"\n"
    "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_0.o\"\n"
    "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_1.o\"\nendseg\n"
    "beginseg\n    name \"Audioseq\"\n"
    "    include \"$(BUILD_DIR)/assets/audio/sequences/seq_0.prg.o\"\nendseg\n";

static const char *session =
    "AudioSpec gAudioSpecs[2] = {\n"
    "    { 32000, 1, 24, 4, 0, 0, 2, sReverbSettings[0], 0x300, 0x200, 0x7FFF, 0x7F0, 0xE00, 0, "
    "0x3800, 0x2880, 0, 0, 0 },\n"
    "    { 32000, 1, 28, 3, 0, 0, 2, sReverbSettings[10], 0x300, 0x200, 0x7FFF, 0x7F0, 0xE00, 0, "
    "0x2800, 0x2880, 0, 0, 0 },\n};\n";

static const char *heap_c = "ALIGNED(16) u8 gAudioHeap[0x38000];\n";
static const char *buffers_h = "extern u8 gAudioHeap[0x38000]; // 0x38000 bytes\n";

typedef struct {
    const char *path, *data;
} File;

static File checkout[16];
static unsigned checkout_count;
static char artifacts[16][160];
static unsigned artifact_count;

static void reset_checkout(void) {
    checkout_count = 0;
    checkout[checkout_count++] = (File){OOT_SEQUENCE_TABLE, table};
    checkout[checkout_count++] = (File){OOT_SPEC, spec};
    checkout[checkout_count++] = (File){OOT_SESSION_CONFIG, session};
    checkout[checkout_count++] = (File){OOT_AUDIO_HEAP_C, heap_c};
    checkout[checkout_count++] = (File){OOT_BUFFERS_H, buffers_h};

    artifact_count = 0;
}

static int32_t DME_CALL cancelled(void *user) {
    (void)user;
    return 0;
}

static void DME_CALL progress(void *user, uint32_t percent) {
    (void)user;
    assert(percent <= 100);
}

static char messages[8192]; /* Diagnostics since the last clear, one per line. */

static void DME_CALL diagnostic(void *user, uint32_t severity, uint32_t f, const char *message) {
    (void)user;
    (void)severity;
    (void)f;
    assert(message && *message);
    size_t used = strlen(messages);
    snprintf(messages + used, sizeof(messages) - used, "%s\n", message);
}

static int field_index(const DmeUi *ui, const char *id) {
    for (uint32_t i = 0; i < ui->count; i++) {
        if (!strcmp(ui->fields[i].id, id)) {
            return (int)i;
        }
    }

    return -1;
}

static int32_t DME_CALL artifact(void *user, const char *name, const uint8_t *data, uint64_t size) {
    (void)user;
    (void)data;
    assert(artifact_count < 16 && size > 0 && !strchr(name, '/'));
    snprintf(artifacts[artifact_count++], 160, "%s", name);
    return 1;
}

/* A generated checkout of its own: the native fixture under the listed excerpts, with an empty
   plugins folder for its history. */
static void make_checkout(char dir[512], char plugins[600], const char *name) {
    scenario_folder(dir, 512, "oot-install", name);
    native_repository_fixture(dir, true);

    for (unsigned i = 0; i < checkout_count; i++) {
        checkout_write(dir, checkout[i].path, checkout[i].data);
    }

    snprintf(plugins, 600, "%s-plugins", dir);
    assert(SDL_CreateDirectory(plugins));
}

static bool same_file(const char *dir, const char *path, const char *expected) {
    char *actual = checkout_read(dir, path);
    bool same = actual && expected && !strcmp(actual, expected);
    SDL_free(actual);
    return same || (!actual && !expected);
}

static char *install(const DmeExporter *x, void *settings, const DmeContext *c, const DmeHost *host,
                     const char *plugins, const char *dir, const char *read_back) {
    char error[2048] = {0};
    int32_t status = repository_install(x, settings, c, host, plugins, dir, error, sizeof(error));

    if (status != DME_OK) {
        fprintf(stderr, "install: %s\n", error);
    }

    assert(status == DME_OK);
    return read_back ? checkout_read(dir, read_back) : NULL;
}

static bool has_artifact(const char *name) {
    for (unsigned i = 0; i < artifact_count; i++) {
        if (!strcmp(artifacts[i], name)) {
            return true;
        }
    }

    return false;
}

static const DmeField *field(const DmeUi *ui, const char *id) {
    for (uint32_t i = 0; i < ui->count; i++) {
        if (!strcmp(ui->fields[i].id, id)) {
            return &ui->fields[i];
        }
    }

    return NULL;
}

static DmeEvent *events;
static uint64_t event_count, event_capacity;

static void add(uint64_t tick, uint32_t status, uint32_t a, uint32_t b) {
    if (event_count == event_capacity) {
        event_capacity = event_capacity ? event_capacity * 2 : 256;
        events = realloc(events, event_capacity * sizeof(*events));
        assert(events);
    }

    events[event_count] = (DmeEvent){tick, event_count + 1, status, a, b, 0};
    event_count++;
}

static int compare_events(const void *left, const void *right) {
    const DmeEvent *a = left, *b = right;

    if (a->tick != b->tick) {
        return a->tick < b->tick ? -1 : 1;
    }

    return a->source_id < b->source_id ? -1 : 1;
}

static DmeContext context(uint64_t duration) {
    qsort(events, event_count, sizeof(*events), compare_events);
    DmeContext c = {0};
    c.events = events;
    c.event_count = event_count;
    c.duration = duration;
    c.ppq = 96;
    c.project_session = 7;
    c.project_name = "test";
    c.percussion_channels = 1u << MIDI_DRUM_CHANNEL;
    c.source_name = "test";
    return c;
}

int main(void) {
    const DmePlugin *plugin = duckmidi_export_plugin(DME_ABI);
    assert(plugin && plugin->abi == DME_ABI && plugin->size == sizeof(DmePlugin) &&
           !duckmidi_export_plugin(DME_ABI + 1));
    const DmeExporter *x = plugin->exporter(0);
    assert(x && !plugin->exporter(1) && !strcmp(x->name, "OoT") && x->container == DME_ZIP &&
           !strcmp(x->install_label, "Export to Repository") && x->history);
    char error[512] = {0};
    DmeHost host = {NULL, cancelled, progress, diagnostic, artifact, NULL, NULL};
    DmeHost folder = {.cancelled = cancelled, .progress = progress, .diagnostic = diagnostic};

    /* A short piano and drum song. */
    for (unsigned i = 0; i < 8; i++) {
        add(i * 96, MIDI_NOTE_ON, 60 + i, 100);
        add(i * 96 + 90, MIDI_NOTE_OFF, 60 + i, 0);
        add(i * 96, MIDI_NOTE_ON | MIDI_DRUM_CHANNEL, i % 2 ? 38 : 36, 110);
        add(i * 96 + 20, MIDI_NOTE_OFF | MIDI_DRUM_CHANNEL, i % 2 ? 38 : 36, 0);
    }

    DmeContext c = context(8 * 96);
    void *s = x->create();
    assert(s && x->inspect(s, &c, error, sizeof(error)) == DME_OK);
    DmeUi ui;
    assert(x->describe(s, &ui) == DME_OK);
    const char *ids[] = {"song",   "name",       "soundfont", "volume",        "loop",
                         "memory", "percussion", "mapping",   "reset_mappings"};

    for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); i++) {
        assert(field(&ui, ids[i]));
    }

    const DmeField *mapping = field(&ui, "mapping");
    assert(mapping->row_count == 1 && !strcmp(mapping->rows[0].id, "melodic.0.0"));
    bool piano_listed = false;

    for (uint32_t i = 0; i < mapping->choice_count; i++) {
        piano_listed |= mapping->choices[i].value == mapping->rows[0].value &&
                        !strcmp(mapping->choices[i].label, "Piano (Main Orchestra)");
    }

    assert(piano_listed && field(&ui, "song")->choice_count == oot_song_count + 1);

    /* Option names: Area lists areas without a Replace prefix, Sample Set lists Custom and the
       area names, and Sequence memory (Enlarge / Stock) comes before Loop song. */
    const DmeField *area = field(&ui, "song"), *set = field(&ui, "soundfont");
    assert(!strcmp(area->label, "Area") && !strcmp(set->label, "Sample Set"));

    for (uint32_t i = 0; i < area->choice_count; i++) {
        assert(!strstr(area->choices[i].label, "Replace"));
    }

    assert(!strcmp(set->choices[0].label, "Custom") &&
           !strcmp(set->choices[1].label, "Main Orchestra"));

    for (uint32_t i = 0; i < set->choice_count; i++) {
        assert(!strstr(set->choices[i].label, "Vanilla") && !strchr(set->choices[i].label, ':'));
    }

    const DmeField *memory = field(&ui, "memory");
    assert(!strcmp(memory->label, "Sequence memory") && memory->choice_count == 2 &&
           !strcmp(memory->choices[0].label, "Enlarge") &&
           !strcmp(memory->choices[1].label, "Stock"));
    /* Area, Sequence name, Sample Set, Sequence memory, then Sequence volume. */
    assert(field_index(&ui, "song") + 1 == field_index(&ui, "name") &&
           field_index(&ui, "name") + 1 == field_index(&ui, "soundfont") &&
           field_index(&ui, "soundfont") + 1 == field_index(&ui, "memory") &&
           field_index(&ui, "memory") + 1 == field_index(&ui, "volume"));
    assert(!strcmp(field(&ui, "loop")->label, "Loop song") &&
           field_index(&ui, "volume") + 1 == field_index(&ui, "loop"));

    /* The sequence name follows the project name, or the MIDI file's name when the project has
       none, until the user types one. */
    assert(!strcmp(field(&ui, "name")->value.text, "test"));
    DmeContext untitled = c;
    untitled.project_name = "";
    untitled.source_name = "C:\\Music\\My Song (v2).mid";
    untitled.project_session = 8;
    assert(x->inspect(s, &untitled, error, sizeof(error)) == DME_OK &&
           x->describe(s, &ui) == DME_OK);
    assert(!strcmp(field(&ui, "name")->value.text, "my_song_v2"));
    untitled.source_name = "2nd take.mid";
    untitled.project_session = 9;
    assert(x->inspect(s, &untitled, error, sizeof(error)) == DME_OK &&
           x->describe(s, &ui) == DME_OK);
    assert(!strcmp(field(&ui, "name")->value.text, "song_2nd_take"));
    DmeValue typed = {DME_TEXT, 0, "typed_name"};
    assert(x->set(s, "name", &typed, error, sizeof(error)) == DME_OK);
    assert(x->inspect(s, &untitled, error, sizeof(error)) == DME_OK &&
           x->describe(s, &ui) == DME_OK);
    assert(!strcmp(field(&ui, "name")->value.text, "typed_name"));
    assert(x->inspect(s, &c, error, sizeof(error)) == DME_OK && x->describe(s, &ui) == DME_OK);
    assert(!strcmp(field(&ui, "name")->value.text, "test"));

    /* Similar instruments: choices close to the default mapping that reach the row's notes. The
       list stays the same after one of them is chosen. */
    mapping = field(&ui, "mapping");
    const DmeMappingRow *mapped = &mapping->rows[0];
    assert(mapped->similar_count > 0 && mapped->similar_count <= 8);
    int similar[8];
    uint32_t similar_count = mapped->similar_count;

    for (uint32_t i = 0; i < mapped->similar_count; i++) {
        bool listed = false;

        for (uint32_t j = 0; j < mapping->choice_count; j++) {
            listed |= mapping->choices[j].value == mapped->similar[i].value;
        }

        assert(listed && mapped->similar[i].value != mapped->default_value);
        similar[i] = mapped->similar[i].value;
    }

    DmeValue pick = {DME_CHOICE, similar[similar_count - 1], NULL};
    assert(x->set(s, mapped->id, &pick, error, sizeof(error)) == DME_OK &&
           x->describe(s, &ui) == DME_OK);
    mapped = &field(&ui, "mapping")->rows[0];
    assert(mapped->value == pick.integer && mapped->similar_count == similar_count);

    for (uint32_t i = 0; i < similar_count; i++) {
        assert(mapped->similar[i].value == similar[i]);
    }

    DmeValue back = {DME_ACTION, 0, NULL};
    assert(x->set(s, "reset_mappings", &back, error, sizeof(error)) == DME_OK &&
           x->describe(s, &ui) == DME_OK);

    /* Percussion rows list the used keys with the orchestra kit. */
    DmeValue on = {DME_BOOL, 1, NULL};
    assert(x->set(s, "percussion", &on, error, sizeof(error)) == DME_OK);
    assert(x->describe(s, &ui) == DME_OK);
    assert(field(&ui, "mapping")->row_count == 2);
    on.integer = 0;
    assert(x->set(s, "percussion", &on, error, sizeof(error)) == DME_OK);

    /* Settings validate their values. */
    DmeValue bad_name = {DME_TEXT, 0, "seq_7"};
    assert(x->set(s, "name", &bad_name, error, sizeof(error)) == DME_OK);
    assert(!x->create_job(s, error, sizeof(error)) && strstr(error, "seq_"));
    DmeValue name = {DME_TEXT, 0, "my_song"};
    assert(x->set(s, "name", &name, error, sizeof(error)) == DME_OK);
    DmeValue bad_font = {DME_CHOICE, 4, NULL};
    assert(x->set(s, "soundfont", &bad_font, error, sizeof(error)) == DME_INVALID);
    DmeValue bad_volume = {DME_INT, 0, NULL};
    assert(x->set(s, "volume", &bad_volume, error, sizeof(error)) == DME_INVALID);
    DmeValue new_song = {DME_CHOICE, 0, NULL};
    assert(x->set(s, "song", &new_song, error, sizeof(error)) == DME_OK);

    /* Package: sequence, soundfont and install guide. */
    void *job = x->create_job(s, error, sizeof(error));
    assert(job && x->preflight(job, &c, &host, error, sizeof(error)) == DME_OK);
    DmeReview review;
    assert(x->review(job, &review) == DME_OK && review.count == 0);
    reset_checkout();
    assert(x->run(job, &c, &host, error, sizeof(error)) == DME_OK);
    assert(artifact_count == 3 && has_artifact("my_song.seq") &&
           has_artifact("Soundfont_my_song.xml") && has_artifact("INSTALL.txt"));
    x->destroy_job(job);

    /* Export to Repository adds the song, its soundfont and both registrations; audio memory
       stays for a short song. */
    char dir[512], plugins[600];
    reset_checkout();
    make_checkout(dir, plugins, "song");
    char *seq = install(x, s, &c, &folder, plugins, dir, "assets/audio/sequences/my_song.seq");
    char *font = checkout_read(dir, "assets/audio/soundfonts/Soundfont_my_song.xml");
    char *new_table = checkout_read(dir, OOT_SEQUENCE_TABLE);
    char *new_spec = checkout_read(dir, OOT_SPEC);
    assert(seq && strstr(seq, "#include \"Soundfont_my_song.h\""));
    assert(font && strstr(font, "Name=\"Soundfont_my_song\""));
    assert(new_table && strstr(new_table, "(Sequence_my_song, NA_BGM_MY_SONG, MEDIUM_CART"));
    assert(new_spec && strstr(new_spec, "soundfonts/Soundfont_my_song.o") &&
           strstr(new_spec, "sequences/my_song.o"));
    assert(same_file(dir, OOT_SESSION_CONFIG, session));

    /* Re-exporting updates the same logical song and keeps its soundfont index. */
    const char *index = strstr(font, "Index=\"");
    char kept[32];
    assert(index && sscanf(index, "Index=\"%31[0-9]", kept) == 1);
    SDL_free(font);
    font =
        install(x, s, &c, &folder, plugins, dir, "assets/audio/soundfonts/Soundfont_my_song.xml");
    char expected_index[64];
    snprintf(expected_index, sizeof(expected_index), "Index=\"%s\"", kept);
    assert(font && strstr(font, expected_index));
    SDL_free(seq);
    SDL_free(font);
    SDL_free(new_table);
    SDL_free(new_spec);

    /* A folder that is not a checkout is refused before anything is recorded. */
    char empty[512];
    scenario_folder(empty, sizeof(empty), "oot-install", "empty");
    assert(repository_install(x, s, &c, &folder, plugins, empty, error, sizeof(error)) != DME_OK &&
           strstr(error, "zeldaret/oot"));

    /* Replacing a song with a vanilla soundfont edits the table line and adds no XML. The song
       name is still installed as a new song, so that export is refused until it is reverted. */
    DmeValue market = {DME_CHOICE, 0, NULL};

    for (unsigned i = 0; i < oot_song_count; i++) {
        if (!strcmp(oot_songs[i].id, "NA_BGM_MARKET")) {
            market.integer = (int)i + 1;
        }
    }

    DmeValue orchestra = {DME_CHOICE, 3, NULL};
    assert(x->set(s, "song", &market, error, sizeof(error)) == DME_OK);
    assert(x->set(s, "soundfont", &orchestra, error, sizeof(error)) == DME_OK);
    assert(repository_install(x, s, &c, &folder, plugins, dir, error, sizeof(error)) != DME_OK &&
           strstr(error, "my_song"));

    /* Revert all restores the initialized checkout. */
    assert(repository_revert_all(x, s, &folder, plugins, dir, error, sizeof(error)) == DME_OK);
    assert(same_file(dir, OOT_SEQUENCE_TABLE, table) && same_file(dir, OOT_SPEC, spec) &&
           same_file(dir, "assets/audio/sequences/my_song.seq", NULL) &&
           same_file(dir, "assets/audio/soundfonts/Soundfont_my_song.xml", NULL));
    new_table = install(x, s, &c, &folder, plugins, dir, OOT_SEQUENCE_TABLE);
    assert(new_table && strstr(new_table, "(Sequence_my_song,        NA_BGM_MARKET"));
    SDL_free(new_table);
    assert(same_file(dir, "assets/audio/soundfonts/Soundfont_my_song.xml", NULL));
    seq = checkout_read(dir, "assets/audio/sequences/my_song.seq");
    new_spec = checkout_read(dir, OOT_SPEC);
    assert(seq && strstr(seq, "#include \"Soundfont_3.h\"") && new_spec &&
           !strstr(new_spec, "Soundfont_my_song"));
    SDL_free(seq);
    SDL_free(new_spec);
    assert(x->set(s, "soundfont", &orchestra, error, sizeof(error)) == DME_OK);

    /* A high flute line needs a substitute: the review recommends one and applies it. */
    DmeValue custom = {DME_CHOICE, OOT_CUSTOM_CHOICE, NULL};
    assert(x->set(s, "soundfont", &custom, error, sizeof(error)) == DME_OK);
    int flute = -1;

    for (unsigned i = 0; i < oot_instrument_count && flute < 0; i++) {
        flute = strcmp(oot_instruments[i].name, "Flute") ? -1 : (int)i;
    }

    DmeValue flute_value = {DME_CHOICE, flute, NULL};
    assert(x->set(s, "melodic.0.0", &flute_value, error, sizeof(error)) == DME_OK);
    event_count = 0;
    add(0, MIDI_NOTE_ON, 120, 100);
    add(96, MIDI_NOTE_OFF, 120, 0);
    c = context(96);
    assert(x->inspect(s, &c, error, sizeof(error)) == DME_OK);
    job = x->create_job(s, error, sizeof(error));
    assert(x->preflight(job, &c, &host, error, sizeof(error)) == DME_OK);
    assert(x->review(job, &review) == DME_OK && review.count == 1);
    const DmeReviewRow *row = &review.rows[0];
    assert(!strcmp(row->id, "melodic.0.0") && row->recommended_count > 0 && row->supported &&
           !strcmp(row->current_mapping, "Flute (Main Orchestra)"));
    int chosen = row->recommended[0].value;
    assert(x->review_set(job, "melodic.0.0", 999999, error, sizeof(error)) == DME_INVALID);
    assert(x->review_set(job, "melodic.0.0", chosen, error, sizeof(error)) == DME_OK);
    assert(x->review_apply(s, job, error, sizeof(error)) == DME_OK);
    x->destroy_job(job);
    assert(x->describe(s, &ui) == DME_OK && field(&ui, "mapping")->rows[0].value == chosen);

    /* A long song that does not repeat raises the caches and the audio heap. */
    event_count = 0;
    unsigned seed = 12345;

    for (unsigned i = 0; i < 4000; i++) {
        seed = seed * 1103515245u + 12345u;
        unsigned key = 48 + (seed >> 16) % 30, length = 10 + (seed >> 8) % 70;
        add(i * 97, MIDI_NOTE_ON, key, 40 + (seed >> 20) % 80);
        add(i * 97 + length, MIDI_NOTE_OFF, key, 0);
    }

    c = context(4000 * 97);
    assert(x->inspect(s, &c, error, sizeof(error)) == DME_OK);
    reset_checkout();
    make_checkout(dir, plugins, "memory");
    messages[0] = 0;
    char *new_session = install(x, s, &c, &folder, plugins, dir, OOT_SESSION_CONFIG);
    char *new_heap = checkout_read(dir, OOT_AUDIO_HEAP_C);
    assert(new_session && !strstr(new_session, "0x2800, 0x2880"));
    assert(new_heap && !strstr(new_heap, "0x38000") && !same_file(dir, OOT_BUFFERS_H, buffers_h));
    SDL_free(new_session);
    SDL_free(new_heap);

    /* With Enlarge, the memory is raised, so the song draws no memory warning. */
    assert(!strstr(messages, "Sequence memory") && !strstr(messages, "audio spec 10"));

    /* Stock composes the song again, so the audio code returns to its initialized text, and says
       where the song does not fit. */
    DmeValue stock = {DME_CHOICE, 1, NULL};
    assert(x->set(s, "memory", &stock, error, sizeof(error)) == DME_OK);
    messages[0] = 0;
    install(x, s, &c, &folder, plugins, dir, NULL);
    assert(same_file(dir, OOT_SESSION_CONFIG, session) && same_file(dir, OOT_AUDIO_HEAP_C, heap_c));
    assert(strstr(messages, "Sequence memory") || strstr(messages, "audio spec 10"));
    reset_checkout();
    x->destroy(s);
    free(events);
    puts("oot_install_test: ok");
    return 0;
}
