#include "editor/internal.h"
#include "text/en.h"

static void replace_recorded_overlap(Editor *e, DmPattern *p, unsigned port, double start,
                                     double end);

/* A take keeps one undo step: true once it exists, false only when it cannot be saved. */
static bool take_checkpoint(Editor *e) {
    return (e->recording_take && e->take_snapshot) || ed_checkpoint(e);
}

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
    ed_play_hold(e, false);
}

void ed_play_hold(Editor *e, bool hold) {
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
        ed_midi_input_finish(e, false); /* Stopping republishes. */
        ed_release_piano(e);
        e->recording_take = e->take_snapshot = false;

        if (take) {
            ed_recording_finished(e);
        }

        double beat = hold ? dm_seconds_beat(&e->project, dm_audio_position(e->audio))
                           : e->start_beat[e->pattern_mode];
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

    ed_midi_input_finish(e, false); /* The seek republishes. */
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

    if (!p || (p->kind && p->mode)) {
        return false;
    }

    e->take_origin_set = false;

    size_t bytes = p->count * sizeof(DmNote);

    if (!dm_reserve((void **)&e->record_original.data, &e->record_original.capacity, bytes, 1)) {
        return false;
    }

    if (bytes) {
        memcpy(e->record_original.data, p->notes, bytes);
    }

    e->record_original.size = bytes;

    if (!take_checkpoint(e)) {
        return false;
    }

    ed_midi_input_take(e);

    if (!e->overwrite && !p->kind) {
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

static void replace_recorded_overlap(Editor *e, DmPattern *p, unsigned port, double start,
                                     double end) {
    if (!e->overwrite) {
        return;
    }

    DmNote *original = (DmNote *)e->record_original.data;

    for (size_t i = 0; i < e->record_original.size / sizeof(DmNote); ++i) {
        DmNote *old = &original[i];

        if (old->port != port || !old->velocity || old->beat >= end ||
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

static bool placement_contains(const Editor *e, DmClip c, double beat) {
    return c.pattern == e->selected_pattern && beat >= c.beat - 1e-7 &&
           beat < c.beat + dm_clip_length(&e->project, c);
}

/* Song beat where pattern beat 0 falls for a note played at a song beat. Pattern playback
   records at the pattern's own position. In song playback, a placement of the pattern under the
   playhead decides, whatever has focus, so notes land where they were played; the selected
   placement wins where placements overlap. Off its placements, the pattern starts at the bar
   where playing began, keeping the beat within that bar. */
static bool placement_origin(const Editor *e, double beat, double *origin) {
    int selected = e->selected_clip;
    const DmClip *found = NULL;

    if (selected >= 0 && (size_t)selected < e->project.clip_count &&
        placement_contains(e, e->project.clips[selected], beat)) {
        found = &e->project.clips[selected];
    }

    for (size_t i = 0; !found && i < e->project.clip_count; ++i) {
        if (placement_contains(e, e->project.clips[i], beat)) {
            found = &e->project.clips[i];
        }
    }

    if (found) {
        *origin = found->beat - found->offset;
    }

    return found != NULL;
}

static double bar_origin(const Editor *e, double beat) {
    DmMeter meter = dm_meter(&e->project, beat);
    return meter.origin + floor((beat - meter.origin) / meter.bar + 1e-9) * meter.bar;
}

double ed_recording_origin(Editor *e, double beat) {
    double origin = 0;

    if (!e->pattern_mode && !placement_origin(e, beat, &origin)) {
        origin = bar_origin(e, beat);
    }

    return origin;
}

/* Each note is placed by the placement it was played in, so a take that crosses from one
   placement into the next lands in both. Off the placements, the take's first note fixes the
   bar origin for the rest of the take; a loop back to an earlier beat restarts it. */
double ed_record_origin(Editor *e, double beat) {
    double origin = 0;

    if (e->pattern_mode || placement_origin(e, beat, &origin)) {
        return origin;
    }

    if (!e->take_origin_set || beat < e->take_origin) {
        e->take_origin = bar_origin(e, beat);
        e->take_origin_set = true;
    }

    return e->take_origin;
}

bool ed_record_insert(Editor *e, DmPattern *pattern, int key, int velocity, unsigned port,
                      double start, double end, double origin) {
    start = fmax(0, start - origin);
    end = fmax(start, end - origin);

    if (!dm_reserve((void **)&pattern->notes, &pattern->capacity, pattern->count + 1,
                    sizeof(DmNote))) {
        return false;
    }

    if (!take_checkpoint(e)) {
        return false;
    }

    replace_recorded_overlap(e, pattern, port, start, end);
    return ed_add_note(e, pattern,
                       (DmNote){start, fmax(1.0 / DM_PPQ, end - start), (uint8_t)key,
                                (uint8_t)velocity, 0, (uint8_t)port});
}

void ed_record_note(Editor *e, int note) {
    if (!e->record || e->record_start[note] < 0 || !ed_pattern(e) || ed_pattern(e)->kind) {
        return;
    }

    if (!e->recording_take) {
        e->recording_take = true;
        e->take_snapshot = false;
    }

    ed_record_insert(
        e, ed_pattern(e), note, ED_DEFAULT_VELOCITY, ed_note_port(e), e->record_start[note],
        dm_seconds_beat(&e->project, dm_audio_position(e->audio)), e->record_origin[note]);
    e->record_start[note] = -1;
    ed_commit(e);
}

void ed_stop_edit_feedback(Editor *e) {
    if (e->edit_feedback) {
        dm_audio_edit_preview(e->audio, 0, 0, false);
        e->edit_feedback = false;
    }
}

/* The sound lasts while the drag's button is held: left, or a copy drag's own button. */
void ed_finish_edit_feedback(Editor *e) {
    UiMouse mouse = ui_mouse();

    if (!mouse.left && !(e->copy_button >= 0 && ed_mouse_down(mouse, e->copy_button))) {
        ed_stop_edit_feedback(e);
    }
}

void ed_edit_feedback(Editor *e, float pitch) {
    ed_follow_preview(e);

    int part = ed_note_port(e) * 16 + e->pattern_channel;
    dm_audio_edit_preview_part(e->audio, part, e->preview_program, fminf(127, fmaxf(0, pitch)),
                               true);
    e->edit_feedback = true;
}

/* Computer keys only: UI actions call this when the keyboard loses its target. MIDI input
   keeps playing; its takes end in ed_play, ed_seek and ed_toggle_record. */
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
    if (!e->record && ed_pattern(e) && ed_pattern(e)->kind && ed_pattern(e)->mode) {
        ed_message(e, DM_TEXT_OPEN_RECORD_PATTERN);
        return;
    }

    bool take = e->record && dm_audio_playing(e->audio);

    if (e->record) {
        ed_midi_input_finish(e, true);
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
