#include "duck.h"
#include <stdio.h>
#include <stdlib.h>

static void check(bool ok, const char *ed_message) {
    if (!ok) {
        fprintf(stderr, "FAIL sample: %s\n", ed_message);
        exit(1);
    }
}

static void sample(const char *path, const char *output, bool trace) {
    DmBytes input = {0}, midi = {0};
    DmProject p, next;
    char error[512] = {0};
    dm_project_init(&p);
    dm_project_init(&next);
    check(dm_read(path, &input, error, sizeof(error)), error);
    check(dm_import(&p, input.data, input.size, -1, 0, error, sizeof(error)), error);
    check(dm_midi_all(&p, &midi, error, sizeof(error)), error);
    check(dm_write(output, midi.data, midi.size, error, sizeof(error)), error);
    check(dm_import(&next, midi.data, midi.size, -1, 0, error, sizeof(error)), error);
    size_t count = 0, again = 0;

    for (size_t i = 0; i < p.pattern_count; ++i) {
        count += p.patterns[i].count;
    }

    for (size_t i = 0; i < next.pattern_count; ++i) {
        again += next.patterns[i].count;
    }

    check(count == again && count > 0, "note count round trip");
    printf("%s: %zu notes, %zu automation, %.3f seconds, MIDI roundtrip passed\n", path, count,
           p.automation_count, dm_beat_seconds(&p, dm_duration(&p)));

    if (trace) {
        DmSchedule schedule = {0};
        check(dm_schedule(&p, &schedule), "schedule");
        FILE *f = fopen("agents/reports/dream-schedule.csv", "w");
        check(f != NULL, "trace output");

        for (size_t i = 0; i < schedule.count; ++i) {
            DmScheduled *s = &schedule.items[i];
            fprintf(f, "%.9f,%u,%u,%u\n", s->seconds, s->status, s->a, s->b);
        }

        fclose(f);
        dm_schedule_free(&schedule);
    }

    dm_bytes_free(&input);
    dm_bytes_free(&midi);
    dm_project_free(&p);
    dm_project_free(&next);
}

int main(void) {
    sample("samples/dream_express.mid", "agents/reports/dream-reconstructed.mid", true);
    sample("samples/e2m1.mus", "agents/reports/e2m1-converted.mid", false);
    return 0;
}
