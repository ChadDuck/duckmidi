/* Dense editor fixture and frame timing shared by the frame and MIDI input performance
   measurements: 15 channels with about 47,000 notes, 46,000 controller points and 3,000 pitch
   bends, similar to a dense orchestral MIDI file. */
#ifndef DM_FRAME_FIXTURE_H
#define DM_FRAME_FIXTURE_H
#include "editor_internal.h"
#include <windows.h>

static void check(bool ok, const char *text) {
    if (!ok) {
        fprintf(stderr, "FAIL frame performance: %s\n", text);
        exit(1);
    }
}

static double now(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart * 1000;
}

static void frame(Editor *e) {
    ui_frame();
    editor_frame(e, 1400, 900);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_RenderPresent(e->renderer);
}

static double average_frame(Editor *e, int count) {
    frame(e);
    double start = now();

    for (int i = 0; i < count; ++i) {
        frame(e);
    }

    return (now() - start) / count;
}

static void fixture(Editor *e) {
    ed_new_project(e);
    e->project.clip_count = 0;
    e->project.pattern_count = 0;

    for (int c = 0; c < 15; ++c) {
        int channel = c >= 9 ? c + 1 : c;
        int id = dm_pattern_add(&e->project, NULL);
        check(id >= 0, "pattern");
        DmPattern *p = &e->project.patterns[id];

        for (int n = 0; n < 3150; ++n) {
            DmNote note = {n * .25, .2, (uint8_t)(40 + (n * 7 + c * 3) % 48), 90, 0, 0};
            check(dm_note_add(p, note), "note");
        }

        for (int a = 0; a < 3070; ++a) {
            DmAutomation point = {a * .25, 0xb0, channel, 11, (a * 5) % 128, id, 0, 0, 0, false, 0};
            check(dm_automation_add(&e->project, point), "automation");
        }

        if (c == 0) {
            /* Dense pitch bends with bend range changes, as in MIDI files with vibrato. Each
               drawn point needs its bend range, which once rescanned all automation per point. */
            int range[][3] = {{0, MIDI_CC_RPN_MSB, 0},
                              {0, MIDI_CC_RPN_LSB, 0},
                              {0, MIDI_CC_DATA, 12},
                              {400, MIDI_CC_DATA, 5}};

            for (int k = 0; k < 4; ++k) {
                DmAutomation point = {.beat = range[k][0],
                                      .type = MIDI_CONTROL,
                                      .channel = channel,
                                      .a = range[k][1],
                                      .b = range[k][2],
                                      .pattern = id};
                check(dm_automation_add(&e->project, point), "bend range");
            }

            for (int a = 0; a < 3000; ++a) {
                int value = MIDI_BEND_CENTER + (a * 211) % 3000 - 1500;
                DmAutomation point = {.beat = a * .25 + .125,
                                      .type = MIDI_PITCH_BEND,
                                      .channel = channel,
                                      .a = value & MIDI_DATA_MASK,
                                      .b = value >> 7,
                                      .pattern = id};
                check(dm_automation_add(&e->project, point), "pitch bend");
            }
        }

        dm_pattern_fit(&e->project, p);
        check(dm_clip_add(&e->project, (DmClip){0, id, channel, 0, 0, 0}), "placement");
        e->project.channels[channel].enabled = true;
        e->project.channels[channel].program = c * 8;
    }

    e->selected_pattern = 0;
    e->pattern_channel = 0;
    ed_publish(e);
}

#endif
