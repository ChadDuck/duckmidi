/* OoT export settings, declared for duckmidi to draw. No SDL, ImGui or editor headers. */
#include "oot_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const DmeChoice memory_choices[] = {{0, "Enlarge"}, {1, "Stock"}};

int oot_ui_font(const OotPluginUi *s) {
    return s->font == OOT_CUSTOM_CHOICE ? OOT_CUSTOM_FONT : s->font;
}

static void reset_maps(OotPluginUi *s) {
    es_mappings_reset(s->custom);
    int font = oot_ui_font(s);

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned p = 0; p < 128; p++) {
            s->melodic[ch][p] = oot_default_instrument(font, p);
        }
    }

    for (unsigned k = 0; k < 128; k++) {
        s->drums[k] = oot_default_drum(font, k);
    }

    s->similar_stale = true;
}

static int song_index(const char *id) {
    for (unsigned i = 0; i < oot_song_count; i++) {
        if (!strcmp(oot_songs[i].id, id)) {
            return (int)i + 1;
        }
    }

    return 0;
}

void DME_CALL oot_ui_reset(void *instance) {
    OotPluginUi *s = instance;
    s->song = song_index("NA_BGM_MARKET");
    s->font = OOT_CUSTOM_CHOICE;
    snprintf(s->name, sizeof(s->name), "%s", *s->project_name ? s->project_name : "duckmidi_song");
    s->name_set = false;
    s->similar_stale = true;
    s->volume = 100;
    s->loop = 1;
    s->loop_points = 1;
    s->memory = 0;
    s->percussion = 0;
    reset_maps(s);
    es_mappings_reset(s->custom);
}

void *DME_CALL oot_ui_create(void) {
    OotPluginUi *s = calloc(1, sizeof(*s));

    if (!s) {
        return NULL;
    }

    s->custom = es_mappings_create(1, 1);

    if (!s->custom) {
        free(s);
        return NULL;
    }

    oot_ui_reset(s);

    for (unsigned i = 0; i < 128; i++) {
        snprintf(s->program_names[i], 80, "GM program %u", i);
        snprintf(s->drum_names[i], 80, "GM drum note %u", i);
    }

    return s;
}

void DME_CALL oot_ui_destroy(void *instance) {
    OotPluginUi *s = instance;
    es_mappings_destroy(s->custom);
    free(s);
}

/* A valid sequence name from the project name, or the MIDI file name without its folder and
   extension; empty when neither gives one. */
static void project_sequence_name(const DmeContext *c, char *out) {
    char source[256];
    const char *from = c->project_name;

    if (!from || !*from) {
        const char *path = c->source_name ? c->source_name : "";
        const char *slash = strrchr(path, '/'), *backslash = strrchr(path, '\\');
        path = backslash > slash ? backslash + 1 : slash ? slash + 1 : path;
        snprintf(source, sizeof(source), "%s", path);
        char *dot = strrchr(source, '.');

        if (dot) {
            *dot = 0;
        }

        from = source;
    }

    size_t n = 0;

    for (const char *p = from; *p && n < OOT_NAME_CAPACITY - 1; p++) {
        char ch = *p >= 'A' && *p <= 'Z' ? (char)(*p - 'A' + 'a') : *p;
        bool word = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');

        if (word) {
            out[n++] = ch;
        } else if (n && out[n - 1] != '_') {
            out[n++] = '_';
        }
    }

    while (n && out[n - 1] == '_') {
        n--;
    }

    out[n] = 0;

    /* Names start with a letter, and seq_ names belong to the vanilla sequences. */
    if (n && (out[0] < 'a' || out[0] > 'z' || !strncmp(out, "seq_", 4))) {
        char named[OOT_NAME_CAPACITY];
        snprintf(named, sizeof(named), "song_%s", out);
        memcpy(out, named, sizeof(named));
    }
}

int32_t DME_CALL oot_ui_inspect(void *instance, const DmeContext *c, char *error, uint64_t cap) {
    (void)error;
    (void)cap;
    OotPluginUi *s = instance;

    if (!c || (!c->events && c->event_count)) {
        return DME_INVALID;
    }

    if (s->session_known && s->session != c->project_session) {
        reset_maps(s);
        s->name_set = false;
    }

    project_sequence_name(c, s->project_name);

    if (!s->name_set) {
        snprintf(s->name, sizeof(s->name), "%s",
                 *s->project_name ? s->project_name : "duckmidi_song");
    }

    s->session = c->project_session;
    s->session_known = true;
    s->has_loop_points = c->loop_points != 0;
    s->similar_stale = true;
    memset(s->used, 0, sizeof(s->used));
    memset(s->used_drums, 0, sizeof(s->used_drums));
    memset(s->low, 127, sizeof(s->low));
    memset(s->high, 0, sizeof(s->high));

    for (unsigned i = 0; i < 128; i++) {
        if (c->program_names[i]) {
            snprintf(s->program_names[i], 80, "%s", c->program_names[i]);
        }

        if (c->drum_names[i]) {
            snprintf(s->drum_names[i], 80, "%s", c->drum_names[i]);
        }
    }

    OotPortLayout *layout = calloc(1, sizeof(*layout));

    if (!layout || !oot_port_layout(c->events, c->event_count, c->percussion_channels, layout)) {
        free(layout);
        return DME_NOMEM;
    }

    unsigned program[16] = {0};

    for (uint64_t i = 0; i < c->event_count; i++) {
        const DmeEvent *e = &c->events[i];

        if (e->status < MIDI_STATUS || e->status >= MIDI_SYSTEM || e->a >= 128) {
            continue;
        }

        unsigned type = e->status & MIDI_STATUS_MASK;
        unsigned ch = e->port < OOT_PORTS ? layout->out[e->status & MIDI_CHANNEL_MASK][e->port]
                                          : e->status & MIDI_CHANNEL_MASK;

        if (type == MIDI_PROGRAM) {
            program[ch] = e->a;
        }

        if (type == MIDI_NOTE_ON && e->b) {
            if (ch == MIDI_DRUM_CHANNEL) {
                s->used_drums[e->a] = true;
            } else {
                unsigned p = program[ch];
                s->used[ch][p] = true;
                s->low[ch][p] = e->a < s->low[ch][p] ? (uint8_t)e->a : s->low[ch][p];
                s->high[ch][p] = e->a > s->high[ch][p] ? (uint8_t)e->a : s->high[ch][p];
            }
        }
    }

    bool custom_ok = es_mappings_inspect(s->custom, c, &layout->out[0][0], 128, error, (size_t)cap);

    for (size_t i = 0; custom_ok && i < es_mappings_count(s->custom); i++) {
        const EsSourceRow *row = es_mappings_source(s->custom, i);

        if (row->source.flags & DME_SAMPLE_PERCUSSION) {
            s->used_drums[row->source.drum_key] = true;
        } else {
            s->used[row->native_channel][row->source.program] = true;
        }
    }

    free(layout);
    return custom_ok ? DME_OK : DME_FAILED;
}

static unsigned build_song_choices(OotPluginUi *s) {
    unsigned count = 0;
    char id[OOT_NAME_CAPACITY];

    for (size_t i = 0; i < sizeof(id); i++) {
        char c = s->name[i];
        id[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }

    snprintf(s->song_labels[count], 80, "New song (NA_BGM_%s)", id);
    s->song_choices[count] = (DmeChoice){0, s->song_labels[count]};
    count++;

    for (unsigned i = 0; i < oot_song_count && count < 80; i++) {
        snprintf(s->song_labels[count], 80, "%s", oot_songs[i].label);
        s->song_choices[count] = (DmeChoice){(int)i + 1, s->song_labels[count]};
        count++;
    }

    return count;
}

static unsigned build_font_choices(OotPluginUi *s) {
    unsigned count = 0;
    snprintf(s->font_labels[count], 64, "Custom");
    s->font_choices[count] = (DmeChoice){OOT_CUSTOM_CHOICE, s->font_labels[count]};
    count++;

    for (int font = 0; font < 48 && count < 48; font++) {
        if (!oot_music_font(font)) {
            continue;
        }

        snprintf(s->font_labels[count], 64, "%s", oot_font_title(font));
        s->font_choices[count] = (DmeChoice){font, s->font_labels[count]};
        count++;
    }

    return count;
}

int32_t DME_CALL oot_ui_describe(void *instance, DmeUi *ui) {
    OotPluginUi *s = instance;
    bool drums = s->percussion != 0;
    int font = oot_ui_font(s);
    unsigned choices = drums ? oot_drum_choices(font, s->choices, s->choice_labels, 128)
                             : oot_instrument_choices(font, s->choices, s->choice_labels, 128);
    unsigned count = 0;

    for (size_t source_index = 0; source_index < es_mappings_count(s->custom); source_index++) {
        const EsSourceRow *source = es_mappings_source(s->custom, source_index);
        bool is_drum = (source->source.flags & DME_SAMPLE_PERCUSSION) != 0;

        if (is_drum != drums || count >= 2048) {
            continue;
        }

        unsigned ch = source->native_channel;
        unsigned p = drums ? source->source.drum_key : source->source.program;
        int value = es_mappings_stock(s->custom, 0, source->source,
                                      drums ? s->drums[p] : s->melodic[ch][p]);
        int fallback = drums ? oot_default_drum(font, p) : oot_default_instrument(font, p);

        if (s->similar_stale) {
            OotRangeIssue notes = {.channel = ch,
                                   .program = p,
                                   .low = drums ? p : s->low[ch][p],
                                   .high = drums ? p : s->high[ch][p],
                                   .percussion = drums};
            s->similar_count[count] =
                oot_similar(font, &notes, fallback, s->choices, choices, s->similar[count], 8);
        }

        s->rows[count] = (DmeMappingRow){source->id, source->label,     value,
                                         fallback,   s->similar[count], s->similar_count[count]};
        s->rows[count].source = source->source;
        s->rows[count].custom = es_mappings_describe(s->custom, 0, source_index);
        count++;
    }

    s->similar_stale = false;
    unsigned f = 0;

    s->fields[f++] = (DmeField){
        .id = "song",
        .label = "Area",
        .help = "Replace an existing song everywhere the game plays it, or add a new NA_BGM ID "
                "to play from a scene or code. Hyrule Field's parts cannot be replaced.",
        .type = DME_CHOICE,
        .value = {DME_CHOICE, s->song, NULL},
        .choices = s->song_choices,
        .choice_count = build_song_choices(s)};
    s->fields[f++] = (DmeField){
        .id = "name",
        .label = "Sequence name",
        .help = "Lowercase letters, digits and _. Names the .seq file, Sequence_<name> and the "
                "custom sample set.",
        .type = DME_TEXT,
        .value = {DME_TEXT, 0, s->name},
        .text_capacity = OOT_NAME_CAPACITY};
    s->fields[f++] = (DmeField){
        .id = "soundfont",
        .label = "Sample Set",
        .help = "Custom builds a sample set from the instruments of every area's music. An area's "
                "sample set adds no file but limits the instruments to that area's.",
        .type = DME_CHOICE,
        .value = {DME_CHOICE, s->font, NULL},
        .choices = s->font_choices,
        .choice_count = build_font_choices(s)};
    s->fields[f++] = (DmeField){.id = "volume",
                                .label = "Sequence volume",
                                .type = DME_INT,
                                .value = {DME_INT, s->volume, NULL},
                                .minimum = 1,
                                .maximum = 127};
    s->fields[f++] = (DmeField){
        .id = "memory",
        .label = "Sequence memory",
        .help = "Enlarge lets Export to Repository raise the temporary sequence and sample caches "
                "of every audio spec, and the audio heap by the same amount, when the song needs "
                "more than the smallest scene provides. Stock changes no game files besides the "
                "export itself.",
        .type = DME_CHOICE,
        .value = {DME_CHOICE, s->memory, NULL},
        .choices = memory_choices,
        .choice_count = 2};
    s->fields[f++] = (DmeField){
        .id = "loop", .label = "Loop song", .type = DME_BOOL, .value = {DME_BOOL, s->loop, NULL}};

    if (s->has_loop_points) {
        s->fields[f++] = (DmeField){
            .id = "loop_points",
            .label = "Use loop points",
            .help = "Play once, then repeat from M64 - Loop Start to M64 - Loop End. Overrides "
                    "Loop song.",
            .type = DME_BOOL,
            .value = {DME_BOOL, s->loop_points, NULL}};
    }

    s->fields[f++] = (DmeField){.id = "percussion",
                                .label = "Edit percussion mappings",
                                .type = DME_BOOL,
                                .value = {DME_BOOL, s->percussion, NULL}};
    s->fields[f++] =
        (DmeField){.id = "mapping",
                   .label = s->percussion ? "Used percussion" : "Used channels and instruments",
                   .help = "Choose a used source on the left, then its OoT instrument on the "
                           "right.",
                   .type = DME_MAPPING,
                   .choices = s->choices,
                   .choice_count = choices,
                   .rows = s->rows,
                   .row_count = count};
    s->fields[f++] =
        (DmeField){.id = "reset_mappings", .label = "Reset OoT mappings", .type = DME_ACTION};
    *ui = (DmeUi){s->fields, f};
    return DME_OK;
}

static bool font_choice_valid(int value) {
    return value == OOT_CUSTOM_CHOICE || oot_music_font(value);
}

static bool instrument_allowed(const OotPluginUi *s, int value) {
    int font = oot_ui_font(s);
    return oot_instrument_valid(value) && (font == OOT_CUSTOM_FONT || oot_synth_wave(value) >= 0 ||
                                           oot_font_program(font, value) >= 0);
}

static bool drum_allowed(const OotPluginUi *s, int value) {
    int font = oot_ui_font(s);
    return value == OOT_SILENT || (oot_drum_valid(value) &&
                                   (font == OOT_CUSTOM_FONT || oot_font_drum_hi(font, value) >= 0));
}

int32_t DME_CALL oot_ui_set(void *instance, const char *id, const DmeValue *v, char *error,
                            uint64_t cap) {
    OotPluginUi *s = instance;
    char stock_id[64];

    if (id && es_mappings_stock_id(s->custom, id, stock_id)) {
        bool drums = !strncmp(stock_id, "drum.", 5);
        if (v && v->type == DME_CHOICE &&
            (drums ? drum_allowed(s, v->integer) : instrument_allowed(s, v->integer)) &&
            es_mappings_set_stock(s->custom, 0, id, (int32_t)v->integer)) {
            s->similar_stale = true;
            return DME_OK;
        }

        goto invalid;
    }

    unsigned ch, p;
    char tail;

    if (!id || !v) {
        goto invalid;
    }

    s->similar_stale = true;

    if (!strcmp(id, "song") && v->type == DME_CHOICE && v->integer >= 0 &&
        v->integer <= (int)oot_song_count) {
        s->song = v->integer;
        return DME_OK;
    }

    if (!strcmp(id, "soundfont") && v->type == DME_CHOICE && font_choice_valid(v->integer)) {
        if (s->font != v->integer) {
            s->font = v->integer;
            reset_maps(s); /* Mappings name instruments of the chosen soundfont. */
        }

        return DME_OK;
    }

    if (!strcmp(id, "memory") && v->type == DME_CHOICE && (v->integer == 0 || v->integer == 1)) {
        s->memory = v->integer;
        return DME_OK;
    }

    if (!strcmp(id, "volume") && v->type == DME_INT && v->integer >= 1 && v->integer <= 127) {
        s->volume = v->integer;
        return DME_OK;
    }

    if (!strcmp(id, "name") && v->type == DME_TEXT && v->text &&
        strlen(v->text) < OOT_NAME_CAPACITY) {
        memmove(s->name, v->text, strlen(v->text) + 1);
        s->name_set = true;
        return DME_OK;
    }

    if (v->type == DME_BOOL && (v->integer == 0 || v->integer == 1)) {
        if (!strcmp(id, "loop")) {
            s->loop = v->integer;
            return DME_OK;
        }

        if (!strcmp(id, "loop_points")) {
            s->loop_points = v->integer;
            return DME_OK;
        }

        if (!strcmp(id, "percussion")) {
            s->percussion = v->integer;
            return DME_OK;
        }
    }

    if (v->type == DME_CHOICE) {
        if (sscanf(id, "melodic.%u.%u%c", &ch, &p, &tail) == 2 && ch < 16 &&
            ch != MIDI_DRUM_CHANNEL && p < 128 && s->used[ch][p] &&
            instrument_allowed(s, v->integer)) {
            s->melodic[ch][p] = v->integer;
            return DME_OK;
        }

        if (sscanf(id, "drum.%u%c", &p, &tail) == 1 && p < 128 && s->used_drums[p] &&
            drum_allowed(s, v->integer)) {
            s->drums[p] = v->integer;
            return DME_OK;
        }
    }

    if (!strcmp(id, "reset_mappings") && v->type == DME_ACTION) {
        reset_maps(s);
        return DME_OK;
    }

invalid:
    snprintf(error, (size_t)cap, "Invalid OoT exporter setting: %.96s.", id ? id : "(null)");
    return DME_INVALID;
}
