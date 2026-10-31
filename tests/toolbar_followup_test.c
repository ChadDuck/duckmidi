#include "editor_internal.h"
extern void recording_feedback_audio_pause(DmAudio *);
extern void recording_feedback_audio_render(DmAudio *, int);
extern void test_toolbar_followup_ui(Editor *, void (*)(bool, const char *));
static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL toolbar followup: %s\n", text);
        exit(1);
    }
}

static void recording_controls(Editor *e) {
    editor_smoke(e);
    e->pattern_mode = true;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->selected_pattern = e->pattern_channel = 0;
    e->record = e->wait_input = e->wait_armed = false;
    e->piano = true;
    ed_toggle_record(e);
    recording_feedback_audio_render(e->audio, 256);
    check(e->record && !dm_audio_playing(e->audio) && !e->wait_armed,
          "Record enables recording while paused without starting playback");
    ed_action(e, ACT_PLAY);
    recording_feedback_audio_render(e->audio, 256);
    check(dm_audio_playing(e->audio) && !e->wait_armed, "Play starts immediately when Wait is off");
    ed_action(e, ACT_PLAY);
    check(!dm_audio_playing(e->audio), "Play pauses running recording");
    e->wait_input = true;
    ed_action(e, ACT_PLAY);
    recording_feedback_audio_render(e->audio, 256);
    check(e->wait_armed && !dm_audio_playing(e->audio), "Play arms recording when Wait is on");
    ed_action(e, ACT_PLAY);
    check(!e->wait_armed && !e->recording_take && !dm_audio_playing(e->audio),
          "second Play cancels the pending recording take");
    ed_action(e, ACT_PLAY);
    SDL_Event key;
    SDL_zero(key);
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_Z;
    key.key.down = true;
    key.key.windowID = SDL_GetWindowID(e->window);
    editor_event(e, &key);
    recording_feedback_audio_render(e->audio, 4800);
    check(dm_audio_playing(e->audio) && !e->wait_armed && e->take_started,
          "keyboard piano starts the armed recording");
    size_t notes = ed_pattern(e)->count;
    key.type = SDL_EVENT_KEY_UP;
    key.key.down = false;
    editor_event(e, &key);
    check(ed_pattern(e)->count == notes + 1, "armed take records the played key on release");
    ed_action(e, ACT_PLAY);
    ed_action(e, ACT_PLAY);
    check(e->wait_armed, "another Play can re-arm a paused take");
    ed_action(e, ACT_WAIT);
    check(!e->wait_input && !e->wait_armed && !e->recording_take,
          "turning Wait off cancels its pending take without playback");
    ed_toggle_record(e);
    ed_action(e, ACT_PLAY);
    recording_feedback_audio_render(e->audio, 256);
    double position = dm_audio_position(e->audio);
    ed_toggle_record(e);
    recording_feedback_audio_render(e->audio, 256);
    check(e->record && dm_audio_playing(e->audio) && dm_audio_position(e->audio) >= position,
          "enabling Record during playback preserves transport");
    ed_action(e, ACT_PLAY);
    ed_toggle_record(e);
}

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Toolbar followup", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "renderer");
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio && dm_audio_soundfont(audio, "agents/reports/generated-programs.sf2", error,
                                      sizeof(error)),
          "load generated font");
    uint64_t start = SDL_GetTicks();

    while (dm_audio_busy(audio)) {
        bool success = false;

        if (dm_audio_poll(audio, error, sizeof(error), &success, NULL)) {
            check(success, error);
        }

        if (SDL_GetTicks() - start > 30000) {
            check(false, "font timeout");
        }

        SDL_Delay(5);
    }

    recording_feedback_audio_pause(audio);
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, audio, "", true);
    ui_frame();
    ui_root(0, 900);
    recording_controls(e);
    ui_end();
    ui_render(r);
    editor_smoke(e);
    test_toolbar_followup_ui(e, check);
    printf("PASS toolbar followup: %d checks\n", checks);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
