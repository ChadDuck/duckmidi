/* Background jobs: SoundFont loading and offline rendering to an audio file. */
#include "audio/internal.h"

static int32_t DME_CALL render_canceled(void *user) {
    DmAudio *a = user;
    return SDL_GetAtomicInt(&a->cancel);
}

static void DME_CALL render_sample_warning(void *user, uint32_t severity, uint32_t flags,
                                           const char *message) {
    (void)flags;
    (void)message;
    DmAudio *a = user;
    a->render_missing_samples += severity == DME_WARNING;
}

static bool prepare_render_samples(DmAudio *a, DmeContext *context,
                                   DmSampleExportSnapshot **snapshot, DmSampleSynth **synth) {
    if (!a->render_project.size) {
        return true;
    }

    DmProject project;
    dm_project_init(&project);
    size_t count = a->render_schedule.count;
    DmeEvent *events = count ? calloc(count, sizeof(*events)) : NULL;
    bool ok =
        (!count || events) && dm_unpack(&project, a->render_project.data, a->render_project.size,
                                        a->job_message, sizeof(a->job_message));

    if (ok) {
        for (size_t i = 0; i < count; ++i) {
            const DmScheduled *event = &a->render_schedule.items[i];
            events[i] = (DmeEvent){.source_id = i + 1,
                                   .status = event->status,
                                   .a = event->a,
                                   .b = event->b,
                                   .port = event->port};
        }

        *context = (DmeContext){.events = events, .event_count = count};
        DmeHost host = {
            .user = a, .cancelled = render_canceled, .diagnostic = render_sample_warning};
        ok = dm_sample_export_snapshot(&project, context, &host, snapshot);

        if (ok && context->sample_preset_count) {
            *synth = dm_sample_synth_create(context, *snapshot, &host);
            ok = *synth != NULL;
        }
    }

    context->events = NULL;
    free(events);
    dm_project_free(&project);
    dm_bytes_free(&a->render_project);

    if (!ok && !*a->job_message) {
        snprintf(a->job_message, sizeof(a->job_message), "%s", DM_TEXT_SAMPLE_AUDIO_PREPARE);
    }

    return ok;
}

static bool audio_render(DmAudio *a) {
    Synth s = {0};
    Synth *ports[DM_NOTE_PORTS] = {0};

    if (!dm_audio_synth_create(&s, a->font_path)) {
        snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_CANNOT_LOAD_SOUNDFONT_AUDIO);
        return false;
    }

    uint64_t frames = (uint64_t)ceil((a->render_schedule.duration + 2) * RATE);

    if (a->render_options.codec == DM_AUDIO_WAV &&
        frames > (UINT32_MAX - 256) / (2 * (a->render_options.bits / 8))) {
        dm_audio_synth_free(&s);
        snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_WAV_EXCEEDS_RIFF_SIZE);
        return false;
    }

    bool ok = true;

    for (size_t i = 0; i < a->render_schedule.count; i++) {
        unsigned port = a->render_schedule.items[i].port;

        if (!port || ports[port]) {
            continue;
        }

        ports[port] = calloc(1, sizeof(Synth));

        if (!ports[port] || !dm_audio_synth_create(ports[port], a->font_path)) {
            snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_CANNOT_PREPARE_NOTE_PORTS);
            ok = false;
            break;
        }

        if (SDL_GetAtomicInt(&a->cancel)) {
            ok = false;
            break;
        }
    }

    DmOutput output = {0};
    DmAudioEncoder *encoder = NULL;
    DmeContext sample_context = {0};
    DmSampleExportSnapshot *snapshot = NULL;
    DmSampleSynth *sample_synth = NULL;
    ok = ok && prepare_render_samples(a, &sample_context, &snapshot, &sample_synth);

    if (ok) {
        ok = dm_output_begin(&output, a->job_path, a->job_message, sizeof(a->job_message));
    }

    if (ok) {
        encoder = dm_audio_encoder_begin(&output, &a->render_options, a->job_message,
                                         sizeof(a->job_message));
        ok = encoder != NULL;
    }

    size_t cursor = 0;
    uint64_t pos = 0;

    while (ok && pos < frames) {
        if (SDL_GetAtomicInt(&a->cancel)) {
            ok = false;
            break;
        }

        while (cursor < a->render_schedule.count &&
               a->render_schedule.items[cursor].seconds * RATE <= pos + .5) {
            size_t index = cursor++;
            const DmScheduled *event = &a->render_schedule.items[index];
            Synth *route = event->port ? ports[event->port] : &s;

            if (!sample_synth ||
                !dm_sample_synth_note(sample_synth, route->synth, &route->sample_pedals, event,
                                      sample_context.effective_patches[index])) {
                dm_audio_send_event(route, event);
            }
        }

        int n = (int)fmin(BLOCK, (double)(frames - pos));

        if (cursor < a->render_schedule.count) {
            int until = (int)ceil(a->render_schedule.items[cursor].seconds * RATE - pos);

            if (until > 0 && until < n) {
                n = until;
            }
        }

        float sound[BLOCK * 2];
        dm_audio_render(&s, sound, n, NULL);

        for (unsigned port = 1; port < DM_NOTE_PORTS; port++) {
            if (ports[port]) {
                dm_audio_render_port_mix(ports[port], sound, n, NULL);
            }
        }

        ok = dm_audio_encoder_write(encoder, sound, n);
        pos += (unsigned)n;
        SDL_SetAtomicInt(&a->progress, (int)(pos * 100 / frames));
    }

    if (SDL_GetAtomicInt(&a->cancel)) {
        snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_AUDIO_EXPORT_CANCELED);
        ok = false;
    }

    ok = dm_audio_encoder_finish(encoder, ok) && ok;

    if (output.file) {
        ok = dm_output_finish(&output, ok, a->job_message, sizeof(a->job_message));
    }

    dm_audio_free_port_synths(ports);
    dm_audio_synth_free(&s);
    dm_sample_synth_free(sample_synth);
    dm_sample_export_free(snapshot);
    return ok;
}

static int job_worker(void *userdata) {
    DmAudio *a = userdata;

    if (a->job_type == 1) {
        a->job_success = dm_audio_synth_create(&a->prepared, a->job_path) &&
                         dm_audio_synth_create(&a->prepared_keyboard, a->job_path) &&
                         dm_audio_synth_create(&a->prepared_feedback, a->job_path);

        for (unsigned p = 1; p < DM_NOTE_PORTS && a->job_success; p++) {
            if (a->job_ports[p]) {
                if (SDL_GetAtomicInt(&a->cancel)) {
                    a->job_success = false;
                    break;
                }

                a->prepared_ports[p] = calloc(1, sizeof(Synth));
                a->job_success = a->prepared_ports[p] &&
                                 dm_audio_synth_create(a->prepared_ports[p], a->job_path);
            }
        }

        if (SDL_GetAtomicInt(&a->cancel)) {
            a->job_success = false;
        }

        if (!a->job_success) {
            dm_audio_synth_free(&a->prepared);
            dm_audio_synth_free(&a->prepared_keyboard);
            dm_audio_synth_free(&a->prepared_feedback);
            dm_audio_free_port_synths(a->prepared_ports);
        }

        snprintf(a->job_message, sizeof(a->job_message), "%s",
                 a->job_success ? DM_TEXT_SOUNDFONT_READY : DM_TEXT_UNABLE_LOAD_SOUNDFONT);
    } else {
        a->job_success = audio_render(a);

        if (a->job_success && a->render_missing_samples) {
            snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_SAMPLE_AUDIO_MISSING,
                     dm_audio_codec_name(a->render_options.codec), a->render_missing_samples);
        } else if (a->job_success) {
            snprintf(a->job_message, sizeof(a->job_message), "%s export complete",
                     dm_audio_codec_name(a->render_options.codec));
        }
    }

    SDL_SetAtomicInt(&a->job_done, 1);
    return 0;
}

static bool job_start(DmAudio *a, const char *path, int type, char *error, size_t cap) {
    if (!a || SDL_GetAtomicInt(&a->job_busy)) {
        snprintf(error, cap, DM_TEXT_BACKGROUND_OPERATION_ALREADY);
        return false;
    }

    snprintf(a->job_path, sizeof(a->job_path), "%s", path);
    a->job_type = type;

    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        a->job_ports[p] = a->port_live[p] != NULL;
    }

    a->job_success = false;
    a->job_message[0] = 0;
    SDL_SetAtomicInt(&a->cancel, 0);
    SDL_SetAtomicInt(&a->job_done, 0);
    SDL_SetAtomicInt(&a->progress, 0);
    SDL_SetAtomicInt(&a->job_busy, 1);
    a->worker =
        SDL_CreateThread(job_worker, type == 1 ? DM_TEXT_LOAD_SOUNDFONT : DM_TEXT_RENDER_AUDIO, a);

    if (!a->worker) {
        SDL_SetAtomicInt(&a->job_busy, 0);
        snprintf(error, cap, DM_TEXT_CANNOT_START_BACKGROUND);
        return false;
    }

    return true;
}

bool dm_audio_soundfont(DmAudio *a, const char *path, char *error, size_t cap) {
    return job_start(a, path, 1, error, cap);
}

bool dm_audio_busy(DmAudio *a) {
    return a && SDL_GetAtomicInt(&a->job_busy) != 0;
}

bool dm_audio_poll(DmAudio *a, char *message, size_t cap, bool *success, int *kind) {
    if (!a) {
        return false;
    }

    dm_audio_collect_sample_banks(a);

    if (*a->sample_warning) {
        if (kind) {
            *kind = 0;
        }

        *success = true;
        snprintf(message, cap, "%s", a->sample_warning);
        *a->sample_warning = 0;
        return true;
    }

    if (SDL_SetAtomicInt(&a->port_warning, 0)) {
        if (kind) {
            *kind = 0;
        }

        *success = true;
        snprintf(message, cap, DM_TEXT_INDEPENDENT_NOTE_PORTS_SHARE);
        return true;
    }

    if (SDL_SetAtomicInt(&a->midi_failed, 0)) {
        if (kind) {
            *kind = 0;
        }

        snprintf(message, cap, DM_TEXT_EXTERNAL_MIDI_DEVICE);
        *success = false;
        return true;
    }

    if (!SDL_GetAtomicInt(&a->job_done)) {
        return false;
    }

    SDL_WaitThread(a->worker, NULL);

    if (kind) {
        *kind = a->job_type;
    }

    a->worker = NULL;

    if (a->job_type == 1 && a->job_success) {
        if (a->stream) {
            SDL_LockAudioStream(a->stream);
        }

        Synth old = a->live;
        Synth old_keyboard = a->keyboard;
        Synth old_feedback = a->feedback;
        Synth *old_ports[DM_NOTE_PORTS] = {0};

        for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
            old_ports[p] = a->port_live[p];
            a->port_live[p] = a->prepared_ports[p];
            a->prepared_ports[p] = NULL;
        }

        a->live = a->prepared;
        a->keyboard = a->prepared_keyboard;
        a->feedback = a->prepared_feedback;
        a->feedback_key = -1;
        a->feedback_sample_id = 0;
        memset(a->keyboard_sample_id, 0, sizeof(a->keyboard_sample_id));
        a->feedback_tail = 0;
        memset(&a->prepared, 0, sizeof(a->prepared));
        memset(&a->prepared_keyboard, 0, sizeof(a->prepared_keyboard));
        memset(&a->prepared_feedback, 0, sizeof(a->prepared_feedback));

        for (int c = 0; c < 16; ++c) {
            a->keyboard_applied[c].program = -1;
            a->keyboard_applied[c].volume = -1;
            a->keyboard_applied[c].pan = -1;
        }

        a->keyboard_config = true;
        a->keyboard_tail = 0;
        a->reset = true;

        if (a->stream) {
            SDL_UnlockAudioStream(a->stream);
        }

        dm_audio_synth_free(&old);
        dm_audio_synth_free(&old_keyboard);
        dm_audio_synth_free(&old_feedback);
        dm_audio_free_port_synths(old_ports);
        snprintf(a->font_path, sizeof(a->font_path), "%s", a->job_path);
    }

    dm_schedule_free(&a->render_schedule);
    dm_bytes_free(&a->render_project);
    snprintf(message, cap, "%s", a->job_message);
    *success = a->job_success;
    SDL_SetAtomicInt(&a->job_done, 0);
    SDL_SetAtomicInt(&a->progress, 0);
    SDL_SetAtomicInt(&a->job_busy, 0);
    return true;
}

bool dm_audio_export(DmAudio *a, const DmProject *p, const char *path,
                     const DmAudioOptions *options, char *error, size_t cap) {
    DmAudioOptions chosen = options ? *options : dm_audio_options_default();

    if (!dm_audio_options_valid(&chosen)) {
        snprintf(error, cap, DM_TEXT_INVALID_AUDIO_EXPORT_SETTINGS);
        return false;
    }

    if (!a || !*p->soundfont) {
        snprintf(error, cap, DM_TEXT_CHOOSE_SOUNDFONT_IN_PREFERENCES);
        return false;
    }

    if (dm_audio_busy(a)) {
        snprintf(error, cap, DM_TEXT_BACKGROUND_OPERATION_ALREADY);
        return false;
    }

    if (!dm_schedule(p, &a->render_schedule)) {
        return false;
    }

    if (p->patch_replacement_count && !dm_pack(p, &a->render_project)) {
        dm_schedule_free(&a->render_schedule);
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_PREPARE);
        return false;
    }

    a->render_missing_samples = 0;
    a->render_options = chosen;
    snprintf(a->font_path, sizeof(a->font_path), "%s", p->soundfont);

    if (job_start(a, path, 2, error, cap)) {
        return true;
    }

    dm_schedule_free(&a->render_schedule);
    dm_bytes_free(&a->render_project);
    return false;
}

bool dm_audio_exporting(DmAudio *a) {
    return a && a->job_type == 2 && SDL_GetAtomicInt(&a->job_busy) != 0;
}

int dm_audio_export_progress(DmAudio *a) {
    return a ? SDL_GetAtomicInt(&a->progress) : 0;
}
