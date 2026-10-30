#include "../src/audio.c"
static int checks;
static void check(bool value, const char *what) {
    ++checks;
    if (!value) {
        fprintf(stderr, "FAIL keyboard loop: %s\n", what);
        exit(1);
    }
}
static unsigned voice_id(Synth *s, int channel, int key, bool held) {
    fluid_voice_t *voices[512] = {0};
    fluid_synth_get_voicelist(s->synth, voices, 512, -1);
    for (int i = 0; i < 512 && voices[i]; ++i) {
        fluid_voice_t *v = voices[i];
        bool active = held ? fluid_voice_is_on(v) : fluid_voice_is_playing(v);
        if (active && fluid_voice_get_channel(v) == channel && fluid_voice_get_key(v) == key) {
            return fluid_voice_get_id(v);
        }
    }
    return UINT32_MAX;
}
static void process(DmAudio *a, int frames) {
    SDL_LockAudioStream(a->stream);
    while (frames > 0) {
        float out[BLOCK * 2];
        int n = frames > BLOCK ? BLOCK : frames;
        run_block(a, out, n);
        frames -= n;
    }
    SDL_UnlockAudioStream(a->stream);
}
static void await_font(DmAudio *a) {
    char text[512];
    uint64_t start = SDL_GetTicks();
    while (dm_audio_busy(a)) {
        bool success = false;
        if (dm_audio_poll(a, text, sizeof(text), &success, NULL)) {
            check(success, "load both SoundFont voice domains");
        }
        if (SDL_GetTicks() - start >= 30000) {
            check(false, "SoundFont timeout");
        }
        SDL_Delay(5);
    }
    check(true, "SoundFont finishes before timeout");
}
static void whole_song(DmAudio *a, DmProject *p) {
    char error[512];
    check(dm_audio_publish(a, p, 1.99, true, error, sizeof(error)), "publish near song end");
    dm_audio_preview(a, 0, 69, 100, true);
    dm_audio_preview(a, 1, 72, 90, true);
    process(a, BLOCK);
    unsigned same = voice_id(&a->keyboard, 0, 69, true);
    unsigned chord = voice_id(&a->keyboard, 1, 72, true);
    check(same != UINT32_MAX && chord != UINT32_MAX, "keyboard chord starts");
    process(a, RATE * 4 + 3000);
    check(voice_id(&a->keyboard, 0, 69, true) == same &&
              voice_id(&a->keyboard, 1, 72, true) == chord,
          "original keyboard voices survive multiple full-song wraps without retriggering");
    check(dm_audio_peak(a, 1) > .001f, "held keyboard-only channel remains audible across wraps");
    int bend = 0;
    fluid_synth_get_pitch_bend(a->keyboard.synth, 0, &bend);
    check(bend == 8192, "song reset and bend automation do not change keyboard pitch");
}
static void region_and_release(DmAudio *a, DmProject *p) {
    char error[512];
    unsigned original = voice_id(&a->keyboard, 0, 69, true);
    check(dm_audio_publish(a, p, .39, true, error, sizeof(error)), "publish near region end");
    dm_audio_loop(a, true, .2, .4);
    process(a, RATE);
    check(voice_id(&a->keyboard, 0, 69, true) == original,
          "keyboard attack remains unchanged across repeated region wraps and voice chasing");
    dm_audio_preview(a, 1, 72, 0, false);
    process(a, BLOCK);
    check(voice_id(&a->keyboard, 1, 72, true) == UINT32_MAX,
          "keyboard note-off still releases its voice after looping");
    process(a, RATE);
    check(voice_id(&a->keyboard, 1, 72, true) == UINT32_MAX,
          "later wraps do not resurrect released keyboard keys");
    dm_audio_loop(a, false, 0, 0);
    check(dm_audio_publish(a, p, 0, true, error, sizeof(error)), "restart scheduled same-key note");
    process(a, BLOCK);
    unsigned song = voice_id(&a->live, 0, 69, true);
    dm_audio_preview(a, 0, 69, 0, false);
    process(a, BLOCK);
    check(song != UINT32_MAX && voice_id(&a->live, 0, 69, true) == song,
          "releasing keyboard key does not release a scheduled note of the same pitch");
}
static void sustain_and_monitor(DmAudio *a, DmProject *p) {
    char error[512];
    dm_audio_control(a, 1, 64, 127);
    dm_audio_preview(a, 1, 76, 100, true);
    process(a, BLOCK);
    unsigned sustained = voice_id(&a->keyboard, 1, 76, true);
    dm_audio_preview(a, 1, 76, 0, false);
    process(a, BLOCK);
    check(dm_audio_publish(a, p, 1.99, true, error, sizeof(error)), "prepare sustained-note wrap");
    process(a, RATE / 2);
    int pedal = 0;
    fluid_synth_get_cc(a->keyboard.synth, 1, 64, &pedal);
    check(sustained != UINT32_MAX && pedal == 127 &&
              voice_id(&a->keyboard, 1, 76, false) == sustained,
          "loop resets preserve keyboard sustain state and sustained voice");
    dm_audio_control(a, 1, 64, 0);
    process(a, RATE * 3);
    check(voice_id(&a->keyboard, 1, 76, false) == UINT32_MAX,
          "pedal release clears sustained keyboard voice normally");
    dm_audio_preview(a, 1, 77, 100, true);
    process(a, BLOCK);
    dm_audio_monitor(a, false);
    float muted[BLOCK * 2];
    SDL_LockAudioStream(a->stream);
    run_block(a, muted, BLOCK);
    SDL_UnlockAudioStream(a->stream);
    bool quiet = true;
    for (int i = 0; i < BLOCK * 2; ++i) {
        quiet &= muted[i] == 0;
    }
    check(quiet, "turning internal monitoring off still silences keyboard voices");
    dm_audio_monitor(a, true);
    process(a, BLOCK * 4);
    check(voice_id(&a->keyboard, 1, 77, true) == UINT32_MAX,
          "reenabling monitoring does not leave stuck keyboard notes");
}
int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi");
    check(SDL_Init(SDL_INIT_AUDIO), "SDL audio");
    char error[512];
    DmAudio *a = dm_audio_create(error, sizeof(error));
    check(a && a->stream, "real audio backend");
    check(dm_audio_soundfont(a, "agents/reports/generated-sine.sf2", error, sizeof(error)),
          "start isolated fixture SoundFont load");
    await_font(a);
    check(SDL_PauseAudioStreamDevice(a->stream), "pause device for deterministic rendering");
    DmProject p;
    dm_project_init(&p);
    p.channels[1].enabled = true;
    int id = dm_pattern_add(&p, "Loop fixture");
    dm_note_add(&p.patterns[id], (DmNote){0, 2, 69, 100, 0});
    dm_clip_add(&p, (DmClip){0, id, 0, 0, 0, 0});
    dm_automation_add(&p, (DmAutomation){0, 0xe0, 0, 0, 80, id, 0, 0, 0, false});
    whole_song(a, &p);
    region_and_release(a, &p);
    sustain_and_monitor(a, &p);
    printf("PASS keyboard loop: %d checks; held voice IDs unchanged across song/region wraps\n",
           checks);
    dm_project_free(&p);
    dm_audio_destroy(a);
    SDL_Quit();
}
