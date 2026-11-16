#include "package.h"
#include "internal.h"
#include "../../src/text/en.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool es_package_plan(EsRepository *probe, const EsModifications *mods, const DmeHost *host,
                     char *error, size_t cap) {
    EsPlan plan = {0};
    bool ok = es_render(probe, mods, &plan, error, cap);
    char *manifest = NULL;
    size_t used = 0, capacity = 0;

    for (size_t i = 0; ok && i < plan.count; i++) {
        const EsWrite *write = &plan.writes[i];
        EsFile baseline = {0};
        ok = es_repository_baseline(probe, write->path, &baseline, error, cap);
        uint8_t digest[32];
        es_sha256(baseline.data, baseline.size, digest);
        char hash[65];

        for (unsigned b = 0; b < 32; b++) {
            snprintf(hash + b * 2, 3, "%02x", digest[b]);
        }

        size_t length = strlen(write->path) + 96;
        ok = ok && es_reserve((void **)&manifest, &capacity, used + length + 1, 1);

        if (ok) {
            int n = snprintf(manifest + used, capacity - used, "%s %s %s\n",
                             write->file.exists ? "WRITE" : "REMOVE",
                             baseline.exists ? hash : "ABSENT", write->path);
            ok = n >= 0 && (size_t)n < capacity - used;
            used += ok ? (size_t)n : 0;
        }

        if (ok && write->file.exists) {
            ok = host->artifact(host->user, write->path, write->file.data, write->file.size) != 0;
        }

        es_file_free(&baseline);
    }

    for (size_t i = 0; ok && i < plan.backups.count; i++) {
        const char *path = plan.backups.items[i];
        size_t length = strlen(path) * 2 + 32;
        ok = es_reserve((void **)&manifest, &capacity, used + length + 1, 1);

        if (ok) {
            int n = snprintf(manifest + used, capacity - used, "BACKUP %s -> %s.bkp\n", path, path);
            ok = n >= 0 && (size_t)n < capacity - used;
            used += ok ? (size_t)n : 0;
        }
    }

    ok = ok && host->artifact(host->user, "repository-plan.txt", (const uint8_t *)manifest, used) &&
         host->artifact(host->user, "INSTALL.txt",
                        (const uint8_t *)DM_TEXT_NATIVE_REPLACE_PACKAGE_GUIDE,
                        strlen(DM_TEXT_NATIVE_REPLACE_PACKAGE_GUIDE));
    free(manifest);
    es_plan_free(&plan);
    return ok;
}
