/* Editor frame times for a large arrangement. The fixture is generated: 15 channels with about
   47,000 notes, 46,000 controller points and 3,000 pitch bends, similar to a dense orchestral MIDI
   file. Set DUCKMIDI_FRAME_FILE to time another file instead. */
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

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Frame performance", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", "", true);
    const char *file = SDL_getenv("DUCKMIDI_FRAME_FILE");

    if (file && *file) {
        ed_load(e, file);
        check(e->active, "load the requested file");
    } else {
        fixture(e);
    }

    size_t notes = 0;

    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        notes += e->project.patterns[i].count;
    }

    /* Breakdown: without the pattern editor, and with the pattern cards scrolled away. */
    e->roll = false;
    double playlist_only = average_frame(e, 20);
    float sidebar = e->sidebar;
    e->mixer_height = 2000;
    double no_cards = average_frame(e, 20);
    e->mixer_height = 420;
    e->sidebar = sidebar;
    printf("playlist_only_ms=%.2f small_card_area_ms=%.2f\n", playlist_only, no_cards);

    e->roll = true;
    e->automation_open = false;
    double arrangement = average_frame(e, 20);
    e->automation_open = true;
    e->automation_cc = 11;
    double events = average_frame(e, 20);
    e->automation_cc = -2;
    double bends = average_frame(e, 20);
    size_t compared = 0;

    /* Drawing reads bend ranges from a cache; it must match a full scan for every point. */
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.type != MIDI_PITCH_BEND) {
            continue;
        }

        float plain = ed_automation_pitch(e, a);
        ed_pitch_cache_begin(e);
        ed_automation_pitch(e, e->project.automation[0]);
        float cached = ed_automation_pitch(e, a);
        ed_pitch_cache_end(e);
        check(plain == cached, "cached bend range matches a full scan");
        ++compared;
    }

    check(compared > 0, "pitch bends compared");
    double start = now();
    ed_checkpoint(e);
    ed_commit(e);
    double edit = now() - start;
    printf("notes=%zu automation=%zu frame_ms=%.2f events_frame_ms=%.2f bends_frame_ms=%.2f "
           "edit_ms=%.2f\n",
           notes, e->project.automation_count, arrangement, events, bends, edit);
    check(arrangement < 20 && events < 20 && bends < 20, "frames stay under 20 ms");
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
