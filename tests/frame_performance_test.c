/* Editor frame times for a large arrangement. The fixture is generated: 15 channels with about
   47,000 notes, 46,000 controller points and 3,000 pitch bends, similar to a dense orchestral MIDI
   file. Set DUCKMIDI_FRAME_FILE to time another file instead. */
#include "frame_fixture.h"

extern size_t audio_probe_engine_size(void);

/* Process CPU time in milliseconds (user and kernel). */
static double cpu_ms(void) {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    return (kernel.dwLowDateTime + user.dwLowDateTime) / 10000.0;
}

/* The scans the editor repeats every frame or drag frame, in microseconds per call. Printed for
   the record; nothing is asserted. */
static void measure_scans(Editor *e) {
    enum { RUNS = 200 };

    double start = now();
    bool used = false, placed = false;
    int program = 0;

    for (int i = 0; i < RUNS; ++i) {
        used |= ed_event_type_used(e, MIDI_CC_EXPRESSION);
    }

    double type_used = now() - start;
    start = now();

    for (int i = 0; i < RUNS; ++i) {
        placed |= ed_pattern_placed(e);
    }

    double pattern_placed = now() - start;
    start = now();

    for (int i = 0; i < RUNS; ++i) {
        program += ed_placed_program(e);
    }

    double placed_program = now() - start;
    start = now();

    for (int i = 0; i < RUNS; ++i) {
        dm_patterns_fit(&e->project);
    }

    double fit = now() - start;
    printf("scan_us: event_type_used=%.1f pattern_placed=%.1f placed_program=%.1f "
           "patterns_fit=%.1f (%d %d %d)\n",
           type_used * 1000 / RUNS, pattern_placed * 1000 / RUNS, placed_program * 1000 / RUNS,
           fit * 1000 / RUNS, used, placed, program);
}

/* Publishing the arrangement, setting a loop, and the engine's idle CPU use over two seconds. */
static void measure_audio(Editor *e) {
    char error[512] = {0};
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio != NULL, "audio engine");
    double start = now();

    for (int i = 0; i < 10; ++i) {
        check(dm_audio_publish(audio, &e->project, 0, false, error, sizeof(error)), "publish");
    }

    double publish = (now() - start) / 10;
    DmSchedule schedule = {0};
    start = now();
    check(dm_schedule(&e->project, &schedule), "schedule");
    double build = now() - start;
    dm_schedule_free(&schedule);
    start = now();

    for (int i = 0; i < 10; ++i) {
        dm_audio_loop(audio, true, 1, 30);
    }

    double loop = (now() - start) / 10;
    dm_audio_loop(audio, false, 0, 0);
    double cpu = cpu_ms();
    SDL_Delay(2000);
    cpu = cpu_ms() - cpu;
    printf("audio: engine_bytes=%zu publish_ms=%.2f (schedule build %.2f) loop_set_ms=%.2f "
           "idle_cpu_ms_per_2s=%.1f driver=%s\n",
           audio_probe_engine_size(), publish, build, loop, cpu, dm_audio_driver(audio));
    dm_audio_destroy(audio);
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
    e->automation_cc = DM_EVENT_BEND;
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
    measure_scans(e);
    measure_audio(e);

    if (file && *file) {
        /* One parse of the file; combining N ports parses it N times. */
        DmBytes data = {0};
        DmProject parsed;
        char error[256];
        dm_project_init(&parsed);
        check(dm_read(file, &data, error, sizeof(error)), "read the requested file");
        start = now();
        bool imported = dm_import(&parsed, data.data, data.size, 0, 0, error, sizeof(error));
        printf("import_ms=%.2f bytes=%zu ok=%d\n", now() - start, data.size, imported);
        dm_project_free(&parsed);
        dm_bytes_free(&data);
    }
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
