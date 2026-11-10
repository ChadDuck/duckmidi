#include "audio/audio.h"
#include "../shared/sample/library.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wave_fixture.h"

static void check(bool value, const char *what) {
    if (!value) {
        fprintf(stderr, "FAIL %s: %s\n", what, SDL_GetError());
        exit(1);
    }
}

static void await_job(DmAudio *a) {
    uint64_t start = SDL_GetTicks();
    bool success = false;
    char ed_message[512];

    while (dm_audio_busy(a)) {
        if (dm_audio_poll(a, ed_message, sizeof(ed_message), &success, NULL)) {
            printf("%s\n", ed_message);
            check(success, ed_message);
        }

        check(SDL_GetTicks() - start < 30000, "worker timeout");
        SDL_Delay(5);
    }
}

static double sample_tone(const DmSampleAudio *audio, double start, int channel, double hz) {
    size_t first = (size_t)(start * audio->rate);
    size_t count = (size_t)audio->rate / 10;
    double sine = 0, cosine = 0;
    check(audio->channels == 2 && first + count <= audio->frames, "offline sample window");

    for (size_t i = 0; i < count; ++i) {
        double phase = 6.283185307179586 * hz * (double)i / audio->rate;
        float value = audio->pcm[(first + i) * 2 + (size_t)channel];
        sine += value * sin(phase);
        cosine += value * cos(phase);
    }

    return 2 * hypot(sine, cosine) / (double)count;
}

static void render_sample_instrument(DmAudio *audio) {
    enum { FRAMES = 4800, RATE = 48000 };

    float pcm[FRAMES * 2];

    for (size_t i = 0; i < FRAMES; ++i) {
        double phase = 6.283185307179586 * (double)i / RATE;
        pcm[2 * i] = (float)(0.5 * sin(660 * phase));
        pcm[2 * i + 1] = (float)(0.5 * sin(880 * phase));
    }

    DmSampleAudio sample = {.pcm = pcm, .frames = FRAMES, .rate = RATE, .channels = 2};
    DmSampleAsset asset = {
        .id = "tone", .file = "tone.wav", .frames = FRAMES, .rate = RATE, .channels = 2};
    DmSamplePreset preset = {.id = "tone", .name = "Generated sample", .zone_count = 1};
    DmSampleZone zone = {.root = 69,
                         .key_hi = 127,
                         .velocity_lo = 1,
                         .velocity_hi = 127,
                         .cents = 100,
                         .envelope = {.attack = 0.001, .sustain = 1, .release = 0.04},
                         .loop_mode = DM_SAMPLE_CONTINUOUS_LOOP,
                         .loop = {0, FRAMES}};
    DmSampleManifest manifest = {.id = "audio-test",
                                 .name = "Generated audio fixture",
                                 .generation = 1,
                                 .asset_count = 1,
                                 .preset_count = 1,
                                 .zone_count = 1,
                                 .assets = &asset,
                                 .presets = &preset,
                                 .zones = &zone};
    char root[DM_PATH], error[512] = {0};
    SDL_Time now;
    check(SDL_GetCurrentTime(&now), "timestamp for isolated sample library");
    snprintf(root, sizeof(root), "agents/reports/sample-audio-%llu", (unsigned long long)now);
    check(SDL_CreateDirectory(root), "create isolated sample library");
    check(dm_sample_library_publish(root, &manifest, &sample, NULL, error, sizeof(error)), error);

    DmProject project;
    dm_project_init(&project);
    snprintf(project.soundfont, sizeof(project.soundfont), "agents/reports/generated-programs.sf2");
    project.channels[0].enabled = true;
    project.channels[0].program = 0;
    int pattern = dm_pattern_add(&project, "Sample, stock, sample");
    check(pattern >= 0, "create sample arrangement");

    for (int i = 0; i < 3; ++i) {
        check(dm_note_add(&project.patterns[pattern], (DmNote){2 * i, .8, 69, 112, 0}),
              "create isolated note");
    }

    check(dm_clip_add(&project, (DmClip){0, pattern, 0, 0}), "place sample arrangement");
    DmAutomation program = {.beat = 2, .type = MIDI_PROGRAM, .a = 1, .pattern = -1};
    check(dm_automation_add(&project, program), "switch to stock Program 1");
    program.beat = 4;
    program.a = 0;
    check(dm_automation_add(&project, program), "return to replaced Program 0");
    DmPatchReplacement replacement = {.generation = 1, .set_id = "audio-test", .preset_id = "tone"};
    snprintf(replacement.locator, sizeof(replacement.locator), "%s", root);
    check(dm_patch_replacement_set(&project, &replacement), "set sample replacement");
    const char *path = "agents/reports/offline-sample-fixture.wav";
    check(dm_audio_export(audio, &project, path, NULL, error, sizeof(error)), error);
    await_job(audio);
    DmSampleAudio rendered = {0};
    check(dm_sample_decode(path, NULL, &rendered, error, sizeof(error)), error);
    double corrected = exp2(-1.0 / 12);
    check(sample_tone(&rendered, .1, 0, 660 * corrected) > .02 &&
              sample_tone(&rendered, .1, 1, 880 * corrected) > .02,
          "offline audio preserves both sample channels and applies fine correction");
    check(sample_tone(&rendered, 1.1, 0, 440) > .02 &&
              sample_tone(&rendered, 1.1, 0, 660 * corrected) < .005,
          "unreplaced program renders through the selected SoundFont");
    check(sample_tone(&rendered, 2.1, 0, 660 * corrected) > .02,
          "returning to the original program restores its sample");
    dm_sample_audio_free(&rendered);

    strcpy(replacement.preset_id, "missing");
    check(dm_patch_replacement_set(&project, &replacement), "point to a missing revision");
    path = "agents/reports/offline-missing-sample-fixture.wav";
    check(dm_audio_export(audio, &project, path, NULL, error, sizeof(error)), error);
    await_job(audio);
    check(dm_sample_decode(path, NULL, &rendered, error, sizeof(error)), error);
    check(sample_tone(&rendered, .1, 0, 440) > .02 &&
              sample_tone(&rendered, .1, 0, 660 * corrected) < .005,
          "missing sample falls back to the selected SoundFont in audio export");
    dm_sample_audio_free(&rendered);
    dm_project_free(&project);
}

static int scheduled_program(const DmSchedule *s, double seconds) {
    int program = -1;

    for (size_t i = 0; i < s->count; ++i) {
        if ((s->items[i].status & 0xf0) == 0xc0 && s->items[i].seconds <= seconds + 1e-6) {
            program = s->items[i].a;
        }
    }

    return program;
}

static int scheduled_volume(const DmSchedule *s) {
    int volume = -1;

    for (size_t i = 0; i < s->count; ++i) {
        if ((s->items[i].status & 0xf0) == 0xb0 && s->items[i].a == 7 && s->items[i].seconds == 0) {
            volume = s->items[i].b;
        }
    }

    return volume;
}

/* Pattern playback takes program changes and channel state from the placement's region. */
static void pattern_region_programs(void) {
    DmProject p;
    dm_project_init(&p);
    p.channels[0].enabled = true;
    int id = dm_pattern_add(&p, "Region");
    dm_note_add(&p.patterns[id], (DmNote){0, 4, 60, 100, 0});
    dm_clip_add(&p, (DmClip){8, id, 0, 0, 0, 0});
    DmBytes before = {0}, volume = {0}, outside = {0};
    dm_bytes(&before, (uint8_t[]){0xc0, 40}, 2);
    dm_bytes(&volume, (uint8_t[]){0xb0, 7, 50}, 3);
    dm_bytes(&outside, (uint8_t[]){0xc0, 90}, 2);
    check(dm_event_add(&p, 4, 1, 0, &before), "add program before region");
    check(dm_event_add(&p, 6, 1, 0, &volume), "add volume before region");
    check(dm_event_add(&p, 20, 1, 0, &outside), "add program after region");
    dm_bytes_free(&before);
    dm_bytes_free(&volume);
    dm_bytes_free(&outside);
    check(dm_automation_add(&p, (DmAutomation){10, 0xc0, 0, 56, 0, -1, 0, 0, 0, false, 0, false}),
          "add song program inside region");
    DmPatternPreview follow = {id, 0, 0, true, 8, 4};
    DmSchedule s = {0};
    check(dm_pattern_schedule(&p, &follow, &s), "schedule placed pattern");
    check(scheduled_program(&s, 0) == 40, "pattern starts with the region's program");
    check(scheduled_program(&s, dm_beat_seconds(&p, 2)) == 56,
          "pattern plays program changes inside its region");
    check(scheduled_program(&s, 1000) == 56, "program changes after the region are left out");
    check(scheduled_volume(&s) == 50, "pattern starts with the region's volume");
    dm_schedule_free(&s);
    DmPatternPreview chosen = {id, 0, 12, false, 8, 4};
    check(dm_pattern_schedule(&p, &chosen, &s), "schedule chosen preview instrument");
    check(scheduled_program(&s, 0) == 12 && scheduled_program(&s, 1000) == 12,
          "a chosen preview instrument overrides the region's programs");
    dm_schedule_free(&s);
    dm_project_free(&p);
}

int main(void) {
    pattern_region_programs();
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi");
    check(SDL_Init(SDL_INIT_AUDIO), "audio initialization");
    char error[512] = {0};
    DmAudio *a = dm_audio_create(error, sizeof(error));
    check(a != NULL, error);
    check(!strcmp(dm_audio_driver(a), "wasapi"), "WASAPI backend");
    check(dm_audio_soundfont(a, "agents/reports/generated-sine.sf2", error, sizeof(error)),
          "start SoundFont worker");
    await_job(a);
    render_sample_instrument(a);
    DmProject p;
    dm_project_init(&p);
    snprintf(p.soundfont, sizeof(p.soundfont), "agents/reports/generated-sine.sf2");
    dm_pattern_add(&p, "Tone");
    dm_note_add(&p.patterns[0], (DmNote){0, 1, 69, 110, 0});
    dm_clip_add(&p, (DmClip){0, 0, 0, 0, 0, 0});
    check(dm_audio_export(a, &p, "agents/reports/generated-tone.wav", NULL, error, sizeof(error)),
          "start WAV worker");
    await_job(a);
    DmBytes wave = {0};
    check(dm_read("agents/reports/generated-tone.wav", &wave, error, sizeof(error)),
          "read generated WAV");
    size_t pcm_size, pcm = wave_data_offset(&wave, &pcm_size);
    check(pcm_size == 4 * 48000 * 4, "WAV PCM length");
    long long energy = 0;

    for (size_t i = pcm; i + 1 < pcm + pcm_size; i += 2) {
        int v = (int16_t)(wave.data[i] | wave.data[i + 1] << 8);
        energy += (long long)v * v;
    }

    check(energy > 1000000, "non-silent audio");
    int modulation = dm_pattern_add(&p, "Rendered expression LFO");
    p.patterns[modulation].kind = 1;
    p.patterns[modulation].mode = 1;
    p.patterns[modulation].event = 11;
    p.lane_count[0] = 1;
    check(dm_clip_add(&p, (DmClip){0, modulation, 0, 0, 4, 1}),
          "place expression LFO for WAV render");
    check(dm_audio_export(a, &p, "agents/reports/generated-lfo.wav", NULL, error, sizeof(error)),
          "render modular LFO to WAV");
    await_job(a);
    DmBytes lfo_wave = {0};
    check(dm_read("agents/reports/generated-lfo.wav", &lfo_wave, error, sizeof(error)),
          "read modular LFO WAV");
    long long lfo_energy = 0;
    pcm = wave_data_offset(&lfo_wave, &pcm_size);

    for (size_t i = pcm; i + 1 < pcm + pcm_size; i += 2) {
        int v = (int16_t)(lfo_wave.data[i] | lfo_wave.data[i + 1] << 8);
        lfo_energy += (long long)v * v;
    }

    check(lfo_energy > 1000000 && lfo_energy < energy / 2,
          "rendered expression automation produces a non-silent, attenuated waveform");
    dm_bytes_free(&lfo_wave);
    dm_pattern_remove(&p, modulation);
    p.lane_count[0] = 0;

    DmProject glide;
    dm_project_init(&glide);
    snprintf(glide.soundfont, sizeof(glide.soundfont), "%s", p.soundfont);
    int glide_id = dm_pattern_add(&glide, "Portamento timing");
    dm_note_add(&glide.patterns[glide_id], (DmNote){0, .5, 57, 110, 0});
    dm_note_add(&glide.patterns[glide_id], (DmNote){.5, 1, 69, 110, 0});
    dm_clip_add(&glide, (DmClip){0, glide_id, 0, 0, 0, 0});
    dm_automation_add(&glide, (DmAutomation){0, 0xb0, 0, 5, 5, glide_id, 0, 0, 0, false});
    dm_automation_add(&glide, (DmAutomation){0, 0xb0, 0, 65, 127, glide_id, 0, 0, 0, false});
    check(dm_audio_export(a, &glide, "agents/reports/portamento-auto.wav", NULL, error,
                          sizeof(error)),
          "portamento render");
    await_job(a);
    DmBytes glide_midi = {0};
    check(dm_midi(&glide, &glide_midi, error, sizeof(error)), "encode portamento fixture");
    check(dm_write("agents/reports/portamento.mid", glide_midi.data, glide_midi.size, error,
                   sizeof(error)),
          "save portamento fixture");
    dm_bytes_free(&glide_midi);
    dm_project_free(&glide);
    check(dm_audio_publish(a, &p, .25, false, error, sizeof(error)),
          "publish paused seek inside a sounding note");
    SDL_Delay(120);
    dm_audio_peak(a, 0); /* Discard meter accumulation before the settled silence window. */
    SDL_Delay(50);
    check(dm_audio_peak(a, 0) < .0001f, "paused scheduling does not replay chased notes");
    dm_audio_audition(a, 0, 69, 110, .1);
    SDL_Delay(40);
    check(dm_audio_peak(a, 0) > .001f && !dm_audio_playing(a) &&
              fabs(dm_audio_position(a) - .25) < .005,
          "explicit note audition sounds without starting paused transport");
    SDL_Delay(180);
    check(dm_audio_publish(a, &p, 0, true, error, sizeof(error)), "start transport");
    SDL_Delay(120);
    check(dm_audio_position(a) > .04, "transport advances independently");
    check(dm_audio_peak(a, 0) > .001f, "channel peak meter");
    double previous = dm_audio_position(a);
    dm_audio_control(a, 0, 7, 70);
    dm_audio_control(a, 0, 10, 32);
    SDL_Delay(80);
    check(dm_audio_playing(a) && dm_audio_position(a) > previous,
          "live mixer controls preserve transport");
    int preview = dm_pattern_add(&p, "Unplaced preview");
    check(preview >= 0, "create unplaced pattern");
    dm_note_add(&p.patterns[preview], (DmNote){5, 1, 69, 110, 0});
    p.channels[0].mute = true;
    p.channels[1].solo = true;
    DmPatternPreview unplaced = {preview, 0, -1, false, 0, p.patterns[preview].length};
    check(dm_audio_publish_pattern(a, &p, &unplaced, 2.5, true, error, sizeof(error)),
          "preview unplaced pattern");
    SDL_Delay(120);
    check(dm_audio_peak(a, 0) > .001f && dm_audio_position(a) > 2.54,
          "unplaced pattern produces audio at its local position");
    check(p.channels[0].mute && p.channels[1].solo,
          "pattern playback leaves song mixer settings intact");
    DmSchedule muted_song = {0};
    check(dm_schedule(&p, &muted_song), "prepare song under mixer mute/solo");
    bool note_on = false;

    for (size_t i = 0; i < muted_song.count; ++i) {
        note_on |= (muted_song.items[i].status & 0xf0) == 0x90 && muted_song.items[i].b > 0;
    }

    check(!note_on, "song playback still respects mixer mute/solo");
    dm_schedule_free(&muted_song);
    p.channels[0].mute = p.channels[1].solo = false;
    int held = dm_pattern_add(&p, "Sustain seek");
    dm_note_add(&p.patterns[held], (DmNote){0, .2, 69, 110, 0});
    dm_automation_add(&p, (DmAutomation){.1, 0xb0, 0, 64, 127, held, 0, 0, 0, false});
    dm_automation_add(&p, (DmAutomation){3, 0xb0, 0, 64, 0, held, 0, 0, 0, false});
    DmPatternPreview sustain = {held, 0, -1, false, 0, p.patterns[held].length};
    check(dm_audio_publish_pattern(a, &p, &sustain, .5, true, error, sizeof(error)),
          "seek under sustain pedal");
    SDL_Delay(300);
    check(dm_audio_peak(a, 0) > .001f, "seek restores pedal-held released voice");
    p.channels[2].enabled = true;
    check(dm_audio_publish(a, &p, 1, true, error, sizeof(error)),
          "enable keyboard fixture channel");
    dm_audio_preview(a, 2, 73, 100, true);
    SDL_Delay(100);
    check(dm_audio_update(a, &p, NULL, error, sizeof(error)),
          "publish without resetting live voices");
    SDL_Delay(250);
    check(dm_audio_peak(a, 2) > .001f,
          "recording update keeps an unrelated held keyboard voice sounding");
    dm_audio_preview(a, 2, 73, 0, false);
    dm_audio_transport(a, false);
    check(dm_audio_soundfont(a, "agents/reports/does-not-exist.sf2", error, sizeof(error)),
          "invalid font job");
    bool success = true;

    while (dm_audio_busy(a)) {
        if (dm_audio_poll(a, error, sizeof(error), &success, NULL)) {
            check(!success, "failed SoundFont is reported");
        }

        SDL_Delay(5);
    }

    dm_audio_preview(a, 0, 69, 100, true);
    SDL_Delay(100);
    check(dm_audio_peak(a, 0) > .001f, "old synth survives failed load");
    dm_audio_preview(a, 0, 69, 0, false);
    check(dm_audio_publish(a, &p, 1.95, true, error, sizeof(error)),
          "start near song end for loop check");
    SDL_Delay(180);
    check(dm_audio_playing(a) && dm_audio_position(a) < .5,
          "ordinary song playback loops to the start without stopping");
    SDL_Delay(2050);
    check(dm_audio_playing(a) && dm_audio_position(a) < .7,
          "ordinary playback continues over a second loop boundary");
    check(dm_audio_publish(a, &p, .1, true, error, sizeof(error)),
          "start bounded loop from before its beginning");
    dm_audio_loop(a, true, .2, .4);
    SDL_Delay(550);
    check(dm_audio_playing(a) && dm_audio_position(a) >= .19 && dm_audio_position(a) < .42,
          "bounded loop wraps at its end and retains playback");
    check(dm_audio_peak(a, 0) > .001f, "bounded loop chases notes crossing its start");
    check(dm_audio_publish(a, &p, .6, true, error, sizeof(error)), "seek beyond selected region");
    dm_audio_loop(a, true, .2, .4);
    SDL_Delay(80);
    check(dm_audio_position(a) > .6,
          "seek past region continues normally instead of jumping immediately");
    SDL_Delay(1850);
    check(dm_audio_position(a) >= .19 && dm_audio_position(a) < .42,
          "after full-song wrap the selected region resumes looping");
    dm_audio_loop(a, false, 0, 0);
    dm_audio_transport(a, false);
    dm_audio_recording(a, true);
    check(dm_audio_publish(a, &p, 1.95, true, error, sizeof(error)),
          "start beyond-end recording check");
    SDL_Delay(160);
    check(dm_audio_playing(a) && dm_audio_position(a) > 2.04,
          "recording continues past arrangement end");
    dm_audio_recording(a, false);
    dm_audio_transport(a, false);
    check(dm_audio_retry(a, error, sizeof(error)), "audio device can be reopened");
    check(dm_audio_buffer_frames(a) > 0, "device format and buffer can be queried");
    check(dm_audio_publish(a, &p, 0, false, error, sizeof(error)), "stop for audition timer check");
    dm_audio_audition(a, 0, 69, 100, .05);
    SDL_Delay(30);
    check(dm_audio_peak(a, 0) > .001f, "instrument audition starts a note");
    SDL_Delay(500);
    dm_audio_peak(a, 0); /* A peak read clears the accumulated maximum. */
    SDL_Delay(50);
    check(dm_audio_peak(a, 0) < .001f, "instrument audition releases without GUI frames");
    uint8_t live_volume[DM_PARTS], live_pan[DM_PARTS];
    check(!dm_audio_mix_state(a, live_volume, live_pan), "stopped mixer shows channel settings");
    check(dm_automation_add(&p, (DmAutomation){.25, 0xb0, 0, 10, 30, -1, 0, 0, 0, false, 0, false}),
          "add song pan change");
    p.channels[0].pan = 64;
    check(dm_audio_publish(a, &p, 0, true, error, sizeof(error)), "play before the pan change");
    SDL_Delay(30);
    check(dm_audio_mix_state(a, live_volume, live_pan) && live_pan[0] == 64,
          "mixer state starts with the channel pan");
    SDL_Delay(250);
    check(dm_audio_mix_state(a, live_volume, live_pan) && live_pan[0] == 30,
          "mixer state follows a pan change as the song plays through it");
    check(dm_automation_add(&p, (DmAutomation){.5, 0xb0, 0, 7, 40, -1, 0, 0, 0, false, 0, false}),
          "add song volume change");
    check(dm_audio_publish(a, &p, 1, true, error, sizeof(error)), "seek after volume change");
    SDL_Delay(60);
    check(dm_audio_mix_state(a, live_volume, live_pan) && live_volume[0] == 40,
          "mixer state follows the song's volume after seeking");
    dm_audio_part_control(a, 0, 0, 10, 20);
    SDL_Delay(60);
    check(dm_audio_mix_state(a, live_volume, live_pan) && live_pan[0] == 20,
          "a mixer pan change during playback holds in the mixer state");
    dm_audio_transport(a, false);
    printf("Device buffer: %d frames (%.2f ms; not round-trip latency)\n",
           dm_audio_buffer_frames(a), dm_audio_buffer_ms(a));
    printf("PASS audio: WASAPI, background SF2/WAV, valid non-silent WAV, transport, meters, "
           "failure recovery\n");
    dm_bytes_free(&wave);
    dm_project_free(&p);
    dm_audio_destroy(a);
    SDL_Quit();
    return 0;
}
