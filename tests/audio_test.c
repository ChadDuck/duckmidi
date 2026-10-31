#include "audio.h"
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

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi");
    check(SDL_Init(SDL_INIT_AUDIO), "audio initialization");
    char error[512] = {0};
    DmAudio *a = dm_audio_create(error, sizeof(error));
    check(a != NULL, error);
    check(!strcmp(dm_audio_driver(a), "wasapi"), "WASAPI backend");
    check(dm_audio_soundfont(a, "agents/reports/generated-sine.sf2", error, sizeof(error)),
          "start SoundFont worker");
    await_job(a);
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
    check(dm_audio_publish_pattern(a, &p, preview, 0, -1, p.patterns[preview].length, 2.5, true,
                                   error, sizeof(error)),
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
    check(dm_audio_publish_pattern(a, &p, held, 0, -1, p.patterns[held].length, .5, true, error,
                                   sizeof(error)),
          "seek under sustain pedal");
    SDL_Delay(300);
    check(dm_audio_peak(a, 0) > .001f, "seek restores pedal-held released voice");
    p.channels[2].enabled = true;
    check(dm_audio_publish(a, &p, 1, true, error, sizeof(error)),
          "enable keyboard fixture channel");
    dm_audio_preview(a, 2, 73, 100, true);
    SDL_Delay(100);
    check(dm_audio_update(a, &p, -1, 0, -1, 0, error, sizeof(error)),
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
