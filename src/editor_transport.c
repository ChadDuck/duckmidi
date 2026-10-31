#include "editor_internal.h"
#include "text/en.h"

static void replace_recorded_overlap(Editor *e, double start, double end);

void ed_cancel_wait(Editor *e) {
    if (e->wait_armed) {
        e->wait_armed = false;
        e->recording_take = e->take_snapshot = false;
    }
}

/* After a take, the open pattern editor takes focus so its shortcuts (Ctrl + A, Ctrl + Q) act on
   the recorded notes straight away. */
void ed_recording_finished(Editor *e) {
    if (e->roll && ed_pattern(e) && !ed_pattern(e)->kind) {
        ed_focus_edit(e, FOCUS_NOTES);
    }
}

void ed_play(Editor *e) {
    if (!e->active) {
        return;
    }

    bool playing = dm_audio_playing(e->audio);

    if (e->wait_armed) {
        ed_cancel_wait(e);
        return;
    }

    if (playing) {
        bool take = e->record;
        ed_release_piano(e);
        e->recording_take = e->take_snapshot = false;

        if (take) {
            ed_recording_finished(e);
        }

        double beat = e->record || e->context == 2
                          ? e->start_beat[e->pattern_mode]
                          : dm_seconds_beat(&e->project, dm_audio_position(e->audio));
        e->cursor_beat[e->pattern_mode] = beat;
        ed_transport_publish(e, dm_beat_seconds(&e->project, beat), false);
        return;
    }

    double pos = dm_beat_seconds(&e->project, e->record ? e->start_beat[e->pattern_mode]
                                                        : e->cursor_beat[e->pattern_mode]);

    if (!e->record && pos >= dm_beat_seconds(&e->project, ed_transport_length(e))) {
        pos = 0;
    }

    e->recording_take = e->record;
    e->take_snapshot = false;
    e->take_started = false;

    if (e->record && e->wait_input) {
        e->wait_armed = true;
        ed_transport_publish(e, pos, false);
        return;
    }

    ed_transport_publish(e, pos, true);
}

void ed_seek(Editor *e, double beat) {
    if (!e->active) {
        return;
    }

    ed_release_piano(e);
    beat = fmax(0, fmin(ed_transport_length(e), beat));
    e->cursor_beat[e->pattern_mode] = e->start_beat[e->pattern_mode] = beat;
    ed_transport_publish(e, dm_beat_seconds(&e->project, beat), dm_audio_playing(e->audio));
}

bool ed_begin_record_take(Editor *e) {
    DmPattern *p = ed_pattern(e);

    if (e->take_started) {
        return true;
    }

    if (!p || p->kind) {
        return false;
    }

    size_t bytes = p->count * sizeof(DmNote);

    if (!dm_reserve((void **)&e->record_original.data, &e->record_original.capacity, bytes, 1)) {
        return false;
    }

    if (bytes) {
        memcpy(e->record_original.data, p->notes, bytes);
    }

    e->record_original.size = bytes;
    ed_checkpoint(e);

    if (!e->overwrite) {
        size_t kept = 0;

        for (size_t i = 0; i < p->count; i++) {
            if (p->notes[i].port != ed_note_port(e)) {
                p->notes[kept++] = p->notes[i];
            }
        }

        p->count = kept;
        ed_publish(e);
    }

    e->take_started = true;
    return true;
}

static void replace_recorded_overlap(Editor *e, double start, double end) {
    if (!e->overwrite) {
        return;
    }

    DmPattern *p = ed_pattern(e);
    DmNote *original = (DmNote *)e->record_original.data;

    for (size_t i = 0; i < e->record_original.size / sizeof(DmNote); ++i) {
        DmNote *old = &original[i];

        if (old->port != ed_note_port(e) || !old->velocity || old->beat >= end ||
            old->beat + old->length <= start) {
            continue;
        }

        for (size_t j = 0; j < p->count; ++j) {
            if (!ed_note_identity(old, &p->notes[j])) {
                memmove(p->notes + j, p->notes + j + 1, (p->count - j - 1) * sizeof(DmNote));
                --p->count;
                break;
            }
        }

        old->velocity = 0;
    }
}

double ed_recording_origin(Editor *e, double beat) {
    if (e->pattern_mode) {
        return 0;
    }

    double origin = 0;
    bool found = false;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        DmClip c = e->project.clips[i];

        if (c.pattern == e->selected_pattern && c.beat <= beat + 1e-7 &&
            (!found || c.beat > origin)) {
            origin = c.beat;
            found = true;
        }
    }

    if (!found && e->selected_clip >= 0 && (size_t)e->selected_clip < e->project.clip_count &&
        e->project.clips[e->selected_clip].pattern == e->selected_pattern) {
        origin = e->project.clips[e->selected_clip].beat;
    }

    return origin;
}

void ed_record_note(Editor *e, int note) {
    if (!e->record || e->record_start[note] < 0 || !ed_pattern(e) || ed_pattern(e)->kind) {
        return;
    }

    if (!e->recording_take) {
        e->recording_take = true;
        e->take_snapshot = false;
    }

    double start = fmax(0, e->record_start[note] - e->record_origin[note]),
           end = fmax(start, dm_seconds_beat(&e->project, dm_audio_position(e->audio)) -
                                 e->record_origin[note]);
    ed_checkpoint(e);
    replace_recorded_overlap(e, start, end);
    ed_add_note(
        e, ed_pattern(e),
        (DmNote){start, fmax(1.0 / DM_PPQ, end - start), note, 100, 0, (uint8_t)ed_note_port(e)});
    e->record_start[note] = -1;
    e->recording_commit = true;
    ed_commit(e);
    e->recording_commit = false;
}

void ed_stop_edit_feedback(Editor *e) {
    if (e->edit_feedback) {
        dm_audio_edit_preview(e->audio, 0, 0, false);
        e->edit_feedback = false;
    }
}

void ed_finish_edit_feedback(Editor *e) {
    if (!ui_mouse().left) {
        ed_stop_edit_feedback(e);
    }
}

void ed_edit_feedback(Editor *e, float pitch) {
    if (!e->preview_override && e->preview_source_channel >= 0) {
        e->preview_program = ed_channel_program(e, e->preview_source_channel);
    }

    dm_audio_edit_preview(e->audio, e->preview_program, fminf(127, fmaxf(0, pitch)), true);
    e->edit_feedback = true;
}

void ed_release_piano(Editor *e) {
    ed_stop_edit_feedback(e);

    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) {
        if (e->held[i] >= 0) {
            int note = e->held[i];
            dm_audio_preview(e->audio, e->held_channels[i], note, 0, false);
            e->held[i] = -1;
            ed_record_note(e, note);
        }
    }
}

void ed_toggle_record(Editor *e) {
    if (!e->record && ed_pattern(e) && ed_pattern(e)->kind) {
        ed_message(e, DM_TEXT_OPEN_NOTE_PATTERN);
        return;
    }

    bool take = e->record && dm_audio_playing(e->audio);

    if (e->record) {
        ed_release_piano(e);
    }

    e->record = !e->record;

    if (take) {
        ed_recording_finished(e);
    }

    e->wait_armed = e->take_started = false;
    e->recording_take = e->record && dm_audio_playing(e->audio);
    e->take_snapshot = false;
    dm_audio_recording(e->audio, e->record);
}
