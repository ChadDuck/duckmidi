/* Generic duckmidi plugin adapter. Conversion stays in ../src; UI in ui.c; memory patches in
   memory.c. */
#define DME_BUILD_PLUGIN
#include "m64_plugin.h"
#include "../src/custom.h"
#include "repository.h"
#include "../../../src/text/en.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef M64PluginJob ExportJob;

static const char *const game_names[M64_GAME_COUNT] = {"SM64", "HackerSM64", "sm64coopdx"};

/* The game for a detected repository target ID. */
static unsigned detected_game(const char *id) {
    return !strcmp(id, "sm64coopdx")   ? M64_GAME_COOPDX
           : !strcmp(id, "hackersm64") ? M64_GAME_HACKERSM64
                                       : M64_GAME_SM64;
}

static uint32_t library_target(int game) {
    return game == M64_GAME_COOPDX       ? M64_TARGET_SM64COOPDX
           : game == M64_GAME_HACKERSM64 ? M64_TARGET_HACKERSM64
                                         : M64_TARGET_SM64_US;
}

static void sample_definitions(ExportJob *j) {
    for (size_t i = 0; i < j->samples.count; i++) {
        const EsNativeRegion *region = &j->samples.regions[i];
        j->custom_definitions[i] = (M64Instrument){
            .sample = region->name,
            .tuning = region->reference_tuning ? region->reference_tuning : region->tuning,
            .release_rate = region->release,
            .attack = 1,
            .decay = 1,
            .sustain = 32767};
    }
}

static void *DME_CALL create_job(const void *instance, char *error, uint64_t cap) {
    const M64PluginUi *s = instance;
    const M64Api *api = m64_get_api_3();
    char bank_lower[sizeof(s->bank)];

    for (size_t i = 0; i < sizeof(bank_lower); i++) {
        unsigned char c = (unsigned char)s->bank[i];
        bank_lower[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }

    if (s->target != M64_GAME_COOPDX &&
        (!strcmp(bank_lower, "manifest") || !strcmp(bank_lower, "sequences-entry") ||
         !strcmp(bank_lower, "sequence-memory"))) {
        snprintf(error, (size_t)cap,
                 "Choose a bank name other than manifest, sequences-entry or sequence-memory.");
        return NULL;
    }

    ExportJob *j = calloc(1, sizeof(*j));

    if (!j) {
        snprintf(error, (size_t)cap, "Cannot allocate SM64 export settings.");
        return NULL;
    }

    snprintf(j->checkout, sizeof(j->checkout), "%s", s->checkout);
    snprintf(j->checkout_identity, sizeof(j->checkout_identity), "%s", s->checkout_identity);

    j->custom = es_mappings_freeze(s->custom, error, (size_t)cap);

    if (!j->custom) {
        free(j);
        return NULL;
    }

    snprintf(j->sequence, 97, "%s", s->sequence);
    snprintf(j->bank, 97, "%s", s->bank);
    snprintf(j->requested_bank, sizeof(j->requested_bank), "%s", s->bank);

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned p = 0; p < 128; p++) {
            j->melodic[ch * 128 + p] = *api->default_program((uint32_t)s->melodic[ch][p]);
        }
    }

    for (unsigned p = 0; p < 128; p++) {
        j->drums[p] = *api->default_drum((uint32_t)s->drums[p]);
    }

    memcpy(j->presets, s->melodic, sizeof(j->presets));
    memcpy(j->drum_presets, s->drums, sizeof(j->drum_presets));
    j->loop_points = s->loop_points && s->has_loop_points;
    j->memory_stock = s->memory == M64_MEMORY_STOCK;
    j->target = s->target;
    j->request.target = library_target(s->target);
    j->request.loop = (uint32_t)s->loop;
    j->request.voice_limit = 16;
    j->request.sequence_name = j->sequence;
    /* sm64coopdx songs name no bank of their own. */
    j->request.bank_name = s->target == M64_GAME_COOPDX ? "extended" : j->bank;
    j->request.channel_programs = j->melodic;
    j->request.drums = j->drums;
    return j;
}

static int32_t M64_CALL cancel(void *user) {
    const DmeHost *h = user;
    return h->cancelled(h->user);
}

static void M64_CALL progress(void *user, uint32_t stage, uint32_t percent) {
    const DmeHost *h = user;
    h->progress(h->user, (stage * 100 + percent) / 4);
}

/* Copies the timeline into the request and applies the arrangement's loop points. The caller
   frees the returned events after compiling. */
static M64Event *prepare(ExportJob *j, const DmeContext *c, const DmeHost *host, char *error,
                         uint64_t cap, int32_t *status) {
    *status = DME_INVALID;

    if (!c || c->event_count > 2000000 || (!c->events && c->event_count)) {
        snprintf(error, (size_t)cap, "Invalid SM64 source timeline.");
        return NULL;
    }

    unsigned target = j->request.target == M64_TARGET_HACKERSM64   ? M64_GAME_HACKERSM64
                      : j->request.target == M64_TARGET_SM64COOPDX ? M64_GAME_COOPDX
                                                                   : M64_GAME_SM64;
    if (*j->target_notice && host->diagnostic) {
        host->diagnostic(host->user, DME_WARNING, DME_DISPLAY | DME_POPUP, j->target_notice);
    }

    /* sm64coopdx cannot add instrument samples; choices made for the other targets are left out
       and those rows play their ordinary instruments. */
    if (target == M64_GAME_COOPDX && !j->samples.fixes[ES_FIX_IGNORED].count &&
        (es_mapping_job_custom(j->custom, M64_GAME_SM64) ||
         es_mapping_job_custom(j->custom, M64_GAME_HACKERSM64))) {
        es_native_fix(&j->samples, ES_FIX_IGNORED, NULL);
    }

    if (!es_native_samples(&j->samples, j->custom, target, ES_NATIVE_SM64, c, host, error,
                           (size_t)cap)) {
        *status = host->cancelled(host->user) ? DME_CANCELLED : DME_FAILED;
        return NULL;
    }

    sample_definitions(j);

    bool replacing = false;

    for (size_t i = 0; i < j->samples.count; i++) {
        replacing |= j->samples.regions[i].target != NULL;
    }

    EsRepository *probe = NULL;

    if (replacing && !j->repository) {
        probe = *j->checkout
                    ? es_repository_probe(j->checkout, &m64_history_adapter, error, (size_t)cap)
                    : NULL;

        /* A configured checkout that cannot be opened, such as a locked history, is an error;
           only a missing or different checkout turns Replace into Add. */
        if (*j->checkout && !probe) {
            *status = DME_FAILED;
            return NULL;
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
    bool targets_ok =
        !target_repository ||
        (m64_repository_sequence(j, target_repository, error, (size_t)cap) &&
         m64_repository_bank(j, target_repository, error, (size_t)cap) &&
         m64_repository_sample_targets(j, target_repository, c, host, error, (size_t)cap));
    es_repository_close(probe);

    if (!targets_ok) {
        *status = DME_FAILED;
        return NULL;
    }

    M64Event *events = calloc((size_t)c->event_count + 1, sizeof(*events));
    free(j->source_programs);
    j->source_programs = malloc(((size_t)c->event_count + 1) * sizeof(*j->source_programs));
    free(j->stock_values);
    j->stock_values = malloc(((size_t)c->event_count + 1) * sizeof(*j->stock_values));
    EsPortLayout *layout = calloc(1, sizeof(*layout));

    if (!events || !j->source_programs || !j->stock_values || !layout ||
        !es_mapping_job_values(j->custom, target, c, j->stock_values, error, (size_t)cap) ||
        !es_port_layout(c->events, c->event_count, c->percussion_channels, layout)) {
        snprintf(error, (size_t)cap, "Cannot allocate SM64 timeline.");
        *status = DME_NOMEM;
        free(events);
        free(layout);
        return NULL;
    }

    /* Colored ports play on the channels the layout gives them. */
    for (uint64_t i = 0; i < c->event_count; i++) {
        const DmeEvent *e = &c->events[i];
        uint32_t status = e->status;

        if (status >= MIDI_STATUS && status < MIDI_SYSTEM && e->port < ES_PORTS) {
            status = (status & MIDI_STATUS_MASK) | layout->out[status & MIDI_CHANNEL_MASK][e->port];
        }

        events[i] = (M64Event){e->tick, e->source_id, status, e->a, e->b};
        j->source_programs[i] = UINT16_MAX;

        if (c->effective_patches && e->status < MIDI_SYSTEM &&
            (e->status & MIDI_STATUS_MASK) == MIDI_NOTE_ON && e->b &&
            !(c->effective_patches[i].flags & DME_SAMPLE_PERCUSSION)) {
            j->source_programs[i] =
                (uint16_t)(((status & MIDI_CHANNEL_MASK) << 8) | c->effective_patches[i].program);
        }
    }

    snprintf(j->port_notice, sizeof(j->port_notice), "%s",
             layout->interference ? layout->merges : "");
    free(layout);

    j->request.events = events;
    j->request.event_count = c->event_count;
    j->request.duration = c->duration;
    j->request.ppq = c->ppq;

    /* Loop points override whole-song looping. A missing end repeats to the song end. */
    if (j->loop_points && c->loop_points) {
        uint64_t start, end;

        if (!es_loop_region(c, &start, &end, error, (size_t)cap)) {
            free(events);
            j->request.events = NULL;
            return NULL;
        }

        j->request.loop = 2;
        j->request.loop_start = start;
        j->request.loop_end = end;
    }

    *status = DME_OK;
    return events;
}

/* Hides the stock pool warning (57) when Sequence memory is Enlarge, and for HackerSM64, whose
   pool the plugin checks itself. */
static const char hacker_stock_warning[] =
    "The song needs more temporary sequence memory than HackerSM64's TEMPORARY_SEQ_MEM provides "
    "and Sequence memory is Stock, so it may not play. Choose Enlarge to raise it.";

/* The warning-list item ("Heading\nItem") for a converter warning; false keeps its full text.
   Code 128 + n omits controller n. */
static bool list_item(const M64DiagnosticView *d, unsigned voices, char *out, size_t cap) {
    static const struct {
        unsigned code;
        const char *heading, *item;
    } items[] = {
        {4, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_SLOW_TEMPO},
        {30, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_TIMING},
        {31, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_ROUND_TEMPO},
        {32, DM_TEXT_EXPORT_LIST_OMITTED, DM_TEXT_EXPORT_ITEM_SYSEX},
        {33, DM_TEXT_EXPORT_LIST_OMITTED, DM_TEXT_EXPORT_ITEM_NOTE_OFF},
        {35, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_BANKS},
        {36, DM_TEXT_EXPORT_LIST_OMITTED, DM_TEXT_EXPORT_ITEM_PRESSURE},
        {37, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_HELD},
        {38, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_SHORT},
        {41, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_MODULATION},
        {50, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_BEND},
        {51, DM_TEXT_EXPORT_LIST_OMITTED, DM_TEXT_EXPORT_ITEM_RPN},
        {54, DM_TEXT_EXPORT_LIST_FIT, DM_TEXT_EXPORT_ITEM_REENCODED},
        {56, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_FAST_TEMPO},
        {63, DM_TEXT_EXPORT_LIST_CONVERTED, DM_TEXT_EXPORT_ITEM_SHARED_FX},
        {64, DM_TEXT_EXPORT_LIST_FIT, DM_TEXT_EXPORT_ITEM_CURVES},
    };

    if (d->code >= 128 && d->code < 256) {
        char item[64];
        snprintf(item, sizeof(item), DM_TEXT_EXPORT_ITEM_CONTROLLER, d->code - 128);
        snprintf(out, cap, "%s\n%s", DM_TEXT_EXPORT_LIST_OMITTED, item);
        return true;
    }

    if (d->code == 22) {
        char item[128];
        snprintf(item, sizeof(item), DM_TEXT_EXPORT_ITEM_VOICES, voices);
        snprintf(out, cap, "%s\n%s", DM_TEXT_EXPORT_LIST_RISK, item);
        return true;
    }

    for (size_t i = 0; i < sizeof(items) / sizeof(*items); i++) {
        if (items[i].code == d->code) {
            snprintf(out, cap, "%s\n%s", items[i].heading, items[i].item);
            return true;
        }
    }

    /* Memory, range and sm64coopdx warnings keep their full advice under one heading. */
    if (d->code == 57 || d->code == 59 || d->code == 71 || d->code == 72) {
        snprintf(out, cap, "%s\n%s", DM_TEXT_EXPORT_LIST_RISK, d->message);
        return true;
    }

    return false;
}

static void report(const ExportJob *j, const DmeHost *host, const M64Result *result, char *error,
                   uint64_t cap) {
    const M64Api *api = m64_get_api_3();
    bool pool_warning = j->memory_stock && j->request.target != M64_TARGET_HACKERSM64;

    for (uint64_t i = 0; i < api->diagnostic_count(result); i++) {
        M64DiagnosticView d;

        if (api->diagnostic_at(result, i, &d) != M64_OK || (d.code == 57 && !pool_warning)) {
            continue;
        }

        uint32_t flags =
            d.code == 22 || d.code == 23 || d.code == 54 || d.code == 57 ? DME_DISPLAY : 0;

        if (d.severity == M64_WARNING) {
            flags |= DME_POPUP;
        }

        char item[1024];

        if (d.severity == M64_WARNING &&
            list_item(&d, j->request.voice_limit, item, sizeof(item))) {
            flags |= DME_LIST | (d.code == 54 ? DME_ALTERED : 0);
            host->diagnostic(host->user, d.severity, flags, item);
        } else {
            host->diagnostic(host->user, d.severity, flags, d.message);
        }

        if (d.severity == M64_ERROR && !*error) {
            snprintf(error, (size_t)cap, "%s", d.message);
        }
    }
}

uint64_t m64_required_memory(const M64Result *result) {
    M64ArtifactView manifest;
    const char *key = "\"minimum_temporary_sequence_pool_bytes\": ";

    if (m64_get_api_3()->artifact_at(result, M64_MANIFEST, &manifest) != M64_OK) {
        return 0;
    }

    char text[65536];
    size_t size = manifest.size < sizeof(text) - 1 ? (size_t)manifest.size : sizeof(text) - 1;
    memcpy(text, manifest.data, size);
    text[size] = 0;
    const char *found = strstr(text, key);
    return found ? strtoull(found + strlen(key), NULL, 10) : 0;
}

/* Converts until every custom note fits the engine: es_native_fit resamples regions played
   above the 4x rate limit and falls back selections that cannot fit. A trial result is never
   published; the final conversion runs without the trial arrays. */
static int compile_fitted(ExportJob *j, const M64Hooks *hooks, M64Custom *custom, bool analyze,
                          const DmeHost *host, char *error, uint64_t cap, M64Result **result) {
    double overspeed[ES_NATIVE_REGIONS];
    bool capacity[ES_NATIVE_REGIONS];

    for (size_t attempt = 0; j->samples.count && attempt <= ES_NATIVE_REGIONS; attempt++) {
        memset(overspeed, 0, sizeof(overspeed));
        memset(capacity, 0, sizeof(capacity));
        custom->overspeed = overspeed;
        custom->capacity = capacity;
        int compiled = m64_compile_custom(&j->request, hooks, custom, analyze, result);
        custom->overspeed = NULL;
        custom->capacity = NULL;
        bool other = *j->sample_bank && strcmp(j->sample_bank, "instruments");
        int fit = es_native_fit(&j->samples, compiled == M64_OK ? overspeed : NULL, capacity,
                                ES_NATIVE_SM64, &other, host, error, (size_t)cap);

        if (fit == ES_FIT_DONE) {
            return compiled;
        }

        m64_get_api_3()->result_free(*result);
        *result = NULL;

        if (fit == ES_FIT_FAILED) {
            return host->cancelled(host->user) ? M64_CANCELLED : M64_NOMEM;
        }

        if (!other) {
            j->sample_bank[0] = 0;
            custom->sample_bank = "instruments";
        }

        sample_definitions(j);
    }

    return m64_compile_custom(&j->request, hooks, custom, analyze, result);
}

static int32_t compile(ExportJob *j, const DmeContext *c, const DmeHost *host, char *error,
                       uint64_t cap, M64Result **result) {
    int32_t status;
    M64Event *events = prepare(j, c, host, error, cap, &status);

    if (!events) {
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

    M64Hooks hooks = {(void *)host, cancel, progress};
    M64Custom custom = {&j->samples,
                        j->custom_definitions,
                        j->source_programs,
                        j->stock_values,
                        c,
                        *j->sample_bank ? j->sample_bank : "instruments"};
    int compiled = compile_fitted(j, &hooks, &custom, false, host, error, cap, result);
    es_native_samples_report(&j->samples, ES_NATIVE_SM64, host);
    report(j, host, *result, error, cap);
    free(events);
    j->request.events = NULL;

    if (host->cancelled(host->user) || compiled == M64_CANCELLED) {
        return DME_CANCELLED;
    }

    if (compiled != M64_OK) {
        if (!*error) {
            snprintf(error, (size_t)cap, "SM64 conversion failed (status %d).", compiled);
        }

        return compiled == M64_NOMEM ? DME_NOMEM : DME_FAILED;
    }

    return DME_OK;
}

/* An sm64coopdx package is a mod folder, ready for the game's mods folder. */
static bool coopdx_package(ExportJob *j, const DmeHost *host, const M64Result *result) {
    const M64Api *api = m64_get_api_3();
    char folder[128], path[256];
    M64ArtifactView sequence, manifest;
    m64_coopdx_folder(j->sequence, folder, sizeof(folder));
    char *lua = m64_coopdx_main_lua(j->sequence);
    snprintf(path, sizeof(path), "%s/main.lua", folder);
    bool ok = lua && host->artifact(host->user, path, (const uint8_t *)lua, strlen(lua));
    snprintf(path, sizeof(path), "%s/sound/%s.m64", folder, j->sequence);
    ok = ok && api->artifact_at(result, M64_SEQUENCE, &sequence) == M64_OK &&
         host->artifact(host->user, path, sequence.data, sequence.size);
    ok = ok && api->artifact_at(result, M64_MANIFEST, &manifest) == M64_OK &&
         host->artifact(host->user, "manifest.json", manifest.data, manifest.size);
    free(lua);
    return ok;
}

static int32_t DME_CALL run(void *job, const DmeContext *c, const DmeHost *host, char *error,
                            uint64_t cap) {
    ExportJob *j = job;
    const M64Api *api = m64_get_api_3();
    M64Result *result = NULL;
    int32_t status = compile(j, c, host, error, cap, &result);
    bool ok = status == DME_OK;

    bool replacing = false;

    for (size_t i = 0; i < j->samples.count; i++) {
        replacing |= j->samples.regions[i].target != NULL;
    }

    if (ok && replacing) {
        EsRepository *probe =
            es_repository_probe(j->checkout, &m64_history_adapter, error, (size_t)cap);
        ok = probe && !strcmp(j->checkout_identity, es_repository_identity(probe)) &&
             m64_repository_package(j, probe, result, host, error, (size_t)cap);
        es_repository_close(probe);
        api->result_free(result);
        return ok ? DME_OK : DME_FAILED;
    }

    if (j->target == M64_GAME_COOPDX) {
        ok = ok && coopdx_package(j, host, result);
        api->result_free(result);

        if (status == DME_OK && !ok) {
            status = host->cancelled(host->user) ? DME_CANCELLED : DME_FAILED;
        }

        return status;
    }

    char names[M64_ARTIFACT_COUNT][128];
    snprintf(names[0], 128, "%s.m64", j->sequence);
    snprintf(names[1], 128, "%s.json", j->bank);
    snprintf(names[2], 128, "sequences-entry.json");
    snprintf(names[3], 128, "manifest.json");

    for (unsigned i = 0; i < M64_ARTIFACT_COUNT && ok; i++) {
        M64ArtifactView a;
        ok = !host->cancelled(host->user) && api->artifact_at(result, i, &a) == M64_OK &&
             host->artifact(host->user, names[i], a.data, a.size);
    }

    for (size_t i = 0; ok && i < j->samples.count; i++) {
        const EsNativeRegion *region = &j->samples.regions[i];
        char path[256];

        if (region->target) {
            snprintf(error, (size_t)cap, "%s", DM_TEXT_NATIVE_REPLACE_REQUIRES_TARGET);
            ok = false;
            break;
        }

        snprintf(path, sizeof(path), "sound/samples/instruments/%s.aiff", region->name);
        ok = host->artifact(host->user, path, region->audio.data, region->audio.size);
    }

    if (ok && j->samples.count) {
        char guide[4096];
        int size = snprintf(guide, sizeof(guide), DM_TEXT_NATIVE_M64_SAMPLE_GUIDE, j->sequence,
                            j->bank, j->sequence, j->bank);
        ok = size > 0 && size < (int)sizeof(guide) &&
             host->artifact(host->user, "INSTALL.txt", (const uint8_t *)guide, (uint64_t)size);
    }

    /* When the song needs source edits for its sequence memory, the package explains them for
       the chosen memory option. */
    uint64_t required = ok ? m64_required_memory(result) : 0;
    M64MemoryPlan plan = m64_memory_plan(required);
    bool hacker = j->target == M64_GAME_HACKERSM64;

    if (ok && j->memory_stock && hacker && required > M64_HACKER_POOL) {
        char item[512];
        snprintf(item, sizeof(item), "%s\n%s", DM_TEXT_EXPORT_LIST_RISK, hacker_stock_warning);
        host->diagnostic(host->user, DME_WARNING, DME_DISPLAY | DME_POPUP | DME_LIST, item);
    }

    if (ok && !j->memory_stock &&
        (hacker ? required > M64_HACKER_POOL : plan.needed || !plan.fits)) {
        char *guide = hacker ? m64_hacker_memory_guide(required, j->sequence)
                             : m64_memory_guide(required, j->sequence);
        ok = guide && host->artifact(host->user, "sequence-memory.txt", (const uint8_t *)guide,
                                     strlen(guide));
        free(guide);
    }

    api->result_free(result);

    if (status == DME_OK && !ok) {
        status = host->cancelled(host->user) ? DME_CANCELLED : DME_FAILED;
    }

    return status;
}

static int32_t DME_CALL preflight(void *job, const DmeContext *c, const DmeHost *host, char *error,
                                  uint64_t cap) {
    ExportJob *j = job;
    const M64Api *api = m64_get_api_3();
    int32_t status;
    M64Event *events = prepare(j, c, host, error, cap, &status);

    if (!events) {
        return status;
    }

    M64Hooks hooks = {(void *)host, cancel, progress};
    M64Result *result = NULL;
    M64Custom custom = {&j->samples,
                        j->custom_definitions,
                        j->source_programs,
                        j->stock_values,
                        c,
                        *j->sample_bank ? j->sample_bank : "instruments"};
    int analyzed = compile_fitted(j, &hooks, &custom, true, host, error, cap, &result);
    es_native_samples_report(&j->samples, ES_NATIVE_SM64, host);
    bool ok = analyzed == M64_OK && m64_review_build(j, c, result);

    if (analyzed != M64_OK) {
        for (uint64_t i = 0; i < api->diagnostic_count(result); i++) {
            M64DiagnosticView d;
            api->diagnostic_at(result, i, &d);

            if (d.severity == M64_ERROR) {
                snprintf(error, (size_t)cap, "%s", d.message);
                break;
            }
        }
    }

    api->result_free(result);
    free(events);
    j->request.events = NULL;

    if (analyzed == M64_CANCELLED || host->cancelled(host->user)) {
        return DME_CANCELLED;
    }

    if (!ok && !*error) {
        snprintf(error, (size_t)cap, "Cannot analyze the selected sample ranges.");
    }

    return ok ? DME_OK : DME_FAILED;
}

static void DME_CALL destroy_job(void *job) {
    ExportJob *j = job;
    m64_review_free(j);
    es_mapping_job_free(j->custom);
    es_native_samples_free(&j->samples);
    free(j->stock_values);
    free(j->source_programs);
    free(j);
}

static int32_t DME_CALL set_custom(void *instance, const char *id, const DmeCustomSelection *value,
                                   char *error, uint64_t cap) {
    M64PluginUi *s = instance;
    return es_mappings_set(s->custom, (uint32_t)s->target, id, value, error, (size_t)cap);
}

/* The detected checkout decides the game; Target governs saved packages only. Custom samples
   chosen for another game do not carry over: those rows play their ordinary instruments. */
static int32_t DME_CALL history_prepare(void *job, const DmeContext *c, const DmeHost *host,
                                        const DmeHistoryApi *api, DmeHistory *history,
                                        const DmeHistoryTarget *target, char *error, uint64_t cap) {
    (void)api;
    ExportJob *j = job;
    EsRepository *repo = (EsRepository *)history;
    unsigned game = detected_game(target->id);

    if ((int)game != j->target) {
        snprintf(j->target_notice, sizeof(j->target_notice), DM_TEXT_NATIVE_REPOSITORY_DETECTED,
                 game_names[game], game_names[game]);

        /* Drop samples prepared for Target so preparation reads the detected game's rows. */
        if (es_mapping_job_custom(j->custom, (uint32_t)j->target) &&
            !j->samples.fixes[ES_FIX_IGNORED].count) {
            es_native_samples_free(&j->samples);
            es_native_fix(&j->samples, ES_FIX_IGNORED, NULL);
        }
    }

    j->request.target = library_target((int)game);
    j->request.bank_name = game == M64_GAME_COOPDX ? "extended" : j->bank;

    if (!m64_repository_sequence(j, repo, error, (size_t)cap)) {
        return DME_INVALID;
    }

    j->repository = repo;
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
    M64Result *result = NULL;
    j->repository = repo;
    int32_t status = compile(j, c, host, error, cap, &result);
    j->repository = NULL;

    if (status == DME_OK && !m64_repository_apply(j, repo, result, error, (size_t)cap)) {
        status = host->cancelled(host->user) ? DME_CANCELLED : DME_FAILED;
    }

    m64_get_api_3()->result_free(result);
    return status;
}

static void DME_CALL history_begin(void *job, const DmeRepositoryRequest *request) {
    ((ExportJob *)job)->keep_originals = request->keep_originals != 0;
}

static int32_t DME_CALL history_finish(void *instance, const DmeHistoryResult *result, char *error,
                                       uint64_t cap) {
    M64PluginUi *s = instance;
    snprintf(s->checkout, sizeof(s->checkout), "%.*s", (int)result->checkout.length,
             result->checkout.data);
    snprintf(s->checkout_identity, sizeof(s->checkout_identity), "%.*s",
             (int)result->identity.length, result->identity.data);
    uint32_t game = detected_game(result->target->id);
    return es_mappings_targets(s->custom, game, result->targets, result->target_count, error,
                               (size_t)cap)
               ? DME_OK
               : DME_FAILED;
}

const DmeHistoryAdapter m64_history_adapter = {.size = sizeof(DmeHistoryAdapter),
                                               .version = DME_REPOSITORY_API,
                                               .detect = m64_history_detect,
                                               .inventory = m64_history_inventory,
                                               .allow = m64_history_allow,
                                               .render = m64_history_render,
                                               .prepare = history_prepare,
                                               .install = history_install,
                                               .targets = m64_history_targets,
                                               .finish = history_finish,
                                               .begin_install = history_begin};

static const DmeHistoryAdapter *DME_CALL history_interface(uint32_t version) {
    return version == DME_REPOSITORY_API ? &m64_history_adapter : NULL;
}

static const DmeExporter exporter = {.id = "org.duckmidi.sm64",
                                     .name = "M64",
                                     .extension = "zip",
                                     .filter_name = "M64 sequence package",
                                     .save_label = "Save M64 Package",
                                     .description =
                                         "SM64, HackerSM64 and sm64coopdx sequence export.",
                                     .container = DME_ZIP,
                                     .create = m64_ui_create,
                                     .destroy = m64_ui_destroy,
                                     .inspect = m64_ui_inspect,
                                     .describe = m64_ui_describe,
                                     .set = m64_ui_set,
                                     .reset = m64_ui_reset,
                                     .create_job = create_job,
                                     .run = run,
                                     .destroy_job = destroy_job,
                                     .preflight = preflight,
                                     .review = m64_review_describe,
                                     .review_set = m64_review_set,
                                     .review_apply = m64_review_apply,
                                     .install_label = "Export to Repository",
                                     .set_custom = set_custom,
                                     .history = history_interface};

static const DmeExporter *DME_CALL get_exporter(uint32_t i) {
    return i ? NULL : &exporter;
}

static const DmePlugin plugin = {.abi = DME_ABI,
                                 .size = sizeof(DmePlugin),
                                 .id = "org.duckmidi.m64export",
                                 .name = "SM64 exporter",
                                 .exporter_count = 1,
                                 .exporter = get_exporter};

DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi) {
    return abi == DME_ABI ? &plugin : NULL;
}
