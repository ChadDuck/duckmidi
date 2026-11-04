/* The OoT plugin through its entry point: settings, review, package and Export to Repository
   against a generated excerpt of a zeldaret/oot checkout. */
#include "../libraries/ootexport/plugin/oot_plugin.h"
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
static char installed[64][160], artifacts[16][160];
static char *contents[64];
static uint32_t flags[64];
static unsigned installed_count, artifact_count;

static void reset_checkout(void) {
    checkout_count = 0;
    checkout[checkout_count++] = (File){OOT_SEQUENCE_TABLE, table};
    checkout[checkout_count++] = (File){OOT_SPEC, spec};
    checkout[checkout_count++] = (File){OOT_SESSION_CONFIG, session};
    checkout[checkout_count++] = (File){OOT_AUDIO_HEAP_C, heap_c};
    checkout[checkout_count++] = (File){OOT_BUFFERS_H, buffers_h};

    for (unsigned i = 0; i < installed_count; i++) {
        free(contents[i]);
    }

    installed_count = artifact_count = 0;
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

static int32_t DME_CALL read_file(void *user, const char *path, const uint8_t **data,
                                  uint64_t *size) {
    (void)user;

    for (unsigned i = 0; i < checkout_count; ++i) {
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
    assert(installed_count < 64);
    snprintf(installed[installed_count], 160, "%s", path);
    contents[installed_count] = calloc(size + 1, 1);
    memcpy(contents[installed_count], data, size);
    flags[installed_count++] = f;
    return 1;
}

static const char *installed_file(const char *path, uint32_t *f) {
    for (unsigned i = 0; i < installed_count; ++i) {
        if (!strcmp(installed[i], path)) {
            if (f) {
                *f = flags[i];
            }

            return contents[i];
        }
    }

    return NULL;
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
           !strcmp(x->install_label, "Export to Repository") && x->install);
    char error[512] = {0};
    DmeHost host = {NULL, cancelled, progress, diagnostic, artifact, NULL, NULL};
    DmeHost folder = {NULL, cancelled, progress, diagnostic, NULL, read_file, install_file};

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
    assert(!strcmp(field(&ui, "loop")->label, "Loop song") &&
           field_index(&ui, "memory") + 1 == field_index(&ui, "loop"));

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

    /* Export to Repository adds the song, its soundfont and both registrations. */
    job = x->create_job(s, error, sizeof(error));
    assert(job && x->preflight(job, &c, &folder, error, sizeof(error)) == DME_OK);
    reset_checkout();
    assert(x->install(job, &c, &folder, error, sizeof(error)) == DME_OK);
    uint32_t f;
    const char *seq = installed_file("assets/audio/sequences/my_song.seq", &f);
    const char *font = installed_file("assets/audio/soundfonts/Soundfont_my_song.xml", NULL);
    const char *new_table = installed_file(OOT_SEQUENCE_TABLE, NULL);
    const char *new_spec = installed_file(OOT_SPEC, NULL);
    assert(seq && f == 0 && strstr(seq, "generated by duckmidi") &&
           strstr(seq, "#include \"Soundfont_my_song.h\""));
    assert(font && strstr(font, "Index=\"2\"") && strstr(font, "generated by duckmidi"));
    assert(new_table && strstr(new_table, "(Sequence_my_song, NA_BGM_MY_SONG, MEDIUM_CART"));
    assert(new_spec && strstr(new_spec, "soundfonts/Soundfont_my_song.o") &&
           strstr(new_spec, "sequences/my_song.o"));
    assert(installed_count == 4 && !installed_file(OOT_SESSION_CONFIG, NULL));
    x->destroy_job(job);

    /* Re-exporting keeps the soundfont's index; a foreign file at the path is never replaced. */
    char existing_font[256];
    snprintf(existing_font, sizeof(existing_font),
             "<!-- my_song: generated by duckmidi -->\n<Soundfont Name=\"Soundfont_my_song\" "
             "Index=\"40\">\n");
    reset_checkout();
    checkout[checkout_count++] =
        (File){"assets/audio/soundfonts/Soundfont_my_song.xml", existing_font};
    job = x->create_job(s, error, sizeof(error));
    assert(x->install(job, &c, &folder, error, sizeof(error)) == DME_OK);
    assert(strstr(installed_file("assets/audio/soundfonts/Soundfont_my_song.xml", NULL),
                  "Index=\"40\""));
    x->destroy_job(job);
    reset_checkout();
    checkout[checkout_count++] = (File){"assets/audio/sequences/my_song.seq", "my own song\n"};
    job = x->create_job(s, error, sizeof(error));
    assert(x->install(job, &c, &folder, error, sizeof(error)) == DME_INVALID &&
           strstr(error, "not made by duckmidi") && installed_count == 0);
    x->destroy_job(job);
    reset_checkout();
    checkout_count = 1; /* Not a checkout: no spec */
    job = x->create_job(s, error, sizeof(error));
    assert(x->install(job, &c, &folder, error, sizeof(error)) == DME_INVALID &&
           strstr(error, "zeldaret/oot"));
    x->destroy_job(job);

    /* Replacing a song with a vanilla soundfont edits the table line and adds no XML. */
    DmeValue market = {DME_CHOICE, 0, NULL};

    for (unsigned i = 0; i < oot_song_count; i++) {
        if (!strcmp(oot_songs[i].id, "NA_BGM_MARKET")) {
            market.integer = (int)i + 1;
        }
    }

    DmeValue orchestra = {DME_CHOICE, 3, NULL};
    assert(x->set(s, "song", &market, error, sizeof(error)) == DME_OK);
    assert(x->set(s, "soundfont", &orchestra, error, sizeof(error)) == DME_OK);
    reset_checkout();
    job = x->create_job(s, error, sizeof(error));
    assert(x->install(job, &c, &folder, error, sizeof(error)) == DME_OK);
    assert(strstr(installed_file(OOT_SEQUENCE_TABLE, NULL),
                  "(Sequence_my_song,        NA_BGM_MARKET"));
    assert(!installed_file("assets/audio/soundfonts/Soundfont_my_song.xml", NULL) &&
           !strstr(installed_file(OOT_SPEC, NULL), "Soundfont_my_song") &&
           strstr(installed_file("assets/audio/sequences/my_song.seq", NULL),
                  "#include \"Soundfont_3.h\""));
    x->destroy_job(job);

    /* Songs earlier exports installed that the game no longer plays are removed, freeing their
       soundfonts: old_a, which this export replaces in the market, and lost_c, whose soundfont
       was deleted by hand. kept_b stays and takes the first free number; theirs was not made by
       duckmidi and is left alone. */
    static const char *leftover_spec =
        "beginseg\n    name \"Audiobank\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_0.o\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_1.o\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_old_a.o\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_lost_c.o\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_kept_b.o\"\nendseg\n"
        "beginseg\n    name \"Audioseq\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/sequences/seq_0.prg.o\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/sequences/old_a.o\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/sequences/lost_c.o\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/sequences/kept_b.o\"\n"
        "    include \"$(BUILD_DIR)/assets/audio/sequences/theirs.o\"\nendseg\n";
    static const char *leftover_table =
        "DEFINE_SEQUENCE    (Sequence_old_a,        NA_BGM_MARKET,           MEDIUM_CART, "
        "CACHE_LOAD_TEMPORARY,  0          ) // market\n"
        "DEFINE_SEQUENCE    (Sequence_kept_b,       NA_BGM_CUTSCENE_EFFECTS, MEDIUM_CART, "
        "CACHE_LOAD_TEMPORARY,  0          ) // cutscene_effects\n";
    DmeValue custom_font = {DME_CHOICE, OOT_CUSTOM_CHOICE, NULL};
    assert(x->set(s, "soundfont", &custom_font, error, sizeof(error)) == DME_OK);
    reset_checkout();
    checkout[0].data = leftover_table;
    checkout[1].data = leftover_spec;
    checkout[checkout_count++] =
        (File){"assets/audio/sequences/old_a.seq", "/* old_a: generated by duckmidi */\n"};
    checkout[checkout_count++] = (File){
        "assets/audio/soundfonts/Soundfont_old_a.xml",
        "<!-- generated by duckmidi -->\n<Soundfont Name=\"Soundfont_old_a\" Index=\"2\"/>\n"};
    checkout[checkout_count++] =
        (File){"assets/audio/sequences/lost_c.seq", "/* lost_c: generated by duckmidi */\n"};
    checkout[checkout_count++] =
        (File){"assets/audio/sequences/kept_b.seq", "/* kept_b: generated by duckmidi */\n"};
    checkout[checkout_count++] = (File){
        "assets/audio/soundfonts/Soundfont_kept_b.xml",
        "<!-- generated by duckmidi -->\n<Soundfont Name=\"Soundfont_kept_b\" Index=\"4\"/>\n"};
    checkout[checkout_count++] = (File){"assets/audio/sequences/theirs.seq", "my own song\n"};
    messages[0] = 0;
    job = x->create_job(s, error, sizeof(error));
    assert(x->install(job, &c, &folder, error, sizeof(error)) == DME_OK);
    const char *removed[] = {"assets/audio/sequences/old_a.seq",
                             "assets/audio/soundfonts/Soundfont_old_a.xml",
                             "assets/audio/sequences/lost_c.seq"};

    for (size_t i = 0; i < 3; i++) {
        assert(installed_file(removed[i], &f) && f == DME_INSTALL_REMOVE);
    }

    assert(!installed_file("assets/audio/sequences/theirs.seq", NULL) &&
           !installed_file("assets/audio/sequences/kept_b.seq", NULL));
    const char *cleaned_spec = installed_file(OOT_SPEC, NULL);
    assert(!strstr(cleaned_spec, "old_a") && !strstr(cleaned_spec, "lost_c") &&
           strstr(cleaned_spec, "Soundfont_kept_b.o") && strstr(cleaned_spec, "theirs.o") &&
           strstr(cleaned_spec, "Soundfont_my_song.o"));
    assert(strstr(installed_file("assets/audio/soundfonts/Soundfont_kept_b.xml", NULL),
                  "Index=\"2\""));
    assert(strstr(installed_file("assets/audio/soundfonts/Soundfont_my_song.xml", NULL),
                  "Index=\"3\""));
    assert(strstr(messages, "old_a") && strstr(messages, "lost_c") && !strstr(messages, "theirs"));
    x->destroy_job(job);
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
    messages[0] = 0;
    job = x->create_job(s, error, sizeof(error));
    assert(x->install(job, &c, &folder, error, sizeof(error)) == DME_OK);
    const char *new_session = installed_file(OOT_SESSION_CONFIG, &f);
    assert(new_session && f == DME_INSTALL_BACKUP && !strstr(new_session, "0x2800, 0x2880"));
    const char *new_heap = installed_file(OOT_AUDIO_HEAP_C, &f);
    assert(new_heap && f == DME_INSTALL_BACKUP && !strstr(new_heap, "0x38000") &&
           installed_file(OOT_BUFFERS_H, NULL));
    x->destroy_job(job);

    /* With Enlarge, the memory is raised, so the song draws no memory warning. */
    assert(!strstr(messages, "Sequence memory") && !strstr(messages, "audio spec 10"));

    /* Stock leaves the audio code alone and says where the song does not fit. */
    DmeValue stock = {DME_CHOICE, 1, NULL};
    assert(x->set(s, "memory", &stock, error, sizeof(error)) == DME_OK);
    reset_checkout();
    messages[0] = 0;
    job = x->create_job(s, error, sizeof(error));
    assert(x->install(job, &c, &folder, error, sizeof(error)) == DME_OK &&
           !installed_file(OOT_SESSION_CONFIG, NULL));
    assert(strstr(messages, "Sequence memory") || strstr(messages, "audio spec 10"));
    x->destroy_job(job);
    reset_checkout();
    x->destroy(s);
    free(events);
    puts("oot_install_test: ok");
    return 0;
}
