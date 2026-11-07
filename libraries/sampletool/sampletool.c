#include "../../include/duckmidi_samples_tool.h"
#include "../../shared/sample_batch.h"
#include "../../shared/sample_manifest.h"
#include "../../shared/sample_library.h"
#include "../../src/duck.h"
#include "../../src/text/en.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

enum { DRAFT_MAX_VALUES = 32 * 1024 * 1024 };

typedef struct {
    unsigned references;
} SharedSource;

typedef struct {
    SharedSource *shared;
    uint64_t token;
    DmSampleBatchItem source;
    DmeSampleEdit edit;
    char name[DM_SAMPLE_NAME_CAP];
    char asset_id[DM_SAMPLE_ID_CAP], preset_id[DM_SAMPLE_ID_CAP];
    char source_name[DM_SAMPLE_NAME_CAP], asset_name[DM_SAMPLE_NAME_CAP];
    bool preset_drums;
    uint32_t preset_zone;
} Draft;

typedef struct {
    size_t values;
    uint32_t count;
    Draft drafts[DM_SAMPLE_BATCH_MAX];
} DraftBank;

/* An undo step keeps only the drafts the bank held, not all DM_SAMPLE_BATCH_MAX slots. */
typedef struct HistoryNode {
    struct HistoryNode *next;
    size_t values;
    uint32_t count;
    Draft drafts[];
} HistoryNode;

typedef struct {
    HistoryNode *past, *future;
    unsigned count;
    bool grouping, recorded;
} History;

typedef struct Workspace {
    DraftBank bank;
    History history;
    char root[DM_PATH], id[DM_SAMPLE_ID_CAP];
    struct Workspace *next;
} Workspace;

typedef struct {
    uint64_t generation, next_token;
    DraftBank bank;
    History history;
    char root[DM_PATH], id[DM_SAMPLE_ID_CAP];
    Workspace *saved;
} SampleTool;

typedef struct {
    const SampleTool *owner;
    uint64_t generation;
    const char *paths[DM_SAMPLE_BATCH_MAX];
    char *existing[DM_SAMPLE_BATCH_MAX];
    uint32_t count, existing_count;
    bool ran, ready;
    DmSampleBatch batch;
    bool loading, same;
    char root[DM_PATH], id[DM_SAMPLE_ID_CAP];
    Workspace *incoming, *stash, *cached;
} Import;

static int32_t load_work(Import *work, const DmeSampleImportHost *host, char *error, size_t cap);

static char *full_path(const char *path);

static DmeString view(const char *s) {
    return (DmeString){s, strlen(s)};
}

static bool text_valid(DmeString text, uint64_t limit) {
    if (!text.data || !text.length || text.length >= limit ||
        memchr(text.data, 0, (size_t)text.length)) {
        return false;
    }

    const char *next = text.data;
    size_t remaining = (size_t)text.length;

    while (remaining) {
        Uint32 codepoint = SDL_StepUTF8(&next, &remaining);

        if (codepoint < 32 || codepoint == 127 || codepoint == SDL_INVALID_UNICODE_CODEPOINT) {
            return false;
        }
    }

    return true;
}

static void draft_free(Draft *draft) {
    if (!draft->shared || !--draft->shared->references) {
        dm_sample_audio_free(&draft->source.audio);
        free(draft->source.path);
        free(draft->source.peaks);
        free(draft->shared);
    }
}

static void bank_free(DraftBank *bank) {
    for (uint32_t i = 0; i < bank->count; ++i) {
        draft_free(&bank->drafts[i]);
    }

    memset(bank, 0, sizeof(*bank));
}

static void bank_names(DraftBank *bank) {
    for (uint32_t i = 0; i < bank->count; ++i) {
        bank->drafts[i].edit.name = view(bank->drafts[i].name);
    }
}

static void history_nodes_free(HistoryNode *node) {
    while (node) {
        HistoryNode *next = node->next;

        for (uint32_t i = 0; i < node->count; ++i) {
            draft_free(&node->drafts[i]);
        }

        free(node);
        node = next;
    }
}

/* Moves the bank's drafts into a new node, leaving the bank empty. */
static HistoryNode *history_take(DraftBank *bank) {
    HistoryNode *node = malloc(sizeof(*node) + bank->count * sizeof(*node->drafts));

    if (node) {
        *node = (HistoryNode){.values = bank->values, .count = bank->count};
        memcpy(node->drafts, bank->drafts, bank->count * sizeof(*node->drafts));
        memset(bank->drafts, 0, bank->count * sizeof(*bank->drafts));
        bank->values = bank->count = 0;
    }

    return node;
}

/* Moves a node's drafts into an empty bank and frees the node. */
static void history_give(DraftBank *bank, HistoryNode *node) {
    bank->values = node->values;
    bank->count = node->count;
    memcpy(bank->drafts, node->drafts, node->count * sizeof(*node->drafts));
    bank_names(bank);
    free(node);
}

static void history_free(History *history) {
    history_nodes_free(history->past);
    history_nodes_free(history->future);
    *history = (History){0};
}

static bool checkpoint(SampleTool *tool) {
    History *history = &tool->history;

    if (history->grouping && history->recorded) {
        return true;
    }

    for (uint32_t i = 0; i < tool->bank.count; ++i) {
        Draft *draft = &tool->bank.drafts[i];

        if (!draft->shared) {
            draft->shared = malloc(sizeof(*draft->shared));

            if (!draft->shared) {
                return false;
            }

            draft->shared->references = 1;
        }
    }

    HistoryNode *node = malloc(sizeof(*node) + tool->bank.count * sizeof(*node->drafts));

    if (!node) {
        return false;
    }

    *node = (HistoryNode){.values = tool->bank.values, .count = tool->bank.count};
    memcpy(node->drafts, tool->bank.drafts, tool->bank.count * sizeof(*node->drafts));

    for (uint32_t i = 0; i < node->count; ++i) {
        ++node->drafts[i].shared->references;
    }

    history_nodes_free(history->future);
    history->future = NULL;
    node->next = history->past;
    history->past = node;
    history->recorded = true;

    if (++history->count > 128) {
        HistoryNode **oldest = &history->past;

        for (unsigned i = 0; i < 128; ++i) {
            oldest = &(*oldest)->next;
        }

        history_nodes_free(*oldest);
        *oldest = NULL;
        history->count = 128;
    }

    return true;
}

static void DME_CALL history_group(void *instance, uint32_t active) {
    SampleTool *tool = instance;

    if (tool->history.grouping != (active != 0)) {
        tool->history.recorded = false;
        tool->history.grouping = active != 0;
    }
}

static uint32_t DME_CALL can_undo(const void *instance, uint32_t redo) {
    const SampleTool *tool = instance;
    return (redo ? tool->history.future : tool->history.past) != NULL;
}

static int32_t DME_CALL undo(void *instance, uint32_t redo) {
    SampleTool *tool = instance;
    History *history = &tool->history;
    HistoryNode **from = redo ? &history->future : &history->past;
    HistoryNode **to = redo ? &history->past : &history->future;
    HistoryNode *node = *from;

    if (!node) {
        return DME_UNAVAILABLE;
    }

    HistoryNode *current = history_take(&tool->bank);

    if (!current) {
        return DME_NOMEM;
    }

    *from = node->next;
    history_give(&tool->bank, node);
    current->next = *to;
    *to = current;
    history->count += redo ? 1 : -1;
    history->grouping = history->recorded = false;
    ++tool->generation;
    return DME_OK;
}

static void DME_CALL reset(void *instance) {
    SampleTool *tool = instance;
    bank_free(&tool->bank);
    history_free(&tool->history);

    while (tool->saved) {
        Workspace *next = tool->saved->next;
        bank_free(&tool->saved->bank);
        history_free(&tool->saved->history);
        free(tool->saved);
        tool->saved = next;
    }

    *tool->root = *tool->id = 0;
    ++tool->generation;
}

static void *DME_CALL create(void) {
    return calloc(1, sizeof(SampleTool));
}

static void DME_CALL destroy(void *instance) {
    reset(instance);
    free(instance);
}

static int32_t DME_CALL open_tool(void *instance) {
    return instance ? DME_OK : DME_INVALID;
}

static void DME_CALL destroy_import(void *job) {
    Import *work = job;

    if (work) {
        for (uint32_t i = 0; i < work->count; ++i) {
            free((void *)work->paths[i]);
        }

        for (uint32_t i = 0; i < work->existing_count; ++i) {
            free(work->existing[i]);
        }

        dm_sample_batch_free(&work->batch);

        if (work->incoming) {
            bank_free(&work->incoming->bank);
            free(work->incoming);
        }

        free(work->stash);
        free(work);
    }
}

static void *DME_CALL create_import(const void *instance, const DmeString *paths, uint32_t count,
                                    char *error, uint64_t capacity) {
    const SampleTool *tool = instance;

    if (!paths || !count || count > DM_SAMPLE_BATCH_MAX) {
        snprintf(error, (size_t)capacity, "%s", DM_TEXT_SAMPLE_TOOL_PATHS);
        return NULL;
    }

    Import *work = calloc(1, sizeof(*work));

    if (!work) {
        snprintf(error, (size_t)capacity, "%s", DM_TEXT_SAMPLE_IMPORT_MEMORY);
        return NULL;
    }

    work->owner = tool;
    work->generation = tool->generation;

    for (uint32_t i = 0; i < count; ++i) {
        DmeString path = paths[i];

        if (!text_valid(path, DM_PATH)) {
            snprintf(error, (size_t)capacity, "%s", DM_TEXT_SAMPLE_TOOL_PATHS);
            destroy_import(work);
            return NULL;
        }

        char *copy = malloc((size_t)path.length + 1);

        if (!copy) {
            snprintf(error, (size_t)capacity, "%s", DM_TEXT_SAMPLE_IMPORT_MEMORY);
            destroy_import(work);
            return NULL;
        }

        memcpy(copy, path.data, (size_t)path.length);
        copy[path.length] = 0;
        work->paths[work->count++] = copy;
    }

    for (uint32_t i = 0; i < tool->bank.count; ++i) {
        char *path = full_path(tool->bank.drafts[i].source.path);

        if (path) {
            work->existing[work->existing_count++] = path;
        }
    }

    return work;
}

static bool include_file(void *user, const char *path) {
    const Import *work = user;
    char *normalized = full_path(path);
    bool include = true;

    for (uint32_t i = 0; normalized && include && i < work->existing_count; ++i) {
#ifdef _WIN32
        include = SDL_strcasecmp(normalized, work->existing[i]) != 0;
#else
        include = strcmp(normalized, work->existing[i]) != 0;
#endif
    }

    free(normalized);
    return include;
}

static bool canceled(void *user) {
    const DmeSampleImportHost *host = user;
    return host && host->cancelled && host->cancelled(host->user);
}

static void progress(void *user, DmSampleBatchStage stage, const char *path, size_t index,
                     size_t total, uint64_t done, uint64_t units) {
    const DmeSampleImportHost *host = user;

    if (host && host->progress) {
        static const uint32_t stages[] = {DME_SAMPLE_SCAN, DME_SAMPLE_DECODE, DME_SAMPLE_ANALYZE};
        host->progress(host->user, stages[stage], view(path), (uint32_t)index, (uint32_t)total,
                       done, units);
    }
}

static int32_t DME_CALL run_import(void *job, const DmeSampleImportHost *host, char *error,
                                   uint64_t capacity) {
    Import *work = job;

    if (work->ran) {
        return DME_INVALID;
    }

    work->ran = true;

    if (work->loading) {
        return load_work(work, host, error, (size_t)capacity);
    }

    DmSampleBatchCallbacks cb = {(void *)host, canceled, progress, include_file, work};
    work->ready = dm_sample_batch_import(work->paths, work->count, &cb, &work->batch, error,
                                         (size_t)capacity);
    return work->ready ? DME_OK : canceled((void *)host) ? DME_CANCELLED : DME_FAILED;
}

static void default_name(Draft *draft) {
    const char *name = *draft->source_name ? draft->source_name : draft->source.path;

    for (const char *p = name; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }

    const char *dot = strrchr(name, '.');
    size_t length = dot && dot > name ? (size_t)(dot - name) : strlen(name);
    size_t limit = sizeof(draft->name) - 1;

    if (length > limit) {
        length = limit;

        while (length && ((unsigned char)name[length] & 0xc0u) == 0x80u) {
            --length;
        }
    }

    memcpy(draft->name, name, length);
    draft->name[length] = 0;
}

static void import_defaults(Draft *draft) {
    default_name(draft);
    const DmSampleAnalysis *a = &draft->source.analysis;
    bool drums = !draft->source.error[0] && a->drum_key >= 0;
    DmSampleEnvelope e = a->envelope;
    unsigned lo = drums ? (unsigned)a->drum_key : 0;
    unsigned hi = drums ? (unsigned)a->drum_key : 127;
    draft->edit = (DmeSampleEdit){.size = sizeof(DmeSampleEdit),
                                  .name = view(draft->name),
                                  .drums = drums,
                                  .program = draft->source.error[0] ? -1 : a->program,
                                  .drum_key = drums ? a->drum_key : -1,
                                  .melodic_key_hi = 127,
                                  .zone = {.size = sizeof(DmeSampleZoneParams),
                                           .root = (unsigned)a->root,
                                           .loop_mode = draft->source.audio.embedded_loop_end
                                                            ? DME_SAMPLE_LOOP_SUSTAIN
                                                            : DME_SAMPLE_NO_LOOP,
                                           .loop_start = draft->source.audio.embedded_loop_start,
                                           .loop_end = draft->source.audio.embedded_loop_end,
                                           .cents = a->cents,
                                           .key_lo = lo,
                                           .key_hi = hi,
                                           .velocity_lo = 1,
                                           .velocity_hi = 127,
                                           .delay = e.delay,
                                           .attack = e.attack,
                                           .hold = e.hold,
                                           .decay = e.decay,
                                           .sustain = e.sustain,
                                           .release = e.release,
                                           .tone.tracking = drums ? 0 : 100}};
}

static void initialize_draft(Draft *draft, DmSampleBatchItem *item) {
    *draft = (Draft){.source = *item};
    *item = (DmSampleBatchItem){0};
    import_defaults(draft);
}

static void *DME_CALL create_load(const void *instance, DmeString root, DmeString set_id,
                                  char *error, uint64_t capacity) {
    const SampleTool *tool = instance;

    if ((!root.length != !set_id.length) ||
        (root.length && (!text_valid(root, DM_PATH) || !text_valid(set_id, DM_SAMPLE_ID_CAP)))) {
        snprintf(error, (size_t)capacity, "%s", DM_TEXT_SAMPLE_TOOL_PATHS);
        return NULL;
    }

    Import *work = calloc(1, sizeof(*work));

    if (!work) {
        snprintf(error, (size_t)capacity, "%s", DM_TEXT_SAMPLE_IMPORT_MEMORY);
        return NULL;
    }

    work->owner = tool;
    work->generation = tool->generation;
    work->loading = true;
    snprintf(work->root, sizeof(work->root), "%.*s", (int)root.length, root.data ? root.data : "");
    snprintf(work->id, sizeof(work->id), "%.*s", (int)set_id.length,
             set_id.data ? set_id.data : "");
    work->same = !strcmp(tool->root, work->root) && !strcmp(tool->id, work->id);

    if (work->same) {
        return work;
    }

    work->stash = calloc(1, sizeof(*work->stash));

    for (Workspace *saved = tool->saved; saved; saved = saved->next) {
        if (!strcmp(saved->root, work->root) && !strcmp(saved->id, work->id)) {
            work->cached = saved;
            break;
        }
    }

    if (!work->cached) {
        work->incoming = calloc(1, sizeof(*work->incoming));
    }

    if (!work->stash || (!work->cached && !work->incoming)) {
        snprintf(error, (size_t)capacity, "%s", DM_TEXT_SAMPLE_IMPORT_MEMORY);
        destroy_import(work);
        return NULL;
    }

    return work;
}

static int32_t load_work(Import *work, const DmeSampleImportHost *host, char *error, size_t cap) {
    if (canceled((void *)host)) {
        return DME_CANCELLED;
    }

    if (work->same || work->cached || !*work->id) {
        work->ready = true;
        return DME_OK;
    }

    DmSampleManifest manifest = {0};

    if (!dm_sample_library_manifest(work->root, work->id, &manifest, error, cap)) {
        return DME_FAILED;
    }

    DraftBank *bank = &work->incoming->bank;
    DmSampleImportCallbacks callbacks = {(void *)host, canceled, NULL};
    bool ok = manifest.zone_count <= DM_SAMPLE_BATCH_MAX;

    if (!ok) {
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_TOOL_DRAFT_LIMIT);
    }

    for (uint32_t p = 0; ok && p < manifest.preset_count; ++p) {
        const DmSamplePreset *preset = &manifest.presets[p];

        for (uint32_t i = 0; ok && i < preset->zone_count; ++i) {
            const DmSampleZone *zone = &manifest.zones[preset->first_zone + i];
            const DmSampleAsset *asset = &manifest.assets[zone->asset];
            size_t values = (size_t)asset->frames * asset->channels;

            if (values > DRAFT_MAX_VALUES - bank->values) {
                snprintf(error, cap, "%s", DM_TEXT_SAMPLE_TOOL_DRAFT_LIMIT);
                ok = false;
                break;
            }

            Draft *draft = &bank->drafts[bank->count++];
            DmSampleBatchItem *source = &draft->source;
            char path[DM_PATH];
            int length = snprintf(path, sizeof(path), "%s/%s/%s", work->root, manifest.directory,
                                  asset->file);
            source->path = length > 0 && length < (int)sizeof(path) ? strdup(path) : NULL;
            source->peaks = calloc(DM_SAMPLE_BATCH_PEAKS, sizeof(*source->peaks));
            ok = source->path && source->peaks &&
                 dm_sample_library_audio(work->root, &manifest, zone->asset, &callbacks,
                                         &source->audio, error, cap) &&
                 dm_sample_peaks(&source->audio, source->peaks, DM_SAMPLE_BATCH_PEAKS,
                                 &source->absolute_peak, &callbacks);

            if (!ok) {
                break;
            }

            bank->values += values;
            snprintf(draft->name, sizeof(draft->name), "%s", preset->name);
            snprintf(draft->asset_id, sizeof(draft->asset_id), "%s", asset->id);
            snprintf(draft->asset_name, sizeof(draft->asset_name), "%.*s",
                     (int)(strlen(asset->file) - 4), asset->file);
            snprintf(draft->preset_id, sizeof(draft->preset_id), "%s", preset->id);
            snprintf(draft->source_name, sizeof(draft->source_name), "%s",
                     *asset->source_name ? asset->source_name : asset->file);
            ok = dm_sample_analyze(&source->audio, draft->source_name, &callbacks,
                                   &source->analysis);

            if (!ok) {
                break;
            }

            draft->preset_drums = preset->drums;
            draft->preset_zone = i;
            draft->edit = (DmeSampleEdit){
                .size = sizeof(DmeSampleEdit),
                .drums = preset->drums,
                .program = preset->suggested_program ? (int)preset->suggested_program - 1 : -1,
                .drum_key = preset->drums ? zone->key_lo : -1,
                .melodic_key_lo = preset->drums ? 0 : zone->key_lo,
                .melodic_key_hi = preset->drums ? 127 : zone->key_hi,
                .zone = dm_sample_zone_params(zone)};

            if (host && host->progress) {
                host->progress(host->user, DME_SAMPLE_DECODE, view(source->path), bank->count - 1,
                               manifest.zone_count, asset->frames, asset->frames);
            }
        }
    }

    dm_sample_manifest_free(&manifest);
    work->ready = ok && !canceled((void *)host);
    return work->ready ? DME_OK : canceled((void *)host) ? DME_CANCELLED : DME_FAILED;
}

static int32_t finish_load(SampleTool *tool, Import *work) {
    if (work->same) {
        work->ready = false;
        return DME_OK;
    }

    Workspace *target = work->cached ? work->cached : work->incoming;

    if (work->cached) {
        Workspace **at = &tool->saved;

        while (*at && *at != target) {
            at = &(*at)->next;
        }

        if (!*at) {
            return DME_CANCELLED;
        }

        *at = target->next;
    }

    Workspace *stash = work->stash;
    stash->bank = tool->bank;
    stash->history = tool->history;
    snprintf(stash->root, sizeof(stash->root), "%s", tool->root);
    snprintf(stash->id, sizeof(stash->id), "%s", tool->id);
    bank_names(&stash->bank);
    stash->next = tool->saved;
    tool->saved = stash;
    work->stash = NULL;
    tool->bank = target->bank;
    tool->history = target->history;

    for (uint32_t i = 0; i < tool->bank.count; ++i) {
        if (!tool->bank.drafts[i].token) {
            tool->bank.drafts[i].token = ++tool->next_token;
        }
    }
    bank_names(&tool->bank);
    snprintf(tool->root, sizeof(tool->root), "%s", work->root);
    snprintf(tool->id, sizeof(tool->id), "%s", work->id);
    free(target);
    work->incoming = work->cached = NULL;
    work->ready = false;
    ++tool->generation;
    return DME_OK;
}

static int32_t DME_CALL published(void *instance, DmeString root, DmeString set_id) {
    SampleTool *tool = instance;

    if (!text_valid(root, DM_PATH) || !text_valid(set_id, DM_SAMPLE_ID_CAP)) {
        return DME_INVALID;
    }

    snprintf(tool->root, sizeof(tool->root), "%.*s", (int)root.length, root.data);
    snprintf(tool->id, sizeof(tool->id), "%.*s", (int)set_id.length, set_id.data);
    return DME_OK;
}

static char *full_path(const char *path) {
#ifdef _WIN32
    wchar_t buffer[DM_PATH];
    wchar_t *wide = (wchar_t *)SDL_iconv_string("UTF-16LE", "UTF-8", path, strlen(path) + 1);
    DWORD length = wide ? GetFullPathNameW(wide, DM_PATH, buffer, NULL) : 0;
    SDL_free(wide);

    if (!length || length >= DM_PATH) {
        return NULL;
    }

    char *utf8 =
        SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)buffer, (length + 1) * sizeof(wchar_t));
    char *copy = utf8 ? strdup(utf8) : NULL;
    SDL_free(utf8);
    return copy;
#else
    return realpath(path, NULL);
#endif
}

static bool folder_duplicate(const SampleTool *tool, const Import *work, size_t index) {
    const DmSampleBatchItem *item = &work->batch.items[index];

    if (!item->from_directory) {
        return false;
    }

    char *path = full_path(item->path);
    bool duplicate = false;

    for (size_t i = 0; path && !duplicate && i < tool->bank.count + index; ++i) {
        const char *other = i < tool->bank.count ? tool->bank.drafts[i].source.path
                                                 : work->batch.items[i - tool->bank.count].path;
        char *normalized = full_path(other);

        if (normalized) {
#ifdef _WIN32
            duplicate = !SDL_strcasecmp(path, normalized);
#else
            duplicate = !strcmp(path, normalized);
#endif
        }

        free(normalized);
    }

    free(path);
    return duplicate;
}

static int32_t DME_CALL finish_import(void *instance, void *job, char *error, uint64_t capacity) {
    SampleTool *tool = instance;
    Import *work = job;

    if (work->owner != tool || !work->ready) {
        return DME_INVALID;
    }

    if (work->generation != tool->generation) {
        snprintf(error, (size_t)capacity, "%s", DM_TEXT_SAMPLE_TOOL_STALE);
        return DME_CANCELLED;
    }

    if (work->loading) {
        return finish_load(tool, work);
    }

    size_t values = tool->bank.values, added = 0;
    bool skip[DM_SAMPLE_BATCH_MAX] = {0};

    for (size_t i = 0; i < work->batch.count; ++i) {
        skip[i] = folder_duplicate(tool, work, i);

        if (skip[i]) {
            continue;
        }

        const DmSampleAudio *a = &work->batch.items[i].audio;
        values += a->frames * (size_t)a->channels;
        ++added;
    }

    if (added > DM_SAMPLE_BATCH_MAX - tool->bank.count || values > DRAFT_MAX_VALUES) {
        snprintf(error, (size_t)capacity, "%s", DM_TEXT_SAMPLE_TOOL_DRAFT_LIMIT);
        return DME_FAILED;
    }

    if (added && !checkpoint(tool)) {
        return DME_NOMEM;
    }

    for (size_t i = 0; i < work->batch.count; ++i) {
        if (!skip[i]) {
            Draft *draft = &tool->bank.drafts[tool->bank.count++];
            initialize_draft(draft, &work->batch.items[i]);
            draft->token = ++tool->next_token;
        }
    }

    tool->bank.values = values;
    work->ready = false;
    return DME_OK;
}

static int32_t DME_CALL remove_draft(void *instance, uint32_t index) {
    SampleTool *tool = instance;

    if (index >= tool->bank.count) {
        return DME_INVALID;
    }

    if (!checkpoint(tool)) {
        return DME_NOMEM;
    }

    DmSampleBatchItem *item = &tool->bank.drafts[index].source;
    tool->bank.values -= item->audio.frames * (size_t)item->audio.channels;
    draft_free(&tool->bank.drafts[index]);
    --tool->bank.count;
    memmove(tool->bank.drafts + index, tool->bank.drafts + index + 1,
            (tool->bank.count - index) * sizeof(*tool->bank.drafts));
    tool->bank.drafts[tool->bank.count] = (Draft){0};

    for (uint32_t i = index; i < tool->bank.count; ++i) {
        tool->bank.drafts[i].edit.name = view(tool->bank.drafts[i].name);
    }

    ++tool->generation;
    return DME_OK;
}

static uint32_t DME_CALL count_drafts(const void *instance) {
    const SampleTool *tool = instance;
    return tool->bank.count;
}

static int32_t DME_CALL describe_draft(const void *instance, uint32_t index, DmeSampleDraft *out) {
    const SampleTool *tool = instance;

    if (!out || index >= tool->bank.count) {
        return DME_INVALID;
    }

    const Draft *d = &tool->bank.drafts[index];
    const DmSampleBatchItem *s = &d->source;
    static const uint32_t sources[] = {DME_ROOT_DEFAULT, DME_ROOT_SIGNAL, DME_ROOT_FILENAME,
                                       DME_ROOT_EMBEDDED};
    *out = (DmeSampleDraft){.size = sizeof(*out),
                            .token = d->token,
                            .channels = (uint32_t)s->audio.channels,
                            .rate = (uint32_t)s->audio.rate,
                            .frames = s->audio.frames,
                            .path = view(s->path),
                            .format = view(s->audio.format),
                            .codec = view(s->audio.codec),
                            .error = view(s->error),
                            .root_source = sources[s->analysis.source],
                            .pitch_conflict = s->analysis.conflicting_pitch,
                            .frequency = s->analysis.frequency,
                            .confidence = s->analysis.confidence,
                            .peak_count = s->peaks ? DM_SAMPLE_BATCH_PEAKS : 0,
                            .absolute_peak = s->absolute_peak,
                            .preset_drums = d->preset_drums,
                            .preset_zone = d->preset_zone,
                            .asset_id = view(d->asset_id),
                            .preset_id = view(d->preset_id),
                            .source_name = view(d->source_name),
                            .asset_name = view(d->asset_name),
                            .edit = d->edit,
                            .embedded_root = s->audio.embedded_root,
                            .embedded_cents = s->audio.embedded_cents,
                            .embedded_loop_start = s->audio.embedded_loop_start,
                            .embedded_loop_end = s->audio.embedded_loop_end};
    return DME_OK;
}

static bool between(double value, double low, double high) {
    return value >= low && value <= high;
}

static bool valid_zone(const DmeSampleZoneParams *z, uint64_t frames) {
    uint64_t end = z->playback_end ? z->playback_end : frames;
    return z->playback_set <= 1 && z->playback_start < end && end <= frames &&
           z->size == sizeof(*z) && z->root <= 127 && z->key_lo <= z->key_hi && z->key_hi <= 127 &&
           z->velocity_lo >= 1 && z->velocity_lo <= z->velocity_hi && z->velocity_hi <= 127 &&
           z->cut_group <= 127 && z->cut_by <= 127 && z->loop_mode <= DME_SAMPLE_LOOP_SUSTAIN &&
           between(z->cents, -100, 100) && between(z->gain_db, -96, 12) && between(z->pan, -1, 1) &&
           between(z->delay, 0, 20) && between(z->attack, 0, 100) && between(z->hold, 0, 20) &&
           between(z->decay, 0, 100) && between(z->sustain, 0, 1) && between(z->release, 0, 100) &&
           dm_sample_tone_valid(&z->tone) &&
           (z->loop_mode == DME_SAMPLE_NO_LOOP ||
            (z->loop_start >= z->playback_start && z->loop_start < z->loop_end &&
             z->loop_end <= end && z->loop_end - z->loop_start >= 2));
}

static int32_t DME_CALL edit_draft(void *instance, uint32_t index, const DmeSampleEdit *edit) {
    SampleTool *tool = instance;

    if (!edit || edit->size != sizeof(*edit) || index >= tool->bank.count ||
        !text_valid(edit->name, DM_SAMPLE_NAME_CAP) || edit->drums > 1 || edit->program < -1 ||
        edit->program > 127 || edit->drum_key < -1 || edit->drum_key > 127 ||
        edit->melodic_key_lo > edit->melodic_key_hi || edit->melodic_key_hi > 127) {
        return DME_INVALID;
    }

    Draft *draft = &tool->bank.drafts[index];

    if (draft->source.error[0] || !valid_zone(&edit->zone, draft->source.audio.frames) ||
        (edit->drums && (edit->drum_key < 0 || edit->zone.key_lo != (unsigned)edit->drum_key ||
                         edit->zone.key_hi != (unsigned)edit->drum_key))) {
        return DME_INVALID;
    }

    if (!checkpoint(tool)) {
        return DME_NOMEM;
    }

    memmove(draft->name, edit->name.data, (size_t)edit->name.length);
    draft->name[edit->name.length] = 0;
    draft->edit = *edit;
    draft->edit.name = view(draft->name);

    if (*draft->preset_id && draft->preset_drums == (edit->drums != 0)) {
        for (uint32_t i = 0; i < tool->bank.count; ++i) {
            Draft *sibling = &tool->bank.drafts[i];

            if (sibling != draft && !strcmp(sibling->preset_id, draft->preset_id) &&
                sibling->edit.drums == edit->drums) {
                memcpy(sibling->name, draft->name, sizeof(sibling->name));
                sibling->edit.name = view(sibling->name);
                sibling->edit.program = edit->program;
            }
        }
    }

    return DME_OK;
}

static int32_t DME_CALL reset_edit(void *instance, uint32_t index) {
    SampleTool *tool = instance;

    if (index >= tool->bank.count || tool->bank.drafts[index].source.error[0]) {
        return DME_INVALID;
    }

    Draft *draft = &tool->bank.drafts[index];
    DmeSampleEdit previous = draft->edit;
    char previous_name[DM_SAMPLE_NAME_CAP];
    memcpy(previous_name, draft->name, sizeof(previous_name));
    import_defaults(draft);
    DmeSampleEdit initial = draft->edit;
    char name[DM_SAMPLE_NAME_CAP];
    memcpy(name, draft->name, sizeof(name));
    initial.name = view(name);
    draft->edit = previous;
    memcpy(draft->name, previous_name, sizeof(draft->name));
    return edit_draft(instance, index, &initial);
}

static int32_t DME_CALL read_pcm(const void *instance, uint32_t index, uint64_t start,
                                 uint64_t frames, float *pcm, uint64_t capacity) {
    const SampleTool *tool = instance;

    if (!pcm || index >= tool->bank.count) {
        return DME_INVALID;
    }

    const DmSampleAudio *a = &tool->bank.drafts[index].source.audio;
    uint64_t channels = (uint64_t)a->channels;

    if (!a->pcm || start > a->frames || frames > a->frames - start ||
        frames * channels > capacity) {
        return DME_INVALID;
    }

    memcpy(pcm, a->pcm + start * channels, (size_t)(frames * channels) * sizeof(*pcm));
    return DME_OK;
}

static int32_t DME_CALL read_peaks(const void *instance, uint32_t index, uint32_t first,
                                   uint32_t count, DmeSamplePeak *out) {
    const SampleTool *tool = instance;

    if (!out || index >= tool->bank.count || first > DM_SAMPLE_BATCH_PEAKS ||
        count > DM_SAMPLE_BATCH_PEAKS - first || !tool->bank.drafts[index].source.peaks) {
        return DME_INVALID;
    }

    const DmSamplePeak *peaks = tool->bank.drafts[index].source.peaks + first;

    for (uint32_t i = 0; i < count; ++i) {
        memcpy(out[i].minimum, peaks[i].minimum, sizeof(out[i].minimum));
        memcpy(out[i].maximum, peaks[i].maximum, sizeof(out[i].maximum));
    }

    return DME_OK;
}

static int32_t DME_CALL snap_loop(const void *instance, uint32_t index, uint64_t *start,
                                  uint64_t *end) {
    const SampleTool *tool = instance;

    if (!start || !end || index >= tool->bank.count) {
        return DME_INVALID;
    }

    const DmSampleAudio *audio = &tool->bank.drafts[index].source.audio;

    if (!audio->pcm || *end > audio->frames || *start >= *end || *end - *start < 2) {
        return DME_INVALID;
    }

    DmSampleLoop snapped;

    if (!dm_sample_loop_snap(audio, (DmSampleLoop){(size_t)*start, (size_t)*end}, &snapped)) {
        return DME_UNAVAILABLE;
    }

    *start = snapped.start;
    *end = snapped.end;
    return DME_OK;
}

static const void *DME_CALL interface_at(uint32_t version) {
    static const DmeSamplesApi api = {sizeof(DmeSamplesApi),
                                      DME_SAMPLES_API,
                                      create_import,
                                      run_import,
                                      finish_import,
                                      destroy_import,
                                      count_drafts,
                                      describe_draft,
                                      edit_draft,
                                      read_pcm,
                                      read_peaks,
                                      snap_loop,
                                      remove_draft,
                                      reset,
                                      create_load,
                                      published,
                                      reset_edit,
                                      history_group,
                                      can_undo,
                                      undo};
    return version == DME_SAMPLES_API ? &api : NULL;
}

static const DmeTool samples = {
    sizeof(DmeTool), DME_SAMPLES_TOOL_ID, DM_TEXT_SAMPLE_TOOL_NAME, create, destroy,
    open_tool,       interface_at};

static const DmeTool *DME_CALL tool_at(uint32_t index) {
    return index == 0 ? &samples : NULL;
}

static const DmePlugin plugin = {.abi = DME_ABI,
                                 .size = sizeof(DmePlugin),
                                 .id = "org.duckmidi.sampletool",
                                 .name = DM_TEXT_SAMPLE_TOOL_NAME,
                                 .tool_count = 1,
                                 .tool = tool_at};

DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi) {
    return abi == DME_ABI ? &plugin : NULL;
}
