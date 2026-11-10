#include "../src/audio/audio.c"
#include "../shared/sample/library.h"
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
    check(dm_audio_update(a, p, NULL, error, sizeof(error)), "live schedule update");
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

static bool custom_voice(Synth *s, int channel, int key) {
    unsigned id = voice_id(s, channel, key, true);
    return id != UINT32_MAX && (id & UINT32_C(0x80000000));
}

static void routed_samples(DmAudio *a) {
    SDL_Time now;
    check(SDL_GetCurrentTime(&now), "unique sample fixture root");
    char root[DM_PATH], error[512] = {0};
    snprintf(root, sizeof(root), "agents/reports/live-samples-%llu", (unsigned long long)now);
    check(SDL_CreateDirectory(root), "make sample fixture library");
    DmSampleAsset asset = {"tone", "tone.wav", 4800, RATE, 1};
    DmSamplePreset preset = {"tone", "Tone", false, 0, 1};
    DmSampleZone zone = {.root = 60,
                         .key_hi = 127,
                         .velocity_lo = 1,
                         .velocity_hi = 127,
                         .envelope = {.sustain = 1, .release = .2},
                         .loop_mode = DM_SAMPLE_SUSTAIN_LOOP,
                         .loop = {480, 4320}};
    DmSampleManifest set = {.id = "live",
                            .name = "Live",
                            .asset_count = 1,
                            .preset_count = 1,
                            .zone_count = 1,
                            .assets = &asset,
                            .presets = &preset,
                            .zones = &zone};
    float pcm[4800];

    for (unsigned i = 0; i < SDL_arraysize(pcm); ++i) {
        pcm[i] = .25f * sinf((float)i * 6.2831853f / 96);
    }

    DmSampleAudio sample = {.pcm = pcm, .frames = 4800, .rate = RATE, .channels = 1};
    check(dm_sample_library_publish(root, &set, &sample, NULL, error, sizeof(error)), error);
    ++set.generation;
    zone.cents = 35;
    check(dm_sample_library_publish(root, &set, &sample, NULL, error, sizeof(error)), error);

    DmProject p;
    dm_project_init(&p);
    p.channels[3].enabled = true;
    p.channels[3].program = 0;
    check(dm_pattern_add(&p, "Routed sample voices") >= 0, "create routed pattern");
    check(dm_note_add(&p.patterns[0], (DmNote){0, 2, 60, 100, 0}), "create default-route note");
    check(dm_note_add(&p.patterns[0], (DmNote){0, 2, 64, 100, 0, 2}), "create port note");
    check(dm_note_add(&p.patterns[0], (DmNote){1, .5, 67, 100, 0}), "create ordinary note");
    check(dm_note_add(&p.patterns[0], (DmNote){2, 1, 69, 100, 0}), "create later sample note");
    check(dm_clip_add(&p, (DmClip){0, 0, 3, 0, 4}), "place routed pattern");
    DmAutomation program = {.beat = 1, .type = MIDI_PROGRAM, .channel = 3, .a = 1, .pattern = -1};
    check(dm_automation_add(&p, program), "switch program with sample voice held");
    program.beat = 2;
    program.a = 0;
    check(dm_automation_add(&p, program), "switch back to sample");
    DmPatchReplacement assignment = {
        .key = {.channel = 3}, .generation = 1, .set_id = "live", .preset_id = "tone"};
    snprintf(assignment.locator, sizeof(assignment.locator), "%s", root);
    check(dm_patch_replacement_set(&p, &assignment), "assign default sample");
    assignment.key.port = 2;
    assignment.generation = 2;
    check(dm_patch_replacement_set(&p, &assignment), "assign distinct colored-port revision");
    check(dm_audio_publish(a, &p, 0, true, error, sizeof(error)), error);
    process(a, BLOCK);
    check(custom_voice(&a->live, 3, 60) && custom_voice(a->port_live[2], 3, 64),
          "independent routes use their custom preset");
    DmeSampleZoneParams edited = {.size = sizeof(edited),
                                  .root = 60,
                                  .key_hi = 127,
                                  .velocity_lo = 1,
                                  .velocity_hi = 127,
                                  .pan = 1,
                                  .sustain = .5,
                                  .release = .3,
                                  .loop_mode = DME_SAMPLE_LOOP_SUSTAIN,
                                  .loop_start = 480,
                                  .loop_end = 4320};
    unsigned held = voice_id(&a->live, 3, 60, true);
    dm_audio_sample_edit(a, root, "live", "tone", 0, "tone", &edited);
    check(voice_id(&a->live, 3, 60, true) == held,
          "editing an assigned sample retains its sounding song voice");
    fluid_voice_t *edited_voices[512] = {0};
    fluid_synth_get_voicelist(a->live.synth, edited_voices, 512, -1);
    bool panned = false;

    for (unsigned i = 0; i < 512 && edited_voices[i]; ++i) {
        if (fluid_voice_get_id(edited_voices[i]) == held) {
            panned = fluid_voice_gen_get(edited_voices[i], GEN_PAN) == 500;
        }
    }

    check(panned, "assigned sample edits reach the active playback bank");
    check(a->schedule.items[0].port < DM_NOTE_PORTS, "prepared schedule is routed");
    process(a, RATE * 3 / 5);
    check(custom_voice(&a->live, 3, 60) && !custom_voice(&a->live, 3, 67) &&
              voice_id(&a->live, 3, 67, true) != UINT32_MAX,
          "ordinary program uses SoundFont while custom voice remains held");

    assignment.key.port = 0;
    check(dm_patch_replacement_set(&p, &assignment), "change sample revision during playback");
    int old = a->sample_bank;
    update(a, &p);
    check(a->sample_bank != old && a->sample_banks[old],
          "publish a new bank while the synth retains its previous voices");
    process(a, RATE / 2);
    check(voice_id(&a->live, 3, 60, true) == UINT32_MAX && custom_voice(&a->live, 3, 69),
          "release the old voice and return to the current custom patch");
    check(dm_audio_publish(a, &p, .7, true, error, sizeof(error)), error);
    process(a, BLOCK);
    check(custom_voice(&a->live, 3, 60), "seek restores the patch at the original attack");
    dm_audio_loop(a, true, .2, .3);
    check(dm_audio_publish(a, &p, .29, true, error, sizeof(error)), error);
    process(a, RATE / 20);
    check(a->position < .3 && custom_voice(&a->live, 3, 60), "loop restarts a chased sample");
    assignment.key.port = 0;
    assignment.generation = 1;
    check(dm_patch_replacement_set(&p, &assignment), "change revision while a region loops");
    update(a, &p);
    process(a, RATE / 5);
    check(custom_voice(&a->live, 3, 60), "new bank also refreshes the prepared loop chase");
    dm_audio_loop(a, false, 0, 0);

    DmPatternPreview preview = {.pattern = 0, .channel = 3, .program = 0, .length = 4};
    check(dm_audio_publish_pattern(a, &p, &preview, .1, true, error, sizeof(error)), error);
    process(a, BLOCK);
    check(custom_voice(&a->live, 0, 60), "remapped pattern uses original channel's assignment");
    dm_audio_keyboard_mix(a, 0, 3, 100, 64);
    dm_audio_preview(a, 0, 72, 100, true);
    dm_audio_edit_preview_part(a, 3, 0, 73, true);
    process(a, BLOCK);
    check(custom_voice(&a->keyboard, 0, 72) && custom_voice(&a->feedback, 0, 73),
          "routed keyboard and edit feedback use separate custom voices");
    dm_audio_panic(a);
    process(a, RATE / 2);
    collect_sample_banks(a);
    check(!a->sample_banks[old], "earlier bank is collected after synth quiescence");

    assignment.generation = 99;
    strcpy(assignment.preset_id, "missing");
    check(dm_patch_replacement_set(&p, &assignment), "simulate a missing sample preset");
    check(dm_audio_publish(a, &p, 0, true, error, sizeof(error)), error);
    process(a, BLOCK);
    bool ok = false;
    check(dm_audio_poll(a, error, sizeof(error), &ok, NULL) && ok && strstr(error, "missing"),
          "missing preset produces a live diagnostic");
    check(voice_id(&a->live, 3, 60, true) != UINT32_MAX && !custom_voice(&a->live, 3, 60),
          "missing preset falls back to SoundFont");
    strcpy(preset.id, "missing");
    set.generation = 99;
    check(dm_sample_library_publish(root, &set, &sample, NULL, error, sizeof(error)),
          error);
    dm_audio_reload_samples(a);
    check(dm_audio_publish(a, &p, 0, true, error, sizeof(error)), error);
    process(a, BLOCK);
    check(custom_voice(&a->live, 3, 60), "explicit reload recovers a previously missing preset");
    int restored = a->sample_bank;
    dm_audio_monitor(a, false);
    assignment.generation = 1;
    check(dm_patch_replacement_set(&p, &assignment), "reassign a muted sample");
    update(a, &p);
    process(a, RATE / 2);
    collect_sample_banks(a);
    check(!a->sample_banks[restored], "muted monitoring drains retired custom voices");
    dm_audio_monitor(a, true);
    dm_audio_panic(a);
    process(a, RATE / 2);
    dm_project_free(&p);
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

static int32_t DME_CALL loop_pcm(void *user, uint32_t asset, uint64_t first, uint64_t count,
                                 float *pcm, uint64_t capacity) {
    (void)user;
    check(!asset && first + count <= 4096 && count <= capacity, "audition PCM bounds");

    for (uint64_t i = 0; i < count; ++i) {
        double frequency = first + i < 2048 ? 220 : 880;
        pcm[i] = (float)(.5 * sin(6.283185307179586 * frequency * (double)(first + i) / RATE));
    }

    return DME_OK;
}

static double audition_tone(DmAudio *a, double frequency) {
    double re = 0, im = 0;
    SDL_LockAudioStream(a->stream);

    for (int first = 0; first < 4800; first += BLOCK) {
        float out[BLOCK * 2] = {0};
        int frames = first + BLOCK <= 4800 ? BLOCK : 4800 - first;
        sample_audition_render(&a->sample_audition, out, frames);

        for (int i = 0; i < frames; ++i) {
            double phase = 6.283185307179586 * frequency * (first + i) / RATE;
            re += out[2 * i] * cos(phase);
            im += out[2 * i] * sin(phase);
        }
    }

    SDL_UnlockAudioStream(a->stream);
    return re * re + im * im;
}

static void live_loop_audition(DmAudio *a) {
    DmeSampleAsset asset = {.size = sizeof(asset), .rate = RATE, .channels = 1, .frames = 4096};
    DmeSampleZone zone = {.size = sizeof(zone),
                          .params = {.size = sizeof(zone.params),
                                     .root = 69,
                                     .key_hi = 127,
                                     .velocity_lo = 1,
                                     .velocity_hi = 127,
                                     .sustain = 1,
                                     .loop_mode = DME_SAMPLE_LOOP_CONTINUOUS,
                                     .loop_end = 2048}};
    DmeSamplePreset preset = {.size = sizeof(preset), .zone_count = 1, .name = {"Loop", 4}};
    DmeContext context = {.sample_assets = &asset,
                          .sample_asset_count = 1,
                          .sample_zones = &zone,
                          .sample_zone_count = 1,
                          .sample_presets = &preset,
                          .sample_preset_count = 1};
    DmeHost host = {.read_sample = loop_pcm};
    char error[512];
    check(dm_audio_sample_audition(a, &context, &host, 69, true, true, error, sizeof(error)),
          "start held loop audition");
    fluid_synth_t *synth = a->sample_audition.voice.synth;
    DmSampleSynth *bank = a->sample_audition.bank;
    double before = audition_tone(a, 220);
    check(dm_audio_sample_audition_loop(a, 2048, 4096), "publish loop boundaries during audition");
    check(a->sample_audition.voice.synth == synth && a->sample_audition.bank == bank &&
              voice_id(&a->sample_audition.voice, 0, 69, true) == 1,
          "loop edit retains synth, bank and sounding voice");
    double after = audition_tone(a, 880);
    double old = audition_tone(a, 220);
    check(before > 1 && after > 1 && after > 20 * old,
          "edited loop changes rendered audio immediately");
    check(!dm_audio_sample_audition_loop(a, 0, 4097), "reject out-of-bounds live loop");
    zone.params.loop_start = 2048;
    zone.params.loop_end = 4096;
    zone.params.attack = .2;
    zone.params.sustain = .4;
    check(dm_audio_sample_audition_edit(a, &zone.params), "publish a live envelope edit");
    check(a->sample_audition.voice.synth == synth && a->sample_audition.bank == bank &&
              voice_id(&a->sample_audition.voice, 0, 69, true) == 1,
          "envelope edit preserves the sounding voice and synthesis state");
    dm_audio_sample_audition_stop(a);
}

static void live_instrument_preview(DmAudio *a) {
    DmProject p;
    dm_project_init(&p);
    p.channels[0].program = 0;
    check(dm_pattern_add(&p, "Instrument preview") == 0, "create instrument preview pattern");
    check(
        dm_note_add(&p.patterns[0], (DmNote){.beat = .5, .length = .2, .key = 69, .velocity = 100}),
        "preview song note");
    check(dm_note_add(&p.patterns[0], (DmNote){.beat = 1, .length = 1, .key = 72, .velocity = 100}),
          "restored song note");
    check(dm_clip_add(&p, (DmClip){.length = 4}), "place preview notes");
    char error[512];
    dm_audio_loop(a, false, 0, 0);
    check(dm_audio_publish(a, &p, 0, true, error, sizeof(error)), "play preview fixture");
    process(a, BLOCK);
    DmPatchReplacement draft = {.key = {.program = 0}};
    check(dm_audio_instrument_preview(a, &draft, 0, error, sizeof(error)),
          "select an instrument while the song plays");
    process(a, RATE * 3 / 10);
    check(a->sample_audition.song_held[69] &&
              voice_id(&a->sample_audition.voice, 1, 69, false) != UINT32_MAX &&
              voice_id(&a->live, 0, 69, true) == UINT32_MAX,
          "future song notes use the selected instrument before Apply");
    check(a->playing && a->position > .3 && p.channels[0].program == 0 &&
              !p.patch_replacement_count,
          "preview keeps song timing and leaves project assignments untouched");
    dm_audio_sample_audition_stop(a);
    process(a, RATE * 3 / 10);
    check(!a->sample_audition.active && voice_id(&a->live, 0, 72, true) != UINT32_MAX,
          "cancelling restores the assigned instrument for following notes");
    dm_audio_transport(a, false);
    process(a, BLOCK);
    dm_project_free(&p);
}

static void live_patch_reset(DmAudio *a) {
    char error[512];
    check(dm_audio_soundfont(a, "agents/reports/generated-programs.sf2", error, sizeof(error)),
          "load distinct program presets");
    await_font(a);
    DmProject p;
    dm_project_init(&p);
    p.channels[0].program = 40;
    check(dm_pattern_add(&p, "Patch after reset") == 0, "create patch fixture");
    check(dm_note_add(&p.patterns[0], (DmNote){1, 1, 60, 100, 0}), "note after reset");
    check(dm_clip_add(&p, (DmClip){.length = 4}), "place notes through the automation resolver");
    uint8_t reset[] = {MIDI_SYSEX, 10, 0x41, 0x10, 0x42, 0x12,
                       0x40,       0,  0x7f, 0,    0x41, MIDI_SYSEX_END};
    uint8_t program[] = {MIDI_PROGRAM, 40};
    DmBytes message = {reset, sizeof(reset), 0};
    check(dm_event_add(&p, .25, 0, 0, &message), "add delayed GS reset");
    message = (DmBytes){program, sizeof(program), 0};
    check(dm_event_add(&p, .5, 0, 0, &message), "repeat the opening program after the reset");

    const double positions[] = {0, 1.25};

    for (size_t i = 0; i < SDL_arraysize(positions); ++i) {
        check(dm_audio_publish(a, &p, dm_beat_seconds(&p, positions[i]), true, error, sizeof(error)),
              error);
        process(a, i == 0 ? (int)(dm_beat_seconds(&p, 1.25) * RATE) : BLOCK);
        fluid_preset_t *preset = fluid_synth_get_channel_preset(a->live.synth, 0);
        check(preset && fluid_preset_get_num(preset) == 40 &&
                  voice_id(&a->live, 0, 60, true) != UINT32_MAX,
              i == 0 ? "post-reset patch plays from the start" : "seek restores the post-reset patch");
    }

    dm_audio_transport(a, false);
    process(a, BLOCK);
    dm_project_free(&p);
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
    routed_samples(a);
    live_loop_audition(a);
    live_instrument_preview(a);
    dm_audio_edit_preview(a, 0, 72, true);
    uint64_t mix_start = SDL_GetPerformanceCounter();
    process(a, RATE);
    double mix_ms =
        (double)(SDL_GetPerformanceCounter() - mix_start) * 1000 / SDL_GetPerformanceFrequency();
    printf("MIX one_second_ms=%.3f three_voice_domains=1\n", mix_ms);
    dm_audio_edit_preview(a, 0, 0, false);
    preparation_benchmark(a);
    live_patch_reset(a);
    printf("PASS live edit: %d checks\n", checks);
    dm_project_free(&p);
    dm_audio_destroy(a);
    SDL_Quit();
}
