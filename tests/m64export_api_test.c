#include "m64export/m64export.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* Dense, distinct volume ramps on every channel pass 64 KiB; they are fitted by simplifying the
   curves (warning 64), never by the note reduction of warning 54. */
static void dense_curves(const M64Api *api) {
    enum { STEPS = 3000 };

    M64Event *events = calloc(15 * (STEPS + 2), sizeof(*events));
    size_t n = 0;
    assert(events);

    for (unsigned ch = 0; ch < 16; ch++) {
        if (ch != 9) {
            events[n] = (M64Event){0, n + 1, 0x90 | ch, 48 + ch, 100};
            n++;
        }
    }

    for (unsigned step = 0; step < STEPS; step++) {
        for (unsigned ch = 0; ch < 16; ch++) {
            unsigned phase = step + ch * 37;

            if (ch != 9) {
                unsigned value = phase % 254 < 127 ? phase % 127 : 127 - phase % 127;
                events[n] = (M64Event){step, n + 1, 0xb0 | ch, 7, value};
                n++;
            }
        }
    }

    for (unsigned ch = 0; ch < 16; ch++) {
        if (ch != 9) {
            events[n] = (M64Event){STEPS, n + 1, 0x80 | ch, 48 + ch, 0};
            n++;
        }
    }

    M64Request q = {0};
    q.events = events;
    q.event_count = n;
    q.duration = STEPS;
    q.ppq = 48;
    q.target = M64_TARGET_SM64_US;
    q.voice_limit = 16;
    q.sequence_name = "03_custom_test";
    q.bank_name = "26_custom_test";
    M64Result *r = NULL;
    assert(api->compile(&q, NULL, &r) == M64_OK);
    bool curves = false, notes = false;

    for (uint64_t i = 0; i < api->diagnostic_count(r); i++) {
        M64DiagnosticView d;
        api->diagnostic_at(r, i, &d);
        curves |= d.code == 64;
        notes |= d.code == 54;
    }

    assert(curves && !notes);
    api->result_free(r);
    free(events);
    puts("PASS dense curves fit without note reduction");
}

/* Requests list events in tick order; ties keep their source order. */
static int by_tick(const void *left, const void *right) {
    const M64Event *a = left, *b = right;
    return a->tick != b->tick ? (a->tick < b->tick ? -1 : 1)
                              : (a->source_id > b->source_id) - (a->source_id < b->source_id);
}

static bool reported(const M64Api *api, M64Result *r, uint32_t code) {
    for (uint64_t i = 0; i < api->diagnostic_count(r); i++) {
        M64DiagnosticView d;
        api->diagnostic_at(r, i, &d);

        if (d.code == code) {
            return true;
        }
    }

    return false;
}

/* Notes are shortened (warning 23) only when a source channel's layers and every spare SM64
   channel are busy. Eight voices, an eight-note chord on one channel and back-to-back legato
   notes fit without cutting; sixteen busy channels with five-note chords do not. */
static void voice_capacity(const M64Api *api) {
    M64Event events[96];
    size_t n = 0;

    for (unsigned k = 0; k < 8; k++) {
        events[n] = (M64Event){k * 6, n + 1, 0x90, 48 + k, 100};
        n++;
        events[n] = (M64Event){192, n + 1, 0x80, 48 + k, 0};
        n++;
    }

    for (unsigned k = 0; k < 8; k++) {
        events[n] = (M64Event){192 + k * 24, n + 1, 0x91, 60 + k, 90};
        n++;
        events[n] = (M64Event){216 + k * 24, n + 1, 0x81, 60 + k, 0};
        n++;
    }

    qsort(events, n, sizeof(*events), by_tick);
    M64Request q = {0};
    q.events = events;
    q.event_count = n;
    q.duration = 384;
    q.ppq = 48;
    q.target = M64_TARGET_SM64_US;
    q.voice_limit = 16;
    q.sequence_name = "03_custom_test";
    q.bank_name = "26_custom_test";
    M64Result *r = NULL;
    assert(api->compile(&q, NULL, &r) == M64_OK);
    assert(reported(api, r, 39) && !reported(api, r, 23) && !reported(api, r, 54));
    api->result_free(r);

    M64Event *full = calloc(16 * 10, sizeof(*full));
    n = 0;
    assert(full);

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned k = 0; k < 5; k++) {
            full[n] = (M64Event){0, n + 1, 0x90 | ch, 40 + k * 3, 100};
            n++;
            full[n] = (M64Event){96, n + 1, 0x80 | ch, 40 + k * 3, 0};
            n++;
        }
    }

    qsort(full, n, sizeof(*full), by_tick);
    q.events = full;
    q.event_count = n;
    q.duration = 96;
    assert(api->compile(&q, NULL, &r) == M64_OK && reported(api, r, 23));
    api->result_free(r);
    free(full);
    puts("PASS notes are shortened only when no layer or channel is free");
}

int main(void) {
    const M64Api *api = m64_get_api_3();
    dense_curves(api);
    voice_capacity(api);
    assert(api && !api->default_program(128) && !api->default_drum(128));

    for (unsigned i = 0; i < 128; i++) {
        assert(api->default_program(i)->sample && api->default_drum(i)->sample);
    }

    M64Event events[] = {{0, 1, 0x90, 60, 100}, {48, 2, 0x80, 60, 0}};
    M64Request q = {0};
    q.events = events;
    q.event_count = 2;
    q.duration = 48;
    q.ppq = 48;
    q.target = M64_TARGET_SM64_US;
    q.voice_limit = 16;
    q.sequence_name = "03_custom_test";
    q.bank_name = "26_custom_test";
    M64Result *r = NULL;
    assert(api->compile(&q, NULL, &r) == M64_OK && api->artifact_count(r) == 4);
    M64ArtifactView a;
    assert(api->artifact_at(r, 4, &a) == M64_INVALID);
    api->result_free(r);
    events[0].a = events[1].a = 127;
    assert(api->analyze(&q, NULL, &r) == M64_OK && api->artifact_count(r) == 0 &&
           api->range_count(r) == 1);
    M64RangeIssue issue;
    assert(api->range_at(r, 0, &issue) == M64_OK && issue.channel == 0 && issue.note_high == 127 &&
           issue.note_count == 1);
    assert(!api->preset_supported(&issue, 0));
    unsigned supported = 0;

    for (unsigned i = 0; i < 128; i++) {
        supported += api->preset_supported(&issue, i) != 0;
    }

    assert(supported && api->range_at(r, 1, &issue) == M64_INVALID);
    api->result_free(r);
    /* A loop region: the intro plays once, then beats 2-5 repeat. The note sounding across beat 2
       is split, and the sequence script jumps back to reinitialize (0xd7) and restart the
       region's channels: vanilla SM64 keeps a hung channel's script stopped on restart. */
    M64Event looped[] = {{0, 1, 0x90, 60, 100}, {48, 2, 0x80, 60, 0},    {72, 3, 0x90, 64, 100},
                         {144, 4, 0x80, 64, 0}, {192, 5, 0x90, 67, 100}, {240, 6, 0x80, 67, 0}};
    M64Request l = q;
    l.events = looped;
    l.event_count = 6;
    l.duration = 288;
    l.loop = 2;
    l.loop_start = 96;
    l.loop_end = 240;
    assert(api->compile(&l, NULL, &r) == M64_OK);
    bool reported = false;

    for (uint64_t i = 0; i < api->diagnostic_count(r); i++) {
        M64DiagnosticView d;
        api->diagnostic_at(r, i, &d);
        reported |= d.code == 61;
    }

    assert(reported && api->artifact_at(r, 0, &a) == M64_OK);
    bool jumps = false;

    for (uint64_t i = 0; i + 2 < a.size && !jumps; i++) {
        size_t target = ((size_t)a.data[i + 1] << 8) | a.data[i + 2];
        jumps = a.data[i] == 0xfb && target > 0 && target + 3 < i && a.data[target] == 0xd7 &&
                a.data[target + 3] == 0x90;
    }

    assert(jumps);
    api->result_free(r);
    l.loop_start = 240;
    assert(api->compile(&l, NULL, &r) == M64_INVALID);
    api->result_free(r);
    puts("PASS loop region");
    q.sequence_name = "../escape";
    assert(api->compile(&q, NULL, &r) == M64_INVALID && api->artifact_count(r) == 0);
    api->result_free(r);
    api->result_free(NULL);
    puts("PASS native static C API");
    return 0;
}
