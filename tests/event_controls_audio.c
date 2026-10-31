#include "../src/audio.c"
#include "wave_fixture.h"

static void controls_process(DmAudio *a, int frames) {
    SDL_LockAudioStream(a->stream);

    while (frames > 0) {
        float out[BLOCK * 2];
        int n = frames > BLOCK ? BLOCK : frames;
        run_block(a, out, n);
        frames -= n;
    }

    SDL_UnlockAudioStream(a->stream);
}

static bool controls_voice(DmAudio *a, int key, bool sostenuto) {
    fluid_voice_t *voices[512] = {0};
    fluid_synth_get_voicelist(a->live.synth, voices, 512, -1);

    for (int i = 0; i < 512 && voices[i]; ++i) {
        if (fluid_voice_get_channel(voices[i]) == 0 && fluid_voice_get_key(voices[i]) == key &&
            (sostenuto ? fluid_voice_is_sostenuto(voices[i]) : fluid_voice_is_on(voices[i]))) {
            return true;
        }
    }

    return false;
}

static void controls_wait(DmAudio *a, void (*check)(bool, const char *)) {
    uint64_t start = SDL_GetTicks();

    while (dm_audio_busy(a)) {
        char error[512];
        bool success = false;

        if (dm_audio_poll(a, error, sizeof(error), &success, NULL)) {
            check(success, error);
        }

        check(SDL_GetTicks() - start < 30000, "worker timeout");
        SDL_Delay(5);
    }
}

static double controls_energy(DmBytes *wave, double start, double end, int channel) {
    double energy = 0;
    size_t size, offset = wave_data_offset(wave, &size);

    for (size_t f = (size_t)(start * RATE); f < (size_t)(end * RATE); ++f) {
        size_t i = offset + f * 4 + channel * 2;

        if (i + 1 >= offset + size) {
            break;
        }

        double sample = (int16_t)(wave->data[i] | wave->data[i + 1] << 8);
        energy += sample * sample;
    }

    return energy;
}

static int controls_crossings(DmBytes *wave, double start, double end) {
    int crossings = 0, previous = 0;
    size_t size, offset = wave_data_offset(wave, &size);

    for (size_t f = (size_t)(start * RATE); f < (size_t)(end * RATE); ++f) {
        size_t i = offset + f * 4;

        if (i + 1 >= offset + size) {
            break;
        }

        int sample = (int16_t)(wave->data[i] | wave->data[i + 1] << 8);
        crossings += previous < 0 && sample >= 0;
        previous = sample;
    }

    return crossings;
}

void test_event_controls_audio(DmAudio *a, void (*check)(bool, const char *)) {
    char error[512];
    check(dm_audio_soundfont(a, "agents/reports/generated-sine.sf2", error, sizeof(error)),
          "load isolated audio fixture");
    controls_wait(a, check);
    check(SDL_PauseAudioStreamDevice(a->stream), "pause device for deterministic controls test");
    DmProject p;
    dm_project_init(&p);
    snprintf(p.soundfont, sizeof(p.soundfont), "agents/reports/generated-sine.sf2");
    p.bpm = 120;
    int notes = dm_pattern_add(&p, "Sostenuto voices");
    dm_note_add(&p.patterns[notes], (DmNote){0, 1, 60, 100, 0});
    dm_note_add(&p.patterns[notes], (DmNote){1.5, 1, 64, 100, 0});
    dm_clip_add(&p, (DmClip){.pattern = notes, .length = 4});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = .5, .type = 0xb0, .a = 66, .b = 127, .pattern = notes, .curve = 1});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = 3, .type = 0xb0, .a = 66, .b = 127, .pattern = notes, .curve = 1});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = 3, .type = 0xb0, .a = 66, .b = 0, .pattern = notes, .curve = -1});
    dm_audio_monitor(a, true);
    check(dm_audio_publish(a, &p, 1, true, error, sizeof(error)), "seek into sostenuto interval");
    controls_process(a, BLOCK * 4);
    check(controls_voice(a, 60, true), "seek restores captured sostenuto voice");
    check(controls_voice(a, 64, false) && !controls_voice(a, 64, true),
          "seek does not capture notes played after pedal down");
    controls_process(a, RATE / 3);
    check(controls_voice(a, 60, true) && !controls_voice(a, 64, true) &&
              !controls_voice(a, 64, false),
          "later note releases while captured note remains held");
    controls_process(a, RATE / 3);
    check(!controls_voice(a, 60, true), "sostenuto endpoint releases captured voice");
    /* Both pedals cooperate when chasing a seek, regardless of release order. */
    reset_chase_channel(a, 0);
    cache_event(a, &(DmScheduled){.status = 0x90, .a = 60, .b = 100});
    cache_controller(a, 0, 66, 127);
    cache_controller(a, 0, 64, 127);
    chase_note_off(a, 0, 60);
    cache_controller(a, 0, 64, 0);
    check(a->state.notes[0][60] == 100, "sustain release preserves sostenuto capture");
    cache_controller(a, 0, 66, 0);
    check(a->state.notes[0][60] == 0, "last pedal release clears chased voice");

    p.automation_count = 0;
    p.patterns[notes].count = 0;
    dm_note_add(&p.patterns[notes], (DmNote){0, 4, 69, 110, 0});
    int controls = dm_pattern_add(&p, "Volume section");
    p.patterns[controls].kind = 1;
    p.patterns[controls].event = 7;
    p.lane_count[0] = 1;
    dm_clip_add(&p, (DmClip){.pattern = controls, .length = 4, .lane = 1});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = 1, .type = 0xb0, .a = 7, .b = 0, .pattern = controls, .curve = 1});
    dm_automation_add(
        &p,
        (DmAutomation){.beat = 3, .type = 0xb0, .a = 7, .b = 0, .pattern = controls, .curve = 1});
    dm_automation_add(
        &p, (DmAutomation){
                .beat = 3, .type = 0xb0, .a = 7, .b = 100, .pattern = controls, .curve = -1});
    dm_audio_transport(a, false);
    check(dm_audio_export(a, &p, "agents/reports/event-controls-volume.wav", NULL, error,
                          sizeof(error)),
          "render volume section");
    controls_wait(a, check);
    DmBytes wave = {0};
    check(dm_read("agents/reports/event-controls-volume.wav", &wave, error, sizeof(error)),
          "read rendered volume fixture");
    double before = controls_energy(&wave, .2, .4, 0);
    double during = controls_energy(&wave, 1, 1.2, 0);
    double after = controls_energy(&wave, 1.7, 1.9, 0);
    check(before > 100000 && during < before * .1 && after > before * .5,
          "volume section audibly mutes and restores the held note");
    dm_bytes_free(&wave);
    p.patterns[controls].event = 10;

    for (size_t i = 0; i < p.automation_count; ++i) {
        p.automation[i].a = 10;
        p.automation[i].b = p.automation[i].curve < 0 ? 64 : 0;
    }

    check(
        dm_audio_export(a, &p, "agents/reports/event-controls-pan.wav", NULL, error, sizeof(error)),
        "render pan section");
    controls_wait(a, check);
    check(dm_read("agents/reports/event-controls-pan.wav", &wave, error, sizeof(error)),
          "read rendered pan fixture");
    double left = controls_energy(&wave, 1, 1.2, 0), right = controls_energy(&wave, 1, 1.2, 1);
    double restored_left = controls_energy(&wave, 1.7, 1.9, 0);
    double restored_right = controls_energy(&wave, 1.7, 1.9, 1);
    check(left > right * 10 && restored_left > restored_right * .8 &&
              restored_left < restored_right * 1.2,
          "pan section audibly moves left and restores center");
    dm_bytes_free(&wave);
    p.patterns[notes].count = 0;
    dm_note_add(&p.patterns[notes], (DmNote){0, 1, 69, 110, 0});
    dm_note_add(&p.patterns[notes], (DmNote){1, 5, 81, 110, 0});
    p.patterns[controls].event = 5;

    for (size_t i = 0; i < p.clip_count; ++i) {
        p.clips[i].length = 8;
    }

    for (size_t i = 0; i < p.automation_count; ++i) {
        p.automation[i].a = 5;
        p.automation[i].beat = i == 0 ? 0 : 6;
        p.automation[i].b = p.automation[i].curve < 0 ? 0 : 5;
    }

    check(dm_audio_export(a, &p, "agents/reports/event-controls-glide-fast.wav", NULL, error,
                          sizeof(error)),
          "render fast automatic glide");
    controls_wait(a, check);
    check(dm_read("agents/reports/event-controls-glide-fast.wav", &wave, error, sizeof(error)),
          "read fast glide fixture");
    int fast = controls_crossings(&wave, .8, 1);
    dm_bytes_free(&wave);

    for (size_t i = 0; i < p.automation_count; ++i) {
        if (p.automation[i].curve > 0) {
            p.automation[i].b = 100;
        }
    }

    check(dm_audio_export(a, &p, "agents/reports/event-controls-glide-slow.wav", NULL, error,
                          sizeof(error)),
          "render slow automatic glide");
    controls_wait(a, check);
    check(dm_read("agents/reports/event-controls-glide-slow.wav", &wave, error, sizeof(error)),
          "read slow glide fixture");
    int slow = controls_crossings(&wave, .8, 1);
    check(fast > slow * 1.2 && slow > 20, "portamento values audibly change glide duration");
    dm_bytes_free(&wave);
    dm_project_free(&p);
}
