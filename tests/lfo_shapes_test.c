#include "editor_internal.h"
static int checks;

static void check(bool ok, const char *what) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL lfo_shapes: %s\n", what);
        exit(1);
    }
}

extern void test_lfo_shapes_ui(Editor *, void (*)(bool, const char *));

static void wave_shapes(void) {
    DmProject p;
    dm_project_init(&p);
    int id = dm_pattern_add(&p, "Shapes");
    DmPattern *lfo = &p.patterns[id];
    lfo->kind = 1;
    lfo->mode = 1;
    lfo->amount = 1;
    check(fabs(dm_lfo(lfo, -.25)) < 1e-9 && fabs(dm_lfo(lfo, -.125) - .5) < 1e-9 &&
              fabs(dm_lfo(lfo, 0) - 1) < 1e-9 && fabs(dm_lfo(lfo, .5) + 1) < 1e-9,
          "neutral tension makes a phase-aligned triangle");
    lfo->tension = -1;
    check(fabs(dm_lfo(lfo, -.125)) < .001 && fabs(dm_lfo(lfo, 0) - 1) < 1e-9 &&
              fabs(dm_lfo(lfo, .5) + 1) < 1e-9,
          "low tension pinches the wave into bipolar spikes");
    lfo->tension = 1;
    check(dm_lfo(lfo, -.125) > .999 && dm_lfo(lfo, .375) < -.999,
          "high tension creates rounded square plateaus");
    lfo->tension = 0;
    lfo->skew = -1;
    check(dm_lfo(lfo, 0) > dm_lfo(lfo, .2) && dm_lfo(lfo, .2) > dm_lfo(lfo, .4),
          "negative skew creates descending sawtooth");
    lfo->skew = 1;
    check(dm_lfo(lfo, 0) < dm_lfo(lfo, .2) && dm_lfo(lfo, .2) < dm_lfo(lfo, .4),
          "positive skew creates ascending sawtooth");
    lfo->skew = 0;
    lfo->pulse = .05;
    check(dm_lfo(lfo, -.125) == -1 && fabs(dm_lfo(lfo, 0) - 1) < 1e-9 && dm_lfo(lfo, .25) == -1,
          "narrow pulse width keeps a low baseline and narrow peaks");
    lfo->pulse = .95;
    check(dm_lfo(lfo, -.125) == 1 && fabs(dm_lfo(lfo, .5) + 1) < 1e-9 && dm_lfo(lfo, .25) == 1,
          "wide pulse width keeps a high plateau and narrow troughs");
    lfo->pulse = .5;
    lfo->amount = .4;
    double normal = dm_lfo(lfo, -.125);
    lfo->inverted = true;
    check(fabs(normal + dm_lfo(lfo, -.125)) < 1e-9, "inversion reflects waveform around center");
    check(dm_lfo_speed_snap(.98, 4) == 1 && dm_lfo_speed_snap(.507, 4) == .5 &&
              fabs(dm_lfo_speed_snap(.67, 4) - 2.0 / 3) < 1e-9,
          "speed snaps near straight and triplet beat divisions");
    check(dm_lfo_speed_snap(1.08, 4) == 1.08, "speed stays continuous away from sync points");
    check(dm_lfo_center_snap(64.4, false, false) == 60 &&
              dm_lfo_center_snap(66, false, false) == 70,
          "controller center snaps in tens");
    check(dm_lfo_center_snap(64.4, false, true) == 64 &&
              dm_lfo_center_snap(64.8, false, true) == 65,
          "Alt controller center uses full integers");
    check(dm_lfo_center_snap(127, false, false) == 127 && dm_lfo_center_snap(-5, false, true) == 0,
          "controller endpoints stay reachable and bounded");
    check(dm_lfo_center_snap(60.6, true, false) == 61 &&
              dm_lfo_center_snap(60.6, true, true) == 60.6,
          "pitch center snaps to notes or moves freely with Alt");
    lfo->event = 11;
    lfo->center = 20;
    lfo->amount = 0;
    check(dm_lfo_value(lfo, .2) == 20, "controller offset reaches MIDI generation");
    lfo->event = -2;
    lfo->reference = 60;
    lfo->pitch_center = 72.25;
    lfo->inverted = false;
    int value = dm_lfo_value(lfo, .2), range = dm_lfo_range(lfo);
    check(range >= 13 && fabs(60 + (value - 8192) * range / 8192.0 - 72.25) < .01,
          "pitch center uses sufficient bend range without changing its reference");
    lfo->amount = .5;
    double delta = 60 + (dm_lfo_value(lfo, 0) - 8192) * dm_lfo_range(lfo) / 8192.0 - 72.25;
    check(fabs(delta - 1) < .02, "Amount retains its two-semitone full-scale depth");
    DmBytes packed = {0};
    DmProject next;
    dm_project_init(&next);
    char error[256];
    lfo->inverted = true;
    check(dm_pack(&p, &packed) && dm_unpack(&next, packed.data, packed.size, error, sizeof(error)),
          "current schema roundtrip");
    check(next.patterns[0].center == 20 && next.patterns[0].pitch_center == 72.25 &&
              next.patterns[0].inverted && next.patterns[0].reference == 60,
          "center, reference and inversion persist");
    packed.data[8] = 3;
    check(!dm_unpack(&next, packed.data, packed.size, error, sizeof(error)),
          "previous schema is rejected");
    dm_bytes_free(&packed);
    dm_project_free(&next);
    dm_project_free(&p);
}

static void midi_centers(void) {
    DmProject p, imported;
    dm_project_init(&p);
    dm_project_init(&imported);
    p.channels[0].program = 0;
    int id = dm_pattern_add(&p, "Centered LFO");
    DmPattern *lfo = &p.patterns[id];
    lfo->kind = lfo->mode = 1;
    lfo->event = 11;
    lfo->center = 30;
    lfo->amount = .2;
    lfo->inverted = true;
    dm_clip_add(&p, (DmClip){0, id, 0, 0, 2, 1});
    DmBytes midi = {0};
    char error[256];
    check(dm_midi(&p, &midi, error, sizeof(error)) &&
              dm_import(&imported, midi.data, midi.size, 0, 0, error, sizeof(error)),
          "centered inverted LFO exports and reimports as MIDI");
    DmSchedule schedule = {0};
    check(dm_schedule(&imported, &schedule), "exported LFO schedules");
    int maximum = 0, minimum = 127;

    for (size_t i = 0; i < schedule.count; ++i) {
        DmScheduled *s = &schedule.items[i];

        if (s->status == 0xb0 && s->a == 11 && s->seconds > .01 && s->seconds < .9) {
            minimum = s->b < minimum ? s->b : minimum;
            maximum = s->b > maximum ? s->b : maximum;
        }
    }

    check(minimum >= 17 && maximum <= 43 && maximum - minimum >= 24,
          "exported controller waveform retains center and amplitude");
    dm_schedule_free(&schedule);
    dm_bytes_free(&midi);
    dm_project_free(&imported);
    dm_project_free(&p);
}

static void editor_units(Editor *e) {
    editor_smoke(e);
    int id = dm_pattern_add(&e->project, "LFO");
    DmPattern *p = &e->project.patterns[id];
    p->kind = 1;
    p->mode = 1;
    p->event = 11;
    ed_open_pattern(e, id, false);
    e->context = 2;
    e->roll_view.zoom = 1;
    ed_action_zoom(e, ACT_ZOOM_IN);
    check(e->roll_view.zoom == 1, "LFO keyboard zoom is disabled");
    e->project.clips[0].selected = 1;
    e->project.clips[2].selected = 1;
    e->erase_active = false;
    size_t count = e->project.clip_count;
    int history = e->undo_count;
    ed_playlist_remove(e, 0);
    ed_playlist_remove(e, 0);
    check(e->project.clip_count == count - 2 && e->undo_count == history + 1,
          "continuous placement erasure uses one history transaction");
    ed_undo(e, false);
    check(e->project.clip_count == count, "one undo restores erased placements");
    editor_smoke(e);
    id = dm_pattern_add(&e->project, "LFO");
    p = &e->project.patterns[id];
    p->kind = 1;
    p->mode = 1;
    p->event = 11;
    e->project.lane_count[0] = 1;
    dm_clip_add(&e->project, (DmClip){0, id, 0, 0, 8, 1});
    ed_open_pattern(e, id, false);
    e->roll_height = 420;
    e->selected_clip = (int)e->project.clip_count - 1;
    e->piano = false;
    e->automation_raw = false;
}

int main(void) {
    wave_shapes();
    midi_centers();
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("lfo_shapes", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", true);
    editor_units(e);
    test_lfo_shapes_ui(e, check);
    printf("PASS lfo_shapes: %d focused checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
