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

static void project_validation(void) {
    DmProject p, copy;
    dm_project_init(&p);
    dm_project_init(&copy);
    int notes = dm_pattern_add(&p, "Validation notes");
    int controls = dm_pattern_add(&p, "Validation controls");
    CHECK(notes == 0 && controls == 1);
    CHECK(dm_note_add(&p.patterns[notes], (DmNote){0, 1, 60, 100, 0}));
    p.patterns[controls].kind = 1;
    p.patterns[controls].length = 4;
    p.lane_count[0] = 1;
    CHECK(dm_clip_add(&p, (DmClip){.pattern = notes}));
    CHECK(dm_clip_add(&p, (DmClip){.pattern = controls, .lane = 1}));
    CHECK(dm_automation_add(&p, (DmAutomation){.type = MIDI_CONTROL,
                                               .a = MIDI_CC_EXPRESSION,
                                               .b = 80,
                                               .pattern = controls,
                                               .curve = 1}));
    CHECK(dm_automation_add(&p, (DmAutomation){.beat = 1,
                                               .type = MIDI_CONTROL,
                                               .a = MIDI_CC_EXPRESSION,
                                               .b = 100,
                                               .pattern = controls,
                                               .curve = 1}));
    CHECK(dm_automation_add(&p, (DmAutomation){.beat = 2,
                                               .type = MIDI_CONTROL,
                                               .a = MIDI_CC_EXPRESSION,
                                               .b = 127,
                                               .pattern = controls,
                                               .curve = -1}));
    uint8_t bytes[] = {MIDI_CONTROL, MIDI_CC_VOLUME, 100};
    DmBytes message = {bytes, sizeof(bytes), 0};
    CHECK(dm_event_add(&p, 0, 0, 0, &message));
    CHECK(dm_event_add(&p, 1, 0, 0, &message));
    char error[256] = {0};
    CHECK(dm_project_validate(&p, error, sizeof(error)));
    CHECK(dm_project_validate(&p, NULL, 0));
    DmBytes packed = {0};
    CHECK(dm_pack(&p, &packed));
    CHECK(dm_unpack(&copy, packed.data, packed.size, error, sizeof(error)));
    CHECK(dm_project_validate(&copy, error, sizeof(error)));
    dm_bytes_free(&packed);

    for (int broken = 0; broken < 9; ++broken) {
        CHECK(dm_project_copy(&copy, &p));

        switch (broken) {
        case 0:
            copy.clips[0].pattern = (int)copy.pattern_count;
            break;
        case 1:
            copy.clips[1].lane = 0;
            CHECK(dm_pack(&copy, &packed));
            CHECK(!dm_unpack(&copy, packed.data, packed.size, error, sizeof(error)));
            dm_bytes_free(&packed);
            break;
        case 2:
            copy.clips[1].lane = 2;
            break;
        case 3:
            copy.automation[0].pattern = (int)copy.pattern_count;
            break;
        case 4:
            copy.patterns[1].uid = copy.patterns[0].uid;
            break;
        case 5:
            copy.automation[0].beat = 1.5;
            break;
        case 6:
            copy.automation[0].curve = copy.automation[1].curve = 0;
            break;
        case 7:
            copy.events[0].beat = 2;
            break;
        case 8:
            copy.automation[0].pattern = -1;
            break;
        }

        error[0] = 0;

        /* An unpaired restoration breaks only an editing rule: older files may hold one, and
           they keep loading (playback ignores it). */
        if (broken == 6) {
            DmProject loaded;
            dm_project_init(&loaded);
            CHECK(dm_project_validate(&copy, NULL, 0));
            CHECK(!dm_project_check_edit(&copy, error, sizeof(error)) && error[0]);
            CHECK(dm_pack(&copy, &packed));
            CHECK(dm_unpack(&loaded, packed.data, packed.size, error, sizeof(error)));
            dm_bytes_free(&packed);
            dm_project_free(&loaded);
            continue;
        }

        CHECK(!dm_project_validate(&copy, error, sizeof(error)));
        CHECK(error[0]);
        CHECK(!dm_project_validate(&copy, NULL, 0));
        CHECK(!dm_project_check_edit(&copy, NULL, 0));
    }

    dm_project_free(&copy);
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
    CHECK(!dm_automation_add(
        &p, (DmAutomation){0, MIDI_CONTROL, 0, MIDI_CC_VOLUME, 100, -1, 0, 0, 0, false}));
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
    CHECK(dm_automation_add(&p,
                            (DmAutomation){1.5, 0xb0, 0, MIDI_CC_RPN_MSB, 0, id, 8, 0, 0, false}));
    CHECK(dm_automation_add(&p,
                            (DmAutomation){1.5, 0xb0, 0, MIDI_CC_RPN_LSB, 0, id, 8, 0, 0, false}));
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

/* The GM System On before the first note goes out at beat 0 ahead of the channel setup (it would
   otherwise reset the opening program); after it, patch messages keep their order. */
static void check_patch_order(const DmProject *p, int opening_program) {
    const int expected[][3] = {
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
    int reset = -1, setup = -1;

    for (size_t i = 0; i < schedule.count && schedule.items[i].seconds == 0; ++i) {
        if (schedule.items[i].status == MIDI_SYSEX && reset < 0) {
            reset = (int)i;
        } else if (schedule.items[i].status == MIDI_PROGRAM && setup < 0) {
            setup = (int)i;
        }
    }

    CHECK(reset >= 0 && setup > reset);

    for (size_t i = 0; i < schedule.count; ++i) {
        const DmScheduled *e = &schedule.items[i];
        bool bank = e->status == MIDI_CONTROL && (e->a == MIDI_CC_BANK || e->a == MIDI_CC_BANK_LSB);

        if (e->seconds > 0 && (bank || e->status == MIDI_SYSEX || e->status == MIDI_PROGRAM ||
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

static int cc_at(const DmExportEvent *events, size_t count, int channel, int controller,
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

static int export_cc_at(const DmProject *p, int channel, int controller, double beat) {
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration = 0;
    char error[256] = {0};
    CHECK(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)));
    int value = cc_at(events, count, channel, controller, (uint64_t)llround(beat * DM_PPQ) + 1);
    free(events);
    return value;
}

static int add_auto_pattern(DmProject *p, const char *name, double length) {
    int id = dm_pattern_add(p, name);
    CHECK(id >= 0);
    p->patterns[id].kind = 1;
    p->patterns[id].event = MIDI_CC_EXPRESSION;
    p->patterns[id].length = length;
    return id;
}

/* F3: a section returns to the pattern's value before its first point, in the order the pattern's
   points play. CC11 is 40 from beat 0; section 1 runs over beats 6-10 and section 2 starts on beat
   10, where section 1 ends. Whole (offset < 0) or as a slice from offset, after a note covering
   the song so no release is dropped. */
static void section_project(DmProject *p, double offset) {
    dm_project_init(p);
    p->channels[0].enabled = true;
    p->lane_count[0] = 1;
    int a = add_auto_pattern(p, "Sections", 16);
    int n = dm_pattern_add(p, "Note");
    CHECK(n >= 0);
    p->patterns[n].length = 20;
    CHECK(dm_note_add(&p->patterns[n], (DmNote){0, 20, 60, 100, 0}));
    const DmAutomation points[] = {
        {.beat = 0, .b = 40},
        {.beat = 6, .b = 100, .curve = 1},
        {.beat = 10, .b = 20, .curve = 1},
        {.beat = 10, .b = 0, .curve = -1},
        {.beat = 10, .b = 60, .curve = 2},
        {.beat = 13, .b = 110, .curve = 2},
        {.beat = 13, .b = 0, .curve = -2},
    };

    for (size_t i = 0; i < sizeof(points) / sizeof(*points); ++i) {
        DmAutomation point = points[i];
        point.type = MIDI_CONTROL;
        point.a = MIDI_CC_EXPRESSION;
        point.pattern = a;
        CHECK(dm_automation_add(p, point));
    }

    CHECK(dm_clip_add(p, (DmClip){.beat = 0, .pattern = n, .channel = 0, .lane = 0}));

    if (offset < 0) {
        CHECK(dm_clip_add(p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
    } else {
        CHECK(dm_clip_add(
            p,
            (DmClip){
                .beat = 0, .pattern = a, .channel = 0, .lane = 1, .offset = 0, .length = offset}));
        CHECK(dm_clip_add(p, (DmClip){.beat = offset,
                                      .pattern = a,
                                      .channel = 0,
                                      .lane = 1,
                                      .offset = offset,
                                      .length = 16 - offset}));
    }
}

static void section_returns(void) {
    DmProject whole;
    section_project(&whole, -1);
    /* Section 2 returns to what section 1 returned to, not to section 1's last step. */
    CHECK(export_cc_at(&whole, 0, MIDI_CC_EXPRESSION, 11) > 60);
    CHECK(export_cc_at(&whole, 0, MIDI_CC_EXPRESSION, 13) == 40);
    CHECK(export_cc_at(&whole, 0, MIDI_CC_EXPRESSION, 15) == 40);
    /* Slices inside section 1, inside section 2 and after both sound like the whole pattern. */
    const double offsets[] = {8, 12, 14};

    for (size_t o = 0; o < sizeof(offsets) / sizeof(*offsets); ++o) {
        DmProject sliced;
        section_project(&sliced, offsets[o]);

        for (double beat = offsets[o]; beat < 16; beat += .25) {
            CHECK(export_cc_at(&sliced, 0, MIDI_CC_EXPRESSION, beat) ==
                  export_cc_at(&whole, 0, MIDI_CC_EXPRESSION, beat));
        }

        dm_project_free(&sliced);
    }

    dm_project_free(&whole);

    /* A Graph lane's release before a slice starts leaves the slice released. */
    DmProject graph;
    dm_project_init(&graph);
    graph.channels[0].enabled = true;
    graph.lane_count[0] = 1;
    int g = add_auto_pattern(&graph, "Graph", 4);
    graph.patterns[g].graph_lanes[MIDI_CC_EXPRESSION] = DM_GRAPH_DEFAULT;
    int n = dm_pattern_add(&graph, "Note");
    CHECK(n >= 0);
    graph.patterns[n].length = 4;
    CHECK(dm_note_add(&graph.patterns[n], (DmNote){0, 4, 60, 100, 0}));
    CHECK(dm_automation_add(
        &graph,
        (DmAutomation){
            .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 50, .pattern = g}));
    CHECK(dm_clip_add(&graph, (DmClip){.beat = 0, .pattern = n, .channel = 0, .lane = 0}));
    CHECK(dm_clip_add(
        &graph,
        (DmClip){.beat = 2, .pattern = g, .channel = 0, .lane = 1, .offset = 2, .length = 2}));
    CHECK(export_cc_at(&graph, 0, MIDI_CC_EXPRESSION, 2.5) == 127);
    dm_project_free(&graph);
}

/* F10: the bend range goes with whoever plays the bend, and the bank with whoever plays the
   program; a pattern without a range (or bank) of its own uses the usual winner's. */
static void pair_project(DmProject *p) {
    dm_project_init(p);
    p->channels[0].enabled = true;
    p->lane_count[0] = 2;
    int n = dm_pattern_add(p, "Note");
    CHECK(n >= 0);
    p->patterns[n].length = 8;
    CHECK(dm_note_add(&p->patterns[n], (DmNote){0, 8, 60, 100, 0}));
    CHECK(dm_clip_add(p, (DmClip){.pattern = n, .channel = 0}));
}

static void pair_range(DmProject *p, int pattern, double beat, int semitones) {
    const int cc[] = {MIDI_CC_RPN_MSB, MIDI_CC_RPN_LSB, MIDI_CC_DATA};
    const int value[] = {0, 0, semitones};

    for (int i = 0; i < 3; ++i) {
        CHECK(dm_automation_add(p, (DmAutomation){.beat = beat,
                                                  .type = MIDI_CONTROL,
                                                  .a = cc[i],
                                                  .b = value[i],
                                                  .pattern = pattern}));
    }
}

static void pair_bend(DmProject *p, int pattern, double beat, int value, int curve) {
    CHECK(dm_automation_add(p, (DmAutomation){.beat = beat,
                                              .type = MIDI_PITCH_BEND,
                                              .a = value & MIDI_DATA_MASK,
                                              .b = value >> 7,
                                              .pattern = pattern,
                                              .curve = curve}));
}

static int program_at(const DmProject *p, double beat) {
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration = 0;
    char error[256] = {0};
    CHECK(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)));
    int value = -1;

    for (size_t i = 0; i < count; ++i) {
        if (events[i].tick <= (uint64_t)llround(beat * DM_PPQ) + 1 &&
            events[i].status == MIDI_PROGRAM) {
            value = (int)events[i].a;
        }
    }

    free(events);
    return value;
}

static void bend_range_pairs(void) {
    /* A bends 0-8 at range 12; B, from beat 1, sets range 2 and bends once at song beat 3. */
    DmProject p;
    pair_project(&p);
    int a = add_auto_pattern(&p, "A", 8), b = add_auto_pattern(&p, "B", 8);
    p.patterns[a].event = p.patterns[b].event = DM_EVENT_BEND;
    pair_range(&p, a, 0, 12);
    pair_bend(&p, a, 0, MIDI_BEND_CENTER, 1);
    pair_bend(&p, a, 4, 12288, 1);
    pair_bend(&p, a, 8, MIDI_BEND_CENTER, 1);
    pair_range(&p, b, 1, 2);
    pair_bend(&p, b, 2, 4096, 0);
    CHECK(dm_clip_add(&p, (DmClip){.pattern = a, .channel = 0, .lane = 1}));
    CHECK(dm_clip_add(&p, (DmClip){.beat = 1, .pattern = b, .channel = 0, .lane = 2, .length = 7}));
    CHECK(export_cc_at(&p, 0, MIDI_CC_DATA, 2.5) == 12);
    CHECK(export_cc_at(&p, 0, MIDI_CC_DATA, 3.5) == 2);
    CHECK(export_cc_at(&p, 0, MIDI_CC_DATA, 5) == 12);
    dm_project_free(&p);

    /* A range-only pattern still sets the range for a bend pattern without a range of its own. */
    pair_project(&p);
    int r = add_auto_pattern(&p, "Range", 8), n = add_auto_pattern(&p, "Bends", 8);
    p.patterns[r].event = p.patterns[n].event = DM_EVENT_BEND;
    pair_range(&p, r, 0, 12);
    pair_bend(&p, n, 2, 12288, 0);
    CHECK(dm_clip_add(&p, (DmClip){.pattern = r, .channel = 0, .lane = 1}));
    CHECK(dm_clip_add(&p, (DmClip){.pattern = n, .channel = 0, .lane = 2}));
    CHECK(export_cc_at(&p, 0, MIDI_CC_DATA, 3) == 12);
    dm_project_free(&p);

    /* A selects bank 1 and programs 5 and 6; B, from beat 1, bank 2 at song beat 2 and program 9
       at song beat 4. A's program 6 plays in A's bank. */
    pair_project(&p);
    a = add_auto_pattern(&p, "A", 8);
    b = add_auto_pattern(&p, "B", 8);
    p.patterns[a].event = p.patterns[b].event = DM_EVENT_PROGRAM;
    CHECK(dm_automation_add(
        &p,
        (DmAutomation){.beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_BANK, .b = 1, .pattern = a}));
    CHECK(dm_automation_add(&p,
                            (DmAutomation){.beat = 0, .type = MIDI_PROGRAM, .a = 5, .pattern = a}));
    CHECK(dm_automation_add(&p,
                            (DmAutomation){.beat = 2, .type = MIDI_PROGRAM, .a = 6, .pattern = a}));
    CHECK(dm_automation_add(
        &p,
        (DmAutomation){.beat = 1, .type = MIDI_CONTROL, .a = MIDI_CC_BANK, .b = 2, .pattern = b}));
    CHECK(dm_automation_add(&p,
                            (DmAutomation){.beat = 3, .type = MIDI_PROGRAM, .a = 9, .pattern = b}));
    CHECK(dm_clip_add(&p, (DmClip){.pattern = a, .channel = 0, .lane = 1}));
    CHECK(dm_clip_add(&p, (DmClip){.beat = 1, .pattern = b, .channel = 0, .lane = 2, .length = 7}));
    CHECK(program_at(&p, 3) == 6 && export_cc_at(&p, 0, MIDI_CC_BANK, 3) == 1);
    CHECK(program_at(&p, 5) == 9 && export_cc_at(&p, 0, MIDI_CC_BANK, 5) == 2);
    dm_project_free(&p);

    /* A bank-only pattern still sets the bank for a program pattern without a bank of its own. */
    pair_project(&p);
    int k = add_auto_pattern(&p, "Bank", 8), g = add_auto_pattern(&p, "Programs", 8);
    p.patterns[k].event = p.patterns[g].event = DM_EVENT_PROGRAM;
    CHECK(dm_automation_add(
        &p,
        (DmAutomation){.beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_BANK, .b = 3, .pattern = k}));
    CHECK(dm_automation_add(&p,
                            (DmAutomation){.beat = 1, .type = MIDI_PROGRAM, .a = 7, .pattern = g}));
    CHECK(dm_clip_add(&p, (DmClip){.pattern = k, .channel = 0, .lane = 1}));
    CHECK(dm_clip_add(&p, (DmClip){.pattern = g, .channel = 0, .lane = 2}));
    CHECK(program_at(&p, 2) == 7 && export_cc_at(&p, 0, MIDI_CC_BANK, 2) == 3);
    dm_project_free(&p);
}

/* Three source ports, each on its own channel: port 24 on channel 1, 27 on channel 10, 30 on
   channel 3. */
static const uint8_t three_ports[] = {
    0x4d, 0x54, 0x68, 0x64, 0,    0,    0,    6,    0,    1,    0,    4,    1,    0xe0, 0x4d, 0x54,
    0x72, 0x6b, 0,    0,    0,    0xb,  0,    0xff, 0x51, 3,    7,    0xa1, 0x20, 0,    0xff, 0x2f,
    0,    0x4d, 0x54, 0x72, 0x6b, 0,    0,    0,    0x12, 0,    0xff, 0x21, 1,    0x18, 0,    0x90,
    0x3c, 0x64, 0x83, 0x60, 0x80, 0x3c, 0,    0,    0xff, 0x2f, 0,    0x4d, 0x54, 0x72, 0x6b, 0,
    0,    0,    0x12, 0,    0xff, 0x21, 1,    0x1b, 0,    0x99, 0x24, 0x64, 0x83, 0x60, 0x89, 0x24,
    0,    0,    0xff, 0x2f, 0,    0x4d, 0x54, 0x72, 0x6b, 0,    0,    0,    0x12, 0,    0xff, 0x21,
    1,    0x1e, 0,    0x92, 0x40, 0x64, 0x83, 0x60, 0x82, 0x40, 0,    0,    0xff, 0x2f, 0};

/* The source port a channel's notes came from, or -1. */
static int channel_source_port(const DmProject *p, int channel) {
    for (size_t i = 0; i < p->clip_count; ++i) {
        const DmPattern *pat = &p->patterns[p->clips[i].pattern];

        if (p->clips[i].channel == channel && !pat->kind && pat->count) {
            int k = pat->notes[0].port;
            return p->port == DM_PORT_COMBINED ? (k < p->port_count ? p->ports[k] : -1) : p->port;
        }
    }

    return -1;
}

static void reimport(DmProject *p) {
    DmBytes out = {0};
    char error[256] = {0};
    CHECK(dm_midi(p, &out, error, sizeof(error)));
    dm_project_free(p);
    dm_project_init(p);
    CHECK(dm_import(p, out.data, out.size, -1, 0, error, sizeof(error)));
    dm_bytes_free(&out);
}

/* F11: an import exports each channel on its own source port; a note port the user added takes a
   number no source port uses; exporting every view no longer refuses a colored note port whose
   number a source port uses. */
static void export_ports_keep_numbers(void) {
    DmProject p;
    dm_project_init(&p);
    char error[256] = {0};
    CHECK(dm_import(&p, three_ports, sizeof(three_ports), -1, 0, error, sizeof(error)));
    CHECK(channel_source_port(&p, 0) == 24 && channel_source_port(&p, 9) == 27 &&
          channel_source_port(&p, 2) == 30);
    reimport(&p);
    CHECK(channel_source_port(&p, 0) == 24 && channel_source_port(&p, 9) == 27 &&
          channel_source_port(&p, 2) == 30);
    int added = dm_pattern_add(&p, "Added port");
    CHECK(added >= 0);
    CHECK(dm_note_add(&p.patterns[added], (DmNote){0, 1, 67, 100, 0, (uint8_t)p.port_count}));
    p.channels[4].enabled = true;
    CHECK(dm_clip_add(&p, (DmClip){.pattern = added, .channel = 4}));
    reimport(&p);
    int fresh = channel_source_port(&p, 4);
    CHECK(fresh >= 0 && fresh != 24 && fresh != 27 && fresh != 30);
    CHECK(channel_source_port(&p, 0) == 24 && channel_source_port(&p, 9) == 27 &&
          channel_source_port(&p, 2) == 30);
    dm_project_free(&p);

    dm_project_init(&p);
    CHECK(dm_import(&p, three_ports, sizeof(three_ports), 24, 0, error, sizeof(error)));
    int colored = dm_pattern_add(&p, "Colored");
    CHECK(colored >= 0);
    CHECK(dm_note_add(&p.patterns[colored], (DmNote){0, 1, 67, 100, 0, 27}));
    p.channels[5].enabled = true;
    CHECK(dm_clip_add(&p, (DmClip){.pattern = colored, .channel = 5}));
    DmBytes all = {0};
    CHECK(dm_midi_all(&p, &all, error, sizeof(error)));
    dm_bytes_free(&all);
    dm_project_free(&p);
}

/* CC11 90 and a note from beat 0; a GS reset at beat 1 with CC11 90 sent again; CC11 90 again at
   beat 2 (a true repeat); a loopStart marker at beat 3 with CC7 100 sent again, CC7 60 at 3.5;
   Reset All Controllers at 3.75 and at 4. */
static const uint8_t command_file[] = {
    0x4d, 0x54, 0x68, 0x64, 0,    0,    0,    6,    0,    0,    0,    1,    1,    0xe0, 0x4d, 0x54,
    0x72, 0x6b, 0,    0,    0,    0x4a, 0,    0xb0, 0xb,  0x5a, 0,    0xb0, 7,    0x64, 0,    0x90,
    0x3c, 0x64, 0x83, 0x60, 0xf0, 0xa,  0x41, 0x10, 0x42, 0x12, 0x40, 0,    0x7f, 0,    0x41, 0xf7,
    0,    0xb0, 0xb,  0x5a, 0x83, 0x60, 0xb0, 0xb,  0x5a, 0x83, 0x60, 0xff, 6,    9,    0x6c, 0x6f,
    0x6f, 0x70, 0x53, 0x74, 0x61, 0x72, 0x74, 0,    0xb0, 7,    0x64, 0x81, 0x70, 0xb0, 7,    0x3c,
    0x78, 0xb0, 0x79, 0,    0x78, 0x80, 0x3c, 0,    0,    0xb0, 0x79, 0,    0,    0xff, 0x2f, 0};

/* GM System On at beat 0.1, program 5 and CC7 127 at beat 0.5, the first note at beat 4. */
static const uint8_t opening_file[] = {
    0x4d, 0x54, 0x68, 0x64, 0,    0,    0,    6,    0,    0,    0,    1,    1,
    0xe0, 0x4d, 0x54, 0x72, 0x6b, 0,    0,    0,    0x1e, 0x30, 0xf0, 5,    0x7e,
    0x7f, 9,    1,    0xf7, 0x81, 0x40, 0xc0, 5,    0,    0xb0, 7,    0x7f, 0x8d,
    0x10, 0x90, 0x3c, 0x64, 0x83, 0x60, 0x80, 0x3c, 0,    0,    0xff, 0x2f, 0};

/* Index of the first exported event matching status, a and b (b < 0: any) at tick, or -1. */
static int event_index(const DmExportEvent *events, size_t count, uint64_t tick, uint32_t status,
                       int a, int b) {
    for (size_t i = 0; i < count; ++i) {
        if (events[i].tick == tick && events[i].status == status &&
            (a < 0 || events[i].a == (uint32_t)a) && (b < 0 || events[i].b == (uint32_t)b)) {
            return (int)i;
        }
    }

    return -1;
}

/* F11: channel mode messages are commands, always sent; after a SysEx reset, Reset All
   Controllers or the loop start, a value the song sends again is sent again; a SysEx reset before
   the first note goes out at beat 0 before the channel setup; export and re-import keep one
   program change at beat 0. */
static void commands_and_resets(void) {
    DmProject p;
    dm_project_init(&p);
    char error[256] = {0};
    CHECK(dm_import(&p, command_file, sizeof(command_file), -1, 0, error, sizeof(error)));
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration = 0;
    CHECK(dm_export_timeline(&p, &events, &count, &duration, error, sizeof(error)));
    CHECK(event_index(events, count, DM_PPQ, MIDI_CONTROL, MIDI_CC_EXPRESSION, 90) >= 0);
    CHECK(event_index(events, count, 2 * DM_PPQ, MIDI_CONTROL, MIDI_CC_EXPRESSION, 90) < 0);
    CHECK(event_index(events, count, 3 * DM_PPQ, MIDI_CONTROL, MIDI_CC_VOLUME, 100) >= 0);
    CHECK(event_index(events, count, DM_PPQ * 15 / 4, MIDI_CONTROL, MIDI_CC_RESET, -1) >= 0);
    CHECK(event_index(events, count, 4 * DM_PPQ, MIDI_CONTROL, MIDI_CC_RESET, -1) >= 0);
    free(events);
    dm_project_free(&p);

    dm_project_init(&p);
    CHECK(dm_import(&p, opening_file, sizeof(opening_file), -1, 0, error, sizeof(error)));
    CHECK(dm_export_timeline(&p, &events, &count, &duration, error, sizeof(error)));
    int reset = event_index(events, count, 0, MIDI_SYSEX, -1, -1);
    int volume = event_index(events, count, 0, MIDI_CONTROL, MIDI_CC_VOLUME, 127);
    CHECK(reset >= 0 && volume > reset);
    CHECK(event_index(events, count, DM_PPQ / 10, MIDI_SYSEX, -1, -1) < 0);
    free(events);
    DmBytes out = {0};
    CHECK(dm_midi(&p, &out, error, sizeof(error)));
    dm_project_free(&p);
    dm_project_init(&p);
    CHECK(dm_import(&p, out.data, out.size, -1, 0, error, sizeof(error)));
    dm_bytes_free(&out);
    CHECK(dm_export_timeline(&p, &events, &count, &duration, error, sizeof(error)));
    int programs = 0;

    for (size_t i = 0; i < count; ++i) {
        programs += events[i].tick == 0 && events[i].status == MIDI_PROGRAM;
    }

    CHECK(programs == 1);
    free(events);
    dm_project_free(&p);
}

/* A note over beats 0-8 with: NRPN 1/32 = 65 at beat 0; RPN 0/1 (fine tuning) = 70 at beat 1 and
   an increment at beat 2; NRPN 5/5 selected and incremented at beat 3 (its value is unknown); a
   null select and a bare data entry (12) at beat 4. */
static const uint8_t parameter_file[] = {
    0x4d, 0x54, 0x68, 0x64, 0,    0,    0,    6,    0,    0,    0,    1,    1,    0xe0, 0x4d, 0x54,
    0x72, 0x6b, 0,    0,    0,    0x45, 0,    0x90, 0x3c, 0x64, 0,    0xb0, 0x63, 1,    0,    0xb0,
    0x62, 0x20, 0,    0xb0, 6,    0x41, 0x83, 0x60, 0xb0, 0x65, 0,    0,    0xb0, 0x64, 1,    0,
    0xb0, 6,    0x46, 0x83, 0x60, 0xb0, 0x60, 0,    0x83, 0x60, 0xb0, 0x63, 5,    0,    0xb0, 0x62,
    5,    0,    0xb0, 0x60, 0,    0x83, 0x60, 0xb0, 0x65, 0x7f, 0,    0xb0, 0x64, 0x7f, 0,    0xb0,
    6,    0xc,  0x8f, 0,    0x80, 0x3c, 0,    0,    0xff, 0x2f, 0};

/* Each value of parameter_file plays as its parameter, as a whole message group. */
static void check_parameter_file(const DmProject *p) {
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration = 0;
    char error[256] = {0};
    CHECK(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)));
    const uint32_t cc = MIDI_CONTROL;
    /* NRPN 1/32 = 65, and no bend-range message in its place. */
    CHECK(event_index(events, count, 0, cc, MIDI_CC_NRPN_MSB, 1) >= 0 &&
          event_index(events, count, 0, cc, MIDI_CC_NRPN_LSB, 32) >= 0 &&
          event_index(events, count, 0, cc, MIDI_CC_DATA, 65) >= 0);
    CHECK(event_index(events, count, 0, cc, MIDI_CC_RPN_MSB, 0) < 0);
    /* RPN 0/1 = 70, then 71 by the increment. */
    CHECK(event_index(events, count, DM_PPQ, cc, MIDI_CC_RPN_LSB, 1) >= 0 &&
          event_index(events, count, DM_PPQ, cc, MIDI_CC_DATA, 70) >= 0);
    CHECK(event_index(events, count, 2 * DM_PPQ, cc, MIDI_CC_RPN_LSB, 1) >= 0 &&
          event_index(events, count, 2 * DM_PPQ, cc, MIDI_CC_DATA, 71) >= 0);
    /* The increment of NRPN 5/5, whose value is unknown, plays as written with its select. */
    CHECK(event_index(events, count, 3 * DM_PPQ, cc, MIDI_CC_NRPN_LSB, 5) >= 0 &&
          event_index(events, count, 3 * DM_PPQ, cc, MIDI_CC_INCREMENT, 0) >= 0);
    /* The bare data entry plays as written, alone. */
    CHECK(event_index(events, count, 4 * DM_PPQ, cc, MIDI_CC_DATA, 12) >= 0 &&
          event_index(events, count, 4 * DM_PPQ, cc, MIDI_CC_RPN_MSB, 0) < 0 &&
          event_index(events, count, 4 * DM_PPQ, cc, MIDI_CC_NRPN_MSB, -1) < 0);
    free(events);
}

/* F11: RPN and NRPN parameters are settings of their own. */
static void parameters(void) {
    DmProject p;
    dm_project_init(&p);
    char error[256] = {0};
    CHECK(dm_import(&p, parameter_file, sizeof(parameter_file), -1, 0, error, sizeof(error)));
    check_parameter_file(&p);
    /* Export and re-import keep every value, and add nothing. */
    DmBytes out = {0};
    CHECK(dm_midi(&p, &out, error, sizeof(error)));
    DmProject again;
    dm_project_init(&again);
    CHECK(dm_import(&again, out.data, out.size, -1, 0, error, sizeof(error)));
    check_parameter_file(&again);
    DmExportEvent *first = NULL, *second = NULL;
    size_t first_count = 0, second_count = 0;
    uint64_t duration = 0;
    CHECK(dm_export_timeline(&p, &first, &first_count, &duration, error, sizeof(error)) &&
          dm_export_timeline(&again, &second, &second_count, &duration, error, sizeof(error)));
    CHECK(first_count == second_count);
    free(first);
    free(second);
    dm_bytes_free(&out);
    dm_project_free(&again);
    dm_project_free(&p);

    /* Two automation patterns set NRPN 1/32: the newest point wins, as for a controller. */
    pair_project(&p);
    int a = add_auto_pattern(&p, "A", 8), b = add_auto_pattern(&p, "B", 8);
    const int beats_a[] = {0, 2}, values_a[] = {10, 30};

    for (int k = 0; k < 2; ++k) {
        const int cc[] = {MIDI_CC_NRPN_MSB, MIDI_CC_NRPN_LSB, MIDI_CC_DATA};
        const int v[] = {1, 32, values_a[k]};

        for (int i = 0; i < 3; ++i) {
            CHECK(dm_automation_add(&p, (DmAutomation){.beat = beats_a[k],
                                                       .type = MIDI_CONTROL,
                                                       .a = cc[i],
                                                       .b = v[i],
                                                       .pattern = a}));
            CHECK(k || dm_automation_add(&p, (DmAutomation){.beat = 0,
                                                            .type = MIDI_CONTROL,
                                                            .a = cc[i],
                                                            .b = i == 2 ? 20 : v[i],
                                                            .pattern = b}));
        }
    }

    CHECK(dm_clip_add(&p, (DmClip){.pattern = a, .channel = 0, .lane = 1}));
    CHECK(dm_clip_add(&p, (DmClip){.beat = 1, .pattern = b, .channel = 0, .lane = 2, .length = 7}));
    CHECK(export_cc_at(&p, 0, MIDI_CC_DATA, 0.5) == 10);
    CHECK(export_cc_at(&p, 0, MIDI_CC_DATA, 1.5) == 20);
    CHECK(export_cc_at(&p, 0, MIDI_CC_DATA, 2.5) == 30);
    dm_project_free(&p);
}

static void automation_precedence(void) {
    /* 1. Newest placed point wins among overlapping automation patterns. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 2;
        int a = add_auto_pattern(&p, "A", 8);
        int b = add_auto_pattern(&p, "B", 6);
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 6, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40, .pattern = a}));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 1, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 90, .pattern = b}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 2, .pattern = b, .channel = 0, .lane = 2}));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 4) == 90);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 6) == 40);
        dm_project_free(&p);
    }

    /* 2. Line steps keep their starting node's placed tick; only the end node wins later. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 2;
        int a = add_auto_pattern(&p, "Line A", 10);
        int b = add_auto_pattern(&p, "Point B", 8);
        CHECK(dm_automation_add(&p, (DmAutomation){.beat = 0,
                                                   .type = MIDI_CONTROL,
                                                   .a = MIDI_CC_EXPRESSION,
                                                   .b = 20,
                                                   .pattern = a,
                                                   .curve = 1}));
        CHECK(dm_automation_add(&p, (DmAutomation){.beat = 8,
                                                   .type = MIDI_CONTROL,
                                                   .a = MIDI_CC_EXPRESSION,
                                                   .b = 100,
                                                   .pattern = a,
                                                   .curve = 1}));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 60, .pattern = b}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 2, .pattern = b, .channel = 0, .lane = 2}));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 4) == 60);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 8) == 100);
        dm_project_free(&p);
    }

    /* 3. Same beat: higher row (smaller lane number) wins; swapping subrows swaps winner. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 2;
        int a = add_auto_pattern(&p, "Row A", 8);
        int b = add_auto_pattern(&p, "Row B", 8);
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 4, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 30, .pattern = a}));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 4, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 80, .pattern = b}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 2}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = b, .channel = 0, .lane = 1}));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 4) == 80);
        p.clips[0].lane = 1;
        p.clips[1].lane = 2;
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 4) == 30);
        dm_project_free(&p);
    }

    /* 4. Note pattern beats both automation patterns while its placement plays. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 2;
        int a = add_auto_pattern(&p, "Auto A", 8);
        int b = add_auto_pattern(&p, "Auto B", 8);
        int n = dm_pattern_add(&p, "Notes");
        CHECK(n >= 0);
        p.patterns[n].length = 4;
        CHECK(dm_note_add(&p.patterns[n], (DmNote){0, 1, 60, 100, 0}));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 30, .pattern = a}));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 4, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 80, .pattern = b}));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 50, .pattern = n}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = b, .channel = 0, .lane = 2}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 2, .pattern = n, .channel = 0, .lane = 0}));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 1) == 30);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 3) == 50);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 5) == 50);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 7) == 80);
        dm_project_free(&p);
    }

    /* 5. Automation pattern beats a Channel Event inside its placement; after, the event plays. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 1;
        int a = add_auto_pattern(&p, "Auto", 8);
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40, .pattern = a}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
        const uint8_t bytes[] = {MIDI_CONTROL, MIDI_CC_EXPRESSION, 95};
        DmBytes msg = {(uint8_t *)bytes, sizeof(bytes), 0};
        CHECK(dm_event_add(&p, 4, 1, 0, &msg));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 1) == 40);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 5) == 40);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 9) == 95);
        dm_project_free(&p);
    }

    /* 6. Slice: a point before offset replays at the slice start and counts as placed there. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 2;
        int a = add_auto_pattern(&p, "A", 8);
        int b = add_auto_pattern(&p, "B", 8);
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 2, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 30, .pattern = a}));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 1, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 90, .pattern = b}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
        CHECK(dm_clip_add(
            &p,
            (DmClip){.beat = 4, .pattern = b, .channel = 0, .lane = 2, .offset = 3, .length = 4}));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 3) == 30);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 5) == 90);
        dm_project_free(&p);
    }

    /* 7. A Line without restoration holds its last value until the placement end, then releases. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 1;
        int a = add_auto_pattern(&p, "LineHold", 8);
        CHECK(dm_automation_add(&p, (DmAutomation){.beat = 0,
                                                   .type = MIDI_CONTROL,
                                                   .a = MIDI_CC_EXPRESSION,
                                                   .b = 20,
                                                   .curve = 1,
                                                   .pattern = a}));
        CHECK(dm_automation_add(&p, (DmAutomation){.beat = 4,
                                                   .type = MIDI_CONTROL,
                                                   .a = MIDI_CC_EXPRESSION,
                                                   .b = 80,
                                                   .curve = 1,
                                                   .pattern = a}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 6) == 80);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 8) == 127);
        dm_project_free(&p);
    }

    /* 8. A Graph lane returns one cell after its last bar by default, or holds with graph_hold. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 1;
        int a = add_auto_pattern(&p, "GraphHold", 4);
        p.patterns[a].graph_lanes[MIDI_CC_EXPRESSION] = 16;
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40, .pattern = a}));
        CHECK(dm_automation_add(&p, (DmAutomation){.beat = 0.25,
                                                   .type = MIDI_CONTROL,
                                                   .a = MIDI_CC_EXPRESSION,
                                                   .b = 90,
                                                   .pattern = a}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 0.25) == 90);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 0.5) == 127);
        p.patterns[a].graph_hold[MIDI_CC_EXPRESSION] = true;
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 0.5) == 90);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 3) == 90);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 4) == 127);
        dm_project_free(&p);
    }
}

static bool has_cc_at_tick(const DmProject *p, int channel, int controller, uint64_t tick,
                           int *value_out) {
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration = 0;
    char error[256] = {0};
    CHECK(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)));
    bool found = false;

    for (size_t i = 0; i < count; ++i) {
        if (events[i].tick == tick && events[i].status == (uint32_t)(MIDI_CONTROL | channel) &&
            events[i].a == (uint32_t)controller) {
            found = true;

            if (value_out) {
                *value_out = (int)events[i].b;
            }
        }
    }

    free(events);
    return found;
}

static void one_rule_set_timeline(void) {
    /* 1. Contiguous slices match the unsliced placement. */
    {
        DmProject whole, sliced;
        dm_project_init(&whole);
        dm_project_init(&sliced);
        whole.channels[0].enabled = sliced.channels[0].enabled = true;
        whole.lane_count[0] = sliced.lane_count[0] = 1;
        int w_pat = add_auto_pattern(&whole, "Whole", 8);
        int s_pat = add_auto_pattern(&sliced, "Sliced", 8);
        CHECK(dm_automation_add(&whole, (DmAutomation){.beat = 1,
                                                       .type = MIDI_CONTROL,
                                                       .a = MIDI_CC_EXPRESSION,
                                                       .b = 40,
                                                       .pattern = w_pat}));
        CHECK(dm_automation_add(&whole, (DmAutomation){.beat = 5,
                                                       .type = MIDI_CONTROL,
                                                       .a = MIDI_CC_EXPRESSION,
                                                       .b = 90,
                                                       .pattern = w_pat}));
        CHECK(dm_automation_add(&sliced, (DmAutomation){.beat = 1,
                                                        .type = MIDI_CONTROL,
                                                        .a = MIDI_CC_EXPRESSION,
                                                        .b = 40,
                                                        .pattern = s_pat}));
        CHECK(dm_automation_add(&sliced, (DmAutomation){.beat = 5,
                                                        .type = MIDI_CONTROL,
                                                        .a = MIDI_CC_EXPRESSION,
                                                        .b = 90,
                                                        .pattern = s_pat}));
        CHECK(dm_clip_add(&whole, (DmClip){.beat = 0, .pattern = w_pat, .channel = 0, .lane = 1}));
        CHECK(dm_clip_add(
            &sliced,
            (DmClip){
                .beat = 0, .pattern = s_pat, .channel = 0, .lane = 1, .offset = 0, .length = 4}));
        CHECK(dm_clip_add(
            &sliced,
            (DmClip){
                .beat = 4, .pattern = s_pat, .channel = 0, .lane = 1, .offset = 4, .length = 4}));
        const double beats[] = {0.5, 2.0, 4.5, 6.0};

        for (size_t i = 0; i < sizeof(beats) / sizeof(*beats); ++i) {
            CHECK(export_cc_at(&whole, 0, MIDI_CC_EXPRESSION, beats[i]) ==
                  export_cc_at(&sliced, 0, MIDI_CC_EXPRESSION, beats[i]));
        }

        dm_project_free(&whole);
        dm_project_free(&sliced);
    }

    /* 2. A slice followed by a gap and a later note returns to the default at the slice end. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 1;
        int a = add_auto_pattern(&p, "Slice", 8);
        int n = dm_pattern_add(&p, "Note");
        CHECK(n >= 0);
        p.patterns[n].length = 2;
        CHECK(dm_note_add(&p.patterns[n], (DmNote){0, 2, 60, 100, 0}));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 1, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40, .pattern = a}));
        CHECK(dm_clip_add(
            &p,
            (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1, .offset = 0, .length = 4}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 6, .pattern = n, .channel = 0, .lane = 0}));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 2) == 40);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 4) == 127);
        dm_project_free(&p);
    }

    /* 3. A release after the last note-off is dropped; the same release before a later note is
       written. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 1;
        int a = add_auto_pattern(&p, "Auto", 4);
        int n = dm_pattern_add(&p, "Note");
        CHECK(n >= 0);
        p.patterns[n].length = 2;
        CHECK(dm_note_add(&p.patterns[n], (DmNote){0, 2, 60, 100, 0}));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40, .pattern = a}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = n, .channel = 0, .lane = 0}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
        CHECK(!has_cc_at_tick(&p, 0, MIDI_CC_EXPRESSION, 4 * DM_PPQ, NULL));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 6, .pattern = n, .channel = 0, .lane = 0}));
        int released = -1;
        CHECK(has_cc_at_tick(&p, 0, MIDI_CC_EXPRESSION, 4 * DM_PPQ, &released) && released == 127);
        dm_project_free(&p);
    }

    /* 4. Adding an automation pattern on channel 3 leaves channel 1's flattened events unchanged.
     */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 1;
        int n = dm_pattern_add(&p, "Ch1");
        CHECK(n >= 0);
        p.patterns[n].length = 4;
        CHECK(dm_note_add(&p.patterns[n], (DmNote){0, 2, 60, 100, 0}));
        CHECK(dm_automation_add(&p, (DmAutomation){.beat = 0.5,
                                                   .type = MIDI_CONTROL,
                                                   .a = MIDI_CC_EXPRESSION,
                                                   .b = 64,
                                                   .pattern = n}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = n, .channel = 0, .lane = 0}));

        DmExportEvent *before = NULL, *after = NULL;
        size_t before_count = 0, after_count = 0;
        uint64_t dur = 0;
        char error[256] = {0};
        CHECK(dm_export_timeline(&p, &before, &before_count, &dur, error, sizeof(error)));

        p.channels[2].enabled = true;
        p.lane_count[2] = 1;
        int a3 = add_auto_pattern(&p, "Ch3", 4);
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 20, .pattern = a3}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a3, .channel = 2, .lane = 1}));
        CHECK(dm_export_timeline(&p, &after, &after_count, &dur, error, sizeof(error)));

        size_t ch1_before = 0, ch1_after = 0;

        for (size_t i = 0; i < before_count; ++i) {
            if (before[i].status < MIDI_SYSTEM && (before[i].status & MIDI_CHANNEL_MASK) == 0) {
                while (ch1_after < after_count &&
                       (after[ch1_after].status >= MIDI_SYSTEM ||
                        (after[ch1_after].status & MIDI_CHANNEL_MASK) != 0)) {
                    ++ch1_after;
                }

                CHECK(ch1_after < after_count);
                CHECK(before[i].tick == after[ch1_after].tick &&
                      before[i].status == after[ch1_after].status &&
                      before[i].a == after[ch1_after].a && before[i].b == after[ch1_after].b);
                ++ch1_before;
                ++ch1_after;
            }
        }

        while (ch1_after < after_count) {
            CHECK(after[ch1_after].status >= MIDI_SYSTEM ||
                  (after[ch1_after].status & MIDI_CHANNEL_MASK) != 0);
            ++ch1_after;
        }

        CHECK(ch1_before > 0);
        free(before);
        free(after);
        dm_project_free(&p);
    }

    /* 5. Line restoration in a note pattern returns to an earlier discrete point in that pattern,
       or releases to the automation pattern underneath when none precedes it. */
    {
        DmProject with_prior, without_prior;
        dm_project_init(&with_prior);
        dm_project_init(&without_prior);
        with_prior.channels[0].enabled = without_prior.channels[0].enabled = true;
        with_prior.lane_count[0] = without_prior.lane_count[0] = 1;

        int a1 = add_auto_pattern(&with_prior, "Auto", 8);
        int n1 = dm_pattern_add(&with_prior, "Notes");
        CHECK(n1 >= 0);
        with_prior.patterns[n1].length = 4;
        CHECK(dm_note_add(&with_prior.patterns[n1], (DmNote){0, 4, 60, 100, 0}));
        CHECK(dm_automation_add(
            &with_prior,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40, .pattern = a1}));
        CHECK(dm_automation_add(&with_prior, (DmAutomation){.beat = 0.5,
                                                            .type = MIDI_CONTROL,
                                                            .a = MIDI_CC_EXPRESSION,
                                                            .b = 70,
                                                            .pattern = n1}));
        CHECK(dm_automation_add(&with_prior, (DmAutomation){.beat = 1,
                                                            .type = MIDI_CONTROL,
                                                            .a = MIDI_CC_EXPRESSION,
                                                            .b = 100,
                                                            .pattern = n1,
                                                            .curve = 1}));
        CHECK(dm_automation_add(&with_prior, (DmAutomation){.beat = 2,
                                                            .type = MIDI_CONTROL,
                                                            .a = MIDI_CC_EXPRESSION,
                                                            .b = 100,
                                                            .pattern = n1,
                                                            .curve = 1}));
        CHECK(dm_automation_add(&with_prior, (DmAutomation){.beat = 2.001,
                                                            .type = MIDI_CONTROL,
                                                            .a = MIDI_CC_EXPRESSION,
                                                            .b = 0,
                                                            .pattern = n1,
                                                            .curve = -1}));
        CHECK(
            dm_clip_add(&with_prior, (DmClip){.beat = 0, .pattern = a1, .channel = 0, .lane = 1}));
        CHECK(
            dm_clip_add(&with_prior, (DmClip){.beat = 0, .pattern = n1, .channel = 0, .lane = 0}));
        CHECK(export_cc_at(&with_prior, 0, MIDI_CC_EXPRESSION, 1.5) == 100);
        CHECK(export_cc_at(&with_prior, 0, MIDI_CC_EXPRESSION, 2.5) == 70);

        int a2 = add_auto_pattern(&without_prior, "Auto", 8);
        int n2 = dm_pattern_add(&without_prior, "Notes");
        CHECK(n2 >= 0);
        without_prior.patterns[n2].length = 4;
        CHECK(dm_note_add(&without_prior.patterns[n2], (DmNote){0, 4, 60, 100, 0}));
        CHECK(dm_automation_add(
            &without_prior,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40, .pattern = a2}));
        CHECK(dm_automation_add(&without_prior, (DmAutomation){.beat = 1,
                                                               .type = MIDI_CONTROL,
                                                               .a = MIDI_CC_EXPRESSION,
                                                               .b = 100,
                                                               .pattern = n2,
                                                               .curve = 1}));
        CHECK(dm_automation_add(&without_prior, (DmAutomation){.beat = 2,
                                                               .type = MIDI_CONTROL,
                                                               .a = MIDI_CC_EXPRESSION,
                                                               .b = 100,
                                                               .pattern = n2,
                                                               .curve = 1}));
        CHECK(dm_automation_add(&without_prior, (DmAutomation){.beat = 2.001,
                                                               .type = MIDI_CONTROL,
                                                               .a = MIDI_CC_EXPRESSION,
                                                               .b = 127,
                                                               .pattern = n2,
                                                               .curve = -1}));
        CHECK(dm_clip_add(&without_prior,
                          (DmClip){.beat = 0, .pattern = a2, .channel = 0, .lane = 1}));
        CHECK(dm_clip_add(&without_prior,
                          (DmClip){.beat = 0, .pattern = n2, .channel = 0, .lane = 0}));
        CHECK(export_cc_at(&without_prior, 0, MIDI_CC_EXPRESSION, 1.5) == 100);
        CHECK(export_cc_at(&without_prior, 0, MIDI_CC_EXPRESSION, 2.5) == 40);

        /* A later section whose earlier section released also releases: the value in effect
           before it is what plays underneath, not the earlier section's last node. */
        for (int i = 0; i < 3; ++i) {
            double beats[] = {2.75, 3, 3.001};
            int curve = i < 2 ? 2 : -2;
            CHECK(dm_automation_add(&without_prior, (DmAutomation){.beat = beats[i],
                                                                   .type = MIDI_CONTROL,
                                                                   .a = MIDI_CC_EXPRESSION,
                                                                   .b = 90,
                                                                   .pattern = n2,
                                                                   .curve = curve}));
        }

        CHECK(export_cc_at(&without_prior, 0, MIDI_CC_EXPRESSION, 2.9) == 90);
        CHECK(export_cc_at(&without_prior, 0, MIDI_CC_EXPRESSION, 3.5) == 40);

        dm_project_free(&with_prior);
        dm_project_free(&without_prior);
    }

    /* 6. Plain SMF import exports the source's channel messages and notes with no extra release
       after the last note-off. */
    {
        /* clang-format off */
        const uint8_t track[] = {
            0, MIDI_PROGRAM | 0, 5,
            0, MIDI_CONTROL | 0, MIDI_CC_VOLUME, 110,
            0, MIDI_CONTROL | 0, MIDI_CC_PAN, 40,
            0, MIDI_NOTE_ON | 0, 60, 100,
            48, MIDI_CONTROL | 0, MIDI_CC_EXPRESSION, 80,
            48, MIDI_CONTROL | 0, MIDI_CC_VOLUME, 90,
            0, MIDI_CONTROL | 0, MIDI_CC_PAN, 80,
            0, MIDI_PITCH_BEND | 0, 0, 80,
            96, MIDI_NOTE_OFF | 0, 60, 0,
            0, MIDI_META, MIDI_META_END, 0,
        };
        /* clang-format on */
        DmBytes file = one_track(track, sizeof(track));
        DmProject p;
        dm_project_init(&p);
        char error[256] = {0};
        CHECK(dm_import(&p, file.data, file.size, 0, 0, error, sizeof(error)));
        CHECK(p.channels[0].enabled && !p.channels[1].enabled);

        DmExportEvent *events = NULL;
        size_t count = 0;
        uint64_t dur = 0;
        CHECK(dm_export_timeline(&p, &events, &count, &dur, error, sizeof(error)));

        const struct {
            uint64_t tick;
            uint32_t status, a, b;
        } expected[] = {
            {0, MIDI_META, 500000, 0},
            /* The imported beat-0 program, played as the mixer's; the setup does not repeat it. */
            {0, MIDI_PROGRAM | 0, 5, 0},
            {0, MIDI_CONTROL | 0, MIDI_CC_VOLUME, 110},
            {0, MIDI_CONTROL | 0, MIDI_CC_PAN, 40},
            {0, MIDI_NOTE_ON | 0, 60, 100},
            {480, MIDI_CONTROL | 0, MIDI_CC_EXPRESSION, 80},
            {960, MIDI_CONTROL | 0, MIDI_CC_VOLUME, 90},
            {960, MIDI_CONTROL | 0, MIDI_CC_PAN, 80},
            {960, MIDI_PITCH_BEND | 0, 0, 80},
            {1920, MIDI_NOTE_OFF | 0, 60, 0},
        };

        CHECK(count == sizeof(expected) / sizeof(*expected));

        for (size_t i = 0; i < count; ++i) {
            CHECK(events[i].tick == expected[i].tick && events[i].status == expected[i].status &&
                  events[i].a == expected[i].a && events[i].b == expected[i].b);
        }

        free(events);
        dm_project_free(&p);
        dm_bytes_free(&file);
    }

    /* 7. A Graph lane's last bar releases after one cell to a Channel Event underneath. */
    {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].enabled = true;
        p.lane_count[0] = 1;
        int a = add_auto_pattern(&p, "GraphRelease", 4);
        p.patterns[a].graph_lanes[MIDI_CC_EXPRESSION] = 16;
        const uint8_t bytes[] = {MIDI_CONTROL, MIDI_CC_EXPRESSION, 55};
        DmBytes msg = {(uint8_t *)bytes, sizeof(bytes), 0};
        CHECK(dm_event_add(&p, 0, 1, 0, &msg));
        CHECK(dm_automation_add(
            &p,
            (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 90, .pattern = a}));
        CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 1}));
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 0) == 90);
        CHECK(export_cc_at(&p, 0, MIDI_CC_EXPRESSION, 0.25) == 55);
        dm_project_free(&p);
    }
}

static void export_channel(void) {
    DmProject p;
    dm_project_init(&p);
    p.channels[0].enabled = true;
    p.channels[1].enabled = true;
    p.channels[0].program = 3;
    p.channels[1].program = 11;
    p.lane_count[0] = 2;
    p.lane_count[1] = 1;

    int a = add_auto_pattern(&p, "Under", 8);
    int b = add_auto_pattern(&p, "Over", 8);
    int c = dm_pattern_add(&p, "Ch1");
    CHECK(c >= 0);
    p.patterns[c].length = 4;
    CHECK(dm_note_add(&p.patterns[c], (DmNote){0, 4, 60, 100, 0}));

    CHECK(dm_automation_add(
        &p, (DmAutomation){
                .beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 40, .pattern = a}));
    CHECK(dm_automation_add(&p, (DmAutomation){.beat = 2,
                                               .type = MIDI_CONTROL,
                                               .a = MIDI_CC_EXPRESSION,
                                               .b = 90,
                                               .pattern = b,
                                               .curve = 1}));
    CHECK(dm_automation_add(&p, (DmAutomation){.beat = 3,
                                               .type = MIDI_CONTROL,
                                               .a = MIDI_CC_EXPRESSION,
                                               .b = 110,
                                               .pattern = b,
                                               .curve = 1}));
    CHECK(dm_automation_add(&p, (DmAutomation){.beat = 4,
                                               .type = MIDI_CONTROL,
                                               .a = MIDI_CC_EXPRESSION,
                                               .b = 127,
                                               .pattern = b,
                                               .curve = -1}));
    CHECK(dm_automation_add(
        &p, (DmAutomation){
                .beat = 1, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 25, .pattern = c}));

    CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = a, .channel = 0, .lane = 2, .length = 8}));
    CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = b, .channel = 0, .lane = 1, .length = 8}));
    CHECK(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = c, .channel = 1, .lane = 0, .length = 4}));

    const uint8_t bytes[] = {MIDI_CONTROL | 0, MIDI_CC_EXPRESSION, 60};
    DmBytes msg = {(uint8_t *)bytes, sizeof(bytes), 0};
    CHECK(dm_event_add(&p, 5, 0, 0, &msg));

    DmExportEvent *all = NULL, *one = NULL;
    size_t all_count = 0, one_count = 0;
    uint64_t duration = 0;
    char error[256] = {0};
    CHECK(dm_export_timeline(&p, &all, &all_count, &duration, error, sizeof(error)));

    for (int ch = 0; ch < 2; ++ch) {
        CHECK(dm_export_channel(&p, ch, &one, &one_count, error, sizeof(error)));
        size_t matched = 0;

        for (size_t i = 0; i < all_count; ++i) {
            if (all[i].status < MIDI_SYSTEM && (int)(all[i].status & MIDI_CHANNEL_MASK) != ch) {
                continue;
            }

            CHECK(matched < one_count);
            CHECK(one[matched].tick == all[i].tick && one[matched].status == all[i].status &&
                  one[matched].a == all[i].a && one[matched].b == all[i].b &&
                  one[matched].port == all[i].port && one[matched].flags == all[i].flags);
            ++matched;
        }

        CHECK(matched == one_count);
        free(one);
        one = NULL;
        one_count = 0;
    }

    CHECK(!dm_export_channel(&p, -1, &one, &one_count, error, sizeof(error)));
    CHECK(!dm_export_channel(&p, 16, &one, &one_count, error, sizeof(error)));
    free(all);
    dm_project_free(&p);
}

int main(void) {
    channel_ports();
    automation_precedence();
    one_rule_set_timeline();
    section_returns();
    bend_range_pairs();
    export_ports_keep_numbers();
    commands_and_resets();
    parameters();
    export_channel();
    midi_metadata();
    midi_metadata_views();
    bounded_curves();
    mus_input();
    pattern_meter();
    roundtrip();
    model();
    project_validation();
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
