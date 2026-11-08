#include "internal.h"
#include "../../src/text/en.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The history runtime's host-facing jobs, the same for every exporter. One job inspects,
   initializes, reverts or installs into one checkout and keeps an immutable view of the result
   for the history panel. */

typedef struct {
    const DmeHistoryAdapter *adapter;
    char *exporter;
    void *instance;
    void *export_job;
    uint32_t action;
    uint64_t scope;
    uint64_t generation;
    bool prepared;
    bool untracked;      /* History off; see DmeRepositoryRequest.untracked. */
    bool keep_originals; /* Keep original files; used by untracked installs. */
    char *plugins, *checkout, *key;
    DmeHistoryTarget target;
    char identity[96], notice[2048];
    EsScopes scopes;
    EsModifications modifications;
    EsTargets targets;
    DmeRepositoryScope *scope_views;
    DmeRepositoryRow *row_views;
    DmeString **row_paths;
} Job;

static DmeString string(const char *s) {
    return (DmeString){s ? s : "", s ? strlen(s) : 0};
}

static char *copy(DmeString s) {
    if (s.length > 8192 || (s.length && (!s.data || memchr(s.data, 0, (size_t)s.length)))) {
        return NULL;
    }

    char *p = SDL_malloc((size_t)s.length + 1);

    if (p) {
        if (s.length) {
            memcpy(p, s.data, (size_t)s.length);
        }

        p[s.length] = 0;
    }

    return p;
}

bool es_targets_add(EsTargets *t, const char *id, const char *label, const char *path) {
    if (!id || !label || !path || t->count >= 20000 ||
        !es_reserve((void **)&t->items, &t->capacity, t->count + 1, sizeof(*t->items))) {
        return false;
    }

    char *a = SDL_strdup(id), *b = SDL_strdup(label), *c = SDL_strdup(path);

    if (!a || !b || !c) {
        SDL_free(a);
        SDL_free(b);
        SDL_free(c);
        return false;
    }

    t->items[t->count++] = (DmeSampleTarget){string(a), string(b), string(c)};
    return true;
}

void es_targets_free(EsTargets *t) {
    for (size_t i = 0; i < t->count; i++) {
        SDL_free((void *)t->items[i].id.data);
        SDL_free((void *)t->items[i].label.data);
        SDL_free((void *)t->items[i].path.data);
    }

    free(t->items);
    *t = (EsTargets){0};
}

static void DME_CALL destroy_job(void *job) {
    Job *b = job;

    if (!b) {
        return;
    }

    if (b->row_paths) {
        for (size_t i = 0; i < b->modifications.count; i++) {
            SDL_free(b->row_paths[i]);
        }
    }

    SDL_free(b->row_paths);
    SDL_free(b->row_views);
    SDL_free(b->scope_views);
    es_targets_free(&b->targets);
    es_scopes_free(&b->scopes);
    es_modifications_free(&b->modifications);
    SDL_free(b->exporter);
    SDL_free(b->plugins);
    SDL_free(b->checkout);
    SDL_free(b->key);
    SDL_free(b);
}

static void *DME_CALL create_job(const DmeHistoryAdapter *adapter, const char *exporter,
                                 void *instance, void *export_job,
                                 const DmeRepositoryRequest *request, char *error, uint64_t cap) {
    if (!adapter || adapter->size != sizeof(*adapter) || adapter->version != DME_REPOSITORY_API ||
        !adapter->detect || !adapter->render || !adapter->install || !exporter || !request ||
        request->size != sizeof(*request) || request->action > DME_REPOSITORY_REVERT_ALL ||
        (request->action == DME_REPOSITORY_INSTALL && !export_job) ||
        (request->untracked && request->action != DME_REPOSITORY_INSPECT &&
         request->action != DME_REPOSITORY_INSTALL)) {
        es_fail(error, (size_t)cap, DM_TEXT_REPOSITORY_INVALID_REPOSITORY_ACTION);
        return NULL;
    }

    Job *b = SDL_calloc(1, sizeof(*b));

    if (!b) {
        return NULL;
    }

    *b = (Job){.adapter = adapter,
               .exporter = SDL_strdup(exporter),
               .instance = instance,
               .export_job = export_job,
               .action = request->action,
               .scope = request->scope,
               .untracked = request->untracked != 0,
               .keep_originals = request->keep_originals != 0,
               .plugins = copy(request->plugins_directory),
               .checkout = copy(request->checkout),
               .key = copy(request->logical_key)};

    if (!b->exporter || !b->plugins || !b->checkout || !b->key) {
        destroy_job(b);
        es_fail(error, (size_t)cap, DM_TEXT_REPOSITORY_INVALID_REPOSITORY_ACTION);
        return NULL;
    }

    if (export_job && adapter->begin_install) {
        adapter->begin_install(export_job, request);
    }

    return b;
}

static EsRepository *open_repository(Job *b, const DmeHost *host, char *error, size_t cap) {
    /* A selected history section names its checkout; otherwise the request's folder is used. */
    if (b->scope && !b->untracked) {
        EsScopes scopes = {0};
        const EsScope *scope = NULL;

        if (!es_history_scopes(b->plugins, b->exporter, &scopes, error, cap)) {
            return NULL;
        }

        for (size_t i = 0; i < scopes.count; i++) {
            if (scopes.items[i].id == b->scope) {
                scope = &scopes.items[i];
            }
        }

        if (scope) {
            SDL_free(b->checkout);
            b->checkout = SDL_strdup(scope->checkout);
        }

        es_scopes_free(&scopes);

        if (!scope) {
            es_fail(error, cap,
                    DM_TEXT_REPOSITORY_THE_SELECTED_REPOSITORY_HISTORY_SCOPE_IS_UNAVAILABLE);
            return NULL;
        }
    }

    EsRoot *root = b->checkout ? es_root_open(b->checkout, error, cap) : NULL;
    b->target = (DmeHistoryTarget){0};
    bool valid = root && b->adapter->detect(es_history_api(), (DmeHistoryRoot *)root, &b->target,
                                            error, cap);
    es_root_close(root);

    if (!valid) {
        return NULL;
    }

    b->target.id[sizeof(b->target.id) - 1] = 0;
    b->target.name[sizeof(b->target.name) - 1] = 0;
    b->target.version[sizeof(b->target.version) - 1] = 0;

    if (b->untracked) {
        EsRepository *repo =
            es_repository_open_untracked(b->checkout, b->adapter, host, b->keep_originals, error,
                                         cap);

        if (repo) {
            snprintf(b->identity, sizeof(b->identity), "%s", es_repository_identity(repo));
        }

        return repo;
    }

    EsOpen options = {.plugins = b->plugins,
                      .checkout = b->checkout,
                      .exporter = b->exporter,
                      .target = b->target.id,
                      .target_name = *b->target.name ? b->target.name : b->target.id,
                      .capabilities = b->target.capabilities & DME_REPOSITORY_CUSTOM,
                      .version = b->target.version,
                      .host = host,
                      .adapter = b->adapter};
    EsRepository *repo = es_repository_open(&options, error, cap);

    if (repo) {
        snprintf(b->identity, sizeof(b->identity), "%s", es_repository_identity(repo));
    }

    if (repo && b->scope && es_repository_scope_id(repo) != b->scope) {
        es_fail(error, cap,
                DM_TEXT_REPOSITORY_THE_CHECKOUT_IDENTITY_OR_BUILD_VERSION_CHANGED_SELECT_ITS);
        es_repository_close(repo);
        return NULL;
    }

    return repo;
}

/* Install preflight against the current repository. The generation it saw must still be
   current when the install runs, so a concurrent change cannot be overwritten. */
static int32_t DME_CALL prepare(void *job, const DmeContext *c, const DmeHost *host, char *error,
                                uint64_t cap) {
    Job *b = job;

    if (b->action != DME_REPOSITORY_INSTALL) {
        return DME_OK;
    }

    EsRepository *repo = open_repository(b, host, error, (size_t)cap);
    int32_t status = repo ? DME_OK : DME_FAILED;

    if (repo && !b->untracked) {
        b->scope = es_repository_scope_id(repo);
    }

    if (repo && b->adapter->prepare) {
        status = b->adapter->prepare(b->export_job, c, host, es_history_api(), (DmeHistory *)repo,
                                     &b->target, error, cap);
    }

    if (status == DME_OK) {
        b->generation = es_repository_generation(repo);
        b->prepared = b->untracked || b->generation != 0;
        status = b->prepared ? DME_OK : DME_FAILED;
    }

    es_repository_close(repo);
    return status;
}

static bool describe_rows(Job *b) {
    b->scope_views = SDL_calloc(b->scopes.count + 1, sizeof(*b->scope_views));
    b->row_views = SDL_calloc(b->modifications.count + 1, sizeof(*b->row_views));
    b->row_paths = SDL_calloc(b->modifications.count + 1, sizeof(*b->row_paths));

    if (!b->scope_views || !b->row_views || !b->row_paths) {
        return false;
    }

    for (size_t i = 0; i < b->scopes.count; i++) {
        const EsScope *s = &b->scopes.items[i];
        SDL_PathInfo info;
        bool available = SDL_GetPathInfo(s->checkout, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
        bool selected_error = s->id == b->scope && *b->notice;
        b->scope_views[i] = (DmeRepositoryScope){
            .size = sizeof(DmeRepositoryScope),
            .id = s->id,
            .capabilities = DME_REPOSITORY_INITIALIZED |
                            (available && !selected_error ? DME_REPOSITORY_WRITABLE : 0) |
                            (!available ? DME_REPOSITORY_MISSING : 0) |
                            (s->capabilities & DME_REPOSITORY_CUSTOM),
            .target = string(s->target),
            .target_name = string(s->target_name),
            .checkout = string(s->checkout),
            .game_version = string(s->version)};
    }

    for (size_t i = 0; i < b->modifications.count; i++) {
        const EsModification *m = &b->modifications.items[i];
        DmeString *paths = SDL_calloc(m->paths.count + 1, sizeof(*paths));
        b->row_paths[i] = paths;

        if (!paths) {
            return false;
        }

        for (size_t j = 0; j < m->paths.count; j++) {
            paths[j] = string(m->paths.items[j]);
        }

        b->row_views[i] = (DmeRepositoryRow){.size = sizeof(DmeRepositoryRow),
                                             .can_revert = 1,
                                             .path_count = (uint32_t)m->paths.count,
                                             .key = string(m->key),
                                             .label = string(m->label),
                                             .paths = paths};
    }

    return true;
}

static void DME_CALL quiet_diagnostic(void *user, uint32_t severity, uint32_t flags,
                                      const char *message) {
    (void)user;
    (void)severity;
    (void)flags;
    (void)message;
}

static int32_t DME_CALL run(void *job, const DmeContext *c, const DmeHost *host, char *error,
                            uint64_t cap) {
    Job *b = job;
    EsRepository *repo = open_repository(b, host, error, (size_t)cap);
    bool ok = repo != NULL;

    if (ok && !b->untracked) {
        b->scope = es_repository_scope_id(repo);
    }

    if (ok) {
        switch (b->action) {
        case DME_REPOSITORY_INSTALL:
            if (b->prepared && b->generation != es_repository_generation(repo)) {
                ok = es_fail(
                    error, (size_t)cap,
                    DM_TEXT_REPOSITORY_REPOSITORY_AUDIO_OR_HISTORY_CHANGED_DURING_PREFLIGHT_START_THE);
            } else if (!b->prepared && b->adapter->prepare) {
                /* Preflight already reported its diagnostics when the user reviewed it. */
                DmeHost preparation = *host;
                preparation.diagnostic = quiet_diagnostic;
                ok = b->adapter->prepare(b->export_job, c, &preparation, es_history_api(),
                                         (DmeHistory *)repo, &b->target, error, cap) == DME_OK;
            }

            ok = ok && b->adapter->install(b->export_job, c, host, es_history_api(),
                                           (DmeHistory *)repo, error, cap) == DME_OK;
            break;
        case DME_REPOSITORY_INITIALIZE:
            ok = es_repository_initialize(repo, error, (size_t)cap);
            break;
        case DME_REPOSITORY_REVERT:
        case DME_REPOSITORY_REVERT_ALL:
            ok = es_repository_revert(repo, b->action == DME_REPOSITORY_REVERT ? b->key : NULL,
                                      error, (size_t)cap);
            break;
        default:
            break;
        }
    }

    /* The view reflects the stored history even after a failed action. */
    if (repo) {
        char view_error[1024] = {0};
        bool viewed =
            (b->untracked || es_repository_modifications(repo, &b->modifications, view_error,
                                                         sizeof(view_error))) &&
            (!b->adapter->targets ||
             b->adapter->targets(es_history_api(), (DmeHistory *)repo,
                                 (DmeHistoryTargets *)&b->targets, view_error, sizeof(view_error)));

        if (ok && !viewed) {
            ok = es_fail(error, (size_t)cap, "%s", view_error);
        }
    }

    es_repository_close(repo);
    ok = es_history_scopes(b->plugins, b->exporter, &b->scopes, error, (size_t)cap) && ok;

    if (!ok && b->action == DME_REPOSITORY_INSPECT) {
        snprintf(b->notice, sizeof(b->notice), "%s", error);
        *error = 0;
        ok = true;
    }

    ok = describe_rows(b) && ok;
    return ok                                               ? DME_OK
           : host->cancelled && host->cancelled(host->user) ? DME_CANCELLED
                                                            : DME_FAILED;
}

/* Main thread: hands the detected checkout to the exporter and detaches the export job. */
static int32_t DME_CALL finish(void *job, char *error, uint64_t cap) {
    Job *b = job;
    b->export_job = NULL;

    if (!b->adapter->finish) {
        return DME_OK;
    }

    DmeHistoryResult result = {.size = sizeof(result),
                               .target_count = (uint32_t)b->targets.count,
                               .checkout = string(b->checkout),
                               .identity = string(b->identity),
                               .target = &b->target,
                               .targets = b->targets.items};
    return b->adapter->finish(b->instance, &result, error, cap);
}

static int32_t DME_CALL describe(const void *job, DmeRepositoryView *view) {
    const Job *b = job;
    *view = (DmeRepositoryView){.size = sizeof(*view),
                                .selected_scope = b->scope,
                                .scope_count = (uint32_t)b->scopes.count,
                                .scopes = b->scope_views,
                                .row_count = (uint32_t)b->modifications.count,
                                .rows = b->row_views,
                                .target_count = (uint32_t)b->targets.count,
                                .targets = b->targets.items,
                                .notice = string(b->notice)};
    return DME_OK;
}

static const DmeHistoryRuntime runtime = {.size = sizeof(DmeHistoryRuntime),
                                          .version = DME_REPOSITORY_API,
                                          .create_job = create_job,
                                          .prepare = prepare,
                                          .run = run,
                                          .finish = finish,
                                          .describe = describe,
                                          .destroy_job = destroy_job};

DES_API const DmeHistoryRuntime *DME_CALL duckmidi_history_runtime(uint32_t version) {
    return version == DME_REPOSITORY_API ? &runtime : NULL;
}
