#include "../src/duck.h"
#include "m64export/m64export.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int main(void) {
    const char *paths[] = {"samples/imp_night.mid", "samples/e2m1.mus", "samples/D_46_D15.mid",
                           "samples/d_01_51.mus"};
    const M64Api *api = m64_get_api_3();

    for (unsigned f = 0; f < 4; f++) {
        DmProject p;
        dm_project_init(&p);
        DmBytes file = {0};
        char error[512] = {0};

        if (!dm_read(paths[f], &file, error, sizeof(error)) ||
            !dm_import(&p, file.data, file.size, 0, 0, error, sizeof(error))) {
            printf("IMPORT %s: %s\n", paths[f], error);
            return 1;
        }

        DmExportEvent *timeline = NULL;
        size_t count = 0;
        uint64_t duration = 0;

        if (!dm_export_timeline(&p, &timeline, &count, &duration, error, sizeof(error))) {
            puts(error);
            return 1;
        }

        M64Event *events = calloc(count, sizeof(*events));

        for (size_t i = 0; i < count; i++) {
            events[i] = (M64Event){timeline[i].tick, timeline[i].source_id, timeline[i].status,
                                   timeline[i].a, timeline[i].b};
        }

        char path[256];
        snprintf(path, sizeof(path), "agents/reports/m64export/fidelity/sample-%u.source.json", f);
        FILE *source = fopen(path, "wb");
        assert(source);
        fprintf(source, "{\"ppq\":%u,\"duration\":%llu,\"events\":[", DM_PPQ,
                (unsigned long long)duration);

        for (size_t i = 0; i < count; i++) {
            fprintf(source, "%s[%llu,%u,%u,%u]", i ? "," : "", (unsigned long long)events[i].tick,
                    events[i].status, events[i].a, events[i].b);
        }

        assert(fputs("]}", source) >= 0);
        assert(!fclose(source));

        for (unsigned limit = 16; limit <= 64; limit *= 2) {
            M64Request q = {0};
            q.events = events;
            q.event_count = count;
            q.duration = duration;
            q.ppq = DM_PPQ;
            q.target = 1;
            q.voice_limit = limit;
            q.sequence_name = "03_custom_test";
            q.bank_name = "26_custom_test";
            M64Result *r = NULL;
            int status = api->compile(&q, NULL, &r);
            printf("%s limit=%u events=%zu status=%d\n", paths[f], limit, count, status);
            assert(status == M64_OK);

            for (uint64_t i = 0; i < api->diagnostic_count(r); i++) {
                M64DiagnosticView d;
                api->diagnostic_at(r, i, &d);
                printf("  %u: %s (tick %llu ch %d)\n", d.severity, d.message,
                       (unsigned long long)d.tick, d.channel);
            }

            if (status == 0) {
                M64ArtifactView a;
                api->artifact_at(r, 0, &a);

                if (f == 3) {
                    assert(((a.size + 15) & ~15ULL) + 32 <= 0x4400);
                }

                printf("  sequence bytes=%llu\n", (unsigned long long)a.size);
            }

            if (limit == 16) {
                const char *extensions[] = {"m64", "bank.json", "entry.json", "manifest.json"};

                for (unsigned kind = 0; kind < M64_ARTIFACT_COUNT; kind++) {
                    M64ArtifactView a;
                    assert(api->artifact_at(r, kind, &a) == M64_OK);
                    snprintf(path, sizeof(path), "agents/reports/m64export/fidelity/sample-%u.%s",
                             f, extensions[kind]);
                    assert(dm_write(path, a.data, (size_t)a.size, error, sizeof(error)));
                }
            }

            api->result_free(r);

            if (f == 3 && limit == 16) {
                q.loop = 1;
                r = NULL;
                assert(api->compile(&q, NULL, &r) == M64_OK);
                M64ArtifactView a;
                assert(api->artifact_at(r, 0, &a) == M64_OK);
                assert(((a.size + 15) & ~15ULL) + 32 <= 0x4400);
                assert(dm_write("agents/reports/m64export/fidelity/sample-3.loop.m64", a.data,
                                (size_t)a.size, error, sizeof(error)));
                api->result_free(r);
                puts("PASS d_01_51 looped export fits padded stock US pool");
            }
        }

        free(events);
        free(timeline);
        dm_bytes_free(&file);
        dm_project_free(&p);
    }

    return 0;
}
