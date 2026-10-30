#include "../src/audio.c"
static int checks;
static void check(bool value, const char *what) {
    ++checks;
    if (!value) {
        fprintf(stderr, "FAIL live edit: %s\n", what);
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

static void update(DmAudio *a, DmProject *p) {
    char error[512];
    check(dm_audio_update(a, p, -1, 0, -1, 0, error, sizeof(error)), "live schedule update");
}
static void continuity(DmAudio *a, DmProject *p) {
    char error[512];
    DmPattern *pat = &p->patterns[0];
    dm_note_add(pat, (DmNote){0, 2, 60, 100, 0});
    dm_note_add(pat, (DmNote){0, 4, 64, 100, 0});
    dm_note_add(pat, (DmNote){4, 1, 67, 100, 0});
    pat->length = 8;
    check(dm_audio_publish(a, p, 0, true, error, sizeof(error)), "start scheduled chord");
    process(a, RATE / 4);
    unsigned first = voice_id(&a->live, 0, 60, true), second = voice_id(&a->live, 0, 64, true);
    check(first != UINT32_MAX && second != UINT32_MAX, "two actual song voices start");
    pat->count = 0;
    dm_note_add(pat, (DmNote){1.5, .25, 69, 100, 0});
    update(a, p);
    check(fabs(a->position - .25) < 1.0 / RATE, "edit keeps precise audio position");
    process(a, RATE / 10);
    check(voice_id(&a->live, 0, 60, true) == first && voice_id(&a->live, 0, 64, true) == second,
          "deleting sounding notes preserves original voice IDs");
    pat->notes[0].key = 71;
    update(a, p);
    update(a, p);
    process(a, (int)(RATE * .45));
    check(voice_id(&a->live, 0, 71, true) != UINT32_MAX &&
              voice_id(&a->live, 0, 69, true) == UINT32_MAX,
          "future edited pitch plays, superseded pitch never plays");
    check(voice_id(&a->live, 0, 60, true) == first && voice_id(&a->live, 0, 64, true) == second,
          "unrelated new note leaves both original voices intact");
    process(a, RATE / 4);
    check(voice_id(&a->live, 0, 60, true) == UINT32_MAX &&
              voice_id(&a->live, 0, 64, true) == second,
          "first original deadline releases only its individual note");
    check(voice_id(&a->live, 0, 71, true) == UINT32_MAX, "new note releases at its new deadline");
    p->clip_count = 0;
    update(a, p);
    process(a, RATE / 2);
    check(voice_id(&a->live, 0, 64, true) == second && a->position > 1.5,
          "removing final placement does not wrap early or silence the remaining voice");
    process(a, RATE / 2);
    check(voice_id(&a->live, 0, 64, true) == UINT32_MAX,
          "deleted placement's final note releases at original deadline");
}
static void replacement_and_pause(DmAudio *a, DmProject *p) {
    char error[512];
    DmPattern *pat = &p->patterns[0];
    pat->count = 0;
    dm_note_add(pat, (DmNote){0, 2, 60, 100, 0});
    dm_note_add(pat, (DmNote){1.5, 2, 60, 100, 0});
    pat->length = 8;
    dm_clip_add(p, (DmClip){0, 0, 0, 0, 0, 0});
    check(dm_audio_publish(a, p, 0, true, error, sizeof(error)), "same-pitch fixture");
    process(a, RATE / 4);
    update(a, p);
    process(a, (int)(RATE * .55));
    unsigned replacement = voice_id(&a->live, 0, 60, true);
    process(a, RATE / 4);
    check(replacement != UINT32_MAX && voice_id(&a->live, 0, 60, true) == replacement,
          "old same-key deadline cannot release a newer attack");
    dm_audio_transport(a, false);
    process(a, BLOCK);
    check(a->release_count == 0 && voice_id(&a->live, 0, 60, true) == UINT32_MAX,
          "pause explicitly stops song voices and pending releases");
}
static void isolated_feedback(DmAudio *a, DmProject *p) {
    char error[512];
    check(dm_audio_publish(a, p, 0, true, error, sizeof(error)), "start isolation fixture");
    dm_audio_preview(a, 0, 60, 100, true);
    process(a, BLOCK);
    unsigned song = voice_id(&a->live, 0, 60, true);
    unsigned keyboard = voice_id(&a->keyboard, 0, 60, true);
    dm_audio_edit_preview(a, 0, 60.5f, true);
    process(a, BLOCK);
    check(voice_id(&a->feedback, 0, 60, true) != UINT32_MAX,
          "editor has an independent same-key voice");
    int bend = 0;
    fluid_synth_get_pitch_bend(a->feedback.synth, 0, &bend);
    check(bend == 10240, "fractional pitch-bend feedback has half-semitone pitch");
    dm_audio_edit_preview(a, 0, 64, true);
    process(a, BLOCK);
    check(voice_id(&a->feedback, 0, 64, true) != UINT32_MAX, "pitch drag changes feedback note");
    check(voice_id(&a->live, 0, 60, true) == song &&
              voice_id(&a->keyboard, 0, 60, true) == keyboard,
          "drag feedback does not retrigger song or keyboard voices");
    dm_audio_edit_preview(a, 0, 0, false);
    process(a, BLOCK);
    check(voice_id(&a->feedback, 0, 64, true) == UINT32_MAX &&
              voice_id(&a->keyboard, 0, 60, true) == keyboard,
          "ending drag releases only editor feedback");
}

static void preparation_benchmark(DmAudio *a) {
    const int counts[] = {1000, 10000, 100000};
    for (size_t n = 0; n < sizeof(counts) / sizeof(counts[0]); ++n) {
        DmProject p;
        dm_project_init(&p);
        p.channels[0].program = 0;
        dm_pattern_add(&p, "Generated preparation benchmark");
        bool notes_ok = true;
        for (int i = 0; i < counts[n]; ++i) {
            notes_ok &= dm_note_add(&p.patterns[0], (DmNote){i * .25, .125, 48 + i % 24, 100, 0});
        }
        check(notes_ok, "generated benchmark notes");
        dm_clip_add(&p, (DmClip){0, 0, 0, 0, 0, 0});
        uint64_t start = SDL_GetPerformanceCounter();
        DmSchedule schedule = {0};
        check(dm_schedule(&p, &schedule), "benchmark schedule");
        double elapsed =
            (double)(SDL_GetPerformanceCounter() - start) * 1000 / SDL_GetPerformanceFrequency();
        size_t events = schedule.count;
        size_t bytes = events * sizeof(DmScheduled) + schedule.beat_count * sizeof(double);
        dm_schedule_free(&schedule);
        start = SDL_GetPerformanceCounter();
        update(a, &p);
        double edit_ms =
            (double)(SDL_GetPerformanceCounter() - start) * 1000 / SDL_GetPerformanceFrequency();
        printf("PREP notes=%d events=%zu schedule_ms=%.3f edit_ms=%.3f schedule_MiB=%.3f\n",
               counts[n], events, elapsed, edit_ms, (double)bytes / (1024 * 1024));
        dm_project_free(&p);
    }
}
int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi");
    check(SDL_Init(SDL_INIT_AUDIO), "SDL audio");
    char error[512];
    DmAudio *a = dm_audio_create(error, sizeof(error));
    check(a && a->stream, "real audio backend");
    check(dm_audio_soundfont(a, "agents/reports/generated-sine.sf2", error, sizeof(error)),
          "fixture SoundFont");
    await_font(a);
    check(SDL_PauseAudioStreamDevice(a->stream), "deterministic rendering");
    DmProject p;
    dm_project_init(&p);
    p.channels[0].program = 0;
    dm_pattern_add(&p, "Live edits");
    dm_clip_add(&p, (DmClip){0, 0, 0, 0, 0, 0});
    continuity(a, &p);
    replacement_and_pause(a, &p);
    isolated_feedback(a, &p);
    dm_audio_edit_preview(a, 0, 72, true);
    uint64_t mix_start = SDL_GetPerformanceCounter();
    process(a, RATE);
    double mix_ms =
        (double)(SDL_GetPerformanceCounter() - mix_start) * 1000 / SDL_GetPerformanceFrequency();
    printf("MIX one_second_ms=%.3f three_voice_domains=1\n", mix_ms);
    dm_audio_edit_preview(a, 0, 0, false);
    preparation_benchmark(a);
    printf("PASS live edit: %d checks\n", checks);
    dm_project_free(&p);
    dm_audio_destroy(a);
    SDL_Quit();
}
