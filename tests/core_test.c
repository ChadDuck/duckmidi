#include "core/duck.h"
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
    CHECK(dm_playback_bpm(&p, .499) == 120 && dm_playback_bpm(&p, .5) == 60);
    p.bpm = 100;
    CHECK(dm_playback_bpm(&p, .599) == 100 && dm_playback_bpm(&p, .6) == 60);
    p.bpm = 120;
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

static int scheduled_control(const DmSchedule *s, int controller, double seconds) {
    int value = -1;

    for (size_t i = 0; i < s->count; ++i) {
        if (s->items[i].status == 0xb0 && s->items[i].a == controller &&
            s->items[i].seconds <= seconds + 1e-9) {
            value = s->items[i].b;
        }
    }

    return value;
}

/* Files often set volume and pan shortly after the start. Import makes them the mixer settings,
   so a mixer change is what plays until the song's next change. */
static void opening_controls(void) {
    const uint8_t source[] = {'M',  'T',  'h',  'd',  0,    0,    0,    6,    0,    0,    0,
                              1,    0,    0x60, 'M',  'T',  'r',  'k',  0,    0,    0,    33,
                              0x60, 0xb0, 10,   40,   0,    0xb0, 7,    80,   0x60, 0x90, 60,
                              100,  0x60, 0x80, 60,   0,    0x81, 0x40, 0xb0, 10,   127,  0x60,
                              0x90, 62,   100,  0x60, 0x80, 62,   0,    0,    0xff, 0x2f, 0};
    DmProject p;
    dm_project_init(&p);
    char err[256];
    CHECK(dm_import(&p, source, sizeof(source), 0, 0, err, sizeof(err)));
    CHECK(p.channels[0].pan == 40 && p.channels[0].volume == 80);
    p.channels[0].pan = 100;
    p.channels[0].volume = 30;
    DmSchedule s = {0};
    CHECK(dm_schedule(&p, &s));
    double note = dm_beat_seconds(&p, 2.5), later = dm_beat_seconds(&p, 5.5);
    CHECK(scheduled_control(&s, 10, note) == 100);
    CHECK(scheduled_control(&s, 7, note) == 30);
    CHECK(scheduled_control(&s, 10, later) == 127);
    dm_schedule_free(&s);
    dm_project_free(&p);
}

static unsigned default_pan_flags(const DmProject *p) {
    DmExportEvent *events = NULL;
    size_t count;
    uint64_t duration;
    char error[256] = {0};
    CHECK(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)));
    unsigned flags = 0;

    /* Mirrors the export host: authored channels clear the default flag. */
    unsigned authored = dm_export_authored_pan(p);

    for (size_t i = 0; i < count; i++) {
        if (events[i].status == MIDI_CONTROL && events[i].a == MIDI_CC_PAN && events[i].tick == 0 &&
            !(authored & 1u)) {
            flags |= events[i].flags;
        }
    }

    free(events);
    return flags;
}

static void pan_provenance(void) {
    DmProject p;
    dm_project_init(&p);
    p.channels[0].enabled = true;
    int pattern = dm_pattern_add(&p, "Pan provenance");
    CHECK(pattern == 0 && dm_note_add(&p.patterns[0], (DmNote){0, 1, 60, 100, 0}));
    CHECK(dm_clip_add(&p, (DmClip){.pattern = 0, .channel = 0, .length = 4}));
    CHECK(default_pan_flags(&p) == DM_EXPORT_DEFAULT_CONTROL);
    p.clips[0].length = 0;
    CHECK(default_pan_flags(&p) == DM_EXPORT_DEFAULT_CONTROL);
    p.clips[0].length = 4;
    CHECK(dm_automation_add(
        &p,
        (DmAutomation){.beat = .5, .type = MIDI_CONTROL, .a = MIDI_CC_PAN, .b = 64, .pattern = 0}));
    CHECK(!default_pan_flags(&p));
    p.automation_count = 0;
    p.channels[0].pan = 32;
    CHECK(!default_pan_flags(&p));
    p.channels[0].pan = 64;
    DmBytes pan = {0};
    const uint8_t bytes[] = {MIDI_CONTROL, MIDI_CC_PAN, 64};
    CHECK(dm_bytes(&pan, bytes, sizeof(bytes)) && dm_event_add(&p, 0, 0, 0, &pan));
    CHECK(!default_pan_flags(&p));
    dm_bytes_free(&pan);
    dm_project_free(&p);

    const uint8_t imported[] = {'M',
                                'T',
                                'h',
                                'd',
                                0,
                                0,
                                0,
                                6,
                                0,
                                0,
                                0,
                                1,
                                0,
                                96,
                                'M',
                                'T',
                                'r',
                                'k',
                                0,
                                0,
                                0,
                                16,
                                0,
                                MIDI_CONTROL,
                                MIDI_CC_PAN,
                                64,
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
                                MIDI_META_END,
                                0};
    dm_project_init(&p);
    char error[256] = {0};
    CHECK(dm_import(&p, imported, sizeof(imported), 0, 0, error, sizeof(error)));
    CHECK(p.channels[0].pan == 64 && !default_pan_flags(&p));
    DmBytes packed = {0};
    CHECK(dm_pack(&p, &packed));
    dm_project_free(&p);
    dm_project_init(&p);
    CHECK(dm_unpack(&p, packed.data, packed.size, error, sizeof(error)));
    CHECK(!default_pan_flags(&p));
    dm_bytes_free(&packed);
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
        DmPatchReplacement first_sample = {.generation = 1,
                                           .locator = "generated/missing-library",
                                           .set_id = "view-set",
                                           .preset_id = "first"};
        CHECK(dm_patch_replacement_set(&p, &first_sample));
        CHECK(dm_view_switch(&p, 2, format == 2 ? 1 : 0, error, sizeof(error)));
        CHECK(!p.patch_replacement_count);
        DmPatchReplacement second_sample = first_sample;
        second_sample.key.channel = 1;
        second_sample.generation = 2;
        strcpy(second_sample.preset_id, "second");
        CHECK(dm_patch_replacement_set(&p, &second_sample));
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
        CHECK(q.patch_replacement_count == 1 &&
              dm_patch_replacement(&q, first_sample.key)->generation == 1);
        CHECK(dm_view_switch(&q, 2, format == 2 ? 1 : 0, error, sizeof(error)));
        CHECK(q.patterns[0].notes[0].velocity == 83);
        CHECK(q.patch_replacement_count == 1 &&
              dm_patch_replacement(&q, second_sample.key)->generation == 1);

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
        CHECK(!q.patch_replacement_count);
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
        CHECK(q.patch_replacement_count == 1 &&
              dm_patch_replacement(&q, first_sample.key)->generation == 1);
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

/* A one-track file from raw track bytes (delta times included). */
static DmBytes one_track(const unsigned char *track, size_t size) {
    DmBytes file = {0};
    const unsigned char header[] = {
        'M', 'T', 'h', 'd', 0,   0,   0,   6, 0, 0, 0,
        1,   0,   96,  'M', 'T', 'r', 'k', 0, 0, 0, (unsigned char)size};
    CHECK(size < 256 && dm_bytes(&file, header, sizeof(header)) && dm_bytes(&file, track, size));
    return file;
}

static void program_repeats(void) {
    /* clang-format off */
    const uint8_t first_track[] = {
        0, MIDI_PROGRAM, 4,
        0, MIDI_CONTROL, MIDI_CC_VOLUME, 100,
        0, MIDI_CONTROL, MIDI_CC_PAN, 64,
        0, MIDI_PROGRAM, 4, /* Same setup repeated. */
        0, MIDI_CONTROL, MIDI_CC_VOLUME, 100,
        0, MIDI_CONTROL, MIDI_CC_PAN, 64,
        0, MIDI_PROGRAM | 1, 4, /* Different channel. */
        0, MIDI_CONTROL, MIDI_CC_BANK, 1,
        0, MIDI_PROGRAM, 4, /* New bank. */
        0, MIDI_PROGRAM, 5,
        0, MIDI_PROGRAM, 4, /* Return to program 4. */
        0, MIDI_NOTE_ON, 60, 100,
        0, MIDI_PROGRAM, 4, /* A note intervened. */
        0, MIDI_NOTE_ON | 1, 61, 100,
        96, MIDI_NOTE_OFF, 60, 0,
        0, MIDI_NOTE_OFF | 1, 61, 0,
        0, MIDI_PROGRAM, 4, /* Different tick. */
        0, MIDI_META, MIDI_META_PORT, 1, 1,
        0, MIDI_PROGRAM, 4, /* Different port, same tick. */
        0, MIDI_PROGRAM, 4,
        0, MIDI_NOTE_ON, 64, 100,
        96, MIDI_NOTE_OFF, 64, 0,
        0, MIDI_META, MIDI_META_END, 0,
    };
    const uint8_t second_track[] = {
        0, MIDI_PROGRAM, 4, /* Different track. */
        0, MIDI_PROGRAM, 4,
        0, MIDI_META, MIDI_META_END, 0,
    };
    /* clang-format on */
    DmBytes midi = one_track(first_track, sizeof(first_track));
    DmBytes second = one_track(second_track, sizeof(second_track));
    midi.data[9] = 1;
    midi.data[11] = 2;
    CHECK(dm_bytes(&midi, second.data + 14, second.size - 14));

    DmProject p;
    dm_project_init(&p);
    char error[256];
    CHECK(dm_import(&p, midi.data, midi.size, -1, 0, error, sizeof(error)));
    CHECK(p.port == DM_PORT_COMBINED);

    const struct {
        double beat;
        int track, port, status, program;
    } expected[] = {
        {0, 0, 0, MIDI_PROGRAM, 4}, {0, 0, 0, MIDI_PROGRAM | 1, 4}, {0, 0, 0, MIDI_PROGRAM, 4},
        {0, 0, 0, MIDI_PROGRAM, 5}, {0, 0, 0, MIDI_PROGRAM, 4},     {0, 0, 0, MIDI_PROGRAM, 4},
        {0, 1, 0, MIDI_PROGRAM, 4}, {1, 0, 0, MIDI_PROGRAM, 4},     {1, 0, 1, MIDI_PROGRAM, 4},
    };

    size_t seen = 0;

    for (size_t i = 0; i < p.event_count; ++i) {
        const DmEvent *e = &p.events[i];

        if ((e->message.data[0] & MIDI_STATUS_MASK) != MIDI_PROGRAM) {
            continue;
        }

        CHECK(seen < sizeof(expected) / sizeof(*expected));
        CHECK(e->beat == expected[seen].beat && e->track == expected[seen].track &&
              e->port == expected[seen].port && e->message.data[0] == expected[seen].status &&
              e->message.data[1] == expected[seen].program);
        ++seen;
    }

    CHECK(seen == sizeof(expected) / sizeof(*expected));
    CHECK(p.original.size == midi.size && !memcmp(p.original.data, midi.data, midi.size));
    dm_project_free(&p);
    dm_bytes_free(&second);
    dm_bytes_free(&midi);

    /* An older editor snapshot can still contain both records. Normalize on MIDI import. */
    const uint8_t patch[] = {MIDI_PROGRAM, 4};
    DmBytes program = {(uint8_t *)patch, sizeof(patch), sizeof(patch)}, exported = {0};
    DmProject restored;
    dm_project_init(&p);
    dm_project_init(&restored);
    CHECK(dm_event_add(&p, 0, 0, 0, &program));
    CHECK(dm_event_add(&p, 0, 0, 0, &program));
    CHECK(dm_event_add(&p, 0, 1, 0, &program));
    CHECK(dm_midi_export(&p, &exported, false, DM_METADATA_KEEP_ALL, error, sizeof(error)));
    CHECK(dm_import(&restored, exported.data, exported.size, -1, 0, error, sizeof(error)));
    CHECK(restored.event_count == 2 && restored.events[0].track == 0 &&
          restored.events[1].track == 1);
    dm_bytes_free(&exported);
    dm_project_free(&restored);
    dm_project_free(&p);
}

static void check_patch_order(const DmProject *p, int opening_program) {
    const int expected[][3] = {
        {MIDI_SYSEX, 5, 0x7e},
        {MIDI_PROGRAM, opening_program, 0},
        {MIDI_NOTE_ON, 60, 100},
        {MIDI_PROGRAM, 40, 0},
        {MIDI_SYSEX, 10, 0x41},
        {MIDI_CONTROL, MIDI_CC_BANK, 1},
        {MIDI_CONTROL, MIDI_CC_BANK_LSB, 2},
        {MIDI_PROGRAM, 40, 0},
        {MIDI_CONTROL, MIDI_CC_BANK, 2},
        {MIDI_PROGRAM, 40, 0},
        {MIDI_NOTE_ON, 62, 100},
    };
    DmSchedule schedule = {0};
    CHECK(dm_schedule(p, &schedule));
    size_t seen = 0;

    for (size_t i = 0; i < schedule.count; ++i) {
        const DmScheduled *e = &schedule.items[i];
        bool bank =
            e->status == MIDI_CONTROL && (e->a == MIDI_CC_BANK || e->a == MIDI_CC_BANK_LSB);

        if (e->seconds > 0 &&
            (bank || e->status == MIDI_SYSEX || e->status == MIDI_PROGRAM ||
             (e->status == MIDI_NOTE_ON && e->b))) {
            CHECK(seen < sizeof(expected) / sizeof(*expected));
            CHECK(e->status == expected[seen][0] && e->a == expected[seen][1] &&
                  e->b == expected[seen][2]);
            ++seen;
        }
    }

    CHECK(seen == sizeof(expected) / sizeof(*expected));
    dm_schedule_free(&schedule);
}

static void patch_order(void) {
    /* clang-format off */
    const uint8_t track[] = {
        12, MIDI_SYSEX, 5, 0x7e, 0x7f, 0x09, 0x01, MIDI_SYSEX_END,
        12, MIDI_PROGRAM, 40,
        12, MIDI_NOTE_ON, 60, 100,
        12, MIDI_NOTE_OFF, 60, 0,
        0, MIDI_PROGRAM, 40,
        0, MIDI_SYSEX, 10, 0x41, 0x10, 0x42, 0x12, 0x40, 0, 0x7f, 0, 0x41, MIDI_SYSEX_END,
        0, MIDI_CONTROL, MIDI_CC_BANK, 1,
        0, MIDI_CONTROL, MIDI_CC_BANK_LSB, 2,
        0, MIDI_PROGRAM, 40,
        0, MIDI_CONTROL, MIDI_CC_BANK, 2,
        0, MIDI_PROGRAM, 40,
        0, MIDI_NOTE_ON, 62, 100,
        24, MIDI_NOTE_OFF, 62, 0,
        0x84, 0, MIDI_META, MIDI_META_END, 0,
    };
    /* clang-format on */
    DmBytes file = one_track(track, sizeof(track)), exported = {0};
    DmProject p, imported;
    dm_project_init(&p);
    dm_project_init(&imported);
    char error[256] = {0};
    CHECK(dm_import(&p, file.data, file.size, 0, 0, error, sizeof(error)));
    CHECK(p.clip_count == 1 && p.clips[0].length > 0);
    check_patch_order(&p, 40);
    CHECK(dm_set_initial_program(&p, 0, 41, true));
    check_patch_order(&p, 41);

    CHECK(dm_midi(&p, &exported, error, sizeof(error)));
    CHECK(dm_import(&imported, exported.data, exported.size, 0, 0, error, sizeof(error)));
    check_patch_order(&imported, 41);
    dm_project_free(&imported);
    dm_project_free(&p);
    dm_bytes_free(&exported);
    dm_bytes_free(&file);
}

static size_t count_bytes(const DmBytes *b, const unsigned char *pattern, size_t size) {
    size_t found = 0;

    for (size_t i = 0; i + size <= b->size; ++i) {
        found += !memcmp(b->data + i, pattern, size);
    }

    return found;
}

/* Percussion parts other than channel 10: a GS rhythm part (channel 11), XG and GM2 drum banks
   before the first note (channels 12 and 13), and a drum bank after the first note, which does
   not count (channel 14). Saved files announce every percussion part with a GS message. */
static void percussion_parts(void) {
    const unsigned char track[] = {
        0,    0xF0, 10,  0x41, 0x10, 0x42, 0x12, 0x40, 0x1A, 0x15, 0x02, 0x0F, 0xF7, /* Ch 11. */
        0,    0xBB, 0,   127,                                                        /* Ch 12 XG. */
        0,    0xBC, 0,   120, /* Ch 13 GM2. */
        0,    0x99, 36,  100,  0,    0x9A, 38,   100,  0,    0x9B, 42,   100,  0,
        0x9C, 46,   100, 0,    0x9D, 60,   100,  1,    0xBD, 0,    127, /* Ch 14: too late. */
        95,   0x89, 36,  0,    0,    0x8A, 38,   0,    0,    0x8B, 42,   0,    0,
        0x8C, 46,   0,   0,    0x8D, 60,   0,    0,    0xFF, 0x2F, 0};
    DmBytes file = one_track(track, sizeof(track)), saved = {0};
    DmProject p, again;
    dm_project_init(&p);
    dm_project_init(&again);
    char error[256] = {0};
    CHECK(dm_import(&p, file.data, file.size, 0, 0, error, sizeof(error)));

    for (int c = 0; c < 16; ++c) {
        CHECK(p.channels[c].drums == (c >= 9 && c <= 12));
        CHECK(dm_channel_drums(&p, c) == p.channels[c].drums);
    }

    /* The source's own GS message is kept once; channels 12 and 13 gain one each. */
    CHECK(dm_midi(&p, &saved, error, sizeof(error)));
    const unsigned char part11[] = {0x40, 0x1A, 0x15, 0x02}, part12[] = {0x40, 0x1B, 0x15, 0x01},
                        part13[] = {0x40, 0x1C, 0x15, 0x01}, part14[] = {0x40, 0x1D, 0x15};
    CHECK(count_bytes(&saved, part11, 4) == 1 && count_bytes(&saved, part12, 4) == 1 &&
          count_bytes(&saved, part13, 4) == 1 && count_bytes(&saved, part14, 3) == 0);
    CHECK(dm_import(&again, saved.data, saved.size, 0, 0, error, sizeof(error)));

    for (int c = 0; c < 16; ++c) {
        CHECK(again.channels[c].drums == p.channels[c].drums);
    }

    /* A melodic channel 10 is announced too. */
    p.channels[MIDI_DRUM_CHANNEL].drums = false;
    dm_bytes_free(&saved);
    CHECK(dm_midi(&p, &saved, error, sizeof(error)));
    const unsigned char melodic10[] = {0xF0, 10,   0x41, 0x10, 0x42, 0x12,
                                       0x40, 0x10, 0x15, 0x00, 0x1B, 0xF7};
    CHECK(count_bytes(&saved, melodic10, sizeof(melodic10)) == 1);

    /* A GS reset afterwards returns to the General MIDI layout. */
    const unsigned char reset[] = {0,    0xF0, 10,   0x41, 0x10, 0x42, 0x12, 0x40, 0x1A, 0x15,
                                   0x02, 0x0F, 0xF7, 0,    0xF0, 10,   0x41, 0x10, 0x42, 0x12,
                                   0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7, 0,    0x9A, 38,   100,
                                   96,   0x8A, 38,   0,    0,    0xFF, 0x2F, 0};
    DmBytes later = one_track(reset, sizeof(reset));
    CHECK(dm_import(&again, later.data, later.size, 0, 0, error, sizeof(error)));
    CHECK(!again.channels[10].drums && again.channels[MIDI_DRUM_CHANNEL].drums);
    dm_bytes_free(&file);
    dm_bytes_free(&later);
    dm_bytes_free(&saved);
    dm_project_free(&p);
    dm_project_free(&again);
}

static void opening_tempo(void) {
    const uint8_t track[] = {
        12,
        MIDI_META,
        MIDI_META_TEMPO,
        3,
        0x09,
        0x27,
        0xc0, /* 100 BPM before the first note. */
        12,
        MIDI_NOTE_ON,
        60,
        100,
        96,
        MIDI_NOTE_OFF,
        60,
        0,
        0,
        MIDI_META,
        MIDI_META_TEMPO,
        3,
        0x0f,
        0x42,
        0x40, /* 60 BPM later in the song. */
        0,
        MIDI_META,
        MIDI_META_END,
        0,
    };
    DmBytes file = one_track(track, sizeof(track));
    DmProject project;
    dm_project_init(&project);
    char error[256];
    CHECK(dm_import(&project, file.data, file.size, 0, 0, error, sizeof(error)));
    CHECK(project.bpm == 100 && dm_playback_bpm(&project, 0) == 100);
    CHECK(dm_playback_bpm(&project, dm_beat_seconds(&project, 2)) == 60);
    dm_project_free(&project);
    dm_bytes_free(&file);
}

/* Route 0 counts as used only when something on the channel uses it, like any other route. */
static void channel_ports(void) {
    DmProject p;
    dm_project_init(&p);
    bool used[DM_NOTE_PORTS];
    dm_channel_ports(&p, 0, used);
    CHECK(!used[0]);
    CHECK(dm_pattern_add(&p, "Route 2") == 0);
    CHECK(dm_note_add(&p.patterns[0], (DmNote){0, 1, 60, 100, 0, 2}));
    CHECK(dm_clip_add(&p, (DmClip){.pattern = 0, .channel = 0}));
    dm_channel_ports(&p, 0, used);
    CHECK(!used[0] && used[2]);
    CHECK(dm_note_add(&p.patterns[0], (DmNote){1, 1, 62, 100, 0, 0}));
    dm_channel_ports(&p, 0, used);
    CHECK(used[0] && used[2]);
    dm_project_free(&p);
}

int main(void) {
    channel_ports();
    midi_metadata();
    midi_metadata_views();
    bounded_curves();
    mus_input();
    pattern_meter();
    roundtrip();
    model();
    multiport();
    opening_controls();
    pan_provenance();
    program_repeats();
    patch_order();
    opening_tempo();
    malformed();
    advanced();
    independent_sequences();
    mutated_files();
    percussion_parts();
    printf("PASS: %d checks (MIDI, editor metadata, tempo, projects, corruption, ports, malformed "
           "data)\n",
           checks);
    return 0;
}
