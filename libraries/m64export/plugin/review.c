/* M64 pre-export mapping UI data and recommendation policy. No editor/UI toolkit. */
#include "m64_plugin.h"
#include "../src/custom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
    M64RangeIssue issue;
    int selected;
    char id[64], label[180], description[384], current[256];
    DmeChoice recommended[8];
    unsigned recommended_count;
} ReviewItem;

struct M64ReviewUi {
    unsigned count;
    ReviewItem *items;
    DmeReviewRow *rows;
    DmeChoice melodic[M64_CHOICE_LIMIT], drums[M64_CHOICE_LIMIT];
    char melodic_labels[M64_CHOICE_LIMIT][220], drum_labels[M64_CHOICE_LIMIT][220];
    unsigned melodic_count, drum_count;
};

static int drum_family(int note) {
    if (note == 35 || note == 36) {
        return 0;
    }

    if (note == 37 || note == 38 || note == 39 || note == 40) {
        return 1;
    }

    if (note == 41 || note == 43 || note == 45 || note == 47 || note == 48 || note == 50) {
        return 2;
    }

    if (note == 42 || note == 44 || note == 46) {
        return 3;
    }

    if (note == 49 || note == 51 || note == 52 || note == 53 || note == 55 || note == 57 ||
        note == 59) {
        return 4;
    }

    if (note >= 60 && note <= 81) {
        return 5;
    }

    return 6;
}

static int score(int original, int preset, bool percussion) {
    if (percussion) {
        return (drum_family(original) == drum_family(preset) ? 0 : 200) + abs(original - preset);
    }

    int value = abs(original / 8 - preset / 8) * 100 + abs(original - preset);
    /* Prefer names matching the actual native timbre over GM approximations
       sharing that sample (e.g. French Horn before Tuba for the stock horns). */
    if ((original / 8 == 7 && preset == 60) || (original / 8 == 5 && preset == 40) ||
        (original / 8 == 0 && preset == 0) || (original / 8 == 10 && preset == 80)) {
        value -= 16;
    }

    return value;
}

/* Adds preset to out when it plays the issue's notes, is listed, and is not the current mapping or
   already added. Presets sharing a sample and its settings are one choice. */
static void add_similar(const M64RangeIssue *issue, int preset, int current,
                        const DmeChoice *choices, unsigned count, DmeChoice *out, unsigned *found) {
    bool percussion = issue->percussion != 0;

    if (!m64_get_api_3()->preset_supported(issue, (uint32_t)preset)) {
        return;
    }

    int canonical = m64_canonical_preset(percussion, preset);
    bool skip = canonical == current;

    for (unsigned n = 0; n < *found; n++) {
        skip |= out[n].value == canonical;
    }

    for (unsigned c = 0; !skip && c < count; c++) {
        if (choices[c].value == canonical) {
            out[(*found)++] = choices[c];
            break;
        }
    }
}

unsigned m64_similar(const M64RangeIssue *issue, int original, const DmeChoice *choices,
                     unsigned count, DmeChoice *out, unsigned capacity) {
    const M64Api *api = m64_get_api_3();
    bool percussion = issue->percussion != 0, visited[128] = {0};
    int current = m64_canonical_preset(percussion, original);
    unsigned found = 0;

    /* A melodic source offers its program's default and the catalog's curated alternates. */
    if (!percussion && issue->program < 128) {
        uint32_t list[8] = {issue->program};
        uint32_t listed = 1 + api->program_alternates(issue->program, list + 1, 7);

        for (uint32_t i = 0; i < listed && found < capacity; i++) {
            add_similar(issue, (int)list[i], current, choices, count, out, &found);
        }
    }

    /* Drums, and a melodic source none of whose curated presets reach its notes, fall back to
       the nearest supported presets by GM number. */
    bool nearest = percussion || !found;

    for (unsigned pass = 0; nearest && pass < 128 && found < capacity; pass++) {
        int best = -1, best_score = INT_MAX;

        for (int candidate = 0; candidate < 128; candidate++) {
            if (visited[candidate] || (percussion && (candidate < 35 || candidate > 81)) ||
                !api->preset_supported(issue, (uint32_t)candidate)) {
                continue;
            }

            int rank = score(original, candidate, percussion);

            if (rank < best_score) {
                best = candidate;
                best_score = rank;
            }
        }

        if (best < 0) {
            break;
        }

        visited[best] = true;
        add_similar(issue, best, current, choices, count, out, &found);
    }

    return found;
}

static DmeChoice choice_for(const M64ReviewUi *ui, bool percussion, int value) {
    const DmeChoice *choices = percussion ? ui->drums : ui->melodic;
    unsigned count = percussion ? ui->drum_count : ui->melodic_count;

    for (unsigned i = 0; i < count; ++i) {
        if (choices[i].value == value) {
            return choices[i];
        }
    }

    return choices[0];
}

void m64_review_free(M64PluginJob *job) {
    if (!job || !job->review_ui) {
        return;
    }

    free(job->review_ui->items);
    free(job->review_ui->rows);
    free(job->review_ui);
    job->review_ui = NULL;
}

bool m64_review_build(M64PluginJob *job, const DmeContext *context, const M64Result *result) {
    const M64Api *api = m64_get_api_3();
    m64_review_free(job);
    uint64_t count = api->range_count(result);

    if (!count) {
        return true;
    }

    if (count > DME_ROW_LIMIT) {
        return false;
    }

    M64ReviewUi *ui = calloc(1, sizeof(*ui));

    if (!ui) {
        return false;
    }

    job->review_ui = ui;
    ui->count = (unsigned)count;
    ui->items = calloc(ui->count, sizeof(*ui->items));
    ui->rows = calloc(ui->count, sizeof(*ui->rows));

    if (!ui->items || !ui->rows) {
        m64_review_free(job);
        return false;
    }

    /* Replacement choices list each stock sample once, as in the mapping panel. */
    ui->melodic_count = m64_preset_choices(false, ui->melodic, ui->melodic_labels);
    ui->drum_count = m64_preset_choices(true, ui->drums, ui->drum_labels);

    for (unsigned i = 0; i < ui->count; i++) {
        ReviewItem *item = &ui->items[i];

        if (api->range_at(result, i, &item->issue) != M64_OK) {
            m64_review_free(job);
            return false;
        }

        M64RangeIssue *issue = &item->issue;
        unsigned ch = issue->channel, p = issue->program;
        uint64_t origin;
        if (!m64_range_origin(result, i, &origin) ||
            !es_mapping_row_id(context, origin, item->id)) {
            return false;
        }

        int original = job->stock_values && job->stock_values[origin] >= 0
                           ? job->stock_values[origin]
                       : issue->percussion ? job->drum_presets[p]
                                           : job->presets[ch][p];
        snprintf(item->label, 180, "Channel %u: %s (%s %u)", ch + 1,
                 issue->percussion ? context->drum_names[p] : context->program_names[p],
                 issue->percussion ? "note" : "GM", p);
        m64_preset_label(issue->percussion != 0, original, item->current, sizeof(item->current));
        snprintf(
            item->description, 384,
            "The current stock mapping cannot cover MIDI keys %u-%u (%u notes; up to %.3fx playback, limit near 4x). Choose a supported substitute, or export unchanged and accept possible pitch clipping.",
            issue->note_low, issue->note_high, issue->note_count, issue->maximum_rate);
        item->recommended_count = m64_similar(
            issue, original, issue->percussion ? ui->drums : ui->melodic,
            issue->percussion ? ui->drum_count : ui->melodic_count, item->recommended, 8);
        item->selected = item->recommended_count
                             ? item->recommended[0].value
                             : m64_canonical_preset(issue->percussion != 0, original);
    }

    return true;
}

int32_t DME_CALL m64_review_describe(void *user, DmeReview *review) {
    M64PluginJob *job = user;
    M64ReviewUi *ui = job->review_ui;

    if (!ui) {
        *review = (DmeReview){0};
        return DME_OK;
    }

    const M64Api *api = m64_get_api_3();

    for (unsigned i = 0; i < ui->count; i++) {
        ReviewItem *item = &ui->items[i];
        ui->rows[i] = (DmeReviewRow){
            .id = item->id,
            .label = item->label,
            .description = item->description,
            .current_mapping = item->current,
            .value = item->selected,
            .supported = api->preset_supported(&item->issue, (unsigned)item->selected),
            .recommended = item->recommended,
            .recommended_count = item->recommended_count,
            .choices = item->issue.percussion ? ui->drums : ui->melodic,
            .choice_count = item->issue.percussion ? ui->drum_count : ui->melodic_count};
    }

    *review = (DmeReview){
        "Some selected mappings exceed SM64's stock sample range. Recommended substitutes preserve the note pitches but change the instrument sound. Only the affected instruments are listed below.",
        ui->rows, ui->count};
    return DME_OK;
}

int32_t DME_CALL m64_review_set(void *user, const char *id, int32_t value, char *error,
                                uint64_t cap) {
    M64PluginJob *job = user;
    M64ReviewUi *ui = job->review_ui;

    if (ui && id) {
        for (unsigned i = 0; i < ui->count; i++) {
            if (!strcmp(id, ui->items[i].id) &&
                m64_preset_valid(ui->items[i].issue.percussion != 0, value)) {
                ui->items[i].selected = value;
                return DME_OK;
            }
        }
    }

    snprintf(error, (size_t)cap, "Invalid review mapping.");
    return DME_INVALID;
}

int32_t DME_CALL m64_review_apply(void *instance, void *user, char *error, uint64_t cap) {
    (void)error;
    (void)cap;
    M64PluginUi *settings = instance;
    M64PluginJob *job = user;
    M64ReviewUi *ui = job->review_ui;
    const M64Api *api = m64_get_api_3();

    if (ui) {
        for (unsigned i = 0; i < ui->count; i++) {
            ReviewItem *item = &ui->items[i];
            unsigned p = item->issue.program, ch = item->issue.channel;
            int selected = item->selected;
            if (!es_mapping_job_set_stock(job->custom, (uint32_t)job->target, item->id, selected)) {
                return DME_NOMEM;
            }

            if (settings->target == job->target) {
                es_mappings_set_stock(settings->custom, (uint32_t)job->target, item->id, selected);
            }

            if (item->issue.percussion) {
                job->drums[p] = *api->default_drum((unsigned)selected);
                settings->drums[p] = job->drum_presets[p] = selected;
            } else {
                job->melodic[ch * 128 + p] = *api->default_program((unsigned)selected);
                settings->melodic[ch][p] = job->presets[ch][p] = selected;
            }
        }
    }

    return DME_OK;
}
