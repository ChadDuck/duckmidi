/* Generate Patterns: the preview leaves the project alone, repeated phrases become linked
   placements, controller values and note timelines survive, and Apply is one undo step. */
#include "editor/internal.h"
#include "../shared/pattern_generation.h"
#include "text/en.h"
#include <assert.h>
#define TEST_NAME "pattern generation"
#include "test_common.h"

/* Draws a few frames so windows opened by the step before are measured and shown. */
static void draw(Editor *e) {
    for (int frame = 0; frame < 6; frame++) {
        ui_frame();
        editor_frame(e, 1400, 900);
        ui_render(e->renderer);
    }
}

static void fixture(Editor *e) {
    ed_save_free(e);
    ed_release_piano(e);
    ed_clear_history(e->undo, &e->undo_count);
    ed_clear_history(e->redo, &e->redo_count);
    dm_project_free(&e->project);
    dm_project_init(&e->project);
    ++e->project_session;
    int p = dm_pattern_add(&e->project, "Generated fixture");
    assert(p == 0);
    assert(dm_note_add(&e->project.patterns[0], (DmNote){0, 1, 60, 100, 0}));
    e->active = true;
    e->selected_pattern = e->selected_channel = e->pattern_channel = 0;
    e->drag = DRAG_NONE;
    e->tool = TOOL_SELECT;
    e->percussion_window = false;
}

static void wait_generation(Editor *e) {
    uint64_t start = SDL_GetTicks();

    while (ed_generate_busy(e) && SDL_GetTicks() - start < 15000) {
        ed_generate_poll(e);
        SDL_Delay(1);
    }

    assert(!ed_generate_busy(e));
}

static int cc_value(const DmProject *p, uint64_t tick, int cc) {
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration;
    char error[256];
    assert(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)));
    int value = -1;

    for (size_t i = 0; i < count; i++) {
        if (events[i].tick <= tick && events[i].status == 0xb0 && events[i].a == (unsigned)cc) {
            value = (int)events[i].b;
        }
    }

    free(events);
    return value;
}

static void generation(Editor *e) {
    fixture(e);
    DmPattern *p = ed_pattern(e);
    p->count = 0;

    for (int repeat = 0; repeat < 3; repeat++) {
        for (int j = 0; j < 4; j++) {
            assert(dm_note_add(p, (DmNote){repeat * 3.125 + j * .625, .25, (uint8_t)(60 + j * 2),
                                           (uint8_t)(j == 1 && repeat == 2 ? 99 : 100), 0}));
        }
    }

    assert(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0}));
    DmAutomation a = {
        .pattern = 0, .type = 0xb0, .channel = 0, .a = 1, .b = 20, .curve = 1, .anchor = 60};
    assert(dm_automation_add(&e->project, a));
    a.beat = 2;
    a.b = 100;
    a.bend = 12;
    assert(dm_automation_add(&e->project, a));
    a.beat = 2.01;
    a.curve = -1;
    a.b = 20;
    assert(dm_automation_add(&e->project, a));
    int lfo = dm_pattern_add(&e->project, "Authored LFO");
    assert(lfo >= 0);
    e->project.patterns[lfo].kind = 1;
    e->project.patterns[lfo].mode = 1;
    e->project.patterns[lfo].event = 11;
    e->project.patterns[lfo].tension = .37;
    e->project.patterns[lfo].length = 9;
    e->project.lane_count[0] = 1;
    snprintf(e->project.lane_names[0][0], 64, "Original lane");
    assert(dm_clip_add(&e->project, (DmClip){.pattern = lfo, .channel = 0, .lane = 1}));
    DmBytes before = {0}, after = {0};
    assert(dm_pack(&e->project, &before));
    ed_generate_start(e);
    wait_generation(e);
    const DmProject *preview = ed_generate_preview(e);
    assert(preview && preview->pattern_count > 1);
    draw(e);
    assert(dm_pack(&e->project, &after) && before.size == after.size &&
           !memcmp(before.data, after.data, before.size));
    dm_bytes_free(&after);

    for (unsigned t = 0; t < 9000; t += 100) {
        assert(cc_value(&e->project, t, 1) == cc_value(preview, t, 1));
    }

    size_t notes = 0;
    bool different_velocity = false, curves = false;

    for (size_t i = 0; i < preview->clip_count; i++) {
        DmPattern q = preview->patterns[preview->clips[i].pattern];

        if (!q.kind) {
            notes += q.count;

            for (size_t j = 0; j < q.count; j++) {
                different_velocity |= q.notes[j].velocity == 99;
            }
        }
    }

    for (size_t i = 0; i < preview->automation_count; i++) {
        curves |= preview->automation[i].bend == 12;
    }

    assert(notes == 12 && different_velocity && curves);
    bool lfo_retained = false;

    for (size_t i = 0; i < preview->pattern_count; i++) {
        lfo_retained |= preview->patterns[i].mode == 1 && preview->patterns[i].tension == .37;
    }

    assert(lfo_retained && !strcmp(preview->lane_names[0][0], "Original lane"));
    DmBytes generated = {0};
    assert(dm_pack(preview, &generated));
    DmProject check;
    dm_project_init(&check);
    char error[256];
    assert(dm_unpack(&check, generated.data, generated.size, error, sizeof(error)));
    dm_project_free(&check);
    dm_bytes_free(&generated);
    ed_pattern(e)->notes[0].velocity = 29;
    ed_changed(e);
    assert(!ed_generate_apply(e));
    ed_generate_start(e);
    wait_generation(e);
    assert(ed_generate_apply(e) && e->dirty);
    assert(e->undo_count == 1);
    ed_undo(e, false);
    assert(e->project.patterns[0].count == 12 && e->project.patterns[0].notes[0].velocity == 29);
    ed_generate_start(e);
    e->generate_window = false;
    wait_generation(e);
    ed_generate_poll(e);
    assert(!e->pattern_job);
    dm_bytes_free(&before);
    puts(
        "PASS pattern generation preview, repeated phrases, velocity variants, editable curves, stale edits, Apply/undo and cancellation");
}

static int note_order(const void *a, const void *b) {
    const DmExportEvent *x = a, *y = b;

    if (x->tick != y->tick) {
        return x->tick < y->tick ? -1 : 1;
    }

    if (x->status != y->status) {
        return (int)x->status - (int)y->status;
    }

    if (x->a != y->a) {
        return (int)x->a - (int)y->a;
    }

    return (int)x->b - (int)y->b;
}

static void control_update(int state[16][260], DmExportEvent event) {
    unsigned type = event.status & 0xf0, ch = event.status & 15;
    int index = -1, value = (int)event.b;

    if (type == 0xb0) {
        index = (int)event.a;
    } else if (type == 0xc0) {
        index = 128;
        value = (int)event.a;
    } else if (type == 0xe0) {
        index = 129;
        value = (int)(event.a | (event.b << 7));
    } else if (type == 0xd0) {
        index = 130;
        value = (int)event.a;
    } else if (type == 0xa0) {
        index = 131 + (int)event.a;
    }

    if (index >= 0) {
        state[ch][index] = value;
    }
}

static void compare_controls(const char *file, const DmExportEvent *before, size_t bn,
                             const DmExportEvent *after, size_t an) {
    int source[16][260], generated[16][260];

    for (int ch = 0; ch < 16; ch++) {
        for (int c = 0; c < 260; c++) {
            source[ch][c] = generated[ch][c] = -1;
        }
    }

    size_t i = 0, j = 0;

    while (i < bn || j < an) {
        uint64_t tick = i < bn ? before[i].tick : UINT64_MAX;

        if (j < an && after[j].tick < tick) {
            tick = after[j].tick;
        }

        while (i < bn && before[i].tick == tick) {
            control_update(source, before[i++]);
        }

        while (j < an && after[j].tick == tick) {
            control_update(generated, after[j++]);
        }

        for (int ch = 0; ch < 16; ch++) {
            for (int c = 0; c < 260; c++) {
                if (source[ch][c] >= 0 && source[ch][c] != generated[ch][c]) {
                    fprintf(stderr, "Control mismatch %s tick %llu channel %d control %d: %d/%d\n",
                            file, (unsigned long long)tick, ch, c, source[ch][c], generated[ch][c]);
                    assert(false);
                }
            }
        }
    }
}

/* The generated song plays the same notes and controller values as its source, and its layout
   is usable: no placements overlap on a row, and every note pattern spans at least a beat. */
static size_t same_song(const char *name, const DmProject *source, const DmProject *result) {
    char error[512];
    DmExportEvent *before = NULL, *after = NULL;
    size_t bn = 0, an = 0;
    uint64_t bd, ad;
    assert(dm_export_timeline(source, &before, &bn, &bd, error, sizeof(error)) &&
           dm_export_timeline(result, &after, &an, &ad, error, sizeof(error)));
    compare_controls(name, before, bn, after, an);
    qsort(before, bn, sizeof(*before), note_order);
    qsort(after, an, sizeof(*after), note_order);
    size_t i = 0, j = 0, notes = 0;

    for (;;) {
        while (i < bn && (before[i].status & 0xe0) != 0x80) {
            i++;
        }

        while (j < an && (after[j].status & 0xe0) != 0x80) {
            j++;
        }

        if (i == bn || j == an) {
            assert(i == bn && j == an);
            break;
        }

        if (before[i].tick != after[j].tick || before[i].status != after[j].status ||
            before[i].a != after[j].a || before[i].b != after[j].b) {
            fprintf(stderr, "Note mismatch %s index %zu: %llu %x %u %u / %llu %x %u %u\n", name,
                    notes, (unsigned long long)before[i].tick, before[i].status, before[i].a,
                    before[i].b, (unsigned long long)after[j].tick, after[j].status, after[j].a,
                    after[j].b);
            assert(false);
        }

        i++;
        j++;
        notes++;
    }

    for (size_t a = 0; a < result->clip_count; ++a) {
        DmClip x = result->clips[a];
        assert(x.lane || x.row < result->note_rows[x.channel]);
        assert(result->patterns[x.pattern].kind || result->patterns[x.pattern].length >= 1);

        for (size_t k = a + 1; k < result->clip_count; ++k) {
            DmClip y = result->clips[k];
            assert(x.channel != y.channel || x.lane != y.lane || x.row != y.row ||
                   x.beat >= y.beat + dm_clip_length(result, y) - 1e-9 ||
                   y.beat >= x.beat + dm_clip_length(result, x) - 1e-9);
        }
    }

    assert(fabs(dm_duration(source) - dm_duration(result)) < 1e-6);
    free(before);
    free(after);
    return notes;
}

static void imported_generation(void) {
    const char *files[] = {"samples/imp_night.mid", "samples/e2m1.mus", "samples/D_46_D15.mid",
                           "samples/d_01_51.mus"};

    for (unsigned f = 0; f < 4; f++) {
        DmProject source, result;
        dm_project_init(&source);
        dm_project_init(&result);
        DmBytes data = {0};
        char error[512];
        DmGenerationStats stats;
        assert(dm_read(files[f], &data, error, sizeof(error)) &&
               dm_import(&source, data.data, data.size, 0, 0, error, sizeof(error)));
        assert(dm_generate_patterns(&source, &result, NULL, NULL, &stats, error, sizeof(error)));
        size_t notes = same_song(files[f], &source, &result);
        /* Phrases group many notes; the old note-group splitter averaged two or three. */
        assert(stats.note_patterns * 10 < notes);
        printf("PASS generation note timeline: %s (%zu events, %zu patterns, %zu linked repeats)\n",
               files[f], notes, stats.note_patterns, stats.linked_placements);
        dm_bytes_free(&data);
        dm_project_free(&source);
        dm_project_free(&result);
    }
}

/* Placements as the editor makes them today: a slice showing the middle of its pattern, on the
   channel's second note row, and a bar repeated off the bar grid. */
static void sliced_generation(void) {
    DmProject source, result;
    dm_project_init(&source);
    dm_project_init(&result);
    int id = dm_pattern_add(&source, "Riff");
    assert(id >= 0);

    for (int bar = 0; bar < 6; ++bar) {
        for (int j = 0; j < 4; ++j) {
            assert(dm_note_add(&source.patterns[id],
                               (DmNote){bar * 3.5 + j * .75, .5, (uint8_t)(60 + j), 100, 0}));
        }
    }

    dm_pattern_fit(&source, &source.patterns[id]);
    source.channels[0].enabled = true;
    source.note_rows[0] = 2;
    assert(dm_clip_add(&source, (DmClip){.beat = 0, .pattern = id, .channel = 0}));
    assert(dm_clip_add(
        &source,
        (DmClip){
            .beat = 32, .pattern = id, .channel = 0, .row = 1, .offset = 3.5, .length = 10.5}));
    char error[256];
    DmGenerationStats stats;
    assert(dm_generate_patterns(&source, &result, NULL, NULL, &stats, error, sizeof(error)));
    size_t notes = same_song("sliced", &source, &result);
    /* Each note is an on and an off event: six whole bars and the slice's three. */
    assert(notes == 2 * (4 * 6 + 4 * 3) && stats.linked_placements > 0);
    printf("PASS generation reads slices and rows (%zu notes, %zu patterns, %zu linked)\n", notes,
           stats.note_patterns, stats.linked_placements);
    dm_project_free(&source);
    dm_project_free(&result);
}

/* An event's place in a canonical order that keeps what can be heard: by tick and port, then by
   kind in playback's same-tick order (note off, controls, note on, key pressure), then by status
   and data byte; notes struck together sort by velocity, and messages for one controller keep
   their original sequence. Only the order of simultaneous notes, and of different controllers on
   one tick, is lost. */
typedef struct {
    DmExportEvent event;
    size_t index;
} TimelineEvent;

static int event_class(uint32_t status) {
    switch (status & 0xf0) {
    case 0x80:
        return 0;
    case 0x90:
        return 2;
    case 0xa0:
        return 3;
    default:
        return 1;
    }
}

static int timeline_order(const void *left, const void *right) {
    const TimelineEvent *x = left, *y = right;
    const DmExportEvent *a = &x->event, *b = &y->event;

    if (a->tick != b->tick) {
        return a->tick < b->tick ? -1 : 1;
    }

    if (a->port != b->port) {
        return a->port < b->port ? -1 : 1;
    }

    if (event_class(a->status) != event_class(b->status)) {
        return event_class(a->status) - event_class(b->status);
    }

    if (a->status != b->status) {
        return a->status < b->status ? -1 : 1;
    }

    if (a->a != b->a) {
        return a->a < b->a ? -1 : 1;
    }

    if ((a->status & 0xe0) == 0x80 && a->b != b->b) {
        return a->b < b->b ? -1 : 1;
    }

    return x->index < y->index ? -1 : x->index > y->index;
}

static void canonical_timeline(DmExportEvent *events, size_t count) {
    TimelineEvent *items = malloc((count + 1) * sizeof(*items));
    assert(items);

    for (size_t i = 0; i < count; ++i) {
        items[i] = (TimelineEvent){events[i], i};
    }

    qsort(items, count, sizeof(*items), timeline_order);

    for (size_t i = 0; i < count; ++i) {
        events[i] = items[i].event;
    }

    free(items);
}

/* The source and the generated song play the same events (in canonical_timeline's order). */
static void same_timeline(const char *name, const DmProject *source, const DmProject *result) {
    char error[512];
    DmExportEvent *before = NULL, *after = NULL;
    size_t bn = 0, an = 0;
    uint64_t bd = 0, ad = 0;
    assert(dm_export_timeline(source, &before, &bn, &bd, error, sizeof(error)) &&
           dm_export_timeline(result, &after, &an, &ad, error, sizeof(error)));
    canonical_timeline(before, bn);
    canonical_timeline(after, an);

    if (bn != an) {
        fprintf(stderr, "Timeline count mismatch %s: %zu/%zu\n", name, bn, an);
        assert(false);
    }

    for (size_t i = 0; i < bn; ++i) {
        if (before[i].tick != after[i].tick || before[i].status != after[i].status ||
            before[i].a != after[i].a || before[i].b != after[i].b ||
            before[i].port != after[i].port) {
            fprintf(stderr,
                    "Timeline event mismatch %s index %zu: %llu %x %u %u port %u / "
                    "%llu %x %u %u port %u\n",
                    name, i, (unsigned long long)before[i].tick, before[i].status, before[i].a,
                    before[i].b, before[i].port, (unsigned long long)after[i].tick, after[i].status,
                    after[i].a, after[i].b, after[i].port);
            assert(false);
        }
    }

    free(before);
    free(after);
}

/* Controls set in one phrase and held across phrase boundaries (or differing across repeated
   phrases) keep the exported timeline identical after pattern generation. */
static void controls_cross_phrases(void) {
    DmProject source, result;
    dm_project_init(&source);
    dm_project_init(&result);
    int id = dm_pattern_add(&source, "Source");
    assert(id >= 0);

    for (int bar = 0; bar < 40; ++bar) {
        uint8_t key;

        if (bar < 32) {
            int offset = bar % 16;
            key = (uint8_t)(60 + (offset == 8 ? 4 : offset));
        } else {
            key = (uint8_t)(76 + (bar - 32));
        }

        assert(dm_note_add(&source.patterns[id], (DmNote){bar * 4.0, 1.0, key, 100, 0}));
    }

    dm_pattern_fit(&source, &source.patterns[id]);
    source.channels[0].enabled = true;
    assert(dm_clip_add(&source, (DmClip){.beat = 0, .pattern = id, .channel = 0}));

    /* CC7 = 100 at bar 1 (beat 0), held through bars 1-19, then CC7 = 60 at bar 20 (beat 76). */
    assert(dm_automation_add(&source, (DmAutomation){.pattern = id,
                                                     .type = MIDI_CONTROL,
                                                     .channel = 0,
                                                     .a = MIDI_CC_VOLUME,
                                                     .b = 100,
                                                     .beat = 0}));
    assert(dm_automation_add(&source, (DmAutomation){.pattern = id,
                                                     .type = MIDI_CONTROL,
                                                     .channel = 0,
                                                     .a = MIDI_CC_VOLUME,
                                                     .b = 60,
                                                     .beat = 19 * 4.0}));

    /* Pitch bend set at bar 15 (beat 56), held across the bar 16/17 boundary into bar 18. */
    int bend = 12288;
    assert(dm_automation_add(&source, (DmAutomation){.pattern = id,
                                                     .type = MIDI_PITCH_BEND,
                                                     .channel = 0,
                                                     .a = bend & MIDI_DATA_MASK,
                                                     .b = bend >> 7,
                                                     .beat = 14 * 4.0}));
    assert(dm_automation_add(&source, (DmAutomation){.pattern = id,
                                                     .type = MIDI_PITCH_BEND,
                                                     .channel = 0,
                                                     .a = MIDI_BEND_CENTER & MIDI_DATA_MASK,
                                                     .b = MIDI_BEND_CENTER >> 7,
                                                     .beat = 17 * 4.0}));

    /* CC11 change at bar 33 (beat 128). */
    assert(dm_automation_add(&source, (DmAutomation){.pattern = id,
                                                     .type = MIDI_CONTROL,
                                                     .channel = 0,
                                                     .a = MIDI_CC_EXPRESSION,
                                                     .b = 90,
                                                     .beat = 32 * 4.0}));

    char error[256];
    DmGenerationStats stats;
    assert(dm_generate_patterns(&source, &result, NULL, NULL, &stats, error, sizeof(error)));
    same_song("controls_cross_phrases", &source, &result);
    same_timeline("controls_cross_phrases", &source, &result);

    size_t note_patterns = 0, reused_phrases = 0;

    for (size_t i = 0; i < result.pattern_count; ++i) {
        if (result.patterns[i].kind) {
            continue;
        }

        note_patterns++;
        size_t placements = 0;

        for (size_t j = 0; j < result.clip_count; ++j) {
            if (result.clips[j].pattern == (int)i) {
                placements++;
            }
        }

        if (placements >= 2) {
            reused_phrases++;
        }
    }

    assert(note_patterns > 1 && reused_phrases >= 1 && stats.linked_placements >= 1);
    printf("PASS generation controls cross phrases (%zu patterns, %zu reused, %zu linked)\n",
           note_patterns, reused_phrases, stats.linked_placements);
    dm_project_free(&source);
    dm_project_free(&result);
}

/* Two source ports on MIDI channel 1, sounding together: each sets its own program, volume
   and bends, so both ports' controls overlap in time on one channel number. */
static const uint8_t two_ports[] = {
    0x4d, 0x54, 0x68, 0x64, 0,    0,    0,    6,    0,    0,    0,    1,    1,    0xe0, 0x4d, 0x54,
    0x72, 0x6b, 0,    0,    0,    0x59, 0,    0xff, 0x21, 1,    0,    0,    0xc0, 5,    0,    0xb0,
    7,    0x64, 0,    0x90, 0x3c, 0x64, 0,    0xff, 0x21, 1,    1,    0,    0xc0, 0x14, 0,    0xb0,
    7,    0x32, 0,    0x90, 0x41, 0x64, 0x81, 0x70, 0xff, 0x21, 1,    0,    0,    0xe0, 0,    0x50,
    0,    0xb0, 7,    0x5a, 0,    0xff, 0x21, 1,    1,    0,    0xe0, 0,    0x30, 0,    0xc0, 0x15,
    0x81, 0x70, 0xff, 0x21, 1,    0,    0,    0x80, 0x3c, 0,    0,    0xe0, 0,    0x40, 0,    0xff,
    0x21, 1,    1,    0,    0x80, 0x41, 0,    0,    0xb0, 7,    0x46, 0,    0xff, 0x2f, 0};

/* Real imports, with program changes mid-song (d_betwee), bends and bend-range changes
   (beat-it), several ports (stalks, two_ports): generation plays the same events. */
static void imported_event_timelines(void) {
    struct {
        const char *path;
        int port;
        const uint8_t *bytes;
        size_t size;
    } files[] = {
        {"samples/imp_night.mid", 0}, {"samples/e2m1.mus", 0},
        {"samples/D_46_D15.mid", 0},  {"samples/d_01_51.mus", 0},
        {"samples/d_betwee.mid", 0},  {"samples/beat-it.mid", 0},
        {"samples/stalks.mid", -1},   {"two ports", -1, two_ports, sizeof(two_ports)},
    };

    for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        DmProject source, result;
        dm_project_init(&source);
        dm_project_init(&result);
        DmBytes data = {0};
        char error[512];
        DmGenerationStats stats;

        if (files[f].bytes) {
            assert(dm_bytes(&data, files[f].bytes, files[f].size));
        } else {
            assert(dm_read(files[f].path, &data, error, sizeof(error)));
        }

        assert(dm_import(&source, data.data, data.size, files[f].port, 0, error, sizeof(error)));
        assert(!dm_generation_changes_sound(&source));
        assert(dm_generate_patterns(&source, &result, NULL, NULL, &stats, error, sizeof(error)));
        same_timeline(files[f].path, &source, &result);

        for (size_t i = 0; i < result.automation_count; i++) {
            int pat = result.automation[i].pattern;
            assert(pat >= 0 && (size_t)pat < result.pattern_count);
            assert(result.patterns[pat].kind == 1);
        }

        for (size_t i = 0; i < result.clip_count; i++) {
            DmClip a = result.clips[i];
            DmPattern pa = result.patterns[a.pattern];

            if (pa.kind != 1) {
                continue;
            }

            double a_end = a.beat + dm_clip_length(&result, a);

            for (size_t j = i + 1; j < result.clip_count; j++) {
                DmClip b = result.clips[j];
                DmPattern pb = result.patterns[b.pattern];

                if (pb.kind != 1 || a.channel != b.channel || pa.event != pb.event ||
                    pa.port != pb.port) {
                    continue;
                }

                assert(a.beat >= b.beat + dm_clip_length(&result, b) - 1e-9 ||
                       b.beat >= a_end - 1e-9);
            }
        }

        printf("PASS generation event timeline: %s (%zu automation patterns, %zu placements)\n",
               files[f].path, stats.automation_patterns, stats.placements);
        dm_bytes_free(&data);
        dm_project_free(&source);
        dm_project_free(&result);
    }
}

static void graph_lane_generation(void) {
    for (int hold = 0; hold < 2; hold++) {
        DmProject source, result;
        dm_project_init(&source);
        dm_project_init(&result);
        int id = dm_pattern_add(&source, "Graph");
        assert(id >= 0);

        for (int i = 0; i < 4; i++) {
            assert(dm_note_add(&source.patterns[id], (DmNote){i * 1.0, 0.5, 60, 100, 0}));
        }

        dm_pattern_fit(&source, &source.patterns[id]);
        source.patterns[id].graph_lanes[MIDI_CC_EXPRESSION] = 16;
        source.patterns[id].graph_hold[MIDI_CC_EXPRESSION] = hold != 0;
        source.channels[0].enabled = true;
        assert(dm_clip_add(&source, (DmClip){.beat = 0, .pattern = id, .channel = 0}));

        const double beats[] = {0.0, 0.5, 1.25, 2.5};
        const int values[] = {40, 80, 110, 60};

        for (size_t i = 0; i < 4; i++) {
            assert(dm_automation_add(&source, (DmAutomation){.pattern = id,
                                                             .type = MIDI_CONTROL,
                                                             .channel = 0,
                                                             .a = MIDI_CC_EXPRESSION,
                                                             .b = values[i],
                                                             .beat = beats[i]}));
        }

        char error[256];
        DmGenerationStats stats;
        assert(dm_generate_patterns(&source, &result, NULL, NULL, &stats, error, sizeof(error)));
        same_timeline(hold ? "graph_lane_hold" : "graph_lane_release", &source, &result);

        bool found_graph = false;

        for (size_t i = 0; i < result.pattern_count; i++) {
            DmPattern p = result.patterns[i];

            if (p.kind == 1 && p.event == MIDI_CC_EXPRESSION) {
                assert(p.graph_lanes[MIDI_CC_EXPRESSION] == 16);
                assert(p.graph_hold[MIDI_CC_EXPRESSION] == (hold != 0));
                found_graph = true;
            }
        }

        assert(found_graph);
        dm_project_free(&source);
        dm_project_free(&result);
    }

    puts("PASS generation graph lane (release and hold)");
}

static void precedence_after_generation(void) {
    DmProject source, result;
    dm_project_init(&source);
    dm_project_init(&result);
    int id = dm_pattern_add(&source, "Source");
    assert(id >= 0);
    assert(dm_note_add(&source.patterns[id], (DmNote){0, 4, 60, 100, 0}));
    dm_pattern_fit(&source, &source.patterns[id]);
    source.channels[0].enabled = true;
    assert(dm_clip_add(&source, (DmClip){.beat = 0, .pattern = id, .channel = 0}));
    assert(dm_automation_add(&source, (DmAutomation){.pattern = id,
                                                     .type = MIDI_CONTROL,
                                                     .channel = 0,
                                                     .a = MIDI_CC_VOLUME,
                                                     .b = 100,
                                                     .beat = 0}));

    char error[256];
    DmGenerationStats stats;
    assert(dm_generate_patterns(&source, &result, NULL, NULL, &stats, error, sizeof(error)));

    int max_lane = 0;

    for (size_t i = 0; i < result.clip_count; i++) {
        if (result.clips[i].channel == 0 && result.clips[i].lane > max_lane) {
            max_lane = result.clips[i].lane;
        }
    }

    int new_lane = max_lane + 1;

    if (result.lane_count[0] < new_lane) {
        result.lane_count[0] = new_lane;
    }

    int user_pat = dm_pattern_add(&result, "User CC7");
    assert(user_pat >= 0);
    result.patterns[user_pat].kind = 1;
    result.patterns[user_pat].event = MIDI_CC_VOLUME;
    result.patterns[user_pat].length = 4;
    assert(dm_automation_add(&result, (DmAutomation){.pattern = user_pat,
                                                     .type = MIDI_CONTROL,
                                                     .channel = 0,
                                                     .a = MIDI_CC_VOLUME,
                                                     .b = 50,
                                                     .beat = 2}));
    assert(dm_clip_add(&result,
                       (DmClip){.beat = 0, .pattern = user_pat, .channel = 0, .lane = new_lane}));

    assert(cc_value(&result, DM_PPQ, MIDI_CC_VOLUME) == 100);
    assert(cc_value(&result, 2 * DM_PPQ, MIDI_CC_VOLUME) == 50);
    assert(cc_value(&result, 3 * DM_PPQ, MIDI_CC_VOLUME) == 50);

    dm_project_free(&source);
    dm_project_free(&result);
    puts("PASS generation precedence with user automation row");
}

static void warning_detection(void) {
    DmProject source, result;
    dm_project_init(&source);
    dm_project_init(&result);
    DmBytes data = {0};
    char error[512];
    DmGenerationStats stats;
    assert(dm_read("samples/imp_night.mid", &data, error, sizeof(error)) &&
           dm_import(&source, data.data, data.size, 0, 0, error, sizeof(error)));
    assert(!dm_generation_changes_sound(&source));
    assert(dm_generate_patterns(&source, &result, NULL, NULL, &stats, error, sizeof(error)));
    assert(!dm_generation_changes_sound(&result));
    dm_bytes_free(&data);
    dm_project_free(&source);
    dm_project_free(&result);

    /* (a) An automation pattern over a note pattern's controls for the same setting. Alone, or
       over a different setting, it competes with nothing and generation keeps the sound. */
    DmProject p;
    dm_project_init(&p);
    int auto_pat = dm_pattern_add(&p, "Auto");
    int note_controls = dm_pattern_add(&p, "Notes");
    assert(auto_pat >= 0 && note_controls >= 0);
    p.patterns[auto_pat].kind = 1;
    p.patterns[auto_pat].event = MIDI_CC_VOLUME;
    p.patterns[auto_pat].length = 4;
    p.patterns[note_controls].length = 4;
    p.channels[0].enabled = true;
    p.lane_count[0] = 1;
    assert(dm_automation_add(
        &p,
        (DmAutomation){
            .pattern = auto_pat, .type = MIDI_CONTROL, .a = MIDI_CC_VOLUME, .b = 50, .beat = 1}));
    assert(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = auto_pat, .channel = 0, .lane = 1}));
    assert(!dm_generation_changes_sound(&p));
    assert(dm_automation_add(
        &p,
        (DmAutomation){
            .pattern = note_controls, .type = MIDI_CONTROL, .a = MIDI_CC_PAN, .b = 20, .beat = 0}));
    assert(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = note_controls, .channel = 0}));
    assert(!dm_generation_changes_sound(&p));
    assert(dm_automation_add(&p, (DmAutomation){.pattern = note_controls,
                                                .type = MIDI_CONTROL,
                                                .a = MIDI_CC_VOLUME,
                                                .b = 90,
                                                .beat = 0}));
    assert(dm_generation_changes_sound(&p));
    dm_project_free(&p);

    /* (b) Pitch bend attached to a note in a placed note pattern. */
    dm_project_init(&p);
    int note_pat = dm_pattern_add(&p, "Notes");
    assert(note_pat >= 0);
    assert(dm_note_add(&p.patterns[note_pat], (DmNote){0, 4, 60, 100, 0}));
    dm_pattern_fit(&p, &p.patterns[note_pat]);
    p.channels[0].enabled = true;
    assert(dm_automation_add(&p, (DmAutomation){.pattern = note_pat,
                                                .type = MIDI_PITCH_BEND,
                                                .channel = 0,
                                                .a = MIDI_BEND_CENTER & MIDI_DATA_MASK,
                                                .b = MIDI_BEND_CENTER >> 7,
                                                .beat = 0,
                                                .bend_note = 1}));
    assert(!dm_generation_changes_sound(&p));
    assert(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = note_pat, .channel = 0}));
    assert(dm_generation_changes_sound(&p));
    p.automation[0].bend_note = 0;
    assert(!dm_generation_changes_sound(&p));
    p.automation[0].bend_root = DM_BEND_ROOT_PITCH;
    assert(dm_generation_changes_sound(&p));
    dm_project_free(&p);

    /* (c) Overlapping note placements on the same channel sharing a control group. */
    dm_project_init(&p);
    int p0 = dm_pattern_add(&p, "P0");
    int p1 = dm_pattern_add(&p, "P1");
    assert(p0 >= 0 && p1 >= 0);
    assert(dm_note_add(&p.patterns[p0], (DmNote){0, 4, 60, 100, 0}));
    assert(dm_note_add(&p.patterns[p1], (DmNote){0, 4, 64, 100, 0}));
    dm_pattern_fit(&p, &p.patterns[p0]);
    dm_pattern_fit(&p, &p.patterns[p1]);
    p.channels[0].enabled = true;
    p.channels[1].enabled = true;
    p.note_rows[0] = 2;
    assert(dm_automation_add(&p, (DmAutomation){.pattern = p0,
                                                .type = MIDI_CONTROL,
                                                .channel = 0,
                                                .a = MIDI_CC_MODULATION,
                                                .b = 64,
                                                .beat = 0}));
    assert(dm_automation_add(&p, (DmAutomation){.pattern = p1,
                                                .type = MIDI_CONTROL,
                                                .channel = 0,
                                                .a = MIDI_CC_VOLUME,
                                                .b = 80,
                                                .beat = 0}));
    assert(dm_clip_add(&p, (DmClip){.beat = 0, .pattern = p0, .channel = 0, .row = 0}));
    assert(dm_clip_add(&p, (DmClip){.beat = 2, .pattern = p1, .channel = 0, .row = 1}));
    assert(!dm_generation_changes_sound(&p));
    assert(dm_automation_add(&p, (DmAutomation){.pattern = p0,
                                                .type = MIDI_CONTROL,
                                                .channel = 0,
                                                .a = MIDI_CC_VOLUME,
                                                .b = 100,
                                                .beat = 1}));
    assert(dm_generation_changes_sound(&p));
    p.clips[1].beat = 4;
    assert(!dm_generation_changes_sound(&p));
    p.clips[1].beat = 2;
    p.clips[1].channel = 1;
    p.clips[1].row = 0;
    assert(!dm_generation_changes_sound(&p));
    dm_project_free(&p);

    puts("PASS generation warning detection");
}

static int pattern_named(const DmProject *p, const char *name) {
    for (size_t i = 0; i < p->pattern_count; ++i) {
        if (!strcmp(p->patterns[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

/* The pattern's notes and event points are the same in both projects. */
static void same_pattern(const DmProject *s, int x, const DmProject *r, int y) {
    const DmPattern *a = &s->patterns[x], *b = &r->patterns[y];
    assert(!strcmp(a->name, b->name) && a->kind == b->kind && a->count == b->count &&
           a->length == b->length);

    for (size_t i = 0; i < a->count; ++i) {
        DmNote m = a->notes[i], n = b->notes[i];
        assert(m.beat == n.beat && m.length == n.length && m.key == n.key &&
               m.velocity == n.velocity && m.port == n.port);
    }

    size_t j = 0;

    for (size_t i = 0; i < s->automation_count; ++i) {
        DmAutomation m = s->automation[i];

        if (m.pattern != x) {
            continue;
        }

        while (j < r->automation_count && r->automation[j].pattern != y) {
            j++;
        }

        assert(j < r->automation_count);
        DmAutomation n = r->automation[j++];
        assert(m.beat == n.beat && m.type == n.type && m.channel == n.channel && m.a == n.a &&
               m.b == n.b && m.curve == n.curve && m.port == n.port);
    }

    while (j < r->automation_count && r->automation[j].pattern != y) {
        j++;
    }

    assert(j == r->automation_count);
}

/* A channel a single-channel run leaves alone: its placements in order, their patterns' contents
   and points, its subrows and their names. */
static void channel_unchanged(const DmProject *s, const DmProject *r, int channel) {
    size_t j = 0;

    for (size_t i = 0; i < s->clip_count; ++i) {
        DmClip a = s->clips[i];

        if (a.channel != channel) {
            continue;
        }

        while (j < r->clip_count && r->clips[j].channel != channel) {
            j++;
        }

        assert(j < r->clip_count);
        DmClip b = r->clips[j++];
        assert(a.beat == b.beat && a.length == b.length && a.offset == b.offset && a.row == b.row &&
               a.lane == b.lane);
        same_pattern(s, a.pattern, r, b.pattern);
    }

    while (j < r->clip_count && r->clips[j].channel != channel) {
        j++;
    }

    assert(j == r->clip_count);
    assert(s->lane_count[channel] == r->lane_count[channel] &&
           s->note_rows[channel] == r->note_rows[channel] &&
           !memcmp(s->lane_names[channel], r->lane_names[channel], sizeof(s->lane_names[channel])));
}

/* Eight bars of a four-note riff with a volume change and a bend, placed on the channel. */
static int riff(DmProject *p, const char *name, int channel, uint8_t key) {
    int id = dm_pattern_add(p, name);
    assert(id >= 0);

    for (int bar = 0; bar < 8; ++bar) {
        for (int j = 0; j < 4; ++j) {
            assert(dm_note_add(&p->patterns[id],
                               (DmNote){bar * 4.0 + j, .5, (uint8_t)(key + j), 100, 0}));
        }
    }

    dm_pattern_fit(p, &p->patterns[id]);
    p->channels[channel].enabled = true;
    assert(dm_automation_add(p, (DmAutomation){.pattern = id,
                                               .type = MIDI_CONTROL,
                                               .channel = channel,
                                               .a = MIDI_CC_VOLUME,
                                               .b = 90,
                                               .beat = 0}));
    assert(dm_automation_add(p, (DmAutomation){.pattern = id,
                                               .type = MIDI_PITCH_BEND,
                                               .channel = channel,
                                               .a = 0,
                                               .b = 80,
                                               .beat = 12}));
    assert(dm_clip_add(p, (DmClip){.beat = 0, .pattern = id, .channel = channel}));
    return id;
}

static void single_channel_generation(void) {
    DmGenerationScope first = {.channel = 0};
    char error[256];
    DmGenerationStats stats;

    /* Two channels with their own notes and controls; channel 2 also has an automation
       placement on a named subrow. Only channel 1 is regenerated. */
    {
        DmProject source, result;
        dm_project_init(&source);
        dm_project_init(&result);
        riff(&source, "Lead", 0, 60);
        riff(&source, "Bass", 1, 36);
        int lfo = dm_pattern_add(&source, "Bass LFO");
        assert(lfo >= 0);
        source.patterns[lfo].kind = 1;
        source.patterns[lfo].mode = 1;
        source.patterns[lfo].event = MIDI_CC_EXPRESSION;
        source.patterns[lfo].length = 8;
        source.lane_count[1] = 1;
        snprintf(source.lane_names[1][0], 64, "Bass swell");
        assert(dm_clip_add(&source, (DmClip){.pattern = lfo, .channel = 1, .lane = 1}));
        assert(dm_generate_patterns(&source, &result, &first, NULL, &stats, error, sizeof(error)));
        assert(dm_project_validate(&result, error, sizeof(error)));
        same_timeline("single channel", &source, &result);
        channel_unchanged(&source, &result, 1);
        /* Channel 1's source pattern is replaced by generated notes and controls. */
        assert(pattern_named(&result, "Lead") < 0 && stats.note_patterns > 0 &&
               stats.automation_patterns == 2);
        bool generated = false;

        for (size_t i = 0; i < result.clip_count; ++i) {
            DmClip c = result.clips[i];

            if (c.channel == 0 && !result.patterns[c.pattern].kind) {
                generated = true;
                assert(!strncmp(result.patterns[c.pattern].name, "Generated notes", 15));
            }
        }

        assert(generated && result.lane_count[0] == 2);
        dm_project_free(&source);
        dm_project_free(&result);
    }

    /* One pattern placed on channels 1 and 2: regenerating channel 1 keeps it, notes and
       controls, for channel 2's placement. */
    {
        DmProject source, result;
        dm_project_init(&source);
        dm_project_init(&result);
        int shared = riff(&source, "Shared", 0, 60);
        source.channels[1].enabled = true;
        assert(dm_clip_add(&source, (DmClip){.beat = 0, .pattern = shared, .channel = 1}));
        assert(dm_generate_patterns(&source, &result, &first, NULL, &stats, error, sizeof(error)));
        assert(dm_project_validate(&result, error, sizeof(error)));
        same_timeline("shared pattern", &source, &result);
        channel_unchanged(&source, &result, 1);
        int kept = pattern_named(&result, "Shared");
        assert(kept >= 0);
        same_pattern(&source, shared, &result, kept);

        for (size_t i = 0; i < result.clip_count; ++i) {
            assert(result.clips[i].pattern != kept || result.clips[i].channel == 1);
        }

        dm_project_free(&source);
        dm_project_free(&result);
    }

    puts("PASS single-channel generation keeps other channels and shared patterns");
}

static bool lane_named(const DmProject *p, int channel, const char *name) {
    for (int lane = 0; lane < p->lane_count[channel]; ++lane) {
        if (!strcmp(p->lane_names[channel][lane], name)) {
            return true;
        }
    }

    return false;
}

/* Generated automation patterns and subrows are named after the event they carry; a second
   pattern for the same event gets the usual duplicate suffix. */
static void automation_names(void) {
    DmProject source, result;
    dm_project_init(&source);
    dm_project_init(&result);
    int lead = riff(&source, "Lead", 0, 60);
    riff(&source, "Bass", 1, 36);
    assert(dm_automation_add(&source, (DmAutomation){.pattern = lead,
                                                     .type = MIDI_CONTROL,
                                                     .channel = 0,
                                                     .a = MIDI_CC_REVERB,
                                                     .b = 40,
                                                     .beat = 4}));
    char error[256];
    DmGenerationStats stats;
    assert(dm_generate_patterns(&source, &result, NULL, NULL, &stats, error, sizeof(error)));
    same_timeline("automation names", &source, &result);
    char duplicate[64];
    snprintf(duplicate, sizeof(duplicate), "%s-2", DM_TEXT_PITCH_BEND);
    assert(pattern_named(&result, DM_TEXT_PITCH_BEND) >= 0 &&
           pattern_named(&result, duplicate) >= 0 && pattern_named(&result, DM_TEXT_VOLUME) >= 0 &&
           pattern_named(&result, DM_TEXT_REVERB_SEND) >= 0);
    assert(result.lane_count[0] == 3 && lane_named(&result, 0, DM_TEXT_PITCH_BEND) &&
           lane_named(&result, 0, DM_TEXT_VOLUME) && lane_named(&result, 0, DM_TEXT_REVERB_SEND));
    assert(result.lane_count[1] == 2 && lane_named(&result, 1, DM_TEXT_PITCH_BEND) &&
           lane_named(&result, 1, DM_TEXT_VOLUME));
    dm_project_free(&source);
    dm_project_free(&result);
    puts("PASS generated automation is named after its event");
}

int main(void) {
    Editor *e = test_editor_open("Pattern generation", (TestDesktop){.renderer = "software"});
    generation(e);
    imported_generation();
    sliced_generation();
    controls_cross_phrases();
    imported_event_timelines();
    graph_lane_generation();
    precedence_after_generation();
    warning_detection();
    single_channel_generation();
    automation_names();
    test_editor_close(e);
    return 0;
}
