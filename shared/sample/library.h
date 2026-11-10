#ifndef DM_SAMPLE_LIBRARY_H
#define DM_SAMPLE_LIBRARY_H

#include "sample_manifest.h"

/* The caller selects an existing library root. Saving replaces the named set after staging
   its complete contents; no saved history or pointer files are created. */
bool dm_sample_library_publish(const char *root, DmSampleManifest *manifest,
                               const DmSampleAudio *assets,
                               const DmSampleImportCallbacks *callbacks, char *error, size_t cap);
/* Probes the existing root through the same anchored path used for publication. */
bool dm_sample_library_writable(const char *root);
/* Finds the current named folder by its stable set ID. Returned objects belong to the caller. */
bool dm_sample_library_manifest(const char *root, const char *id, DmSampleManifest *out,
                                char *error, size_t cap);
bool dm_sample_library_audio(const char *root, const DmSampleManifest *manifest, uint32_t asset,
                             const DmSampleImportCallbacks *callbacks, DmSampleAudio *out,
                             char *error, size_t cap);

typedef struct DmSampleLibraryReader DmSampleLibraryReader;

/* Worker-side batch reader. Opens one physical saved set and returns its current manifest.
   The caller owns the manifest. All reads stay anchored to that directory, so publication
   cannot substitute another set between assets. Close after preparation, before running a job;
   retain the manifest and decoded audio independently for the job's lifetime. */
DmSampleLibraryReader *dm_sample_library_reader_open(const char *root, const char *id,
                                                     DmSampleManifest *manifest, char *error,
                                                     size_t cap);
/* The same, trying a cataloged set folder first; a renamed or replaced folder falls back to
   searching the library by ID. */
DmSampleLibraryReader *dm_sample_library_reader_open_in(const char *root, const char *directory,
                                                        const char *id, DmSampleManifest *manifest,
                                                        char *error, size_t cap);
bool dm_sample_library_reader_audio(const DmSampleLibraryReader *reader,
                                    const DmSampleManifest *manifest, uint32_t asset,
                                    const DmSampleImportCallbacks *callbacks, DmSampleAudio *out,
                                    char *error, size_t cap);
void dm_sample_library_reader_close(DmSampleLibraryReader *reader);

typedef struct {
    char id[DM_SAMPLE_ID_CAP], name[DM_SAMPLE_NAME_CAP];
    char directory[DM_SAMPLE_NAME_CAP]; /* Set folder when cataloged; it may be renamed later. */
    uint32_t generation;
    uint64_t modified;
    bool writable_root;
} DmSampleCatalogEntry;

typedef struct {
    DmSampleCatalogEntry *entries;
    size_t count;
} DmSampleCatalog;

/* Catalogs are sorted by ID. A valid writable set takes precedence over a program set. */
bool dm_sample_library_catalog(const char *program_root, const char *writable_root,
                               const DmSampleImportCallbacks *callbacks, DmSampleCatalog *out,
                               char *error, size_t cap);
void dm_sample_catalog_free(DmSampleCatalog *catalog);

#endif
