/* Generic duckmidi plugin adapter. Conversion stays in ../src; UI in ui.c; memory patches in
   memory.c. */
#define DME_BUILD_PLUGIN
#include "m64_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef M64PluginJob ExportJob;

static void *DME_CALL create_job(const void *instance, char *error, uint64_t cap) {
    const M64PluginUi *s = instance;
    const M64Api *api = m64_get_api_3();
    char bank_lower[sizeof(s->bank)];

    for (size_t i = 0; i < sizeof(bank_lower); i++) {
        unsigned char c = (unsigned char)s->bank[i];
        bank_lower[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }

    if (!strcmp(bank_lower, "manifest") || !strcmp(bank_lower, "sequences-entry") ||
        !strcmp(bank_lower, "sequence-memory")) {
        snprintf(error, (size_t)cap,
                 "Choose a bank name other than manifest, sequences-entry or sequence-memory.");
        return NULL;
    }

    ExportJob *j = calloc(1, sizeof(*j));

    if (!j) {
        snprintf(error, (size_t)cap, "Cannot allocate SM64 export settings.");
        return NULL;
    }

    snprintf(j->sequence, 97, "%s", s->sequence);
    snprintf(j->bank, 97, "%s", s->bank);

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
    j->request.target = M64_TARGET_SM64_US;
    j->request.loop = (uint32_t)s->loop;
    j->request.voice_limit = 16;
    j->request.sequence_name = j->sequence;
    j->request.bank_name = j->bank;
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
static M64Event *prepare(ExportJob *j, const DmeContext *c, char *error, uint64_t cap,
                         int32_t *status) {
    *status = DME_INVALID;

    if (!c || c->event_count > 2000000 || (!c->events && c->event_count)) {
        snprintf(error, (size_t)cap, "Invalid SM64 source timeline.");
        return NULL;
    }

    for (uint64_t i = 0; i < c->event_count; i++) {
        if (c->events[i].port) {
            snprintf(error, (size_t)cap,
                     "M64 cannot preserve independent note ports. Remove note colors or export "
                     "MIDI instead.");
            return NULL;
        }
    }

    M64Event *events = calloc((size_t)c->event_count + 1, sizeof(*events));

    if (!events) {
        snprintf(error, (size_t)cap, "Cannot allocate SM64 timeline.");
        *status = DME_NOMEM;
        return NULL;
    }

    for (uint64_t i = 0; i < c->event_count; i++) {
        const DmeEvent *e = &c->events[i];
        events[i] = (M64Event){e->tick, e->source_id, e->status, e->a, e->b};
    }

    j->request.events = events;
    j->request.event_count = c->event_count;
    j->request.duration = c->duration;
    j->request.ppq = c->ppq;

    /* Loop points override whole-song looping. A missing end repeats to the song end. */
    if (j->loop_points && c->loop_points) {
        uint64_t start = c->loop_points & DME_LOOP_START ? c->loop_start : 0;
        uint64_t end = c->loop_points & DME_LOOP_END ? c->loop_end : 0;

        if (end >= c->duration) {
            end = 0;
        }

        if (start >= (end ? end : c->duration)) {
            snprintf(error, (size_t)cap,
                     "M64 - Loop Start must come before M64 - Loop End and the end of the song.");
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

static void report(const DmeHost *host, const M64Result *result, char *error, uint64_t cap) {
    const M64Api *api = m64_get_api_3();

    for (uint64_t i = 0; i < api->diagnostic_count(result); i++) {
        M64DiagnosticView d;

        if (api->diagnostic_at(result, i, &d) != M64_OK) {
            continue;
        }

        uint32_t flags =
            d.code == 22 || d.code == 23 || d.code == 54 || d.code == 57 ? DME_DISPLAY : 0;

        if (d.severity == M64_WARNING) {
            flags |= DME_POPUP;
        }

        host->diagnostic(host->user, d.severity, flags, d.message);

        if (d.severity == M64_ERROR && !*error) {
            snprintf(error, (size_t)cap, "%s", d.message);
        }
    }
}

/* The manifest records the temporary sequence memory the song needs. */
static uint64_t required_memory(const M64Result *result) {
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

static int32_t compile(ExportJob *j, const DmeContext *c, const DmeHost *host, char *error,
                       uint64_t cap, M64Result **result) {
    int32_t status;
    M64Event *events = prepare(j, c, error, cap, &status);

    if (!events) {
        return status;
    }

    M64Hooks hooks = {(void *)host, cancel, progress};
    int compiled = m64_get_api_3()->compile(&j->request, &hooks, result);
    report(host, *result, error, cap);
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

static int32_t DME_CALL run(void *job, const DmeContext *c, const DmeHost *host, char *error,
                            uint64_t cap) {
    ExportJob *j = job;
    const M64Api *api = m64_get_api_3();
    M64Result *result = NULL;
    int32_t status = compile(j, c, host, error, cap, &result);
    bool ok = status == DME_OK;
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

    /* The package also explains any sequence memory changes the song needs. */
    if (ok) {
        char *guide = m64_memory_guide(required_memory(result), j->sequence);
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

static bool install_text(const DmeHost *host, const char *path, M64Text *text, uint32_t flags) {
    bool ok = host->install_file(host->user, path, (const uint8_t *)text->data, text->size, flags);
    m64_text_free(text);
    return ok;
}

/* Applies the console-compatible memory change. Files still matching the stock checkout are kept
   as backups even when the user turned backups off. */
static bool install_memory(const DmeHost *host, const M64MemoryPlan *plan, char *error,
                           uint64_t cap) {
    const char *paths[] = {M64_DATA_C, M64_DATA_H, M64_BUFFERS_C};

    for (int i = 0; i < 3; ++i) {
        const uint8_t *data;
        uint64_t size;

        if (!host->read_file(host->user, paths[i], &data, &size)) {
            snprintf(error, (size_t)cap, "Cannot read %s in the chosen folder.", paths[i]);
            return false;
        }

        M64Text text = {0};
        bool ok = i == 0   ? m64_patch_data_c(data, size, plan, &text, error, (size_t)cap)
                  : i == 1 ? m64_patch_data_h(data, size, plan, &text, error, (size_t)cap)
                           : m64_patch_buffers_c(data, size, plan, &text, error, (size_t)cap);

        if (!ok) {
            m64_text_free(&text);
            return false;
        }

        uint32_t flags = m64_stock_memory_file(paths[i], data, size) ? DME_INSTALL_BACKUP : 0;

        if (!install_text(host, paths[i], &text, flags)) {
            snprintf(error, (size_t)cap, "Cannot queue %s for writing.", paths[i]);
            return false;
        }
    }

    return true;
}

/* Installs into an SM64 decomp checkout: the sequence replaces the chosen song, the bank is
   added, the song's entry in sound/sequences.json points at the bank, and the audio memory grows
   when the song needs it. */
static int32_t DME_CALL install(void *job, const DmeContext *c, const DmeHost *host, char *error,
                                uint64_t cap) {
    ExportJob *j = job;
    const M64Api *api = m64_get_api_3();
    const uint8_t *sequences;
    uint64_t sequences_size;

    if (!host->read_file(host->user, M64_SEQUENCES_JSON, &sequences, &sequences_size)) {
        snprintf(error, (size_t)cap,
                 "Choose the root folder of an SM64 decomp checkout; it contains "
                 "sound/sequences.json.");
        return DME_INVALID;
    }

    char path[256];
    const uint8_t *unused;
    uint64_t unused_size;
    snprintf(path, sizeof(path), "sound/sequences/%s.s", j->sequence);

    if (host->read_file(host->user, path, &unused, &unused_size)) {
        snprintf(error, (size_t)cap, "%s is an assembly sequence; choose another song to replace.",
                 j->sequence);
        return DME_INVALID;
    }

    M64Text entry = {0};

    if (!m64_patch_sequences(sequences, sequences_size, j->sequence, j->bank, &entry, error,
                             (size_t)cap)) {
        m64_text_free(&entry);
        return DME_INVALID;
    }

    M64Result *result = NULL;
    int32_t status = compile(j, c, host, error, cap, &result);
    M64ArtifactView sequence, bank;
    bool ok = status == DME_OK && api->artifact_at(result, M64_SEQUENCE, &sequence) == M64_OK &&
              api->artifact_at(result, M64_BANK_JSON, &bank) == M64_OK;

    if (ok) {
        M64MemoryPlan plan = m64_memory_plan(required_memory(result), false);

        if (plan.needed && !plan.fits) {
            snprintf(error, (size_t)cap,
                     "This song needs more sequence memory than the stock SM64 layout can hold. "
                     "See sequence-memory.txt in a saved package.");
            ok = false;
            status = DME_FAILED;
        }

        snprintf(path, sizeof(path), "sound/sequences/us/%s.m64", j->sequence);
        ok = ok && host->install_file(host->user, path, sequence.data, sequence.size, 0);
        snprintf(path, sizeof(path), "sound/sound_banks/%s.json", j->bank);
        ok = ok && host->install_file(host->user, path, bank.data, bank.size, 0);
        ok = ok && install_text(host, M64_SEQUENCES_JSON, &entry, 0);
        ok = ok && (!plan.needed || install_memory(host, &plan, error, cap));

        if (!ok && status == DME_OK) {
            status = DME_FAILED;

            if (!*error) {
                snprintf(error, (size_t)cap, "Cannot prepare the repository files.");
            }
        }
    }

    m64_text_free(&entry);
    api->result_free(result);
    return status;
}

static int32_t DME_CALL preflight(void *job, const DmeContext *c, const DmeHost *host, char *error,
                                  uint64_t cap) {
    ExportJob *j = job;
    const M64Api *api = m64_get_api_3();
    int32_t status;
    M64Event *events = prepare(j, c, error, cap, &status);

    if (!events) {
        return status;
    }

    M64Hooks hooks = {(void *)host, cancel, progress};
    M64Result *result = NULL;
    int analyzed = api->analyze(&j->request, &hooks, &result);
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
    m64_review_free(job);
    free(job);
}

static const DmeExporter exporter = {"org.duckmidi.sm64",
                                     "M64",
                                     "zip",
                                     "M64 sequence package",
                                     "Save M64 Package",
                                     "Native SM64 US sequence and sound-bank export.",
                                     DME_ZIP,
                                     m64_ui_create,
                                     m64_ui_destroy,
                                     m64_ui_inspect,
                                     m64_ui_describe,
                                     m64_ui_set,
                                     m64_ui_reset,
                                     create_job,
                                     run,
                                     destroy_job,
                                     preflight,
                                     m64_review_describe,
                                     m64_review_set,
                                     m64_review_apply,
                                     "Export to Repository",
                                     install};

static const DmeExporter *DME_CALL get_exporter(uint32_t i) {
    return i ? NULL : &exporter;
}

static const DmePlugin plugin = {
    DME_ABI, sizeof(DmePlugin), "org.duckmidi.m64export", "SM64 exporter", 1, get_exporter};

DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi) {
    return abi == DME_ABI ? &plugin : NULL;
}
