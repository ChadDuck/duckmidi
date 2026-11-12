/* duckmidi plugin adapter for OoT export. Conversion and repository patches stay in ../src;
   settings in ui.c; substitutes in review.c. */
#define DME_BUILD_PLUGIN
#include "oot_plugin.h"
#include "repository.h"
#include "../../../src/text/en.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef OotPluginJob ExportJob;

#define MAX_FONTS 48 /* fontLoadStatus[0x30] in include/audio.h */

static bool name_allowed(const char *name, char *error, uint64_t cap) {
    if (!oot_name_valid(name)) {
        snprintf(error, (size_t)cap,
                 "The sequence name must start with a lowercase letter and use only lowercase "
                 "letters, digits and _ (at most 48).");
        return false;
    }

    /* seq_N names belong to the extracted vanilla sequences. */
    if (!strncmp(name, "seq_", 4)) {
        snprintf(error, (size_t)cap, "Choose a sequence name that does not start with seq_.");
        return false;
    }

    return true;
}

static void *DME_CALL create_job(const void *instance, char *error, uint64_t cap) {
    const OotPluginUi *s = instance;

    if (!name_allowed(s->name, error, cap)) {
        return NULL;
    }

    ExportJob *j = calloc(1, sizeof(*j));

    if (!j) {
        snprintf(error, (size_t)cap, "Cannot allocate OoT export settings.");
        return NULL;
    }

    snprintf(j->checkout, sizeof(j->checkout), "%s", s->checkout);
    snprintf(j->checkout_identity, sizeof(j->checkout_identity), "%s", s->checkout_identity);

    j->custom = es_mappings_freeze(s->custom, error, (size_t)cap);

    if (!j->custom) {
        free(j);
        return NULL;
    }

    snprintf(j->name, sizeof(j->name), "%s", s->name);
    j->song = s->song > 0 && s->song <= (int)oot_song_count ? oot_songs[s->song - 1].id : NULL;
    j->loop_points = s->loop_points && s->has_loop_points;
    j->enlarge = s->memory == 0;
    OotRequest *q = &j->request;
    memcpy(q->instruments, s->melodic, sizeof(q->instruments));
    memcpy(q->drums, s->drums, sizeof(q->drums));
    q->font = oot_ui_font(s);
    q->font_index = 38;
    q->volume = (uint32_t)s->volume;
    q->enlarge_memory = j->enlarge;
    q->loop = s->loop ? OOT_LOOP_SONG : OOT_LOOP_NONE;
    q->name = j->name;
    return j;
}

/* Copies the arrangement with colored ports moved to their channels and applies loop points. */
static int32_t prepare(ExportJob *j, const DmeContext *c, const DmeHost *host, char *error,
                       uint64_t cap) {
    if (!c || c->event_count > 2000000 || (!c->events && c->event_count) || !c->ppq) {
        snprintf(error, (size_t)cap, "Invalid OoT source timeline.");
        return DME_INVALID;
    }

    /* Only the generated Custom soundfont can hold custom samples. With a stock Sample Set the
       Custom rows play their ordinary instruments. */
    if (j->request.font != OOT_CUSTOM_FONT) {
        if (es_mapping_job_custom(j->custom, 0) && !j->samples.fixes[ES_FIX_IGNORED].count) {
            es_native_fix(&j->samples, ES_FIX_IGNORED, NULL);
        }
    } else if (!es_native_samples(&j->samples, j->custom, 0, ES_NATIVE_OOT, c, host, error,
                                  (size_t)cap)) {
        return host->cancelled(host->user) ? DME_CANCELLED : DME_FAILED;
    }

    j->request.custom = j->samples.count ? &j->samples : NULL;
    j->request.source_patches = c->effective_patches;

    bool replacing = false;

    for (size_t i = 0; i < j->samples.count; i++) {
        replacing |= j->samples.regions[i].target != NULL;
    }

    EsRepository *probe = NULL;

    if (replacing && !j->repository) {
        probe = *j->checkout
                    ? es_repository_probe(j->checkout, &oot_history_adapter, error, (size_t)cap)
                    : NULL;

        /* A configured checkout that cannot be opened, such as a locked history, is an error;
           only a missing or different checkout turns Replace into Add. */
        if (*j->checkout && !probe) {
            return DME_FAILED;
        }

        /* Without the inspected checkout a saved package cannot replace its samples, so the
           Replace regions are added as new samples instead. */
        if (!probe || strcmp(j->checkout_identity, es_repository_identity(probe))) {
            es_repository_close(probe);
            probe = NULL;
            *error = 0;

            for (size_t i = 0; i < j->samples.count; i++) {
                if (j->samples.regions[i].target) {
                    es_native_add_instead(&j->samples, &j->samples.regions[i]);
                }
            }
        }
    }

    EsRepository *target_repository = j->repository ? j->repository : probe;
    bool targets_ok = !target_repository || oot_repository_sample_targets(j, target_repository, c,
                                                                          host, error, (size_t)cap);
    es_repository_close(probe);

    if (!targets_ok) {
        return DME_FAILED;
    }

    free(j->events);
    j->events = calloc((size_t)c->event_count + 1, sizeof(*j->events));
    free(j->stock_values);
    j->stock_values = malloc(((size_t)c->event_count + 1) * sizeof(*j->stock_values));
    EsPortLayout *layout = calloc(1, sizeof(*layout));

    if (!j->events || !j->stock_values || !layout ||
        !es_mapping_job_values(j->custom, 0, c, j->stock_values, error, (size_t)cap) ||
        !es_port_layout(c->events, c->event_count, c->percussion_channels, layout)) {
        snprintf(error, (size_t)cap, "Cannot allocate the OoT timeline.");
        free(layout);
        return DME_NOMEM;
    }

    for (uint64_t i = 0; i < c->event_count; i++) {
        DmeEvent e = c->events[i];

        if (e.status >= MIDI_STATUS && e.status < MIDI_SYSTEM && e.port < ES_PORTS) {
            e.status =
                (e.status & MIDI_STATUS_MASK) | layout->out[e.status & MIDI_CHANNEL_MASK][e.port];
        }

        j->events[i] = e;
    }

    snprintf(j->port_notice, sizeof(j->port_notice), "%s",
             layout->interference ? layout->merges : "");
    free(layout);
    OotRequest *q = &j->request;
    q->events = j->events;
    q->sample_bank = *j->sample_bank ? j->sample_bank : "assets/audio/samplebanks/SampleBank_0.xml";
    q->stock_values = j->stock_values;
    q->event_count = c->event_count;
    q->duration = c->duration;
    q->ppq = c->ppq;

    if (j->loop_points && c->loop_points) {
        uint64_t start, end;

        if (!es_loop_region(c, &start, &end, error, (size_t)cap)) {
            return DME_INVALID;
        }

        q->loop = OOT_LOOP_REGION;
        q->loop_start = start;
        q->loop_end = end;
    }

    return DME_OK;
}

static void report(const DmeHost *host, const OotResult *result) {
    for (size_t i = 0; i < result->diagnostic_count; i++) {
        const OotDiagnostic *d = &result->diagnostics[i];
        uint32_t flags = DME_DISPLAY | d->flags | (d->severity == DME_WARNING ? DME_POPUP : 0);
        host->diagnostic(host->user, d->severity, flags, d->text);
    }
}

/* Converts until every custom note fits the engine: es_native_fit resamples regions played
   above the 4x rate limit and falls back selections that cannot fit. A trial result is never
   published; the final conversion runs without the trial arrays. */
static int32_t convert_fitted(ExportJob *j, const DmeHost *host, bool analyze, OotResult *result,
                              char *error, uint64_t cap) {
    static const char default_bank[] = "assets/audio/samplebanks/SampleBank_0.xml";
    double overspeed[ES_NATIVE_REGIONS];
    bool capacity[ES_NATIVE_REGIONS];

    for (size_t attempt = 0; j->samples.count && attempt <= ES_NATIVE_REGIONS; attempt++) {
        memset(overspeed, 0, sizeof(overspeed));
        memset(capacity, 0, sizeof(capacity));
        j->request.overspeed = overspeed;
        j->request.capacity = capacity;
        int32_t status = oot_convert(&j->request, host, analyze, result, error, (size_t)cap);
        j->request.overspeed = NULL;
        j->request.capacity = NULL;

        if (status != DME_OK) {
            return status;
        }

        bool other = *j->sample_bank && strcmp(j->sample_bank, default_bank);
        int fit = es_native_fit(&j->samples, overspeed, capacity, ES_NATIVE_OOT, &other, host,
                                error, (size_t)cap);

        if (fit == ES_FIT_DONE) {
            return status;
        }

        oot_result_free(result);

        if (fit == ES_FIT_FAILED) {
            return host->cancelled(host->user) ? DME_CANCELLED : DME_NOMEM;
        }

        if (!other) {
            j->sample_bank[0] = 0;
            j->request.sample_bank = default_bank;
        }
    }

    return oot_convert(&j->request, host, analyze, result, error, (size_t)cap);
}

static int32_t convert(ExportJob *j, const DmeContext *c, const DmeHost *host, char *error,
                       uint64_t cap, OotResult *result) {
    *result = (OotResult){0};
    int32_t status = prepare(j, c, host, error, cap);

    if (status != DME_OK) {
        return status;
    }

    /* Each interfering merge is one warning-list item. */
    for (const char *line = j->port_notice; *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        char item[512];
        snprintf(item, sizeof(item), "%s\n%.*s", DM_TEXT_EXPORT_LIST_MERGED, (int)length, line);
        host->diagnostic(host->user, DME_WARNING, DME_DISPLAY | DME_POPUP | DME_LIST, item);
        line += length + (end != NULL);
    }

    status = convert_fitted(j, host, false, result, error, cap);
    es_native_samples_report(&j->samples, ES_NATIVE_OOT, host);
    report(host, result);

    if (host->cancelled(host->user)) {
        return DME_CANCELLED;
    }

    if (status != DME_OK && !*error) {
        snprintf(error, (size_t)cap, "OoT conversion failed.");
    }

    return status;
}

static int32_t DME_CALL run(void *job, const DmeContext *c, const DmeHost *host, char *error,
                            uint64_t cap) {
    ExportJob *j = job;
    OotResult result;
    int32_t status = convert(j, c, host, error, cap, &result);
    bool font = j->request.font == OOT_CUSTOM_FONT;
    char name[128];
    bool ok = status == DME_OK;

    bool replacing = false;

    for (size_t i = 0; i < j->samples.count; i++) {
        replacing |= j->samples.regions[i].target != NULL;
    }

    if (ok && replacing) {
        EsRepository *probe =
            es_repository_probe(j->checkout, &oot_history_adapter, error, (size_t)cap);
        ok = probe && !strcmp(j->checkout_identity, es_repository_identity(probe)) &&
             oot_repository_package(j, probe, &result, host, error, (size_t)cap);
        es_repository_close(probe);
        oot_result_free(&result);
        return ok ? DME_OK : DME_FAILED;
    }

    if (ok) {
        snprintf(name, sizeof(name), "%s.seq", j->name);
        ok = host->artifact(host->user, name, (const uint8_t *)result.sequence.data,
                            result.sequence.size);
    }

    if (ok && font) {
        snprintf(name, sizeof(name), "Soundfont_%s.xml", j->name);
        ok = host->artifact(host->user, name, (const uint8_t *)result.soundfont.data,
                            result.soundfont.size);
    }

    OotText sample_entries = {0};

    for (size_t i = 0; ok && i < j->samples.count; i++) {
        const EsNativeRegion *region = &j->samples.regions[i];

        if (region->target) {
            snprintf(error, (size_t)cap, "%s", DM_TEXT_NATIVE_REPLACE_REQUIRES_TARGET);
            ok = false;
            break;
        }

        char path[256];
        snprintf(path, sizeof(path), "assets/audio/samples/SampleBank_0/%s.wav", region->name);
        ok = host->artifact(host->user, path, region->audio.data, region->audio.size);
        oot_text_add(
            &sample_entries,
            "    <Sample Name=\"%s\" Path=\"$(BUILD_DIR)/assets/audio/samples/SampleBank_0/%s.aifc\"/>\n",
            region->name, region->name);
    }

    if (ok && sample_entries.size) {
        ok = !sample_entries.failed &&
             host->artifact(host->user, "samplebank-additions.xml",
                            (const uint8_t *)sample_entries.data, sample_entries.size);
    }

    oot_text_free(&sample_entries);

    if (ok) {
        char *guide = oot_install_guide(j->name, j->song, font, j->request.font_index,
                                        result.sequence_bytes, result.soundfont_bytes);
        OotText full_guide = {0};

        if (guide) {
            oot_text_add(&full_guide, "%s", guide);
        }

        if (j->samples.count) {
            oot_text_add(&full_guide, "%s", DM_TEXT_NATIVE_OOT_SAMPLE_GUIDE);
        }

        ok = guide && !full_guide.failed &&
             host->artifact(host->user, "INSTALL.txt", (const uint8_t *)full_guide.data,
                            full_guide.size);
        oot_text_free(&full_guide);
        free(guide);
    }

    oot_result_free(&result);

    if (status == DME_OK && !ok) {
        status = host->cancelled(host->user) ? DME_CANCELLED : DME_FAILED;
    }

    host->progress(host->user, 100);
    return status;
}

static int32_t DME_CALL preflight(void *job, const DmeContext *c, const DmeHost *host, char *error,
                                  uint64_t cap) {
    ExportJob *j = job;
    int32_t status = prepare(j, c, host, error, cap);

    if (status != DME_OK) {
        return status;
    }

    OotResult result;
    status = convert_fitted(j, host, true, &result, error, cap);
    es_native_samples_report(&j->samples, ES_NATIVE_OOT, host);
    bool ok = status == DME_OK && oot_review_build(j, c, &result);
    oot_result_free(&result);

    if (host->cancelled(host->user)) {
        return DME_CANCELLED;
    }

    if (status != DME_OK) {
        return status;
    }

    if (!ok) {
        snprintf(error, (size_t)cap, "Cannot analyze the mapped sample ranges.");
        return DME_FAILED;
    }

    return DME_OK;
}

static void DME_CALL destroy_job(void *job) {
    ExportJob *j = job;
    oot_review_free(j);
    es_mapping_job_free(j->custom);
    es_native_samples_free(&j->samples);
    free(j->stock_values);
    oot_text_free(&j->spec_cleaned);
    free(j->events);
    free(j);
}

static int32_t DME_CALL set_custom(void *instance, const char *id, const DmeCustomSelection *value,
                                   char *error, uint64_t cap) {
    OotPluginUi *s = instance;
    return es_mappings_set(s->custom, 0, id, value, error, (size_t)cap);
}

static int32_t DME_CALL history_prepare(void *job, const DmeContext *c, const DmeHost *host,
                                        const DmeHistoryApi *api, DmeHistory *history,
                                        const DmeHistoryTarget *target, char *error, uint64_t cap) {
    (void)api;
    (void)target;
    ExportJob *j = job;
    j->repository = (EsRepository *)history;
    int32_t status = preflight(j, c, host, error, cap);
    j->repository = NULL;
    return status;
}

static int32_t DME_CALL history_install(void *job, const DmeContext *c, const DmeHost *host,
                                        const DmeHistoryApi *api, DmeHistory *history, char *error,
                                        uint64_t cap) {
    (void)api;
    ExportJob *j = job;
    EsRepository *repo = (EsRepository *)history;
    OotResult result = {0};
    j->repository = repo;
    int32_t status = convert(j, c, host, error, cap, &result);
    j->repository = NULL;

    if (status == DME_OK && !oot_repository_apply(j, repo, &result, error, (size_t)cap)) {
        status = host->cancelled(host->user) ? DME_CANCELLED : DME_FAILED;
    }

    oot_result_free(&result);
    return status;
}

static void DME_CALL history_begin(void *job, const DmeRepositoryRequest *request) {
    ((ExportJob *)job)->keep_originals = request->keep_originals != 0;
}

static int32_t DME_CALL history_finish(void *instance, const DmeHistoryResult *result, char *error,
                                       uint64_t cap) {
    OotPluginUi *s = instance;
    snprintf(s->checkout, sizeof(s->checkout), "%.*s", (int)result->checkout.length,
             result->checkout.data);
    snprintf(s->checkout_identity, sizeof(s->checkout_identity), "%.*s",
             (int)result->identity.length, result->identity.data);
    return es_mappings_targets(s->custom, 0, result->targets, result->target_count, error,
                               (size_t)cap)
               ? DME_OK
               : DME_FAILED;
}

const DmeHistoryAdapter oot_history_adapter = {.size = sizeof(DmeHistoryAdapter),
                                               .version = DME_REPOSITORY_API,
                                               .detect = oot_history_detect,
                                               .inventory = oot_history_inventory,
                                               .allow = oot_history_allow,
                                               .render = oot_history_render,
                                               .prepare = history_prepare,
                                               .install = history_install,
                                               .targets = oot_history_targets,
                                               .finish = history_finish,
                                               .begin_install = history_begin};

static const DmeHistoryAdapter *DME_CALL history_interface(uint32_t version) {
    return version == DME_REPOSITORY_API ? &oot_history_adapter : NULL;
}

static const DmeExporter exporter = {
    .id = "org.duckmidi.oot",
    .name = "OoT",
    .extension = "zip",
    .filter_name = "OoT sequence package",
    .save_label = "Save OoT Package",
    .description = "Ocarina of Time sequence and soundfont export for the zeldaret/oot decomp.",
    .container = DME_ZIP,
    .create = oot_ui_create,
    .destroy = oot_ui_destroy,
    .inspect = oot_ui_inspect,
    .describe = oot_ui_describe,
    .set = oot_ui_set,
    .reset = oot_ui_reset,
    .create_job = create_job,
    .run = run,
    .destroy_job = destroy_job,
    .preflight = preflight,
    .review = oot_review_describe,
    .review_set = oot_review_set,
    .review_apply = oot_review_apply,
    .install_label = "Export to Repository",
    .set_custom = set_custom,
    .history = history_interface};

static const DmeExporter *DME_CALL get_exporter(uint32_t i) {
    return i ? NULL : &exporter;
}

static const DmePlugin plugin = {.abi = DME_ABI,
                                 .size = sizeof(DmePlugin),
                                 .id = "org.duckmidi.ootexport",
                                 .name = "OoT exporter",
                                 .exporter_count = 1,
                                 .exporter = get_exporter};

DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi) {
    return abi == DME_ABI ? &plugin : NULL;
}
