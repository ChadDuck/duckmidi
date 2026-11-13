/* Generate Patterns: the preview leaves the project alone, repeated phrases become linked
   placements, controller values and note timelines survive, and Apply is one undo step. */
#include "editor/internal.h"
#include "../shared/pattern_generation.h"
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
        assert(dm_generate_patterns(&source, &result, NULL, &stats, error, sizeof(error)));
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
    assert(dm_generate_patterns(&source, &result, NULL, &stats, error, sizeof(error)));
    size_t notes = same_song("sliced", &source, &result);
    /* Each note is an on and an off event: six whole bars and the slice's three. */
    assert(notes == 2 * (4 * 6 + 4 * 3) && stats.linked_placements > 0);
    printf("PASS generation reads slices and rows (%zu notes, %zu patterns, %zu linked)\n", notes,
           stats.note_patterns, stats.linked_placements);
    dm_project_free(&source);
    dm_project_free(&result);
}

int main(void) {
    Editor *e = test_editor_open("Pattern generation", (TestDesktop){.renderer = "software"});
    generation(e);
    imported_generation();
    sliced_generation();
    test_editor_close(e);
    return 0;
}
