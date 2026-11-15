/* Loads generated schema-9 through schema-18 projects and checks their conversion to the current
   schema. The writers below follow those layouts field by field; they exist only to produce these
   fixtures. */
#include "core/duck.h"
#include "text/en.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TEST_NAME "project compat"
#include "test_common.h"

static void u32(DmBytes *b, uint32_t n) {
    uint8_t v[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    check(dm_bytes(b, v, 4), "fixture memory");
}

static void real(DmBytes *b, double n) {
    uint64_t bits;
    memcpy(&bits, &n, 8);
    u32(b, (uint32_t)bits);
    u32(b, (uint32_t)(bits >> 32));
}

static void text(DmBytes *b, const char *s) {
    u32(b, (uint32_t)strlen(s));
    check(dm_bytes(b, s, strlen(s)), "fixture memory");
}

static uint32_t crc32(const uint8_t *p, size_t size) {
    uint32_t crc = ~0u;

    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];

        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }

    return ~crc;
}

/* One pattern with a note and modulation point on port 2, one placement on channel 1, a port-2
   mixer entry and a program change event that schema 9 stored with source port 3. */
static DmBytes schema_9_project(void) {
    DmBytes body = {0}, file = {0};
    text(&body, "Old song");
    text(&body, "");
    real(&body, 120);
    u32(&body, 4);
    u32(&body, 4);
    u32(&body, 3); /* Source port. */
    u32(&body, 0); /* Sequence. */
    u32(&body, 1); /* Source format. */
    u32(&body, 1); /* Sequence count. */
    u32(&body, 1); /* Port count. */
    u32(&body, 3); /* Ports. */
    u32(&body, 0); /* Percussion override. */

    for (int i = 0; i < 128; ++i) {
        u32(&body, 0);
    }

    for (int c = 0; c < 16; ++c) {
        u32(&body, c == 0);
        u32(&body, 0);
        u32(&body, 0);
        u32(&body, c == 0 ? 5 : UINT32_MAX);
        u32(&body, 100);
        u32(&body, 64);
    }

    for (int c = 0; c < 16; ++c) {
        u32(&body, 0);
    }

    u32(&body, 0); /* Original MIDI block. */
    u32(&body, 1); /* Next pattern uid. */
    u32(&body, 1); /* Pattern count. */
    u32(&body, 1);
    text(&body, "Pattern 1");
    text(&body, "Pattern 1");
    u32(&body, 1);
    real(&body, 4);
    u32(&body, 1); /* Note count. */
    u32(&body, 0); /* Kind, mode and event. */
    u32(&body, 0);
    u32(&body, (uint32_t)-2);
    real(&body, 1);
    real(&body, 0);
    real(&body, 0);
    real(&body, .5);
    real(&body, .5);
    real(&body, 64);
    real(&body, 60);
    u32(&body, 60);
    u32(&body, 0);
    u32(&body, 0); /* Control scope. */
    u32(&body, 0); /* Pattern port; schema 10 adds the color after it. */
    real(&body, 0);
    real(&body, 1);
    u32(&body, 60);
    u32(&body, 100);
    u32(&body, 0);
    u32(&body, 2); /* Note port. */
    u32(&body, 1); /* Placement; schema 10 adds the offset after the lane. */
    real(&body, 0);
    u32(&body, 0);
    u32(&body, 0);
    u32(&body, 0);
    real(&body, 0);
    u32(&body, 0);
    u32(&body, 1); /* Automation count. */
    real(&body, 0);
    u32(&body, MIDI_CONTROL);
    u32(&body, 0); /* Channel. */
    u32(&body, MIDI_CC_MODULATION);
    u32(&body, 64); /* Value. */
    u32(&body, 0);  /* Pattern. */
    u32(&body, 1);  /* Curve. */
    u32(&body, 60); /* Anchor. */
    real(&body, 0); /* Bend. */
    u32(&body, 0);  /* Ramp. */
    u32(&body, 2);  /* Note port. */
    u32(&body, 1);  /* Event: program change with source port 3. */
    real(&body, 0);
    u32(&body, 1);
    u32(&body, 3);
    u32(&body, 2);
    uint8_t program[] = {0xc0, 5};
    check(dm_bytes(&body, program, 2), "fixture memory");
    u32(&body, 0); /* Views. */
    u32(&body, 1); /* Port mixer; schema 10 adds a program after solo. */
    u32(&body, 2);
    u32(&body, 0);
    u32(&body, 90);
    u32(&body, 30);
    u32(&body, 0);
    u32(&body, 0);

    check(dm_bytes(&file, "DUCKMIDI", 8), "fixture memory");
    u32(&file, 9);
    u32(&file, (uint32_t)body.size);
    u32(&file, crc32(body.data, body.size));
    check(dm_bytes(&file, body.data, body.size), "fixture memory");
    dm_bytes_free(&body);
    return file;
}

/* Schema 10 writer: variable-length integers, compact reals and time deltas, compressed body. */
static void v10(DmBytes *b, uint64_t n) {
    do {
        uint8_t byte = (uint8_t)((n & 127) | (n > 127 ? 128 : 0));
        check(dm_bytes(b, &byte, 1), "fixture memory");
        n >>= 7;
    } while (n);
}

/* Exact multiples of 1/3840 beat, stored as zigzag steps shifted left once. */
static void r10(DmBytes *b, double n) {
    int64_t steps = llround(n * 3840.0);
    uint64_t zigzag = steps < 0 ? ((uint64_t)(-(steps + 1)) << 1) | 1 : (uint64_t)steps << 1;
    v10(b, zigzag << 1);
}

static void t10(DmBytes *b, const char *s) {
    v10(b, strlen(s));
    check(dm_bytes(b, s, strlen(s)), "fixture memory");
}

/* A schema-10 automation point; schema 11 adds the hold flag after the port. */
static void point10(DmBytes *b, uint32_t schema, double beat, double previous, int type, int a,
                    int value, int curve, int ramp) {
    r10(b, beat - previous); /* Time delta from the previous point. */
    v10(b, (uint32_t)type);
    v10(b, 0);
    v10(b, (uint32_t)a);
    v10(b, (uint32_t)value);
    v10(b, 0); /* Pattern. */
    v10(b, (uint32_t)curve);
    v10(b, 60); /* Anchor. */
    r10(b, 0);  /* Bend. */
    v10(b, (uint32_t)ramp);
    v10(b, 0); /* Port. */

    if (schema >= 11) {
        v10(b, 0); /* Hold. */
    }

    if (schema >= 17) {
        v10(b, type == MIDI_PITCH_BEND && curve > 0); /* Fixed pitch reference. */
        v10(b, 0);                                    /* Detached. */
    }

    if (schema >= 18) {
        v10(b, 0); /* Bend root units. */
    }
}

/* One pattern with one note, one placement and modulation data, saved as schema 10 through 18.
   The schema-15 fixture has a Graph resolution, discrete bars and a percussion mapping. */
static DmBytes line_project(uint32_t schema) {
    DmBytes body = {0}, stored = {0}, file = {0};
    t10(&body, "Line song");
    t10(&body, "");
    r10(&body, 120);
    v10(&body, 4);
    v10(&body, 4);
    v10(&body, 0);            /* Source port. */
    v10(&body, 0);            /* Sequence. */
    v10(&body, 1);            /* Source format. */
    v10(&body, 1);            /* Sequence count. */
    v10(&body, 1);            /* Port count. */
    v10(&body, 0);            /* Ports. */
    v10(&body, schema >= 15); /* Percussion override. */

    for (int i = 0; i < 128; ++i) {
        v10(&body, 0);
    }

    if (schema >= 15) {
        for (int i = 0; i < 128; ++i) {
            v10(&body, i == 36 ? 61 : 0);
        }
    }

    for (int c = 0; c < 16; ++c) {
        v10(&body, c == 0);
        v10(&body, 0);
        v10(&body, 0);
        v10(&body, c == 0 ? 0 : UINT32_MAX);
        v10(&body, 100);
        v10(&body, 64);

        if (schema >= 12) {
            v10(&body, c == 10);
        }
    }

    for (int c = 0; c < 16; ++c) {
        if (schema >= 19) {
            v10(&body, 1); /* Note rows. */
        }

        v10(&body, 0); /* Lane count. */
    }

    v10(&body, 0); /* Original MIDI block. */
    v10(&body, 2); /* Next pattern uid. */
    v10(&body, 1); /* Pattern count. */
    v10(&body, 1);
    t10(&body, "Pattern 1");
    t10(&body, "Pattern 1");
    v10(&body, 1);
    r10(&body, 4);
    v10(&body, 1); /* Note count. */
    v10(&body, 0); /* Kind, mode and event. */
    v10(&body, 0);
    v10(&body, (uint32_t)-2);
    r10(&body, 1);
    r10(&body, 0);
    r10(&body, 0);
    r10(&body, .5);
    r10(&body, .5);
    r10(&body, 64);
    r10(&body, 60);
    v10(&body, 60);
    v10(&body, 0);
    v10(&body, 0); /* Control scope. */
    v10(&body, 0); /* Pattern port. */
    v10(&body, 0); /* Pattern color. */

    if (schema >= 16) {
        v10(&body, 0); /* No Graph lanes. */
    } else if (schema == 15) {
        v10(&body, 32); /* Pattern-wide Graph resolution. */
    }

    r10(&body, 0); /* Note. */
    r10(&body, 4);
    v10(&body, schema == 17 ? 57 : 60);
    v10(&body, 100);
    v10(&body, 0);
    v10(&body, 0);

    if (schema >= 17) {
        v10(&body, 0); /* Note bend identity. */
    }

    v10(&body, 1); /* Placement. */
    r10(&body, 0);
    v10(&body, 0);
    v10(&body, 0);
    v10(&body, 0);
    r10(&body, 0);
    v10(&body, 0);
    r10(&body, 0); /* Offset. */

    if (schema >= 19) {
        v10(&body, 0); /* Row. */
    }

    if (schema >= 16) {
        v10(&body, 7);
        point10(&body, schema, 0, 0, MIDI_CONTROL, MIDI_CC_DATA, 12, 1, 0);
        point10(&body, schema, 0, 0, MIDI_PITCH_BEND, 0, schema == 17 ? 48 : 64, 1, 0);
        point10(&body, schema, 1, 0, MIDI_PITCH_BEND, 0, 90, 1, 0);
        point10(&body, schema, 2, 1, MIDI_CONTROL, MIDI_CC_DATA, 127, 2, 0);
        point10(&body, schema, 2, 2, MIDI_PITCH_BEND, 0, 64, 2, 0);
        point10(&body, schema, 3, 2, MIDI_PITCH_BEND, 0, 90, 2, 0);
        point10(&body, schema, 4, 3, MIDI_PITCH_BEND, 0, 64, 0, 0);
    } else {
        v10(&body, 4); /* Automation count. */
        int id = schema < 15 ? 1 : 0;
        point10(&body, schema, 0, 0, MIDI_CONTROL, MIDI_CC_MODULATION, 0, id, 0);
        point10(&body, schema, 2, 0, MIDI_CONTROL, MIDI_CC_MODULATION, 127, id, 0);
        point10(&body, schema, 2, 2, MIDI_CONTROL, MIDI_CC_MODULATION, 0, -id, 0);
        point10(&body, schema, 3, 2, MIDI_CONTROL, MIDI_CC_MODULATION, 40, 0, schema < 15);
    }
    v10(&body, 0); /* Events. */
    v10(&body, 0); /* Views. */
    v10(&body, 0); /* Port mixer. */

    if (schema >= 13) {
        v10(&body, 2); /* Patch replacements. */

        for (unsigned i = 0; i < 2; ++i) {
            v10(&body, i);   /* Port. */
            v10(&body, 0);   /* Channel. */
            v10(&body, 130); /* Bank. */
            v10(&body, 6);   /* Program. */
            v10(&body, 0);   /* Drums. */
            /* Schema 13 stored a sample revision; schema 14 marks a current reference. */
            v10(&body, i ? 0 : schema >= 14 ? 1 : 73);
            t10(&body, i ? "" : "generated/library");
            t10(&body, i ? "" : "generated-set");
            t10(&body, i ? "" : "generated-preset");
        }
    }

    check(dm_compress(body.data, body.size, &stored), "fixture memory");
    check(dm_bytes(&file, "DUCKMIDI", 8), "fixture memory");
    u32(&file, schema);
    u32(&file, (uint32_t)stored.size);
    u32(&file, crc32(stored.data, stored.size));
    check(dm_bytes(&file, stored.data, stored.size), "fixture memory");
    dm_bytes_free(&body);
    dm_bytes_free(&stored);
    return file;
}

static void be32(DmBytes *b, uint32_t value) {
    uint8_t bytes[] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                       (uint8_t)value};
    check(dm_bytes(b, bytes, sizeof(bytes)), "MIDI fixture bytes");
}

static void midi_length(DmBytes *b, uint32_t size) {
    uint8_t bytes[4];
    size_t start = sizeof(bytes);
    bytes[--start] = (uint8_t)(size & 127u);

    while ((size >>= 7)) {
        bytes[--start] = (uint8_t)((size & 127u) | 128u);
    }

    check(dm_bytes(b, bytes + start, sizeof(bytes) - start), "MIDI fixture length");
}

static DmBytes old_metadata_midi(const DmProject *p, const DmBytes *old) {
    DmBytes midi = {0}, result = {0}, track = {0};
    char error[256];
    check(dm_midi_export(p, &midi, false, DM_METADATA_DUCKMIDI_ONLY, error, sizeof(error)), error);
    check(midi.size > 46 && !memcmp(midi.data + 14, "MTrk", 4) && !midi.data[22] &&
              midi.data[23] == MIDI_META && midi.data[24] == MIDI_META_SEQUENCER,
          "fixture metadata leads track zero");
    uint32_t track_size = (uint32_t)midi.data[18] << 24 | (uint32_t)midi.data[19] << 16 |
                          (uint32_t)midi.data[20] << 8 | midi.data[21];
    size_t payload = 25;
    uint32_t previous_length = 0;

    do {
        check(payload < 29, "fixture metadata length");
        previous_length = (previous_length << 7) | (midi.data[payload] & 127u);
    } while (midi.data[payload++] & 128);

    size_t next = payload + previous_length;
    check(previous_length > 21 && next <= 22 + track_size && 22 + track_size <= midi.size,
          "fixture track bounds");
    check(dm_bytes(&track, midi.data + 22, 3), "fixture MIDI meta event");
    midi_length(&track, (uint32_t)old->size + 21);
    check(dm_bytes(&track, midi.data + payload, 21) && dm_bytes(&track, old->data, old->size) &&
              dm_bytes(&track, midi.data + next, 22 + track_size - next),
          "old fixture MIDI track");
    check(dm_bytes(&result, midi.data, 18), "old fixture MIDI header");
    be32(&result, (uint32_t)track.size);
    check(dm_bytes(&result, track.data, track.size) &&
              dm_bytes(&result, midi.data + 22 + track_size, midi.size - 22 - track_size),
          "old fixture MIDI contents");
    dm_bytes_free(&track);
    dm_bytes_free(&midi);
    return result;
}

static void check_patch_replacements(void) {
    DmProject p, q;
    dm_project_init(&p);
    dm_project_init(&q);
    char error[256] = {0};
    DmBytes previous = line_project(12);
    check(dm_unpack(&p, previous.data, previous.size, error, sizeof(error)), error);
    check(!p.patch_replacement_count && p.channels[10].drums && !p.channels[9].drums,
          "schema-12 projects retain their percussion mapping and have no replacements");
    DmBytes old_midi = old_metadata_midi(&p, &previous);
    check(dm_import(&q, old_midi.data, old_midi.size, -1, 0, error, sizeof(error)), error);
    check(!q.patch_replacement_count && q.channels[10].drums && q.pattern_count == 1,
          "embedded schema-12 project is converted on MIDI import");
    dm_bytes_free(&old_midi);
    dm_bytes_free(&previous);

    previous = line_project(13);
    check(dm_unpack(&q, previous.data, previous.size, error, sizeof(error)), error);
    check(q.patch_replacement_count == 2 && q.patch_replacements[0].generation == 1 &&
              !q.patch_replacements[1].generation &&
              !strcmp(q.patch_replacements[0].set_id, "generated-set") &&
              !strcmp(q.patch_replacements[0].preset_id, "generated-preset"),
          "schema-13 sample references become current references and retain SoundFont overrides");
    dm_bytes_free(&previous);

    DmBytes plain = {0}, after = {0}, packed = {0}, midi = {0};
    check(dm_midi_export(&p, &plain, false, DM_METADATA_STRIP_ALL, error, sizeof(error)), error);
    DmPatchReplacement channel = {.key = {.channel = 0, .bank = 130, .program = 0},
                                  .generation = 3,
                                  .locator = "test/sample-library",
                                  .set_id = "generated-set",
                                  .preset_id = "one"};
    check(dm_patch_replacement_set(&p, &channel), "add channel replacement");
    DmPatchKey other = channel.key;
    other.port = 2;
    const DmPatchReplacement *inherited = dm_patch_replacement(&p, other);
    check(inherited && inherited->generation == 3, "a note port inherits its channel replacement");
    DmPatchReplacement stock = {.key = other};
    check(dm_patch_replacement_set(&p, &stock) && !dm_patch_replacement(&p, other)->generation,
          "an explicit SoundFont port selection masks the channel replacement");
    DmPatchReplacement port = channel;
    port.key.port = 3;
    port.generation = 4;
    strcpy(port.preset_id, "two");
    check(dm_patch_replacement_set(&p, &port), "same original patch can have two routed samples");
    other.program = 1;
    check(!dm_patch_replacement(&p, other), "a different program uses the original SoundFont");
    check(dm_midi_export(&p, &after, false, DM_METADATA_STRIP_ALL, error, sizeof(error)) &&
              plain.size == after.size && !memcmp(plain.data, after.data, plain.size),
          "ordinary MIDI bytes remain identical");

    check(dm_project_copy(&q, &p) && q.patch_replacements != p.patch_replacements,
          "project copy owns independent patch replacements");
    dm_patch_replacement_clear(&q, stock.key);
    check(dm_patch_replacement(&q, stock.key)->generation == 3 && q.patch_replacement_count == 2,
          "clearing an override restores inheritance");
    check(dm_pack(&p, &packed) && packed.data[8] == DM_PROJECT_SCHEMA &&
              dm_unpack(&q, packed.data, packed.size, error, sizeof(error)),
          error);
    check(q.patch_replacement_count == 3 && dm_patch_replacement(&q, port.key)->generation == 1,
          "current project preserves independent routes");
    check(dm_midi_export(&p, &midi, true, DM_METADATA_DUCKMIDI_ONLY, error, sizeof(error)) &&
              dm_import(&q, midi.data, midi.size, -1, 0, error, sizeof(error)),
          error);
    const DmPatchReplacement *restored = dm_patch_replacement(&q, port.key);
    check(restored && restored->generation == 1 && !strcmp(restored->locator, port.locator) &&
              !strcmp(restored->preset_id, "two"),
          "editor MIDI metadata preserves a reference even without its library");
    dm_bytes_free(&plain);
    dm_bytes_free(&after);
    dm_bytes_free(&packed);
    dm_bytes_free(&midi);
    dm_project_free(&p);
    dm_project_free(&q);
}

static bool reloads(const DmProject *p) {
    DmProject q;
    DmBytes saved = {0};
    char error[256];
    dm_project_init(&q);
    bool ok = dm_pack(p, &saved) && dm_unpack(&q, saved.data, saved.size, error, sizeof(error));
    dm_bytes_free(&saved);
    dm_project_free(&q);
    return ok;
}

/* Schema 14 to 15: patterns gain the graph resolution and projects the percussion MIDI mapping. */
static void check_graph_and_percussion_midi(void) {
    DmProject p, q;
    dm_project_init(&p);
    dm_project_init(&q);
    char error[256] = {0};
    uint8_t none[128] = {0};
    DmBytes previous = line_project(14), saved = {0};
    check(dm_unpack(&p, previous.data, previous.size, error, sizeof(error)), error);
    check(p.pattern_count == 1 && !dm_pattern_has_graph(&p.patterns[0]) &&
              !memcmp(p.percussion_midi, none, 128),
          "schema-14 patterns edit points and play incoming notes as themselves");
    p.patterns[0].graph_lanes[MIDI_CC_MODULATION] = 32;
    p.percussion_override = true;
    p.percussion_midi[36] = 61;
    check(dm_pack(&p, &saved) && saved.data[8] == DM_PROJECT_SCHEMA,
          "converted schema-14 projects save as the current schema");
    check(dm_unpack(&q, saved.data, saved.size, error, sizeof(error)) &&
              dm_graph_resolution(&q.patterns[0], MIDI_CC_MODULATION) == 32 &&
              q.percussion_midi[36] == 61,
          "the graph resolution and percussion MIDI mapping are saved and reloaded");

    static const int bad_graph[] = {4, 12, 24, 512};

    for (unsigned i = 0; i < sizeof(bad_graph) / sizeof(*bad_graph); ++i) {
        p.patterns[0].graph_lanes[MIDI_CC_MODULATION] = (uint16_t)bad_graph[i];
        check(!reloads(&p), "graph resolutions other than 1/8 to 1/256 are rejected");
    }

    p.patterns[0].graph_lanes[MIDI_CC_MODULATION] = 16;
    p.patterns[0].mode = 1;
    check(reloads(&p) && !dm_pattern_has_graph(&p.patterns[0]),
          "LFO hides saved Graph settings while active");
    p.patterns[0].mode = 0;
    p.percussion_midi[38] = 61;
    check(!reloads(&p), "one incoming note plays one drum");
    p.percussion_midi[38] = 0;
    p.percussion_override = false;
    check(!reloads(&p), "the project MIDI mapping requires the project override");
    p.percussion_override = true;
    check(reloads(&p), "valid graph and percussion fields reload");

    /* Point-only events keep discrete points. */
    p.patterns[0].count = 0;
    p.automation_count = p.clip_count = 0;
    p.patterns[0].kind = 1;
    p.patterns[0].event = MIDI_CC_SUSTAIN;
    check(reloads(&p), "opening a point-only event preserves another lane's Graph setting");
    p.patterns[0].graph_lanes[MIDI_CC_SUSTAIN] = 16;
    check(!reloads(&p), "point-only events reject their own graph setting");

    dm_bytes_free(&previous);
    dm_bytes_free(&saved);
    dm_project_free(&p);
    dm_project_free(&q);
}

static void check_legacy_graph(void) {
    DmProject p, q;
    dm_project_init(&p);
    dm_project_init(&q);
    char error[256] = {0};
    DmBytes old = line_project(15);
    check(dm_unpack(&p, old.data, old.size, error, sizeof(error)), error);
    check(p.pattern_count == 1 && p.automation_count == 4 && p.percussion_midi[36] == 61 &&
              dm_graph_resolution(&p.patterns[0], DM_EVENT_BEND) == 32 &&
              dm_graph_resolution(&p.patterns[0], MIDI_CC_MODULATION) == 32 &&
              dm_graph_resolution(&p.patterns[0], MIDI_CC_EXPRESSION) == 32 &&
              !dm_graph_resolution(&p.patterns[0], MIDI_CC_SUSTAIN),
          "schema-15 Graph patterns retain their resolution on all eligible event types");
    DmBytes midi = old_metadata_midi(&p, &old);
    check(dm_import(&q, midi.data, midi.size, -1, 0, error, sizeof(error)), error);
    check(q.pattern_count == 1 && !memcmp(q.patterns[0].graph_lanes, p.patterns[0].graph_lanes,
                                          sizeof(p.patterns[0].graph_lanes)),
          "embedded schema-15 Graph settings also convert");
    dm_bytes_free(&midi);
    dm_bytes_free(&old);
    dm_project_free(&p);
    dm_project_free(&q);
}

static void check_legacy_bends(void) {
    DmProject p, q;
    dm_project_init(&p);
    dm_project_init(&q);
    char error[256] = {0};
    DmBytes old = line_project(16), saved = {0};
    check(dm_unpack(&p, old.data, old.size, error, sizeof(error)), error);
    uint32_t id = p.patterns[0].notes[0].bend_id;
    check(id && p.automation[1].bend_note == id && !p.automation[2].bend_note,
          "schema-16 piano-roll bend acquires a root attached to the note");
    check(!p.automation[4].bend_note && !p.automation[6].fixed_anchor,
          "schema-16 Events line and imported discrete point retain their behavior");
    check(dm_pack(&p, &saved) && dm_unpack(&q, saved.data, saved.size, error, sizeof(error)) &&
              q.patterns[0].notes[0].bend_id == id && q.automation[1].bend_note == id,
          "the migrated attachment survives saving and reopening");
    dm_bytes_free(&saved);
    dm_bytes_free(&old);
    dm_project_free(&q);
    dm_project_free(&p);
}

static void check_legacy_pitch_units(void) {
    DmProject p, q;
    dm_project_init(&p);
    dm_project_init(&q);
    char error[256] = {0};
    DmBytes old = line_project(17), saved = {0};
    check(dm_unpack(&p, old.data, old.size, error, sizeof(error)), error);
    check(p.automation[1].bend_root == DM_BEND_ROOT_PITCH && !p.automation[1].bend_note &&
              p.automation[2].bend_root == DM_BEND_ROOT_NONE,
          "schema-17 detached piano-roll bend gets a Pitch root");
    check(p.automation[4].bend_root == DM_BEND_ROOT_NONE &&
              p.automation[6].bend_root == DM_BEND_ROOT_NONE,
          "ambiguous Events lines and imported points keep their MIDI values");
    check(dm_pack(&p, &saved) && dm_unpack(&q, saved.data, saved.size, error, sizeof(error)) &&
              q.automation[1].bend_root == DM_BEND_ROOT_PITCH,
          "the migrated Pitch root survives saving and reopening");
    dm_bytes_free(&saved);
    dm_bytes_free(&old);
    dm_project_free(&q);
    dm_project_free(&p);
}

static void parallel_note_rows(void) {
    DmProject p, loaded;
    dm_project_init(&p);
    dm_project_init(&loaded);
    char error[256] = {0};
    DmBytes old = line_project(18), saved = {0};
    check(dm_unpack(&p, old.data, old.size, error, sizeof(error)), error);

    for (int c = 0; c < 16; ++c) {
        check(p.note_rows[c] == 1, "schema-18 channels start with one note row");
    }

    check(p.clip_count == 1 && p.clips[0].row == 0,
          "schema-18 placements start on the first note row");
    p.note_rows[0] = 3;
    p.clips[0].row = 2;
    check(dm_pack(&p, &saved) && dm_unpack(&loaded, saved.data, saved.size, error, sizeof(error)),
          error);
    check(loaded.note_rows[0] == 3 && loaded.clips[0].row == 2,
          "schema-19 round trip preserves parallel note rows");
    dm_bytes_free(&saved);

    p.clips[0].row = 3;
    check(dm_pack(&p, &saved) && !dm_unpack(&loaded, saved.data, saved.size, error, sizeof(error)),
          "loading rejects a placement outside its channel's note rows");
    dm_bytes_free(&saved);
    p.clips[0].row = 0;
    p.note_rows[0] = 0;
    check(dm_pack(&p, &saved) && !dm_unpack(&loaded, saved.data, saved.size, error, sizeof(error)),
          "loading rejects zero note rows");
    dm_bytes_free(&saved);
    p.note_rows[0] = DM_NOTE_ROWS + 1;
    check(dm_pack(&p, &saved) && !dm_unpack(&loaded, saved.data, saved.size, error, sizeof(error)),
          "loading rejects excessive note rows");
    dm_bytes_free(&saved);
    dm_bytes_free(&old);
    dm_project_free(&p);
    dm_project_free(&loaded);
}

static uint32_t schema_from_header(const DmBytes *file) {
    check(file->size >= 20, "packed header size");
    return (uint32_t)file->data[8] | ((uint32_t)file->data[9] << 8) |
           ((uint32_t)file->data[10] << 16) | ((uint32_t)file->data[11] << 24);
}

static bool bytes_equal(DmBytes a, DmBytes b) {
    return a.size == b.size && (!a.size || !memcmp(a.data, b.data, a.size));
}

static bool projects_equal(const DmProject *a, const DmProject *b) {
    if (strcmp(a->name, b->name) || strcmp(a->soundfont, b->soundfont) || a->bpm != b->bpm ||
        a->numerator != b->numerator || a->denominator != b->denominator || a->port != b->port ||
        a->sequence != b->sequence || a->source_format != b->source_format ||
        a->sequence_count != b->sequence_count ||
        a->percussion_override != b->percussion_override ||
        memcmp(a->percussion_keys, b->percussion_keys, sizeof(a->percussion_keys)) ||
        memcmp(a->percussion_midi, b->percussion_midi, sizeof(a->percussion_midi)) ||
        a->port_count != b->port_count || a->next_pattern_uid != b->next_pattern_uid ||
        !bytes_equal(a->original, b->original)) {
        return false;
    }

    for (int i = 0; i < a->port_count; ++i) {
        if (a->ports[i] != b->ports[i]) {
            return false;
        }
    }

    for (int c = 0; c < 16; ++c) {
        if (a->channels[c].enabled != b->channels[c].enabled ||
            a->channels[c].mute != b->channels[c].mute ||
            a->channels[c].solo != b->channels[c].solo ||
            a->channels[c].program != b->channels[c].program ||
            a->channels[c].volume != b->channels[c].volume ||
            a->channels[c].pan != b->channels[c].pan ||
            a->channels[c].drums != b->channels[c].drums || a->note_rows[c] != b->note_rows[c] ||
            a->lane_count[c] != b->lane_count[c]) {
            return false;
        }

        for (int l = 0; l < a->lane_count[c]; ++l) {
            if (strcmp(a->lane_names[c][l], b->lane_names[c][l])) {
                return false;
            }
        }
    }

    if (a->pattern_count != b->pattern_count) {
        return false;
    }

    for (size_t i = 0; i < a->pattern_count; ++i) {
        const DmPattern *pa = &a->patterns[i], *pb = &b->patterns[i];

        if (pa->uid != pb->uid || strcmp(pa->name, pb->name) ||
            strcmp(pa->base_name, pb->base_name) || pa->duplicate_index != pb->duplicate_index ||
            pa->length != pb->length || pa->kind != pb->kind || pa->mode != pb->mode ||
            pa->event != pb->event || pa->speed != pb->speed || pa->tension != pb->tension ||
            pa->skew != pb->skew || pa->pulse != pb->pulse || pa->amount != pb->amount ||
            pa->center != pb->center || pa->pitch_center != pb->pitch_center ||
            pa->reference != pb->reference || pa->port != pb->port ||
            pa->inverted != pb->inverted || pa->color != pb->color ||
            memcmp(pa->graph_lanes, pb->graph_lanes, sizeof(pa->graph_lanes)) ||
            memcmp(pa->graph_hold, pb->graph_hold, sizeof(pa->graph_hold)) ||
            pa->count != pb->count) {
            return false;
        }

        for (size_t n = 0; n < pa->count; ++n) {
            const DmNote *na = &pa->notes[n], *nb = &pb->notes[n];

            if (na->beat != nb->beat || na->length != nb->length || na->key != nb->key ||
                na->velocity != nb->velocity || na->selected != nb->selected ||
                na->port != nb->port || na->bend_id != nb->bend_id) {
                return false;
            }
        }
    }

    if (a->clip_count != b->clip_count) {
        return false;
    }

    for (size_t i = 0; i < a->clip_count; ++i) {
        const DmClip *ca = &a->clips[i], *cb = &b->clips[i];

        if (ca->beat != cb->beat || ca->pattern != cb->pattern || ca->channel != cb->channel ||
            ca->selected != cb->selected || ca->length != cb->length || ca->lane != cb->lane ||
            ca->row != cb->row || ca->offset != cb->offset) {
            return false;
        }
    }

    if (a->automation_count != b->automation_count) {
        return false;
    }

    for (size_t i = 0; i < a->automation_count; ++i) {
        const DmAutomation *aa = &a->automation[i], *ab = &b->automation[i];

        if (aa->beat != ab->beat || aa->type != ab->type || aa->channel != ab->channel ||
            aa->a != ab->a || aa->b != ab->b || aa->pattern != ab->pattern ||
            aa->curve != ab->curve || aa->anchor != ab->anchor || aa->bend != ab->bend ||
            aa->ramp != ab->ramp || aa->port != ab->port || aa->hold != ab->hold ||
            aa->fixed_anchor != ab->fixed_anchor || aa->bend_note != ab->bend_note ||
            aa->bend_root != ab->bend_root) {
            return false;
        }
    }

    if (a->event_count != b->event_count) {
        return false;
    }

    for (size_t i = 0; i < a->event_count; ++i) {
        const DmEvent *ea = &a->events[i], *eb = &b->events[i];

        if (ea->beat != eb->beat || ea->track != eb->track || ea->port != eb->port ||
            !bytes_equal(ea->message, eb->message)) {
            return false;
        }
    }

    if (a->view_count != b->view_count) {
        return false;
    }

    for (size_t i = 0; i < a->view_count; ++i) {
        const DmView *va = &a->views[i], *vb = &b->views[i];

        if (va->port != vb->port || va->sequence != vb->sequence ||
            !bytes_equal(va->state, vb->state)) {
            return false;
        }
    }

    if (a->port_mix_count != b->port_mix_count) {
        return false;
    }

    for (size_t i = 0; i < a->port_mix_count; ++i) {
        const DmPortMix *ma = &a->port_mix[i], *mb = &b->port_mix[i];

        if (ma->port != mb->port || ma->channel != mb->channel || ma->volume != mb->volume ||
            ma->pan != mb->pan || ma->mute != mb->mute || ma->solo != mb->solo ||
            ma->program != mb->program) {
            return false;
        }
    }

    if (a->patch_replacement_count != b->patch_replacement_count) {
        return false;
    }

    for (size_t i = 0; i < a->patch_replacement_count; ++i) {
        const DmPatchReplacement *ra = &a->patch_replacements[i], *rb = &b->patch_replacements[i];

        if (ra->key.bank != rb->key.bank || ra->key.port != rb->key.port ||
            ra->key.channel != rb->key.channel || ra->key.program != rb->key.program ||
            ra->key.drums != rb->key.drums || (ra->generation != 0) != (rb->generation != 0) ||
            strcmp(ra->locator, rb->locator) || strcmp(ra->set_id, rb->set_id) ||
            strcmp(ra->preset_id, rb->preset_id)) {
            return false;
        }
    }

    return true;
}

static void build_full_project(DmProject *p) {
    static const uint8_t midi[] = {
        'M',
        'T',
        'h',
        'd',
        0,
        0,
        0,
        6,
        0,
        2,
        0,
        2,
        0,
        96,
        'M',
        'T',
        'r',
        'k',
        0,
        0,
        0,
        30,
        0,
        MIDI_META,
        MIDI_META_PORT,
        1,
        0,
        0,
        MIDI_NOTE_ON,
        60,
        100,
        96,
        MIDI_NOTE_OFF,
        60,
        0,
        0,
        MIDI_META,
        MIDI_META_PORT,
        1,
        1,
        0,
        MIDI_NOTE_ON,
        62,
        100,
        96,
        MIDI_NOTE_OFF,
        62,
        0,
        0,
        MIDI_META,
        MIDI_META_END,
        0,
        'M',
        'T',
        'r',
        'k',
        0,
        0,
        0,
        17,
        0,
        MIDI_META,
        MIDI_META_PORT,
        1,
        1,
        0,
        MIDI_NOTE_ON,
        64,
        100,
        96,
        MIDI_NOTE_OFF,
        64,
        0,
        0,
        MIDI_META,
        MIDI_META_END,
        0,
    };
    char error[256] = {0};
    check(dm_import(p, midi, sizeof(midi), 0, 0, error, sizeof(error)), error);
    check(dm_view_switch(p, 1, 1, error, sizeof(error)), error);

    snprintf(p->name, sizeof(p->name), "Tagged round trip");
    snprintf(p->soundfont, sizeof(p->soundfont), "custom/soundfont.sf2");
    p->bpm = 137.5;
    p->numerator = 3;
    p->denominator = 8;
    p->percussion_override = true;
    p->percussion_keys[36] = 1;
    p->percussion_midi[36] = 38;

    p->channels[0] = (DmChannel){true, true, true, 12, 85, 32, true};
    p->note_rows[0] = 3;
    p->lane_count[0] = 2;
    snprintf(p->lane_names[0][0], sizeof(p->lane_names[0][0]), "Modulation");
    snprintf(p->lane_names[0][1], sizeof(p->lane_names[0][1]), "Expression");

    int pat_notes = dm_pattern_add(p, "Notes");
    check(pat_notes >= 0, "add note pattern");
    snprintf(p->patterns[pat_notes].name, sizeof(p->patterns[pat_notes].name), "Notes 2");
    p->patterns[pat_notes].duplicate_index = 2;
    p->patterns[pat_notes].length = 8;
    p->patterns[pat_notes].port = 2;
    p->patterns[pat_notes].color = 3;
    p->patterns[pat_notes].graph_lanes[MIDI_CC_MODULATION] = 16;
    p->patterns[pat_notes].graph_lanes[DM_GRAPH_PITCH_LANE] = 32;
    check(dm_note_add(&p->patterns[pat_notes], (DmNote){0.5, 1.5, 62, 95, 1, 2, 7}), "add note 1");
    check(dm_note_add(&p->patterns[pat_notes], (DmNote){2.25, 0.75, 65, 80, 0, 1, 0}),
          "add note 2");

    int pat_auto = dm_pattern_add(p, "Auto Graph");
    check(pat_auto >= 0, "add graph automation pattern");
    p->patterns[pat_auto].kind = 1;
    p->patterns[pat_auto].mode = 0;
    p->patterns[pat_auto].event = MIDI_CC_EXPRESSION;
    p->patterns[pat_auto].length = 4;
    p->patterns[pat_auto].port = 1;
    p->patterns[pat_auto].color = 5;
    p->patterns[pat_auto].graph_lanes[MIDI_CC_EXPRESSION] = 16;
    p->patterns[pat_auto].graph_hold[MIDI_CC_EXPRESSION] = true;

    int pat_lfo = dm_pattern_add(p, "LFO Pattern");
    check(pat_lfo >= 0, "add LFO pattern");
    p->patterns[pat_lfo].kind = 1;
    p->patterns[pat_lfo].mode = 1;
    p->patterns[pat_lfo].event = MIDI_CC_MODULATION;
    p->patterns[pat_lfo].length = 4;
    p->patterns[pat_lfo].speed = 2.5;
    p->patterns[pat_lfo].tension = 0.25;
    p->patterns[pat_lfo].skew = -0.5;
    p->patterns[pat_lfo].pulse = 0.25;
    p->patterns[pat_lfo].amount = 0.75;
    p->patterns[pat_lfo].center = 50;
    p->patterns[pat_lfo].pitch_center = 62;
    p->patterns[pat_lfo].reference = 64;
    p->patterns[pat_lfo].port = 1;
    p->patterns[pat_lfo].inverted = true;
    p->patterns[pat_lfo].color = 2;

    check(dm_clip_add(p, (DmClip){1.0, pat_notes, 0, 1, 4.0, 0, 2, 0.5}), "add note clip");
    check(dm_clip_add(p, (DmClip){2.0, pat_auto, 0, 0, 2.0, 1, 0, 0.25}), "add auto clip");

    check(dm_automation_add(p, (DmAutomation){0.5, MIDI_PITCH_BEND, 0, 0, 64, pat_notes, 1, 62, 0,
                                              false, 2, false, true, 7, DM_BEND_ROOT_PITCH}),
          "add bend root automation");
    check(dm_automation_add(p, (DmAutomation){1.5, MIDI_PITCH_BEND, 0, 64, 96, pat_notes, 1, 62,
                                              0.5, true, 2, true, false, 0, DM_BEND_ROOT_NONE}),
          "add bend end automation");

    const uint8_t ev_bytes[] = {MIDI_CONTROL, MIDI_CC_VOLUME, 90};
    DmBytes ev_msg = {0};
    check(dm_bytes(&ev_msg, ev_bytes, sizeof(ev_bytes)), "event bytes");
    check(dm_event_add(p, 0.25, 1, 2, &ev_msg), "add channel event");
    dm_bytes_free(&ev_msg);

    check(dm_port_mix_set(p, (DmPortMix){2, 3, 80, 40, true, true, 15}), "add port mix");

    DmPatchReplacement patch = {
        .key = {.bank = 130, .port = 2, .channel = 3, .program = 14, .drums = true},
        .generation = 5,
        .locator = "test/library",
        .set_id = "test-set",
        .preset_id = "test-preset"};
    check(dm_patch_replacement_set(p, &patch), "add patch replacement");
    check(dm_project_validate(p, error, sizeof(error)), error);
}

static void check_schema_19_to_20(void) {
    char error[256] = {0};

    for (uint32_t s = 9; s <= 19; ++s) {
        DmProject p, q;
        dm_project_init(&p);
        dm_project_init(&q);
        DmBytes old = s == 9 ? schema_9_project() : line_project(s);
        DmBytes packed = {0};
        check(dm_unpack(&p, old.data, old.size, error, sizeof(error)), error);
        check(dm_project_validate(&p, error, sizeof(error)), error);
        check(dm_pack(&p, &packed), "pack converted legacy project");
        check(schema_from_header(&packed) == DM_PROJECT_SCHEMA, "dm_pack writes current schema");
        check(dm_unpack(&q, packed.data, packed.size, error, sizeof(error)), error);
        check(projects_equal(&p, &q), "legacy project converted to current schema reloads equal");
        dm_bytes_free(&old);
        dm_bytes_free(&packed);
        dm_project_free(&p);
        dm_project_free(&q);
    }
}

static void check_tagged_round_trip(void) {
    DmProject p, q;
    dm_project_init(&p);
    dm_project_init(&q);
    build_full_project(&p);

    DmBytes packed = {0};
    char error[256] = {0};
    check(dm_pack(&p, &packed), "pack full project");
    check(schema_from_header(&packed) == DM_PROJECT_SCHEMA, "full project saves as current schema");
    check(dm_unpack(&q, packed.data, packed.size, error, sizeof(error)), error);
    check(projects_equal(&p, &q), "full project round-trips every saved field");

    dm_bytes_free(&packed);
    dm_project_free(&p);
    dm_project_free(&q);
}

static uint64_t zigzag64(int64_t n) {
    return n < 0 ? ((uint64_t)(-(n + 1)) << 1) | 1 : (uint64_t)n << 1;
}

static void put_entry(DmBytes *out, uint64_t tag, const void *data, size_t size) {
    v10(out, tag);
    v10(out, size);

    if (size) {
        check(dm_bytes(out, data, size), "fixture memory");
    }
}

static void put_varint_field(DmBytes *out, uint64_t tag, uint64_t value) {
    DmBytes v = {0};
    v10(&v, value);
    put_entry(out, tag, v.data, v.size);
    dm_bytes_free(&v);
}

static void put_int_field(DmBytes *out, uint64_t tag, int64_t value) {
    put_varint_field(out, tag, zigzag64(value));
}

static void put_real_field(DmBytes *out, uint64_t tag, double value) {
    DmBytes v = {0};
    r10(&v, value);
    put_entry(out, tag, v.data, v.size);
    dm_bytes_free(&v);
}

static void put_text_field(DmBytes *out, uint64_t tag, const char *s) {
    put_entry(out, tag, s, strlen(s));
}

static DmBytes wrap_schema(const DmBytes *body, uint32_t schema) {
    DmBytes stored = {0}, file = {0};
    check(dm_compress(body->data, body->size, &stored), "fixture memory");
    check(dm_bytes(&file, "DUCKMIDI", 8), "fixture memory");
    u32(&file, schema);
    u32(&file, (uint32_t)stored.size);
    u32(&file, crc32(stored.data, stored.size));
    check(dm_bytes(&file, stored.data, stored.size), "fixture memory");
    dm_bytes_free(&stored);
    return file;
}

static DmBytes wrap_schema_20(const DmBytes *body) {
    return wrap_schema(body, 20);
}

static DmBytes minimal_schema_20_body(uint64_t extra_note_tag, uint64_t extra_pattern_tag,
                                      uint64_t extra_record_tag) {
    DmBytes body = {0}, proj = {0}, pat = {0}, note = {0}, clip = {0};
    put_varint_field(&proj, 11, 1); /* next_pattern_uid */
    put_entry(&body, 1, proj.data, proj.size);

    put_varint_field(&note, 1, 0);   /* beat (time delta 0) */
    put_real_field(&note, 2, 1);     /* length */
    put_varint_field(&note, 3, 60);  /* key */
    put_varint_field(&note, 4, 100); /* velocity */

    if (extra_note_tag) {
        put_varint_field(&note, extra_note_tag, 1);
    }

    put_varint_field(&pat, 1, 1);   /* uid */
    put_text_field(&pat, 3, "Pat"); /* base_name */
    put_entry(&pat, 30, note.data, note.size);

    if (extra_pattern_tag) {
        put_varint_field(&pat, extra_pattern_tag, 1);
    }

    put_entry(&body, 4, pat.data, pat.size);

    put_varint_field(&clip, 1, 0); /* beat (time delta 0) */
    put_entry(&body, 5, clip.data, clip.size);

    if (extra_record_tag) {
        DmBytes extra = {0};
        put_int_field(&extra, 1, 0);
        put_entry(&body, extra_record_tag, extra.data, extra.size);
        dm_bytes_free(&extra);
    }

    dm_bytes_free(&proj);
    dm_bytes_free(&pat);
    dm_bytes_free(&note);
    dm_bytes_free(&clip);
    return body;
}

static DmBytes make_known_valid_project(DmProject *p) {
    DmBytes old = line_project(19), packed = {0};
    char error[256] = {0};
    check(dm_unpack(p, old.data, old.size, error, sizeof(error)), error);
    check(dm_pack(p, &packed), "pack destination project");
    dm_bytes_free(&old);
    return packed;
}

static bool project_bytes_equal(const DmProject *p, const DmBytes *expected) {
    DmBytes actual = {0};
    bool ok = dm_pack(p, &actual) && bytes_equal(actual, *expected);
    dm_bytes_free(&actual);
    return ok;
}

static void check_missing_fields_default(void) {
    DmBytes body = minimal_schema_20_body(0, 0, 0);
    DmBytes file = wrap_schema_20(&body);
    DmProject p;
    dm_project_init(&p);
    char error[256] = {0};

    check(dm_unpack(&p, file.data, file.size, error, sizeof(error)), error);
    check(p.pattern_count == 1 && p.patterns[0].speed == 1.0,
          "missing pattern speed takes table default (1)");
    check(p.patterns[0].count == 1 && p.patterns[0].notes[0].selected == 0 &&
              p.patterns[0].notes[0].port == 0,
          "missing note selected and port take table defaults (0)");

    dm_bytes_free(&body);
    dm_bytes_free(&file);
    dm_project_free(&p);

    /* A schema-20 Graph lane record without field 3 (hold) loads with graph_hold false. */
    DmBytes s20_body = {0}, proj = {0}, pat = {0}, gl = {0};
    put_varint_field(&proj, 11, 1); /* next_pattern_uid */
    put_entry(&s20_body, 1, proj.data, proj.size);
    put_varint_field(&pat, 1, 1);
    put_text_field(&pat, 3, "Pat");
    put_int_field(&pat, 6, 1); /* kind = automation (signed field) */
    put_int_field(&gl, 1, MIDI_CC_EXPRESSION);
    put_varint_field(&gl, 2, 16);
    put_entry(&pat, 31, gl.data, gl.size);
    put_entry(&s20_body, 4, pat.data, pat.size);
    DmBytes s20_file = wrap_schema_20(&s20_body);
    dm_project_init(&p);
    check(dm_unpack(&p, s20_file.data, s20_file.size, error, sizeof(error)), error);
    check(p.pattern_count == 1 && p.patterns[0].graph_lanes[MIDI_CC_EXPRESSION] == 16 &&
              !p.patterns[0].graph_hold[MIDI_CC_EXPRESSION],
          "missing graph_hold field defaults to false");
    dm_bytes_free(&proj);
    dm_bytes_free(&pat);
    dm_bytes_free(&gl);
    dm_bytes_free(&s20_body);
    dm_bytes_free(&s20_file);
    dm_project_free(&p);
}

static void check_unknown_tags_refused(void) {
    const struct {
        uint64_t note_tag, pattern_tag, record_tag;
        const char *label;
    } cases[] = {
        {60, 0, 0, "unknown field tag inside note record"},
        {0, 0, 11, "unknown top-level record tag"},
        {0, 40, 0, "unknown nested tag inside pattern record"},
    };

    DmProject dest;
    dm_project_init(&dest);
    DmBytes before = make_known_valid_project(&dest);

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        DmBytes body =
            minimal_schema_20_body(cases[i].note_tag, cases[i].pattern_tag, cases[i].record_tag);
        DmBytes file = wrap_schema_20(&body);
        char error[256] = {0};

        check(!dm_unpack(&dest, file.data, file.size, error, sizeof(error)), cases[i].label);
        check(!strcmp(error, DM_TEXT_PROJECT_NEWER_DUCKMIDI),
              "unknown tag reports newer-duckmidi error");
        check(project_bytes_equal(&dest, &before), "unknown tag leaves destination unchanged");

        dm_bytes_free(&body);
        dm_bytes_free(&file);
    }

    dm_bytes_free(&before);
    dm_project_free(&dest);
}

static void check_structural_error(DmProject *dest, const DmBytes *before, const DmBytes *body,
                                   const char *label) {
    DmBytes file = wrap_schema_20(body);
    char error[256] = {0};
    check(!dm_unpack(dest, file.data, file.size, error, sizeof(error)), label);
    check(!strcmp(error, DM_TEXT_PROJECT_CONTAINS_INVALID),
          "structural error reports invalid-project error");
    check(project_bytes_equal(dest, before), "structural error leaves destination unchanged");
    dm_bytes_free(&file);
}

static void check_structural_errors_refused(void) {
    DmProject dest;
    dm_project_init(&dest);
    DmBytes before = make_known_valid_project(&dest);

    /* 1. Field length running past its record. */
    {
        DmBytes body = {0}, proj = {0};
        v10(&proj, 11);
        v10(&proj, 100);
        uint8_t one = 1;
        check(dm_bytes(&proj, &one, 1), "fixture memory");
        put_entry(&body, 1, proj.data, proj.size);
        check_structural_error(&dest, &before, &body, "field length past record");
        dm_bytes_free(&proj);
        dm_bytes_free(&body);
    }

    /* 2. FIELD_U8 field (note key) with value 300. */
    {
        DmBytes body = {0}, proj = {0}, pat = {0}, note = {0};
        put_varint_field(&proj, 11, 1);
        put_entry(&body, 1, proj.data, proj.size);
        put_varint_field(&note, 1, 0);
        put_real_field(&note, 2, 1);
        put_varint_field(&note, 3, 300);
        put_varint_field(&note, 4, 100);
        put_varint_field(&pat, 1, 1);
        put_text_field(&pat, 3, "Pat");
        put_entry(&pat, 30, note.data, note.size);
        put_entry(&body, 4, pat.data, pat.size);
        check_structural_error(&dest, &before, &body, "FIELD_U8 overflow");
        dm_bytes_free(&proj);
        dm_bytes_free(&pat);
        dm_bytes_free(&note);
        dm_bytes_free(&body);
    }

    /* 3. Placement record before pattern records. */
    {
        DmBytes body = {0}, proj = {0}, pat = {0}, clip = {0};
        put_varint_field(&proj, 11, 1);
        put_entry(&body, 1, proj.data, proj.size);
        put_varint_field(&clip, 1, 0);
        put_entry(&body, 5, clip.data, clip.size);
        put_varint_field(&pat, 1, 1);
        put_text_field(&pat, 3, "Pat");
        put_entry(&body, 4, pat.data, pat.size);
        check_structural_error(&dest, &before, &body, "placement before pattern");
        dm_bytes_free(&proj);
        dm_bytes_free(&pat);
        dm_bytes_free(&clip);
        dm_bytes_free(&body);
    }

    /* 4. Two project records. */
    {
        DmBytes body = {0}, proj = {0};
        put_varint_field(&proj, 11, 1);
        put_entry(&body, 1, proj.data, proj.size);
        put_entry(&body, 1, proj.data, proj.size);
        check_structural_error(&dest, &before, &body, "two project records");
        dm_bytes_free(&proj);
        dm_bytes_free(&body);
    }

    /* 5. Same field twice in one record. */
    {
        DmBytes body = {0}, proj = {0};
        put_varint_field(&proj, 11, 1);
        put_varint_field(&proj, 11, 2);
        put_entry(&body, 1, proj.data, proj.size);
        check_structural_error(&dest, &before, &body, "duplicate field in record");
        dm_bytes_free(&proj);
        dm_bytes_free(&body);
    }

    /* 6. Text field containing a 0 byte. */
    {
        DmBytes body = {0}, proj = {0}, pat = {0};
        put_varint_field(&proj, 11, 1);
        put_entry(&body, 1, proj.data, proj.size);
        put_varint_field(&pat, 1, 1);
        const char bad_text[3] = {'P', '\0', 't'};
        put_entry(&pat, 3, bad_text, sizeof(bad_text));
        put_entry(&body, 4, pat.data, pat.size);
        check_structural_error(&dest, &before, &body, "text field with 0 byte");
        dm_bytes_free(&proj);
        dm_bytes_free(&pat);
        dm_bytes_free(&body);
    }

    dm_bytes_free(&before);
    dm_project_free(&dest);
}

static uint32_t next_lcg(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void check_tagged_fuzz(void) {
    DmProject src, dest;
    dm_project_init(&src);
    dm_project_init(&dest);
    build_full_project(&src);

    DmBytes packed = {0}, body = {0};
    check(dm_pack(&src, &packed), "pack full project for fuzz");
    check(packed.size > 20, "packed size");
    check(dm_decompress(packed.data + 20, packed.size - 20, 1024u * 1024u * 1024u, &body),
          "decompress packed body");

    DmBytes dest_bytes = make_known_valid_project(&dest);
    int accepted = 0, refused = 0;
    char error[256] = {0};

    for (size_t len = 0; len < body.size; ++len) {
        DmBytes trunc = {0};

        if (len) {
            check(dm_bytes(&trunc, body.data, len), "truncation copy");
        }

        DmBytes file = wrap_schema_20(&trunc);

        if (dm_unpack(&dest, file.data, file.size, error, sizeof(error))) {
            ++accepted;
            check(dm_project_validate(&dest, NULL, 0), "accepted truncation validates");
            check(dm_unpack(&dest, dest_bytes.data, dest_bytes.size, error, sizeof(error)),
                  "restore destination");
        } else {
            ++refused;
            check(project_bytes_equal(&dest, &dest_bytes),
                  "refused truncation leaves destination unchanged");
        }

        dm_bytes_free(&trunc);
        dm_bytes_free(&file);
    }

    uint32_t rng = 0x12345678u;
    DmBytes mutated = {0};
    check(dm_bytes(&mutated, body.data, body.size), "fuzz buffer");

    for (int iter = 0; iter < 2000; ++iter) {
        memcpy(mutated.data, body.data, body.size);
        unsigned flips = 1u + (next_lcg(&rng) % 4u);

        for (unsigned f = 0; f < flips; ++f) {
            size_t pos = next_lcg(&rng) % body.size;
            mutated.data[pos] ^= (uint8_t)(1u + (next_lcg(&rng) % 255u));
        }

        DmBytes file = wrap_schema_20(&mutated);

        if (dm_unpack(&dest, file.data, file.size, error, sizeof(error))) {
            ++accepted;
            check(dm_project_validate(&dest, NULL, 0), "accepted corruption validates");
            check(dm_unpack(&dest, dest_bytes.data, dest_bytes.size, error, sizeof(error)),
                  "restore destination");
        } else {
            ++refused;
            check(project_bytes_equal(&dest, &dest_bytes),
                  "refused corruption leaves destination unchanged");
        }

        dm_bytes_free(&file);
    }

    printf("fuzz tagged format: %d accepted, %d refused\n", accepted, refused);
    dm_bytes_free(&mutated);
    dm_bytes_free(&dest_bytes);
    dm_bytes_free(&body);
    dm_bytes_free(&packed);
    dm_project_free(&dest);
    dm_project_free(&src);
}

static void add_schema_20_song_point(DmBytes *body, double beat, double *prev_beat, int type,
                                     int channel, int a, int b, int port) {
    DmBytes rec = {0};
    put_real_field(&rec, 1, beat - *prev_beat);
    *prev_beat = beat;
    put_int_field(&rec, 2, type);
    put_int_field(&rec, 3, channel);
    put_int_field(&rec, 4, a);
    put_int_field(&rec, 5, b);
    put_int_field(&rec, 6, -1);

    if (port) {
        put_varint_field(&rec, 11, (uint64_t)port);
    }

    put_entry(body, 6, rec.data, rec.size);
    dm_bytes_free(&rec);
}

static void add_raw_event(DmProject *p, double beat, int port, uint8_t s, uint8_t a, uint8_t b,
                          size_t size) {
    uint8_t bytes[] = {s, a, b};
    DmBytes m = {0};
    check(dm_bytes(&m, bytes, size), "event bytes");
    check(dm_event_add(p, beat, 0, port, &m), "add equivalent raw event");
    dm_bytes_free(&m);
}

static void song_points_convert(void) {
    DmBytes body = {0}, proj = {0}, pat = {0}, note = {0}, clip = {0};
    put_varint_field(&proj, 11, 1); /* next_pattern_uid */
    put_entry(&body, 1, proj.data, proj.size);

    put_varint_field(&note, 1, 0);
    put_real_field(&note, 2, 1);
    put_varint_field(&note, 3, 60);
    put_varint_field(&note, 4, 100);
    put_varint_field(&note, 6, 2); /* port 2 */

    put_varint_field(&pat, 1, 1);
    put_text_field(&pat, 3, "Pat");
    put_entry(&pat, 30, note.data, note.size);
    put_entry(&body, 4, pat.data, pat.size);

    put_varint_field(&clip, 1, 0);
    put_int_field(&clip, 2, 0);
    put_int_field(&clip, 3, 2);
    put_entry(&body, 5, clip.data, clip.size);
    /* The same document without the song-level points: the base of the expected project. */
    DmBytes base = {0};
    check(dm_bytes(&base, body.data, body.size), "fixture memory");

    double prev = 0;
    add_schema_20_song_point(&body, 0, &prev, MIDI_PROGRAM, 2, 5, 0, 0);
    add_schema_20_song_point(&body, 0, &prev, MIDI_CONTROL, 2, MIDI_CC_VOLUME, 90, 0);
    add_schema_20_song_point(&body, 0, &prev, MIDI_CONTROL, 2, MIDI_CC_PAN, 30, 0);
    add_schema_20_song_point(&body, 0, &prev, MIDI_CONTROL, 2, MIDI_CC_VOLUME, 70, 2);
    add_schema_20_song_point(&body, 2, &prev, MIDI_CONTROL, 2, MIDI_CC_EXPRESSION, 64, 0);
    add_schema_20_song_point(&body, 2, &prev, MIDI_PITCH_BEND, 2, 0, 80, 0);
    add_schema_20_song_point(&body, 4, &prev, MIDI_CONTROL, 2, MIDI_CC_VOLUME, 50, 0);
    /* A raw bank select at beat 0; event records follow the automation records. */
    DmBytes bank_ev = {0};
    uint8_t cc0[] = {MIDI_CONTROL | 2, 0, 1};
    put_real_field(&bank_ev, 1, 0);
    put_int_field(&bank_ev, 2, 0);
    put_int_field(&bank_ev, 3, 0);
    put_entry(&bank_ev, 4, cc0, sizeof(cc0));
    put_entry(&body, 7, bank_ev.data, bank_ev.size);
    dm_bytes_free(&bank_ev);

    DmBytes file = wrap_schema_20(&body);
    DmProject p, equiv;
    dm_project_init(&p);
    dm_project_init(&equiv);
    char error[256] = {0};
    check(dm_unpack(&p, file.data, file.size, error, sizeof(error)), error);

    for (size_t i = 0; i < p.automation_count; ++i) {
        check(p.automation[i].pattern >= 0, "no automation point has pattern < 0");
    }

    check(p.channels[2].program == 5 && p.channels[2].volume == 90 && p.channels[2].pan == 30,
          "beat-0 port-0 points move to channel mixer");
    check(dm_port_mix(&p, 2, 2).volume == 70, "beat-0 port-2 volume moves to port mix");

    bool has_bank = false, has_prog = false, has_port2_vol = false, has_expr = false,
         has_bend = false, has_beat4_vol = false;

    for (size_t i = 0; i < p.event_count; ++i) {
        const DmEvent *ev = &p.events[i];
        const uint8_t *d = ev->message.data;

        if (ev->beat == 0 && ev->port == 0 && ev->message.size == 3 && d[0] == (MIDI_CONTROL | 2) &&
            d[1] == 0 && d[2] == 1) {
            has_bank = true;
        } else if (ev->beat == 0 && ev->port == 0 && ev->message.size == 2 &&
                   d[0] == (MIDI_PROGRAM | 2) && d[1] == 5) {
            has_prog = true;
        } else if (ev->beat == 0 && ev->port == 2 && ev->message.size == 3 &&
                   d[0] == (MIDI_CONTROL | 2) && d[1] == MIDI_CC_VOLUME && d[2] == 70) {
            has_port2_vol = true;
        } else if (ev->beat == 2 && ev->port == 0 && ev->message.size == 3 &&
                   d[0] == (MIDI_CONTROL | 2) && d[1] == MIDI_CC_EXPRESSION && d[2] == 64) {
            has_expr = true;
        } else if (ev->beat == 2 && ev->port == 0 && ev->message.size == 3 &&
                   d[0] == (MIDI_PITCH_BEND | 2) && d[1] == 0 && d[2] == 80) {
            has_bend = true;
        } else if (ev->beat == 4 && ev->port == 0 && ev->message.size == 3 &&
                   d[0] == (MIDI_CONTROL | 2) && d[1] == MIDI_CC_VOLUME && d[2] == 50) {
            has_beat4_vol = true;
        }
    }

    check(has_bank && has_prog && has_port2_vol && has_expr && has_bend && has_beat4_vol,
          "converted song points exist as raw events");

    DmBytes base_file = wrap_schema_20(&base);
    check(dm_unpack(&equiv, base_file.data, base_file.size, error, sizeof(error)), error);
    dm_bytes_free(&base_file);
    dm_bytes_free(&base);
    equiv.channels[2].program = 5;
    equiv.channels[2].volume = 90;
    equiv.channels[2].pan = 30;
    DmPortMix mix = dm_port_mix(&equiv, 2, 2);
    mix.volume = 70;
    check(dm_port_mix_set(&equiv, mix), "set equiv port mix");
    add_raw_event(&equiv, 0, 0, MIDI_CONTROL | 2, 0, 1, 3);
    add_raw_event(&equiv, 0, 0, MIDI_PROGRAM | 2, 5, 0, 2);
    add_raw_event(&equiv, 0, 2, MIDI_CONTROL | 2, MIDI_CC_VOLUME, 70, 3);
    add_raw_event(&equiv, 2, 0, MIDI_CONTROL | 2, MIDI_CC_EXPRESSION, 64, 3);
    add_raw_event(&equiv, 2, 0, MIDI_PITCH_BEND | 2, 0, 80, 3);
    add_raw_event(&equiv, 4, 0, MIDI_CONTROL | 2, MIDI_CC_VOLUME, 50, 3);

    DmExportEvent *actual = NULL, *expected = NULL;
    size_t actual_count = 0, expected_count = 0;
    uint64_t actual_dur = 0, expected_dur = 0;
    check(dm_export_timeline(&p, &actual, &actual_count, &actual_dur, error, sizeof(error)), error);
    check(
        dm_export_timeline(&equiv, &expected, &expected_count, &expected_dur, error, sizeof(error)),
        error);
    check(actual_count == expected_count, "converted timeline event count matches equivalent");

    int bank_idx = -1, prog_idx = -1;

    for (size_t i = 0; i < actual_count; ++i) {
        if (actual[i].tick == 0 && actual[i].port == 0 && actual[i].status == (MIDI_CONTROL | 2) &&
            actual[i].a == 0 && actual[i].b == 1 && bank_idx < 0) {
            bank_idx = (int)i;
        } else if (actual[i].tick == 0 && actual[i].port == 0 &&
                   actual[i].status == (MIDI_PROGRAM | 2) && actual[i].a == 5) {
            prog_idx = (int)i;
        }
    }

    /* The mixer's setup program always comes first; what was heard is the program sent after
       the bank select, as the song-level point played it. */
    check(bank_idx >= 0 && prog_idx >= 0 && bank_idx < prog_idx,
          "a program change follows the beat-0 bank select on channel 2");

    for (size_t i = 0; i < actual_count; ++i) {
        check(actual[i].tick == expected[i].tick && actual[i].port == expected[i].port &&
                  actual[i].status == expected[i].status && actual[i].a == expected[i].a &&
                  actual[i].b == expected[i].b,
              "converted timeline event matches equivalent");
    }

    free(actual);
    free(expected);
    dm_project_free(&p);
    dm_project_free(&equiv);
    dm_bytes_free(&file);
    dm_bytes_free(&proj);
    dm_bytes_free(&pat);
    dm_bytes_free(&note);
    dm_bytes_free(&clip);
    dm_bytes_free(&body);
}

static int compat_cc_at(const DmExportEvent *events, size_t count, int channel, int controller,
                        uint64_t tick) {
    int value = -1;

    for (size_t i = 0; i < count; ++i) {
        if (events[i].tick <= tick && events[i].status == (uint32_t)(MIDI_CONTROL | channel) &&
            events[i].a == (uint32_t)controller) {
            value = (int)events[i].b;
        }
    }

    return value;
}

/* A note pattern and a CC7 "Controls" automation pattern, both placed at beat 0; scope < 0 leaves
   out the retired control_scope field (pattern tag 17). */
static DmBytes control_scope_file(uint32_t schema, int scope) {
    DmBytes body = {0}, proj = {0}, ch = {0}, pat_notes = {0}, pat_auto = {0}, note = {0},
            clip0 = {0}, clip1 = {0}, auto_pt = {0};
    put_varint_field(&proj, 11, 2); /* next_pattern_uid */
    put_entry(&body, 1, proj.data, proj.size);

    put_int_field(&ch, 1, 0);    /* channel 0 */
    put_varint_field(&ch, 2, 1); /* enabled */
    put_int_field(&ch, 10, 1);   /* lane_count = 1 */
    put_entry(&body, 2, ch.data, ch.size);

    put_real_field(&note, 1, 0);
    put_real_field(&note, 2, 4);
    put_varint_field(&note, 3, 60);
    put_varint_field(&note, 4, 100);

    put_varint_field(&pat_notes, 1, 1);
    put_text_field(&pat_notes, 3, "Notes");
    put_real_field(&pat_notes, 5, 4);
    put_entry(&pat_notes, 30, note.data, note.size);
    put_entry(&body, 4, pat_notes.data, pat_notes.size);

    put_varint_field(&pat_auto, 1, 2);
    put_text_field(&pat_auto, 3, "Controls");
    put_real_field(&pat_auto, 5, 4);
    put_int_field(&pat_auto, 6, 1);              /* kind = automation */
    put_int_field(&pat_auto, 8, MIDI_CC_VOLUME); /* event = CC7 */

    if (scope >= 0) {
        put_int_field(&pat_auto, 17, scope); /* control_scope, retired in schema 23 */
    }

    put_entry(&body, 4, pat_auto.data, pat_auto.size);

    put_real_field(&clip0, 1, 0);
    put_int_field(&clip0, 2, 0);
    put_int_field(&clip0, 3, 0);
    put_int_field(&clip0, 6, 0);
    put_entry(&body, 5, clip0.data, clip0.size);

    put_real_field(&clip1, 1, 0);
    put_int_field(&clip1, 2, 1);
    put_int_field(&clip1, 3, 0);
    put_int_field(&clip1, 6, 1);
    put_entry(&body, 5, clip1.data, clip1.size);

    put_real_field(&auto_pt, 1, 1);
    put_int_field(&auto_pt, 2, MIDI_CONTROL);
    put_int_field(&auto_pt, 3, 0);
    put_int_field(&auto_pt, 4, MIDI_CC_VOLUME);
    put_int_field(&auto_pt, 5, 64);
    put_int_field(&auto_pt, 6, 1);
    put_entry(&body, 6, auto_pt.data, auto_pt.size);

    DmBytes file = wrap_schema(&body, schema);
    dm_bytes_free(&auto_pt);
    dm_bytes_free(&clip1);
    dm_bytes_free(&clip0);
    dm_bytes_free(&note);
    dm_bytes_free(&pat_auto);
    dm_bytes_free(&pat_notes);
    dm_bytes_free(&ch);
    dm_bytes_free(&proj);
    dm_bytes_free(&body);
    return file;
}

static bool control_scope_events(uint32_t schema, int scope, DmExportEvent **events,
                                 size_t *count) {
    DmBytes file = control_scope_file(schema, scope);
    DmProject p;
    dm_project_init(&p);
    char error[256] = {0};
    uint64_t duration = 0;
    bool ok = dm_unpack(&p, file.data, file.size, error, sizeof(error)) &&
              dm_project_validate(&p, error, sizeof(error)) &&
              dm_export_timeline(&p, events, count, &duration, error, sizeof(error));
    dm_project_free(&p);
    dm_bytes_free(&file);
    return ok;
}

/* Schema 23 drops control_scope: a generated Controls pattern from a schema 21 or 22 file (scope 2
   or 1) loads as an ordinary automation pattern, and with nothing overlapping it plays exactly as
   the same pattern saved without the field. A schema 23 file carrying the field is refused. */
static void control_scope_retired(void) {
    DmExportEvent *reference = NULL;
    size_t reference_count = 0;
    check(control_scope_events(DM_PROJECT_SCHEMA, -1, &reference, &reference_count),
          "load the Controls fixture without control_scope");
    check(compat_cc_at(reference, reference_count, 0, MIDI_CC_VOLUME, 0) == 100 &&
              compat_cc_at(reference, reference_count, 0, MIDI_CC_VOLUME, DM_PPQ) == 64 &&
              compat_cc_at(reference, reference_count, 0, MIDI_CC_VOLUME, 4 * DM_PPQ) == 64,
          "the Controls pattern sets CC7 at beat 1 and holds it over its placement");

    const struct {
        uint32_t schema;
        int scope;
    } old[] = {{21, 2}, {22, 1}, {22, 2}};

    for (size_t i = 0; i < sizeof(old) / sizeof(*old); ++i) {
        DmExportEvent *events = NULL;
        size_t count = 0;
        bool same = control_scope_events(old[i].schema, old[i].scope, &events, &count) &&
                    count == reference_count;

        for (size_t k = 0; same && k < count; ++k) {
            same = events[k].tick == reference[k].tick &&
                   events[k].status == reference[k].status && events[k].a == reference[k].a &&
                   events[k].b == reference[k].b && events[k].port == reference[k].port &&
                   events[k].flags == reference[k].flags;
        }

        check(same, "an older file's control_scope is dropped and plays the same events");
        free(events);
    }

    free(reference);
    DmBytes file = control_scope_file(DM_PROJECT_SCHEMA, 1);
    DmProject p;
    dm_project_init(&p);
    char error[256] = {0};
    check(!dm_unpack(&p, file.data, file.size, error, sizeof(error)),
          "a schema 23 file carrying control_scope is refused");
    dm_project_free(&p);
    dm_bytes_free(&file);
}

int main(void) {
    check_schema_19_to_20();
    check_tagged_round_trip();
    check_missing_fields_default();
    check_unknown_tags_refused();
    check_structural_errors_refused();
    check_tagged_fuzz();
    song_points_convert();
    control_scope_retired();
    parallel_note_rows();
    check_legacy_pitch_units();
    check_legacy_bends();
    check_patch_replacements();
    check_graph_and_percussion_midi();
    check_legacy_graph();
    DmProject p, again;
    dm_project_init(&p);
    dm_project_init(&again);
    char error[256] = {0};
    DmBytes old = schema_9_project(), current = {0};

    check(dm_unpack(&p, old.data, old.size, error, sizeof(error)), error);
    check(!strcmp(p.name, "Old song") && p.pattern_count == 1 && p.clip_count == 1 &&
              p.patterns[0].notes[0].port == 2 && p.channels[0].program == 5,
          "schema-9 content loads");
    check(p.patterns[0].color == 0 && p.clips[0].offset == 0 &&
              dm_port_mix(&p, 2, 0).volume == 90 && dm_port_mix(&p, 2, 0).program == 0,
          "fields added in schema 10 take their defaults");
    check(p.event_count == 1 && p.events[0].port == 0,
          "schema-9 events move to the default note port they played on");
    check(p.automation_count == 1 && p.automation[0].port == 2 &&
              dm_automation_curves(p.automation[0]),
          "schema 9 keeps curving points through the schema-11 upgrade");

    for (int c = 0; c < 16; ++c) {
        check(p.channels[c].drums == (c == MIDI_DRUM_CHANNEL),
              "schema 9 uses the schema-12 percussion mapping");
    }

    check(dm_pack(&p, &current) && current.data[8] == DM_PROJECT_SCHEMA,
          "converted projects save as the current schema");
    check(dm_unpack(&again, current.data, current.size, error, sizeof(error)) &&
              again.patterns[0].notes[0].port == 2 && again.events[0].port == 0,
          "saved conversion reloads");

    old.data[8] = 8;
    check(!dm_unpack(&again, old.data, old.size, error, sizeof(error)) && strstr(error, "schema 8"),
          "older schemas are rejected with a clear message");

    /* Schema 10 to 11: points gain the hold flag, off, so lines keep curving. */
    DmProject line;
    dm_project_init(&line);
    DmBytes previous = line_project(10), saved = {0};
    check(dm_unpack(&line, previous.data, previous.size, error, sizeof(error)), error);
    check(!strcmp(line.name, "Line song") && line.pattern_count == 1 && line.clip_count == 1 &&
              line.automation_count == 4,
          "schema-10 content loads");
    check(line.automation[0].curve == 1 && line.automation[1].b == 127 &&
              line.automation[2].curve == -1 && line.automation[3].ramp,
          "schema-10 points keep their lines and ramps");

    for (size_t i = 0; i < line.automation_count; ++i) {
        check(!line.automation[i].hold, "schema-10 points do not hold");
    }

    check(dm_automation_curves(line.automation[0]) && dm_automation_curves(line.automation[3]) &&
              !dm_automation_curves(line.automation[2]),
          "schema-10 line nodes and ramped points still curve");

    line.automation[0].hold = true;
    check(!dm_automation_curves(line.automation[0]), "a held line node does not curve");
    check(dm_pack(&line, &saved) && saved.data[8] == DM_PROJECT_SCHEMA,
          "converted schema-10 projects save as the current schema");
    dm_project_free(&line);
    dm_project_init(&line);
    check(dm_unpack(&line, saved.data, saved.size, error, sizeof(error)) &&
              line.automation[0].hold && !line.automation[1].hold,
          "the hold flag is saved and reloaded");
    dm_bytes_free(&previous);
    dm_bytes_free(&saved);
    dm_project_free(&line);

    /* Schema 11 to 12: channels gain the percussion flag, and channel 10 stays the drum part. */
    DmProject drums;
    dm_project_init(&drums);
    previous = line_project(11);
    saved = (DmBytes){0};
    check(dm_unpack(&drums, previous.data, previous.size, error, sizeof(error)), error);
    check(!strcmp(drums.name, "Line song") && drums.automation_count == 4,
          "schema-11 content loads");

    for (int c = 0; c < 16; ++c) {
        check(drums.channels[c].drums == (c == MIDI_DRUM_CHANNEL),
              "schema-11 projects play percussion on channel 10 only");
    }

    drums.channels[10].drums = true;
    drums.channels[MIDI_DRUM_CHANNEL].drums = false;
    check(dm_pack(&drums, &saved) && saved.data[8] == DM_PROJECT_SCHEMA,
          "converted schema-11 projects save as the current schema");
    dm_project_free(&drums);
    dm_project_init(&drums);
    check(dm_unpack(&drums, saved.data, saved.size, error, sizeof(error)) &&
              drums.channels[10].drums && !drums.channels[MIDI_DRUM_CHANNEL].drums,
          "the percussion flag is saved and reloaded");
    dm_bytes_free(&previous);
    dm_bytes_free(&saved);
    dm_project_free(&drums);

    /* New projects keep the General MIDI layout. */
    dm_project_init(&drums);

    for (int c = 0; c < 16; ++c) {
        check(drums.channels[c].drums == (c == MIDI_DRUM_CHANNEL) &&
                  dm_channel_default(c).drums == (c == MIDI_DRUM_CHANNEL),
              "new projects play percussion on channel 10");
    }

    dm_project_free(&drums);

    printf("PASS project compat: %d checks\n", checks);
    dm_bytes_free(&old);
    dm_bytes_free(&current);
    dm_project_free(&p);
    dm_project_free(&again);
    return 0;
}
