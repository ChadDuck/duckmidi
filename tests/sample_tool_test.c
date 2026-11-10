#include "../include/duckmidi_samples_tool.h"
#include "export_plugins.h"
#include "../shared/sample/library.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int32_t DME_CALL cancel(void *user) {
    return *(const bool *)user;
}

static void DME_CALL progress(void *user, uint32_t stage, DmeString path, uint32_t index,
                              uint32_t total, uint64_t done, uint64_t units) {
    (void)user;
    assert(stage <= DME_SAMPLE_ANALYZE && path.data && path.length && index < total &&
           done <= units);
}

typedef struct {
    const DmeSamplesApi *api;
    DmeSampleImportHost host;
    void *job;
    int32_t result;
    char error[256];
} ImportWorker;

static int SDLCALL import_worker(void *data) {
    ImportWorker *worker = data;
    worker->result =
        worker->api->run_import(worker->job, &worker->host, worker->error, sizeof(worker->error));
    return 0;
}

static void run_job(ImportWorker *worker) {
    SDL_Thread *thread = SDL_CreateThread(import_worker, "sample fixture", worker);
    assert(thread);
    SDL_WaitThread(thread, NULL);
}

static void assert_zone_values(const DmeSampleZoneParams *a, const DmeSampleZoneParams *b) {
    assert(a->size == b->size && a->root == b->root && a->key_lo == b->key_lo &&
           a->key_hi == b->key_hi && a->velocity_lo == b->velocity_lo &&
           a->velocity_hi == b->velocity_hi && a->cut_group == b->cut_group &&
           a->cut_by == b->cut_by && a->loop_mode == b->loop_mode &&
           a->loop_start == b->loop_start && a->loop_end == b->loop_end && a->cents == b->cents &&
           a->gain_db == b->gain_db && a->pan == b->pan && a->delay == b->delay &&
           a->attack == b->attack && a->hold == b->hold && a->decay == b->decay &&
           a->sustain == b->sustain && a->release == b->release &&
           a->playback_start == b->playback_start && a->playback_end == b->playback_end &&
           a->playback_set == b->playback_set);
    assert(a->tone.custom_tracking == b->tone.custom_tracking && a->tone.coarse == b->tone.coarse &&
           a->tone.fine == b->tone.fine && a->tone.tracking == b->tone.tracking &&
           a->tone.cutoff == b->tone.cutoff && a->tone.resonance == b->tone.resonance &&
           a->tone.filter_depth == b->tone.filter_depth &&
           a->tone.vibrato_rate == b->tone.vibrato_rate &&
           a->tone.vibrato_depth == b->tone.vibrato_depth && a->tone.chorus == b->tone.chorus &&
           a->tone.reverb == b->tone.reverb);
}

static void import_reset(DmNativeTool *tool, const DmeSamplesApi *api, const char *directory) {
    char path[4096], error[256] = {0};
    snprintf(path, sizeof(path), "%s/Bass_C4.wav", directory);
    DmeString input = {path, strlen(path)};
    bool canceled = false;
    ImportWorker worker = {.api = api, .host = {&canceled, cancel, progress}};
    worker.job = api->create_import(tool->instance, &input, 1, error, sizeof(error));
    assert(worker.job);
    run_job(&worker);
    assert(worker.result == DME_OK);
    assert(api->finish_import(tool->instance, worker.job, error, sizeof(error)) == DME_OK);
    api->destroy_import(worker.job);
    DmeSampleDraft draft;
    assert(api->draft(tool->instance, 0, &draft) == DME_OK);
    assert(draft.edit.zone.root == 60 && draft.root_source == DME_ROOT_FILENAME &&
           draft.embedded_root == 72 && draft.pitch_conflict && draft.edit.zone.loop_start == 64 &&
           draft.edit.zone.loop_end == 128 && draft.edit.zone.loop_mode == DME_SAMPLE_LOOP_SUSTAIN);
    DmeSampleEdit changed = draft.edit;
    changed.name = (DmeString){"Changed", 7};
    changed.program = 7;
    changed.zone.root = 55;
    changed.zone.playback_start = 32;
    changed.zone.playback_end = 200;
    changed.zone.playback_set = 1;
    changed.zone.release = 9;
    changed.zone.tone.reverb = .8;
    changed.zone.cut_group = 5;
    assert(api->edit(tool->instance, 0, &changed) == DME_OK);
    assert(api->reset_edit(tool->instance, 0) == DME_OK);
    assert(api->draft(tool->instance, 0, &draft) == DME_OK);
    assert(draft.edit.zone.root == 60 && draft.edit.zone.release == .12 &&
           !draft.edit.zone.tone.reverb && !draft.edit.zone.cut_group &&
           !draft.edit.zone.playback_set && !draft.edit.zone.playback_start &&
           !draft.edit.zone.playback_end && !strcmp(draft.edit.name.data, "Bass_C4") &&
           draft.edit.program == 33 && draft.edit.zone.loop_end == 128);
    changed = draft.edit;
    changed.zone.playback_end = 100;
    assert(api->edit(tool->instance, 0, &changed) == DME_INVALID);
    assert(api->reset_edit(tool->instance, 1) == DME_INVALID);
    api->reset(tool->instance);
    puts(
        "PASS imported loop defaults, filename-first root, complete reset and playback validation");
}

static void saved_sets(const DmNativeTool *tool, const DmeSamplesApi *api, const char *directory) {
    char source[4096], root[4096], error[256] = {0};
    SDL_Time now;
    assert(SDL_GetCurrentTime(&now));
    snprintf(source, sizeof(source), "%s/codec-0-0.wav", directory);
    snprintf(root, sizeof(root), "%s/saved-set-%llu", directory, (unsigned long long)now);
    assert(SDL_CreateDirectory(root));
    DmSampleAudio audio = {0};
    assert(dm_sample_decode(source, NULL, &audio, error, sizeof(error)));
    audio.embedded_root = 72;
    audio.embedded_loop_start = 100;
    audio.embedded_loop_end = 500;
    DmSampleAsset asset = {.id = "wave",
                           .file = "wave.wav",
                           .frames = (uint32_t)audio.frames,
                           .rate = (uint32_t)audio.rate,
                           .channels = (uint8_t)audio.channels,
                           .source_name = "Original C4.wav"};
    DmSampleZone zones[3] = {{.root = 60,
                              .key_hi = 63,
                              .velocity_lo = 7,
                              .velocity_hi = 111,
                              .cents = 3,
                              .gain_db = -2,
                              .pan = .4,
                              .cut_group = 3,
                              .cut_by = 5,
                              .envelope = {.delay = .11,
                                           .attack = .13,
                                           .hold = .17,
                                           .decay = .19,
                                           .sustain = .37,
                                           .release = .41},
                              .loop_mode = DM_SAMPLE_SUSTAIN_LOOP,
                              .loop = {64, 2048},
                              .tone = {.coarse = 12,
                                       .fine = -20,
                                       .custom_tracking = true,
                                       .tracking = 75,
                                       .cutoff = 1200,
                                       .resonance = 6,
                                       .filter_depth = 1200,
                                       .vibrato_depth = 40,
                                       .vibrato_rate = 5,
                                       .chorus = .2,
                                       .reverb = .3}}};
    zones[1] = zones[2] = zones[0];
    zones[1].key_lo = 64;
    zones[1].key_hi = 127;
    zones[2].root = 69;
    zones[2].key_lo = zones[2].key_hi = 42;
    DmSamplePreset presets[] = {
        {.id = "layered", .name = "Layered", .zone_count = 2, .suggested_program = 9},
        {.id = "kit", .name = "Kit", .drums = true, .first_zone = 2, .zone_count = 1}};
    DmSampleManifest manifest = {.id = "loaded",
                                 .name = "Loaded",
                                 .generation = 1,
                                 .asset_count = 1,
                                 .preset_count = 2,
                                 .zone_count = 3,
                                 .assets = &asset,
                                 .presets = presets,
                                 .zones = zones};
    assert(dm_sample_library_publish(root, &manifest, &audio, NULL, error, sizeof(error)));
    dm_sample_audio_free(&audio);

    /* Samples and their SFZ companions sit together in the set folder; both presets use the same
       sample, so the second SFZ name adds the preset ID. */
    const char *saved[] = {"manifest.json", "wave.wav", "wave.sfz", "wave-kit.sfz"};

    for (size_t i = 0; i < SDL_arraysize(saved); ++i) {
        char file[4096];
        SDL_PathInfo info;
        snprintf(file, sizeof(file), "%s/Loaded/%s", root, saved[i]);
        assert(SDL_GetPathInfo(file, &info) && info.type == SDL_PATHTYPE_FILE);
    }

    void *instance = tool->api->create();
    assert(instance);
    bool stopped = false;
    ImportWorker worker = {.api = api, .host = {&stopped, cancel, progress}};
    DmeString locator = {root, strlen(root)}, id = {"loaded", 6};
    worker.job = api->create_load(instance, locator, id, error, sizeof(error));
    assert(worker.job);
    run_job(&worker);
    assert(worker.result == DME_OK && !api->count(instance));
    assert(api->finish_import(instance, worker.job, error, sizeof(error)) == DME_OK);
    api->destroy_import(worker.job);
    assert(api->count(instance) == 3);

    for (uint32_t i = 0; i < 3; ++i) {
        DmeSampleDraft draft;
        assert(api->draft(instance, i, &draft) == DME_OK && draft.peak_count == 4096);
        DmeSampleZoneParams expected = dm_sample_zone_params(&zones[i]);
        assert_zone_values(&draft.edit.zone, &expected);
        assert(!strcmp(draft.asset_id.data, "wave") &&
               !strcmp(draft.source_name.data, "Original C4.wav") &&
               !strcmp(draft.preset_id.data, i == 2 ? "kit" : "layered"));
        assert(draft.edit.drums == (i == 2) && draft.preset_drums == (i == 2));
        assert(draft.preset_zone == (i == 2 ? 0 : i));
    }

    DmeSampleDraft draft;
    assert(api->draft(instance, 0, &draft) == DME_OK);
    DmeSampleEdit edit = draft.edit;
    edit.name = (DmeString){"Unsaved", 7};
    edit.zone.root = 71;
    edit.program = 27;
    assert(api->edit(instance, 0, &edit) == DME_OK);
    assert(api->draft(instance, 1, &draft) == DME_OK && !strcmp(draft.edit.name.data, "Unsaved") &&
           draft.edit.program == 27 && draft.edit.zone.key_lo == 64);
    worker.job = api->create_load(instance, (DmeString){0}, (DmeString){0}, error, sizeof(error));
    assert(worker.job);
    run_job(&worker);
    assert(worker.result == DME_OK && api->count(instance) == 3);
    assert(api->finish_import(instance, worker.job, error, sizeof(error)) == DME_OK);
    api->destroy_import(worker.job);
    assert(!api->count(instance));
    DmeString path = {source, strlen(source)};
    worker.job = api->create_import(instance, &path, 1, error, sizeof(error));
    assert(worker.job);
    run_job(&worker);
    assert(worker.result == DME_OK);
    assert(api->finish_import(instance, worker.job, error, sizeof(error)) == DME_OK);
    api->destroy_import(worker.job);
    assert(api->count(instance) == 1);
    worker.job = api->create_load(instance, locator, id, error, sizeof(error));
    assert(worker.job);
    run_job(&worker);
    assert(worker.result == DME_OK);
    assert(api->finish_import(instance, worker.job, error, sizeof(error)) == DME_OK);
    api->destroy_import(worker.job);
    assert(api->count(instance) == 3 && api->draft(instance, 0, &draft) == DME_OK &&
           draft.edit.zone.root == 71 && !strcmp(draft.edit.name.data, "Unsaved"));
    assert(api->can_undo(instance, 0) && api->undo(instance, 0) == DME_OK);
    assert(api->draft(instance, 0, &draft) == DME_OK && draft.edit.zone.root == 60 &&
           !strcmp(draft.edit.name.data, "Layered"));
    assert(api->undo(instance, 1) == DME_OK);
    assert(api->draft(instance, 0, &draft) == DME_OK && draft.edit.zone.root == 71);
    worker.job =
        api->create_load(instance, locator, (DmeString){"missing", 7}, error, sizeof(error));
    assert(worker.job);
    run_job(&worker);
    assert(worker.result == DME_FAILED && api->count(instance) == 3);
    api->destroy_import(worker.job);
    worker.job = api->create_load(instance, (DmeString){0}, (DmeString){0}, error, sizeof(error));
    assert(worker.job);
    stopped = true;
    run_job(&worker);
    assert(worker.result == DME_CANCELLED && api->count(instance) == 3);
    api->destroy_import(worker.job);
    assert(api->reset_edit(instance, 0) == DME_OK);
    assert(api->draft(instance, 0, &draft) == DME_OK && draft.edit.zone.root == 60 &&
           draft.edit.zone.loop_start == 100 && draft.edit.zone.loop_end == 500 &&
           draft.edit.zone.loop_mode == DME_SAMPLE_LOOP_SUSTAIN &&
           draft.root_source == DME_ROOT_FILENAME && !draft.edit.zone.tone.reverb &&
           !strcmp(draft.edit.name.data, "Original C4"));
    tool->api->destroy(instance);
    puts(
        "PASS saved set metadata, shared assets, layered presets, cached edits, load failure and cancellation");
}

int main(int argc, char **argv) {
    assert(argc == 3 && SDL_Init(0));
    DmExportRegistry registry;
    dm_export_registry_load(&registry, argv[1]);
    assert(registry.count == 2 && registry.tool_count == 1);
    assert(dm_export_registry_request_tool(&registry, DME_SAMPLES_TOOL_ID) == DME_OK);
    DmNativeTool *tool = dm_export_registry_tool(&registry, DME_SAMPLES_TOOL_ID);
    assert(tool && tool->open_requested);
    const DmeSamplesApi *api = tool->api->interface_at(DME_SAMPLES_API);
    assert(api && api->size == sizeof(*api) && api->version == DME_SAMPLES_API);
    import_reset(tool, api, argv[2]);
    saved_sets(tool, api, argv[2]);

    char path[4096], error[256] = {0};
    int length = snprintf(path, sizeof(path), "%s/codec-0-0.wav", argv[2]);
    assert(length > 0 && (size_t)length < sizeof(path));
    DmeString paths[] = {{path, (uint64_t)length}, {"missing-sample.wav", 18}};
    bool cancel_requested = false;
    ImportWorker worker = {.api = api, .host = {&cancel_requested, cancel, progress}};
    worker.job = api->create_import(tool->instance, paths, 2, error, sizeof(error));
    assert(worker.job);
    memset(path, 'X', (size_t)length); /* The job has copied its inputs. */
    run_job(&worker);
    assert(worker.result == DME_OK && !api->count(tool->instance));
    assert(api->finish_import(tool->instance, worker.job, error, sizeof(error)) == DME_OK);
    api->destroy_import(worker.job);
    assert(api->count(tool->instance) == 2);

    assert(api->can_undo(tool->instance, 0) && !api->can_undo(tool->instance, 1));
    assert(api->undo(tool->instance, 0) == DME_OK && !api->count(tool->instance));
    assert(api->undo(tool->instance, 1) == DME_OK && api->count(tool->instance) == 2);
    DmeSampleDraft draft;
    assert(api->draft(tool->instance, 0, &draft) == DME_OK && !draft.error.length);
    assert(draft.rate == 48000 && draft.channels == 2 && draft.frames == 52817 && draft.peak_count);
    float pcm[16];
    assert(api->read_pcm(tool->instance, 0, 100, 8, pcm, 16) == DME_OK && pcm[0] != 0);
    assert(api->read_pcm(tool->instance, 0, 100, 8, pcm, 15) == DME_INVALID);
    DmeSamplePeak peak;
    assert(api->read_peaks(tool->instance, 0, 0, 1, &peak) == DME_OK);
    assert(draft.peak_count == 4096 && peak.minimum[0] <= peak.maximum[0] && peak.maximum[0] > 0);
    DmeSampleEdit edit = draft.edit;
    edit.name = (DmeString){"Named tone", 10};
    edit.zone.root = 70;
    edit.zone.loop_mode = DME_SAMPLE_LOOP_SUSTAIN;
    edit.zone.loop_start = 64;
    edit.zone.loop_end = 128;
    edit.zone.tone = (DmeSampleToneParams){.coarse = 12,
                                           .fine = -20,
                                           .custom_tracking = true,
                                           .tracking = 75,
                                           .cutoff = 1200,
                                           .resonance = 6,
                                           .filter_depth = 1200,
                                           .vibrato_depth = 40,
                                           .vibrato_rate = 5,
                                           .chorus = .2,
                                           .reverb = .3};
    assert(api->edit(tool->instance, 0, &edit) == DME_OK);
    assert(api->draft(tool->instance, 0, &draft) == DME_OK);
    assert(draft.edit.zone.root == 70 && draft.edit.zone.loop_end == 128 &&
           !strcmp(draft.edit.name.data, "Named tone") && draft.edit.zone.tone.tracking == 75 &&
           draft.edit.zone.tone.filter_depth == 1200);
    uint64_t token = draft.token;
    assert(token && api->undo(tool->instance, 0) == DME_OK);
    assert(api->draft(tool->instance, 0, &draft) == DME_OK && draft.edit.zone.root != 70 &&
           draft.token == token && strcmp(draft.edit.name.data, "Named tone"));
    assert(api->undo(tool->instance, 1) == DME_OK);
    assert(api->draft(tool->instance, 0, &draft) == DME_OK && draft.edit.zone.root == 70 &&
           !strcmp(draft.edit.name.data, "Named tone"));
    api->history_group(tool->instance, 1);

    for (int root = 71; root <= 80; ++root) {
        edit.zone.root = root;
        assert(api->edit(tool->instance, 0, &edit) == DME_OK);
    }

    api->history_group(tool->instance, 0);
    assert(api->undo(tool->instance, 0) == DME_OK);
    assert(api->draft(tool->instance, 0, &draft) == DME_OK && draft.edit.zone.root == 70);
    assert(api->undo(tool->instance, 1) == DME_OK);
    assert(api->draft(tool->instance, 0, &draft) == DME_OK && draft.edit.zone.root == 80);
    assert(api->undo(tool->instance, 0) == DME_OK);
    edit.zone.root = 70;
    edit.zone.gain_db = -3;
    assert(api->edit(tool->instance, 0, &edit) == DME_OK && !api->can_undo(tool->instance, 1));
    edit.zone.tone.cutoff = 10;
    assert(api->edit(tool->instance, 0, &edit) == DME_INVALID);
    edit.zone.tone.cutoff = 1200;
    edit.zone.loop_end = draft.frames + 1;
    assert(api->edit(tool->instance, 0, &edit) == DME_INVALID);
    assert(api->draft(tool->instance, 1, &draft) == DME_OK && draft.error.length);

    assert(api->remove(tool->instance, 2) == DME_INVALID);
    assert(api->remove(tool->instance, 0) == DME_OK && api->count(tool->instance) == 1);
    assert(api->draft(tool->instance, 0, &draft) == DME_OK && draft.error.length);
    assert(api->undo(tool->instance, 0) == DME_OK && api->count(tool->instance) == 2);
    assert(api->draft(tool->instance, 0, &draft) == DME_OK && draft.token == token &&
           !strcmp(draft.edit.name.data, "Named tone") && draft.edit.zone.gain_db == -3);
    float restored[16];
    assert(api->read_pcm(tool->instance, 0, 100, 8, restored, 16) == DME_OK &&
           !memcmp(pcm, restored, sizeof(pcm)));
    assert(api->undo(tool->instance, 1) == DME_OK && api->count(tool->instance) == 1);
    assert(api->remove(tool->instance, 0) == DME_OK && !api->count(tool->instance));

    SDL_Time now;
    assert(SDL_GetCurrentTime(&now));
    char folder[4096], file[4096], source[4096];
    snprintf(folder, sizeof(folder), "%s/sample-tool-%llu", argv[2], (unsigned long long)now);
    snprintf(file, sizeof(file), "%s/tone.wav", folder);
    snprintf(source, sizeof(source), "%s/codec-0-0.wav", argv[2]);
    assert(SDL_CreateDirectory(folder) && SDL_CopyFile(source, file));
    const unsigned expected[] = {1, 1, 2, 3, 3};

    for (unsigned phase = 0; phase < SDL_arraysize(expected); ++phase) {
        const char *input = phase == 1 || phase == 4 ? folder : file;
        DmeString selected = {input, strlen(input)};
        worker.job = api->create_import(tool->instance, &selected, 1, error, sizeof(error));
        assert(worker.job);
        run_job(&worker);
        assert(worker.result == DME_OK);
        assert(api->finish_import(tool->instance, worker.job, error, sizeof(error)) == DME_OK);
        api->destroy_import(worker.job);
        assert(api->count(tool->instance) == expected[phase]);
    }

    assert(api->remove(tool->instance, 1) == DME_OK);
    assert(api->draft(tool->instance, 1, &draft) == DME_OK &&
           !strcmp(draft.edit.name.data, "tone") && draft.frames == 52817);
    SDL_PathInfo info;
    assert(SDL_GetPathInfo(file, &info) && info.type == SDL_PATHTYPE_FILE);
    api->reset(tool->instance);

    worker.job = api->create_import(tool->instance, paths + 1, 1, error, sizeof(error));
    assert(worker.job);
    run_job(&worker);
    api->reset(tool->instance);
    assert(api->finish_import(tool->instance, worker.job, error, sizeof(error)) == DME_CANCELLED);
    api->destroy_import(worker.job);
    assert(!api->count(tool->instance) && !api->can_undo(tool->instance, 0) &&
           !api->can_undo(tool->instance, 1));

    worker.job = api->create_import(tool->instance, paths + 1, 1, error, sizeof(error));
    assert(worker.job);
    cancel_requested = true;
    run_job(&worker);
    assert(worker.result == DME_CANCELLED);
    api->destroy_import(worker.job);
    dm_export_registry_free(&registry);
    SDL_Quit();
    puts("PASS bundled Samples tool, owned job paths, draft edits, PCM bounds and canceled work");
    return 0;
}
