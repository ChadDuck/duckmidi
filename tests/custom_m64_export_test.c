#ifdef CUSTOM_TEST_OOT
#include "../libraries/ootexport/plugin/oot_plugin.h"
#define M64_MEMORY_STOCK 1
#else
#include "../libraries/m64export/plugin/m64_plugin.h"
#endif
#include "../include/duckmidi_repository_plugin.h"
#include "../src/text/en.h"
#include "native_sequence_decode.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t);

static DmeSampleZone zones[256];
static DmeSampleAsset asset;
static DmeSamplePreset preset;
static unsigned freezes, outputs;
static uint32_t frozen_zones = 2;
static char bank_output[256000];
static char sequence_output[256000];
static size_t sequence_size;

static DmeString str(const char *s) {
    return (DmeString){s, strlen(s)};
}

static int32_t DME_CALL cancel(void *user) {
    (void)user;
    return 0;
}

static void DME_CALL progress(void *user, uint32_t percent) {
    (void)user;
    (void)percent;
}

static char warnings[16384];

static void DME_CALL diagnostic(void *user, uint32_t severity, uint32_t flags, const char *text) {
    (void)user;
    (void)flags;

    if (severity == DME_ERROR) {
        fprintf(stderr, "%s\n", text);
    }

    if (severity == DME_WARNING && strlen(warnings) + strlen(text) + 2 < sizeof(warnings)) {
        strcat(warnings, text);
        strcat(warnings, "\n");
    }
}

static int32_t DME_CALL output(void *user, const char *name, const uint8_t *data, uint64_t size) {
    (void)user;
    assert(data && size);
    outputs++;
    if (strstr(name, ".seq") || strstr(name, ".m64")) {
        assert(size < sizeof(sequence_output));
        memcpy(sequence_output, data, (size_t)size);
        sequence_output[size] = 0;
        sequence_size = (size_t)size;
    }

    if ((strstr(name, ".json") && !strstr(name, "manifest") && !strstr(name, "entry")) ||
        strstr(name, "Soundfont_")) {
        assert(size < sizeof(bank_output));
        memcpy(bank_output, data, (size_t)size);
        bank_output[size] = 0;
    }

    if (strstr(name, ".aiff")) {
        assert(size > 124 && size <= 124 + (512 - 16) * 2);
        assert(!memcmp(data, "FORM", 4));
        assert(!memcmp(data + 8, "AIFF", 4));
        assert(!memcmp(data + 108, "SSND", 4));
    }

    if (strstr(name, ".wav")) {
        assert(size > 112 && size <= 112 + (512 - 16) * 2);
        assert(!memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4));
    }

    return 1;
}

static int32_t DME_CALL freeze(void *user, const DmeSampleSelection *selection,
                               DmeFrozenSample *out, char *error, uint64_t cap) {
    (void)user;
    (void)error;
    (void)cap;
    assert(selection->generation == 7);
    assert(!strcmp(selection->preset_id.data, "preset") ||
           !strcmp(selection->preset_id.data, "second"));
    freezes++;
    *out = (DmeFrozenSample){.size = sizeof(*out),
                             .preset_count = 1,
                             .zone_count = frozen_zones,
                             .asset_count = 1,
                             .handle = 41,
                             .presets = &preset,
                             .zones = zones,
                             .assets = &asset};
    return DME_OK;
}

static int32_t DME_CALL pcm(void *user, uint64_t handle, uint32_t index, uint64_t start,
                            uint64_t frames, float *data, uint64_t capacity) {
    (void)user;
    assert(handle == 41 && !index && frames * 2 <= capacity && start + frames <= 512);

    for (uint64_t i = 0; i < frames; i++) {
        data[i * 2] = (float)(sin((start + i) * .1) * .3);
        data[i * 2 + 1] = data[i * 2] * .5f;
    }

    return DME_OK;
}

static void check(int32_t status, const char *error) {
    if (status != DME_OK) {
        fprintf(stderr, "status %d: %s\n", status, error);
        abort();
    }
}

static void independent_stock_rows(const DmeExporter *api) {
    void *instance = api->create();
    assert(instance);
    DmeEvent events[] = {{0, 1, MIDI_NOTE_ON, 60, 90, 0},
                         {96, 2, MIDI_NOTE_OFF, 60, 0, 0},
                         {96, 3, MIDI_NOTE_ON, 62, 90, 0},
                         {192, 4, MIDI_NOTE_OFF, 62, 0, 0}};
    DmeEffectivePatch patches[] = {
        {.program = 0}, {.program = 0}, {.bank = 1, .program = 0}, {.bank = 1, .program = 0}};
    DmeContext context = {.events = events,
                          .event_count = 4,
                          .duration = 192,
                          .ppq = 96,
                          .effective_patches = patches,
                          .percussion_channels = 1u << 9};
    char error[4096] = {0};
    check(api->inspect(instance, &context, error, sizeof(error)), error);
    DmeUi ui;
    check(api->describe(instance, &ui), error);
    const DmeField *mapping = NULL;

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING) {
            mapping = &ui.fields[i];
        }
    }

    assert(mapping && mapping->row_count == 2);
    char ids[2][64];

    for (unsigned i = 0; i < 2; i++) {
        snprintf(ids[i], sizeof(ids[i]), "%s", mapping->rows[i].id);
    }

    int first = mapping->rows[0].value;
    int second = first;

    for (unsigned i = 0; i < mapping->choice_count; i++) {
        int value = mapping->choices[i].value;
#ifdef CUSTOM_TEST_OOT
        if (value >= 0 && (unsigned)value < oot_instrument_count &&
            !strcmp(oot_instruments[value].name, "Flute")) {
            second = value;
            break;
        }
#else
        if (value == m64_canonical_preset(false, 73)) {
            second = value;
            break;
        }
#endif
    }

    assert(first != second);
    check(api->set(instance, ids[1], &(DmeValue){DME_CHOICE, second}, error, sizeof(error)), error);
    check(api->describe(instance, &ui), error);

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING) {
            assert(ui.fields[i].rows[0].value == first && ui.fields[i].rows[1].value == second);
        }
    }

    DmeHost host = {
        .cancelled = cancel, .progress = progress, .diagnostic = diagnostic, .artifact = output};
    void *job = api->create_job(instance, error, sizeof(error));
    assert(job);
    bank_output[0] = 0;
    check(api->run(job, &context, &host, error, sizeof(error)), error);
#ifdef CUSTOM_TEST_OOT
    char name[64];
    snprintf(name, sizeof(name), "SAMPLE_0_%u", oot_instruments[second].regions[0].sample);
    assert(strstr(bank_output, name));
#else
    assert(strstr(bank_output, m64_get_api_3()->default_program((unsigned)second)->sample));
    assert(strstr(bank_output, m64_get_api_3()->default_program((unsigned)first)->sample));
#endif
    api->destroy_job(job);
    api->destroy(instance);
    puts("PASS independent original-bank stock mappings through native conversion");
}

static void controller_rate_limit(const DmeExporter *api, DmeSampleCatalog *catalog) {
    void *instance = api->create();
    assert(instance);
    DmeEvent events[] = {{0, 1, MIDI_NOTE_ON, 60, 90, 0},
                         {48, 2, MIDI_PITCH_BEND, 127, 127, 0},
                         {96, 3, MIDI_NOTE_OFF, 60, 0, 0}};
    DmeEffectivePatch patches[3] = {{0}};
    DmeContext context = {.events = events,
                          .event_count = 3,
                          .duration = 192,
                          .ppq = 96,
                          .effective_patches = patches,
                          .library = catalog};
    char error[4096] = {0};
    check(api->inspect(instance, &context, error, sizeof(error)), error);
    DmeUi ui;
    check(api->describe(instance, &ui), error);
    bool selected = false;

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING && ui.fields[i].row_count) {
            const DmeMappingRow *row = &ui.fields[i].rows[0];
            DmeCustomSelection choice = row->custom->selection;
            choice.enabled = true;
            choice.sample.preset_id = str("preset");
            check(api->set_custom(instance, row->id, &choice, error, sizeof(error)), error);
            selected = true;
            break;
        }
    }

    assert(selected);
    DmeHost host = {.cancelled = cancel,
                    .progress = progress,
                    .diagnostic = diagnostic,
                    .artifact = output,
                    .freeze_sample = freeze,
                    .read_frozen_sample = pcm};
    zones[0].params.root = 37;
    zones[1].params.root = 49;
    void *job = api->create_job(instance, error, sizeof(error));
    assert(job);
    unsigned before = outputs;
    /* The bend pushes the added sample past 4x; its rate is lowered instead of failing. */
    check(api->preflight(job, &context, &host, error, sizeof(error)), error);
    assert(outputs == before);
    api->destroy_job(job);

    /* A rate below the resampler's floor restores the ordinary mapping for the whole preset. */
    zones[0].params.tone.coarse = zones[1].params.tone.coarse = 120;
    warnings[0] = bank_output[0] = 0;
    job = api->create_job(instance, error, sizeof(error));
    assert(job);
    check(api->preflight(job, &context, &host, error, sizeof(error)), error);
    assert(strstr(warnings, DM_TEXT_NATIVE_FIX_ORDINARY_LABEL) && outputs == before);
    check(api->run(job, &context, &host, error, sizeof(error)), error);
    assert(!strstr(bank_output, "Validation_tone"));
    api->destroy_job(job);
    zones[0].params.tone.coarse = zones[1].params.tone.coarse = 0;

    /* A package without an inspected replacement target carries the custom audio as Add. */
    check(api->describe(instance, &ui), error);

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING && ui.fields[i].row_count) {
            const DmeMappingRow *row = &ui.fields[i].rows[0];
            DmeCustomSelection choice = row->custom->selection;
            choice.operation = DME_SAMPLE_REPLACE;
            choice.physical_target = str("unavailable-target");
            choice.region_set = true;
            choice.replace_zone = 0;
            check(api->set_custom(instance, row->id, &choice, error, sizeof(error)), error);
            break;
        }
    }

    warnings[0] = bank_output[0] = 0;
    job = api->create_job(instance, error, sizeof(error));
    assert(job);
    check(api->preflight(job, &context, &host, error, sizeof(error)), error);
    assert(strstr(warnings, DM_TEXT_NATIVE_FIX_REPLACE_ADD_LABEL));
    check(api->run(job, &context, &host, error, sizeof(error)), error);
    assert(strstr(bank_output, "Validation_tone"));
    api->destroy_job(job);
    before = outputs;
#ifndef CUSTOM_TEST_OOT
    check(api->set(instance, "target", &(DmeValue){DME_CHOICE, M64_GAME_COOPDX}, error,
                   sizeof(error)),
          error);
    job = api->create_job(instance, error, sizeof(error));
    assert(job);
    warnings[0] = 0;
    check(api->preflight(job, &context, &host, error, sizeof(error)), error);
    assert(strstr(warnings, DM_TEXT_NATIVE_FIX_IGNORED_LABEL) && outputs == before);
    api->destroy_job(job);
#else
    check(api->describe(instance, &ui), error);

    for (unsigned i = 0; i < ui.count; i++) {
        if (!strcmp(ui.fields[i].id, "soundfont")) {
            assert(ui.fields[i].choice_count > 1);
            check(api->set(instance, "soundfont",
                           &(DmeValue){DME_CHOICE, ui.fields[i].choices[1].value}, error,
                           sizeof(error)),
                  error);
            break;
        }
    }

    /* The UI resets mappings when Sample Set changes; exercise the plugin boundary explicitly. */
    check(api->describe(instance, &ui), error);

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING && ui.fields[i].row_count) {
            const DmeMappingRow *row = &ui.fields[i].rows[0];
            DmeCustomSelection choice = row->custom->selection;
            choice.enabled = true;
            choice.sample.preset_id = str("preset");
            check(api->set_custom(instance, row->id, &choice, error, sizeof(error)), error);
            break;
        }
    }

    warnings[0] = 0;
    job = api->create_job(instance, error, sizeof(error));
    assert(job);
    check(api->preflight(job, &context, &host, error, sizeof(error)), error);
    assert(strstr(warnings, DM_TEXT_NATIVE_FIX_IGNORED_LABEL) && outputs == before);
    api->destroy_job(job);
#endif
    api->destroy(instance);
    puts("PASS custom sample rate limits lower the added sample rate instead of failing");
}

static bool coop_scope(const DmeRepositoryView *view) {
    for (unsigned i = 0; i < view->scope_count; i++) {
        if (view->scopes[i].id == view->selected_scope &&
            !strcmp(view->scopes[i].target.data, "sm64coopdx")) {
            assert(!(view->scopes[i].capabilities & DME_REPOSITORY_CUSTOM) && !view->target_count);
            return true;
        }
    }

    return false;
}

static NativeDecodedSequence decoded;

static void decode(void) {
#ifdef CUSTOM_TEST_OOT
    native_decode_oot(sequence_output, &decoded);
#else
    native_decode_m64((const uint8_t *)sequence_output, sequence_size, &decoded);
#endif
}

static const NativeDecodedNote *decoded_note(unsigned channel, unsigned key) {
    for (size_t i = 0; i < decoded.count; i++) {
        if (decoded.notes[i].channel == channel && decoded.notes[i].key == key) {
            return &decoded.notes[i];
        }
    }

    assert(!"missing decoded note");
    return NULL;
}

static void choke_export(const DmeExporter *api, const DmeSampleCatalog *source) {
    DmeSampleZone saved[2] = {zones[0], zones[1]};

    for (unsigned i = 0; i < 2; i++) {
        zones[i].params.root = 60;
        zones[i].params.loop_mode = DME_SAMPLE_LOOP_SUSTAIN;
        zones[i].params.key_lo = i ? 61 : 0;
        zones[i].params.key_hi = i ? 127 : 60;
        zones[i].params.cut_group = i ? 1 : 0;
        zones[i].params.cut_by = i ? 0 : 1;
    }

    DmeSampleZoneParams params[4] = {zones[0].params, zones[1].params, zones[0].params,
                                     zones[1].params};
    DmeSampleCatalogPreset entries[2] = {source->presets[0], source->presets[0]};
    entries[1].id = str("second");
    entries[1].first_zone = 2;
    DmeSampleCatalogSet set = source->sets[0];
    set.preset_count = 2;
    DmeSampleCatalog catalog = {.size = sizeof(catalog),
                                .set_count = 1,
                                .preset_count = 2,
                                .zone_count = 4,
                                .sets = &set,
                                .presets = entries,
                                .zones = params};
    DmeEvent events[] = {{0, 1, MIDI_CONTROL, MIDI_CC_SUSTAIN, 127, 0},
                         {0, 2, MIDI_NOTE_ON, 60, 100, 0},
                         {0, 3, MIDI_NOTE_ON, 59, 100, 0},
                         {0, 4, MIDI_NOTE_ON | 1, 60, 100, 0},
                         {48, 5, MIDI_NOTE_OFF, 60, 0, 0},
                         {96, 6, MIDI_NOTE_ON, 61, 100, 0},
                         {192, 7, MIDI_NOTE_OFF, 61, 0, 0},
                         {384, 8, MIDI_CONTROL, MIDI_CC_SUSTAIN, 0, 0},
                         {384, 9, MIDI_NOTE_OFF, 59, 0, 0},
                         {384, 10, MIDI_NOTE_OFF | 1, 60, 0, 0}};
    DmeEffectivePatch patches[10] = {{0}};
    patches[2].program = patches[8].program = 1;
    DmeContext context = {.events = events,
                          .event_count = 10,
                          .duration = 480,
                          .ppq = 96,
                          .library = &catalog,
                          .effective_patches = patches};
    char error[4096] = {0};
    void *instance = api->create();
    assert(instance);
    check(api->inspect(instance, &context, error, sizeof(error)), error);
    DmeUi ui;
    check(api->describe(instance, &ui), error);

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING && ui.fields[i].row_count) {
            assert(ui.fields[i].row_count == 3);
            char ids[3][64];
            DmeCustomSelection choices[3];

            for (unsigned j = 0; j < 3; j++) {
                const DmeMappingRow *row = &ui.fields[i].rows[j];
                snprintf(ids[j], sizeof(ids[j]), "%s", row->id);
                choices[j] = row->custom->selection;
                choices[j].enabled = true;
                choices[j].sample.preset_id = entries[row->source.program == 1].id;
            }

            for (unsigned j = 0; j < 3; j++) {
                check(api->set_custom(instance, ids[j], &choices[j], error, sizeof(error)), error);
            }

            break;
        }
    }

    DmeHost host = {.cancelled = cancel,
                    .progress = progress,
                    .diagnostic = diagnostic,
                    .artifact = output,
                    .freeze_sample = freeze,
                    .read_frozen_sample = pcm};
    warnings[0] = 0;
    void *job = api->create_job(instance, error, sizeof(error));
    assert(job);
    check(api->run(job, &context, &host, error, sizeof(error)), error);
    decode();
    assert(decoded.count == 4);
    assert(decoded_note(0, 60)->end == 48);
    assert(decoded_note(0, 59)->end == 192 && decoded_note(1, 60)->end == 192);
    assert(strstr(warnings, DM_TEXT_NATIVE_FIX_CHOKE_LABEL) &&
           strstr(warnings, DM_TEXT_NATIVE_FIX_SUSTAIN_LOOP_LABEL));
    api->destroy_job(job);
    api->destroy(instance);
    zones[0] = saved[0];
    zones[1] = saved[1];
    puts("PASS decoded choke groups, sustain pedal, saved-preset and channel scope");
}

static void select_pan_preset(const DmeExporter *api, void *instance, bool drums, char *error,
                              size_t cap) {
    if (drums) {
        check(api->set(instance, "percussion", &(DmeValue){DME_BOOL, 1}, error, cap), error);
    }

    DmeUi ui;
    check(api->describe(instance, &ui), error);

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING && ui.fields[i].row_count) {
            char ids[16][64];
            DmeCustomSelection choices[16];
            unsigned count = 0;

            for (unsigned j = 0; j < ui.fields[i].row_count; j++) {
                const DmeMappingRow *row = &ui.fields[i].rows[j];

                if (!drums && row->source.program != 0) {
                    continue;
                }

                assert(count < 16);
                snprintf(ids[count], sizeof(ids[count]), "%s", row->id);
                choices[count] = row->custom->selection;
                choices[count].enabled = true;
                choices[count++].sample.preset_id = str("preset");
            }

            assert(count);

            for (unsigned j = 0; j < count; j++) {
                check(api->set_custom(instance, ids[j], &choices[j], error, cap), error);
            }

            return;
        }
    }

    assert(!"missing pan mapping rows");
}

static void pan_export(const DmeExporter *api, const DmeSampleCatalog *source) {
    DmeSampleZone saved[2] = {zones[0], zones[1]};

    for (unsigned drums = 0; drums < 2; drums++) {
        for (unsigned z = 0; z < 2; z++) {
            zones[z].params.root = 60;
            zones[z].params.pan = z ? .5 : -.5;
            zones[z].params.key_lo = z ? (drums ? 43 : 61) : 0;
            zones[z].params.key_hi = z ? 127 : (drums ? 42 : 60);
        }

        DmeSampleZoneParams params[2] = {zones[0].params, zones[1].params};
        DmeSampleCatalogPreset entry = source->presets[0];
        entry.flags = preset.flags = drums ? DME_SAMPLE_PERCUSSION : 0;
        DmeSampleCatalog catalog = *source;
        catalog.presets = &entry;
        catalog.zones = params;
        DmeHost host = {.cancelled = cancel,
                        .progress = progress,
                        .diagnostic = diagnostic,
                        .artifact = output,
                        .freeze_sample = freeze,
                        .read_frozen_sample = pcm};

        for (unsigned mode = 0; mode < 5; mode++) {
            DmeEvent events[66] = {{0}};
            DmeEffectivePatch patches[66] = {{0}};
            unsigned count = 0, notes = drums ? 2 : 32;

            if (mode == 1 || mode == 2 || mode == 4) {
                events[count++] = (DmeEvent){0,
                                             1,
                                             MIDI_CONTROL | (drums ? MIDI_DRUM_CHANNEL : 0),
                                             MIDI_CC_PAN,
                                             mode == 4 ? 32 : 64,
                                             0,
                                             mode == 1 ? DME_EVENT_DEFAULT_CONTROL : 0};
            }

            for (unsigned i = 0; i < notes; i++) {
                unsigned key = drums ? 42 + i : 60 + i % 4;
                unsigned channel = drums ? MIDI_DRUM_CHANNEL : 0;
                DmeEffectivePatch patch = {.program = !drums && i % 4 >= 2 ? 1 : 0,
                                           .flags = drums ? DME_SAMPLE_PERCUSSION : 0};
                patches[count] = patches[count + 1] = patch;
                events[count] = (DmeEvent){i * 96, count + 1, MIDI_NOTE_ON | channel, key, 100, 0};
                count++;
                events[count] =
                    (DmeEvent){i * 96 + 48, count + 1, MIDI_NOTE_OFF | channel, key, 0, 0};
                count++;
            }

            if (mode == 3) {
                events[count] = (DmeEvent){
                    notes * 96,  count + 1, MIDI_CONTROL | (drums ? MIDI_DRUM_CHANNEL : 0),
                    MIDI_CC_PAN, 127,       0};
                count++;
            }

            DmeContext context = {.events = events,
                                  .event_count = count,
                                  .duration = (notes + 1) * 96,
                                  .ppq = 96,
                                  .library = &catalog,
                                  .effective_patches = patches,
                                  .percussion_channels = drums ? 1u << MIDI_DRUM_CHANNEL : 0};
            char error[4096] = {0};
            void *instance = api->create();
            assert(instance);
            check(api->inspect(instance, &context, error, sizeof(error)), error);
            select_pan_preset(api, instance, drums != 0, error, sizeof(error));
            void *job = api->create_job(instance, error, sizeof(error));
            assert(job);
            check(api->run(job, &context, &host, error, sizeof(error)), error);
            decode();
            assert(decoded.count == notes);

            for (size_t i = 0; i < decoded.count; i++) {
                const NativeDecodedNote *n = &decoded.notes[i];
                unsigned expected = mode >= 2 || drums ? 64
                                    : n->key == 60     ? 33
                                    : n->key == 61     ? 96
                                                       : 64;
                assert(n->pan == expected && n->weight == (mode >= 2 ? 128 : 0));
            }

            if (!drums && mode == 0) {
                assert(decoded.calls > 0);
            }

            if (drums) {
#ifdef CUSTOM_TEST_OOT
                assert(strstr(bank_output, "Pan=\"33\"") && strstr(bank_output, "Pan=\"96\""));
#else
                assert(strstr(bank_output, "\"pan\": 33") && strstr(bank_output, "\"pan\": 96"));
#endif
            }

            api->destroy_job(job);
            api->destroy(instance);
        }

        if (!drums) {
            DmeEvent events[12];
            DmeEffectivePatch patches[12] = {{0}};

            for (unsigned i = 0; i < 6; i++) {
                events[i] = (DmeEvent){0, i + 1, MIDI_NOTE_ON, 60 + i, 100, 0};
                events[i + 6] = (DmeEvent){96, i + 7, MIDI_NOTE_OFF, 60 + i, 0, 0};
            }

            DmeContext context = {.events = events,
                                  .event_count = 12,
                                  .duration = 192,
                                  .ppq = 96,
                                  .library = &catalog,
                                  .effective_patches = patches};
            char error[4096] = {0};
            void *instance = api->create();
            assert(instance);
            check(api->inspect(instance, &context, error, sizeof(error)), error);
            select_pan_preset(api, instance, false, error, sizeof(error));
            void *job = api->create_job(instance, error, sizeof(error));
            assert(job);
            check(api->run(job, &context, &host, error, sizeof(error)), error);
            decode();
            assert(decoded.count == 6);
            bool overflow = false;

            for (size_t i = 0; i < decoded.count; i++) {
                const NativeDecodedNote *n = &decoded.notes[i];
                assert(n->weight == 0 && n->pan == (n->key == 60 ? 33 : 96));
                overflow |= n->channel != 0;
            }

            assert(overflow);
            api->destroy_job(job);
            api->destroy(instance);
        }
    }

    preset.flags = 0;
    zones[0] = saved[0];
    zones[1] = saved[1];
    puts("PASS decoded melodic/drum pan, MIDI precedence, defaults and shared layer calls");
}

/* Values of one channel effect command on channel 0, in output order. */
static size_t decoded_values(unsigned op, unsigned values[8]) {
    size_t count = 0;
    bool first = true;

    for (size_t i = 0; i < decoded.control_count; i++) {
        const NativeDecodedControl *c = &decoded.controls[i];

        if (c->channel != 0 || c->op != op) {
            continue;
        }

#ifndef CUSTOM_TEST_OOT
        /* M64 channel scripts open with reverb 0, vibrato rate 8 and depth 0. */
        if (first) {
            first = false;
            assert(!c->tick && c->value == (op == 0xd7 ? 8u : 0u));
            continue;
        }
#endif
        first = false;
        assert(count < 8);
        values[count++] = c->value;
    }

    return count;
}

/* Preset reverb and vibrato become channel commands at note starts; ordinary notes return to the
   defaults, MIDI CC91 and CC1 keep their channels, and overlapping conflicts warn. */
static void effects_export(const DmeExporter *api, const DmeSampleCatalog *source) {
#ifdef CUSTOM_TEST_OOT
    const unsigned rate = 57; /* 5 Hz at 180 updates/s. */
#else
    const unsigned rate = 43; /* 5 Hz at 240 updates/s. */
#endif
    DmeSampleZone saved[2] = {zones[0], zones[1]};

    for (unsigned z = 0; z < 2; z++) {
        zones[z].params.root = 60;
        zones[z].params.pan = 0;
        zones[z].params.key_lo = z ? 61 : 0;
        zones[z].params.key_hi = z ? 127 : 60;
        zones[z].params.tone =
            z ? (DmeSampleToneParams){0}
              : (DmeSampleToneParams){.reverb = .5, .vibrato_rate = 5, .vibrato_depth = 50};
    }

    DmeSampleZoneParams params[2] = {zones[0].params, zones[1].params};
    DmeSampleCatalogPreset entry = source->presets[0];
    entry.flags = preset.flags = 0;
    DmeSampleCatalog catalog = *source;
    catalog.presets = &entry;
    catalog.zones = params;
    DmeHost host = {.cancelled = cancel,
                    .progress = progress,
                    .diagnostic = diagnostic,
                    .artifact = output,
                    .freeze_sample = freeze,
                    .read_frozen_sample = pcm};

    for (unsigned mode = 0; mode < 4; mode++) {
        /* Keys 60/61 use the custom zones; key 62 (program 1) keeps its stock instrument. */
        const unsigned keys[4] = {60, 61, 62, 60};
        DmeEvent events[9];
        DmeEffectivePatch patches[9] = {{0}};
        unsigned count = 0;

        if (mode == 1 || mode == 2) {
            events[count++] = (DmeEvent){0,
                                         1,
                                         MIDI_CONTROL,
                                         mode == 1 ? MIDI_CC_REVERB : MIDI_CC_MODULATION,
                                         mode == 1 ? 40 : 0,
                                         0};
        }

        if (mode == 3) {
            /* Keys 60 and 61 sound together but want different reverb and vibrato. */
            events[0] = (DmeEvent){0, 1, MIDI_NOTE_ON, 60, 100, 0};
            events[1] = (DmeEvent){0, 2, MIDI_NOTE_ON, 61, 100, 0};
            events[2] = (DmeEvent){48, 3, MIDI_NOTE_OFF, 60, 0, 0};
            events[3] = (DmeEvent){48, 4, MIDI_NOTE_OFF, 61, 0, 0};
            count = 4;
        }

        for (unsigned i = 0; mode != 3 && i < 4; i++) {
            patches[count] = patches[count + 1] = (DmeEffectivePatch){.program = keys[i] == 62};
            events[count] = (DmeEvent){i * 96, count + 1, MIDI_NOTE_ON, keys[i], 100, 0};
            count++;
            events[count] = (DmeEvent){i * 96 + 48, count + 1, MIDI_NOTE_OFF, keys[i], 0, 0};
            count++;
        }

        DmeContext context = {.events = events,
                              .event_count = count,
                              .duration = 480,
                              .ppq = 96,
                              .library = &catalog,
                              .effective_patches = patches};
        char error[4096] = {0};
        void *instance = api->create();
        assert(instance);
        check(api->inspect(instance, &context, error, sizeof(error)), error);
        select_pan_preset(api, instance, false, error, sizeof(error));
        warnings[0] = 0;
        void *job = api->create_job(instance, error, sizeof(error));
        assert(job);
        check(api->run(job, &context, &host, error, sizeof(error)), error);
        decode();
        unsigned reverb[8], rates[8], depth[8];
        size_t reverbs = decoded_values(0xd4, reverb), rate_count = decoded_values(0xd7, rates),
               depths = decoded_values(0xd8, depth);

        if (mode == 3) {
            assert(strstr(warnings, DM_TEXT_EXPORT_ITEM_SHARED_FX));
        } else {
            assert(!strstr(warnings, DM_TEXT_EXPORT_ITEM_SHARED_FX));
            assert(rate_count == 1 && rates[0] == rate);

            if (mode == 1) {
                assert(reverbs == 1 && reverb[0] == 40);
            } else {
                assert(reverbs == 3 && reverb[0] == 64 && !reverb[1] && reverb[2] == 64);
            }

            if (mode == 2) {
                assert(!depths);
            } else {
                assert(depths == 3 && depth[0] && !depth[1] && depth[2] == depth[0]);
            }
        }

        api->destroy_job(job);
        api->destroy(instance);
    }

    zones[0] = saved[0];
    zones[1] = saved[1];
    puts("PASS decoded preset reverb/vibrato, MIDI precedence, defaults and shared warnings");
}

static void bank_capacity(const DmeExporter *api, const DmeSampleCatalog *source) {
    DmeEvent events[508];
    DmeEffectivePatch patches[508];
    DmeSampleZoneParams params[254];

    for (unsigned i = 0; i < 254; i++) {
        unsigned region = i % 127;
        params[i] = zones[0].params;
        params[i].root = 60;
        params[i].tone = (DmeSampleToneParams){0};
        params[i].velocity_lo = params[i].velocity_hi = region + 1;
        events[i * 2] = (DmeEvent){i * 96, i * 2 + 1, MIDI_NOTE_ON, 60, region + 1, 0};
        events[i * 2 + 1] = (DmeEvent){i * 96 + 48, i * 2 + 2, MIDI_NOTE_OFF, 60, 0, 0};
        patches[i * 2] = patches[i * 2 + 1] = (DmeEffectivePatch){.program = i / 127};
    }

    for (unsigned i = 0; i < 127; i++) {
        zones[i] = (DmeSampleZone){.size = sizeof(zones[i]), .params = params[i]};
    }

    frozen_zones = preset.zone_count = 127;
    DmeSampleCatalogPreset entries[2] = {source->presets[0], source->presets[0]};
    entries[0].zone_count = entries[1].zone_count = 127;
    entries[1].id = str("second");
    entries[1].first_zone = 127;
    DmeSampleCatalogSet set = source->sets[0];
    set.preset_count = 2;
    DmeSampleCatalog catalog = {.size = sizeof(catalog),
                                .set_count = 1,
                                .preset_count = 2,
                                .zone_count = 254,
                                .sets = &set,
                                .presets = entries,
                                .zones = params};
    DmeContext context = {.events = events,
                          .event_count = 508,
                          .effective_patches = patches,
                          .duration = 254 * 96,
                          .ppq = 96,
                          .library = &catalog};
    char error[4096] = {0};
    void *instance = api->create();
    assert(instance);
    check(api->inspect(instance, &context, error, sizeof(error)), error);
    DmeUi ui;
    check(api->describe(instance, &ui), error);

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING) {
            assert(ui.fields[i].row_count == 2);
            char ids[2][64];
            DmeCustomSelection choices[2];

            for (unsigned row = 0; row < 2; row++) {
                snprintf(ids[row], sizeof(ids[row]), "%s", ui.fields[i].rows[row].id);
                choices[row] = ui.fields[i].rows[row].custom->selection;
                choices[row].enabled = true;
                choices[row].sample.preset_id = entries[row].id;
            }

            for (unsigned row = 0; row < 2; row++) {
                check(api->set_custom(instance, ids[row], &choices[row], error, sizeof(error)),
                      error);
            }

            break;
        }
    }

    DmeHost host = {.cancelled = cancel,
                    .progress = progress,
                    .diagnostic = diagnostic,
                    .artifact = output,
                    .freeze_sample = freeze,
                    .read_frozen_sample = pcm};
    warnings[0] = bank_output[0] = 0;
    unsigned before = outputs;
    void *job = api->create_job(instance, error, sizeof(error));
    assert(job);
    check(api->preflight(job, &context, &host, error, sizeof(error)), error);
    assert(strstr(warnings, DM_TEXT_NATIVE_FIX_ORDINARY_LABEL) && before == outputs);
    check(api->run(job, &context, &host, error, sizeof(error)), error);
    assert(!strstr(bank_output, "Validation_tone"));
    api->destroy_job(job);
    api->destroy(instance);
    puts("PASS native bank capacity falls back by whole selection");

#ifdef CUSTOM_TEST_OOT
    /* Sequential custom drums exceed OoT's 64 playable slots before the shared 128-entry cap. */
    for (unsigned i = 0; i < 254; i++) {
        events[i].status = (events[i].status & MIDI_STATUS_MASK) | MIDI_DRUM_CHANNEL;
        events[i].a = 42;
        patches[i] = (DmeEffectivePatch){.flags = DME_SAMPLE_PERCUSSION};
    }

    entries[0].flags = entries[1].flags = preset.flags = DME_SAMPLE_PERCUSSION;
    context.event_count = 254;
    context.percussion_channels = 1u << MIDI_DRUM_CHANNEL;
    instance = api->create();
    assert(instance);
    check(api->inspect(instance, &context, error, sizeof(error)), error);
    check(api->set(instance, "percussion", &(DmeValue){DME_BOOL, 1}, error, sizeof(error)), error);
    check(api->describe(instance, &ui), error);

    for (unsigned i = 0; i < ui.count; i++) {
        if (ui.fields[i].type == DME_MAPPING && ui.fields[i].row_count) {
            assert(ui.fields[i].row_count == 1);
            const DmeMappingRow *row = &ui.fields[i].rows[0];
            DmeCustomSelection choice = row->custom->selection;
            choice.enabled = true;
            choice.sample.preset_id = entries[0].id;
            check(api->set_custom(instance, row->id, &choice, error, sizeof(error)), error);
            break;
        }
    }

    warnings[0] = bank_output[0] = 0;
    job = api->create_job(instance, error, sizeof(error));
    assert(job);
    check(api->preflight(job, &context, &host, error, sizeof(error)), error);
    assert(strstr(warnings, DM_TEXT_NATIVE_FIX_ORDINARY_LABEL));
    check(api->run(job, &context, &host, error, sizeof(error)), error);
    assert(!strstr(bank_output, "Validation_tone"));
    api->destroy_job(job);
    api->destroy(instance);
    puts("PASS OoT drum bank capacity falls back by whole selection");
#endif
}

int main(int argc, char **argv) {
    assert(argc == 1 || (argc >= 3 && argc <= 5));
    const char *mode = argc >= 4 ? argv[3] : "add";
    bool replace = strstr(mode, "replace") != NULL;
    bool layered = strstr(mode, "layered") != NULL || strstr(mode, "missing-region") != NULL;
    frozen_zones = replace && !layered ? 1 : 2;
    const DmePlugin *plugin = duckmidi_export_plugin(DME_ABI);
    assert(plugin && !duckmidi_export_plugin(DME_ABI - 1));
    const DmeExporter *api = plugin->exporter(0);
    void *instance = api->create();
    assert(instance && api->set_custom && api->history);
    DmeEvent events[] = {{0, 1, MIDI_PROGRAM, 40, 0, 0},
                         {0, 2, MIDI_NOTE_ON, 60, 90, 0},
                         {96, 3, MIDI_NOTE_OFF, 60, 0, 0}};
    DmeEffectivePatch patches[3] = {
        {0}, {.program = 0, .note_source_id = 2}, {.program = 0, .note_source_id = 2}};
    DmeSampleCatalogSet set = {.size = sizeof(set),
                               .generation = 7,
                               .available = 1,
                               .preset_count = 1,
                               .modified = 10,
                               .locator = str("generated"),
                               .id = str("set"),
                               .name = str("Validation set")};
    DmeSampleCatalogPreset entry = {.size = sizeof(entry),
                                    .suggested_program = 1,
                                    .zone_count = frozen_zones,
                                    .id = str("preset"),
                                    .name = str("Layered validation tone")};
    DmeSampleZoneParams params[2];

    for (unsigned i = 0; i < 2; i++) {
        params[i] = (DmeSampleZoneParams){.size = sizeof(params[i]),
                                          .root = 60 + i * 12,
                                          .key_hi = 127,
                                          .velocity_hi = 127,
                                          .sustain = .7,
                                          .attack = .005,
                                          .hold = .01,
                                          .decay = .05,
                                          .release = .1,
                                          .playback_start = 16,
                                          .playback_end = 512,
                                          .loop_mode = DME_SAMPLE_LOOP_CONTINUOUS,
                                          .loop_start = 32,
                                          .loop_end = 480};
        /* A panned Replace also bakes a filter, so Revert must restore bank, sample and song. */
        if (strstr(mode, "pan-replace")) {
            params[i].pan = -.5;
            params[i].tone.cutoff = 2000;
        }

        /* Every converted or baked effect, for real native tool round trips. */
        if (strstr(mode, "effects")) {
            params[i].pan = -.25;
            params[i].tone = (DmeSampleToneParams){.cutoff = 2000,
                                                   .resonance = 3,
                                                   .chorus = .3,
                                                   .reverb = .4,
                                                   .vibrato_rate = 5,
                                                   .vibrato_depth = 30};
        }

        zones[i] = (DmeSampleZone){.size = sizeof(zones[i]), .params = params[i]};
    }

    asset = (DmeSampleAsset){
        .size = sizeof(asset), .rate = 32000, .channels = 2, .frames = 512, .id = str("asset")};
    preset = (DmeSamplePreset){
        .size = sizeof(preset), .zone_count = frozen_zones, .name = str("Validation tone")};
    DmeSampleCatalog catalog = {.size = sizeof(catalog),
                                .set_count = 1,
                                .preset_count = 1,
                                .zone_count = 2,
                                .sets = &set,
                                .presets = &entry,
                                .zones = params};
    DmeContext context = {.events = events,
                          .event_count = 3,
                          .duration = 192,
                          .ppq = 96,
                          .effective_patches = patches,
                          .library = &catalog,
                          .percussion_channels = 1u << 9};
    char error[4096] = {0};
#ifndef CUSTOM_TEST_OOT
    if (argc >= 3 && strstr(argv[2], "hacker") && !strstr(mode, "target-mismatch")) {
        check(api->set(instance, "target", &(DmeValue){DME_CHOICE, M64_GAME_HACKERSM64}, error,
                       sizeof(error)),
              error);
    }
#endif

    check(api->inspect(instance, &context, error, sizeof(error)), error);
    DmeUi ui;
    check(api->describe(instance, &ui), error);
    const DmeMappingRow *row = NULL;

    for (unsigned f = 0; f < ui.count; f++) {
        if (ui.fields[f].type == DME_MAPPING && ui.fields[f].row_count) {
            row = &ui.fields[f].rows[0];
        }
    }

    assert(row && row->source.program == 0 && row->custom && !row->custom->selection.enabled);
    DmeCustomSelection selection = row->custom->selection;
    selection.enabled = 1;
    selection.sample.preset_id = entry.id;
    check(api->set_custom(instance, row->id, &selection, error, sizeof(error)), error);
    check(api->set(instance, "memory", &(DmeValue){DME_CHOICE, M64_MEMORY_STOCK}, error,
                   sizeof(error)),
          error);
    DmeHost host = {.cancelled = cancel,
                    .progress = progress,
                    .diagnostic = diagnostic,
                    .artifact = output,
                    .freeze_sample = freeze,
                    .read_frozen_sample = pcm};
    void *job = NULL;

    if (argc == 1) {
        job = api->create_job(instance, error, sizeof(error));
        assert(job);
        check(api->preflight(job, &context, &host, error, sizeof(error)), error);
        assert(freezes == 1);
        /* Changing the source catalog after preparation cannot alter this job. */
        set.generation = 99;
        check(api->run(job, &context, &host, error, sizeof(error)), error);
        assert(freezes == 1);
#ifdef CUSTOM_TEST_OOT
        assert(outputs == 6);
        const char *layer = strstr(sequence_output, ".layer ");
        assert(layer && strstr(layer, "instr 0") && strstr(layer, "instr 1"));
        assert(!strstr(bank_output, "SAMPLE_0_424"));
#else
        assert(outputs == 7);
#endif
    } else {
        const DmeHistoryRuntime *repo_api = duckmidi_history_runtime(DME_REPOSITORY_API);
        const DmeHistoryAdapter *adapter = api->history(DME_REPOSITORY_API);
        DmeRepositoryRequest request = {.size = sizeof(request),
                                        .action = DME_REPOSITORY_INSPECT,
                                        .plugins_directory = str(argv[1]),
                                        .checkout = str(argv[2])};
        void *inspection =
            repo_api->create_job(adapter, api->id, instance, NULL, &request, error, sizeof(error));
        assert(inspection);
        check(repo_api->run(inspection, &context, &host, error, sizeof(error)), error);
        check(repo_api->finish(inspection, error, sizeof(error)), error);
        DmeRepositoryView prior;
        check(repo_api->describe(inspection, &prior), error);

        if (prior.notice.length) {
            fprintf(stderr, "%.*s\n", (int)prior.notice.length, prior.notice.data);
        }

        if (replace) {
            check(api->describe(instance, &ui), error);

            for (unsigned f = 0; f < ui.count; f++) {
                if (ui.fields[f].type == DME_MAPPING && ui.fields[f].row_count) {
                    row = &ui.fields[f].rows[0];
                }
            }

            selection = row->custom->selection;
            const DmeSampleTarget *target = NULL;
            char piano[32] = {0};

            if (argc == 5 && !strcmp(argv[4], "auto-piano")) {
#ifdef CUSTOM_TEST_OOT
                int value = oot_default_instrument(OOT_CUSTOM_FONT, 0);
                snprintf(piano, sizeof(piano), "Sample%03u.wav",
                         oot_instruments[value].regions[0].sample);
#else
                const char *sample = m64_get_api_3()->default_program(0)->sample;
                snprintf(piano, sizeof(piano), "/%.2s", sample);
#endif
            }

            for (uint32_t t = 0; t < prior.target_count; t++) {
                const DmeSampleTarget *candidate = &prior.targets[t];
#ifdef CUSTOM_TEST_OOT
                bool compatible = strstr(candidate->path.data, "/SampleBank_0/") != NULL;
#else
                bool compatible = strstr(candidate->path.data, "/instruments/") != NULL;
#endif
                if (argc == 5 && strcmp(argv[4], "auto-piano")) {
                    compatible = true;
                }

                if (compatible &&
                    (argc != 5 || strstr(candidate->path.data, *piano ? piano : argv[4]))) {
                    target = candidate;
                    break;
                }
            }

            assert(target);
            selection.operation = DME_SAMPLE_REPLACE;
            selection.physical_target = target->id;
            selection.region_set = strstr(mode, "layered") != NULL;
            selection.replace_zone = 1;

            if (strstr(mode, "missing-target")) {
                selection.physical_target = str("unavailable-physical-target");
            }

            check(api->set_custom(instance, row->id, &selection, error, sizeof(error)), error);
        }

        if (strstr(mode, "package")) {
            job = api->create_job(instance, error, sizeof(error));
            assert(job);
            check(api->preflight(job, &context, &host, error, sizeof(error)), error);
            check(api->run(job, &context, &host, error, sizeof(error)), error);
            assert(outputs > 2);
            printf("package artifacts=%u\n", outputs);
            repo_api->destroy_job(inspection);
            api->destroy_job(job);
            api->destroy(instance);
            return 0;
        }

        if (!strncmp(mode, "song-b", 6)) {
            if (strstr(mode, "stock")) {
                check(api->describe(instance, &ui), error);

                for (unsigned f = 0; f < ui.count; f++) {
                    if (ui.fields[f].type == DME_MAPPING && ui.fields[f].row_count) {
                        row = &ui.fields[f].rows[0];
                    }
                }

                selection = row->custom->selection;
                selection.enabled = 0;
                check(api->set_custom(instance, row->id, &selection, error, sizeof(error)), error);
            }

#ifdef CUSTOM_TEST_OOT
            check(api->set(instance, "song", &(DmeValue){DME_CHOICE, 0}, error, sizeof(error)),
                  error);
            check(api->set(instance, "name", &(DmeValue){DME_TEXT, 0, "duckmidi_song_b"}, error,
                           sizeof(error)),
                  error);
#else
            check(api->set(instance, "sequence", &(DmeValue){DME_CHOICE, 4}, error, sizeof(error)),
                  error);
            check(api->set(instance, "bank_name", &(DmeValue){DME_TEXT, 0, "zz_dm_bank_b"}, error,
                           sizeof(error)),
                  error);
#endif
            if (strstr(mode, "update")) {
                events[1].a = events[2].a = 65;
                events[1].b = 110;
            }
        }

        if (!strcmp(mode, "rename")) {
#ifndef CUSTOM_TEST_OOT
            check(api->set(instance, "sequence", &(DmeValue){DME_CHOICE, 35}, error, sizeof(error)),
                  error);
            check(api->set(instance, "sequence_name", &(DmeValue){DME_TEXT, 0, "03_renamed"}, error,
                           sizeof(error)),
                  error);
#endif
        }

        if (!strcmp(mode, "revert-all") || !strcmp(mode, "revert-sample") ||
            !strcmp(mode, "revert-song-a") || !strcmp(mode, "initialize")) {
            request.action = !strcmp(mode, "revert-all")   ? DME_REPOSITORY_REVERT_ALL
                             : !strcmp(mode, "initialize") ? DME_REPOSITORY_INITIALIZE
                                                           : DME_REPOSITORY_REVERT;
            request.scope = prior.selected_scope;

            for (uint32_t i = 0; request.action == DME_REPOSITORY_REVERT && i < prior.row_count;
                 i++) {
                bool sample = !strncmp(prior.rows[i].key.data, "sample:", 7);

                if (sample == !strcmp(mode, "revert-sample")) {
                    request.logical_key = prior.rows[i].key;
                    break;
                }
            }

            assert(request.action != DME_REPOSITORY_REVERT || request.logical_key.length);
        } else if (!strcmp(mode, "inspect")) {
            request.action = DME_REPOSITORY_INSPECT;
        } else {
            request.action = DME_REPOSITORY_INSTALL;
            job = api->create_job(instance, error, sizeof(error));
            assert(job);
        }

        void *repo_job =
            repo_api->create_job(adapter, api->id, instance, job, &request, error, sizeof(error));
        assert(repo_job);
        repo_api->destroy_job(inspection);
        warnings[0] = 0;
        int32_t prepared = repo_api->prepare(repo_job, &context, &host, error, sizeof(error));

        /* A layered Replace without a chosen region uses the region played most. */
        if (strstr(mode, "missing-region")) {
            check(prepared, error);
            assert(strstr(warnings, DM_TEXT_NATIVE_FIX_REPLACE_ZONE_LABEL));
            repo_api->destroy_job(repo_job);
            api->destroy_job(job);
            api->destroy(instance);
            return 0;
        }

        check(prepared, error);

        if (strstr(mode, "missing-target")) {
            assert(strstr(warnings, DM_TEXT_NATIVE_FIX_REPLACE_ADD_LABEL));
        }

        if (strstr(mode, "target-mismatch")) {
            assert(strstr(warnings, DM_TEXT_NATIVE_FIX_IGNORED_LABEL));
        }

        check(repo_api->run(repo_job, &context, &host, error, sizeof(error)), error);
        check(repo_api->finish(repo_job, error, sizeof(error)), error);

        /* A panned Replace sets the shared drum pan; a centred one keeps the seeded stock 17. */
        bool centre = strstr(mode, "centre-replace") != NULL;

        if (centre || strstr(mode, "pan-replace")) {
            char path[8192];
#ifdef CUSTOM_TEST_OOT
            snprintf(path, sizeof(path), "%s/assets/audio/soundfonts/Soundfont_0.xml", argv[2]);
            const char *expected = centre ? "Pan=\"17\"" : "Pan=\"33\"";
            const char *spaced = expected;
#else
            snprintf(path, sizeof(path), "%s/sound/sound_banks/11.json", argv[2]);
            const char *expected = centre ? "\"pan\":17" : "\"pan\":33";
            const char *spaced = centre ? "\"pan\": 17" : "\"pan\": 33";
#endif
            char *bank = SDL_LoadFile(path, NULL);
            assert(bank && (strstr(bank, expected) || strstr(bank, spaced)));
            SDL_free(bank);
        }
        DmeRepositoryView view;
        check(repo_api->describe(repo_job, &view), error);
        assert(view.selected_scope && (view.target_count || coop_scope(&view)));
        printf("scope=%llu targets=%u rows=%u paths=%u\n", (unsigned long long)view.selected_scope,
               view.target_count, view.row_count, view.row_count ? view.rows[0].path_count : 0);
        repo_api->destroy_job(repo_job);
    }

    if (job) {
        api->destroy_job(job);
    }

    api->destroy(instance);

    if (argc == 1) {
        independent_stock_rows(api);
        set.generation = 7;
        choke_export(api, &catalog);
        pan_export(api, &catalog);
        effects_export(api, &catalog);
        controller_rate_limit(api, &catalog);
        bank_capacity(api, &catalog);
    }

    return 0;
}
