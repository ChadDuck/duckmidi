#ifndef DM_SAMPLE_EXPORT_CATALOG_H
#define DM_SAMPLE_EXPORT_CATALOG_H

#include "library.h"

typedef struct DmSampleExportCatalog DmSampleExportCatalog;

/* Uses the same program/user roots as Samples without creating an editor/tool instance. */
bool dm_sample_library_roots(const char *base, const char *settings, char *program,
                              size_t program_cap, char *writable, size_t writable_cap);
bool dm_sample_export_catalog(const char *program, const char *writable, const DmeHost *host,
                               DmSampleExportCatalog **out, char *error, size_t cap);
const DmeSampleCatalog *dm_sample_export_catalog_view(const DmSampleExportCatalog *);
bool dm_sample_export_catalog_equal(const DmSampleExportCatalog *, const DmSampleExportCatalog *);
void dm_sample_export_catalog_free(DmSampleExportCatalog *);

#endif
