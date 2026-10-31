/* M64-specific declarative UI. No SDL, ImGui, editor headers or renderer calls. */
#include "m64_plugin.h"
#include <stdio.h>
#include <stdlib.h>
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

static void reset_maps(M64PluginUi *s) {
    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned p = 0; p < 128; p++) {
            s->melodic[ch][p] = (int)p;
        }
    }

    for (unsigned p = 0; p < 128; p++) {
        s->drums[p] = (int)p;
    }
}

void DME_CALL m64_ui_reset(void *instance) {
    M64PluginUi *s = instance;
    snprintf(s->sequence, sizeof(s->sequence), "03_level_grass");
    snprintf(s->bank, sizeof(s->bank), "26_custom_duckmidi");
    s->sequence_choice = 3;
    s->custom_sequence[0] = 0;
    s->custom_sequence_set = false;
    s->loop = 0;
    s->loop_points = 1;
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

int32_t DME_CALL m64_ui_inspect(void *instance, const DmeContext *c, char *error, uint64_t cap) {
    (void)error;
    (void)cap;
    M64PluginUi *s = instance;

    if (!c || (!c->events && c->event_count)) {
        return DME_INVALID;
    }

    if (s->session_known && s->session != c->project_session) {
        reset_maps(s);
    }

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

    unsigned program[16] = {0};

    for (uint64_t i = 0; i < c->event_count; i++) {
        const DmeEvent *e = &c->events[i];

        if (e->status >= MIDI_SYSTEM || e->a >= 128) {
            continue;
        }

        unsigned ch = e->status & MIDI_CHANNEL_MASK, type = e->status & MIDI_STATUS_MASK;

        if (type == MIDI_PROGRAM) {
            program[ch] = e->a;
        }

        if (type == MIDI_NOTE_ON && e->b) {
            if (ch == MIDI_DRUM_CHANNEL) {
                s->used_drums[e->a] = true;
            } else {
                s->used[ch][program[ch]] = true;
            }
        }
    }

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
            int value = m64_canonical_preset(drums, drums ? s->drums[p] : s->melodic[ch][p]);
            s->rows[count] = (DmeMappingRow){s->row_ids[count], s->row_labels[count], value,
                                             m64_canonical_preset(drums, (int)p)};
            count++;
        }
    }

    unsigned f = 0;
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

    s->fields[f++] = (DmeField){.id = "bank_name",
                                .label = "Bank name",
                                .type = DME_TEXT,
                                .value = {DME_TEXT, 0, s->bank},
                                .text_capacity = 97};
    s->fields[f++] = (DmeField){.id = "loop",
                                .label = "Loop the complete song",
                                .type = DME_BOOL,
                                .value = {DME_BOOL, s->loop, NULL}};

    if (s->has_loop_points) {
        s->fields[f++] = (DmeField){
            .id = "loop_points",
            .label = "Use M64 loop points",
            .help =
                "Play once from the start, then repeat from M64 - Loop Start to M64 - Loop End (or the song end). Overrides Loop the complete song.",
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

    if (!strcmp(id, "sequence") && v->type == DME_CHOICE && v->integer >= 0 &&
        v->integer <= CUSTOM_SEQUENCE) {
        if (v->integer == CUSTOM_SEQUENCE) {
            if (!s->custom_sequence_set) {
                memmove(s->custom_sequence, s->sequence, strlen(s->sequence) + 1);
                s->custom_sequence_set = true;
            }

            memmove(s->sequence, s->custom_sequence, strlen(s->custom_sequence) + 1);
        } else {
            snprintf(s->sequence, 97, "%s", sequences[v->integer].label);
        }

        s->sequence_choice = v->integer;
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
