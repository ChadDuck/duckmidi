#include "export_plugins.h"
#include <assert.h>
#include <stdio.h>
static unsigned artifacts;

static int32_t DME_CALL cancelled(void *u) {
    (void)u;
    return 0;
}

static void DME_CALL progress(void *u, uint32_t p) {
    (void)u;
    (void)p;
}

static void DME_CALL diagnostic(void *u, uint32_t s, uint32_t f, const char *m) {
    (void)u;
    (void)s;
    (void)f;
    assert(m);
}

static int32_t DME_CALL artifact(void *u, const char *name, const uint8_t *bytes, uint64_t size) {
    (void)u;
    assert(name && bytes && size);
    artifacts++;
    return 1;
}

int main(int argc, char **argv) {
    assert(argc == 2 && SDL_Init(0));
    DmExportRegistry registry;
    dm_export_registry_load(&registry, argv[1]);
    assert(registry.count == 1 && !*registry.errors);
    DmExportFormat *format = &registry.formats[0];
    const DmeExporter *api = format->api;
    assert(!strcmp(api->name, "M64"));
    DmeEvent events[] = {{0, 1, 0x90, 127, 100, 0}, {960, 2, 0x80, 127, 0, 0}};
    DmeContext context = {.events = events,
                          .event_count = 2,
                          .duration = 960,
                          .project_session = 1,
                          .ppq = 960,
                          .project_name = "Generated release fixture",
                          .source_name = ""};

    for (unsigned i = 0; i < 128; i++) {
        context.program_names[i] = "Generated program";
        context.drum_names[i] = "Generated drum";
    }

    char error[512] = {0};
    assert(api->inspect(format->instance, &context, error, sizeof(error)) == DME_OK);
    void *job = api->create_job(format->instance, error, sizeof(error));
    assert(job);
    DmeHost host = {NULL, cancelled, progress, diagnostic, artifact};
    events[0].port = 1;
    assert(api->preflight(job, &context, &host, error, sizeof(error)) == DME_INVALID &&
           strstr(error, "ports") && artifacts == 0);
    events[0].port = 0;
    error[0] = 0;
    assert(api->preflight(job, &context, &host, error, sizeof(error)) == DME_OK && artifacts == 0);
    DmeReview review = {0};
    assert(api->review(job, &review) == DME_OK &&
           dm_export_review_valid(&review, error, sizeof(error)) && review.count == 1);
    assert(review.rows[0].recommended_count && review.rows[0].supported);
    int substitute = review.rows[0].value;
    assert(api->review_set(job, review.rows[0].id, 0, error, sizeof(error)) == DME_OK);
    assert(api->review(job, &review) == DME_OK && !review.rows[0].supported);
    assert(api->review_set(job, review.rows[0].id, substitute, error, sizeof(error)) == DME_OK);
    assert(api->review_apply(format->instance, job, error, sizeof(error)) == DME_OK);
    assert(api->run(job, &context, &host, error, sizeof(error)) == DME_OK && artifacts == 4);
    api->destroy_job(job);
    dm_export_registry_free(&registry);
    SDL_Quit();
    puts(
        "PASS delivered m64.dll discovery, ABI3 preflight without artifacts, structured supported/manual review, apply and four-artifact conversion");
    return 0;
}
