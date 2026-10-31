#include "duck.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static const unsigned char fixture[] = {
    'M',  'T',  'h',  'd', 0,    0,    0,    6,    0,    0,    0,    1,    1,    0xe0,
    'M',  'T',  'r',  'k', 0,    0,    0,    47,   0,    0xff, 0x51, 3,    7,    0xa1,
    0x20, 0,    0xc0, 4,   0,    0x90, 60,   100,  0x81, 0x70, 64,   90,   0x81, 0x70,
    0x80, 60,   0,    0,   64,   0,    0,    0xff, 0x51, 3,    0x0f, 0x42, 0x40, 0x83,
    0x60, 0x90, 67,   80,  0x83, 0x60, 0x80, 67,   0,    0,    0xff, 0x2f, 0};

static void roundtrip(void) {
    DmProject p;
    dm_project_init(&p);
    char error[256] = {0};
    CHECK(dm_import(&p, fixture, sizeof(fixture), 0, 0, error, sizeof(error)));
    CHECK(p.pattern_count == 1 && p.clip_count == 1);
    CHECK(p.patterns[0].count == 3);
    CHECK(p.channels[0].program == 4);
    CHECK(p.original.size == sizeof(fixture));
    CHECK(fabs(p.patterns[0].notes[0].length - 1) < 1e-9);
    CHECK(fabs(p.patterns[0].notes[1].beat - .5) < 1e-9);
    CHECK(fabs(dm_beat_seconds(&p, 3) - 2.5) < 1e-9);
    CHECK(fabs(dm_seconds_beat(&p, 2.5) - 3) < 1e-9);
    DmBytes mid = {0};
    CHECK(dm_midi(&p, &mid, error, sizeof(error)));
    DmProject next;
    dm_project_init(&next);
    CHECK(dm_import(&next, mid.data, mid.size, 0, 0, error, sizeof(error)));
    CHECK(next.patterns[0].count == 3);
    CHECK(fabs(dm_beat_seconds(&next, 3) - 2.5) < 1e-9);
    CHECK(dm_automation_add(&p, (DmAutomation){1, 0xe0, 0, 0, 80, 0, 0, 0, 0, false}));
    snprintf(p.name, sizeof(p.name), "Roundtrip test");
    DmBytes packed = {0};
    CHECK(dm_pack(&p, &packed));
    CHECK(dm_unpack(&next, packed.data, packed.size, error, sizeof(error)));
    CHECK(!strcmp(next.name, p.name));
    CHECK(next.automation_count == 1);
    CHECK(!memcmp(next.original.data, fixture, sizeof(fixture)));
    CHECK(
        dm_write("agents/reports/roundtrip.duck", packed.data, packed.size, error, sizeof(error)));
    CHECK(dm_write("agents/reports/roundtrip.mid", mid.data, mid.size, error, sizeof(error)));
    packed.data[packed.size - 1] ^= 1;
    CHECK(!dm_unpack(&next, packed.data, packed.size, error, sizeof(error)));
    CHECK(!strcmp(next.name, "Roundtrip test"));
    packed.data[packed.size - 1] ^= 1;
    packed.data[8] = 2;
    CHECK(!dm_unpack(&next, packed.data, packed.size, error, sizeof(error)));
    DmSchedule schedule = {0};
    CHECK(dm_schedule(&p, &schedule));
    CHECK(schedule.count >= 10);

    for (size_t i = 1; i < schedule.count; ++i) {
        CHECK(schedule.items[i].seconds >= schedule.items[i - 1].seconds);
    }

    dm_schedule_free(&schedule);
    dm_bytes_free(&packed);
    dm_bytes_free(&mid);
    dm_project_free(&next);
    dm_project_free(&p);
}

static void model(void) {
    DmProject p;
    dm_project_init(&p);
    CHECK(p.channels[9].enabled);
    CHECK(!p.channels[8].enabled);
    int id = dm_pattern_add(&p, "Test");
    CHECK(id == 0);
    CHECK(!dm_note_add(&p.patterns[0], (DmNote){-1, 1, 60, 100, 0}));
    CHECK(!dm_note_add(&p.patterns[0], (DmNote){0, 0, 60, 100, 0}));
    CHECK(!dm_note_add(&p.patterns[0], (DmNote){NAN, 1, 60, 100, 0}));
    CHECK(dm_note_add(&p.patterns[0], (DmNote){.13, .4, 60, 100, 1}));
    CHECK(dm_note_add(&p.patterns[0], (DmNote){.13, .4, 64, 100, 0}));
    dm_quantize(&p.patterns[0], .25);
    CHECK(p.patterns[0].notes[0].beat == .25);
    CHECK(p.patterns[0].notes[1].beat == .13);
    CHECK(!dm_clip_add(&p, (DmClip){0, 3, 0, 0, 0, 0}));
    CHECK(dm_clip_add(&p, (DmClip){0, 0, 0, 0, 0, 0}));
    CHECK(!dm_automation_add(&p, (DmAutomation){0, 0x90, 0, 60, 100, 0, 0, 0, 0, false}));
    CHECK(!dm_automation_add(&p, (DmAutomation){0, 0xb0, 16, 1, 100, 0, 0, 0, 0, false}));
    p.channels[0].mute = true;
    DmSchedule s = {0};
    CHECK(dm_schedule(&p, &s));

    for (size_t i = 0; i < s.count; ++i) {
        CHECK((s.items[i].status & 0xf0) != 0x90);
    }

    dm_schedule_free(&s);
    dm_project_free(&p);
}

static void multiport(void) {
    const uint8_t source[] = {'M',  'T', 'h', 'd',  0,    0,    0,  6,  0, 0,    0,    1, 1, 0xe0,
                              'M',  'T', 'r', 'k',  0,    0,    0,  32, 0, 0xff, 0x21, 1, 1, 0,
                              0x90, 60,  100, 0x83, 0x60, 0x80, 60, 0,  0, 0xff, 0x21, 1, 2, 0,
                              0x90, 65,  100, 0x83, 0x60, 0x80, 65, 0,  0, 0xff, 0x2f, 0};
    DmProject p;
    dm_project_init(&p);
    char err[256];
    CHECK(dm_import(&p, source, sizeof(source), 2, 0, err, sizeof(err)));
    CHECK(p.port_count == 2);
    CHECK(p.port == 2);
    CHECK(p.patterns[0].count == 1);
    CHECK(p.patterns[0].notes[0].key == 65);
    CHECK(p.original.size == sizeof(source));
    snprintf(p.name, sizeof(p.name), "Two ports");
    p.patterns[0].notes[0].key = 66;
    p.bpm = 133;
    DmBytes partial = {0};
    CHECK(dm_midi_all(&p, &partial, err, sizeof(err)));
    DmProject untouched;
    dm_project_init(&untouched);
    CHECK(dm_import(&untouched, partial.data, partial.size, 1, 0, err, sizeof(err)));
    CHECK(untouched.patterns[0].notes[0].key == 60);
    dm_project_free(&untouched);
    dm_bytes_free(&partial);
    CHECK(dm_view_switch(&p, 1, 0, err, sizeof(err)));
    CHECK(p.patterns[0].notes[0].key == 60);
    CHECK(p.bpm == 133);
    p.patterns[0].notes[0].key = 61;
    p.bpm = 155;
    CHECK(dm_view_switch(&p, 2, 0, err, sizeof(err)));
    CHECK(p.patterns[0].notes[0].key == 66);
    CHECK(p.bpm == 155);
    CHECK(!strcmp(p.name, "Two ports"));
    CHECK(p.original.size == sizeof(source) && !memcmp(p.original.data, source, sizeof(source)));
    DmBytes saved = {0};
    CHECK(dm_pack(&p, &saved));
    DmProject reopened;
    dm_project_init(&reopened);
    CHECK(dm_unpack(&reopened, saved.data, saved.size, err, sizeof(err)));
    CHECK(dm_view_switch(&reopened, 1, 0, err, sizeof(err)));
    CHECK(reopened.patterns[0].notes[0].key == 61);
    CHECK(reopened.bpm == 155);
    CHECK(!dm_view_switch(&reopened, 77, 0, err, sizeof(err)));
    CHECK(reopened.patterns[0].notes[0].key == 61);
    DmBytes merged = {0};
    CHECK(dm_midi_all(&p, &merged, err, sizeof(err)));
    CHECK(dm_import(&untouched, merged.data, merged.size, 1, 0, err, sizeof(err)));
    CHECK(untouched.patterns[0].notes[0].key == 61);
    CHECK(fabs(untouched.bpm - 155) < .001);
    CHECK(dm_import(&untouched, merged.data, merged.size, 2, 0, err, sizeof(err)));
    CHECK(untouched.patterns[0].notes[0].key == 66);
    CHECK(untouched.port_count == 2);
    CHECK(dm_write("agents/reports/merged-ports.mid", merged.data, merged.size, err, sizeof(err)));
    dm_bytes_free(&merged);
    dm_project_free(&untouched);
    dm_bytes_free(&saved);
    dm_project_free(&reopened);
    dm_project_free(&p);
}

static void malformed(void) {
    DmProject p;
    dm_project_init(&p);
    char error[256];

    for (size_t n = 0; n < sizeof(fixture); ++n) {
        bool ok = dm_import(&p, fixture, n, 0, 0, error, sizeof(error));
        CHECK(!ok);
    }

    unsigned char mutated[sizeof(fixture)];
    memcpy(mutated, fixture, sizeof(fixture));
    mutated[12] = 0;
    mutated[13] = 0;
    CHECK(!dm_import(&p, mutated, sizeof(mutated), 0, 0, error, sizeof(error)));
    uint32_t state = 12345;

    for (int n = 0; n < 3000; ++n) {
        uint8_t bytes[256];

        for (size_t i = 0; i < sizeof(bytes); ++i) {
            state = state * 1664525 + 1013904223;
            bytes[i] = (uint8_t)(state >> 24);
        }

        CHECK(!dm_import(&p, bytes, sizeof(bytes), 0, 0, error, sizeof(error)));
    }

    dm_project_free(&p);
}

static void advanced(void) {
    DmProject p, next;
    dm_project_init(&p);
    dm_project_init(&next);
    char error[256];
    dm_pattern_add(&p, "Expression");
    dm_note_add(&p.patterns[0], (DmNote){0, 1, 60, 100, 0});
    CHECK(dm_automation_add(&p, (DmAutomation){.5, 0xb0, 0, 1, 83, 0, 0, 0, 0, false}));
    dm_clip_add(&p, (DmClip){0, 0, 0, 0, 0, 0});
    dm_clip_add(&p, (DmClip){4, 0, 1, 0, 0, 0});
    DmSchedule schedule = {0};
    CHECK(dm_schedule(&p, &schedule));
    int controllers = 0;

    for (size_t i = 0; i < schedule.count; ++i) {
        if ((schedule.items[i].status & 0xf0) == 0xb0 && schedule.items[i].a == 1) {
            ++controllers;
            CHECK(schedule.items[i].b == 83);
        }
    }

    CHECK(controllers == 2);
    CHECK(schedule.beat_count == 5);
    CHECK(schedule.beats[4] == 2);
    dm_schedule_free(&schedule);
    int clone = dm_pattern_copy(&p, 0);
    CHECK(clone == 1);
    CHECK(p.automation_count == 2);
    dm_pattern_remove(&p, 0);
    CHECK(p.pattern_count == 1 && p.clip_count == 0);
    CHECK(p.automation_count == 1 && p.automation[0].pattern == 0);
    DmBytes packed = {0};
    CHECK(dm_pack(&p, &packed));
    CHECK(dm_unpack(&next, packed.data, packed.size, error, sizeof(error)));
    dm_bytes_free(&packed);
    CHECK(dm_write("agents/reports/atomic-check.txt", "original", 8, error, sizeof(error)));
    DmOutput output = {0};
    CHECK(dm_output_begin(&output, "agents/reports/atomic-check.txt", error, sizeof(error)));
    CHECK(dm_output_write(&output, "replacement", 11, error, sizeof(error)));
    CHECK(!dm_output_finish(&output, false, error, sizeof(error)));
    DmBytes preserved = {0};
    CHECK(dm_read("agents/reports/atomic-check.txt", &preserved, error, sizeof(error)));
    CHECK(preserved.size == 8 && !memcmp(preserved.data, "original", 8));
    dm_bytes_free(&preserved);
    CHECK(dm_write("agents/reports/unicode-音符.txt", "UTF-8 path", 10, error, sizeof(error)));
    unsigned char smpte[sizeof(fixture)];
    memcpy(smpte, fixture, sizeof(fixture));
    smpte[12] = 0xe7;
    smpte[13] = 40;
    CHECK(dm_import(&next, smpte, sizeof(smpte), 0, 0, error, sizeof(error)));
    CHECK(fabs(next.patterns[0].notes[0].length - .96) < 1e-9);
    CHECK(fabs(dm_beat_seconds(&next, next.patterns[0].notes[0].length) - .48) < 1e-9);
    dm_project_free(&p);
    dm_project_free(&next);
}

static void mutated_files(void) {
    uint32_t state = 7321;
    char error[256];
    int accepted = 0;
    DmProject p, next;
    dm_project_init(&p);
    dm_project_init(&next);

    for (int i = 0; i < 4000; ++i) {
        uint8_t data[sizeof(fixture)];
        memcpy(data, fixture, sizeof(data));
        state = state * 1664525 + 1013904223;
        size_t at = state % sizeof(data);
        state = state * 1664525 + 1013904223;
        data[at] ^= (uint8_t)(1u << (state % 8));

        if (!dm_import(&p, data, sizeof(data), 0, 0, error, sizeof(error))) {
            continue;
        }

        ++accepted;
        CHECK(p.original.size == sizeof(data));
        DmBytes packed = {0};
        CHECK(dm_pack(&p, &packed));
        CHECK(dm_unpack(&next, packed.data, packed.size, error, sizeof(error)));
        dm_bytes_free(&packed);
    }

    CHECK(accepted > 500);
    printf("Mutated valid MIDI cases accepted and project-roundtripped: %d / 4000\n", accepted);
    dm_project_free(&p);
    dm_project_free(&next);
}

static void independent_sequences(void) {
    const uint8_t source[] = {'M',  'T',  'h',  'd',  0,    0,    0,    6, 0,    2,    0,  2,
                              1,    0xe0, 'M',  'T',  'r',  'k',  0,    0, 0,    13,   0,  0x90,
                              60,   100,  0x83, 0x60, 0x80, 60,   0,    0, 0xff, 0x2f, 0,  'M',
                              'T',  'r',  'k',  0,    0,    0,    13,   0, 0x90, 67,   90, 0x83,
                              0x60, 0x80, 67,   0,    0,    0xff, 0x2f, 0};
    DmProject p, next;
    dm_project_init(&p);
    dm_project_init(&next);
    char error[256] = {0};
    CHECK(dm_import(&p, source, sizeof(source), 0, 0, error, sizeof(error)));
    CHECK(p.sequence_count == 2);
    p.patterns[0].notes[0].key = 61;
    p.bpm = 130;
    CHECK(dm_view_switch(&p, 0, 1, error, sizeof(error)));
    CHECK(p.patterns[0].notes[0].key == 67 && p.bpm == 120);
    p.patterns[0].notes[0].key = 68;
    p.bpm = 150;
    CHECK(dm_view_switch(&p, 0, 0, error, sizeof(error)));
    CHECK(p.patterns[0].notes[0].key == 61 && p.bpm == 130);
    DmBytes output = {0};
    CHECK(dm_midi_all(&p, &output, error, sizeof(error)));
    CHECK(output.data[9] == 2 && output.data[11] == 2);
    CHECK(dm_import(&next, output.data, output.size, 0, 0, error, sizeof(error)));
    CHECK(next.patterns[0].notes[0].key == 61);
    CHECK(fabs(next.bpm - 130) < .001);
    CHECK(dm_import(&next, output.data, output.size, 0, 1, error, sizeof(error)));
    CHECK(next.patterns[0].notes[0].key == 68 && next.bpm == 150);
    CHECK(dm_write("agents/reports/merged-sequences.mid", output.data, output.size, error,
                   sizeof(error)));
    dm_bytes_free(&output);
    dm_project_free(&p);
    dm_project_free(&next);
}

static void pattern_meter(void) {
    DmProject p;
    dm_project_init(&p);
    p.numerator = 3;
    int id = dm_pattern_add(&p, "Waltz");
    CHECK(id == 0 && p.patterns[0].length == 1);
    CHECK(dm_note_add(&p.patterns[0], (DmNote){3, .5, 60, 100, 0}));
    dm_pattern_fit(&p, &p.patterns[0]);
    CHECK(p.patterns[0].length == 4);
    CHECK(dm_automation_add(&p, (DmAutomation){6.25, 0xb0, 0, 1, 64, id, 0, 0, 0, false}));
    dm_pattern_fit(&p, &p.patterns[0]);
    CHECK(p.patterns[0].length == 4);
    dm_project_free(&p);
}

static void bounded_curves(void) {
    DmProject p, next;
    dm_project_init(&p);
    dm_project_init(&next);
    int id = dm_pattern_add(&p, "Curve fixture");
    CHECK(dm_note_add(&p.patterns[id], (DmNote){0, 3, 60, 100, 0}));
    CHECK(dm_clip_add(&p, (DmClip){0, id, 0, 0, 0, 0}));
    CHECK(dm_automation_add(&p, (DmAutomation){.5, 0xe0, 0, 0, 64, id, 7, 60, 0, false}));
    CHECK(dm_automation_add(&p, (DmAutomation){1.5, 0xe0, 0, 0, 64, id, -7, 60, 0, false}));
    CHECK(dm_automation_add(&p, (DmAutomation){1.5, 0xe0, 0, 0, 96, id, 7, 60, 0, false}));
    DmSchedule schedule = {0};
    CHECK(dm_schedule(&p, &schedule));
    bool middle = false, restore = false;

    for (size_t i = 0; i < schedule.count; ++i) {
        DmScheduled a = schedule.items[i];
        int value = a.a + (a.b << 7);

        if (a.status == 0xe0 && fabs(a.seconds - .5) < .001 && abs(value - 10240) < 3) {
            middle = true;
        }

        if (a.status == 0xe0 && a.seconds >= .75 && value == 8192) {
            restore = true;
        }
    }

    CHECK(middle && restore);
    DmBytes packed = {0};
    char error[256] = {0};
    CHECK(dm_pack(&p, &packed));
    CHECK(dm_unpack(&next, packed.data, packed.size, error, sizeof(error)));
    CHECK(next.automation[0].curve == 7 && next.automation[0].anchor == 60);
    CHECK(next.automation[2].curve == -7);
    CHECK(dm_automation_add(&p, (DmAutomation){1.5, 0xb0, 0, 6, 2, id, -7, 0, 0, false}));
    CHECK(dm_automation_add(&p, (DmAutomation){1.5, 0xb0, 0, 6, 24, id, 8, 0, 0, false}));
    CHECK(dm_automation_add(&p, (DmAutomation){1.5, 0xe0, 0, 40, 70, id, 8, 60, 0, false}));
    CHECK(dm_automation_add(&p, (DmAutomation){2.5, 0xe0, 0, 0, 96, id, 8, 60, 0, false}));
    DmSchedule adjacent = {0};
    CHECK(dm_schedule(&p, &adjacent));
    int bend = -1, range = -1;

    for (size_t i = 0; i < adjacent.count; ++i) {
        DmScheduled a = adjacent.items[i];

        if (fabs(a.seconds - .75) > .00001) {
            continue;
        }

        if (a.status == 0xe0) {
            bend = a.a + (a.b << 7);
        }

        if (a.status == 0xb0 && a.a == 6) {
            range = a.b;
        }
    }

    CHECK(bend == 40 + (70 << 7) && range == 24);
    dm_schedule_free(&adjacent);
    dm_schedule_free(&schedule);
    dm_bytes_free(&packed);
    dm_project_free(&p);
    dm_project_free(&next);
}

static void mus_input(void) {
    const uint8_t data[] = {'M', 'U', 'S', 0x1a, 9,    0,   16,   0,    1,    0,  0, 0,   0,
                            0,   0,   0,   0x90, 0xbc, 100, 0x81, 0x0c, 0x80, 60, 0, 0x60};
    DmProject p;
    dm_project_init(&p);
    char error[256] = {0};
    CHECK(dm_import(&p, data, sizeof(data), -1, 0, error, sizeof(error)));
    CHECK(p.pattern_count == 1 && p.patterns[0].count == 1);
    CHECK(p.patterns[0].notes[0].key == 60 && fabs(p.patterns[0].notes[0].length - 2) < 1e-9);
    CHECK(p.original.size > 4 && !memcmp(p.original.data, "MThd", 4));

    for (size_t i = 0; i < sizeof(data); ++i) {
        CHECK(!dm_import(&p, data, i, -1, 0, error, sizeof(error)));
    }

    CHECK(p.patterns[0].count == 1);
    dm_project_free(&p);
}

static size_t find_bytes(const DmBytes *b, const void *needle, size_t size, bool last) {
    size_t found = SIZE_MAX;

    for (size_t i = 0; i + size <= b->size; ++i) {
        if (!memcmp(b->data + i, needle, size)) {
            found = i;

            if (!last) {
                break;
            }
        }
    }

    return found;
}

static void midi_metadata(void) {
    DmProject p, restored, raw, stripped;
    dm_project_init(&p);
    dm_project_init(&restored);
    dm_project_init(&raw);
    dm_project_init(&stripped);
    char error[256];
    CHECK(dm_import(&p, fixture, sizeof(fixture), -1, 0, error, sizeof(error)));
    strcpy(p.name, "Metadata roundtrip");
    strcpy(p.soundfont, "test-only.sf2");
    strcpy(p.patterns[0].name, "Reusable notes");
    p.patterns[0].notes[0].selected = 1;
    CHECK(dm_clip_add(&p, (DmClip){8, 0, 0, 1, 0, 0}));
    CHECK(dm_automation_add(&p, (DmAutomation){0, 0xe0, 0, 0, 64, 0, 1, 60, 400, false}));
    CHECK(dm_automation_add(&p, (DmAutomation){2, 0xe0, 0, 0, 80, 0, 1, 60, 0, false}));
    int lfo = dm_pattern_add(&p, "Reusable LFO");
    CHECK(lfo >= 0);
    p.patterns[lfo].kind = p.patterns[lfo].mode = 1;
    p.patterns[lfo].event = 1;
    p.patterns[lfo].speed = 2;
    p.patterns[lfo].inverted = true;
    p.lane_count[1] = 1;
    strcpy(p.lane_names[1][0], "Expression lane");
    p.channels[1].enabled = true;
    p.channels[1].mute = true;
    CHECK(dm_clip_add(&p, (DmClip){0, lfo, 1, 0, 3, 1}));
    const uint8_t foreign[] = {0xff, 0x7f, 5, 5, 15, 35, 99, 88};
    const uint8_t marker[] = {0xff, 6, 4, 'T', 'e', 's', 't'};
    const uint8_t lyric[] = {0xff, 5, 2, 'L', 'a'};
    const uint8_t sysex[] = {0xf0, 7, 0x7f, 0x7f, 4, 1, 0, 100, 0xf7};
    const uint8_t rpn[] = {0xb0, 101, 0};
    DmBytes messages[] = {{(uint8_t *)foreign, sizeof(foreign), 0},
                          {(uint8_t *)marker, sizeof(marker), 0},
                          {(uint8_t *)lyric, sizeof(lyric), 0},
                          {(uint8_t *)sysex, sizeof(sysex), 0},
                          {(uint8_t *)rpn, sizeof(rpn), 0}};

    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i) {
        CHECK(dm_event_add(&p, 0, 0, 0, &messages[i]));
    }

    DmBytes midi = {0}, again = {0}, clean = {0}, plain = {0}, before = {0}, after = {0};
    CHECK(dm_midi_export(&p, &midi, true, DM_METADATA_KEEP_ALL, error, sizeof(error)));
    /* A single-sequence song without source views is fully described by the export, so the
       export leaves out the source copy; everything else must round-trip unchanged. */
    dm_bytes_free(&p.original);
    CHECK(dm_pack(&p, &before));
    CHECK(dm_import(&restored, midi.data, midi.size, -1, 0, error, sizeof(error)));
    CHECK(strstr(error, "editor information") != NULL);
    CHECK(dm_pack(&restored, &after));
    CHECK(before.size == after.size && !memcmp(before.data, after.data, before.size));
    CHECK(restored.automation_count == 2 && restored.pattern_count == 2);

    for (int i = 0; i < 4; ++i) {
        CHECK(dm_midi_export(&restored, &again, true, DM_METADATA_KEEP_ALL, error, sizeof(error)));
        CHECK(midi.size == again.size && !memcmp(midi.data, again.data, midi.size));
        CHECK(dm_import(&restored, again.data, again.size, -1, 0, error, sizeof(error)));
        dm_bytes_free(&again);
    }

    /* File exports leave muted channels out; compare stripped and plain renders of all channels. */
    p.channels[1].mute = false;
    CHECK(dm_midi_export(&p, &clean, true, DM_METADATA_STRIP_ALL, error, sizeof(error)));
    CHECK(find_bytes(&midi, foreign, sizeof(foreign), false) != SIZE_MAX);
    DmBytes only = {0};
    CHECK(dm_midi_export(&p, &only, true, DM_METADATA_DUCKMIDI_ONLY, error, sizeof(error)));
    CHECK(find_bytes(&only, foreign, sizeof(foreign), false) == SIZE_MAX);
    CHECK(find_bytes(&only, "DUCKMIDI", 8, false) != SIZE_MAX);
    CHECK(dm_import(&restored, only.data, only.size, -1, 0, error, sizeof(error)));
    CHECK(strstr(error, "with duckmidi editor information") != NULL);
    CHECK(restored.automation_count == 2 && restored.pattern_count == 2);
    CHECK(!strcmp(restored.name, p.name) && restored.patterns[lfo].inverted);
    CHECK(dm_midi_export(&restored, &again, true, DM_METADATA_DUCKMIDI_ONLY, error, sizeof(error)));
    CHECK(only.size == again.size && !memcmp(only.data, again.data, only.size));
    dm_bytes_free(&only);
    dm_bytes_free(&again);
    CHECK(!dm_midi_export(&p, &only, true, (DmMetadataMode)99, error, sizeof(error)));
    CHECK(!only.size && strstr(error, "metadata export mode"));
    CHECK(find_bytes(&clean, foreign, sizeof(foreign), false) == SIZE_MAX);
    CHECK(find_bytes(&clean, "DUCKMIDI", 8, false) == SIZE_MAX);
    CHECK(find_bytes(&clean, marker, sizeof(marker), false) != SIZE_MAX);
    CHECK(find_bytes(&clean, lyric, sizeof(lyric), false) != SIZE_MAX);
    CHECK(find_bytes(&clean, sysex, sizeof(sysex), false) != SIZE_MAX);
    CHECK(find_bytes(&clean, rpn, sizeof(rpn), false) != SIZE_MAX);
    CHECK(dm_midi_all(&p, &plain, error, sizeof(error)));
    CHECK(dm_import(&raw, plain.data, plain.size, -1, 0, error, sizeof(error)));
    CHECK(dm_import(&stripped, clean.data, clean.size, -1, 0, error, sizeof(error)));
    CHECK(stripped.automation_count > p.automation_count);
    DmSchedule a = {0}, b = {0};
    CHECK(dm_schedule(&raw, &a) && dm_schedule(&stripped, &b));
    CHECK(a.count == b.count && a.duration == b.duration);

    for (size_t i = 0; i < a.count; ++i) {
        CHECK(a.items[i].seconds == b.items[i].seconds && a.items[i].status == b.items[i].status &&
              a.items[i].a == b.items[i].a && a.items[i].b == b.items[i].b &&
              a.items[i].sysex.size == b.items[i].sysex.size);
        CHECK(!a.items[i].sysex.size ||
              !memcmp(a.items[i].sysex.data, b.items[i].sysex.data, a.items[i].sysex.size));
    }

    dm_schedule_free(&a);
    dm_schedule_free(&b);
    const uint8_t note[] = {0x90, 60, 100};
    size_t ed_changed = find_bytes(&midi, note, sizeof(note), true);
    CHECK(ed_changed != SIZE_MAX);
    midi.data[ed_changed + 2] = 99;
    CHECK(dm_import(&restored, midi.data, midi.size, -1, 0, error, sizeof(error)));
    CHECK(strstr(error, "stale") != NULL);
    CHECK(strcmp(restored.name, p.name));
    bool edited_note = false;

    for (size_t i = 0; i < restored.patterns[0].count; ++i) {
        edited_note |= restored.patterns[0].notes[i].velocity == 99;
    }

    CHECK(edited_note);
    CHECK(find_bytes(&restored.original, "DUCKMIDI", 8, false) == SIZE_MAX);
    midi.data[ed_changed + 2] = 100;
    size_t tag = find_bytes(&midi, "DUCKMIDI", 8, false);
    CHECK(tag != SIZE_MAX);
    midi.data[tag + 11] = 2; /* Envelope schema, big endian. */
    CHECK(!dm_import(&restored, midi.data, midi.size, -1, 0, error, sizeof(error)));
    CHECK(strstr(error, "schema 1") != NULL);
    CHECK(strcmp(restored.name, p.name)); /* Failed import is atomic. */
    midi.data[tag + 11] = 1;
    size_t project = find_bytes(&midi, "DUCKMIDI", 8, true);
    CHECK(project > tag);
    midi.data[project + 16] ^= 1; /* Project CRC. */
    CHECK(!dm_import(&restored, midi.data, midi.size, -1, 0, error, sizeof(error)));
    CHECK(strstr(error, "checksum") != NULL);
    midi.data[project + 16] ^= 1;
    /* Duplicate snapshots are rejected even when both payloads are valid. */
    size_t payload_start = 25;
    uint32_t payload_size = 0;
    uint8_t length_byte;

    do {
        length_byte = midi.data[payload_start++];
        payload_size = (payload_size << 7) | (length_byte & 127);
    } while (length_byte & 128);
    size_t event_size = payload_start + payload_size - 22;
    DmBytes duplicate = {0};
    CHECK(dm_bytes(&duplicate, midi.data, 22));
    CHECK(dm_bytes(&duplicate, midi.data + 22, event_size));
    CHECK(dm_bytes(&duplicate, midi.data + 22, midi.size - 22));
    uint32_t track_size = ((uint32_t)midi.data[18] << 24) | ((uint32_t)midi.data[19] << 16) |
                          ((uint32_t)midi.data[20] << 8) | midi.data[21];
    track_size += (uint32_t)event_size;

    for (int i = 0; i < 4; ++i) {
        duplicate.data[18 + i] = (uint8_t)(track_size >> ((3 - i) * 8));
    }

    CHECK(!dm_import(&restored, duplicate.data, duplicate.size, -1, 0, error, sizeof(error)));
    CHECK(strstr(error, "duplicate") != NULL);
    CHECK(!dm_import(&restored, midi.data, payload_start + payload_size - 1, -1, 0, error,
                     sizeof(error)));
    dm_bytes_free(&duplicate);
    dm_bytes_free(&after);
    p.channels[1].mute = true;
    CHECK(dm_pack(&p, &after));
    CHECK(before.size == after.size && !memcmp(before.data, after.data, before.size));
    dm_bytes_free(&midi);
    dm_bytes_free(&again);
    dm_bytes_free(&clean);
    dm_bytes_free(&plain);
    dm_bytes_free(&before);
    dm_bytes_free(&after);
    dm_project_free(&p);
    dm_project_free(&restored);
    dm_project_free(&raw);
    dm_project_free(&stripped);
}

static void midi_metadata_views(void) {
    for (int format = 1; format <= 2; ++format) {
        uint8_t head[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, (uint8_t)format, 0, 2, 0, 96};
        uint8_t track[] = {'M', 'T', 'r', 'k', 0,   0,  0,   29, 0,   255, 33, 1, 1,
                           0,   192, 0,   0,   144, 60, 100, 96, 128, 60,  0,  0, 255,
                           127, 5,   5,   15,  35,  99, 88,  0,  255, 47,  0};
        const uint8_t foreign[] = {255, 127, 5, 5, 15, 35, 99, 88};
        DmBytes source = {0}, midi = {0}, packed = {0}, restored = {0}, active = {0};
        CHECK(dm_bytes(&source, head, sizeof(head)));
        CHECK(dm_bytes(&source, track, sizeof(track)));
        track[12] = 2;
        track[18] = track[22] = 67;
        track[14] = 0xc1;
        track[17] = 0x91;
        track[21] = 0x81;
        CHECK(dm_bytes(&source, track, sizeof(track)));
        DmProject p, q;
        dm_project_init(&p);
        dm_project_init(&q);
        char error[256];
        CHECK(dm_import(&p, source.data, source.size, 1, 0, error, sizeof(error)));
        strcpy(p.patterns[0].name, "Port one private edit");
        CHECK(dm_view_switch(&p, 2, format == 2 ? 1 : 0, error, sizeof(error)));
        strcpy(p.patterns[0].name, "Port two edit");
        p.patterns[0].notes[0].velocity = 83;
        CHECK(dm_pack(&p, &packed));
        CHECK(dm_midi_export(&p, &midi, true, DM_METADATA_KEEP_ALL, error, sizeof(error)));
        CHECK(find_bytes(&midi, foreign, sizeof(foreign), false) != SIZE_MAX);
        CHECK(dm_import(&q, midi.data, midi.size, -1, 0, error, sizeof(error)));
        CHECK(q.port == 2 && q.sequence == (format == 2 ? 1 : 0));
        CHECK(dm_pack(&q, &restored));
        CHECK(packed.size == restored.size && !memcmp(packed.data, restored.data, packed.size));
        CHECK(dm_view_switch(&q, 1, 0, error, sizeof(error)));
        CHECK(!strcmp(q.patterns[0].name, "Port one private edit"));
        CHECK(dm_view_switch(&q, 2, format == 2 ? 1 : 0, error, sizeof(error)));
        CHECK(q.patterns[0].notes[0].velocity == 83);

        if (format == 1) {
            CHECK(
                dm_import(&q, source.data, source.size, DM_PORT_COMBINED, 0, error, sizeof(error)));
            CHECK(q.pattern_count == 2);
            dm_bytes_free(&active);
            CHECK(dm_midi_export(&q, &active, true, DM_METADATA_KEEP_ALL, error, sizeof(error)));
            CHECK(dm_import(&q, active.data, active.size, -1, 0, error, sizeof(error)));
            CHECK(q.port == DM_PORT_COMBINED && q.pattern_count == 2);
        }

        /* Explicit source-view requests are honored on import. */
        CHECK(dm_import(&q, midi.data, midi.size, 1, 0, error, sizeof(error)));
        CHECK(q.port == 1 && !strcmp(q.patterns[0].name, "Port one private edit"));
        dm_bytes_free(&active);
        CHECK(dm_midi_export(&p, &active, false, DM_METADATA_KEEP_ALL, error, sizeof(error)));
        CHECK(find_bytes(&active, "Port one private edit", 21, false) == SIZE_MAX);
        CHECK(dm_import(&q, active.data, active.size, -1, 0, error, sizeof(error)));
        CHECK(q.port_count == 1 && q.sequence_count == 1 && q.port == 0 && q.sequence == 0);
        CHECK(!q.view_count && !q.original.size && !strcmp(q.patterns[0].name, "Port two edit"));
        CHECK(q.patterns[0].notes[0].velocity == 83);
        dm_bytes_free(&active);
        CHECK(dm_midi_export(&p, &active, true, DM_METADATA_STRIP_ALL, error, sizeof(error)));
        CHECK(find_bytes(&active, "DUCKMIDI", 8, false) == SIZE_MAX);
        CHECK(
            dm_import(&q, active.data, active.size, 2, format == 2 ? 1 : 0, error, sizeof(error)));
        CHECK(q.patterns[0].notes[0].velocity == 83);
        dm_bytes_free(&active);
        CHECK(dm_midi_export(&p, &active, true, DM_METADATA_DUCKMIDI_ONLY, error, sizeof(error)));
        /* Search the entire output, including the embedded original and packed views. */
        CHECK(find_bytes(&active, foreign, sizeof(foreign), false) == SIZE_MAX);
        CHECK(dm_import(&q, active.data, active.size, -1, 0, error, sizeof(error)));
        CHECK(strstr(error, "with duckmidi editor information") != NULL);
        CHECK(q.port == 2 && q.patterns[0].notes[0].velocity == 83);
        CHECK(find_bytes(&q.original, foreign, sizeof(foreign), false) == SIZE_MAX);

        for (size_t i = 0; i < q.view_count; ++i) {
            CHECK(find_bytes(&q.views[i].state, foreign, sizeof(foreign), false) == SIZE_MAX);
        }

        CHECK(dm_view_switch(&q, 1, 0, error, sizeof(error)));
        CHECK(!strcmp(q.patterns[0].name, "Port one private edit"));
        CHECK(dm_view_switch(&q, 2, format == 2 ? 1 : 0, error, sizeof(error)));
        CHECK(q.patterns[0].notes[0].velocity == 83);
        dm_bytes_free(&restored);
        CHECK(dm_pack(&p, &restored));
        CHECK(packed.size == restored.size && !memcmp(packed.data, restored.data, packed.size));
        CHECK(find_bytes(&p.original, foreign, sizeof(foreign), false) != SIZE_MAX);
        dm_bytes_free(&active);
        CHECK(dm_midi_export(&p, &active, false, DM_METADATA_DUCKMIDI_ONLY, error, sizeof(error)));
        CHECK(find_bytes(&active, foreign, sizeof(foreign), false) == SIZE_MAX);
        CHECK(dm_import(&q, active.data, active.size, -1, 0, error, sizeof(error)));
        CHECK(!q.view_count && !q.original.size && q.patterns[0].notes[0].velocity == 83);
        dm_bytes_free(&source);
        dm_bytes_free(&midi);
        dm_bytes_free(&packed);
        dm_bytes_free(&restored);
        dm_bytes_free(&active);
        dm_project_free(&p);
        dm_project_free(&q);
    }
}

int main(void) {
    midi_metadata();
    midi_metadata_views();
    bounded_curves();
    mus_input();
    pattern_meter();
    roundtrip();
    model();
    multiport();
    malformed();
    advanced();
    independent_sequences();
    mutated_files();
    printf("PASS: %d checks (MIDI, editor metadata, tempo, projects, corruption, ports, malformed "
           "data)\n",
           checks);
    return 0;
}
