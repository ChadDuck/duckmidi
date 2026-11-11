/* Loads generated schema-9 through schema-17 projects and checks their conversion to the current
   schema. The writers below follow those layouts field by field; they exist only to produce these
   fixtures. */
#include "core/duck.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL project compat: %s\n", text);
        exit(1);
    }
}

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
        v10(b, 0);                                 /* Detached. */
    }
}

/* One pattern with one note, one placement and modulation data, saved as schema 10 through 17.
   The schema-15 fixture has a Graph resolution, discrete bars and a percussion mapping. */
static DmBytes line_project(uint32_t schema) {
    DmBytes body = {0}, stored = {0}, file = {0};
    t10(&body, "Line song");
    t10(&body, "");
    r10(&body, 120);
    v10(&body, 4);
    v10(&body, 4);
    v10(&body, 0); /* Source port. */
    v10(&body, 0); /* Sequence. */
    v10(&body, 1); /* Source format. */
    v10(&body, 1); /* Sequence count. */
    v10(&body, 1); /* Port count. */
    v10(&body, 0); /* Ports. */
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
        v10(&body, 0);
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
            v10(&body, i);          /* Port. */
            v10(&body, 0);          /* Channel. */
            v10(&body, 130);        /* Bank. */
            v10(&body, 6);          /* Program. */
            v10(&body, 0);          /* Drums. */
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
              dm_graph_resolution(&p.patterns[0], -2) == 32 &&
              dm_graph_resolution(&p.patterns[0], MIDI_CC_MODULATION) == 32 &&
              dm_graph_resolution(&p.patterns[0], MIDI_CC_EXPRESSION) == 32 &&
              !dm_graph_resolution(&p.patterns[0], MIDI_CC_SUSTAIN),
          "schema-15 Graph patterns retain their resolution on all eligible event types");
    DmBytes midi = old_metadata_midi(&p, &old);
    check(dm_import(&q, midi.data, midi.size, -1, 0, error, sizeof(error)), error);
    check(q.pattern_count == 1 &&
              !memcmp(q.patterns[0].graph_lanes, p.patterns[0].graph_lanes,
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

int main(void) {
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
