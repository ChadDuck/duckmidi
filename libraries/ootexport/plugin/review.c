/* Pre-export review: mapped OoT samples that cannot reach a source's notes, with substitutes of
   the same family that can. No editor or UI toolkit. */
#include "oot_plugin.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    OotRangeIssue issue;
    int selected;
    char id[64], label[180], description[384], current[96];
    DmeChoice recommended[8];
    unsigned recommended_count;
} ReviewItem;

struct OotReviewUi {
    unsigned count;
    ReviewItem *items;
    DmeReviewRow *rows;
    DmeChoice melodic[128], drums[128];
    char melodic_labels[128][96], drum_labels[128][96];
    unsigned melodic_count, drum_count;
};

void oot_review_free(OotPluginJob *job) {
    if (!job || !job->review_ui) {
        return;
    }

    free(job->review_ui->items);
    free(job->review_ui->rows);
    free(job->review_ui);
    job->review_ui = NULL;
}

/* Lower is closer: same instrument name, then the same family, then anything else. */
static int score(int original, int candidate, bool percussion) {
    if (percussion) {
        return (oot_drum_class(original) == oot_drum_class(candidate) ? 0 : 100) +
               abs(original - candidate);
    }

    bool same_name = oot_instrument_valid(original) && oot_instrument_valid(candidate) &&
                     original < OOT_SYNTH_BASE && candidate < OOT_SYNTH_BASE &&
                     !strcmp(oot_instruments[original].name, oot_instruments[candidate].name);
    return (same_name                                                           ? 0
            : oot_instrument_class(original) == oot_instrument_class(candidate) ? 50
                                                                                : 200) +
           abs(original - candidate);
}

unsigned oot_similar(int font, const OotRangeIssue *issue, int original, const DmeChoice *choices,
                     unsigned count, DmeChoice *out, unsigned capacity) {
    bool drum = issue->percussion, usable[128] = {false};
    unsigned found = 0;

    count = count < 128 ? count : 128;

    for (unsigned c = 0; c < count; c++) {
        int candidate = choices[c].value;
        usable[c] = candidate != original && candidate != OOT_SILENT &&
                    oot_font_range_supported(font, issue, candidate);
    }

    while (found < capacity) {
        int best = -1, best_score = INT_MAX;

        for (unsigned c = 0; c < count; c++) {
            int rank = usable[c] ? score(original, choices[c].value, drum) : INT_MAX;

            if (rank < best_score) {
                best = (int)c;
                best_score = rank;
            }
        }

        if (best < 0) {
            break;
        }

        usable[best] = false;
        out[found++] = choices[best];
    }

    return found;
}

static DmeChoice choice_for(const OotReviewUi *ui, bool percussion, int value) {
    const DmeChoice *choices = percussion ? ui->drums : ui->melodic;
    unsigned count = percussion ? ui->drum_count : ui->melodic_count;

    for (unsigned i = 0; i < count; ++i) {
        if (choices[i].value == value) {
            return choices[i];
        }
    }

    return choices[0];
}

bool oot_review_build(OotPluginJob *job, const DmeContext *context, const OotResult *result) {
    oot_review_free(job);

    if (!result->range_count) {
        return true;
    }

    if (result->range_count > 2048) {
        return false;
    }

    OotReviewUi *ui = calloc(1, sizeof(*ui));

    if (!ui) {
        return false;
    }

    job->review_ui = ui;
    ui->count = (unsigned)result->range_count;
    ui->items = calloc(ui->count, sizeof(*ui->items));
    ui->rows = calloc(ui->count, sizeof(*ui->rows));

    if (!ui->items || !ui->rows) {
        oot_review_free(job);
        return false;
    }

    int font = job->request.font;
    ui->melodic_count = oot_instrument_choices(font, ui->melodic, ui->melodic_labels, 128);
    ui->drum_count = oot_drum_choices(font, ui->drums, ui->drum_labels, 128);

    for (unsigned i = 0; i < ui->count; i++) {
        ReviewItem *item = &ui->items[i];
        item->issue = result->ranges[i];
        const OotRangeIssue *issue = &item->issue;
        bool drum = issue->percussion;
        unsigned ch = issue->channel, p = issue->program;
        int original = issue->value;
        if (!es_mapping_row_id(context, issue->source_event, item->id)) {
            return false;
        }
        snprintf(item->label, sizeof(item->label), "Channel %u: %s (%s %u)", ch + 1,
                 drum ? context->drum_names[p] : context->program_names[p], drum ? "note" : "GM",
                 p);

        if (drum) {
            oot_drum_label(original, item->current, sizeof(item->current));
        } else {
            oot_instrument_label(original, item->current, sizeof(item->current));
        }

        snprintf(item->description, sizeof(item->description),
                 "This sample cannot reach MIDI keys %u-%u (%u notes; up to %.2fx playback, limit "
                 "near 4x), so they play flat. Choose a substitute that covers them, or export "
                 "unchanged.",
                 issue->low, issue->high, issue->count, issue->rate);
        item->recommended_count =
            oot_similar(font, issue, original, drum ? ui->drums : ui->melodic,
                        drum ? ui->drum_count : ui->melodic_count, item->recommended, 8);
        item->selected = item->recommended_count ? item->recommended[0].value
                                                 : choice_for(ui, drum, original).value;
    }

    return true;
}

int32_t DME_CALL oot_review_describe(void *user, DmeReview *review) {
    OotPluginJob *job = user;
    OotReviewUi *ui = job->review_ui;

    if (!ui) {
        *review = (DmeReview){0};
        return DME_OK;
    }

    for (unsigned i = 0; i < ui->count; i++) {
        ReviewItem *item = &ui->items[i];
        bool drum = item->issue.percussion;
        ui->rows[i] = (DmeReviewRow){
            .id = item->id,
            .label = item->label,
            .description = item->description,
            .current_mapping = item->current,
            .value = item->selected,
            .supported = oot_range_supported(&job->request, &item->issue, item->selected),
            .recommended = item->recommended,
            .recommended_count = item->recommended_count,
            .choices = drum ? ui->drums : ui->melodic,
            .choice_count = drum ? ui->drum_count : ui->melodic_count};
    }

    *review = (DmeReview){
        "Some mapped OoT samples cannot play every note their source uses: the driver plays a "
        "sample at most about 4x its recorded rate. Recommended substitutes are the closest "
        "instruments that cover the notes. Only the affected sources are listed.",
        ui->rows, ui->count};
    return DME_OK;
}

int32_t DME_CALL oot_review_set(void *user, const char *id, int32_t value, char *error,
                                uint64_t cap) {
    OotPluginJob *job = user;
    OotReviewUi *ui = job->review_ui;

    if (ui && id) {
        for (unsigned i = 0; i < ui->count; i++) {
            ReviewItem *item = &ui->items[i];

            if (strcmp(id, item->id)) {
                continue;
            }

            bool drum = item->issue.percussion;
            const DmeChoice *choices = drum ? ui->drums : ui->melodic;
            unsigned count = drum ? ui->drum_count : ui->melodic_count;

            for (unsigned c = 0; c < count; c++) {
                if (choices[c].value == value) {
                    item->selected = value;
                    return DME_OK;
                }
            }
        }
    }

    snprintf(error, (size_t)cap, "Invalid review mapping.");
    return DME_INVALID;
}

int32_t DME_CALL oot_review_apply(void *instance, void *user, char *error, uint64_t cap) {
    (void)error;
    (void)cap;
    OotPluginUi *settings = instance;
    OotPluginJob *job = user;
    OotReviewUi *ui = job->review_ui;

    if (!ui) {
        return DME_OK;
    }

    /* Settings follow only when they still describe the same soundfont. */
    bool same_font = oot_ui_font(settings) == job->request.font;

    for (unsigned i = 0; i < ui->count; i++) {
        const ReviewItem *item = &ui->items[i];
        unsigned p = item->issue.program, ch = item->issue.channel;
        if (!es_mapping_job_set_stock(job->custom, 0, item->id, item->selected)) {
            return DME_NOMEM;
        }

        if (same_font) {
            es_mappings_set_stock(settings->custom, 0, item->id, item->selected);
        }

        if (item->issue.percussion) {
            job->request.drums[p] = item->selected;

            if (same_font) {
                settings->drums[p] = item->selected;
            }
        } else {
            job->request.instruments[ch][p] = item->selected;

            if (same_font) {
                settings->melodic[ch][p] = item->selected;
            }
        }
    }

    return DME_OK;
}
