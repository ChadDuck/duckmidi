/* M64-specific declarative UI. No SDL, ImGui, editor headers or renderer calls. */
#include "m64_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
/* Stock US stems from n64decomp/sm64 sound/sequences.json. */
static const DmeChoice sequences[] = {{0, "00_sound_player"},
                                      {1, "01_cutscene_collect_star"},
                                      {2, "02_menu_title_screen"},
                                      {3, "03_level_grass"},
                                      {4, "04_level_inside_castle"},
                                      {5, "05_level_water"},
                                      {6, "06_level_hot"},
                                      {7, "07_level_boss_koopa"},
                                      {8, "08_level_snow"},
                                      {9, "09_level_slide"},
                                      {10, "0A_level_spooky"},
                                      {11, "0B_event_piranha_plant"},
                                      {12, "0C_level_underground"},
                                      {13, "0D_menu_star_select"},
                                      {14, "0E_event_powerup"},
                                      {15, "0F_event_metal_cap"},
                                      {16, "10_event_koopa_message"},
                                      {17, "11_level_koopa_road"},
                                      {18, "12_event_high_score"},
                                      {19, "13_event_merry_go_round"},
                                      {20, "14_event_race"},
                                      {21, "15_cutscene_star_spawn"},
                                      {22, "16_event_boss"},
                                      {23, "17_cutscene_collect_key"},
                                      {24, "18_event_endless_stairs"},
                                      {25, "19_level_boss_koopa_final"},
                                      {26, "1A_cutscene_credits"},
                                      {27, "1B_event_solve_puzzle"},
                                      {28, "1C_event_toad_message"},
                                      {29, "1D_event_peach_message"},
                                      {30, "1E_cutscene_intro"},
                                      {31, "1F_cutscene_victory"},
                                      {32, "20_cutscene_ending"},
                                      {33, "21_menu_file_select"},
                                      {34, "22_cutscene_lakitu"},
                                      {35, "Custom"}};
#define CUSTOM_SEQUENCE 35

static const DmeChoice memory_choices[] = {{M64_MEMORY_ENLARGE, "Enlarge"},
                                           {M64_MEMORY_STOCK, "Stock"}};

static const DmeChoice target_choices[] = {
    {M64_GAME_SM64, "SM64"}, {M64_GAME_HACKERSM64, "HackerSM64"}, {M64_GAME_COOPDX, "sm64coopdx"}};

int m64_stock_sequence(const char *name) {
    for (int i = 0; i < CUSTOM_SEQUENCE; i++) {
        if (!strcmp(sequences[i].label, name)) {
            return i;
        }
    }

    return -1;
}

static void reset_maps(M64PluginUi *s) {
    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned p = 0; p < 128; p++) {
            s->melodic[ch][p] = (int)p;
        }
    }

    for (unsigned p = 0; p < 128; p++) {
        s->drums[p] = (int)p;
    }

    s->similar_stale = true;
}

void DME_CALL m64_ui_reset(void *instance) {
    M64PluginUi *s = instance;
    snprintf(s->sequence, sizeof(s->sequence), "03_level_grass");
    snprintf(s->bank, sizeof(s->bank), "26_custom_duckmidi");
    s->sequence_choice = 3;
    snprintf(s->custom_sequence, sizeof(s->custom_sequence), "%s", s->project_sequence);
    s->custom_sequence_set = false;
    s->target = M64_GAME_SM64;
    s->loop = 0;
    s->loop_points = 1;
    s->memory = M64_MEMORY_ENLARGE;
    s->percussion = 0;
    reset_maps(s);
}

void *DME_CALL m64_ui_create(void) {
    M64PluginUi *s = calloc(1, sizeof(*s));

    if (!s) {
        return NULL;
    }

    m64_ui_reset(s);

    for (unsigned i = 0; i < 128; i++) {
        snprintf(s->program_names[i], 80, "GM program %u", i);
        snprintf(s->drum_names[i], 80, "GM drum note %u", i);
    }

    return s;
}

void DME_CALL m64_ui_destroy(void *instance) {
    free(instance);
}

/* A sequence file stem from the project name, or the MIDI file name without its folder and
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

    for (const char *p = from; *p && n < 96; p++) {
        char ch = *p >= 'A' && *p <= 'Z' ? (char)(*p - 'A' + 'a') : *p;

        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            out[n++] = ch;
        } else if (n && out[n - 1] != '_') {
            out[n++] = '_';
        }
    }

    while (n && out[n - 1] == '_') {
        n--;
    }

    out[n] = 0;
}

int32_t DME_CALL m64_ui_inspect(void *instance, const DmeContext *c, char *error, uint64_t cap) {
    (void)error;
    (void)cap;
    M64PluginUi *s = instance;

    if (!c || (!c->events && c->event_count)) {
        return DME_INVALID;
    }

    if (s->session_known && s->session != c->project_session) {
        reset_maps(s);
        s->custom_sequence_set = false;
    }

    project_sequence_name(c, s->project_sequence);

    /* Until the user types or picks a custom name, Custom offers the project's name. */
    if (!s->custom_sequence_set) {
        snprintf(s->custom_sequence, sizeof(s->custom_sequence), "%s", s->project_sequence);

        if (s->sequence_choice == CUSTOM_SEQUENCE) {
            snprintf(s->sequence, sizeof(s->sequence), "%s", s->custom_sequence);
        }
    }

    s->similar_stale = true;
    memset(s->demand, 0, sizeof(s->demand));
    s->session = c->project_session;
    s->session_known = true;
    s->has_loop_points = c->loop_points != 0;
    memset(s->used, 0, sizeof(s->used));
    memset(s->used_drums, 0, sizeof(s->used_drums));

    for (unsigned i = 0; i < 128; i++) {
        if (c->program_names[i]) {
            snprintf(s->program_names[i], 80, "%s", c->program_names[i]);
        }

        if (c->drum_names[i]) {
            snprintf(s->drum_names[i], 80, "%s", c->drum_names[i]);
        }
    }

    M64PortLayout *layout = calloc(1, sizeof(*layout));

    if (!layout || !m64_port_layout(c->events, c->event_count, c->percussion_channels, layout)) {
        free(layout);
        return DME_NOMEM;
    }

    snprintf(s->port_notice, sizeof(s->port_notice), "%s", layout->notice);
    s->port_interference = layout->interference;
    unsigned program[16] = {0};

    for (uint64_t i = 0; i < c->event_count; i++) {
        const DmeEvent *e = &c->events[i];

        if (e->status >= MIDI_SYSTEM || e->a >= 128) {
            continue;
        }

        unsigned type = e->status & MIDI_STATUS_MASK;
        unsigned ch = e->port < M64_PORTS ? layout->out[e->status & MIDI_CHANNEL_MASK][e->port]
                                          : e->status & MIDI_CHANNEL_MASK;

        if (type == MIDI_PROGRAM) {
            program[ch] = e->a;
        }

        if (type == MIDI_NOTE_ON && e->b) {
            if (ch == MIDI_DRUM_CHANNEL) {
                s->used_drums[e->a] = true;
            } else {
                /* Pitch bends can raise the rate further; the export review checks those. */
                unsigned p = program[ch];
                double rate = pow(2.0, ((double)e->a - 60) / 12.0);
                s->used[ch][p] = true;
                s->demand[ch][p] = rate > s->demand[ch][p] ? rate : s->demand[ch][p];
            }
        }
    }

    free(layout);
    return DME_OK;
}

int32_t DME_CALL m64_ui_describe(void *instance, DmeUi *ui) {
    M64PluginUi *s = instance;
    bool drums = s->percussion != 0;

    /* The SM64 side lists each stock sample once; see samples.c. */
    unsigned choices = m64_preset_choices(drums, s->choices, s->choice_labels);
    unsigned count = 0;

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned p = 0; p < 128; p++) {
            if (drums ? !(ch == MIDI_DRUM_CHANNEL && s->used_drums[p]) : !s->used[ch][p]) {
                continue;
            }

            if (drums) {
                snprintf(s->row_ids[count], 40, "drum.%u", p);
            } else {
                snprintf(s->row_ids[count], 40, "melodic.%u.%u", ch, p);
            }

            snprintf(s->row_labels[count], 180, "Ch %u: %s%s%u", ch + 1,
                     drums ? s->drum_names[p] : s->program_names[p], drums ? " / note " : " / GM ",
                     p);
            int mapped = drums ? s->drums[p] : s->melodic[ch][p];
            int value = m64_canonical_preset(drums, mapped);

            if (s->similar_stale) {
                M64RangeIssue notes = {.channel = ch,
                                       .program = p,
                                       .percussion = drums,
                                       .demand = drums ? 1 : s->demand[ch][p]};
                s->similar_count[count] =
                    m64_similar(&notes, (int)p, s->choices, choices, s->similar[count], 8);
            }

            s->rows[count] = (DmeMappingRow){s->row_ids[count],
                                             s->row_labels[count],
                                             value,
                                             m64_canonical_preset(drums, (int)p),
                                             s->similar[count],
                                             s->similar_count[count]};
            count++;
        }
    }

    s->similar_stale = false;
    unsigned f = 0;

    if (*s->port_notice) {
        s->fields[f++] = (DmeField){.id = "ports", .label = s->port_notice, .type = DME_NOTICE};
    }

    s->fields[f++] = (DmeField){
        .id = "target",
        .label = "Target",
        .help =
            "The game the saved package is made for. Export to Repository detects the checkout and installs for its game.",
        .type = DME_CHOICE,
        .value = {DME_CHOICE, s->target, NULL},
        .choices = target_choices,
        .choice_count = M64_GAME_COUNT};
    s->fields[f++] = (DmeField){.id = "sequence",
                                .label = "Sequence name",
                                .type = DME_CHOICE,
                                .value = {DME_CHOICE, s->sequence_choice, NULL},
                                .choices = sequences,
                                .choice_count = sizeof(sequences) / sizeof(*sequences)};

    if (s->sequence_choice == CUSTOM_SEQUENCE) {
        s->fields[f++] = (DmeField){.id = "sequence_name",
                                    .label = "Custom sequence name",
                                    .type = DME_TEXT,
                                    .value = {DME_TEXT, 0, s->sequence},
                                    .text_capacity = 97};
    }

    /* sm64coopdx songs use its existing extended bank. */
    if (s->target != M64_GAME_COOPDX) {
        s->fields[f++] = (DmeField){.id = "bank_name",
                                    .label = "Bank name",
                                    .type = DME_TEXT,
                                    .value = {DME_TEXT, 0, s->bank},
                                    .text_capacity = 97};
    }
    /* sm64coopdx mods use no sequence pool. */
    if (s->target != M64_GAME_COOPDX) {
        bool hacker = s->target == M64_GAME_HACKERSM64;
        s->fields[f++] = (DmeField){
            .id = "memory",
            .label = "Sequence memory",
            .help =
                hacker
                    ? "Enlarge raises TEMPORARY_SEQ_MEM when the song needs more. Stock changes no game files besides the export itself."
                    : "Enlarge makes the smallest memory change the song needs, and the ROM still runs on console. Stock changes no game files besides the export itself.",
            .type = DME_CHOICE,
            .value = {DME_CHOICE, s->memory, NULL},
            .choices = memory_choices,
            .choice_count = 2};
    }

    s->fields[f++] = (DmeField){
        .id = "loop", .label = "Loop song", .type = DME_BOOL, .value = {DME_BOOL, s->loop, NULL}};

    if (s->has_loop_points) {
        s->fields[f++] = (DmeField){
            .id = "loop_points",
            .label = "Use M64 loop points",
            .help =
                "Play once, then repeat from M64 - Loop Start to M64 - Loop End. Overrides Loop song.",
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
                   .help = "Choose a used source on the left, then its SM64 mapping on the right.",
                   .type = DME_MAPPING,
                   .choices = s->choices,
                   .choice_count = choices,
                   .rows = s->rows,
                   .row_count = count};
    s->fields[f++] =
        (DmeField){.id = "reset_mappings", .label = "Reset SM64 mappings", .type = DME_ACTION};
    *ui = (DmeUi){s->fields, f};
    return DME_OK;
}

int32_t DME_CALL m64_ui_set(void *instance, const char *id, const DmeValue *v, char *error,
                            uint64_t cap) {
    M64PluginUi *s = instance;
    unsigned ch, p;
    char tail;

    if (!id || !v) {
        goto invalid;
    }

    s->similar_stale = true;

    if (!strcmp(id, "sequence") && v->type == DME_CHOICE && v->integer >= 0 &&
        v->integer <= CUSTOM_SEQUENCE) {
        if (v->integer == CUSTOM_SEQUENCE) {
            /* Without a project name, Custom starts from the chosen stock name. */
            if (!*s->custom_sequence) {
                memmove(s->custom_sequence, s->sequence, strlen(s->sequence) + 1);
            }

            memmove(s->sequence, s->custom_sequence, strlen(s->custom_sequence) + 1);
        } else {
            snprintf(s->sequence, 97, "%s", sequences[v->integer].label);
        }

        s->sequence_choice = v->integer;
        return DME_OK;
    }

    if (!strcmp(id, "target") && v->type == DME_CHOICE && v->integer >= 0 &&
        v->integer < M64_GAME_COUNT) {
        s->target = v->integer;
        return DME_OK;
    }

    if (!strcmp(id, "memory") && v->type == DME_CHOICE && v->integer >= M64_MEMORY_ENLARGE &&
        v->integer <= M64_MEMORY_STOCK) {
        s->memory = v->integer;
        return DME_OK;
    }

    if (v->type == DME_TEXT && v->text && strlen(v->text) < 97) {
        char *dest = !strcmp(id, "sequence_name") && s->sequence_choice == CUSTOM_SEQUENCE
                         ? s->sequence
                     : !strcmp(id, "bank_name") ? s->bank
                                                : NULL;

        if (dest) {
            memmove(dest, v->text, strlen(v->text) + 1);

            if (dest == s->sequence) {
                memmove(s->custom_sequence, s->sequence, strlen(s->sequence) + 1);
                s->custom_sequence_set = true;
            }

            return DME_OK;
        }
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

    if (v->type == DME_CHOICE && v->integer >= 0 && v->integer < 128) {
        if (sscanf(id, "melodic.%u.%u%c", &ch, &p, &tail) == 2 && ch < 16 &&
            ch != MIDI_DRUM_CHANNEL && p < 128 && s->used[ch][p]) {
            s->melodic[ch][p] = v->integer;
            return DME_OK;
        }

        if (sscanf(id, "drum.%u%c", &p, &tail) == 1 && p < 128 && s->used_drums[p]) {
            s->drums[p] = v->integer;
            return DME_OK;
        }
    }

    if (!strcmp(id, "reset_mappings") && v->type == DME_ACTION) {
        reset_maps(s);
        return DME_OK;
    }

invalid:
    snprintf(error, (size_t)cap, "Invalid SM64 exporter setting: %.96s.", id ? id : "(null)");
    return DME_INVALID;
}
