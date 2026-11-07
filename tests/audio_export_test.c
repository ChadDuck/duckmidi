#include "../shared/audio_export.h"
#include "../shared/sample_analysis.h"
#include "../shared/sample_batch.h"
#include "../shared/sample_export_snapshot.h"
#include "../shared/sample_import.h"
#include "../shared/sample_library.h"
#include "../shared/sample_manifest.h"
#include <SDL3/SDL.h>
#include <libavutil/intreadwrite.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

typedef struct {
    uint64_t read;
    bool cancel;
} ImportProgress;

static void progress(void *user, uint64_t read, uint64_t size) {
    ImportProgress *state = user;
    assert(state->read < read && read <= size);
    state->read = read;
}

static bool canceled(void *user) {
    const ImportProgress *state = user;
    return state->cancel && state->read != 0;
}

static DmSampleAudio check_import(const char *path, int codec) {
    DmSampleAudio sample = {0};
    ImportProgress state = {0};
    DmSampleImportCallbacks callbacks = {&state, canceled, progress};
    char error[512] = {0};
    bool ok = dm_sample_decode(path, &callbacks, &sample, error, sizeof(error));

    if (!ok) {
        fprintf(stderr, "%s: %s\n", path, error);
    }

    assert(ok && state.read > 0 && sample.rate == 48000 && sample.channels == 2);
    assert(sample.embedded_root == -1 && sample.codec[0] && sample.format[0]);

    if (codec == DM_AUDIO_WAV || codec == DM_AUDIO_FLAC) {
        assert(sample.frames == 52817);
    } else {
        assert(sample.frames >= 52000 && sample.frames <= 55000);
    }

    double left = 0, right = 0;

    for (size_t i = 9600; i < 14400; ++i) {
        left += sample.pcm[2 * i] * sample.pcm[2 * i];
        right += sample.pcm[2 * i + 1] * sample.pcm[2 * i + 1];
    }

    assert(right > 20 && right < 55 && left > 2.5 * right && left < 5.5 * right);
    return sample;
}

static DmSampleAudio check_aiff(const char *directory) {
    /* 8 samples of 16-bit PCM at 32000 Hz, MIDI B4 and -12 cents. */
    static const unsigned char aiff[] = {
        'F', 'O',  'R', 'M',  0,   0,   0,   90,   'A', 'I',  'F', 'F',  'C',  'O',  'M', 'M', 0,
        0,   0,    18,  0,    1,   0,   0,   0,    8,   0,    16,  0x40, 0x0d, 0xfa, 0,   0,   0,
        0,   0,    0,   0,    'I', 'N', 'S', 'T',  0,   0,    0,   20,   71,   244,  0,   127, 1,
        127, 0,    0,   0,    0,   0,   0,   0,    0,   0,    0,   0,    0,    0,    0,   'S', 'S',
        'N', 'D',  0,   0,    0,   24,  0,   0,    0,   0,    0,   0,    0,    0,    0,   0,   0x10,
        0,   0x20, 0,   0x30, 0,   0,   0,   0xf0, 0,   0xe0, 0,   0xd0, 0,
    };
    char path[DM_PATH], error[512] = {0};
    snprintf(path, sizeof(path), "%s/tone_B4.aiff", directory);
    DmOutput output;
    assert(dm_output_begin(&output, path, error, sizeof(error)));
    assert(dm_output_write(&output, aiff, sizeof(aiff), error, sizeof(error)));
    assert(dm_output_finish(&output, true, error, sizeof(error)));
    DmSampleAudio sample = {0};
    assert(dm_sample_decode(path, NULL, &sample, error, sizeof(error)));
    assert(sample.frames == 8 && sample.rate == 32000 && sample.channels == 1);
    assert(sample.embedded_root == 71 && sample.embedded_cents == -12);
    assert(fabs(sample.pcm[3] - 0.375) < 0.00001);

    ImportProgress state = {.cancel = true};
    DmSampleImportCallbacks callbacks = {&state, canceled, progress};
    DmSampleAudio rejected = {0};
    assert(!dm_sample_decode(path, &callbacks, &rejected, error, sizeof(error)));
    assert(!rejected.pcm && strstr(error, "canceled"));

    snprintf(path, sizeof(path), "%s/broken.aiff", directory);
    assert(dm_output_begin(&output, path, error, sizeof(error)));
    assert(dm_output_write(&output, aiff, 29, error, sizeof(error)));
    assert(dm_output_finish(&output, true, error, sizeof(error)));
    assert(!dm_sample_decode(path, NULL, &rejected, error, sizeof(error)));
    assert(!rejected.pcm && !rejected.frames);
    return sample;
}

static void check_wave_loops(const char *directory) {
    char path[DM_PATH], error[256] = {0};
    snprintf(path, sizeof(path), "%s/codec-0-0.wav", directory);
    DmBytes wave = {0};
    assert(dm_read(path, &wave, error, sizeof(error)));
    size_t original = wave.size;
    assert(dm_reserve((void **)&wave.data, &wave.capacity, original + 68, 1));
    wave.size += 68;
    AV_WL32(wave.data + 4, (uint32_t)wave.size - 8);
    uint8_t *chunk = wave.data + original;

    for (unsigned variant = 0; variant < 8; ++variant) {
        memset(chunk, 0, 68);
        memcpy(chunk, "smpl", 4);
        AV_WL32(chunk + 4, 60);
        AV_WL32(chunk + 20, variant == 7 ? 128 : 72);
        AV_WL32(chunk + 36, variant == 1 ? 0 : variant == 2 ? 2 : 1);
        AV_WL32(chunk + 48, variant == 3 ? 1 : 0);
        AV_WL32(chunk + 52, 64);
        AV_WL32(chunk + 56, variant == 4 ? UINT32_MAX : 127);
        AV_WL32(chunk + 60, variant == 5 ? 1 : 0);
        AV_WL32(chunk + 64, variant == 6 ? 2 : 0);
        snprintf(path, sizeof(path), "%s/loop-test-%u.wav", directory, variant);
        assert(dm_write(path, wave.data, wave.size, error, sizeof(error)));
        DmSampleAudio audio = {.embedded_root = -1};
        assert(dm_sample_decode(path, NULL, &audio, error, sizeof(error)));
        bool valid = variant == 0 || variant == 7;
        assert(audio.embedded_loop_end == (valid ? 128 : 0));
        assert(audio.embedded_loop_start == (valid ? 64 : 0));
        dm_sample_audio_free(&audio);
    }

    snprintf(path, sizeof(path), "%s/loop-test-0.wav", directory);
    char named[DM_PATH];
    snprintf(named, sizeof(named), "%s/Bass_C4.wav", directory);
    assert(SDL_CopyFile(path, named));
    dm_bytes_free(&wave);
    puts(
        "PASS inclusive WAV loops, absent/malformed/unsupported records and widened endpoint bounds");
}

typedef struct {
    unsigned scans, decodes, analyses;
    bool stop_after_decode;
} BatchProgress;

static void batch_progress(void *user, DmSampleBatchStage stage, const char *path, size_t index,
                           size_t total, uint64_t done, uint64_t units) {
    BatchProgress *p = user;
    assert(path && index < total && done <= units);

    if (stage == DM_SAMPLE_SCAN_PATHS) {
        ++p->scans;
    } else if (stage == DM_SAMPLE_DECODE_FILE) {
        ++p->decodes;
    } else if (stage == DM_SAMPLE_ANALYZE_FILE) {
        ++p->analyses;
    }
}

static bool batch_canceled(void *user) {
    const BatchProgress *p = user;
    return p->stop_after_decode && p->analyses;
}

static void check_batch(const char *directory) {
    char folder[DM_PATH], path[DM_PATH], input[DM_PATH], error[512] = {0};
    snprintf(folder, sizeof(folder), "%s/batch-fixture", directory);
    assert(SDL_CreateDirectory(folder));
    snprintf(path, sizeof(path), "%s/subfolder", folder);
    assert(SDL_CreateDirectory(path));
    snprintf(path, sizeof(path), "%s/subfolder/ignored.aiff", folder);
    DmOutput output;
    assert(dm_output_begin(&output, path, error, sizeof(error)));
    assert(dm_output_write(&output, "bad", 3, error, sizeof(error)));
    assert(dm_output_finish(&output, true, error, sizeof(error)));
    snprintf(path, sizeof(path), "%s/alpha.wav", folder);
    assert(dm_output_begin(&output, path, error, sizeof(error)));
    assert(dm_output_write(&output, "bad", 3, error, sizeof(error)));
    assert(dm_output_finish(&output, true, error, sizeof(error)));
    snprintf(path, sizeof(path), "%s/Zulu_a4.wav", folder);
    snprintf(input, sizeof(input), "%s/codec-0-0.wav", directory);
    DmBytes bytes = {0};
    assert(dm_read(input, &bytes, error, sizeof(error)));
    assert(dm_output_begin(&output, path, error, sizeof(error)));
    assert(dm_output_write(&output, bytes.data, bytes.size, error, sizeof(error)));
    assert(dm_output_finish(&output, true, error, sizeof(error)));
    dm_bytes_free(&bytes);

    const char *paths[] = {folder, input};
    DmSampleBatch batch = {0};
    BatchProgress progress = {0};
    DmSampleBatchCallbacks cb = {&progress, batch_canceled, batch_progress};
    assert(dm_sample_batch_import(paths, 2, &cb, &batch, error, sizeof(error)));
    assert(batch.count == 3 && progress.scans == 4 && progress.analyses == 6);
    assert(strstr(batch.items[0].path, "alpha.wav") && batch.items[0].error[0]);
    assert(strstr(batch.items[1].path, "Zulu_a4.wav") && !batch.items[1].error[0]);
    assert(batch.items[1].analysis.root == 69 && batch.items[1].absolute_peak > 0.24f);
    assert(!strcmp(batch.items[2].path, input) && batch.items[2].peaks);
    dm_sample_batch_free(&batch);

    progress = (BatchProgress){.stop_after_decode = true};
    assert(!dm_sample_batch_import(paths, 2, &cb, &batch, error, sizeof(error)));
    assert(!batch.items && !batch.count && strstr(error, "canceled"));
    puts(
        "PASS ordered directory and multi-path imports, retained errors and analysis cancellation");
}

static void check_analysis(void) {
    enum { RATE = 48000, FRAMES = 12000 };

    float stereo[FRAMES * 2];
    DmSampleAudio sample = {
        .pcm = stereo, .frames = FRAMES, .rate = RATE, .channels = 2, .embedded_root = -1};

    for (size_t frame = 0; frame < FRAMES; ++frame) {
        double phase = 6.283185307179586 * frame / 120;
        stereo[frame * 2] = (float)(0.5 * sin(phase));
        stereo[frame * 2 + 1] = -0.4f * stereo[frame * 2];
    }

    DmSampleAnalysis analysis;
    assert(dm_sample_analyze(&sample, "tone.wav", NULL, &analysis));
    assert(analysis.source == DM_SAMPLE_PITCH_SIGNAL && analysis.root == 67);
    assert(fabs(analysis.frequency - 400) < 0.5 && analysis.confidence > 0.95);
    assert(analysis.program == -1 && analysis.drum_key == -1);
    assert(dm_sample_analyze(&sample, "piano_C#4_take.wav", NULL, &analysis));
    assert(analysis.source == DM_SAMPLE_PITCH_FILENAME && analysis.root == 61);
    assert(analysis.conflicting_pitch && analysis.program == 0);
    assert(dm_sample_analyze(&sample, "C4-C#4.wav", NULL, &analysis));
    assert(analysis.source == DM_SAMPLE_PITCH_SIGNAL && analysis.conflicting_pitch);
    sample.embedded_root = 71;
    sample.embedded_cents = -12;
    assert(dm_sample_analyze(&sample, "kick_C4.wav", NULL, &analysis));
    assert(analysis.source == DM_SAMPLE_PITCH_FILENAME && analysis.root == 60);
    assert(analysis.cents == 0 && analysis.drum_key == 36 && analysis.conflicting_pitch);
    assert(dm_sample_analyze(&sample, "kick.wav", NULL, &analysis));
    assert(analysis.source == DM_SAMPLE_PITCH_EMBEDDED && analysis.root == 71 &&
           analysis.cents == -12);
    sample.embedded_root = 72;
    assert(dm_sample_analyze(&sample, "E.PIAN2 C4.wav", NULL, &analysis));
    assert(analysis.root == 60 && analysis.source == DM_SAMPLE_PITCH_FILENAME &&
           analysis.conflicting_pitch);
    sample.embedded_root = 84;
    assert(dm_sample_analyze(&sample, "E.PIAN2 C5.wav", NULL, &analysis));
    assert(analysis.root == 72 && analysis.source == DM_SAMPLE_PITCH_FILENAME &&
           analysis.conflicting_pitch);

    DmSamplePeak bins[8];
    float absolute;
    assert(dm_sample_peaks(&sample, bins, 8, &absolute, NULL));
    assert(fabs(absolute - 0.5) < 0.0001 && fabs(bins[2].minimum[1] + 0.2) < 0.0001);
    DmSampleLoop loop;
    assert(dm_sample_loop_snap(&sample, (DmSampleLoop){130, 957}, &loop));
    assert(dm_sample_loop_valid(loop, sample.frames) && (loop.end - loop.start) % 120 == 0);
    assert(!dm_sample_loop_snap(&sample, (DmSampleLoop){120, 120}, &loop));

    sample.embedded_root = -1;
    uint32_t noise = 1;

    for (size_t i = 0; i < sizeof(stereo) / sizeof(*stereo); ++i) {
        noise = noise * 1664525u + 1013904223u;
        stereo[i] = (float)(noise >> 16) / 32768 - 1;
    }

    assert(dm_sample_analyze(&sample, "noise.wav", NULL, &analysis));
    assert(analysis.source == DM_SAMPLE_PITCH_DEFAULT && analysis.root == 60);
    memset(stereo, 0, sizeof(stereo));
    stereo[200] = stereo[201] = 0.9f;
    assert(dm_sample_analyze(&sample, "transient.wav", NULL, &analysis));
    assert(analysis.source == DM_SAMPLE_PITCH_DEFAULT && analysis.drum_key == -1);
    assert(analysis.envelope.attack == 0.001);
    memset(stereo, 0, sizeof(stereo));
    assert(dm_sample_peaks(&sample, bins, 8, &absolute, NULL));
    assert(absolute == 0 && bins[0].minimum[0] == 0 && bins[0].maximum[1] == 0);
    puts("PASS sample pitch precedence, transients, noise, stereo peaks and loop bounds");
}

static bool cancel_publication(void *user) {
    int *checks = user;
    return ++*checks >= 4;
}

static void DME_CALL count_missing(void *user, uint32_t severity, uint32_t flags,
                                   const char *message) {
    unsigned *count = user;
    assert(severity == DME_WARNING && flags == DME_DISPLAY && strstr(message, "missing"));
    ++*count;
}

static void check_anchored_reader(const char *root, const DmSampleManifest *set) {
    char error[256] = {0}, path[DM_PATH], moved[DM_PATH];
    DmSampleManifest manifest = {0};
    DmSampleLibraryReader *reader =
        dm_sample_library_reader_open(root, set->id, &manifest, error, sizeof(error));
    assert(reader && !strcmp(manifest.id, set->id));
    snprintf(path, sizeof(path), "%s/%s", root, manifest.directory);
    snprintf(moved, sizeof(moved), "%s/reader-held-set", root);
    bool renamed = SDL_RenamePath(path, moved);

    if (renamed) {
        assert(SDL_CreateDirectory(path));
    }

    DmSampleAudio audio = {0};
    assert(
        dm_sample_library_reader_audio(reader, &manifest, 0, NULL, &audio, error, sizeof(error)));
    assert(audio.pcm[200] == 0.5f && audio.pcm[201] == -0.125f);
    dm_sample_audio_free(&audio);
    dm_sample_library_reader_close(reader);

    if (renamed) {
        assert(SDL_RemovePath(path));
        assert(SDL_RenamePath(moved, path));
    } else {
        /* Windows denies publication while the physical set is being read. */
        assert(SDL_RenamePath(path, moved));
        assert(SDL_RenamePath(moved, path));
    }

    dm_sample_manifest_free(&manifest);
    puts("PASS anchored set reads and directory release after preparation");
}

static void check_frozen_export(const char *root, const DmSampleManifest *set, const char *asset) {
    check_anchored_reader(root, set);
    DmProject project;
    dm_project_init(&project);
    DmPatchReplacement replacement = {.key = {.bank = 130}, .generation = 2};
    assert(strlen(root) < sizeof(replacement.locator));
    strcpy(replacement.locator, root);
    strcpy(replacement.set_id, set->id);
    strcpy(replacement.preset_id, set->presets[0].id);
    assert(dm_patch_replacement_set(&project, &replacement));
    replacement.key.port = 2;
    replacement.generation = 3;
    assert(dm_patch_replacement_set(&project, &replacement));
    replacement.key.port = 3;
    strcpy(replacement.preset_id, "missing");
    assert(dm_patch_replacement_set(&project, &replacement));
    const DmeEvent events[] = {
        {0, 1, MIDI_CONTROL, MIDI_CC_BANK, 1, 1},
        {0, 2, MIDI_CONTROL, MIDI_CC_BANK_LSB, 2, 1},
        {0, 3, MIDI_PROGRAM, 0, 0, 1},
        {0, 4, MIDI_NOTE_ON, 60, 100, 1},
        {0, 5, MIDI_PROGRAM, 73, 0, 1},
        {0, 6, MIDI_NOTE_ON, 60, 110, 1},
        {1, 7, MIDI_NOTE_OFF, 60, 0, 1},
        {1, 8, MIDI_NOTE_ON, 60, 0, 1},
        {1, 9, MIDI_PROGRAM, 0, 0, 1},
        {2, 10, MIDI_NOTE_ON, 62, 100, 1},
        {2, 11, MIDI_CONTROL, MIDI_CC_BANK, 1, 2},
        {2, 12, MIDI_CONTROL, MIDI_CC_BANK_LSB, 2, 2},
        {2, 13, MIDI_PROGRAM, 0, 0, 2},
        {2, 14, MIDI_NOTE_ON, 60, 100, 2},
        {2, 15, MIDI_CONTROL, MIDI_CC_BANK, 1, 3},
        {2, 16, MIDI_CONTROL, MIDI_CC_BANK_LSB, 2, 3},
        {2, 17, MIDI_PROGRAM, 0, 0, 3},
        {2, 18, MIDI_NOTE_ON, 60, 100, 3},
    };
    DmeContext context = {.events = events, .event_count = sizeof(events) / sizeof(*events)};
    unsigned warnings = 0;
    DmeHost host = {.user = &warnings, .diagnostic = count_missing};
    DmSampleExportSnapshot *snapshot = NULL;
    assert(dm_sample_export_snapshot(&project, &context, &host, &snapshot));
    assert(snapshot && warnings == 1 && context.sample_preset_count == 1 &&
           context.sample_asset_count == 1 && context.sample_zone_count == 1);
    const DmeEffectivePatch *patch = context.effective_patches;
    assert(patch[3].sample_preset == 1 && patch[3].bank == 130 && patch[3].program == 0);
    assert(!patch[5].sample_preset && patch[5].program == 73);
    assert(patch[6].sample_preset == 1 && patch[6].program == 0);
    assert(!patch[7].sample_preset && patch[7].program == 73);
    assert(patch[9].sample_preset == 1 && patch[13].sample_preset == 1);
    assert(patch[17].bank == 130 && !patch[17].sample_preset &&
           (patch[17].flags & DME_SAMPLE_MISSING));
    assert(context.sample_presets[0].generation == set->generation &&
           context.sample_zones[0].asset == 0);
    dm_project_free(&project);

    DmPatchReplacement selections[2] = {replacement, replacement};
    strcpy(selections[0].preset_id, set->presets[0].id);
    selections[1] = selections[0];
    DmeContext selected = {0};
    DmSampleExportSnapshot *selected_snapshot = NULL;
    uint32_t indices[2] = {0};
    char error[256] = {0};
    assert(dm_sample_export_selection_snapshot(selections, 2, &selected, &host, indices,
                                               &selected_snapshot, error, sizeof(error)));
    assert(indices[0] == 1 && indices[1] == 1 && selected.sample_preset_count == 1 &&
           selected.sample_asset_count == 1 && selected.sample_zone_count == 1 &&
           !selected.effective_patches && !selected.event_count);
    assert(!strcmp(selected.sample_presets[0].set_id.data, set->id) &&
           !strcmp(selected.sample_presets[0].preset_id.data, set->presets[0].id));
    DmSampleExportSnapshot *failed_snapshot = NULL;
    DmeContext unchanged = selected;
    strcpy(selections[1].preset_id, "missing");
    uint32_t failed_indices[2] = {42, 43};
    assert(!dm_sample_export_selection_snapshot(selections, 2, &selected, &host, failed_indices,
                                                &failed_snapshot, error, sizeof(error)));
    assert(!failed_snapshot && failed_indices[0] == 42 && failed_indices[1] == 43 &&
           !memcmp(&selected, &unchanged, sizeof(selected)));
    assert(SDL_RemovePath(asset));
    float pcm[4];
    assert(dm_sample_export_read(snapshot, 0, 100, 2, pcm, 4) == DME_OK);
    assert(pcm[0] == 0.5f && pcm[1] == -0.125f);
    assert(dm_sample_export_read(snapshot, 0, 100, 2, pcm, 3) == DME_INVALID);
    assert(dm_sample_export_read(selected_snapshot, 0, 100, 2, pcm, 4) == DME_OK &&
           pcm[0] == 0.5f && pcm[1] == -0.125f);
    dm_sample_export_free(snapshot);
    dm_sample_export_free(selected_snapshot);
    puts("PASS frozen sample PCM, banks, same-tick patches, note releases and missing fallback");
    puts("PASS export-only saved selections, deduplication, atomic failure and retained PCM");
}

static void check_stock_export_identity(void) {
    DmProject project;
    dm_project_init(&project);
    const DmeEvent events[] = {
        {0, 11, MIDI_CONTROL, MIDI_CC_BANK, 1, 2},
        {0, 12, MIDI_CONTROL, MIDI_CC_BANK_LSB, 2, 2},
        {0, 13, MIDI_PROGRAM, 7, 0, 2},
        {0, 14, MIDI_NOTE_ON, 60, 111, 2},
        {0, 15, MIDI_PROGRAM, 8, 0, 2},
        {1, 16, MIDI_NOTE_OFF, 60, 0, 2},
    };
    DmeContext context = {.events = events, .event_count = sizeof(events) / sizeof(*events)};
    DmSampleExportSnapshot *snapshot = NULL;
    assert(dm_sample_export_snapshot(&project, &context, NULL, &snapshot));
    assert(snapshot && !context.sample_preset_count && context.effective_patches);
    assert(context.effective_patches[3].bank == 130 && context.effective_patches[3].program == 7 &&
           context.effective_patches[3].note_velocity == 111 &&
           context.effective_patches[3].note_source_id == 14);
    assert(context.effective_patches[5].bank == 130 && context.effective_patches[5].program == 7 &&
           context.effective_patches[5].note_source_id == 14);
    dm_sample_export_free(snapshot);
    dm_project_free(&project);
    puts("PASS original-route identity and paired releases without playback sample assignments");
}

static SDL_EnumerationResult SDLCALL count_entry(void *user, const char *path, const char *name) {
    (void)path;
    (void)name;
    ++*(unsigned *)user;
    return SDL_ENUM_CONTINUE;
}

static void check_library(const char *directory, DmSampleManifest *set) {
    char program[DM_PATH], writable[DM_PATH], path[DM_PATH];
    SDL_Time now;
    assert(SDL_GetCurrentTime(&now));
    unsigned long long serial = (unsigned long long)now;
    snprintf(program, sizeof(program), "%s/program-%llu", directory, serial);
    snprintf(writable, sizeof(writable), "%s/library-%llu", directory, serial);
    assert(SDL_CreateDirectory(program) && SDL_CreateDirectory(writable));
    set->assets[0].frames = 8001;
    set->assets[0].channels = 2;
    set->presets[0].suggested_program = 5;
    set->zones[0].tone = (DmeSampleToneParams){.coarse = 12,
                                               .custom_tracking = 1,
                                               .tracking = 75,
                                               .cutoff = 1200,
                                               .resonance = 6,
                                               .filter_depth = 1000,
                                               .vibrato_rate = 5,
                                               .vibrato_depth = 40,
                                               .chorus = .2,
                                               .reverb = .3};
    float pcm[8001 * 2];

    for (size_t i = 0; i < sizeof(pcm) / sizeof(*pcm); i += 2) {
        pcm[i] = 0.5f;
        pcm[i + 1] = -0.125f;
    }

    DmSampleAudio audio = {.pcm = pcm,
                           .frames = 8001,
                           .rate = 32000,
                           .channels = 2,
                           .embedded_root = 71,
                           .embedded_cents = -12,
                           .embedded_loop_start = 64,
                           .embedded_loop_end = 128};
    set->zones[0].playback = (DmSampleLoop){32, 1200};
    set->zones[0].playback_set = true;
    char error[512] = {0};
    bool published = dm_sample_library_publish(program, set, &audio, NULL, error, sizeof(error));

    if (!published) {
        fprintf(stderr, "Sample library publication: %s\n", error);
    }

    assert(published);
    assert(dm_sample_library_publish(writable, set, &audio, NULL, error, sizeof(error)));
    assert(dm_sample_library_publish(writable, set, &audio, NULL, error, sizeof(error)));

    DmSampleManifest loaded = {0};
    assert(dm_sample_library_manifest(writable, set->id, &loaded, error, sizeof(error)));
    assert(loaded.generation == set->generation && loaded.zones[0].loop.end == 800);
    assert(loaded.presets[0].suggested_program == 5 && loaded.zones[0].tone.coarse == 12 &&
           loaded.zones[0].tone.custom_tracking && loaded.zones[0].tone.tracking == 75 &&
           loaded.zones[0].tone.cutoff == 1200 && loaded.zones[0].tone.reverb == .3);
    DmSampleAudio decoded = {0};
    assert(dm_sample_library_audio(writable, &loaded, 0, NULL, &decoded, error, sizeof(error)));
    assert(decoded.rate == 32000 && decoded.channels == 2 && decoded.frames == 8001);
    assert(decoded.pcm[0] == 0.5f && decoded.pcm[1] == -0.125f);
    assert(decoded.embedded_root == 71 && decoded.embedded_cents == -12 &&
           decoded.embedded_loop_start == 64 && decoded.embedded_loop_end == 128);
    assert(loaded.zones[0].playback.start == 32 && loaded.zones[0].playback.end == 1200 &&
           loaded.zones[0].playback_set);
    dm_sample_audio_free(&decoded);
    dm_sample_manifest_free(&loaded);

    snprintf(path, sizeof(path), "%s/%s/tone.sfz", writable, set->name);
    DmBytes sfz = {0};
    assert(dm_read(path, &sfz, error, sizeof(error)));
    assert(dm_reserve((void **)&sfz.data, &sfz.capacity, sfz.size + 1, 1));
    sfz.data[sfz.size] = 0;
    assert(
        strstr((char *)sfz.data, "loop_mode=loop_sustain") &&
        strstr((char *)sfz.data, "loop_start=100 loop_end=799") &&
        strstr((char *)sfz.data, "offset=32 end=1199") &&
        strstr((char *)sfz.data, "group=2 off_by=3") && strstr((char *)sfz.data, "tune=12.0000") &&
        strstr((char *)sfz.data, "transpose=12") && strstr((char *)sfz.data, "pitch_keytrack=75") &&
        strstr((char *)sfz.data, "cutoff=1200.0000") &&
        strstr((char *)sfz.data, "fileg_depth=1000") &&
        strstr((char *)sfz.data, "pitchlfo_freq=5.0000") &&
        strstr((char *)sfz.data, "effect1=30.0000") && strstr((char *)sfz.data, "effect2=20.0000"));
    dm_bytes_free(&sfz);

    uint32_t previous_generation = set->generation;
    char previous_name[DM_SAMPLE_NAME_CAP];
    snprintf(previous_name, sizeof(previous_name), "%s", set->name);
    strcpy(set->name, "Renamed set");
    set->zones[0].envelope.attack = .05;
    int checks = 0;
    DmSampleImportCallbacks callbacks = {&checks, cancel_publication, NULL};
    assert(!dm_sample_library_publish(writable, set, &audio, &callbacks, error, sizeof(error)));
    assert(checks >= 4 && strstr(error, "canceled"));
    assert(dm_sample_library_manifest(writable, set->id, &loaded, error, sizeof(error)));
    assert(loaded.generation == previous_generation && !strcmp(loaded.name, previous_name));
    dm_sample_manifest_free(&loaded);
    DmSampleAudio bad = audio;
    bad.rate = 44100;
    assert(!dm_sample_library_publish(writable, set, &bad, NULL, error, sizeof(error)));
    assert(dm_sample_library_manifest(writable, set->id, &loaded, error, sizeof(error)));
    assert(loaded.generation == previous_generation);
    dm_sample_manifest_free(&loaded);
    assert(dm_sample_library_publish(writable, set, &audio, NULL, error, sizeof(error)));
    assert(dm_sample_library_manifest(writable, set->id, &loaded, error, sizeof(error)));
    assert(loaded.generation == set->generation && loaded.generation != previous_generation);
    dm_sample_manifest_free(&loaded);
    snprintf(path, sizeof(path), "%s/%s", writable, previous_name);
    SDL_PathInfo gone;
    assert(!SDL_GetPathInfo(path, &gone));
    snprintf(path, sizeof(path), "%s/%s/current", writable, set->name);
    assert(!SDL_GetPathInfo(path, &gone));
    snprintf(path, sizeof(path), "%s/%s/.publish.lock", writable, set->name);
    assert(!SDL_GetPathInfo(path, &gone));

    char foreign[DM_PATH];
    snprintf(foreign, sizeof(foreign), "%s/%s/keep.txt", writable, set->name);
    assert(dm_write(foreign, "keep", 4, error, sizeof(error)));
    assert(!dm_sample_library_publish(writable, set, &audio, NULL, error, sizeof(error)));
    assert(SDL_GetPathInfo(foreign, &gone) && gone.size == 4);
    assert(SDL_RemovePath(foreign));
    snprintf(foreign, sizeof(foreign), "%s/Occupied", writable);
    assert(SDL_CreateDirectory(foreign));
    strcpy(set->name, "Occupied");
    assert(!dm_sample_library_publish(writable, set, &audio, NULL, error, sizeof(error)));
    strcpy(set->name, "Renamed set");
    assert(SDL_RemovePath(foreign));
    assert(!dm_sample_name_valid("../escape") && !dm_sample_name_valid("CON") &&
           !dm_sample_name_valid("ends.") && !dm_sample_name_valid("bad:name") &&
           dm_sample_name_valid("My sample C4"));
    assert(!dm_sample_asset_filename_valid("one/two.wav") &&
           !dm_sample_asset_filename_valid("one/one.wav") &&
           !dm_sample_asset_filename_valid("../one.wav") &&
           dm_sample_asset_filename_valid("one.wav"));
    snprintf(set->assets[0].file, sizeof(set->assets[0].file), "My sample C4.wav");
    assert(dm_sample_library_publish(writable, set, &audio, NULL, error, sizeof(error)));
    snprintf(path, sizeof(path), "%s/%s/tone.wav", writable, set->name);
    assert(!SDL_GetPathInfo(path, &gone));
    snprintf(path, sizeof(path), "%s/%s/tone.sfz", writable, set->name);
    assert(!SDL_GetPathInfo(path, &gone));
    snprintf(path, sizeof(path), "%s/%s/My sample C4.sfz", writable, set->name);
    assert(SDL_GetPathInfo(path, &gone) && gone.type == SDL_PATHTYPE_FILE);
    snprintf(path, sizeof(path), "%s/%s/%s", writable, set->name, set->assets[0].file);
    assert(SDL_GetPathInfo(path, &gone) && gone.type == SDL_PATHTYPE_FILE);
    unsigned entries = 0;
    assert(SDL_EnumerateDirectory(writable, count_entry, &entries) && entries == 1);
    entries = 0;
    snprintf(path, sizeof(path), "%s/%s", writable, set->name);
    assert(SDL_EnumerateDirectory(path, count_entry, &entries) && entries == 3);

    snprintf(path, sizeof(path), "%s/set-invalid", writable);
    assert(SDL_CreateDirectory(path));
    DmSampleCatalog catalog = {0};
    assert(dm_sample_library_catalog(program, writable, NULL, &catalog, error, sizeof(error)));
    assert(catalog.count == 1 && catalog.entries[0].writable_root &&
           catalog.entries[0].generation == set->generation &&
           !strcmp(catalog.entries[0].name, set->name));
    dm_sample_catalog_free(&catalog);

#ifndef _WIN32
    snprintf(path, sizeof(path), "%s/Redirected", writable);
    char absolute[DM_PATH];
    assert(realpath(program, absolute));
    assert(symlink(absolute, path) == 0);
    strcpy(set->id, "redirected");
    strcpy(set->name, "Redirected");
    assert(!dm_sample_library_publish(writable, set, &audio, NULL, error, sizeof(error)));
    strcpy(set->id, "generated");
    strcpy(set->name, "Renamed set");
#endif

    snprintf(path, sizeof(path), "%s/%s/%s", writable, set->name, set->assets[0].file);
    check_frozen_export(writable, set, path);
    assert(!dm_sample_library_audio(writable, set, 0, NULL, &decoded, error, sizeof(error)));
    puts(
        "PASS single-copy named storage, rename, cancellation, retry, WAV, SFZ and catalog precedence");
}

static void check_manifest(const char *directory) {
    static const char manifest[] =
        "{\"version\":3,\"id\":\"generated\",\"name\":\"A \\\"quoted\\\" set\","
        "\"assets\":[{\"id\":\"tone\",\"file\":\"tone.wav\",\"frames\":1000,\"rate\":32000,\"channels\":1}],"
        "\"presets\":[{\"id\":\"tone\",\"name\":\"M\\u00e9lodie\",\"drums\":false,\"zones\":[{"
        "\"asset\":\"tone\",\"root\":60,\"cents\":-12,\"keyLo\":0,\"keyHi\":127,"
        "\"velocityLo\":1,\"velocityHi\":127,\"gainDb\":0,\"pan\":0,\"cut\":2,\"cutBy\":3,"
        "\"envelope\":{\"delay\":0,\"attack\":0.01,\"hold\":0,\"decay\":0.2,\"sustain\":0.9,\"release\":0.3},"
        "\"loop\":{\"mode\":\"sustain\",\"start\":100,\"end\":800}}]}]}";
    char error[256] = {0};
    DmSampleManifest set = {0};
    assert(dm_sample_manifest_parse(manifest, strlen(manifest), &set, error, sizeof(error)));
    assert(!strcmp(set.name, "A \"quoted\" set") && set.asset_count == 1);
    assert(!strcmp(set.presets[0].name, "M\xc3\xa9lodie") && !set.presets[0].drums);
    assert(set.zones[0].loop_mode == DM_SAMPLE_SUSTAIN_LOOP && set.zones[0].loop.end == 800);
    assert(set.zones[0].root == 60 && set.zones[0].cents == -12 && set.zones[0].cut_group == 2 &&
           set.zones[0].cut_by == 3);

    snprintf(set.assets[0].source_name, sizeof(set.assets[0].source_name), "Original C4.wav");
    size_t length;
    char *written = dm_sample_manifest_write(&set, &length);
    assert(written && length && !strstr(written, "revision") && !strstr(written, "generation"));
    DmSampleManifest reload = {0};
    assert(dm_sample_manifest_parse(written, length, &reload, error, sizeof(error)));
    assert(reload.zones[0].cut_group == 2 && reload.zones[0].cut_by == 3);
    assert(!strcmp(reload.assets[0].source_name, "Original C4.wav"));
    size_t repeated_length;
    char *repeated = dm_sample_manifest_write(&reload, &repeated_length);
    assert(repeated && repeated_length == length && !memcmp(written, repeated, length));
    free(repeated);
    free(written);
    dm_sample_manifest_free(&reload);

    static const char *const bad[][2] = {
        {"\"version\":3", "\"version\":2"},
        {"\"version\":3", "\"version\":3,\"versi\\u006fn\":1"},
        {"tone.wav", "tone/tone.wav"},
        {"tone.wav", "../tone.wav"},
        {"tone.wav", "con.wav"},
        {"tone.wav", "tone.WAV"},
        {"tone.wav", "tone.wav\\u0000.txt"},
        {"\"end\":800", "\"end\":1001"},
        {"\"drums\":false", "\"drums\":true"},
        {"\"asset\":\"tone\"", "\"asset\":\"missing\""},
        {"\"loop\":", "\"tone\":{\"cutoff\":10},\"loop\":"},
        {"\"loop\":", "\"tone\":{\"coarse\":0.5},\"loop\":"},
        {"\"loop\":", "\"tone\":{\"tracking\":75},\"loop\":"},
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        const char *match = strstr(manifest, bad[i][0]);
        assert(match);
        char invalid[sizeof(manifest) + 64];
        int size = snprintf(invalid, sizeof(invalid), "%.*s%s%s", (int)(match - manifest), manifest,
                            bad[i][1], match + strlen(bad[i][0]));
        assert(size > 0 && size < (int)sizeof(invalid));
        assert(!dm_sample_manifest_parse(invalid, (size_t)size, &reload, error, sizeof(error)));
        assert(!reload.assets && !reload.presets && !reload.zones);
    }

    set.zones[0].loop.end = 2000;
    assert(!dm_sample_manifest_write(&set, &length) && !length);
    set.zones[0].loop.end = 800;
    strcpy(set.name, "Generated set");
    check_library(directory, &set);
    dm_sample_manifest_free(&set);
    puts("PASS manifest determinism, escapes, bounds, duplicate keys and portable names");
}

enum { CODEC_FIXTURES = 2 * DM_AUDIO_CODECS + 1 };

static void check_codec_library(const char *directory, DmSampleAudio *samples) {
    char root[DM_PATH], error[512] = {0};
    SDL_Time now;
    assert(SDL_GetCurrentTime(&now));
    snprintf(root, sizeof(root), "%s/codecs-%llu", directory, (unsigned long long)now);
    assert(SDL_CreateDirectory(root));
    DmSampleAsset assets[CODEC_FIXTURES] = {0};
    DmSamplePreset presets[CODEC_FIXTURES] = {0};
    DmSampleZone zones[CODEC_FIXTURES];
    DmSampleManifest set = {.id = "codec-fixtures",
                            .name = "Codec fixtures",
                            .generation = 1,
                            .asset_count = CODEC_FIXTURES,
                            .preset_count = CODEC_FIXTURES,
                            .zone_count = CODEC_FIXTURES,
                            .assets = assets,
                            .presets = presets,
                            .zones = zones};

    for (unsigned i = 0; i < CODEC_FIXTURES; ++i) {
        const DmSampleAudio *sample = &samples[i];
        DmSampleAsset *asset = &assets[i];
        DmSamplePreset *preset = &presets[i];
        asset->frames = (uint32_t)sample->frames;
        asset->rate = (uint32_t)sample->rate;
        asset->channels = (uint8_t)sample->channels;
        snprintf(asset->id, sizeof(asset->id), "audio-%u", i);
        snprintf(asset->file, sizeof(asset->file), "%s.wav", asset->id);
        snprintf(preset->id, sizeof(preset->id), "%s", asset->id);
        snprintf(preset->name, sizeof(preset->name), "Audio %u", i + 1);
        preset->first_zone = i;
        preset->zone_count = 1;
        zones[i] = (DmSampleZone){.asset = i,
                                  .root = 69,
                                  .key_hi = 127,
                                  .velocity_lo = 1,
                                  .velocity_hi = 127,
                                  .envelope.sustain = 1};
    }

    assert(dm_sample_library_publish(root, &set, samples, NULL, error, sizeof(error)));
    DmSampleCatalog catalog = {0};
    assert(dm_sample_library_catalog(NULL, root, NULL, &catalog, error, sizeof(error)));
    assert(catalog.count == 1 && catalog.entries[0].generation == set.generation &&
           !strcmp(catalog.entries[0].id, set.id));
    dm_sample_catalog_free(&catalog);
    DmSampleManifest loaded = {0};
    assert(dm_sample_library_manifest(root, set.id, &loaded, error, sizeof(error)));
    assert(loaded.asset_count == CODEC_FIXTURES && loaded.preset_count == CODEC_FIXTURES);

    for (unsigned i = 0; i < CODEC_FIXTURES; ++i) {
        DmSampleAudio *sample = &samples[i];
        DmSampleAudio restored = {0};
        assert(dm_sample_library_audio(root, &loaded, i, NULL, &restored, error, sizeof(error)));
        assert(restored.frames == sample->frames && restored.rate == sample->rate &&
               restored.channels == sample->channels);

        for (size_t v = 0; v < sample->frames * (size_t)sample->channels; ++v) {
            assert(fabsf(restored.pcm[v] - sample->pcm[v]) <= 2.0f / 8388608);
        }

        dm_sample_audio_free(&restored);
        dm_sample_audio_free(sample);
        assert(!sample->pcm && !sample->frames);
    }

    dm_sample_manifest_free(&loaded);
    puts("PASS all imported codecs publish, discover and reload with native audio geometry");
}

/* A non-frame-aligned stereo signal exercises buffering, padding and flushing. */
int main(int argc, char **argv) {
    assert(argc == 2);
    char path[DM_PATH], error[512];
    DmSampleAudio samples[CODEC_FIXTURES];

    for (int codec = 0; codec < DM_AUDIO_CODECS; ++codec) {
        for (int variant = 0; variant < 2; ++variant) {
            DmAudioOptions options = dm_audio_options_default();
            options.codec = (DmAudioCodec)codec;

            if (codec == DM_AUDIO_WAV || codec == DM_AUDIO_FLAC) {
                options.bits = variant ? 24 : 16;
            } else {
                options.variable = variant != 0;
            }

            snprintf(path, sizeof(path), "%s/codec-%d-%d.%s", argv[1], codec, variant,
                     dm_audio_codec_extension(options.codec));
            error[0] = 0;
            DmOutput output;
            assert(dm_output_begin(&output, path, error, sizeof(error)));
            DmAudioEncoder *encoder =
                dm_audio_encoder_begin(&output, &options, error, sizeof(error));

            if (!encoder) {
                fprintf(stderr, "%s: %s\n", path, error);
            }

            assert(encoder);

            for (int start = 0; start < 52817;) {
                float stereo[514];
                int frames = 52817 - start;

                if (frames > 257) {
                    frames = 257;
                }

                for (int i = 0; i < frames; ++i) {
                    double t = (start + i) / 48000.0;
                    stereo[2 * i] = (float)(0.25 * sin(6.283185307179586 * 440 * t));
                    stereo[2 * i + 1] = (float)(0.125 * sin(6.283185307179586 * 660 * t));
                }

                assert(dm_audio_encoder_write(encoder, stereo, frames));
                start += frames;
            }

            bool ok = dm_audio_encoder_finish(encoder, true);

            if (!ok) {
                fprintf(stderr, "%s: %s\n", path, error);
            }

            assert(ok && dm_output_finish(&output, true, error, sizeof(error)));
            DmBytes bytes = {0};
            assert(dm_read(path, &bytes, error, sizeof(error)) && bytes.size > 64);
            dm_bytes_free(&bytes);
            samples[2 * codec + variant] = check_import(path, codec);
            printf("PASS %s\n", path);
        }
    }

    /* Aborting an export must discard its temporary output. */
    snprintf(path, sizeof(path), "%s/cancelled.wav", argv[1]);
    DmOutput output;
    error[0] = 0;
    assert(dm_output_begin(&output, path, error, sizeof(error)));
    DmAudioOptions options = dm_audio_options_default();
    DmAudioEncoder *encoder = dm_audio_encoder_begin(&output, &options, error, sizeof(error));
    assert(encoder);
    dm_audio_encoder_finish(encoder, false);
    dm_output_finish(&output, false, error, sizeof(error));
    FILE *file = fopen(output.temporary, "rb");
    assert(!file);
    file = fopen(path, "rb");
    assert(!file);
    puts("PASS cancellation removes the temporary output");
    samples[CODEC_FIXTURES - 1] = check_aiff(argv[1]);
    puts("PASS AIFF metadata, canceled import and corrupt input");
    check_wave_loops(argv[1]);
    check_codec_library(argv[1], samples);
    check_batch(argv[1]);
    check_analysis();
    check_stock_export_identity();
    check_manifest(argv[1]);
    return 0;
}
