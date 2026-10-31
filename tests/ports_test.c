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

static size_t port_notes(const DmProject *p, unsigned port) {
    size_t n = 0;

    for (size_t i = 0; i < p->pattern_count; ++i) {
        for (size_t j = 0; j < p->patterns[i].count; ++j) {
            n += p->patterns[i].notes[j].port == port;
        }
    }

    return n;
}

int main(void) {
    DmProject p, q, r;
    dm_project_init(&p);
    dm_project_init(&q);
    dm_project_init(&r);
    DmBytes b = {0}, out = {0}, packed = {0};
    char error[512] = {0};

    /* Two ports on separate channels open as one arrangement, one note port per source port. */
    b = fixture(false, 1);
    check(dm_import(&p, b.data, b.size, -1, 0, error, sizeof(error)), error);
    check(p.port == DM_PORT_COMBINED && p.port_count == 2 && notes(&p) == 2 &&
              port_notes(&p, 0) == 1 && port_notes(&p, 1) == 1 && ed_channels(&p) == 2,
          "disjoint ports import together on their own note ports");
    check(p.channels[0].program == 0 && p.channels[1].program == 40,
          "programs stay on their original channels");
    check(p.original.size == b.size && !memcmp(p.original.data, b.data, b.size),
          "original MIDI retained byte for byte");
    dm_bytes_free(&b);

    /* Ports sharing a channel are no longer rejected; each keeps its notes and program. */
    b = fixture(true, 1);
    check(dm_import(&q, b.data, b.size, -1, 0, error, sizeof(error)), error);
    check(q.port == DM_PORT_COMBINED && notes(&q) == 2 && port_notes(&q, 1) == 1 &&
              ed_channels(&q) == 1 && q.clip_count == 1,
          "ports sharing channel 1 share its placement on separate note ports");
    check(dm_port_mix(&q, 1, 0).program == 41, "the second port keeps its own program");
    check(dm_write("agents/reports/2026-10-30-run/ports-shared-channel.mid", b.data, b.size, error,
                   sizeof(error)),
          error);

    /* Exports keep the ports; reopening shows the same split. */
    check(dm_midi(&q, &out, error, sizeof(error)), error);
    check(dm_import(&r, out.data, out.size, -1, 0, error, sizeof(error)), error);
    check(r.port_count == 2 && port_notes(&r, 0) == 1 && port_notes(&r, 1) == 1,
          "MIDI export roundtrip keeps both ports");
    dm_bytes_free(&out);
    check(dm_pack(&q, &packed) && dm_unpack(&r, packed.data, packed.size, error, sizeof(error)),
          error);
    check(r.port == DM_PORT_COMBINED && port_notes(&r, 1) == 1 &&
              dm_port_mix(&r, 1, 0).program == 41 && !memcmp(r.original.data, b.data, b.size),
          "project roundtrip keeps ports, programs and the original");
    dm_bytes_free(&packed);

    /* An explicit port still opens that port alone. */
    check(dm_import(&r, b.data, b.size, 2, 0, error, sizeof(error)) && notes(&r) == 1 &&
              r.port == 2,
          "explicit port import");
    dm_bytes_free(&b);

    /* Format 2 sequences keep separate views. */
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
          "multi-sequence export retains edits without duplicates");
    /* MIDI files are exported as the arrangement sounds: a muted channel is left out. */
    b = fixture(false, 1);
    check(dm_import(&p, b.data, b.size, -1, 0, error, sizeof(error)), error);
    p.channels[1].mute = true;
    check(dm_midi_export(&p, &out, true, DM_METADATA_STRIP_ALL, error, sizeof(error)), error);
    check(dm_import(&r, out.data, out.size, -1, 0, error, sizeof(error)) && notes(&r) == 1 &&
              !r.channels[1].enabled,
          "muted channels are left out of MIDI exports");
    dm_bytes_free(&out);
    dm_bytes_free(&b);
    printf("PASS ports: %d checks\n", checks);
    dm_project_free(&p);
    dm_project_free(&q);
    dm_project_free(&r);
    dm_bytes_free(&b);
    dm_bytes_free(&packed);
    dm_bytes_free(&out);
    return 0;
}
