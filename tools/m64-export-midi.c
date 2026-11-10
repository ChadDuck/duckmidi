/* Exports a MIDI file through an installed M64 or OoT plugin, the way the editor does, and writes
   every artifact into an output folder. Diagnostics go to stdout. Usage: m64_export_midi <plugins
   dir> <file.mid> <output dir> [oot] [hackersm64] [loop] [stock] */
#include "core/duck.h"
#include "export_plugins.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *output;

static int32_t DME_CALL cancelled(void *user) {
    (void)user;
    return 0;
}

static void DME_CALL progress(void *user, uint32_t percent) {
    (void)user;
    (void)percent;
}

static void DME_CALL diagnostic(void *user, uint32_t severity, uint32_t flags,
                                const char *message) {
    (void)user;
    printf("[%s%s] %s\n",
           severity == DME_ERROR ? "error"
           : severity            ? "warning"
                                 : "info",
           flags & DME_ALTERED ? ", altered" : "", message);
}

static int32_t DME_CALL artifact(void *user, const char *name, const uint8_t *data, uint64_t size) {
    (void)user;
    char path[DM_PATH];
    const char *leaf = strrchr(name, '/');
    snprintf(path, sizeof(path), "%s/%s", output, leaf ? leaf + 1 : name);
    FILE *file = fopen(path, "wb");

    if (!file || fwrite(data, 1, (size_t)size, file) != size) {
        return 0;
    }

    fclose(file);
    printf("wrote %s (%llu bytes)\n", path, (unsigned long long)size);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 4 || !SDL_Init(0)) {
        fprintf(stderr, "usage: m64_export_midi <plugins dir> <file.mid> <output dir> [target]\n");
        return 2;
    }

    output = argv[3];
    SDL_CreateDirectory(output);
    DmBytes file = {0};
    char error[4096] = {0};
    DmProject p;
    dm_project_init(&p);

    if (!dm_read(argv[2], &file, error, sizeof(error)) ||
        !dm_import(&p, file.data, file.size, -1, 0, error, sizeof(error))) {
        fprintf(stderr, "import: %s\n", error);
        return 1;
    }

    DmExportEvent *timeline = NULL;
    size_t count = 0;
    uint64_t duration = 0;

    if (!dm_export_timeline(&p, &timeline, &count, &duration, error, sizeof(error))) {
        fprintf(stderr, "timeline: %s\n", error);
        return 1;
    }

    DmeEvent *events = calloc(count ? count : 1, sizeof(*events));
    unsigned authored = dm_export_authored_pan(&p);

    for (size_t i = 0; i < count; i++) {
        const DmExportEvent *e = &timeline[i];
        bool generated = (e->flags & DM_EXPORT_DEFAULT_CONTROL) &&
                         !(authored & (1u << (e->status & MIDI_CHANNEL_MASK)));
        events[i] = (DmeEvent){e->tick,
                               e->source_id,
                               e->status,
                               e->a,
                               e->b,
                               e->port,
                               generated ? DME_EVENT_DEFAULT_CONTROL : 0};
    }

    DmeContext context = {.events = events,
                          .event_count = count,
                          .duration = duration,
                          .project_session = 1,
                          .ppq = DM_PPQ,
                          .project_name = "probe",
                          .source_name = argv[2]};

    for (int c = 0; c < 16; c++) {
        context.percussion_channels |= dm_channel_drums(&p, c) ? 1u << c : 0;
    }

    for (unsigned i = 0; i < 128; i++) {
        context.program_names[i] = dm_program_name((int)i);
        context.drum_names[i] = dm_drum_name((int)i);
    }

    double start, end;

    if (dm_loop_points(&p, &start, &end)) {
        if (start >= 0) {
            context.loop_start = (uint64_t)fmin(duration, llround(start * DM_PPQ));
            context.loop_points |= DME_LOOP_START;
        }

        if (end >= 0) {
            context.loop_end = (uint64_t)fmin(duration, llround(end * DM_PPQ));
            context.loop_points |= DME_LOOP_END;
        }
    }

    printf("events %zu duration %llu loop %u %llu-%llu\n", count, (unsigned long long)duration,
           context.loop_points, (unsigned long long)context.loop_start,
           (unsigned long long)context.loop_end);
    DmExportRegistry registry;
    dm_export_registry_load(&registry, argv[1]);
    DmExportFormat *format = NULL;

    for (size_t i = 0; i < registry.count; i++) {
        const char *wanted = "org.duckmidi.sm64";

        for (int k = 4; k < argc; k++) {
            wanted = !strcmp(argv[k], "oot") ? "org.duckmidi.oot" : wanted;
        }

        if (!strcmp(registry.formats[i].api->id, wanted)) {
            format = &registry.formats[i];
        }
    }

    if (!format) {
        fprintf(stderr, "no M64 plugin in %s: %s\n", argv[1], registry.errors);
        return 1;
    }

    const DmeExporter *api = format->api;
    DmeHost host = {.cancelled = cancelled,
                    .progress = progress,
                    .diagnostic = diagnostic,
                    .artifact = artifact};
    void *job = NULL;
    int32_t status = api->inspect(format->instance, &context, error, sizeof(error));

    for (int i = 4; status == DME_OK && i < argc; i++) {
        const char *id = !strcmp(argv[i], "hackersm64") ? "target"
                         : !strcmp(argv[i], "loop")     ? "loop"
                         : !strcmp(argv[i], "stock")    ? "memory"
                                                        : NULL;
        DmeValue value = {!strcmp(argv[i], "loop") ? DME_BOOL : DME_CHOICE, 1, NULL};
        status = !strcmp(argv[i], "oot") ? DME_OK
                 : id ? api->set(format->instance, id, &value, error, sizeof(error))
                      : DME_INVALID;
    }

    if (status == DME_OK) {
        job = api->create_job(format->instance, error, sizeof(error));
        status = job ? api->preflight(job, &context, &host, error, sizeof(error)) : DME_FAILED;
    }

    if (status == DME_OK) {
        status = api->review_apply(format->instance, job, error, sizeof(error));
    }

    if (status == DME_OK) {
        status = api->run(job, &context, &host, error, sizeof(error));
    }

    printf("status %d %s\n", status, error);

    if (job) {
        api->destroy_job(job);
    }

    dm_export_registry_free(&registry);
    free(events);
    free(timeline);
    dm_bytes_free(&file);
    dm_project_free(&p);
    SDL_Quit();
    return status == DME_OK ? 0 : 1;
}
