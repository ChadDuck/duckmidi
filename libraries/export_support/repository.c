#include "internal.h"
#include "../../src/text/en.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Repository history: one SQLite ledger per plugins folder, shared by every exporter.

   A scope is one (exporter, target, checkout, version). Each scope keeps one files row per
   managed path: its initialized (baseline) content and the content duckmidi expects there now
   (current), both as deduplicated content IDs, plus the directory-entry signature observed when
   that expectation was last confirmed. Modifications are logical intents with an
   adapter-specific recipe; the adapter renders baseline plus all current intents into the
   complete desired file set, so any intent can be removed and the rest re-composed.

   A pending transaction stores every before/after content before a checkout file changes, so a
   killed process is rolled back deterministically on the next open. */

#define ES_DATABASE_SCHEMA 2
#define ES_MAX_PATHS 20000
#define ES_MAX_RECIPES (256u * 1024u * 1024u)
#define ES_CACHE_BYTES (64u * 1024u * 1024u)

enum { OPERATION_APPLY = 1, OPERATION_INITIALIZE = 2 };

/* Keep the held history directory as the pathname anchor. The default Unix VFS resolves
   /proc/self/fd links in xFullPathname, losing that anchor before journal creation. */
static const char *history_vfs(void) {
#ifdef __linux__
    static sqlite3_vfs anchored;
    extern int es_history_full_path(sqlite3_vfs *, const char *, int, char *);

    if (!anchored.zName) {
        sqlite3_vfs *native = sqlite3_vfs_find(NULL);
        anchored = *native;
        anchored.zName = "duckmidi-anchored";
        anchored.xFullPathname = es_history_full_path;
        sqlite3_vfs_register(&anchored, 0);
    }

    return anchored.zName;
#else
    return NULL;
#endif
}

#ifdef __linux__
int es_history_full_path(sqlite3_vfs *vfs, const char *name, int capacity, char *out) {
    (void)vfs;

    if (strncmp(name, "/proc/self/fd/", 14) || strlen(name) >= (size_t)capacity) {
        return SQLITE_CANTOPEN;
    }

    memcpy(out, name, strlen(name) + 1);
    return SQLITE_OK;
}
#endif

/* One managed path of the open scope. Content IDs are zero for an absent file. */
typedef struct {
    char *path;
    sqlite3_int64 baseline, current;
    uint32_t baseline_mode, current_mode;
    uint8_t baseline_hash[32], current_hash[32];
    EsStat seen; /* Signature of the current content on disk; exists=false when unknown. */
} Entry;

typedef struct {
    uint8_t hash[32];
    EsFile file;
} Cached;

typedef struct {
    char *path;
    EsStat stat;
} Observation;

struct EsRepository {
    sqlite3 *db;
    EsRoot *root;
    void *writer;
    void *checkout_lock;
    sqlite3_int64 scope;
    const DmeHost *host;
    const DmeHistoryAdapter *adapter;
    Entry *entries;
    size_t entry_count, entry_capacity;
    EsMap entry_index;
    Cached *cache;
    size_t cache_count, cache_capacity, cache_bytes;
    /* Signatures gathered by the latest namespace scan, for drift checks. */
    Observation *observed;
    size_t observed_count, observed_capacity;
    EsMap observed_index;
    bool observing;
    bool untracked;      /* History off: installs write files directly. */
    bool keep_originals; /* History off: keep each changed file's original as .bkp. */
};

static const char schema[] =
    "CREATE TABLE content(id INTEGER PRIMARY KEY,hash BLOB NOT NULL UNIQUE CHECK(length(hash)=32),"
    "size INTEGER NOT NULL CHECK(size>=0),data BLOB NOT NULL);"
    "CREATE TABLE scopes(id INTEGER PRIMARY KEY,exporter TEXT NOT NULL,target TEXT NOT NULL,"
    "target_name TEXT NOT NULL,root TEXT NOT NULL,identity TEXT NOT NULL,version TEXT NOT NULL,"
    "capabilities INTEGER NOT NULL DEFAULT 0,generation INTEGER NOT NULL DEFAULT 0,"
    "UNIQUE(exporter,target,identity,version));"
    "CREATE INDEX scopes_identity ON scopes(identity);"
    "CREATE TABLE files(scope INTEGER NOT NULL REFERENCES scopes ON DELETE CASCADE,"
    "path TEXT NOT NULL,baseline INTEGER REFERENCES content,baseline_mode INTEGER NOT NULL,"
    "current INTEGER REFERENCES content,current_mode INTEGER NOT NULL,"
    "seen_size INTEGER NOT NULL DEFAULT -1,seen_mtime INTEGER NOT NULL DEFAULT 0,"
    "seen_mode INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(scope,path)) WITHOUT ROWID;"
    "CREATE TABLE modifications(id INTEGER PRIMARY KEY,"
    "scope INTEGER NOT NULL REFERENCES scopes ON DELETE CASCADE,logical_key TEXT NOT NULL,"
    "label TEXT NOT NULL,recipe INTEGER NOT NULL REFERENCES content,UNIQUE(scope,logical_key));"
    "CREATE TABLE modification_paths(modification INTEGER NOT NULL REFERENCES modifications "
    "ON DELETE CASCADE,path TEXT NOT NULL,PRIMARY KEY(modification,path)) WITHOUT ROWID;"
    "CREATE TABLE directories(scope INTEGER NOT NULL REFERENCES scopes ON DELETE CASCADE,"
    "path TEXT NOT NULL,PRIMARY KEY(scope,path)) WITHOUT ROWID;"
    "CREATE TABLE pending(id INTEGER PRIMARY KEY,"
    "scope INTEGER NOT NULL UNIQUE REFERENCES scopes ON DELETE CASCADE,kind INTEGER NOT NULL);"
    "CREATE TABLE pending_files(pending INTEGER NOT NULL REFERENCES pending ON DELETE CASCADE,"
    "ordinal INTEGER NOT NULL,path TEXT NOT NULL,before INTEGER REFERENCES content,"
    "before_mode INTEGER NOT NULL,after INTEGER REFERENCES content,after_mode INTEGER NOT NULL,"
    "PRIMARY KEY(pending,ordinal)) WITHOUT ROWID;"
    "CREATE TABLE pending_directories(pending INTEGER NOT NULL REFERENCES pending ON DELETE "
    "CASCADE,ordinal INTEGER NOT NULL,path TEXT NOT NULL,created INTEGER NOT NULL,"
    "PRIMARY KEY(pending,ordinal)) WITHOUT ROWID;"
    "PRAGMA user_version=2;";

/* ---- Small helpers ---------------------------------------------------------------------- */

static bool cancelled(const EsRepository *r) {
    return r->host && r->host->cancelled && r->host->cancelled(r->host->user);
}

static void progress(EsRepository *r, uint32_t percent) {
    if (r->host && r->host->progress) {
        r->host->progress(r->host->user, percent);
    }
}

static bool sql(EsRepository *r, const char *text, char *error, size_t cap) {
    char *message = NULL;
    int result = sqlite3_exec(r->db, text, NULL, NULL, &message);

    if (result == SQLITE_OK) {
        return true;
    }

    es_fail(error, cap, DM_TEXT_REPOSITORY_REPOSITORY_HISTORY_S,
            message ? message : sqlite3_errmsg(r->db));
    sqlite3_free(message);
    return false;
}

static bool prepare(EsRepository *r, const char *text, sqlite3_stmt **statement, char *error,
                    size_t cap) {
    return sqlite3_prepare_v2(r->db, text, -1, statement, NULL) == SQLITE_OK ||
           es_fail(error, cap, DM_TEXT_REPOSITORY_REPOSITORY_HISTORY_S, sqlite3_errmsg(r->db));
}

/* Steps a reusable statement to completion and resets it. */
static bool step(EsRepository *r, sqlite3_stmt *statement, char *error, size_t cap) {
    bool ok = sqlite3_step(statement) == SQLITE_DONE;

    if (!ok) {
        es_fail(error, cap, DM_TEXT_REPOSITORY_REPOSITORY_HISTORY_S, sqlite3_errmsg(r->db));
    }

    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return ok;
}

/* Runs a one-shot statement with scope (?1) and an optional second integer (?2). */
static bool run(EsRepository *r, const char *text, sqlite3_int64 second, char *error, size_t cap) {
    sqlite3_stmt *s = NULL;
    bool ok = prepare(r, text, &s, error, cap);

    if (ok) {
        sqlite3_bind_int64(s, 1, r->scope);

        if (sqlite3_bind_parameter_count(s) >= 2) {
            sqlite3_bind_int64(s, 2, second);
        }

        ok = step(r, s, error, cap);
    }

    sqlite3_finalize(s);
    return ok;
}

static void text(sqlite3_stmt *s, int index, const char *value) {
    sqlite3_bind_text(s, index, value, -1, SQLITE_TRANSIENT);
}

static void bind_content(sqlite3_stmt *s, int index, sqlite3_int64 id) {
    if (id) {
        sqlite3_bind_int64(s, index, id);
    } else {
        sqlite3_bind_null(s, index);
    }
}

static bool same_signature(const EsStat *a, const EsStat *b) {
    return a->exists && b->exists && a->size == b->size && a->mtime == b->mtime &&
           a->mode == b->mode;
}

static bool equal_file(const EsFile *a, const EsFile *b) {
    return a->exists == b->exists &&
           (!a->exists || (a->size == b->size && a->mode == b->mode &&
                           (!a->size || !memcmp(a->data, b->data, a->size))));
}

/* Every failed operation reports a reason; allocation and bound failures have no specific one. */
static bool described(char *error, size_t cap) {
    if (error && cap && !*error) {
        es_fail(error, cap, DM_TEXT_REPOSITORY_OPERATION_FAILED);
    }

    return false;
}

static bool backup_path(const char *path, char sidecar[DME_PATH_LIMIT]) {
    return snprintf(sidecar, DME_PATH_LIMIT, "%s.bkp", path) < DME_PATH_LIMIT;
}

static bool is_backup(const char *path) {
    size_t length = strlen(path);
    return length > 4 && !strcmp(path + length - 4, ".bkp");
}

static bool allowed(const EsRepository *r, const char *path) {
    return es_relative(path) && (!r->adapter->allow || r->adapter->allow(path));
}

/* Adapters without an inventory capture each path's baseline when it is first written. */
static bool lazy(const EsRepository *r) {
    return !r->adapter->inventory;
}

static bool run_inventory(EsRepository *r, EsPaths *paths, char *error, size_t cap) {
    return r->adapter->inventory(es_history_api(), (DmeHistory *)r, (DmeHistoryList *)paths, error,
                                 cap) != 0;
}

bool es_render(EsRepository *r, const EsModifications *mods, EsPlan *plan, char *error,
               size_t cap) {
    DmeHistoryIntent *intents = calloc(mods->count + 1, sizeof(*intents));

    for (size_t i = 0; intents && i < mods->count; i++) {
        const EsModification *m = &mods->items[i];
        intents[i] = (DmeHistoryIntent){
            {m->key, strlen(m->key)}, {m->label, strlen(m->label)}, m->data, m->size};
    }

    bool ok = intents &&
              r->adapter->render(es_history_api(), (DmeHistory *)r, intents, (uint32_t)mods->count,
                                 (DmeHistoryPlan *)plan, error, cap) != 0;
    free(intents);
    return ok;
}

/* ---- Paths and plans -------------------------------------------------------------------- */

void es_file_free(EsFile *file) {
    free(file->data);
    *file = (EsFile){0};
}

bool es_paths_has(const EsPaths *paths, const char *path) {
    return es_map_get(&paths->index, path, NULL);
}

/* Adds any string once; es_paths_add also requires a safe relative path. */
bool es_strings_add(EsPaths *paths, const char *path) {
    if (es_paths_has(paths, path)) {
        return true;
    }

    if (paths->count >= ES_MAX_PATHS ||
        !es_reserve((void **)&paths->items, &paths->capacity, paths->count + 1, sizeof(char *))) {
        return false;
    }

    char *copy = SDL_strdup(path);

    if (!copy || !es_map_put(&paths->index, copy, paths->count)) {
        SDL_free(copy);
        return false;
    }

    paths->items[paths->count++] = copy;
    return true;
}

bool es_paths_add(EsPaths *paths, const char *path) {
    return es_relative(path) && es_strings_add(paths, path);
}

void es_paths_free(EsPaths *paths) {
    for (size_t i = 0; i < paths->count; ++i) {
        SDL_free(paths->items[i]);
    }

    free(paths->items);
    es_map_free(&paths->index);
    *paths = (EsPaths){0};
}

const EsFile *es_plan_find(const EsPlan *plan, const char *path) {
    size_t index;
    return es_map_get(&plan->index, path, &index) ? &plan->writes[index].file : NULL;
}

bool es_plan_put(EsPlan *plan, const char *path, const void *bytes, size_t size, uint32_t mode,
                 bool exists, char *error, size_t cap) {
    if (!es_relative(path) || size > ES_MAX_RECIPES || (size && !bytes) || (!exists && size)) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_REPOSITORY_WRITE_S, path ? path : "");
    }

    size_t index;
    EsWrite *write = NULL;

    if (es_map_get(&plan->index, path, &index)) {
        write = &plan->writes[index];
    } else {
        if (plan->count >= ES_MAX_PATHS || !es_reserve((void **)&plan->writes, &plan->capacity,
                                                       plan->count + 1, sizeof(EsWrite))) {
            return es_fail(error, cap,
                           DM_TEXT_REPOSITORY_REPOSITORY_WRITE_PLAN_EXCEEDS_ITS_MEMORY_LIMIT);
        }

        char *copy = SDL_strdup(path);

        if (!copy || !es_map_put(&plan->index, copy, plan->count)) {
            SDL_free(copy);
            return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_REPOSITORY_PATH);
        }

        write = &plan->writes[plan->count++];
        *write = (EsWrite){.path = copy};
    }

    uint8_t *copy = exists ? malloc(size + 1) : NULL;

    if (exists && !copy) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_REPOSITORY_OUTPUT_S, path);
    }

    if (size) {
        memcpy(copy, bytes, size);
    }

    if (copy) {
        copy[size] = 0;
    }

    es_file_free(&write->file);
    write->file = (EsFile){copy, size, mode, exists};
    return true;
}

bool es_plan_backup(EsPlan *plan, const char *path) {
    return es_paths_add(&plan->backups, path);
}

bool es_plan_affect(EsPlan *plan, const char *key, const char *path, const char *selector) {
    if (!key || !es_relative(path) || !selector || plan->effect_count >= 640000 ||
        !es_reserve((void **)&plan->effects, &plan->effect_capacity, plan->effect_count + 1,
                    sizeof(*plan->effects))) {
        return false;
    }

    EsEffect effect = {SDL_strdup(key), SDL_strdup(path), SDL_strdup(selector)};

    if (!effect.key || !effect.path || !effect.selector) {
        SDL_free(effect.key);
        SDL_free(effect.path);
        SDL_free(effect.selector);
        return false;
    }

    plan->effects[plan->effect_count++] = effect;
    return true;
}

bool es_plan_depend(EsPlan *plan, const char *consumer, const char *provider, const char *role) {
    if (!es_relative(consumer) || !es_relative(provider) || !role ||
        plan->dependency_count >= 640000 ||
        !es_reserve((void **)&plan->dependencies, &plan->dependency_capacity,
                    plan->dependency_count + 1, sizeof(*plan->dependencies))) {
        return false;
    }

    EsDependency dependency = {SDL_strdup(consumer), SDL_strdup(provider), SDL_strdup(role)};

    if (!dependency.consumer || !dependency.provider || !dependency.role) {
        SDL_free(dependency.consumer);
        SDL_free(dependency.provider);
        SDL_free(dependency.role);
        return false;
    }

    plan->dependencies[plan->dependency_count++] = dependency;
    return true;
}

void es_plan_free(EsPlan *plan) {
    for (size_t i = 0; i < plan->count; ++i) {
        SDL_free(plan->writes[i].path);
        es_file_free(&plan->writes[i].file);
    }

    es_paths_free(&plan->backups);

    for (size_t i = 0; i < plan->effect_count; ++i) {
        SDL_free(plan->effects[i].key);
        SDL_free(plan->effects[i].path);
        SDL_free(plan->effects[i].selector);
    }

    for (size_t i = 0; i < plan->dependency_count; ++i) {
        SDL_free(plan->dependencies[i].consumer);
        SDL_free(plan->dependencies[i].provider);
        SDL_free(plan->dependencies[i].role);
    }

    free(plan->effects);
    free(plan->dependencies);
    free(plan->writes);
    es_map_free(&plan->index);
    *plan = (EsPlan){0};
}

/* ---- Content store ---------------------------------------------------------------------- */

/* Stores bytes once per hash. hash may be NULL to compute it here. */
static bool put_content(EsRepository *r, const EsFile *file, const uint8_t *known,
                        sqlite3_int64 *id, char *error, size_t cap) {
    *id = 0;

    if (!file->exists) {
        return true;
    }

    uint8_t hash[32];

    if (known) {
        memcpy(hash, known, 32);
    } else {
        es_sha256(file->data, file->size, hash);
    }

    sqlite3_stmt *s = NULL;

    if (!prepare(r, "SELECT id,size FROM content WHERE hash=?", &s, error, cap)) {
        return false;
    }

    sqlite3_bind_blob(s, 1, hash, 32, SQLITE_TRANSIENT);
    int result = sqlite3_step(s);

    if (result == SQLITE_ROW) {
        *id = sqlite3_column_int64(s, 0);
        bool ok = (uint64_t)sqlite3_column_int64(s, 1) == file->size;
        sqlite3_finalize(s);
        return ok || es_fail(error, cap, DM_TEXT_REPOSITORY_REPOSITORY_CONTENT_HASH_SIZE_CONFLICT);
    }

    sqlite3_finalize(s);
    s = NULL;

    if (result != SQLITE_DONE ||
        !prepare(r, "INSERT INTO content(hash,size,data) VALUES(?,?,?)", &s, error, cap)) {
        return false;
    }

    sqlite3_bind_blob(s, 1, hash, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)file->size);
    sqlite3_bind_blob64(s, 3, file->size ? file->data : (const void *)"", file->size,
                        SQLITE_STATIC);
    bool ok = step(r, s, error, cap);
    sqlite3_finalize(s);
    *id = ok ? sqlite3_last_insert_rowid(r->db) : 0;
    return ok || es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_RETAIN_REPOSITORY_RECOVERY_CONTENT_S,
                         sqlite3_errmsg(r->db));
}

static bool copy_file(const EsFile *source, uint32_t mode, EsFile *out) {
    *out = (EsFile){.mode = mode, .exists = source->exists, .size = source->size};

    if (!source->exists) {
        return true;
    }

    out->data = malloc(source->size + 1);

    if (!out->data) {
        return false;
    }

    memcpy(out->data, source->data, source->size);
    out->data[source->size] = 0;
    return true;
}

/* Cached content is keyed by hash, which identifies its bytes permanently; row IDs can be
   reused after garbage collection or a rolled-back insert. */
static void cache_store(EsRepository *r, const uint8_t hash[32], const EsFile *file) {
    if (file->size > ES_CACHE_BYTES / 4) {
        return;
    }

    while (r->cache_count && r->cache_bytes + file->size > ES_CACHE_BYTES) {
        r->cache_bytes -= r->cache[0].file.size;
        es_file_free(&r->cache[0].file);
        memmove(r->cache, r->cache + 1, --r->cache_count * sizeof(*r->cache));
    }

    Cached entry = {0};
    memcpy(entry.hash, hash, 32);

    if (copy_file(file, 0, &entry.file) &&
        es_reserve((void **)&r->cache, &r->cache_capacity, r->cache_count + 1, sizeof(*r->cache))) {
        r->cache[r->cache_count++] = entry;
        r->cache_bytes += file->size;
    } else {
        es_file_free(&entry.file);
    }
}

/* Reads stored content. Its hash is verified when it is first read through this handle. */
static bool get_content(EsRepository *r, sqlite3_int64 id, uint32_t mode, EsFile *out, char *error,
                        size_t cap) {
    *out = (EsFile){.mode = mode};

    if (!id) {
        return true;
    }

    sqlite3_stmt *s = NULL;

    if (!prepare(r, "SELECT size,hash FROM content WHERE id=?", &s, error, cap)) {
        return false;
    }

    sqlite3_bind_int64(s, 1, id);
    uint8_t expected[32];
    bool ok = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_bytes(s, 1) == 32;
    sqlite3_int64 size = ok ? sqlite3_column_int64(s, 0) : -1;

    if (ok) {
        memcpy(expected, sqlite3_column_blob(s, 1), 32);
    }

    sqlite3_finalize(s);
    s = NULL;
    ok = ok && size >= 0 && size <= ES_MAX_RECIPES;

    for (size_t i = r->cache_count; ok && i > 0; --i) {
        if (!memcmp(r->cache[i - 1].hash, expected, 32)) {
            return copy_file(&r->cache[i - 1].file, mode, out) ||
                   es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_REPOSITORY_OUTPUT_S, "");
        }
    }

    ok = ok && prepare(r, "SELECT data FROM content WHERE id=?", &s, error, cap);

    if (ok) {
        sqlite3_bind_int64(s, 1, id);
        ok = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_bytes(s, 0) == size &&
             (out->data = malloc((size_t)size + 1)) != NULL;
    }

    if (ok) {
        uint8_t actual[32];

        if (size) {
            memcpy(out->data, sqlite3_column_blob(s, 0), (size_t)size);
        }

        es_sha256(out->data, (size_t)size, actual);
        ok = !memcmp(actual, expected, 32);
    }

    sqlite3_finalize(s);

    if (!ok) {
        es_file_free(out);
        return es_fail(error, cap,
                       DM_TEXT_REPOSITORY_REPOSITORY_HISTORY_CONTENT_IS_UNREADABLE_OR_DAMAGED);
    }

    out->size = (size_t)size;
    out->data[out->size] = 0;
    out->exists = true;
    cache_store(r, expected, out);
    return true;
}

/* ---- Scope index ------------------------------------------------------------------------ */

static void clear_index(EsRepository *r) {
    for (size_t i = 0; i < r->entry_count; ++i) {
        SDL_free(r->entries[i].path);
    }

    free(r->entries);
    es_map_free(&r->entry_index);
    r->entries = NULL;
    r->entry_count = r->entry_capacity = 0;
}

static Entry *entry(const EsRepository *r, const char *path) {
    size_t index;
    return es_map_get(&r->entry_index, path, &index) ? &r->entries[index] : NULL;
}

static void copy_hash(uint8_t out[32], sqlite3_stmt *s, int column) {
    if (sqlite3_column_bytes(s, column) == 32) {
        memcpy(out, sqlite3_column_blob(s, column), 32);
    } else {
        memset(out, 0, 32);
    }
}

/* Loads the scope's managed paths without content, so lookups never touch blobs. */
static bool load_index(EsRepository *r, char *error, size_t cap) {
    clear_index(r);
    sqlite3_stmt *s = NULL;

    if (!prepare(r,
                 "SELECT f.path,f.baseline,f.baseline_mode,b.hash,f.current,f.current_mode,c.hash,"
                 "f.seen_size,f.seen_mtime,f.seen_mode FROM files f "
                 "LEFT JOIN content b ON b.id=f.baseline LEFT JOIN content c ON c.id=f.current "
                 "WHERE f.scope=?",
                 &s, error, cap)) {
        return false;
    }

    sqlite3_bind_int64(s, 1, r->scope);
    bool ok = true;
    int result;

    while (ok && (result = sqlite3_step(s)) == SQLITE_ROW) {
        ok = es_reserve((void **)&r->entries, &r->entry_capacity, r->entry_count + 1,
                        sizeof(*r->entries));

        if (!ok) {
            break;
        }

        Entry *e = &r->entries[r->entry_count];
        *e = (Entry){.path = SDL_strdup((const char *)sqlite3_column_text(s, 0)),
                     .baseline = sqlite3_column_int64(s, 1),
                     .baseline_mode = (uint32_t)sqlite3_column_int(s, 2),
                     .current = sqlite3_column_int64(s, 4),
                     .current_mode = (uint32_t)sqlite3_column_int(s, 5)};
        copy_hash(e->baseline_hash, s, 3);
        copy_hash(e->current_hash, s, 6);
        sqlite3_int64 size = sqlite3_column_int64(s, 7);
        e->seen = (EsStat){.exists = size >= 0,
                           .size = size,
                           .mtime = sqlite3_column_int64(s, 8),
                           .mode = (uint32_t)sqlite3_column_int(s, 9)};
        ok = e->path && es_map_put(&r->entry_index, e->path, r->entry_count);

        if (ok) {
            r->entry_count++;
        } else {
            SDL_free(e->path);
        }
    }

    ok = ok && result == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok || es_fail(error, cap, DM_TEXT_REPOSITORY_REPOSITORY_HISTORY_S, "index");
}

static bool modified(const Entry *e) {
    return e->current != e->baseline || (e->current && e->current_mode != e->baseline_mode);
}

/* ---- Observed signatures ---------------------------------------------------------------- */

static void clear_observed(EsRepository *r) {
    for (size_t i = 0; i < r->observed_count; ++i) {
        SDL_free(r->observed[i].path);
    }

    free(r->observed);
    es_map_free(&r->observed_index);
    r->observed = NULL;
    r->observed_count = r->observed_capacity = 0;
}

static bool observe(EsRepository *r, const char *path, const EsStat *info) {
    if (!r->observing || es_map_get(&r->observed_index, path, NULL)) {
        return true;
    }

    if (!es_reserve((void **)&r->observed, &r->observed_capacity, r->observed_count + 1,
                    sizeof(*r->observed))) {
        return false;
    }

    char *copy = SDL_strdup(path);

    if (!copy || !es_map_put(&r->observed_index, copy, r->observed_count)) {
        SDL_free(copy);
        return false;
    }

    r->observed[r->observed_count++] = (Observation){copy, *info};
    return true;
}

typedef struct {
    EsRepository *repo;
    EsPaths *paths;
} Collect;

static bool collect(void *user, const char *path, const EsStat *info) {
    Collect *c = user;
    return observe(c->repo, path, info) && es_paths_add(c->paths, path);
}

/* The latest signature of path: from the current scan when it saw it, otherwise a stat. */
static bool signature(EsRepository *r, const char *path, EsStat *out, char *error, size_t cap) {
    size_t index;

    if (es_map_get(&r->observed_index, path, &index)) {
        *out = r->observed[index].stat;
        return true;
    }

    return es_fs_stat(r->root, path, out, error, cap);
}

/* ---- Read API --------------------------------------------------------------------------- */

static bool read_baseline(EsRepository *r, const char *path, EsFile *out, char *error, size_t cap) {
    if (!r->db) {
        return es_fs_read(r->root, path, out, error, cap);
    }

    const Entry *e = entry(r, path);

    if (e) {
        return get_content(r, e->baseline, e->baseline_mode, out, error, cap);
    }

    if (lazy(r)) {
        return es_fs_read(r->root, path, out, error, cap);
    }

    *out = (EsFile){0};
    return true;
}

bool es_repository_baseline(EsRepository *r, const char *path, EsFile *out, char *error,
                            size_t cap) {
    if (!allowed(r, path)) {
        *out = (EsFile){0};
        return es_fail(error, cap,
                       DM_TEXT_REPOSITORY_PATH_IS_OUTSIDE_THE_INITIALIZED_AUDIO_NAMESPACE_S, path);
    }

    return read_baseline(r, path, out, error, cap);
}

bool es_plan_exists(EsRepository *r, const EsPlan *plan, const char *path, bool *exists,
                    char *error, size_t cap) {
    const EsFile *file = plan ? es_plan_find(plan, path) : NULL;

    if (file) {
        *exists = file->exists;
        return true;
    }

    return es_repository_baseline_exists(r, path, exists, error, cap);
}

bool es_repository_baseline_exists(EsRepository *r, const char *path, bool *exists, char *error,
                                   size_t cap) {
    *exists = false;

    if (!allowed(r, path)) {
        return es_fail(error, cap,
                       DM_TEXT_REPOSITORY_PATH_IS_OUTSIDE_THE_INITIALIZED_AUDIO_NAMESPACE_S, path);
    }

    const Entry *e = r->db ? entry(r, path) : NULL;

    if (e) {
        *exists = e->baseline != 0;
        return true;
    }

    if (!r->db || lazy(r)) {
        EsStat info;
        bool ok = es_fs_stat(r->root, path, &info, error, cap);
        *exists = info.exists && !info.directory;
        return ok;
    }

    return true;
}

/* Whether path's baseline equals desired, comparing hashes instead of stored bytes. */
static bool same_as_baseline(EsRepository *r, const char *path, const EsFile *desired, bool *same,
                             char *error, size_t cap) {
    const Entry *e = entry(r, path);

    if (e || !lazy(r)) {
        bool exists = e && e->baseline;
        *same = exists == desired->exists;

        if (*same && exists) {
            uint8_t hash[32];
            es_sha256(desired->data, desired->size, hash);
            *same = !memcmp(hash, e->baseline_hash, 32) &&
                    (!desired->mode || desired->mode == e->baseline_mode);
        }

        return true;
    }

    EsFile original = {0};
    bool ok = es_fs_read(r->root, path, &original, error, cap);
    *same =
        ok && original.exists == desired->exists &&
        (!desired->exists ||
         (original.size == desired->size && (!desired->mode || desired->mode == original.mode) &&
          (!desired->size || !memcmp(original.data, desired->data, desired->size))));
    es_file_free(&original);
    return ok;
}

bool es_plan_keep_originals(EsRepository *repo, EsPlan *plan, const char *key, char *error,
                            size_t cap) {
    for (size_t i = 0; i < plan->effect_count; i++) {
        const EsEffect *effect = &plan->effects[i];
        const EsFile *desired = es_plan_find(plan, effect->path);
        bool exists = false, same = true;

        if (strcmp(effect->key, key) || !desired) {
            continue;
        }

        if (!es_repository_baseline_exists(repo, effect->path, &exists, error, cap) ||
            (exists && !same_as_baseline(repo, effect->path, desired, &same, error, cap))) {
            return false;
        }

        if (exists && !same && !es_plan_backup(plan, effect->path)) {
            return false;
        }
    }

    return true;
}

bool es_repository_read(EsRepository *r, const char *path, EsFile *out, char *error, size_t cap) {
    return es_fs_read(r->root, path, out, error, cap);
}

const char *es_repository_identity(const EsRepository *r) {
    return es_root_identity(r->root);
}

bool es_repository_paths(EsRepository *r, const char *path, bool recursive, EsPaths *out,
                         char *error, size_t cap) {
    Collect c = {r, out};
    return es_fs_scan(r->root, path, recursive, collect, &c, error, cap);
}

uint64_t es_repository_scope_id(const EsRepository *r) {
    return (uint64_t)r->scope;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool es_repository_baseline_paths(EsRepository *r, EsPaths *out, char *error, size_t cap) {
    if (!r->db) {
        return !lazy(r) && run_inventory(r, out, error, cap);
    }

    const char **names = malloc((r->entry_count + 1) * sizeof(*names));
    size_t count = 0;

    if (!names) {
        return false;
    }

    for (size_t i = 0; i < r->entry_count; ++i) {
        if (r->entries[i].baseline) {
            names[count++] = r->entries[i].path;
        }
    }

    qsort(names, count, sizeof(*names), compare_names);
    bool ok = true;

    for (size_t i = 0; ok && i < count; ++i) {
        ok = es_paths_add(out, names[i]);
    }

    free(names);
    return ok;
}

bool es_repository_modifications(EsRepository *r, EsModifications *out, char *error, size_t cap) {
    *out = (EsModifications){0};
    sqlite3_stmt *s = NULL;

    if (!prepare(r,
                 "SELECT m.id,m.logical_key,m.label,m.recipe,p.path FROM modifications m "
                 "LEFT JOIN modification_paths p ON p.modification=m.id WHERE m.scope=? "
                 "ORDER BY m.id,p.path",
                 &s, error, cap)) {
        return false;
    }

    sqlite3_bind_int64(s, 1, r->scope);
    size_t capacity = 0, bytes = 0;
    bool ok = true;
    int result;

    while (ok && (result = sqlite3_step(s)) == SQLITE_ROW) {
        uint64_t id = (uint64_t)sqlite3_column_int64(s, 0);
        EsModification *m = out->count ? &out->items[out->count - 1] : NULL;

        if (!m || m->order != id) {
            ok = out->count < ES_MAX_PATHS &&
                 es_reserve((void **)&out->items, &capacity, out->count + 1, sizeof(*out->items));

            if (!ok) {
                break;
            }

            m = &out->items[out->count++];
            *m = (EsModification){.order = id,
                                  .key = SDL_strdup((const char *)sqlite3_column_text(s, 1)),
                                  .label = SDL_strdup((const char *)sqlite3_column_text(s, 2))};
            EsFile data;
            ok = m->key && m->label &&
                 get_content(r, sqlite3_column_int64(s, 3), 0, &data, error, cap);

            if (ok) {
                m->data = data.data;
                m->size = data.size;
                ok = m->size <= ES_MAX_RECIPES - bytes;
                bytes += m->size;
            }
        }

        if (ok && sqlite3_column_type(s, 4) != SQLITE_NULL) {
            ok = es_paths_add(&m->paths, (const char *)sqlite3_column_text(s, 4));
        }
    }

    ok = ok && result == SQLITE_DONE;
    sqlite3_finalize(s);

    if (!ok) {
        es_modifications_free(out);
        return es_fail(error, cap,
                       DM_TEXT_REPOSITORY_CANNOT_READ_BOUNDED_CURRENT_REPOSITORY_MODIFICATIONS);
    }

    return true;
}

void es_modifications_free(EsModifications *mods) {
    for (size_t i = 0; i < mods->count; ++i) {
        SDL_free(mods->items[i].key);
        SDL_free(mods->items[i].label);
        free(mods->items[i].data);
        es_paths_free(&mods->items[i].paths);
    }

    free(mods->items);
    *mods = (EsModifications){0};
}

/* ---- Drift ------------------------------------------------------------------------------ */

static bool record_signature(EsRepository *r, sqlite3_stmt *s, Entry *e, const EsStat *info,
                             char *error, size_t cap) {
    e->seen = *info;
    sqlite3_bind_int64(s, 1, info->exists ? info->size : -1);
    sqlite3_bind_int64(s, 2, info->mtime);
    sqlite3_bind_int(s, 3, (int)info->mode);
    sqlite3_bind_int64(s, 4, r->scope);
    text(s, 5, e->path);
    return step(r, s, error, cap);
}

/* Compares the checkout with the expected state. Unchanged signatures need no reads; a changed
   signature is confirmed by hashing, and an identical file just refreshes its signature. */
static bool check_drift(EsRepository *r, char *error, size_t cap) {
    EsPaths inventory = {0};
    clear_observed(r);
    r->observing = true;
    bool ok = lazy(r) || run_inventory(r, &inventory, error, cap);
    r->observing = false;
    sqlite3_stmt *s = NULL;
    bool open = false;

    for (size_t i = 0; ok && i < r->entry_count; ++i) {
        Entry *e = &r->entries[i];
        EsStat info;
        ok = !cancelled(r) && signature(r, e->path, &info, error, cap);

        if (!ok) {
            break;
        }

        bool expected = e->current != 0, actual = info.exists && !info.directory;

        if (expected != actual) {
            ok = es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_REPOSITORY_CHANGED_OUTSIDE_DUCKMIDI_S_RESOLVE_THIS_PATH_OR,
                e->path);
        } else if (expected && !same_signature(&info, &e->seen)) {
            EsFile file = {0};
            uint8_t hash[32];
            bool same = false;
            ok = es_fs_read(r->root, e->path, &file, error, cap);

            if (ok && file.exists) {
                es_sha256(file.data, file.size, hash);
                same = file.mode == e->current_mode && !memcmp(hash, e->current_hash, 32);
            }

            es_file_free(&file);

            if (ok && !same) {
                ok = es_fail(
                    error, cap,
                    DM_TEXT_REPOSITORY_REPOSITORY_CHANGED_OUTSIDE_DUCKMIDI_S_RESOLVE_THIS_PATH_OR,
                    e->path);
            }

            if (ok && !open) {
                open = sql(r, "BEGIN IMMEDIATE", error, cap);
                ok = open && prepare(r,
                                     "UPDATE files SET seen_size=?1,seen_mtime=?2,seen_mode=?3 "
                                     "WHERE scope=?4 AND path=?5",
                                     &s, error, cap);
            }

            ok = ok && record_signature(r, s, e, &info, error, cap);
        }
    }

    /* A recognized file the scope does not know about appeared after initialization. */
    for (size_t i = 0; ok && i < inventory.count; ++i) {
        EsStat info;

        if (entry(r, inventory.items[i])) {
            continue;
        }

        ok = signature(r, inventory.items[i], &info, error, cap);

        if (ok && info.exists) {
            ok = es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_REPOSITORY_CHANGED_OUTSIDE_DUCKMIDI_S_RESOLVE_THIS_PATH_OR,
                inventory.items[i]);
        }
    }

    sqlite3_finalize(s);

    if (open) {
        ok = sql(r, ok ? "COMMIT" : "ROLLBACK", ok ? error : NULL, ok ? cap : 0) && ok;
    }

    es_paths_free(&inventory);
    return ok;
}

/* ---- Recovery --------------------------------------------------------------------------- */

static bool pending_plan(EsRepository *r, sqlite3_int64 tx, bool after, EsPlan *out, char *error,
                         size_t cap) {
    sqlite3_stmt *s = NULL;
    const char *query =
        after
            ? "SELECT path,after,after_mode FROM pending_files WHERE pending=? ORDER BY ordinal"
            : "SELECT path,before,before_mode FROM pending_files WHERE pending=? ORDER BY ordinal";

    if (!prepare(r, query, &s, error, cap)) {
        return false;
    }

    sqlite3_bind_int64(s, 1, tx);
    bool ok = true;
    int result;

    while (ok && (result = sqlite3_step(s)) == SQLITE_ROW) {
        EsFile file = {0};
        ok = get_content(r, sqlite3_column_int64(s, 1), (uint32_t)sqlite3_column_int(s, 2), &file,
                         error, cap) &&
             es_plan_put(out, (const char *)sqlite3_column_text(s, 0), file.data, file.size,
                         file.mode, file.exists, error, cap);
        es_file_free(&file);
    }

    sqlite3_finalize(s);
    return ok && result == SQLITE_DONE;
}

static bool pending_directories(EsRepository *r, sqlite3_int64 tx, int created,
                                bool (*action)(EsRoot *, const char *, char *, size_t), char *error,
                                size_t cap) {
    sqlite3_stmt *s = NULL;

    if (!prepare(r,
                 "SELECT path FROM pending_directories WHERE pending=? AND created=? "
                 "ORDER BY ordinal DESC",
                 &s, error, cap)) {
        return false;
    }

    sqlite3_bind_int64(s, 1, tx);
    sqlite3_bind_int(s, 2, created);
    bool ok = true;
    int result;

    while (ok && (result = sqlite3_step(s)) == SQLITE_ROW) {
        ok = action(r->root, (const char *)sqlite3_column_text(s, 0), error, cap);
    }

    sqlite3_finalize(s);
    return ok && result == SQLITE_DONE;
}

static bool ensure_directory(EsRoot *root, const char *path, char *error, size_t cap) {
    bool exists = false, directory = false;
    return es_fs_type(root, path, &exists, &directory, error, cap) &&
           (exists ? directory : es_fs_mkdir(root, path, error, cap));
}

/* Rolls an interrupted operation back to its recorded before-state. Every path is validated
   first; an outside edit retains every recovery byte and blocks writes for this scope. */
static bool recover(EsRepository *r, char *error, size_t cap) {
    sqlite3_stmt *s = NULL;

    if (!prepare(r, "SELECT id FROM pending WHERE scope=?", &s, error, cap)) {
        return false;
    }

    sqlite3_bind_int64(s, 1, r->scope);
    int result = sqlite3_step(s);
    sqlite3_int64 tx = result == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
    sqlite3_finalize(s);

    if (result == SQLITE_DONE) {
        return true;
    }

    if (!tx) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_INSPECT_PENDING_REPOSITORY_RECOVERY);
    }

    EsPlan before = {0}, after = {0};
    bool ok = pending_plan(r, tx, false, &before, error, cap) &&
              pending_plan(r, tx, true, &after, error, cap) && before.count == after.count;

    for (size_t i = 0; ok && i < before.count; ++i) {
        EsFile actual = {0};
        ok = es_fs_read(r->root, before.writes[i].path, &actual, error, cap);

        if (ok && !equal_file(&actual, &before.writes[i].file) &&
            !equal_file(&actual, &after.writes[i].file)) {
            ok = es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_RECOVERY_CONFLICT_AT_S_PENDING_ORIGINALS_REMAIN_IN_REPOSITORY,
                before.writes[i].path);
        }

        es_file_free(&actual);
    }

    ok = ok && pending_directories(r, tx, -1, ensure_directory, error, cap);

    for (size_t i = before.count; ok && i > 0; --i) {
        EsWrite *write = &before.writes[i - 1];
        EsFile actual = {0};
        ok = es_fs_read(r->root, write->path, &actual, error, cap) &&
             es_fs_stage_remove(r->root, write->path, (uint64_t)tx, i - 1,
                                &after.writes[i - 1].file, &write->file, error, cap);

        if (ok && !equal_file(&actual, &write->file)) {
            ok = es_fs_write(r->root, write->path, &write->file, (uint64_t)tx, i - 1, error, cap);
        }

        es_file_free(&actual);
    }

    ok = ok && pending_directories(r, tx, 1, es_fs_rmdir, error, cap) &&
         run(r, "DELETE FROM pending WHERE scope=?1", 0, error, cap);
    es_plan_free(&before);
    es_plan_free(&after);
    return ok;
}

/* ---- Planning checks -------------------------------------------------------------------- */

static size_t name_index(char **names, size_t count, const char *name) {
    size_t first = 0;

    while (first < count) {
        size_t middle = first + (count - first) / 2;

        if (strcmp(names[middle], name) < 0) {
            first = middle + 1;
        } else {
            count = middle;
        }
    }

    return first;
}

/* Every declared dependency must exist in the desired state, and the graph must be acyclic.
   Every affected path must belong to a current intent. */
static bool validate_graph(EsRepository *r, const EsPlan *plan, const EsModifications *mods,
                           char *error, size_t cap) {
    size_t edges = plan->dependency_count;

    if (edges > ES_MAX_PATHS * 32u) {
        return es_fail(error, cap,
                       DM_TEXT_REPOSITORY_THE_NATIVE_DEPENDENCY_GRAPH_EXCEEDS_ITS_BOUNDED_SIZE);
    }

    char **names = malloc((edges * 2 + 1) * sizeof(*names));
    size_t count = 0;
    bool ok = names != NULL;

    for (size_t i = 0; ok && i < edges; i++) {
        names[count++] = plan->dependencies[i].consumer;
        names[count++] = plan->dependencies[i].provider;
    }

    if (ok) {
        qsort(names, count, sizeof(*names), compare_names);
        size_t unique = 0;

        for (size_t i = 0; i < count; i++) {
            if (!unique || strcmp(names[unique - 1], names[i])) {
                names[unique++] = names[i];
            }
        }

        count = unique;
    }

    size_t *head = malloc((count + 1) * sizeof(*head));
    size_t *degree = calloc(count + 1, sizeof(*degree));
    size_t *next = malloc((edges + 1) * sizeof(*next));
    size_t *destination = malloc((edges + 1) * sizeof(*destination));
    size_t *queue = malloc((count + 1) * sizeof(*queue));
    ok = ok && head && degree && next && destination && queue;

    for (size_t i = 0; ok && i < count; i++) {
        head[i] = SIZE_MAX;
        const EsFile *desired = es_plan_find(plan, names[i]);
        bool exists = desired && desired->exists;

        if (!desired) {
            ok = es_repository_baseline_exists(r, names[i], &exists, error, cap);
        }

        if (ok && !exists) {
            ok = es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_NATIVE_DEPENDENCY_IS_MISSING_OR_INSPECTION_WAS_CANCELED_S,
                names[i]);
        }
    }

    for (size_t i = 0; ok && i < edges; i++) {
        size_t consumer = name_index(names, count, plan->dependencies[i].consumer);
        size_t provider = name_index(names, count, plan->dependencies[i].provider);
        destination[i] = consumer;
        next[i] = head[provider];
        head[provider] = i;
        degree[consumer]++;
    }

    size_t queued = 0, consumed = 0;

    for (size_t i = 0; ok && i < count; i++) {
        if (!degree[i]) {
            queue[queued++] = i;
        }
    }

    while (ok && consumed < queued) {
        size_t provider = queue[consumed++];

        for (size_t e = head[provider]; e != SIZE_MAX; e = next[e]) {
            if (!--degree[destination[e]]) {
                queue[queued++] = destination[e];
            }
        }
    }

    if (ok && consumed != count) {
        ok = es_fail(error, cap,
                     DM_TEXT_REPOSITORY_THE_NATIVE_AUDIO_DEPENDENCY_GRAPH_CONTAINS_A_CYCLE);
    }

    free(names);
    free(head);
    free(degree);
    free(next);
    free(destination);
    free(queue);

    if (ok && mods) {
        EsMap keys = {0};

        for (size_t m = 0; ok && m < mods->count; m++) {
            ok = es_map_put(&keys, mods->items[m].key, m);
        }

        for (size_t i = 0; ok && i < plan->effect_count; i++) {
            if (!es_map_get(&keys, plan->effects[i].key, NULL)) {
                ok = es_fail(error, cap,
                             DM_TEXT_REPOSITORY_AN_AFFECTED_NATIVE_RESOURCE_HAS_NO_LOGICAL_OWNER_S,
                             plan->effects[i].path);
            }
        }

        es_map_free(&keys);
    }

    return ok;
}

static bool missing_parents(EsRepository *r, const char *path, EsPaths *dirs, char *error,
                            size_t cap) {
    char copy[DME_PATH_LIMIT];
    snprintf(copy, sizeof(copy), "%s", path);
    bool ok = true;

    for (char *p = copy; ok && *p; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = 0;
        bool exists = false, directory = false;
        ok = es_paths_has(dirs, copy) ||
             (es_fs_type(r->root, copy, &exists, &directory, error, cap) &&
              (exists ? directory : es_paths_add(dirs, copy)));
        *p = '/';
    }

    return ok;
}

/* Physical files managed by other scopes of the same checkout (another build version). */
static bool foreign_destinations(EsRepository *r, EsPaths *out, char *error, size_t cap) {
    sqlite3_stmt *s = NULL;

    if (!prepare(r,
                 "SELECT f.path FROM files f JOIN scopes s ON s.id=f.scope "
                 "WHERE s.identity=? AND f.scope<>? AND (f.current IS NOT f.baseline OR "
                 "f.current_mode<>f.baseline_mode)",
                 &s, error, cap)) {
        return false;
    }

    text(s, 1, es_root_identity(r->root));
    sqlite3_bind_int64(s, 2, r->scope);
    bool ok = true;
    int result;

    while (ok && (result = sqlite3_step(s)) == SQLITE_ROW) {
        char *key =
            es_fs_destination_key(r->root, (const char *)sqlite3_column_text(s, 0), error, cap);
        ok = key && es_strings_add(out, key);
        free(key);
    }

    sqlite3_finalize(s);
    return ok && result == SQLITE_DONE;
}

/* ---- Durable execution ------------------------------------------------------------------ */

static bool collect_garbage(EsRepository *r, char *error, size_t cap) {
    return sql(r,
               "DELETE FROM content WHERE id NOT IN ("
               "SELECT baseline FROM files WHERE baseline IS NOT NULL UNION "
               "SELECT current FROM files WHERE current IS NOT NULL UNION "
               "SELECT recipe FROM modifications UNION "
               "SELECT before FROM pending_files WHERE before IS NOT NULL UNION "
               "SELECT after FROM pending_files WHERE after IS NOT NULL)",
               error, cap);
}

/* Replaces the scope's baseline with the current inventory. Files whose signature matches the
   recorded current state reuse its stored content instead of being read again. */
static bool snapshot_baseline(EsRepository *r, char *error, size_t cap) {
    if (!run(r, "DELETE FROM directories WHERE scope=?1", 0, error, cap) ||
        !run(r, "DELETE FROM modifications WHERE scope=?1", 0, error, cap)) {
        return false;
    }

    if (lazy(r)) {
        return run(r, "DELETE FROM files WHERE scope=?1", 0, error, cap);
    }

    EsPaths inventory = {0};
    clear_observed(r);
    r->observing = true;
    bool ok = run_inventory(r, &inventory, error, cap);
    r->observing = false;
    sqlite3_int64 *ids = calloc(inventory.count + 1, sizeof(*ids));
    uint32_t *modes = calloc(inventory.count + 1, sizeof(*modes));
    EsStat *seen = calloc(inventory.count + 1, sizeof(*seen));
    ok = ok && ids && modes && seen;

    for (size_t i = 0; ok && i < inventory.count; ++i) {
        const Entry *e = entry(r, inventory.items[i]);
        ok = !cancelled(r) && signature(r, inventory.items[i], &seen[i], error, cap);

        if (ok && e && e->current && same_signature(&seen[i], &e->seen)) {
            ids[i] = e->current;
            modes[i] = e->current_mode;
            continue;
        }

        EsFile file = {0};
        ok = ok && es_fs_read(r->root, inventory.items[i], &file, error, cap) &&
             put_content(r, &file, NULL, &ids[i], error, cap);
        modes[i] = file.mode;
        seen[i].exists = seen[i].exists && file.exists;
        es_file_free(&file);
    }

    sqlite3_stmt *s = NULL;
    ok = ok && run(r, "DELETE FROM files WHERE scope=?1", 0, error, cap) &&
         prepare(r,
                 "INSERT INTO files(scope,path,baseline,baseline_mode,current,current_mode,"
                 "seen_size,seen_mtime,seen_mode) VALUES(?1,?2,?3,?4,?3,?4,?5,?6,?7)",
                 &s, error, cap);

    for (size_t i = 0; ok && i < inventory.count; ++i) {
        if (!ids[i]) {
            continue;
        }

        sqlite3_bind_int64(s, 1, r->scope);
        text(s, 2, inventory.items[i]);
        sqlite3_bind_int64(s, 3, ids[i]);
        sqlite3_bind_int(s, 4, (int)modes[i]);
        sqlite3_bind_int64(s, 5, seen[i].exists ? seen[i].size : -1);
        sqlite3_bind_int64(s, 6, seen[i].mtime);
        sqlite3_bind_int(s, 7, (int)seen[i].mode);
        ok = step(r, s, error, cap);
    }

    sqlite3_finalize(s);
    free(ids);
    free(modes);
    free(seen);
    es_paths_free(&inventory);
    return ok;
}

/* Records the applied state: each written path's new current content and signature (capturing
   a first-touch baseline for lazy adapters), the current intents and owned directories. */
static bool settle(EsRepository *r, const EsPlan *plan, const EsPlan *before,
                   const sqlite3_int64 *before_ids, const sqlite3_int64 *after_ids,
                   const EsPaths *created, const EsPaths *removed, const EsModifications *mods,
                   char *error, size_t cap) {
    sqlite3_stmt *upsert = NULL, *drop = NULL;
    bool ok = prepare(r,
                      "INSERT INTO files(scope,path,baseline,baseline_mode,current,current_mode,"
                      "seen_size,seen_mtime,seen_mode) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) "
                      "ON CONFLICT(scope,path) DO UPDATE SET current=excluded.current,"
                      "current_mode=excluded.current_mode,seen_size=excluded.seen_size,"
                      "seen_mtime=excluded.seen_mtime,seen_mode=excluded.seen_mode",
                      &upsert, error, cap) &&
              prepare(r, "DELETE FROM files WHERE scope=? AND path=?", &drop, error, cap);

    for (size_t i = 0; ok && i < plan->count; ++i) {
        const EsWrite *write = &plan->writes[i];
        const Entry *e = entry(r, write->path);
        sqlite3_int64 baseline = e ? e->baseline : lazy(r) ? before_ids[i] : 0;
        uint32_t baseline_mode = e ? e->baseline_mode : before->writes[i].file.mode;

        if (!baseline && !after_ids[i]) {
            sqlite3_bind_int64(drop, 1, r->scope);
            text(drop, 2, write->path);
            ok = step(r, drop, error, cap);
            continue;
        }

        EsStat info;
        ok = es_fs_stat(r->root, write->path, &info, error, cap);
        sqlite3_bind_int64(upsert, 1, r->scope);
        text(upsert, 2, write->path);
        bind_content(upsert, 3, baseline);
        sqlite3_bind_int(upsert, 4, (int)baseline_mode);
        bind_content(upsert, 5, after_ids[i]);
        sqlite3_bind_int(upsert, 6, (int)write->file.mode);
        sqlite3_bind_int64(upsert, 7, info.exists ? info.size : -1);
        sqlite3_bind_int64(upsert, 8, info.mtime);
        sqlite3_bind_int(upsert, 9, (int)info.mode);
        ok = ok && step(r, upsert, error, cap);
    }

    sqlite3_finalize(upsert);
    sqlite3_finalize(drop);
    upsert = NULL;

    /* Current intents keep their stable IDs, which order the history list. */
    ok = ok && run(r, "DELETE FROM modifications WHERE scope=?1", 0, error, cap) &&
         prepare(r,
                 "INSERT INTO modifications(id,scope,logical_key,label,recipe) "
                 "VALUES(NULLIF(?,0),?,?,?,?)",
                 &upsert, error, cap) &&
         prepare(r, "INSERT OR IGNORE INTO modification_paths VALUES(?,?)", &drop, error, cap);

    for (size_t i = 0; ok && mods && i < mods->count; i++) {
        const EsModification *m = &mods->items[i];
        EsFile content = {.data = m->data, .size = m->size, .exists = true};
        sqlite3_int64 recipe = 0;
        ok = put_content(r, &content, NULL, &recipe, error, cap);
        sqlite3_bind_int64(upsert, 1, (sqlite3_int64)m->order);
        sqlite3_bind_int64(upsert, 2, r->scope);
        text(upsert, 3, m->key);
        text(upsert, 4, m->label);
        sqlite3_bind_int64(upsert, 5, recipe);
        ok = ok && step(r, upsert, error, cap);
        sqlite3_int64 id = sqlite3_last_insert_rowid(r->db);

        for (size_t e = 0; ok && e < plan->effect_count; e++) {
            if (!strcmp(plan->effects[e].key, m->key)) {
                sqlite3_bind_int64(drop, 1, id);
                text(drop, 2, plan->effects[e].path);
                ok = step(r, drop, error, cap);
            }
        }
    }

    sqlite3_finalize(upsert);
    sqlite3_finalize(drop);
    upsert = drop = NULL;
    ok = ok && prepare(r, "INSERT OR IGNORE INTO directories VALUES(?,?)", &upsert, error, cap) &&
         prepare(r, "DELETE FROM directories WHERE scope=? AND path=?", &drop, error, cap);

    for (size_t i = 0; ok && i < created->count; i++) {
        sqlite3_bind_int64(upsert, 1, r->scope);
        text(upsert, 2, created->items[i]);
        ok = step(r, upsert, error, cap);
    }

    for (size_t i = 0; ok && i < removed->count; i++) {
        sqlite3_bind_int64(drop, 1, r->scope);
        text(drop, 2, removed->items[i]);
        ok = step(r, drop, error, cap);
    }

    sqlite3_finalize(upsert);
    sqlite3_finalize(drop);
    return ok;
}

/* The durable state machine shared by exports, reverts and initialization:
   validate -> persist before/after -> apply -> verify -> commit ledger -> discard recovery. */
static bool execute_plan(EsRepository *r, EsPlan *plan, int kind, const EsModifications *mods,
                         char *error, size_t cap) {
    EsPlan before = {0};
    EsPaths created = {0}, removed = {0}, foreign = {0}, destinations = {0};
    sqlite3_int64 *before_ids = calloc(plan->count + 1, sizeof(*before_ids));
    sqlite3_int64 *after_ids = calloc(plan->count + 1, sizeof(*after_ids));
    sqlite3_stmt *s = NULL;
    bool ok = before_ids && after_ids && validate_graph(r, plan, mods, error, cap) &&
              foreign_destinations(r, &foreign, error, cap);

    /* Owned directories no longer holding a desired file are removed after writing. */
    if (ok && kind == OPERATION_APPLY &&
        prepare(r, "SELECT path FROM directories WHERE scope=? ORDER BY length(path) DESC,path", &s,
                error, cap)) {
        sqlite3_bind_int64(s, 1, r->scope);
        int result = SQLITE_DONE;

        while (ok && (result = sqlite3_step(s)) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(s, 0);
            size_t length = strlen(path);
            bool needed = false;

            for (size_t i = 0; !needed && i < plan->count; i++) {
                const EsWrite *write = &plan->writes[i];
                needed = write->file.exists && !strncmp(write->path, path, length) &&
                         write->path[length] == '/';
            }

            for (size_t i = 0; !needed && i < r->entry_count; i++) {
                const Entry *e = &r->entries[i];
                needed = e->current && !es_plan_find(plan, e->path) &&
                         !strncmp(e->path, path, length) && e->path[length] == '/';
            }

            ok = needed || es_paths_add(&removed, path);
        }

        ok = ok && result == SQLITE_DONE;
        sqlite3_finalize(s);
        s = NULL;
    }

    for (size_t i = 0; ok && i < plan->count; ++i) {
        EsWrite *write = &plan->writes[i];
        EsFile file = {0};
        char *key = NULL;
        ok = allowed(r, write->path) ||
             es_fail(error, cap,
                     DM_TEXT_REPOSITORY_PATH_IS_OUTSIDE_THE_INITIALIZED_AUDIO_NAMESPACE_S,
                     write->path);
        ok = ok && (key = es_fs_destination_key(r->root, write->path, error, cap)) != NULL;

        if (ok && es_paths_has(&destinations, key)) {
            size_t other;
            es_map_get(&destinations.index, key, &other);
            ok = es_fail(error, cap, DM_TEXT_REPOSITORY_ALIASED_REPOSITORY_DESTINATIONS_S_AND_S,
                         write->path, plan->writes[other].path);
        }

        if (ok && es_paths_has(&foreign, key)) {
            ok = es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_ANOTHER_CHECKOUT_VERSION_SCOPE_OWNS_THIS_PHYSICAL_RESOURCE_S,
                write->path);
        }

        ok = ok && es_strings_add(&destinations, key) &&
             es_fs_read(r->root, write->path, &file, error, cap) &&
             es_plan_put(&before, write->path, file.data, file.size, file.mode, file.exists, error,
                         cap);
        free(key);

        /* An inventoried namespace knows every initialized file; an unknown file is drift. */
        if (ok && file.exists && !lazy(r) && !entry(r, write->path)) {
            ok = es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_REPOSITORY_CHANGED_OUTSIDE_DUCKMIDI_S_RESOLVE_THIS_PATH_OR,
                write->path);
        }

        if (ok && write->file.exists && !write->file.mode) {
            write->file.mode = file.exists ? file.mode : 0644;
        }

        ok = ok && (!write->file.exists || missing_parents(r, write->path, &created, error, cap));
        es_file_free(&file);
    }

    es_paths_free(&foreign);
    es_paths_free(&destinations);
    sqlite3_int64 tx = 0;
    ok = ok && !cancelled(r) && sql(r, "BEGIN IMMEDIATE", error, cap);

    if (ok && prepare(r, "INSERT INTO pending(scope,kind) VALUES(?,?)", &s, error, cap)) {
        sqlite3_bind_int64(s, 1, r->scope);
        sqlite3_bind_int(s, 2, kind);
        ok = step(r, s, error, cap);
        tx = sqlite3_last_insert_rowid(r->db);
        sqlite3_finalize(s);
        s = NULL;
    } else {
        ok = false;
    }

    ok = ok && prepare(r,
                       "INSERT INTO pending_files(pending,ordinal,path,before,before_mode,after,"
                       "after_mode) VALUES(?,?,?,?,?,?,?)",
                       &s, error, cap);

    for (size_t i = 0; ok && i < plan->count; ++i) {
        const EsFile *old = &before.writes[i].file;
        ok = es_fs_stage_available(r->root, plan->writes[i].path, (uint64_t)tx, i, error, cap) &&
             put_content(r, old, NULL, &before_ids[i], error, cap) &&
             put_content(r, &plan->writes[i].file, NULL, &after_ids[i], error, cap);
        sqlite3_bind_int64(s, 1, tx);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)i);
        text(s, 3, plan->writes[i].path);
        bind_content(s, 4, before_ids[i]);
        sqlite3_bind_int(s, 5, (int)old->mode);
        bind_content(s, 6, after_ids[i]);
        sqlite3_bind_int(s, 7, (int)plan->writes[i].file.mode);
        ok = ok && step(r, s, error, cap);
    }

    sqlite3_finalize(s);
    s = NULL;
    ok = ok && prepare(r,
                       "INSERT INTO pending_directories(pending,ordinal,path,created) "
                       "VALUES(?,?,?,?)",
                       &s, error, cap);

    for (size_t i = 0; ok && i < created.count + removed.count; ++i) {
        bool create = i < created.count;
        sqlite3_bind_int64(s, 1, tx);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)i);
        text(s, 3, create ? created.items[i] : removed.items[i - created.count]);
        sqlite3_bind_int(s, 4, create ? 1 : -1);
        ok = step(r, s, error, cap);
    }

    sqlite3_finalize(s);
    s = NULL;

    if (!ok || cancelled(r) || !sql(r, "COMMIT", error, cap)) {
        sql(r, "ROLLBACK", NULL, 0);
        es_plan_free(&before);
        es_paths_free(&created);
        es_paths_free(&removed);
        free(before_ids);
        free(after_ids);
        return false;
    }

    progress(r, 20);

    for (size_t i = 0; ok && i < created.count; ++i) {
        ok = !cancelled(r) && es_fs_mkdir(r->root, created.items[i], error, cap);
    }

    for (size_t i = 0; ok && i < plan->count; ++i) {
        EsFile actual = {0};
        ok = !cancelled(r) && es_fs_read(r->root, plan->writes[i].path, &actual, error, cap);

        if (ok && !equal_file(&actual, &before.writes[i].file)) {
            ok = es_fail(error, cap, DM_TEXT_REPOSITORY_REPOSITORY_CHANGED_DURING_APPLICATION_S,
                         plan->writes[i].path);
        }

        if (ok && !equal_file(&actual, &plan->writes[i].file)) {
            ok = es_fs_write(r->root, plan->writes[i].path, &plan->writes[i].file, (uint64_t)tx, i,
                             error, cap);
        }

        es_file_free(&actual);
        progress(r, 20 + (uint32_t)((i + 1) * 60 / (plan->count ? plan->count : 1)));
    }

    for (size_t i = 0; ok && i < plan->count; ++i) {
        EsFile actual = {0};
        ok = es_fs_read(r->root, plan->writes[i].path, &actual, error, cap) &&
             equal_file(&actual, &plan->writes[i].file);
        es_file_free(&actual);
    }

    for (size_t i = 0; ok && i < removed.count; i++) {
        ok = !cancelled(r) && es_fs_rmdir(r->root, removed.items[i], error, cap);
    }

    progress(r, 90);
    ok = ok && !cancelled(r) && sql(r, "BEGIN IMMEDIATE", error, cap);

    if (ok) {
        ok = kind == OPERATION_INITIALIZE ? snapshot_baseline(r, error, cap)
                                          : settle(r, plan, &before, before_ids, after_ids,
                                                   &created, &removed, mods, error, cap);
        ok = ok && run(r, "UPDATE scopes SET generation=generation+1 WHERE id=?1", 0, error, cap) &&
             run(r, "DELETE FROM pending WHERE id=?2", tx, error, cap) &&
             collect_garbage(r, error, cap) && sql(r, "COMMIT", error, cap);
    }

    if (!ok) {
        char original[2048];
        snprintf(original, sizeof(original), "%s",
                 cancelled(r) ? DM_TEXT_REPOSITORY_REPOSITORY_OPERATION_CANCELED : error);
        sql(r, "ROLLBACK", NULL, 0);
        char recovery[2048] = {0};

        if (recover(r, recovery, sizeof(recovery))) {
            es_fail(error, cap, DM_TEXT_REPOSITORY_FILES_RESTORED_S, original);
        } else {
            es_fail(error, cap, "%s %s", original, recovery);
        }
    }

    es_plan_free(&before);
    es_paths_free(&created);
    es_paths_free(&removed);
    free(before_ids);
    free(after_ids);
    char reload[256];
    ok = load_index(r, reload, sizeof(reload)) && ok;
    progress(r, 100);
    return ok;
}

/* ---- Operations ------------------------------------------------------------------------- */

bool es_repository_initialize(EsRepository *r, char *error, size_t cap) {
    if (!recover(r, error, cap)) {
        return false;
    }

    /* Sidecars duckmidi created are removed while unchanged; outside edits keep them. */
    EsPlan cleanup = {0};
    bool ok = true;

    for (size_t i = 0; ok && i < r->entry_count; ++i) {
        const Entry *e = &r->entries[i];

        if (!is_backup(e->path) || !modified(e)) {
            continue;
        }

        EsFile actual = {0}, original = {0};
        uint8_t hash[32];
        ok = es_fs_read(r->root, e->path, &actual, error, cap);

        if (ok && actual.exists && e->current) {
            es_sha256(actual.data, actual.size, hash);

            if (!memcmp(hash, e->current_hash, 32)) {
                ok = get_content(r, e->baseline, e->baseline_mode, &original, error, cap) &&
                     es_plan_put(&cleanup, e->path, original.data, original.size, original.mode,
                                 original.exists, error, cap);
            }
        }

        es_file_free(&actual);
        es_file_free(&original);
    }

    ok = ok && execute_plan(r, &cleanup, OPERATION_INITIALIZE, NULL, error, cap);
    es_plan_free(&cleanup);
    return ok || described(error, cap);
}

static void remove_modification(EsModifications *mods, size_t index) {
    EsModification *m = &mods->items[index];
    SDL_free(m->key);
    SDL_free(m->label);
    free(m->data);
    es_paths_free(&m->paths);
    memmove(m, m + 1, (mods->count - index - 1) * sizeof(*m));
    mods->count--;
}

/* Drops intents whose every effect already equals the baseline, then re-renders. */
static bool prune_baseline(EsRepository *r, EsModifications *mods, EsPlan *plan, char *error,
                           size_t cap) {
    bool changed = false;

    for (size_t i = 0; i < mods->count;) {
        bool owned = false, baseline = true;

        for (size_t e = 0; baseline && e < plan->effect_count; e++) {
            const EsEffect *effect = &plan->effects[e];
            const EsFile *desired = es_plan_find(plan, effect->path);

            if (strcmp(effect->key, mods->items[i].key)) {
                continue;
            }

            owned = true;

            if (desired && !same_as_baseline(r, effect->path, desired, &baseline, error, cap)) {
                return false;
            }
        }

        if (owned && baseline) {
            remove_modification(mods, i);
            changed = true;
        } else {
            i++;
        }
    }

    if (changed) {
        es_plan_free(plan);
        return es_render(r, mods, plan, error, cap);
    }

    return true;
}

/* Renders the complete desired state for the given intents and applies it. */
static bool apply_state(EsRepository *r, EsModifications *mods, char *error, size_t cap) {
    EsPlan plan = {0};
    bool ok = es_render(r, mods, &plan, error, cap) && prune_baseline(r, mods, &plan, error, cap);

    /* Paths no longer rendered return to their baseline. */
    for (size_t i = 0; ok && i < r->entry_count; ++i) {
        const Entry *e = &r->entries[i];

        if (modified(e) && !es_plan_find(&plan, e->path)) {
            EsFile original = {0};
            ok = get_content(r, e->baseline, e->baseline_mode, &original, error, cap) &&
                 es_plan_put(&plan, e->path, original.data, original.size, original.mode,
                             original.exists, error, cap);
            es_file_free(&original);
        }
    }

    /* A first-original sidecar stays while its source is still changed, even if a later export
       turns the optional backup setting off. */
    for (size_t i = 0; ok && i < r->entry_count; ++i) {
        const Entry *e = &r->entries[i];
        char source[DME_PATH_LIMIT];
        size_t length = strlen(e->path);

        if (!is_backup(e->path) || !modified(e) || length >= sizeof(source)) {
            continue;
        }

        memcpy(source, e->path, length - 4);
        source[length - 4] = 0;
        const EsFile *desired = es_plan_find(&plan, source);
        bool exists = false, same = true;
        ok = !desired || (es_repository_baseline_exists(r, source, &exists, error, cap) &&
                          (!exists || same_as_baseline(r, source, desired, &same, error, cap)));

        if (ok && desired && exists && !same) {
            ok = es_plan_backup(&plan, source);
        }
    }

    for (size_t i = 0; ok && i < plan.backups.count; ++i) {
        const char *path = plan.backups.items[i];
        char sidecar[DME_PATH_LIMIT];
        EsFile original = {0}, backup = {0};
        ok = backup_path(path, sidecar) && read_baseline(r, path, &original, error, cap) &&
             read_baseline(r, sidecar, &backup, error, cap);

        if (ok && original.exists && backup.exists &&
            (original.size != backup.size || memcmp(original.data, backup.data, original.size))) {
            ok = es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_CONFLICTING_PREEXISTING_BACKUP_S_IT_WILL_NOT_BE_OVERWRITTEN,
                sidecar);
        }

        if (ok && original.exists) {
            ok = es_plan_put(&plan, sidecar, original.data, original.size,
                             backup.exists ? backup.mode : 0644, true, error, cap);
        }

        es_file_free(&original);
        es_file_free(&backup);
    }

    ok = ok && execute_plan(r, &plan, OPERATION_APPLY, mods, error, cap);
    es_plan_free(&plan);
    return ok;
}

static bool set_modification(EsModification *m, const char *key, const char *label,
                             const void *recipe, size_t size) {
    SDL_free(m->key);
    SDL_free(m->label);
    free(m->data);
    es_paths_free(&m->paths);
    m->key = SDL_strdup(key);
    m->label = SDL_strdup(label);
    m->data = malloc(size ? size : 1);
    m->size = size;

    if (m->data && size) {
        memcpy(m->data, recipe, size);
    }

    return m->key && m->label && m->data;
}

static bool valid_intent(const EsModification *m) {
    return m->key && *m->key && strlen(m->key) <= DME_LABEL_LIMIT && m->label &&
           strlen(m->label) <= DME_LABEL_LIMIT && m->data && m->size <= ES_MAX_RECIPES;
}

/* Directories above path, created when missing. */
static bool make_parents(EsRoot *root, const char *path, char *error, size_t cap) {
    char parent[DME_PATH_LIMIT];
    size_t length = strlen(path);

    if (length >= sizeof(parent)) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_REPOSITORY_WRITE_S, path);
    }

    memcpy(parent, path, length + 1);
    bool ok = true;

    for (char *slash = strchr(parent, '/'); ok && slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        ok = ensure_directory(root, parent, error, cap);
        *slash = '/';
    }

    return ok;
}

/* History off: renders only these intents over the current files and writes the changed paths
   directly. With Keep original files on, a file's current content is kept beside it as .bkp
   before it first changes, and an existing .bkp is never overwritten. With it off, files are
   simply overwritten. */
static bool apply_untracked(EsRepository *r, const EsModification *incoming, size_t count,
                            char *error, size_t cap) {
    EsModifications mods = {.items = (EsModification *)incoming, .count = count};
    EsPlan plan = {0};
    bool ok = es_render(r, &mods, &plan, error, cap) && validate_graph(r, &plan, &mods, error, cap);

    for (size_t i = 0; ok && i < plan.count; ++i) {
        const EsWrite *write = &plan.writes[i];
        EsFile current = {0};
        char sidecar[DME_PATH_LIMIT];
        ok = (allowed(r, write->path) ||
              es_fail(error, cap,
                      DM_TEXT_REPOSITORY_PATH_IS_OUTSIDE_THE_INITIALIZED_AUDIO_NAMESPACE_S,
                      write->path)) &&
             backup_path(write->path, sidecar) &&
             es_fs_read(r->root, write->path, &current, error, cap);
        bool same = ok && current.exists == write->file.exists &&
                    (!current.exists ||
                     (current.size == write->file.size &&
                      (!current.size || !memcmp(current.data, write->file.data, current.size))));

        if (ok && !same && current.exists && r->keep_originals) {
            EsStat backup;
            ok = es_fs_stat(r->root, sidecar, &backup, error, cap);

            if (ok && !backup.exists) {
                ok = es_fs_write(r->root, sidecar, &current, 0, i, error, cap);
            }
        }

        if (ok && !same) {
            EsFile desired = write->file;
            desired.mode = desired.mode ? desired.mode : current.mode ? current.mode : 0644;
            ok = (!desired.exists || make_parents(r->root, write->path, error, cap)) &&
                 es_fs_write(r->root, write->path, &desired, 0, i, error, cap);
        }

        es_file_free(&current);
    }

    es_plan_free(&plan);
    return ok || described(error, cap);
}

/* Upserts incoming intents by logical key, or removes one key (all when key is NULL). */
static bool modify(EsRepository *r, const EsModification *incoming, size_t count,
                   const char *remove_key, bool remove, char *error, size_t cap) {
    if (r->untracked) {
        return !remove ? apply_untracked(r, incoming, count, error, cap)
                       : es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_REPOSITORY_ACTION);
    }

    if (!r->db || !recover(r, error, cap) || !check_drift(r, error, cap)) {
        return false;
    }

    EsModifications mods = {0};
    bool ok = es_repository_modifications(r, &mods, error, cap);

    for (size_t i = 0; ok && remove && i < mods.count;) {
        if (!remove_key || !strcmp(mods.items[i].key, remove_key)) {
            remove_modification(&mods, i);
        } else {
            i++;
        }
    }

    for (size_t n = 0; ok && n < count; n++) {
        size_t index = 0;

        while (index < mods.count && strcmp(mods.items[index].key, incoming[n].key)) {
            index++;
        }

        if (index == mods.count) {
            EsModification *grown = realloc(mods.items, (mods.count + 1) * sizeof(*mods.items));
            ok = grown != NULL;

            if (!ok) {
                break;
            }

            mods.items = grown;
            mods.items[mods.count++] = (EsModification){0};
        }

        ok = set_modification(&mods.items[index], incoming[n].key, incoming[n].label,
                              incoming[n].data, incoming[n].size);
    }

    ok = ok && apply_state(r, &mods, error, cap);
    es_modifications_free(&mods);
    return ok || described(error, cap);
}

bool es_repository_apply(EsRepository *r, const char *key, const char *label, const void *recipe,
                         size_t size, char *error, size_t cap) {
    EsModification m = {
        .key = (char *)key, .label = (char *)label, .data = (uint8_t *)recipe, .size = size};
    return es_repository_apply_batch(r, &m, 1, error, cap);
}

bool es_repository_apply_batch(EsRepository *r, const EsModification *incoming, size_t count,
                               char *error, size_t cap) {
    if (!incoming || !count || count > ES_MAX_PATHS) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_REPOSITORY_WRITE_S, "");
    }

    for (size_t n = 0; n < count; n++) {
        if (!valid_intent(&incoming[n])) {
            return es_fail(error, cap, DM_TEXT_REPOSITORY_INVALID_REPOSITORY_WRITE_S,
                           incoming[n].key ? incoming[n].key : "");
        }

        for (size_t previous = 0; previous < n; previous++) {
            if (!strcmp(incoming[previous].key, incoming[n].key)) {
                return es_fail(error, cap,
                               DM_TEXT_REPOSITORY_DUPLICATE_LOGICAL_MODIFICATION_IN_ONE_OPERATION);
            }
        }
    }

    return modify(r, incoming, count, NULL, false, error, cap);
}

bool es_repository_revert(EsRepository *r, const char *key, char *error, size_t cap) {
    return modify(r, NULL, 0, key, true, error, cap);
}

/* ---- Opening ---------------------------------------------------------------------------- */

static bool open_database(EsRepository *r, const char *path, bool writable, char *error,
                          size_t cap) {
    int flags = (writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY) |
                SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
    sqlite3_stmt *s = NULL;
    bool ok = sqlite3_open_v2(path, &r->db, flags, history_vfs()) == SQLITE_OK;

    if (ok) {
        sqlite3_busy_timeout(r->db, 2000);
        sqlite3_limit(r->db, SQLITE_LIMIT_LENGTH, ES_MAX_RECIPES + 65536);
        ok = sql(r,
                 writable ? "PRAGMA foreign_keys=ON;PRAGMA journal_mode=DELETE;"
                            "PRAGMA synchronous=FULL;PRAGMA trusted_schema=OFF;"
                          : "PRAGMA foreign_keys=ON;PRAGMA trusted_schema=OFF;",
                 error, cap) &&
             prepare(r, "PRAGMA user_version", &s, error, cap);
    }

    if (ok) {
        ok = sqlite3_step(s) == SQLITE_ROW;
        int version = ok ? sqlite3_column_int(s, 0) : -1;
        sqlite3_finalize(s);

        if (!version && writable) {
            ok = sql(r, "BEGIN IMMEDIATE", error, cap) && sql(r, schema, error, cap) &&
                 sql(r, "COMMIT", error, cap);
        } else if (version != ES_DATABASE_SCHEMA) {
            ok = es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_UNSUPPORTED_REPOSITORY_HISTORY_SCHEMA_D_USE_A_COMPATIBLE_DUCKMIDI,
                version);
        }
    }

    return ok;
}

EsRepository *es_repository_open(const EsOpen *options, char *error, size_t cap) {
    if (!options || !options->adapter || !options->adapter->render || !options->plugins ||
        !options->checkout || !options->exporter || !options->target || !options->version) {
        es_fail(error, cap, DM_TEXT_REPOSITORY_INCOMPLETE_REPOSITORY_ADAPTER_CONFIGURATION);
        return NULL;
    }

#ifdef _WIN32
    if (options->plugins[0] == '\\' && options->plugins[1] == '\\') {
        es_fail(error, cap,
                DM_TEXT_REPOSITORY_REPOSITORY_HISTORY_MUST_BE_STORED_IN_A_LOCAL_PLUGINS);
        return NULL;
    }
#endif
    EsRepository *r = calloc(1, sizeof(*r));

    if (!r) {
        return NULL;
    }

    r->host = options->host;
    r->adapter = options->adapter;
    r->writer = es_writer_lock(options->plugins, error, cap);
    r->root = r->writer ? es_root_open(options->checkout, error, cap) : NULL;
    r->checkout_lock = r->root ? es_checkout_lock(r->root, error, cap) : NULL;
    const char *database = es_writer_database(r->writer);
    bool ok =
        r->root && r->checkout_lock && database && open_database(r, database, true, error, cap);
    sqlite3_stmt *s = NULL;

    if (ok &&
        prepare(
            r,
            "INSERT INTO scopes(exporter,target,target_name,root,identity,version,capabilities) "
            "VALUES(?,?,?,?,?,?,?) ON CONFLICT(exporter,target,identity,version) DO UPDATE "
            "SET root=excluded.root,target_name=excluded.target_name,"
            "capabilities=excluded.capabilities RETURNING id,generation",
            &s, error, cap)) {
        text(s, 1, options->exporter);
        text(s, 2, options->target);
        text(s, 3, options->target_name ? options->target_name : options->target);
        text(s, 4, es_root_name(r->root));
        text(s, 5, es_root_identity(r->root));
        text(s, 6, options->version);
        sqlite3_bind_int(s, 7, (int)options->capabilities);
        ok = sqlite3_step(s) == SQLITE_ROW;
        r->scope = ok ? sqlite3_column_int64(s, 0) : 0;
        bool initialized = ok && sqlite3_column_int64(s, 1) > 0;
        sqlite3_finalize(s);
        ok = ok && recover(r, error, cap) && load_index(r, error, cap) &&
             (initialized || es_repository_initialize(r, error, cap));
    } else {
        ok = false;
    }

    if (!ok) {
        if (error && cap && !*error) {
            es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_OPEN_WRITABLE_PLUGIN_REPOSITORY_HISTORY_S,
                    options->plugins);
        }

        es_repository_close(r);
        return NULL;
    }

    return r;
}

EsRepository *es_repository_probe(const char *checkout, const DmeHistoryAdapter *adapter,
                                  char *error, size_t cap) {
    if (!checkout || !adapter || !adapter->render) {
        return NULL;
    }

    EsRepository *r = calloc(1, sizeof(*r));

    if (r) {
        r->adapter = adapter;
        r->root = es_root_open(checkout, error, cap);

        if (!r->root) {
            free(r);
            r = NULL;
        }
    }

    return r;
}

EsRepository *es_repository_open_untracked(const char *checkout, const DmeHistoryAdapter *adapter,
                                           const DmeHost *host, bool keep_originals, char *error,
                                           size_t cap) {
    EsRepository *r = es_repository_probe(checkout, adapter, error, cap);

    if (r) {
        r->host = host;
        r->untracked = true;
        r->keep_originals = keep_originals;
        r->checkout_lock = es_checkout_lock(r->root, error, cap);

        if (!r->checkout_lock) {
            es_repository_close(r);
            r = NULL;
        }
    }

    return r;
}

bool es_repository_untracked(const EsRepository *r) {
    return r && r->untracked;
}

uint64_t es_repository_generation(const EsRepository *r) {
    sqlite3_stmt *statement = NULL;
    uint64_t generation = 0;

    if (r && r->db &&
        sqlite3_prepare_v2(r->db, "SELECT generation FROM scopes WHERE id=?", -1, &statement,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_int64(statement, 1, r->scope);

        if (sqlite3_step(statement) == SQLITE_ROW) {
            generation = (uint64_t)sqlite3_column_int64(statement, 0);
        }
    }

    sqlite3_finalize(statement);
    return generation;
}

void es_repository_close(EsRepository *r) {
    if (r) {
        if (r->db) {
            sqlite3_close(r->db);
        }

        clear_index(r);
        clear_observed(r);

        for (size_t i = 0; i < r->cache_count; ++i) {
            es_file_free(&r->cache[i].file);
        }

        free(r->cache);
        es_root_close(r->root);
        es_checkout_unlock(r->checkout_lock);
        es_writer_unlock(r->writer);
        free(r);
    }
}

bool es_history_scopes(const char *plugins, const char *exporter, EsScopes *out, char *error,
                       size_t cap) {
    *out = (EsScopes){0};
    EsRepository connection = {0};
    connection.writer = es_writer_lock(plugins, error, cap);

    if (!connection.writer) {
        return false;
    }

    EsRoot *root = es_root_open(plugins, error, cap);
    bool exists = false, directory = false;
    bool ok =
        root && es_fs_type(root, "history/repositories.sqlite3", &exists, &directory, error, cap);
    es_root_close(root);

    if (ok && !exists) {
        es_writer_unlock(connection.writer);
        return true;
    }

    const char *path = es_writer_database(connection.writer);
    ok = ok && !directory && path && open_database(&connection, path, false, error, cap);
    size_t capacity = 0;
    sqlite3_stmt *s = NULL;

    if (ok && prepare(&connection,
                      "SELECT id,generation,capabilities,exporter,target,target_name,root,version "
                      "FROM scopes WHERE exporter=? ORDER BY target,root,version,id",
                      &s, error, cap)) {
        text(s, 1, exporter);
        int result;

        while (ok && (result = sqlite3_step(s)) == SQLITE_ROW) {
            ok = out->count < ES_MAX_PATHS &&
                 es_reserve((void **)&out->items, &capacity, out->count + 1, sizeof(*out->items));

            if (!ok) {
                break;
            }

            EsScope *scope = &out->items[out->count++];
            *scope = (EsScope){.id = (uint64_t)sqlite3_column_int64(s, 0),
                               .generation = (uint64_t)sqlite3_column_int64(s, 1),
                               .capabilities = (uint32_t)sqlite3_column_int(s, 2),
                               .exporter = SDL_strdup((const char *)sqlite3_column_text(s, 3)),
                               .target = SDL_strdup((const char *)sqlite3_column_text(s, 4)),
                               .target_name = SDL_strdup((const char *)sqlite3_column_text(s, 5)),
                               .checkout = SDL_strdup((const char *)sqlite3_column_text(s, 6)),
                               .version = SDL_strdup((const char *)sqlite3_column_text(s, 7))};
            ok = scope->exporter && scope->target && scope->target_name && scope->checkout &&
                 scope->version;
        }

        ok = ok && result == SQLITE_DONE;
        sqlite3_finalize(s);
    } else {
        ok = false;
    }

    if (connection.db) {
        sqlite3_close(connection.db);
    }

    es_writer_unlock(connection.writer);

    if (!ok) {
        es_scopes_free(out);
    }

    return ok;
}

void es_scopes_free(EsScopes *scopes) {
    for (size_t i = 0; i < scopes->count; ++i) {
        SDL_free(scopes->items[i].exporter);
        SDL_free(scopes->items[i].target);
        SDL_free(scopes->items[i].target_name);
        SDL_free(scopes->items[i].checkout);
        SDL_free(scopes->items[i].version);
    }

    free(scopes->items);
    *scopes = (EsScopes){0};
}
