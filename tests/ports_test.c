#include "duck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks;
static void check(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL ports: %s\n", text);
        exit(1);
    }
}
static size_t notes(const DmProject *p) {
    size_t n = 0;
    for (size_t i = 0; i < p->pattern_count; ++i) {
        n += p->patterns[i].count;
    }
    return n;
}
static int ed_channels(const DmProject *p) {
    int n = 0;
    for (int c = 0; c < 16; ++c) {
        n += p->channels[c].enabled;
    }
    return n;
}
static DmBytes fixture(bool collision, int format) {
    uint8_t head[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, (uint8_t)format, 0, 2, 0, 96};
    uint8_t track[] = {'M', 'T', 'r', 'k', 0,  0,   0,  20,  0,  255, 33, 1,   1,  0,
                       192, 0,   0,   144, 60, 100, 96, 128, 60, 0,   0,  255, 47, 0};
    DmBytes b = {0};
    check(dm_bytes(&b, head, sizeof(head)), "fixture header");
    for (int i = 0; i < 2; ++i) {
        int c = collision ? 0 : i;
        track[12] = i + 1;
        track[14] = 192 | c;
        track[15] = i * 40;
        track[17] = 144 | c;
        track[18] = 60 + i * 7;
        track[21] = 128 | c;
        track[22] = track[18];
        check(dm_bytes(&b, track, sizeof(track)), "fixture track");
    }
    return b;
}
int main(void) {
    DmProject p, q, r;
    dm_project_init(&p);
    dm_project_init(&q);
    dm_project_init(&r);
    DmBytes b = {0}, out = {0}, packed = {0};
    char error[512] = {0};
    check(dm_read("samples/stalks.mid", &b, error, sizeof(error)), error);
    check(dm_import(&p, b.data, b.size, -1, 0, error, sizeof(error)), error);
    printf("stalks original: ports=%d sequences=%d default-port=%d notes=%zu channels=%d\n",
           p.port_count, p.sequence_count, p.port, notes(&p), ed_channels(&p));
    check(p.sequence_count == 1 && p.port_count == 7 && notes(&p) == 1484 && ed_channels(&p) == 1,
          "original file routing explains isolated percussion import");
    check(dm_import(&q, b.data, b.size, DM_PORT_COMBINED, 0, error, sizeof(error)), error);
    check(q.port == DM_PORT_COMBINED && notes(&q) == 4119 && ed_channels(&q) == 7,
          "all seven musical channels combine");
    check(q.original.size == b.size && !memcmp(q.original.data, b.data, b.size),
          "original MIDI retained byte for byte");
    for (int c = 0; c < 6; ++c) {
        check(q.channels[c].enabled, "melodic channel enabled");
    }
    check(q.channels[9].enabled, "percussion channel enabled");
    for (int all = 0; all < 2; ++all) {
        check(all ? dm_midi_all(&q, &out, error, sizeof(error))
                  : dm_midi(&q, &out, error, sizeof(error)),
              error);
        if (all) {
            check(dm_write("agents/reports/stalks-combined.mid", out.data, out.size, error,
                           sizeof(error)),
                  error);
        }
        check(dm_import(&r, out.data, out.size, -1, 0, error, sizeof(error)), error);
        check(r.port_count == 1 && r.port == 0 && notes(&r) == 4119 && ed_channels(&r) == 7,
              "MIDI export roundtrip exposes the complete song on one port");
        dm_bytes_free(&out);
    }
    check(dm_pack(&q, &packed) && dm_unpack(&r, packed.data, packed.size, error, sizeof(error)),
          error);
    check(r.port == DM_PORT_COMBINED && notes(&r) == 4119 &&
              !memcmp(r.original.data, b.data, b.size),
          "combined project roundtrip retains mode, notes and original");
    check(!dm_view_switch(&r, 27, 0, error, sizeof(error)),
          "combined projects cannot mix conflicting individual-port view histories");
    dm_bytes_free(&packed);
    dm_bytes_free(&b);
    b = fixture(true, 1);
    check(dm_write("agents/reports/ports-collision.mid", b.data, b.size, error, sizeof(error)),
          error);
    size_t before = notes(&q);
    check(!dm_import(&q, b.data, b.size, DM_PORT_COMBINED, 0, error, sizeof(error)),
          "colliding channels are rejected");
    check(strstr(error, "channel 1") && strstr(error, "ports 1 and 2"),
          "collision explains exact channel and ports");
    check(notes(&q) == before && q.port == DM_PORT_COMBINED,
          "failed combine leaves destination intact");
    dm_bytes_free(&b);
    b = fixture(false, 1);
    check(dm_import(&p, b.data, b.size, DM_PORT_COMBINED, 0, error, sizeof(error)) &&
              notes(&p) == 2 && ed_channels(&p) == 2,
          "disjoint synthetic ports combine");
    check(p.channels[0].program == 0 && p.channels[1].program == 40,
          "programs stay on their original channels");
    dm_bytes_free(&b);
    b = fixture(true, 2);
    check(dm_import(&p, b.data, b.size, DM_PORT_COMBINED, 0, error, sizeof(error)) &&
              notes(&p) == 1,
          "independent sequences may reuse channels");
    p.patterns[0].notes[0].key = 65;
    check(dm_view_switch(&p, DM_PORT_COMBINED, 1, error, sizeof(error)) && notes(&p) == 1,
          "combined sequence switching works");
    check(dm_view_switch(&p, DM_PORT_COMBINED, 0, error, sizeof(error)) &&
              p.patterns[0].notes[0].key == 65,
          "combined sequence edits survive switching");
    check(dm_pack(&p, &packed) && dm_unpack(&q, packed.data, packed.size, error, sizeof(error)),
          error);
    check(dm_midi_all(&q, &out, error, sizeof(error)) &&
              dm_import(&r, out.data, out.size, -1, 0, error, sizeof(error)),
          error);
    check(r.sequence_count == 2 && notes(&r) == 1 && r.patterns[0].notes[0].key == 65,
          "multi-sequence combined export retains edits without duplicates");
    printf("PASS ports: %d checks; stalks combines 4119 notes across 7 channels\n", checks);
    dm_project_free(&p);
    dm_project_free(&q);
    dm_project_free(&r);
    dm_bytes_free(&b);
    dm_bytes_free(&packed);
    dm_bytes_free(&out);
    return 0;
}
