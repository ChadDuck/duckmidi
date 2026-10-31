#include "editor_internal.h"
#include <assert.h>
extern void recording_feedback_audio_pause(DmAudio *);
extern void recording_feedback_audio_render(DmAudio *, int);
extern int recording_feedback_audio_port_cc(DmAudio *, unsigned, int, int);
extern uint64_t recording_feedback_worker_thread(DmAudio *);
extern void test_todo_c_ui(Editor *);

static bool wait_job(DmAudio *a) {
    bool success = false;
    char message[512] = {0};
    int kind = 0;
    uint64_t start = SDL_GetTicks();

    while (dm_audio_busy(a) && SDL_GetTicks() - start < 30000) {
        bool value = false;

        if (dm_audio_poll(a, message, sizeof(message), &value, &kind) && kind) {
            success = value;

            if (!success) {
                printf("Expected/actual job result: %s\n", message);
            }
        }

        SDL_Delay(1);
    }

    assert(!dm_audio_busy(a));
    return success;
}

static void mixer(Editor *e) {
    dm_project_free(&e->project);
    dm_project_init(&e->project);
    e->active = true;
    e->selected_pattern = dm_pattern_add(&e->project, "Ports");
    e->selected_channel = e->pattern_channel = 0;
    e->channel_selected = true;
    e->project.channels[0].program = 0;
    DmPattern *p = ed_pattern(e);
    p->length = 1;
    assert(dm_note_add(p, (DmNote){0, .5, 60, 100, 0, 0}) &&
           dm_note_add(p, (DmNote){0, .5, 72, 100, 0, 1}));
    assert(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0}));
    assert(dm_port_mix_set(&e->project, (DmPortMix){0, 0, 70, 20, false, false}));
    assert(dm_port_mix_set(&e->project, (DmPortMix){1, 0, 35, 100, false, false}));
    DmBytes data = {0};
    DmProject copy;
    dm_project_init(&copy);
    char error[512];
    assert(dm_pack(&e->project, &data) &&
           dm_unpack(&copy, data.data, data.size, error, sizeof(error)));
    assert(dm_port_mix(&copy, 1, 0).volume == 35 && dm_port_mix(&copy, 0, 0).pan == 20);
    assert(dm_project_copy(&copy, &e->project));
    copy.port_mix[0].volume = 99;
    assert(e->project.port_mix[0].volume == 35);
    dm_project_free(&copy);
    dm_bytes_free(&data);
    assert(dm_audio_publish(e->audio, &e->project, 0, true, error, sizeof(error)));
    recording_feedback_audio_render(e->audio, 128);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 0, 7) == 70 &&
           recording_feedback_audio_port_cc(e->audio, 1, 0, 7) == 35);
    dm_audio_part_control(e->audio, 1, 0, 7, 88);
    recording_feedback_audio_render(e->audio, 64);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 0, 7) == 70 &&
           recording_feedback_audio_port_cc(e->audio, 1, 0, 7) == 88);
    DmPortMix mix = dm_port_mix(&e->project, 1, 0);
    mix.mute = true;
    assert(dm_port_mix_set(&e->project, mix));
    DmExportEvent *events = NULL;
    size_t count;
    uint64_t duration;
    assert(dm_export_timeline(&e->project, &events, &count, &duration, error, sizeof(error)));
    int notes[2] = {0};

    for (size_t i = 0; i < count; i++) {
        if ((events[i].status & 0xf0) == 0x90 && events[i].b) {
            notes[events[i].port]++;
        }
    }

    assert(notes[0] == 1 && notes[1] == 0);
    free(events);
    mix.mute = false;
    mix.solo = true;
    assert(dm_port_mix_set(&e->project, mix));
    assert(dm_export_timeline(&e->project, &events, &count, &duration, error, sizeof(error)));
    notes[0] = notes[1] = 0;

    for (size_t i = 0; i < count; i++) {
        if ((events[i].status & 0xf0) == 0x90 && events[i].b) {
            notes[events[i].port]++;
        }
    }

    assert(notes[0] == 0 && notes[1] == 1);
    free(events);
    mix.solo = false;
    assert(dm_port_mix_set(&e->project, mix));
    assert(dm_midi(&e->project, &data, error, sizeof(error)));
    assert(data.size);
    dm_bytes_free(&data);
    dm_audio_transport(e->audio, false);
    puts(
        "PASS independent port volume/pan, live routing, mute/solo, mixer persistence and deep copy");
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
            snprintf(path, sizeof(path), "agents/reports/todo-2026-10-29-c/codec-%d-%d.%s", codec,
                     variable, dm_audio_codec_extension(options.codec));
            assert(dm_audio_export(e->audio, &e->project, path, &options, error, sizeof(error)));
            assert(recording_feedback_worker_thread(e->audio) != SDL_GetCurrentThreadID());
            options.codec = DM_AUDIO_WAV;
            options.bitrate = 32; /* Job owns the original settings. */
            assert(wait_job(e->audio));
            DmBytes file = {0};
            assert(dm_read(path, &file, error, sizeof(error)) && file.size > 64);
            dm_bytes_free(&file);
        }
    }

    char error[512];
    assert(dm_audio_export(e->audio, &e->project,
                           "agents/reports/todo-2026-10-29-c/no-such-folder/output.wav", NULL,
                           error, sizeof(error)));
    assert(!wait_job(e->audio));
    puts(
        "PASS all codecs in CBR/VBR modes, worker-thread execution, frozen options and output failure recovery");
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO));
    SDL_Window *window = SDL_CreateWindow("Todo C checks", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    assert(window && renderer && ui_init(window, renderer, NULL));
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    assert(audio);
    recording_feedback_audio_pause(audio);
    assert(
        dm_audio_soundfont(audio, "agents/reports/generated-programs.sf2", error, sizeof(error)) &&
        wait_job(audio));
    Editor *e = calloc(1, sizeof(*e));
    assert(e);
    editor_init(e, window, renderer, audio, "", true);
    mixer(e);
    codecs(e);
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
    saved.user.data1 = SDL_strdup("agents/reports/mixer-visual-2026-10-29/completed.wav");
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
    test_todo_c_ui(e);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
