/* Rendering audio: every codec on the worker thread, the suggested file name, the remembered
   destination, and the Export window's progress and cancellation. */
#include "editor/internal.h"
#include <assert.h>
#define TEST_NAME "audio render"
#include "test_common.h"
extern uint64_t recording_feedback_worker_thread(DmAudio *);
extern void test_audio_render_ui(Editor *);

/* One short pattern on the playlist. */
static void fixture(Editor *e) {
    dm_project_free(&e->project);
    dm_project_init(&e->project);
    e->active = true;
    e->selected_pattern = dm_pattern_add(&e->project, "Render");
    e->selected_channel = e->pattern_channel = 0;
    DmPattern *p = ed_pattern(e);
    p->length = 1;
    assert(dm_note_add(p, (DmNote){0, .5, 60, 100, 0, 0}));
    assert(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0}));
}

static void codecs(Editor *e) {
    snprintf(e->project.soundfont, sizeof(e->project.soundfont),
             "agents/reports/generated-programs.sf2");

    for (int codec = 0; codec < DM_AUDIO_CODECS; codec++) {
        for (int variable = 0; variable < 2; variable++) {
            if (variable && (codec == DM_AUDIO_WAV || codec == DM_AUDIO_FLAC)) {
                continue;
            }

            DmAudioOptions options = dm_audio_options_default();
            options.codec = (DmAudioCodec)codec;
            options.variable = variable != 0;

            if (codec == DM_AUDIO_FLAC || codec == DM_AUDIO_WAV) {
                options.bits = 24;
            }

            char path[DM_PATH], error[512];
            snprintf(path, sizeof(path), "agents/reports/audio-render/codec-%d-%d.%s", codec,
                     variable, dm_audio_codec_extension(options.codec));
            assert(dm_audio_export(e->audio, &e->project, path, &options, error, sizeof(error)));
            assert(recording_feedback_worker_thread(e->audio) != SDL_GetCurrentThreadID());
            options.codec = DM_AUDIO_WAV;
            options.bitrate = 32; /* Job owns the original settings. */
            assert(test_audio_wait(e->audio));
            DmBytes file = {0};
            assert(dm_read(path, &file, error, sizeof(error)) && file.size > 64);
            dm_bytes_free(&file);
        }
    }

    char error[512];
    assert(dm_audio_export(e->audio, &e->project,
                           "agents/reports/audio-render/no-such-folder/output.wav", NULL, error,
                           sizeof(error)));
    assert(!test_audio_wait(e->audio));
    puts(
        "PASS all codecs in CBR/VBR modes, worker-thread execution, frozen options and output failure recovery");
}

/* The suggested file name falls back from the project name to the imported file, and a finished
   render remembers where it was written. */
static void export_names(Editor *e) {
    DmAudio *audio = e->audio;
    char suggested[DM_PATH];
    snprintf(e->project.name, sizeof(e->project.name), "Export name");
    assert(ed_export_suggest(e, "mp3", suggested, sizeof(suggested)) &&
           !strcmp(suggested, "Export name.mp3"));
    e->project.name[0] = 0;
    snprintf(e->export_source, DM_PATH, "C:/music/Source.mid");
    assert(ed_export_suggest(e, "mid", suggested, sizeof(suggested)) &&
           !strcmp(suggested, "Source.mid"));
    SDL_Event saved;
    SDL_zero(saved);
    saved.type = ed_dialog_event;
    saved.user.code = DIALOG_WAVE;
    saved.user.data1 = SDL_strdup("agents/reports/audio-render/completed.wav");
    e->dialog_busy = true;
    editor_event(e, &saved);
    uint64_t start = SDL_GetTicks();

    while (dm_audio_busy(audio) && SDL_GetTicks() - start < 15000) {
        editor_background(e);
        SDL_Delay(1);
    }

    editor_background(e);
    assert(!dm_audio_busy(audio) && strstr(e->audio_exported, "completed.wav") &&
           !*e->audio_export_pending);
    puts("PASS export name fallback and successful render destination tracking");
}

int main(void) {
    Editor *e = test_editor_open("Audio render", (TestDesktop){.renderer = "software",
                                                               .driver = "dummy",
                                                               .soundfont = TEST_SOUNDFONT,
                                                               .paused = true});
    SDL_CreateDirectory("agents/reports/audio-render");
    fixture(e);
    codecs(e);
    export_names(e);
    test_audio_render_ui(e);
    test_editor_close(e);
    return 0;
}
