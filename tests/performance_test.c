#include "duck.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double milliseconds(clock_t start) {
    return (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
}
static void require(bool ok) {
    if (!ok) {
        fprintf(stderr, "Performance fixture failed\n");
        exit(1);
    }
}
int main(void) {
    DmProject p, imported;
    dm_project_init(&p);
    dm_project_init(&imported);
    for (int channel = 0; channel < 16; ++channel) {
        p.channels[channel].enabled = true;
        int id = dm_pattern_add(&p, "Generated load");
        require(id >= 0);
        for (int i = 0; i < 6250; ++i) {
            require(
                dm_note_add(&p.patterns[id], (DmNote){i * .25, .2, (uint8_t)(48 + i % 24), 90, 0}));
        }
        dm_pattern_fit(&p, &p.patterns[id]);
        require(dm_clip_add(&p, (DmClip){0, id, channel, 0, 0, 0}));
    }
    char error[256] = {0};
    DmSchedule schedule = {0};
    DmBytes midi = {0}, project = {0};
    clock_t start = clock();
    require(dm_schedule(&p, &schedule));
    printf("schedule_100000_notes_ms=%.2f events=%zu\n", milliseconds(start), schedule.count);
    start = clock();
    require(dm_midi(&p, &midi, error, sizeof(error)));
    printf("midi_export_100000_notes_ms=%.2f bytes=%zu\n", milliseconds(start), midi.size);
    start = clock();
    require(dm_import(&imported, midi.data, midi.size, 0, 0, error, sizeof(error)));
    printf("midi_import_100000_notes_ms=%.2f\n", milliseconds(start));
    start = clock();
    require(dm_pack(&p, &project));
    printf("project_pack_100000_notes_ms=%.2f bytes=%zu\n", milliseconds(start), project.size);
    dm_schedule_free(&schedule);
    dm_bytes_free(&midi);
    dm_bytes_free(&project);
    dm_project_free(&p);
    dm_project_free(&imported);
    return 0;
}
