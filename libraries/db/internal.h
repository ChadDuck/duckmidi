#ifndef DES_INTERNAL_H
#define DES_INTERNAL_H

#include "db.h"
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

EsRoot *es_root_open(const char *, char *, size_t);
void es_root_close(EsRoot *);
const char *es_root_name(const EsRoot *);
const char *es_root_identity(const EsRoot *);
bool es_fs_read(EsRoot *, const char *, EsFile *, char *, size_t);
bool es_fs_type(EsRoot *, const char *, bool *exists, bool *directory, char *, size_t);
bool es_fs_stat(EsRoot *, const char *, EsStat *, char *, size_t);
/* Visits every regular file below path with its directory-entry metadata. */
typedef bool (*EsFsVisit)(void *user, const char *path, const EsStat *);
bool es_fs_scan(EsRoot *, const char *path, bool recursive, EsFsVisit, void *user, char *, size_t);
bool es_fs_write(EsRoot *, const char *, const EsFile *, uint64_t transaction, size_t index, char *,
                 size_t);
bool es_fs_stage_remove(EsRoot *, const char *, uint64_t transaction, size_t index,
                        const EsFile *expected, const EsFile *alternate, char *, size_t);
bool es_fs_stage_available(EsRoot *, const char *, uint64_t, size_t, char *, size_t);
char *es_fs_destination_key(EsRoot *, const char *, char *, size_t);
bool es_fs_mkdir(EsRoot *, const char *, char *, size_t);
bool es_fs_rmdir(EsRoot *, const char *, char *, size_t);
void *es_writer_lock(const char *plugins, char *, size_t);
void es_writer_unlock(void *);
const char *es_writer_database(void *);
void *es_checkout_lock(EsRoot *, char *, size_t);
void es_checkout_unlock(void *);
/* The DmeHistoryApi every adapter callback and DmeHistoryAdapter.attach receive. */
extern const DmeHistoryApi es_history_table;

#ifdef __cplusplus
}
#endif

#endif
