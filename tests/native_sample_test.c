#include "../libraries/export_support/curves.h"
#include "../libraries/export_support/sample.h"
#include "../include/duckmidi_midi.h"
#include "../src/text/en.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    DmeSampleZone zones[257];
    DmeSampleAsset asset;
    DmeSamplePreset preset;
    uint32_t zone_count;
    int32_t freeze_status, read_status;
    bool cancelled, cancel_after_freeze, distinct;
    unsigned freezes, popups, infos;
    char summary[2048];
} Fixture;

static DmeString str(const char *s) {
    return (DmeString){s, strlen(s)};
}

static Fixture fixture(void) {
    Fixture f = {
        .zone_count = 1,
        .asset = {.size = sizeof(DmeSampleAsset), .rate = 32000, .channels = 1, .frames = 512},
        .preset = {.size = sizeof(DmeSamplePreset), .zone_count = 1, .name = {"Test preset", 11}}};

    for (unsigned i = 0; i < 257; i++) {
        f.zones[i] = (DmeSampleZone){.size = sizeof(DmeSampleZone),
                                     .params = {.size = sizeof(DmeSampleZoneParams),
                                                .root = 60,
                                                .key_hi = 127,
                                                .velocity_hi = 127,
                                                .sustain = .7,
                                                .release = .1}};
    }

    return f;
}

static int32_t DME_CALL cancelled(void *user) {
    return ((Fixture *)user)->cancelled;
}

static int32_t DME_CALL freeze(void *user, const DmeSampleSelection *selection,
                               DmeFrozenSample *out, char *error, uint64_t cap) {
    Fixture *f = user;
    assert(selection->generation == 1);
    f->freezes++;

    if (f->freeze_status != DME_OK) {
        snprintf(error, (size_t)cap, "fixture freeze failure");
        return f->freeze_status;
    }

    f->preset.zone_count = f->zone_count;
    f->cancelled = f->cancel_after_freeze;
    *out = (DmeFrozenSample){.size = sizeof(*out),
                             .preset_count = 1,
                             .zone_count = f->zone_count,
                             .asset_count = 1,
                             .handle = 1,
                             .presets = &f->preset,
                             .zones = f->zones,
                             .assets = &f->asset};
    return DME_OK;
}

static int32_t DME_CALL pcm(void *user, uint64_t handle, uint32_t asset, uint64_t first,
                            uint64_t frames, float *out, uint64_t capacity) {
    Fixture *f = user;
    assert(handle == 1 && asset == 0 && first + frames <= f->asset.frames);
    assert(capacity >= frames * f->asset.channels);

    for (uint64_t i = 0; i < frames; i++) {
        for (uint32_t ch = 0; ch < f->asset.channels; ch++) {
            out[i * f->asset.channels + ch] = (float)(.75 * sin((first + i) * .1));
        }
    }

    return f->read_status;
}

static void DME_CALL diagnostic(void *user, uint32_t severity, uint32_t flags,
                                const char *message) {
    Fixture *f = user;

    if (severity == DME_WARNING) {
        /* Each custom sample change is one warning-list item under the Custom samples heading. */
        assert(flags == (DME_POPUP | DME_LIST) && strstr(message, "Custom samples") == message);
        f->popups++;
        size_t used = strlen(f->summary);
        snprintf(f->summary + used, sizeof(f->summary) - used, "%s;", message);
    } else {
        assert(severity == DME_INFO && flags == DME_DISPLAY);
        f->infos++;
    }
}

static DmeHost host(Fixture *f) {
    return (DmeHost){.user = f,
                     .cancelled = cancelled,
                     .freeze_sample = freeze,
                     .read_frozen_sample = pcm,
                     .diagnostic = diagnostic};
}

static bool prepare(Fixture *f, uint32_t native, const DmeContext *context, bool replace,
                    bool choose, EsNativeSamples *samples, char *error, size_t cap) {
    EsMappings *mappings = es_mappings_create(1, 1);
    assert(mappings && es_mappings_inspect(mappings, context, NULL, 1, error, cap));

    for (size_t i = 0; i < es_mappings_count(mappings); i++) {
        const EsSourceRow *row = es_mappings_source(mappings, i);
        char preset_id[32];
        snprintf(preset_id, sizeof(preset_id), "preset%zu", f->distinct ? i : 0);
        DmeCustomSelection selection = {.size = sizeof(selection),
                                        .enabled = 1,
                                        .operation = replace ? DME_SAMPLE_REPLACE : DME_SAMPLE_ADD,
                                        .sample = {.size = sizeof(DmeSampleSelection),
                                                   .generation = 1,
                                                   .flags = row->source.flags,
                                                   .locator = str("generated"),
                                                   .set_id = str("set"),
                                                   .preset_id = choose ? str(preset_id) : str("")},
                                        .physical_target =
                                            replace ? str("generated-target") : str("")};
        assert(es_mappings_set(mappings, 0, row->id, &selection, error, cap) == DME_OK);
    }

    EsMappingJob *job = es_mappings_freeze(mappings, error, cap);
    assert(job);
    DmeHost callbacks = host(f);
    bool ok = es_native_samples(samples, job, 0, native, context, &callbacks, error, cap);
    es_mapping_job_free(job);
    es_mappings_destroy(mappings);
    return ok;
}

static void preparation(void) {
    DmeEvent events[] = {{0, 1, MIDI_NOTE_ON, 60, 100, 0}, {96, 2, MIDI_NOTE_OFF, 60, 0, 0}};
    DmeContext context = {.events = events, .event_count = 2, .duration = 192, .ppq = 96};
    char error[2048] = {0};

    for (uint32_t native = ES_NATIVE_SM64; native <= ES_NATIVE_OOT; native++) {
        Fixture f = fixture();
        DmeSampleZoneParams *z = &f.zones[0].params;
        z->pan = .5;
        z->cut_group = z->cut_by = 1;
        z->loop_mode = DME_SAMPLE_LOOP_SUSTAIN;
        z->loop_start = 16;
        z->loop_end = 496;
        z->playback_end = 1024;
        z->gain_db = 36;
        z->delay = z->hold = z->decay = z->release = 200;
        z->attack = -1;
        z->sustain = 2;
        z->tone = (DmeSampleToneParams){.custom_tracking = 1,
                                        .tracking = 50,
                                        .cutoff = 1000,
                                        .resonance = 3,
                                        .filter_depth = 100,
                                        .vibrato_rate = 5,
                                        .vibrato_depth = 20,
                                        .chorus = .4,
                                        .reverb = .5};
        f.asset.channels = 3;
        f.asset.rate = 384000;
        DmeSampleZone original = f.zones[0];
        EsNativeSamples s = {0};
        assert(prepare(&f, native, &context, false, true, &s, error, sizeof(error)));
        assert(s.count == 1 && s.notes[0].custom && s.notes[0].count == 1 && f.freezes == 1);
        /* The 1.25 ms loop is too short to hold the baked chorus. */
        uint32_t expected = 1u << ES_FIX_CHOKE | 1u << ES_FIX_SUSTAIN_LOOP | 1u << ES_FIX_FILTER |
                            1u << ES_FIX_FILTER_ENVELOPE | 1u << ES_FIX_CHORUS_SHORT |
                            1u << ES_FIX_TRACKING | 1u << ES_FIX_GAIN | 1u << ES_FIX_TIMING |
                            1u << ES_FIX_LENGTH | 1u << ES_FIX_FORMAT;

        if (native == ES_NATIVE_SM64) {
            expected |= 1u << ES_FIX_RELEASE;
        }

        assert(s.regions[0].fixes == expected);
        assert(s.regions[0].rate == 192000 && s.regions[0].frames == 256);
        assert(s.regions[0].loop_start == 8 && s.regions[0].loop_end == 248);
        assert(s.regions[0].params.loop_mode == DME_SAMPLE_LOOP_CONTINUOUS);
        assert(!memcmp(&original, f.zones, sizeof(original)));
        DmeHost callbacks = host(&f);
        es_native_samples_report(&s, native, &callbacks);
        assert(f.popups >= 10 && !f.infos && !strstr(f.summary, "pan") &&
               strstr(f.summary, "Test preset: filter envelope omitted") &&
               strstr(f.summary, "Test preset: gain lowered"));
        es_native_samples_free(&s);

        /* Ordinary envelope rounding is not reported. */
        f = fixture();
        assert(prepare(&f, native, &context, false, true, &s, error, sizeof(error)));
        callbacks = host(&f);
        es_native_samples_report(&s, native, &callbacks);
        assert(!f.popups && !f.infos);
        es_native_samples_free(&s);

        /* A 3 s decay to silence follows duckmidi's 96 dB fall in 12 dB points, as square-root
           levels because both engines square the envelope. */
        f = fixture();
        f.zones[0].params.decay = 3;
        f.zones[0].params.sustain = 0;
        assert(prepare(&f, native, &context, false, true, &s, error, sizeof(error)));
        const EsNativeRegion *shaped = &s.regions[0];
        assert(shaped->point_count == 8 && shaped->points[0][1] == 32767);
        unsigned ticks = 0;

        for (unsigned i = 1; i < 8; i++) {
            ticks += (unsigned)shaped->points[i][0];
        }

        /* The first decay point lands 96/7 dB down: amplitude 0.207, envelope sqrt 0.455. */
        assert(ticks >= 718 && ticks <= 722 && abs(shaped->points[1][1] - 14907) < 40 &&
               shaped->points[7][1] == 0);
        es_native_samples_free(&s);

        /* Sustain keeps its amplitude: 0.25 is written as 0.5. */
        f = fixture();
        f.zones[0].params.sustain = .25;
        assert(prepare(&f, native, &context, false, true, &s, error, sizeof(error)));
        shaped = &s.regions[0];
        assert(shaped->points[shaped->point_count - 1][1] == 16384);
        es_native_samples_free(&s);

        /* A 1 s release becomes the native fade that tracks duckmidi's: shorter, because the
           games fade in a straight line. SM64 uses table entry 12 (0.474 s), OoT 228 (0.383 s). */
        f = fixture();
        f.zones[0].params.release = 1;
        assert(prepare(&f, native, &context, false, true, &s, error, sizeof(error)));
        shaped = &s.regions[0];
        assert(native == ES_NATIVE_SM64 ? shaped->release == 12 : shaped->release == 228);
        assert(!(shaped->fixes & 1u << ES_FIX_RELEASE));
        es_native_samples_free(&s);

        for (unsigned invalid = 0; invalid < 7; invalid++) {
            f = fixture();

            switch (invalid) {
            case 0:
                f.zones[0].params.key_lo = 127;
                f.zones[0].params.key_hi = 60;
                break;
            case 1:
                f.zones[0].params.pan = NAN;
                break;
            case 2:
                f.zones[0].params.size--;
                break;
            case 3:
                f.zones[0].asset = 1;
                break;
            case 4:
                f.zone_count = 257;
                break;
            case 5:
                f.freeze_status = DME_UNAVAILABLE;
                break;
            default:
                break;
            }

            assert(prepare(&f, native, &context, false, invalid != 6, &s, error, sizeof(error)));
            assert(!s.count && !s.notes[0].custom && s.fixes[ES_FIX_ORDINARY].count == 1);
            assert(!*error);
            es_native_samples_free(&s);
        }

        /* Service, PCM-read and cancellation faults remain errors. */
        for (unsigned failure = 0; failure < 4; failure++) {
            f = fixture();
            f.freeze_status = failure == 0 ? DME_NOMEM : failure == 1 ? DME_FAILED : DME_OK;
            f.read_status = failure == 2 ? DME_FAILED : DME_OK;
            f.cancelled = failure == 3;
            assert(!prepare(&f, native, &context, false, true, &s, error, sizeof(error)));
            assert(!s.fixes[ES_FIX_ORDINARY].count);
            es_native_samples_free(&s);
            error[0] = 0;
        }
    }
}

static void replace_and_drums(void) {
    DmeEvent events[] = {{0, 1, MIDI_NOTE_ON, 60, 100, 0},
                         {96, 2, MIDI_NOTE_ON, 72, 100, 0},
                         {192, 3, MIDI_NOTE_ON, 72, 100, 0}};
    DmeContext context = {.events = events, .event_count = 3, .duration = 288, .ppq = 96};
    char error[2048] = {0};

    for (uint32_t native = ES_NATIVE_SM64; native <= ES_NATIVE_OOT; native++) {
        Fixture f = fixture();
        f.zone_count = 2;
        f.zones[0].params.key_hi = 60;
        f.zones[1].params.key_lo = f.zones[1].params.root = 72;
        EsNativeSamples s = {0};
        assert(prepare(&f, native, &context, true, true, &s, error, sizeof(error)));
        assert(s.count == 1 && s.regions[0].params.root == 72);
        assert(s.fixes[ES_FIX_REPLACE_ZONE].count == 1);
        char name[128];
        snprintf(name, sizeof(name), "%s", s.regions[0].name);
        snprintf(s.regions[0].name, sizeof(s.regions[0].name), "physical_name");
        es_native_add_instead(&s, &s.regions[0]);
        assert(!s.regions[0].target && !strcmp(name, s.regions[0].name));
        es_native_samples_free(&s);
        context.event_count = 2;
        assert(prepare(&f, native, &context, true, true, &s, error, sizeof(error)));
        assert(s.regions[0].params.root == 60);
        es_native_samples_free(&s);
        context.event_count = 3;

        f.cancel_after_freeze = true;
        assert(!prepare(&f, native, &context, true, true, &s, error, sizeof(error)));
        assert(!s.fixes[ES_FIX_ORDINARY].count && !*error);
        es_native_samples_free(&s);

        f = fixture();
        f.zone_count = 129;
        context.percussion_channels = 1;
        assert(prepare(&f, native, &context, false, true, &s, error, sizeof(error)));
        assert(!s.count && s.fixes[ES_FIX_DRUMS].count == 1);
        assert(!s.notes[0].custom && !s.notes[1].custom && !s.notes[2].custom);
        es_native_samples_free(&s);
        context.percussion_channels = 0;
    }
}

static void row_fallbacks(void) {
    DmeEvent events[] = {{0, 1, MIDI_NOTE_ON, 60, 100, 0},  {96, 2, MIDI_PROGRAM, 1, 0, 0},
                         {96, 3, MIDI_NOTE_ON, 72, 100, 0}, {120, 4, MIDI_NOTE_ON, 120, 100, 0},
                         {192, 5, MIDI_PROGRAM, 2, 0, 0},   {192, 6, MIDI_NOTE_ON, 84, 100, 0}};
    DmeContext context = {.events = events, .event_count = 6, .duration = 288, .ppq = 96};
    char error[2048] = {0};

    for (uint32_t native = ES_NATIVE_SM64; native <= ES_NATIVE_OOT; native++) {
        Fixture f = fixture();
        f.distinct = true;
        f.zones[0].params.key_hi = 100;
        EsNativeSamples s = {0};
        assert(prepare(&f, native, &context, false, true, &s, error, sizeof(error)));
        assert(s.count == 3 && s.notes[3].custom && !s.notes[3].count);
        es_native_fallback(&s, 1, ES_FIX_ORDINARY);
        assert(s.count == 2 && s.notes[0].custom && !s.notes[2].custom && !s.notes[3].custom);
        assert(s.notes[5].custom && s.layers[s.notes[5].first] == 1);
        es_native_samples_free(&s);

        f = fixture();
        f.distinct = true;
        f.zone_count = 256;
        assert(prepare(&f, native, &context, false, true, &s, error, sizeof(error)));
        assert(s.count == 256 && s.notes[0].custom && s.notes[0].count == 256);
        assert(!s.notes[2].custom && !s.notes[5].custom && s.fixes[ES_FIX_ORDINARY].count == 2);
        es_native_samples_free(&s);
    }
}

static uint32_t little32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static double amplitude(const EsNativeRegion *r, double frequency) {
    double re = 0, im = 0;
    const uint8_t *data = r->audio.data + 112;
    /* Exclude filter boundary transients. */
    unsigned first = 1000, last = r->frames - 1000;

    for (unsigned i = first; i < last; i++) {
        int16_t value = (int16_t)(data[i * 2] | (unsigned)data[i * 2 + 1] << 8);
        double angle = 6.2831853071795864769 * frequency * i / r->rate;
        re += value / 32768.0 * cos(angle);
        im += value / 32768.0 * sin(angle);
    }

    return 2 * sqrt(re * re + im * im) / (last - first);
}

static void choke_groups(void) {
    EsNativeRegion regions[3] = {{.preset_scope = 1, .params = {.cut_by = 1}},
                                 {.preset_scope = 1, .params = {.cut_group = 1}},
                                 {.preset_scope = 2, .params = {.cut_by = 1}}};
    EsNativeSamples samples = {.regions = regions, .count = 3};
    uint64_t ends[] = {100, 100, 100, 30, 100, 40};
    EsNativeChokeNote notes[] = {{0, &ends[0], 0, 0},  {0, &ends[1], 1, 0},  {0, &ends[2], 0, 2},
                                 {10, &ends[3], 0, 1}, {10, &ends[4], 0, 0}, {20, &ends[5], 0, 1}};
    assert(es_native_choke(&samples, notes, 6, NULL));
    assert(ends[0] == 10 && ends[1] == 100 && ends[2] == 100 && ends[4] == 20);
    Fixture f = fixture();
    f.cancelled = true;
    DmeHost callbacks = host(&f);
    assert(!es_native_choke(&samples, notes, 6, &callbacks));

    enum { SIMULTANEOUS = 20000 };

    EsNativeChokeNote *many = calloc(SIMULTANEOUS + 1, sizeof(*many));
    uint64_t *many_ends = malloc((SIMULTANEOUS + 1) * sizeof(*many_ends));
    assert(many && many_ends);
    regions[0].params.cut_group = 1;

    for (unsigned i = 0; i <= SIMULTANEOUS; i++) {
        many_ends[i] = 100;
        many[i] = (EsNativeChokeNote){i == SIMULTANEOUS ? 10 : 0, &many_ends[i], 0, 0};
    }

    assert(es_native_choke(&samples, many, SIMULTANEOUS + 1, NULL));

    for (unsigned i = 0; i < SIMULTANEOUS; i++) {
        assert(many_ends[i] == 10);
    }

    assert(many_ends[SIMULTANEOUS] == 100);
    free(many_ends);
    free(many);
}

/* Drum entries follow playback's key tracking: percussion presets default to 0%. Only melodic
   use of other tracking values is reported. */
static void drum_tracking(void) {
    EsNativeRegion r = {.percussion = true, .params = {.root = 40}};
    assert(es_native_drum_key(&r, 50) == 40);
    r.params.tone.custom_tracking = 1;
    r.params.tone.tracking = 50;
    assert(es_native_drum_key(&r, 50) == 45);
    r = (EsNativeRegion){.params = {.root = 40}};
    assert(es_native_drum_key(&r, 50) == 50);

    DmeEvent events[] = {{0, 1, MIDI_NOTE_ON, 60, 100, 0}, {96, 2, MIDI_NOTE_OFF, 60, 0, 0}};
    DmeContext context = {.events = events, .event_count = 2, .duration = 192, .ppq = 96};
    char error[2048] = {0};

    for (unsigned drums = 0; drums < 2; drums++) {
        Fixture f = fixture();
        f.zones[0].params.tone = (DmeSampleToneParams){.custom_tracking = 1, .tracking = 50};
        context.percussion_channels = drums;
        EsNativeSamples s = {0};
        assert(prepare(&f, ES_NATIVE_SM64, &context, false, true, &s, error, sizeof(error)));
        assert(s.count == 1 && s.regions[0].percussion == (drums != 0));
        assert(!(s.regions[0].fixes & (1u << ES_FIX_TRACKING)) == (drums != 0));
        es_native_samples_free(&s);
    }
}

/* One trial's results: an over-rate region is resampled, a Replace is added first, and a full
   bank drops the whole selection. */
static void fitting(void) {
    DmeEvent events[] = {{0, 1, MIDI_NOTE_ON, 60, 100, 0}, {96, 2, MIDI_NOTE_OFF, 60, 0, 0}};
    DmeContext context = {.events = events, .event_count = 2, .duration = 192, .ppq = 96};
    char error[2048] = {0};
    Fixture f = fixture();
    f.asset.frames = 4096;
    EsNativeSamples s = {0};
    assert(prepare(&f, ES_NATIVE_OOT, &context, true, true, &s, error, sizeof(error)));
    assert(s.count == 1 && s.regions[0].target);
    double overspeed[ES_NATIVE_REGIONS] = {2}, tuning = s.regions[0].tuning;
    bool capacity[ES_NATIVE_REGIONS] = {0}, other = true;
    DmeHost callbacks = host(&f);
    assert(es_native_fit(&s, overspeed, capacity, ES_NATIVE_OOT, &other, &callbacks, error,
                         sizeof(error)) == ES_FIT_RETRY);
    assert(!other && !s.regions[0].target && s.regions[0].tuning < tuning / 2 &&
           s.regions[0].rate < 16001);
    overspeed[0] = 0;
    assert(es_native_fit(&s, overspeed, capacity, ES_NATIVE_OOT, &other, &callbacks, error,
                         sizeof(error)) == ES_FIT_DONE);
    capacity[0] = true;
    assert(es_native_fit(&s, NULL, capacity, ES_NATIVE_OOT, &other, &callbacks, error,
                         sizeof(error)) == ES_FIT_RETRY);
    assert(!s.count && !s.notes[0].custom && s.fixes[ES_FIX_ORDINARY].count == 1);
    es_native_samples_free(&s);
}

static double tone_level(const float *pcm, unsigned first, unsigned last, double frequency,
                         unsigned rate) {
    double re = 0, im = 0;

    for (unsigned i = first; i < last; i++) {
        double angle = 6.2831853071795864769 * frequency * i / rate;
        re += pcm[i] * cos(angle);
        im += pcm[i] * sin(angle);
    }

    return 2 * sqrt(re * re + im * im) / (last - first);
}

/* Baked low-pass attenuation, a seamless loop after filter and chorus, and chorus left out of
   loops too short for its delay. */
static void baking(void) {
    enum { RATE = 32000, FRAMES = 32000 };

    static float pcm[FRAMES];
    const double low = 250, high = 8000;
    DmeSampleToneParams tone = {.cutoff = 1000};
    uint32_t fixes = 0;

    for (unsigned i = 0; i < FRAMES; i++) {
        double angle = 6.2831853071795864769 * i / RATE;
        pcm[i] = (float)(.25 * sin(angle * low) + .25 * sin(angle * high));
    }

    assert(es_native_bake(pcm, FRAMES, RATE, 0, 0, &tone, &fixes) && !fixes);
    double passed = tone_level(pcm, 1000, FRAMES, low, RATE);
    double stopped = tone_level(pcm, 1000, FRAMES, high, RATE);
    /* A second-order low-pass at 1 kHz passes 250 Hz near unity and cuts 8 kHz by about 36 dB. */
    assert(passed > .25 && passed < .32 && stopped < .25 * .03);

    /* 20 cycles of 400 Hz fit the 1600-frame loop exactly. */
    for (unsigned i = 0; i < FRAMES; i++) {
        pcm[i] = (float)(.5 * sin(6.2831853071795864769 * 400 * i / RATE));
    }

    tone = (DmeSampleToneParams){.cutoff = 3000, .resonance = 6, .chorus = .5};
    assert(es_native_bake(pcm, FRAMES, RATE, 8000, 9600, &tone, &fixes) && !fixes);
    double largest = 0;

    for (unsigned i = 8001; i < 9600; i++) {
        largest = fmax(largest, fabs(pcm[i] - pcm[i - 1]));
    }

    assert(fabs(pcm[8000] - pcm[9599]) <= largest * 1.01);

    tone = (DmeSampleToneParams){.chorus = .5};
    assert(es_native_bake(pcm, FRAMES, RATE, 8000, 8200, &tone, &fixes));
    assert(fixes == 1u << ES_FIX_CHORUS_SHORT);
}

/* Added samples take their preset's name as a safe file/symbol name, with a region number for
   layered presets and a short identity suffix. */
static void naming(void) {
    DmeEvent events[] = {{0, 1, MIDI_NOTE_ON, 60, 100, 0}, {96, 2, MIDI_NOTE_OFF, 60, 0, 0}};
    DmeContext context = {.events = events, .event_count = 2, .duration = 192, .ppq = 96};
    const char *names[] = {"E.PIAN2 C4", "808 kick", "..."};
    const char *expected[] = {"E_PIAN2_C4_", "s_808_kick_", "sample_"};
    char error[2048] = {0};

    for (unsigned i = 0; i < 3; i++) {
        Fixture f = fixture();
        f.preset.name = (DmeString){names[i], strlen(names[i])};
        f.zone_count = i == 0 ? 2 : 1;
        EsNativeSamples s = {0};
        assert(prepare(&f, ES_NATIVE_SM64, &context, false, true, &s, error, sizeof(error)));
        size_t prefix = strlen(expected[i]);
        const char *name = s.regions[0].name;

        if (i == 0) {
            assert(!strncmp(name, "E_PIAN2_C4_1_", 13) && strlen(name) == 19);
            assert(!strncmp(s.regions[1].name, "E_PIAN2_C4_2_", 13));
        } else {
            assert(!strncmp(name, expected[i], prefix) && strlen(name) == prefix + 6);
        }

        es_native_samples_free(&s);
    }
}

/* Source rows name the channel and instrument; a custom sample replaces the instrument name. */
static void source_labels(void) {
    DmeEvent events[] = {{0, 1, MIDI_PROGRAM | 1, 4, 0, 1},
                         {0, 2, MIDI_NOTE_ON, 60, 100, 0},
                         {0, 3, MIDI_NOTE_ON | 1, 62, 100, 1}};
    DmeEffectivePatch patches[] = {{0}, {0, 0, 0, 0, 100, 2}, {0, 4, 0, 1, 100, 3}};
    DmeSamplePreset preset = {.size = sizeof(preset), .name = str("Duck bell")};
    DmeContext context = {.events = events,
                          .event_count = 3,
                          .duration = 96,
                          .ppq = 96,
                          .effective_patches = patches,
                          .sample_presets = &preset,
                          .sample_preset_count = 1};
    context.program_names[0] = "Acoustic Grand Piano";
    context.program_names[4] = "Electric Piano 1";
    char error[256] = {0};
    EsMappings *mappings = es_mappings_create(1, 1);
    assert(mappings && es_mappings_inspect(mappings, &context, NULL, 2, error, sizeof(error)));
    assert(es_mappings_count(mappings) == 2);
    assert(!strcmp(es_mappings_source(mappings, 0)->label, "Ch 1: Acoustic Grand Piano / GM 1"));
    assert(!strcmp(es_mappings_source(mappings, 1)->label, "Ch 2: Duck bell / GM 5"));
    es_mappings_destroy(mappings);
}

static double curve_db(int value) {
    return fmax(-40, 20 * log10((value > 0 ? value : 1) / 127.0));
}

/* Curve simplification: audible caps, exact landings, fixed points and densest-first. */
static void curves(void) {
    enum { DENSE = 1000, SPARSE = 20 };

    EsCurvePoint points[DENSE + SPARSE + 1];
    bool keep[DENSE + SPARSE + 1];
    size_t n = 0;

    /* A dense fade from 127 to 20, one change per tick, then a hold. */
    for (unsigned i = 0; i < DENSE; i++) {
        points[n++] = (EsCurvePoint){i, 127 - (int)(i * 107 / (DENSE - 1)), 1, ES_CURVE_VOLUME};
    }

    /* A short vibrato-like bend, 5 cents apart, and an instrument change that must stay. */
    for (unsigned i = 0; i < SPARSE; i++) {
        points[n++] = (EsCurvePoint){DENSE + i * 10, (int)(i % 2) * 5, 2, ES_CURVE_BEND};
    }

    points[n++] = (EsCurvePoint){5, 3, 3, ES_CURVE_FIXED};
    size_t all = es_thin_curves(points, n, n, 0, 24, keep);
    assert(all == 108 + SPARSE + 1 && keep[n - 1]);
    size_t least = es_thin_curves(points, n, 0, 0, 24, keep);
    assert(least < all / 2 && keep[n - 1] && keep[DENSE - 1]);
    int held = -1;

    for (unsigned i = 0; i < DENSE; i++) {
        held = keep[i] ? points[i].value : held;
        assert(fabs(curve_db(held) - curve_db(points[i].value)) <= 1.0 + 1e-9);
    }

    /* The dense curve loosens first: a budget the fade alone can meet leaves the bend exact
       (at its cap, the bend keeps only its first and last point). */
    es_thin_curves(points, n, least + SPARSE - 2, 0, 24, keep);

    for (unsigned i = DENSE; i < DENSE + SPARSE; i++) {
        assert(keep[i]);
    }

    puts("PASS curve simplification bounds, landings and densest-first");
}

int main(void) {
    curves();
    source_labels();
    choke_groups();
    naming();
    baking();
    drum_tracking();
    fitting();
    preparation();
    replace_and_drums();
    row_fallbacks();
    EsNativeRegion r = {
        .rate = 32000, .frames = 32000, .loop_start = 8000, .loop_end = 24000, .tuning = 1};
    r.audio.size = 112 + r.frames * 2;
    r.audio.data = calloc(1, r.audio.size);
    assert(r.audio.data);

    for (unsigned i = 0; i < r.frames; i++) {
        double angle = 6.2831853071795864769 * i / r.rate;
        int16_t value = (int16_t)lround(32768 * (.2 * sin(angle * 440) + .1 * sin(angle * 12000)));
        r.audio.data[112 + i * 2] = (uint8_t)value;
        r.audio.data[113 + i * 2] = (uint8_t)((uint16_t)value >> 8);
    }

    char error[2048] = {0};
    assert(es_native_resample(&r, .5, ES_NATIVE_OOT, NULL, error, sizeof(error)) == ES_RESAMPLE_OK);
    assert(r.rate == 16000 && r.frames == 16000 && r.loop_start == 4000 && r.loop_end == 12000);
    assert(r.tuning == .5 && little32(r.audio.data + 24) == 16000 &&
           little32(r.audio.data + 28) == 32000 && little32(r.audio.data + 88) == 4000 &&
           little32(r.audio.data + 92) == 12000 && little32(r.audio.data + 108) == 32000);
    double fundamental = amplitude(&r, 440), alias = amplitude(&r, 4000);
    assert(fundamental > .199 && fundamental < .201 && alias < .0001);
    uint8_t hash[32], repeated[32];
    es_sha256(r.audio.data, r.audio.size, hash);
    assert(es_native_resample(&r, .5, ES_NATIVE_OOT, NULL, error, sizeof(error)) == ES_RESAMPLE_OK);
    es_sha256(r.audio.data, r.audio.size, repeated);
    assert(!memcmp(hash, repeated, sizeof(hash)));
    assert(es_native_resample(&r, .0001, ES_NATIVE_OOT, NULL, error, sizeof(error)) ==
               ES_RESAMPLE_LIMIT &&
           !es_native_resample_fits(&r, .0001));
    assert(!es_native_resample_fits(&r, 0) && !es_native_resample_fits(&r, NAN) &&
           !es_native_resample_fits(&r, 1e-100));
    es_sha256(r.audio.data, r.audio.size, repeated);
    assert(!memcmp(hash, repeated, sizeof(hash)));
    es_file_free(&r.audio);

    /* A full-scale square wave makes the low-pass overshoot. Normalize the whole output. */
    r = (EsNativeRegion){.rate = 32000, .frames = 4096, .tuning = 1};
    r.audio.size = 44 + r.frames * 2;
    r.audio.data = calloc(1, r.audio.size);
    assert(r.audio.data);

    for (unsigned i = 0; i < r.frames; i++) {
        int16_t value = (i / 64) & 1 ? 32767 : -32767;
        r.audio.data[44 + i * 2] = (uint8_t)value;
        r.audio.data[45 + i * 2] = (uint8_t)((uint16_t)value >> 8);
    }

    assert(es_native_resample(&r, .5, ES_NATIVE_OOT, NULL, error, sizeof(error)) == ES_RESAMPLE_OK);
    assert(r.fixes & (1u << ES_FIX_GAIN));

    for (unsigned i = 0; i < r.frames; i++) {
        int16_t value =
            (int16_t)(r.audio.data[44 + i * 2] | (unsigned)r.audio.data[45 + i * 2] << 8);
        assert(value >= -32766 && value <= 32766);
    }

    es_file_free(&r.audio);
    return 0;
}
