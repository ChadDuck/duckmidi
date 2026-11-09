/* Editor frame times for a large arrangement. The fixture is generated: 15 channels with about
   47,000 notes, 46,000 controller points and 3,000 pitch bends, similar to a dense orchestral MIDI
   file. Set DUCKMIDI_FRAME_FILE to time another file instead. */
#include "frame_fixture.h"

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
