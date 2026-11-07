#include "../src/audio_samples.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RATE = 48000, FRAMES = RATE };

static float input[FRAMES], output[FRAMES * 2];

static int32_t DME_CALL read_pcm(void *user, uint32_t asset, uint64_t start, uint64_t count,
                                 float *pcm, uint64_t capacity) {
    (void)user;
    assert(!asset && start <= FRAMES && count <= FRAMES - start && count <= capacity);
    for (uint64_t i = 0; i < count; ++i) {
        pcm[i] = input[start + i];
    }

    return DME_OK;
}

static void signal(bool harmonic) {
    for (unsigned i = 0; i < FRAMES; ++i) {
        double phase = 6.283185307179586 * i / RATE;
        input[i] = (float)(.15 * sin(440 * phase) + (harmonic ? .15 * sin(6600 * phase) : 0));
    }
}

static double tone(double frequency, unsigned first, unsigned count) {
    double real = 0, imaginary = 0;

    for (unsigned i = first; i < first + count; ++i) {
        double angle = 6.283185307179586 * frequency * i / RATE;
        real += output[2 * i] * cos(angle);
        imaginary += output[2 * i] * sin(angle);
    }

    return 2 * hypot(real, imaginary) / count;
}

static double tail(void) {
    double energy = 0;

    for (unsigned i = RATE / 2; i < FRAMES; ++i) {
        energy += output[2 * i] * output[2 * i];
    }

    return energy / (FRAMES - RATE / 2);
}

static void render_frames(fluid_synth_t *synth, unsigned frames) {
    for (unsigned at = 0; at < frames; at += 64) {
        unsigned count = frames - at < 64 ? frames - at : 64;
        assert(fluid_synth_write_float(synth, (int)count, output + 2 * at, 0, 2, output + 2 * at, 1,
                                       2) == FLUID_OK);
    }
}

static void render_zone(DmeSampleZoneParams params, int key, unsigned mode) {
    DmeSampleAsset asset = {.size = sizeof(asset), .rate = RATE, .channels = 1, .frames = FRAMES};
    DmeSampleZone zones[2] = {{.size = sizeof(DmeSampleZone), .params = params},
                              {.size = sizeof(DmeSampleZone), .params = params}};
    DmeSamplePreset preset = {.size = sizeof(preset), .zone_count = 1, .name = {"Tone", 4}};
    if (mode == 3) {
        preset.flags = DME_SAMPLE_PERCUSSION;
        preset.zone_count = 2;
        zones[0].params.cut_group = zones[1].params.cut_group = 2;
        zones[0].params.cut_by = zones[1].params.cut_by = 2;
        zones[1].params.loop_mode = DME_SAMPLE_NO_LOOP;
    }

    DmeContext context = {.sample_assets = &asset,
                          .sample_asset_count = 1,
                          .sample_zones = zones,
                          .sample_zone_count = preset.zone_count,
                          .sample_presets = &preset,
                          .sample_preset_count = 1};
    DmeHost host = {.read_sample = read_pcm};
    DmSampleSynth *bank = dm_sample_synth_create(&context, NULL, &host);
    assert(bank);
    fluid_settings_t *settings = new_fluid_settings();
    assert(settings);
    fluid_settings_setnum(settings, "synth.sample-rate", RATE);
    fluid_settings_setnum(settings, "synth.gain", .5);
    fluid_settings_setint(settings, "synth.threadsafe-api", 0);
    fluid_synth_t *synth = new_fluid_synth(settings);
    assert(synth);
    assert(dm_sample_synth_start(bank, synth, 1, 1, 0, key, 127) == DM_SAMPLE_RELEASE);
    DmSamplePedals pedals = {0};
    dm_sample_pedal_on(&pedals, 1, 0);

    for (unsigned first = 0; first < FRAMES; first += 64) {
        if (mode == 1 && first == 4800) {
            fluid_synth_all_sounds_off(synth, 0);
        }

        if (mode == 2 && first == 2432) {
            dm_sample_pedal_cc(&pedals, synth, 0, MIDI_CC_SOSTENUTO, 127);
        }

        if ((mode == 2 || mode == 3) && first == 4800) {
            dm_sample_pedal_off(&pedals, synth, 1);
        }

        if (mode == 2 && first == 28800) {
            dm_sample_pedal_cc(&pedals, synth, 0, MIDI_CC_SOSTENUTO, 0);
        }

        assert(fluid_synth_write_float(synth, 64, output + 2 * first, 0, 2, output + 2 * first, 1,
                                       2) == FLUID_OK);
    }

    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
    dm_sample_synth_free(bank);
}

static void check_live_edit_and_cut(DmeSampleZoneParams params) {
    DmeSampleAsset asset = {
        .size = sizeof(asset), .rate = RATE, .channels = 1, .frames = FRAMES, .id = {"tone", 4}};
    DmeSampleZone zones[3] = {{.size = sizeof(DmeSampleZone), .params = params},
                              {.size = sizeof(DmeSampleZone), .params = params},
                              {.size = sizeof(DmeSampleZone), .params = params}};
    zones[0].params.key_lo = zones[0].params.key_hi = 69;
    zones[0].params.cut_by = 7;
    zones[1].params.key_lo = zones[1].params.key_hi = 81;
    zones[1].params.cut_group = 0;
    zones[2].params.cut_by = 7;
    DmeSamplePreset presets[2] = {
        {.size = sizeof(DmeSamplePreset), .zone_count = 2, .name = {"Kit", 3}},
        {.size = sizeof(DmeSamplePreset), .first_zone = 2, .zone_count = 1, .name = {"Other", 5}}};
    DmeContext context = {.sample_assets = &asset,
                          .sample_asset_count = 1,
                          .sample_zones = zones,
                          .sample_zone_count = 3,
                          .sample_presets = presets,
                          .sample_preset_count = 2};
    DmeHost host = {.read_sample = read_pcm};
    DmSampleSynth *bank = dm_sample_synth_create(&context, NULL, &host);
    fluid_settings_t *settings = new_fluid_settings();
    assert(bank && settings);
    fluid_settings_setnum(settings, "synth.sample-rate", RATE);
    fluid_settings_setint(settings, "synth.threadsafe-api", 0);
    fluid_settings_setint(settings, "synth.reverb.active", 0);
    fluid_settings_setint(settings, "synth.chorus.active", 0);
    fluid_synth_t *synth = new_fluid_synth(settings);
    assert(synth);
    assert(dm_sample_synth_start(bank, synth, 1, 10, 0, 69, 127) == DM_SAMPLE_RELEASE);
    render_frames(synth, 4800);
    double before = tone(440, 1280, 2048);
    params.key_lo = params.key_hi = 69;
    params.cut_by = 7;
    params.sustain = .25;
    params.pan = 1;
    params.attack = .2;
    assert(dm_sample_synth_edit_region(bank, synth, 1, 0, "tone", &params));
    assert(fluid_synth_get_active_voice_count(synth) == 1);
    fluid_voice_t *voices[512] = {0};
    fluid_synth_get_voicelist(synth, voices, 512, -1);
    assert(voices[0] && fluid_voice_get_id(voices[0]) == 10);
    assert(fabs(fluid_voice_gen_get(voices[0], GEN_PAN) - 500) < .01);
    render_frames(synth, 4800);
    assert(before > .001 && tone(440, 1280, 2048) < before * .01);
    puts("PASS live envelope and pan edits preserve the active voice and change rendered audio");
    dm_sample_synth_start(bank, synth, 1, 11, 0, 81, 127);
    assert(fluid_synth_get_active_voice_count(synth) == 2);
    memset(voices, 0, sizeof(voices));
    fluid_synth_get_voicelist(synth, voices, 512, -1);
    bool sibling = false;

    for (unsigned i = 0; i < 512 && voices[i]; ++i) {
        if (fluid_voice_get_id(voices[i]) == 11) {
            sibling = fluid_voice_get_key(voices[i]) == 81 &&
                      fluid_voice_gen_get(voices[i], GEN_PAN) == 0;
        }
    }

    assert(sibling);
    fluid_synth_stop(synth, 11);
    render_frames(synth, 4800);
    puts("PASS editing one region leaves its sibling range and pan intact with a shared asset");
    params.pan = 0;
    params.sustain = 1;
    params.gain_db = 6;
    assert(dm_sample_synth_edit_region(bank, synth, 1, 0, "tone", &params));
    /* Small slices exercise a rescale that spans several calls. */
    unsigned slices = 0;

    while (dm_sample_synth_rescale(bank, 1000)) {
        ++slices;
    }

    assert(slices > 1);
    render_frames(synth, 9600);
    assert(tone(440, 4800, 4096) > before * 1.8);
    params.gain_db = -6;
    assert(dm_sample_synth_edit_region(bank, synth, 1, 0, "tone", &params));
    while (dm_sample_synth_rescale(bank, 1000)) {
    }

    render_frames(synth, 9600);
    assert(tone(440, 4800, 4096) < before * .6);
    puts("PASS positive and negative live gain changes reach the held sample");
    params.loop_mode = DME_SAMPLE_NO_LOOP;
    params.playback_end = 256;
    assert(dm_sample_synth_edit_region(bank, synth, 1, 0, "tone", &params));
    render_frames(synth, 4800);
    assert(tone(440, 1280, 2048) < before * .001);
    puts("PASS shortening playback end reaches an already playing voice without retriggering");

    /* Restore the two trigger zones independently before exercising asymmetric groups. */
    delete_fluid_synth(synth);
    dm_sample_synth_free(bank);
    zones[1].params.cut_group = 7;
    bank = dm_sample_synth_create(&context, NULL, &host);
    synth = new_fluid_synth(settings);
    assert(bank && synth);
    dm_sample_synth_start(bank, synth, 1, 20, 0, 69, 127);
    dm_sample_synth_start(bank, synth, 1, 21, 0, 69, 127);
    dm_sample_synth_start(bank, synth, 1, 22, 1, 69, 127);
    dm_sample_synth_start(bank, synth, 2, 23, 0, 69, 127);
    render_frames(synth, 4800);
    assert(fluid_synth_get_active_voice_count(synth) == 4);
    dm_sample_synth_start(bank, synth, 1, 24, 0, 81, 127);
    render_frames(synth, 38400);
    memset(voices, 0, sizeof(voices));
    fluid_synth_get_voicelist(synth, voices, 512, -1);
    bool other_channel = false, other_preset = false, sender = false;

    for (unsigned i = 0; i < 512 && voices[i]; ++i) {
        unsigned id = fluid_voice_get_id(voices[i]);
        assert(id != 20 && id != 21);
        other_channel |= id == 22;
        other_preset |= id == 23;
        sender |= id == 24;
    }

    assert(other_channel && other_preset && sender);
    puts("PASS Cut / Cut by is asymmetric and scoped to the preset and MIDI channel");
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
    dm_sample_synth_free(bank);
}

int main(void) {
    DmeSampleZoneParams zone = {.size = sizeof(zone),
                                .root = 69,
                                .key_hi = 127,
                                .velocity_lo = 1,
                                .velocity_hi = 127,
                                .sustain = 1,
                                .loop_mode = DME_SAMPLE_LOOP_CONTINUOUS,
                                .loop_start = 0,
                                .loop_end = FRAMES};
    signal(false);
    DmeSampleZoneParams trimmed = zone;
    trimmed.loop_mode = DME_SAMPLE_NO_LOOP;
    trimmed.playback_start = RATE / 2;
    trimmed.playback_end = RATE * 3 / 4;

    for (unsigned i = RATE / 2; i < FRAMES; ++i) {
        input[i] = (float)(.15 * sin(6.283185307179586 * 880 * i / RATE));
    }

    render_zone(trimmed, 69, 0);
    assert(tone(880, 2400, 4800) > .01 && tone(440, 2400, 4800) < .001);
    assert(tone(880, 24000, 12000) < .00001);
    signal(false);
    puts("PASS playback boundaries select the audible source segment and stop at its end");
    check_live_edit_and_cut(zone);
    render_zone(zone, 69, false);
    double base = tone(440, 4800, 38400);
    assert(base > .01);
    render_zone(zone, 69, 2);
    assert(tone(440, 14400, 9600) > base * .8 && tone(440, 38400, 4800) < base * .01);
    puts("PASS sostenuto holds the captured custom voice and releases it on pedal-up");
    render_zone(zone, 69, 3);
    assert(tone(440, 1280, 2048) > base * 1.5 && tone(440, 14400, 9600) > base * .8);
    puts("PASS a layered drum retains its one-shot after releasing its loop, with scoped choking");
    render_zone(zone, 81, false);
    assert(tone(880, 4800, 38400) > base * .8);
    zone.tone.custom_tracking = 1;
    render_zone(zone, 81, false);
    assert(tone(440, 4800, 38400) > base * .8 && tone(880, 4800, 38400) < base * .01);
    zone.tone.coarse = 12;
    render_zone(zone, 69, false);
    assert(tone(880, 4800, 38400) > base * .8);
    zone.tone.coarse = 0;
    zone.cents = 75;
    zone.tone.fine = 25;
    render_zone(zone, 69, false);
    assert(tone(440 * exp2(-50.0 / 1200), 4800, 38400) > base * .8);
    puts("PASS native rate, root, tracking, coarse tune and fine pitch correction");
    zone.cents = zone.tone.fine = 0;
    zone.gain_db = -6;
    render_zone(zone, 69, false);
    assert(tone(440, 4800, 38400) > base * .47 && tone(440, 4800, 38400) < base * .53);
    zone.gain_db = 0;
    zone.pan = 1;
    render_zone(zone, 69, false);
    assert(tone(440, 4800, 38400) < base * .01);
    zone.pan = 0;
    zone.key_lo = 70;
    render_zone(zone, 69, false);
    assert(tone(440, 4800, 38400) < base * .001);
    zone.key_lo = 0;
    zone.tone.cutoff = 440;
    render_zone(zone, 69, false);
    double flat = tone(440, 4800, 38400);
    zone.tone.resonance = 12;
    render_zone(zone, 69, false);
    assert(tone(440, 4800, 38400) > flat * 1.5);
    puts("PASS gain, hard pan, key range and filter resonance");
    zone.tone.cutoff = zone.tone.resonance = 0;
    signal(true);
    render_zone(zone, 69, false);
    double high = tone(6600, 4800, 38400);
    zone.tone.cutoff = 1200;
    render_zone(zone, 69, false);
    assert(tone(6600, 4800, 38400) < high * .1 && tone(440, 4800, 38400) > base * .5);
    zone.tone.filter_depth = 3600;
    zone.attack = .2;
    render_zone(zone, 69, false);
    assert(tone(6600, 24000, 19200) > high * .3);
    puts("PASS low-pass cutoff and an envelope that opens the filter");
    zone.tone.filter_depth = zone.attack = zone.tone.cutoff = 0;
    signal(false);
    zone.tone.vibrato_rate = 5;
    zone.tone.vibrato_depth = 80;
    render_zone(zone, 69, false);
    assert(tone(435, 4800, 38400) > base * .05 || tone(445, 4800, 38400) > base * .05);
    puts("PASS vibrato produces pitch sidebands at the configured rate");
    zone.tone.vibrato_depth = 0;
    zone.tone.chorus = 1;
    render_zone(zone, 69, false);
    double spread = 0;

    for (unsigned i = 9600; i < FRAMES; ++i) {
        double difference = output[2 * i] - output[2 * i + 1];
        spread += difference * difference;
    }

    assert(spread > 1e-5);
    zone.tone.chorus = 0;
    puts("PASS chorus send produces a stereo effect");
    render_zone(zone, 69, true);
    double dry = tail();
    zone.tone.reverb = 1;
    render_zone(zone, 69, true);
    assert(tail() > fmax(dry * 2, 1e-10));
    puts("PASS reverb send produces an isolated effect tail");
    return 0;
}
