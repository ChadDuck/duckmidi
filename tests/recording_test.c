#include "editor/editor.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TEST_NAME "recording"
#include "test_common.h"
void test_editor_stop(Editor *e);

static void key(Editor *e, SDL_Scancode scancode, bool down) {
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.mod = SDL_KMOD_NONE;
    event.key.repeat = false;
    event.key.down = down;
    editor_event(e, &event);
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi");
    check(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO), "initialize SDL");
    SDL_Window *window = SDL_CreateWindow("Recording validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "direct3d11");
    check(window && renderer && ui_init(window, renderer, NULL), "initialize actual UI");
    char error[512] = {0};
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio != NULL, error);
    check(!strcmp(dm_audio_driver(audio), "wasapi"), "WASAPI stream is available");
    check(dm_audio_soundfont(audio, "agents/reports/generated-sine.sf2", error, sizeof(error)),
          "start font");
    uint64_t limit = SDL_GetTicks() + 30000;

    while (dm_audio_busy(audio)) {
        bool success = false;

        if (dm_audio_poll(audio, error, sizeof(error), &success, NULL)) {
            check(success, error);
        }

        check(SDL_GetTicks() < limit, "worker timeout");
        SDL_Delay(5);
    }

    Editor *e = calloc(1, sizeof(*e));
    check(e != NULL, "allocate editor");
    editor_init(e, window, renderer, audio, "", "", true);
    editor_smoke(e);
    e->edit_focus = FOCUS_NOTES;
    e->pattern_mode = true;
    e->record = true;
    e->piano = true;
    dm_audio_recording(audio, true);
    e->project.patterns[0].count = 0;
    size_t before = e->project.patterns[0].count;
    key(e, SDL_SCANCODE_SPACE, true);
    uint64_t transport_limit = SDL_GetTicks() + 3000;

    while (dm_audio_position(audio) < .15) {
        check(SDL_GetTicks() < transport_limit, "transport startup timeout");
        SDL_Delay(5);
    }

    SDL_Delay(23);
    key(e, SDL_SCANCODE_Q, true);
    SDL_Delay(83);
    key(e, SDL_SCANCODE_Q, false);
    check(e->project.patterns[0].count == before + 1, "one recorded note per physical press");
    DmNote note = e->project.patterns[0].notes[before];
    check(note.length > .08 && note.length < .24,
          "recorded duration is retained without quarter-beat rounding");
    check(fabs(note.length / .25 - round(note.length / .25)) > .02,
          "recording is not automatically quantized");
    check(dm_audio_playing(audio), "record commit leaves transport running");
    key(e, SDL_SCANCODE_Q, true);
    SDL_Delay(43);
    key(e, SDL_SCANCODE_HOME, true);
    key(e, SDL_SCANCODE_Q, false);
    check(e->project.patterns[0].count == before + 2, "seeking finalizes a held recording once");
    check(e->project.patterns[0].notes[before + 1].length > .01,
          "seeking retains pre-jump note timing");
    e->start_beat[1] = .5;
    key(e, SDL_SCANCODE_SPACE, true);
    check(!dm_audio_playing(audio) && fabs(dm_audio_position(audio) - .25) < .005 &&
              fabs(e->cursor_beat[1] - .5) < .0001,
          "pause immediately restores the remembered pattern start");
    SDL_Event history;
    SDL_zero(history);
    history.type = SDL_EVENT_KEY_DOWN;
    history.key.scancode = SDL_SCANCODE_Z;
    history.key.mod = SDL_KMOD_CTRL;
    editor_event(e, &history);
    check(e->project.patterns[0].count == before,
          "one undo removes the whole recording take, including pre-seek notes");
    history.key.scancode = SDL_SCANCODE_Y;
    editor_event(e, &history);
    check(e->project.patterns[0].count == before + 2,
          "one redo restores the entire recording take");
    key(e, SDL_SCANCODE_SPACE, true);
    key(e, SDL_SCANCODE_W, true);
    SDL_Delay(40);
    key(e, SDL_SCANCODE_W, false);
    test_editor_stop(e);
    check(!e->record && !e->recording_take && !dm_audio_playing(audio),
          "Stop ends the recording take and transport");
    history.key.scancode = SDL_SCANCODE_Z;
    editor_event(e, &history);
    check(e->project.patterns[0].count == before + 2,
          "a later recording take has its own undo boundary after Stop");
    e->piano = false;
    e->record = false;
    e->pattern_mode = true;
    e->start_beat[1] = 0;
    e->cursor_beat[1] = 1;
    DmPatternPreview ordinary = {0, 0, -1, false, 0, e->project.patterns[0].length};
    check(dm_audio_publish_pattern(audio, &e->project, &ordinary, .5, true, error, sizeof(error)),
          "start ordinary pattern playback away from set position");
    SDL_Delay(90);

    key(e, SDL_SCANCODE_SPACE, true);
    check(!dm_audio_playing(audio) && fabs(dm_audio_position(audio)) < .005 &&
              e->start_beat[1] == 0,
          "editor pause returns to the set position");
    double resume = dm_audio_position(audio);
    key(e, SDL_SCANCODE_SPACE, true);
    SDL_Delay(70);
    check(dm_audio_playing(audio) && dm_audio_position(audio) > resume + .02,
          "editor play resumes from the set position");
    key(e, SDL_SCANCODE_SPACE, true);
    key(e, SDL_SCANCODE_HOME, true);
    check(e->start_beat[1] == 0 && e->cursor_beat[1] == 0, "Home updates set position");
    key(e, SDL_SCANCODE_RIGHT, true);
    check(e->start_beat[1] > 0 && e->start_beat[1] == e->cursor_beat[1],
          "arrow seek updates set position");
    /* Song playback into a pattern that is not on the playlist: the take starts at the
       pattern's beginning, keeping the beat within the bar where playing began. */
    int unplaced = dm_pattern_add(&e->project, "Unplaced take");
    e->selected_pattern = unplaced;
    e->edit_focus = FOCUS_NOTES;
    e->pattern_mode = false;
    e->record = true;
    e->piano = true;
    double bar = dm_bar(&e->project), song_start = bar + 1.5;
    e->start_beat[0] = e->cursor_beat[0] = song_start;
    key(e, SDL_SCANCODE_SPACE, true);
    transport_limit = SDL_GetTicks() + 3000;

    while (!dm_audio_playing(audio) ||
           dm_seconds_beat(&e->project, dm_audio_position(audio)) < song_start + .1) {
        check(SDL_GetTicks() < transport_limit, "song recording startup timeout");
        SDL_Delay(5);
    }

    key(e, SDL_SCANCODE_Q, true);
    SDL_Delay(60);
    key(e, SDL_SCANCODE_Q, false);
    SDL_Delay(200);
    key(e, SDL_SCANCODE_W, true);
    SDL_Delay(60);
    key(e, SDL_SCANCODE_W, false);
    key(e, SDL_SCANCODE_SPACE, true);
    DmPattern *take = &e->project.patterns[unplaced];
    check(take->count == 2 && take->notes[0].beat > 1.5 && take->notes[0].beat < 2.5,
          "an unplaced pattern records from its start, offset within the bar");
    check(take->notes[1].beat > take->notes[0].beat + .2,
          "later notes of the take keep their timing after the first");
    e->record = false;
    e->piano = false;
    printf("PASS recording: one note, beat %.6f, length %.6f; live transport preserved\n",
           note.beat, note.length);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
