/* Manual MIDI input flood measurement over the dense frame fixture:
   midi_input_performance_test [seconds] [record|bound]. Set DM_MIDI_TEST_DRIVER to pick the audio
   driver (default dummy). record also compares the recorded Graph lane with Point at the widest
   zoom; bound sends the controller flood to eight CCs learned to four mixer strips. */
#include "frame_fixture.h"
#include "midi_input_fixture.h"

void midi_performance_callbacks(DmAudio *audio, bool reset);
void midi_performance_report(DmAudio *audio);

static double editor_time, render_time, present_time;

static void flood_frame(Editor *e) {
    double start = now();
    ui_frame();
    editor_frame(e, 1400, 900);
    editor_time += now() - start;
    SDL_RenderClear(e->renderer);
    start = now();
    ui_render(e->renderer);
    render_time += now() - start;
    start = now();
    SDL_RenderPresent(e->renderer);
    present_time += now() - start;
}

typedef struct {
    MidiFixture *source;
    atomic_bool stop;
    atomic_uint sent;
    bool bound;
} Flood;

enum { BOUND_STRIPS = 4, BOUND_FIRST_CC = 20 };

static int flood(void *context) {
    Flood *f = context;
    uint64_t start = SDL_GetTicksNS();
    unsigned controls = 0, notes = 0;

    while (!atomic_load(&f->stop)) {
        uint64_t ns = SDL_GetTicksNS();
        unsigned due = (unsigned)((ns - start) * 3000 / 1000000000);

        while (controls < due) {
            unsigned control = f->bound ? BOUND_FIRST_CC + controls % (2 * BOUND_STRIPS)
                                        : MIDI_CC_MODULATION;
            unsigned value = f->bound ? controls / (2 * BOUND_STRIPS) % 128 : controls % 128;
            fixture_emit(f->source, 0, MIDI_CONTROL, control, value, SDL_GetTicksNS());
            ++controls;
            atomic_fetch_add(&f->sent, 1);
        }

        unsigned note_due = (unsigned)((ns - start) * 20 / 1000000000);

        while (notes < note_due) {
            fixture_emit(f->source, 0, MIDI_NOTE_ON, 60 + notes % 12, 90, SDL_GetTicksNS());
            fixture_emit(f->source, 0, MIDI_NOTE_OFF, 60 + notes % 12, 64, SDL_GetTicksNS());
            ++notes;
            atomic_fetch_add(&f->sent, 2);
        }

        SDL_Delay(1);
    }

    return 0;
}

int main(int argc, char **argv) {
    double duration = argc > 1 ? atof(argv[1]) : 60;
    bool record = argc > 2 && !strcmp(argv[2], "record");
    bool bound = argc > 2 && !strcmp(argv[2], "bound");
    const char *driver = SDL_getenv("DM_MIDI_TEST_DRIVER");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, driver ? driver : "dummy");
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), "SDL");
    SDL_Window *window = SDL_CreateWindow("MIDI input performance", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "direct3d11");
    check(window && renderer && ui_init(window, renderer, NULL), "renderer");
    char error[512] = {0};
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio != NULL, "audio");
    check(dm_audio_soundfont(audio, "agents/reports/generated-programs.sf2", error, sizeof(error)),
          "font request");

    while (dm_audio_busy(audio)) {
        bool success = false;

        if (dm_audio_poll(audio, error, sizeof(error), &success, NULL)) {
            check(success, "font load");
        }

        SDL_Delay(1);
    }
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, window, renderer, audio, "", "", true);
    fixture(e);
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->roll = true;
    e->automation_open = true;
    e->automation_cc = MIDI_CC_MODULATION;
    double baseline = average_frame(e, 60);
    MidiFixture source;
    ed_midi_input_attach(e, fixture_create(&source));
    atomic_store(&source.delay, 5000);
    double request = now();
    check(dm_midi_input_open(e->midi_input, "Fixture 0"), "request slow input");
    double request_ms = now() - request;
    double slow = average_frame(e, 60);
    fixture_wait(e->midi_input, 0, DM_MIDI_OPEN);
    atomic_store(&source.delay, 0);
    e->overwrite = true;

    if (bound) {
        /* Volume and pan of four strips, as a controller's faders and knobs would be learned. */
        DmBytes list = {0};

        for (unsigned i = 0; i < 2 * BOUND_STRIPS; ++i) {
            EdMidiBinding binding = {
                .source = {.kind = DM_MIDI_SOURCE_CC, .number = (uint8_t)(BOUND_FIRST_CC + i)},
                .target = {i % 2 ? ED_MIDI_PAN : ED_MIDI_VOLUME, (uint8_t)(i / 2), 0}};
            check(ed_midi_binding_append(&list, binding), "binding");
        }

        check(ed_midi_bindings_replace(e, &list), "bindings");
        e->active = true;
        e->input_pickup = false;
    }

    if (record) {
        ed_toggle_record(e);
        ed_play(e);
    }

    midi_performance_callbacks(audio, true);
    Flood stream = {.source = &source, .bound = bound};
    SDL_Thread *producer = SDL_CreateThread(flood, "MIDI flood", &stream);
    check(producer != NULL, "flood thread");
    double begin = now(), total = 0, maximum = 0;
    unsigned frames = 0, slow_frames = 0;

    while (now() - begin < duration * 1000) {
        double frame_start = now();
        flood_frame(e);
        double elapsed = now() - frame_start;
        total += elapsed;
        maximum = fmax(maximum, elapsed);
        slow_frames += elapsed > 20;
        ++frames;

        if (elapsed < 16) {
            SDL_Delay((uint32_t)(16 - elapsed));
        }
    }

    atomic_store(&stream.stop, true);
    SDL_WaitThread(producer, NULL);
    ed_midi_input_finish(e, false);
    DmMidiInputStats stats;
    dm_midi_input_stats(e->midi_input, &stats);
    printf(
        "record=%d duration_s=%.3f baseline_frame_ms=%.3f slow_driver_frame_ms=%.3f "
        "open_request_ms=%.3f frames=%u flood_frame_ms=%.3f max_frame_ms=%.3f frames_over_20ms=%u "
        "sent=%u received=%llu dropped=%llu automation=%zu undo=%d\n",
        record, (now() - begin) / 1000, baseline, slow, request_ms, frames, total / frames, maximum,
        slow_frames, atomic_load(&stream.sent), (unsigned long long)stats.received,
        (unsigned long long)stats.dropped, e->project.automation_count, e->undo_count);
    midi_performance_report(audio);
    printf("editor_ms=%.3f render_ms=%.3f present_ms=%.3f\n", editor_time, render_time,
           present_time);
    size_t visible_points = 0;
    double first_control = -1, last_control = -1;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.type == MIDI_CONTROL && a.a == MIDI_CC_MODULATION && a.pattern == 0) {
            if (first_control < 0) {
                first_control = a.beat;
            }

            last_control = a.beat;
            double x = a.beat * 80 * e->roll_view.zoom - e->roll_view.scroll_x;
            visible_points += x >= 0 && x <= 1000;
        }
    }

    printf(
        "scroll_x=%.1f zoom=%.2f first_control=%.3f last_control=%.3f visible_points=%zu all=%d\n",
        e->roll_view.scroll_x, e->roll_view.zoom, first_control, last_control, visible_points,
        e->automation_all);
    printf("timing_fallbacks=%llu\n", (unsigned long long)stats.timing_fallbacks);

    if (bound) {
        int moved = 0;

        for (int c = 0; c < BOUND_STRIPS; ++c) {
            DmChannel channel = e->project.channels[c];
            printf("strip=%d volume=%d pan=%d\n", c, channel.volume, channel.pan);
            moved += channel.volume != 100 || channel.pan != 64;
        }

        check(moved == BOUND_STRIPS, "bound controls moved every strip");
    }

    if (record) {
        /* The take records Modulation into Graph; compare the same lane drawn as Point. */
        DmPattern *pattern = &e->project.patterns[0];
        check(pattern->graph_lanes[MIDI_CC_MODULATION] > DM_GRAPH_POINT, "take recorded Graph");
        e->roll_view.zoom = .15f;
        e->roll_view.scroll_x = 0;
        double graph = average_frame(e, 240);
        pattern->graph_lanes[MIDI_CC_MODULATION] = DM_GRAPH_POINT;
        double point = average_frame(e, 240);
        printf("zoom_out_graph_frame_ms=%.3f zoom_out_point_frame_ms=%.3f ratio=%.3f\n", graph,
               point, graph / point);
    }
    check(!record || last_control > duration, "recorded positions span the whole take");
    check(stats.dropped == 0, "flood retained every input event");
    check(request_ms < 100, "slow open is asynchronous");
    editor_free(e);
    free(e);
    fixture_free(&source);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
