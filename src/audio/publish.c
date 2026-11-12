/* Publishing a schedule to the audio thread: chase state, loop bounds, port synths, sample banks
   and the pattern preview schedule. */
#include "audio/internal.h"

static void chase_note_off(DmAudio *a, int channel, int key) {
    a->state.held[channel][key] = 0;

    if (a->state.cc[channel][MIDI_CC_SUSTAIN] < 64 && !a->state.sostenuto[channel][key]) {
        a->state.notes[channel][key] = 0;
    }
}

static void chase_pedal_release(DmAudio *a, int channel) {
    for (int key = 0; key < 128; ++key) {
        if (!a->state.held[channel][key] && a->state.cc[channel][MIDI_CC_SUSTAIN] < 64 &&
            !a->state.sostenuto[channel][key]) {
            a->state.notes[channel][key] = 0;
        }
    }
}

static void reset_chase_channel(DmAudio *a, int c) {
    memset(a->state.cc[c], 255, 128);
    a->state.cc[c][MIDI_CC_SUSTAIN] = 0;
    a->state.cc[c][MIDI_CC_SOSTENUTO] = 0;
    memset(a->state.notes[c], 0, 128);
    memset(a->state.held[c], 0, 128);
    memset(a->state.sostenuto[c], 0, 128);
    memset(a->state.poly[c], 255, 128);
    a->state.pressure[c] = 255;
    a->state.bend[c][0] = 0;
    a->state.bend[c][1] = 64;
}

static void reset_chase_controllers(DmAudio *a, int c) {
    const int keep[] = {MIDI_CC_BANK, MIDI_CC_BANK_LSB, MIDI_CC_VOLUME, MIDI_CC_PAN};
    uint8_t values[4];

    for (int i = 0; i < 4; ++i) {
        values[i] = a->state.cc[c][keep[i]];
    }

    memset(a->state.cc[c], 255, 128);

    for (int i = 0; i < 4; ++i) {
        a->state.cc[c][keep[i]] = values[i];
    }

    a->state.cc[c][MIDI_CC_SUSTAIN] = 0;
    a->state.cc[c][MIDI_CC_SOSTENUTO] = 0;
    memset(a->state.sostenuto[c], 0, 128);
    a->state.bend[c][0] = 0;
    a->state.bend[c][1] = 64;
    a->state.pressure[c] = 0;
    memset(a->state.poly[c], 0, 128);
    chase_pedal_release(a, c);
}

static void cache_controller(DmAudio *a, int c, int cc, int value) {
    int previous = a->state.cc[c][cc];
    a->state.cc[c][cc] = value;

    if (cc == MIDI_CC_SOSTENUTO) {
        if (value >= 64 && previous < 64) {
            memcpy(a->state.sostenuto[c], a->state.held[c], 128);
        } else if (value < 64) {
            memset(a->state.sostenuto[c], 0, 128);
            chase_pedal_release(a, c);
        }
    }

    if (cc == MIDI_CC_SUSTAIN && value < 64) {
        chase_pedal_release(a, c);
    }

    if (cc == MIDI_CC_RESET) {
        reset_chase_controllers(a, c);
    }

    if (cc == MIDI_CC_SOUND_OFF) {
        memset(a->state.notes[c], 0, 128);
        memset(a->state.held[c], 0, 128);
        memset(a->state.sostenuto[c], 0, 128);
    }

    if (cc >= MIDI_CC_NOTES_OFF) {
        for (int key = 0; key < 128; ++key) {
            chase_note_off(a, c, key);
        }
    }
}

static bool gm_reset(const DmScheduled *event) {
    if (event->status != MIDI_SYSEX || event->sysex.size < 4) {
        return false;
    }

    const uint8_t *b = event->sysex.data;
    return b[0] == 0x7e && b[2] == 9 && (b[3] == 1 || b[3] == 3);
}

static void cache_event(DmAudio *a, const DmScheduled *event) {
    if (gm_reset(event)) {
        for (int c = event->port * 16; c < event->port * 16 + 16; ++c) {
            reset_chase_channel(a, c);
        }

        return;
    }

    int c = event->port * 16 + (event->status & MIDI_CHANNEL_MASK);

    switch (event->status & MIDI_STATUS_MASK) {
    case MIDI_KEY_PRESSURE:
        a->state.poly[c][event->a] = event->b;
        break;
    case MIDI_CHANNEL_PRESSURE:
        a->state.pressure[c] = event->a;
        break;
    case MIDI_CONTROL:
        cache_controller(a, c, event->a, event->b);
        break;
    case MIDI_PITCH_BEND:
        a->state.bend[c][0] = event->a;
        a->state.bend[c][1] = event->b;
        break;
    case MIDI_NOTE_ON:
        if (event->b) {
            a->state.notes[c][event->a] = event->b;
            a->state.sample[c][event->a] = event->note_sample;
            a->state.held[c][event->a] = 1;
            a->seek_end[c][event->a] = event->note_end;
        } else {
            chase_note_off(a, c, event->a);
        }
        break;
    case MIDI_NOTE_OFF:
        chase_note_off(a, c, event->a);
        break;
    }
}

static void prepare_chase(DmAudio *a, const DmSchedule *s, double seconds) {
    a->beat_cursor = 0;

    while (a->beat_cursor < s->beat_count && s->beats[a->beat_cursor] < seconds) {
        ++a->beat_cursor;
    }

    memset(a->used_ports, 0, sizeof(a->used_ports));
    a->used_ports[0] = true;

    for (size_t i = 0; i < s->count; i++) {
        a->used_ports[s->items[i].port] = true;
    }

    for (int c = 0; c < DM_PARTS; ++c) {
        if (a->used_ports[c / 16]) {
            reset_chase_channel(a, c);
        }
    }

    a->cursor = 0;

    while (a->cursor < s->count && s->items[a->cursor].seconds < seconds) {
        cache_event(a, &s->items[a->cursor++]);
    }

    a->chase = seconds > 0;
}

void dm_audio_loop(DmAudio *a, bool enabled, double start, double end) {
    if (!a) {
        return;
    }

    enabled =
        enabled && isfinite(start) && isfinite(end) && start >= 0 && end - start >= 1.0 / RATE;
    DmAudio *scratch = enabled ? calloc(1, sizeof(*scratch)) : NULL;

    if (enabled && !scratch) {
        return;
    }

    /* GUI-thread preparation; the callback only copies bounded state at each wrap. */
    if (enabled) {
        prepare_chase(scratch, &a->schedule, start);
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->loop_enabled = enabled;
    a->loop_start = start;
    a->loop_end = end;
    a->loop_wait = enabled && a->position >= end;

    if (enabled) {
        dm_audio_copy_chase(&a->loop_state, &scratch->state, a->used_ports);
        dm_audio_copy_part_rows(a->loop_note_end, scratch->seek_end, sizeof(a->loop_note_end[0]),
                                a->used_ports);
        a->loop_cursor = scratch->cursor;
        a->loop_beat_cursor = scratch->beat_cursor;
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    free(scratch);
}

static uint16_t project_preview_mask(const DmProject *p) {
    bool solo = false;

    for (int c = 0; c < 16; ++c) {
        solo |= p->channels[c].enabled && p->channels[c].solo;
    }

    uint16_t mask = 0;

    for (int c = 0; c < 16; ++c) {
        if (p->channels[c].enabled && !p->channels[c].mute && (!solo || p->channels[c].solo)) {
            mask |= (uint16_t)(1u << c);
        }
    }

    return mask;
}

static bool prepare_note_ends(DmSchedule *schedule, const DmProject *project,
                              const DmPatternPreview *preview, const DmLiveSampleBank *bank) {
    size_t (*head)[128] = malloc(DM_PARTS * sizeof(*head)),
           (*tail)[128] = malloc(DM_PARTS * sizeof(*tail));
    uint8_t programs[DM_PARTS] = {0};
    DmSamplePatchState patches[DM_PARTS];

    if (bank) {
        dm_sample_patch_state_init(project, patches);
    }

    if (!head || !tail) {
        free(head);
        free(tail);
        return false;
    }

    memset(head, 255, DM_PARTS * sizeof(*head));
    memset(tail, 255, DM_PARTS * sizeof(*tail));
    size_t *links = malloc((schedule->count + 1) * sizeof(*links));

    if (!links) {
        free(head);
        free(tail);
        return false;
    }

    for (size_t i = 0; i < schedule->count; ++i) {
        DmScheduled *event = &schedule->items[i];
        int type = event->status & MIDI_STATUS_MASK,
            c = event->port * 16 + (event->status & MIDI_CHANNEL_MASK), key = event->a;
        int source_channel =
            preview && preview->program >= 0 ? preview->channel : c & MIDI_CHANNEL_MASK;
        int source_part = event->port * 16 + source_channel;

        if (type == MIDI_PROGRAM) {
            programs[c] = event->a;
        }

        if (bank && event->status < MIDI_SYSTEM) {
            dm_sample_patch_state_event(&patches[source_part], event->status, event->a, event->b);
        } else if (bank && gm_reset(event)) {
            for (int ch = 0; ch < 16; ++ch) {
                DmSamplePatchState *patch = &patches[event->port * 16 + ch];
                *patch = (DmSamplePatchState){.drums = ch == MIDI_DRUM_CHANNEL};
            }
        }

        if (type == MIDI_NOTE_ON && event->b) {
            event->note_program = programs[c];
            event->note_end = schedule->duration;
            links[i] = SIZE_MAX;

            if (bank) {
                DmSamplePatchState patch = patches[source_part];
                bool drums = preview && preview->program >= 0 && !preview->follow
                                 ? preview->program == 128
                                 : patch.drums;
                DmPatchKey route = {patch.selected, event->port, source_channel, patch.program,
                                    drums};
                event->note_sample = (uint16_t)dm_live_sample_bank_patch(bank, route);
            }

            if (head[c][key] == SIZE_MAX) {
                head[c][key] = i;
            } else {
                links[tail[c][key]] = i;
            }

            tail[c][key] = i;
        } else if ((type == MIDI_NOTE_OFF || (type == MIDI_NOTE_ON && !event->b)) &&
                   head[c][key] != SIZE_MAX) {
            size_t on = head[c][key];
            schedule->items[on].note_end = event->seconds;
            head[c][key] = links[on];
        }
    }

    free(links);
    free(head);
    free(tail);
    return true;
}

void dm_audio_free_retired(DmAudio *a) {
    while (a->retired) {
        RetiredSchedule *old = a->retired;
        a->retired = old->next;
        dm_schedule_free(&old->schedule);
        free(old);
    }
}

static size_t schedule_after(DmSchedule *schedule, double seconds) {
    size_t lo = 0, hi = schedule->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (schedule->items[mid].seconds <= seconds) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo;
}

static NoteRelease *prepare_release_storage(DmAudio *a, size_t count, size_t *capacity) {
    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    *capacity = count + a->schedule.count + a->release_count + 2048;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    return malloc(*capacity * sizeof(NoteRelease));
}

static void edit_cursor(DmAudio *scratch, DmSchedule *next, double seconds) {
    scratch->cursor = schedule_after(next, seconds);
    size_t lo = 0, hi = next->beat_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (next->beats[mid] < seconds) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    scratch->beat_cursor = lo;
}

static bool install_release_storage(DmAudio *a, DmAudio *scratch, DmSchedule *next,
                                    NoteRelease *releases, bool editing, double *seconds,
                                    bool *playing) {
    bool preserve = editing && a->playing;

    if (preserve) {
        *seconds = a->position;
        *playing = a->playing;
        edit_cursor(scratch, next, *seconds);

        if (a->release_count) {
            memcpy(releases, a->releases, a->release_count * sizeof(*releases));
        }
    } else {
        a->release_count = 0;
        a->release_end = 0;
    }

    return preserve;
}

void dm_audio_free_port_synths(Synth **ports) {
    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        if (ports[p]) {
            dm_audio_synth_free(ports[p]);
            free(ports[p]);
            ports[p] = NULL;
        }
    }
}

static bool prepare_ports(DmAudio *a, const bool *used, Synth **fresh, char *error, size_t cap) {
    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        if (used[p] && !a->port_live[p]) {
            if (SDL_GetAtomicInt(&a->job_busy) && a->job_type == 1) {
                snprintf(error, cap, DM_TEXT_WAIT_SOUNDFONT_LOADING_BEFORE);
                dm_audio_free_port_synths(fresh);
                return false;
            }

            fresh[p] = calloc(1, sizeof(Synth));

            if (!fresh[p] || !dm_audio_synth_create(fresh[p], a->font_path)) {
                snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_INDEPENDENT_PLAYBACK, p);
                dm_audio_free_port_synths(fresh);
                return false;
            }
        }
    }

    return true;
}

static unsigned sample_bank_users(Synth *s) {
    if (s && s->sample_banks && !fluid_synth_get_active_voice_count(s->synth)) {
        s->sample_banks = 0;
    }

    return s ? s->sample_banks : 0;
}

void dm_audio_collect_sample_banks(DmAudio *a) {
    unsigned retired = 0;

    for (int i = 0; i < LIVE_SAMPLE_BANKS; ++i) {
        if (i != a->sample_bank && a->sample_banks[i]) {
            retired |= 1u << i;
        }
    }

    if (!retired) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    /* FluidSynth 2.5.4 counts overflow voices as active until they drop their samples. */
    unsigned used = sample_bank_users(&a->live) | sample_bank_users(&a->keyboard) |
                    sample_bank_users(&a->feedback);

    for (unsigned port = 1; port < DM_NOTE_PORTS; ++port) {
        used |= sample_bank_users(a->port_live[port]);
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    for (int i = 0; i < LIVE_SAMPLE_BANKS; ++i) {
        if ((retired & ~used) & (1u << i)) {
            dm_live_sample_bank_free(a->sample_banks[i]);
            a->sample_banks[i] = NULL;
        }
    }
}

static void DME_CALL live_sample_warning(void *user, uint32_t severity, uint32_t flags,
                                         const char *message) {
    (void)flags;
    char *warning = user;

    if (severity == DME_WARNING && !*warning) {
        snprintf(warning, sizeof(((DmAudio *)0)->sample_warning), "%s", message);
    }
}

static bool publish_schedule(DmAudio *a, const DmProject *p, const DmPatternPreview *preview,
                             DmSchedule next, double seconds, bool playing, bool editing,
                             char *error, size_t cap) {
    dm_audio_collect_sample_banks(a);
    int slot = a->sample_bank;
    DmLiveSampleBank *bank = slot < 0 ? NULL : a->sample_banks[slot], *fresh_bank = NULL;
    char warning[sizeof(a->sample_warning)] = {0};

    if (a->sample_bank_dirty || !dm_live_sample_bank_matches(bank, p)) {
        slot = -1;
        bank = NULL;

        if (p->patch_replacement_count) {
            for (int i = 0; i < LIVE_SAMPLE_BANKS; ++i) {
                if (!a->sample_banks[i]) {
                    slot = i;
                    break;
                }
            }

            if (slot < 0) {
                snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_BUSY);
                dm_schedule_free(&next);
                return false;
            }

            DmeHost host = {.user = warning, .diagnostic = live_sample_warning};
            bank = fresh_bank = dm_live_sample_bank_create(p, &host);

            if (!bank) {
                snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_PLAYBACK);
                dm_schedule_free(&next);
                return false;
            }
        }
    }

    /* Prepare seek state outside the audio lock. Swap only bounded state in the critical section.
     */
    DmAudio *scratch = calloc(1, sizeof(*scratch));
    DmAudio *loop = NULL;
    RetiredSchedule *retired = calloc(1, sizeof(*retired));

    if (!scratch || !retired || !prepare_note_ends(&next, p, preview, bank)) {
        free(scratch);
        free(retired);
        dm_live_sample_bank_free(fresh_bank);
        dm_schedule_free(&next);
        return false;
    }

    prepare_chase(scratch, &next, seconds);

    if (a->loop_enabled) {
        loop = calloc(1, sizeof(*loop));

        if (!loop) {
            free(scratch);
            free(retired);
            dm_live_sample_bank_free(fresh_bank);
            dm_schedule_free(&next);
            return false;
        }

        prepare_chase(loop, &next, a->loop_start);
    }

    size_t capacity;
    NoteRelease *releases = prepare_release_storage(a, next.count, &capacity);

    if (!releases) {
        free(scratch);
        free(retired);
        free(loop);
        dm_live_sample_bank_free(fresh_bank);
        dm_schedule_free(&next);
        return false;
    }

    Synth *fresh[DM_NOTE_PORTS] = {0};

    if (!prepare_ports(a, scratch->used_ports, fresh, error, cap)) {
        free(releases);
        free(scratch);
        free(retired);
        free(loop);
        dm_live_sample_bank_free(fresh_bank);
        dm_schedule_free(&next);
        return false;
    }

    SDL_LockMutex(a->device_lock);

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    bool preserve =
        install_release_storage(a, scratch, &next, releases, editing, &seconds, &playing);
    bool was_playing = a->playing;
    NoteRelease *old_releases = a->releases;
    a->releases = releases;
    a->release_capacity = capacity;
    DmSchedule old = a->schedule;
    a->schedule = next;

    if (slot != a->sample_bank) {
        if (fresh_bank) {
            a->sample_banks[slot] = fresh_bank;
        }

        a->sample_bank = slot;
        memcpy(a->sample_warning, warning, sizeof(warning));
    }

    a->sample_bank_dirty = false;

    a->freewheel_rate = next.final_rate;
    a->freewheel_click = next.next_beat;

    if (seconds > a->freewheel_click) {
        a->freewheel_click +=
            ceil((seconds - a->freewheel_click) / a->freewheel_rate) * a->freewheel_rate;
    }

    a->cursor = scratch->cursor;
    a->beat_cursor = scratch->beat_cursor;

    if (!preserve) {
        SDL_SetAtomicInt(&a->midi.read, SDL_GetAtomicInt(&a->midi.write));
    }

    memcpy(a->used_ports, scratch->used_ports, sizeof(a->used_ports));

    if (loop) {
        dm_audio_copy_chase(&a->loop_state, &loop->state, a->used_ports);
        dm_audio_copy_part_rows(a->loop_note_end, loop->seek_end, sizeof(a->loop_note_end[0]),
                                a->used_ports);
        a->loop_cursor = loop->cursor;
        a->loop_beat_cursor = loop->beat_cursor;
    }

    for (unsigned port = 1; port < DM_NOTE_PORTS; port++) {
        if (fresh[port]) {
            a->port_live[port] = fresh[port];
        }
    }

    dm_audio_copy_part_rows(a->seek_end, scratch->seek_end, sizeof(a->seek_end[0]), a->used_ports);
    dm_audio_copy_chase(&a->state, &scratch->state, a->used_ports);
    dm_audio_mix_from_state(a);
    a->preview_mask = project_preview_mask(p);
    memcpy(a->keyboard_channels, p->channels, sizeof(a->keyboard_channels));

    for (int part = 0; part < DM_PARTS; ++part) {
        int channel = part & MIDI_CHANNEL_MASK;
        int program = p->channels[channel].program;
        a->keyboard_programs[part] = program < 0 ? 0 : (uint8_t)program;
    }

    for (size_t i = 0; i < p->port_mix_count; ++i) {
        DmPortMix mix = p->port_mix[i];

        if (mix.program) {
            a->keyboard_programs[mix.port * 16 + mix.channel] = mix.program - 1;
        }
    }

    a->keyboard_config = true;
    a->chase = preserve ? a->chase : playing && scratch->chase;
    a->position = seconds;
    a->playing = playing;
    a->reset = a->reset || !preserve;
    /* Running seeks and song wraps preserve the piano's held voices and pedal state. */
    a->input_reset |= !editing && (!playing || !was_playing);
    dm_audio_input_clock(a, true);
    SDL_SetAtomicInt(&a->position_ms, (int)(seconds * 1000));
    SDL_SetAtomicInt(&a->playing_flag, playing);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    if (!preserve || SDL_GetAtomicInt(&a->midi.read) == SDL_GetAtomicInt(&a->midi.write)) {
        dm_audio_free_retired(a);
    }

    retired->schedule = old;
    retired->next = a->retired;
    a->retired = retired;
    SDL_UnlockMutex(a->device_lock);
    bool colored = false;

    for (unsigned port = 1; port < DM_NOTE_PORTS; port++) {
        colored |= a->used_ports[port];
    }

    if (colored && a->device_index >= 0 && !a->port_warning_sent) {
        SDL_SetAtomicInt(&a->port_warning, 1);
        a->port_warning_sent = true;
    }

    if (!colored) {
        a->port_warning_sent = false;
    }

    free(old_releases);
    free(scratch);
    free(loop);
    dm_audio_collect_sample_banks(a);
    return true;
}

bool dm_audio_publish(DmAudio *a, const DmProject *p, double seconds, bool playing, char *error,
                      size_t cap) {
    if (!a) {
        return false;
    }

    DmSchedule next = {0};

    if (!dm_schedule(p, &next)) {
        snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_PLAYBACK);
        return false;
    }

    return publish_schedule(a, p, NULL, next, seconds, playing, false, error, cap);
}

void dm_audio_reload_samples(DmAudio *a) {
    if (a) {
        a->sample_bank_dirty = true;
    }
}

static void preview_channels(DmProject *view, const DmPatternPreview *v) {
    for (int c = 0; c < 16; ++c) {
        view->channels[c].mute = false;
        view->channels[c].solo = false;

        if (v->program >= 0) {
            view->channels[c].enabled = false;
        }
    }

    view->channels[v->channel].enabled = true;

    if (v->program >= 0 && !v->follow) {
        view->channels[v->channel].program = v->program == 128 ? 0 : v->program;
    }
}

/* One remembered value per channel message type and controller. */
enum { PREVIEW_CHASE_KEYS = 8 * 128 };

typedef struct {
    bool set;
    double beat;
    const DmEvent *event;
    const DmAutomation *automation;
} PreviewChase;

static PreviewChase *preview_chase_slot(PreviewChase *chase, int type, int a) {
    bool keyed = type == MIDI_CONTROL || type == MIDI_KEY_PRESSURE;
    return &chase[((type >> 4) & 7) * 128 + (keyed ? a & 127 : 0)];
}

static void preview_remember(PreviewChase *slot, double beat, const DmEvent *event,
                             const DmAutomation *automation) {
    if (!slot->set || beat >= slot->beat) {
        *slot = (PreviewChase){true, beat, event, automation};
    }
}

/* Song beat 0 values replaced by the channel settings during playback. */
static bool preview_channel_setting(const uint8_t *m, double beat) {
    int type = m[0] & MIDI_STATUS_MASK;
    return beat == 0 && (type == MIDI_PROGRAM ||
                         (type == MIDI_CONTROL && (m[1] == MIDI_CC_VOLUME || m[1] == MIDI_CC_PAN)));
}

static void preview_chase_raw(PreviewChase *chase, const DmEvent *event) {
    const uint8_t *m = event->message.data;
    int type = m[0] & MIDI_STATUS_MASK;

    if (type != MIDI_NOTE_ON && type != MIDI_NOTE_OFF && !preview_channel_setting(m, event->beat)) {
        preview_remember(preview_chase_slot(chase, type, event->message.size > 1 ? m[1] : 0),
                         event->beat, event, NULL);
    }
}

/* Applies the channel state at the region start: programs, volume and pan become the
   channel settings; other messages are sent at the pattern start. */
static void preview_apply_chase(DmProject *view, const PreviewChase *chase,
                                const DmPatternPreview *v, size_t *automation) {
    DmChannel *channel = &view->channels[v->channel];

    for (int i = 0; i < PREVIEW_CHASE_KEYS; ++i) {
        if (!chase[i].set) {
            continue;
        }

        const DmEvent *event = chase[i].event;
        const DmAutomation *a = chase[i].automation;
        int type = event ? event->message.data[0] & MIDI_STATUS_MASK : a->type;
        int first = event ? (event->message.size > 1 ? event->message.data[1] : 0) : a->a;
        int second = event ? (event->message.size > 2 ? event->message.data[2] : 0) : a->b;

        if (type == MIDI_PROGRAM) {
            if (v->follow) {
                channel->program = first;
            }
        } else if (type == MIDI_CONTROL && first == MIDI_CC_VOLUME) {
            channel->volume = second;
        } else if (type == MIDI_CONTROL && first == MIDI_CC_PAN) {
            channel->pan = second;
        } else if (event) {
            DmEvent moved = *event;
            moved.beat = 0;
            view->events[view->event_count++] = moved;
        } else {
            DmAutomation moved = *a;
            moved.beat = 0;
            moved.curve = 0;
            view->automation[(*automation)++] = moved;
        }
    }
}

/* Plays the channel's song events from the pattern's placement region, starting from the state
   in effect at that point. Tempo and other system events keep their song timing. */
static bool preview_region(DmProject *view, const DmProject *p, const DmPatternPreview *v) {
    PreviewChase *chase = calloc(PREVIEW_CHASE_KEYS, sizeof(*chase));
    view->events = calloc(p->event_count + 1, sizeof(*view->events));
    view->automation = calloc(p->automation_count + 1, sizeof(*view->automation));

    if (!chase || !view->events || !view->automation) {
        free(chase);
        return false;
    }

    double origin = v->origin, end = v->origin + v->length;
    view->event_count = 0;

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *event = &p->events[i];

        if (!event->message.size) {
            continue;
        }

        unsigned status = event->message.data[0];

        if (status >= MIDI_SYSTEM) {
            if (event->beat <= v->length) {
                view->events[view->event_count++] = *event;
            }
        } else if ((int)(status & MIDI_CHANNEL_MASK) != v->channel || event->beat > end) {
            continue;
        } else if (event->beat > origin) {
            DmEvent moved = *event;
            moved.beat -= origin;
            view->events[view->event_count++] = moved;
        } else {
            preview_chase_raw(chase, event);
        }
    }

    size_t automation = 0;

    for (size_t i = 0; i < p->automation_count; ++i) {
        const DmAutomation *a = &p->automation[i];

        if (a->pattern >= 0) {
            view->automation[automation++] = *a;
        } else if (a->channel != v->channel || a->beat > end) {
            continue;
        } else if (a->beat > origin) {
            DmAutomation moved = *a;
            moved.beat -= origin;
            view->automation[automation++] = moved;
        } else {
            preview_remember(preview_chase_slot(chase, a->type, a->a), a->beat, NULL, a);
        }
    }

    preview_apply_chase(view, chase, v, &automation);
    view->automation_count = automation;
    free(chase);
    return true;
}

static void preview_schedule_program(DmSchedule *next, const DmPatternPreview *v) {
    if (v->program < 0) {
        return;
    }

    int route = v->program == 128 ? MIDI_DRUM_CHANNEL : 0;

    for (size_t i = 0; i < next->count; ++i) {
        DmScheduled *event = &next->items[i];

        if (event->status >= MIDI_STATUS && event->status < MIDI_SYSTEM) {
            int type = event->status & MIDI_STATUS_MASK;
            event->status = type | route;

            if (v->follow) {
                continue;
            }

            if (type == MIDI_PROGRAM) {
                event->a = v->program == 128 ? 0 : v->program;
            } else if (type == MIDI_CONTROL &&
                       (event->a == MIDI_CC_BANK || event->a == MIDI_CC_BANK_LSB)) {
                event->b = 0;
            }
        }
    }
}

bool dm_pattern_schedule(const DmProject *p, const DmPatternPreview *v, DmSchedule *next) {
    if (!v || v->pattern < 0 || (size_t)v->pattern >= p->pattern_count || v->channel < 0 ||
        v->channel > 15 || v->program < -1 || v->program > 128 || !isfinite(v->length) ||
        v->length <= 0 || v->length > 10000000 || !isfinite(v->origin) || v->origin < 0) {
        return false;
    }

    DmProject view = *p;
    preview_channels(&view, v);
    DmClip clip = {0, v->pattern, v->channel, 0, v->length, 0};
    view.clips = &clip;
    view.clip_count = 1;
    *next = (DmSchedule){0};
    bool ok = preview_region(&view, p, v) && dm_schedule(&view, next);
    free(view.events);
    free(view.automation);

    if (ok) {
        preview_schedule_program(next, v);
        next->duration = dm_beat_seconds(p, v->length);
    }

    return ok;
}

static bool publish_pattern(DmAudio *a, const DmProject *p, const DmPatternPreview *v,
                            double seconds, bool playing, bool editing, char *error, size_t cap) {
    DmSchedule next;

    if (!a || !v) {
        return false;
    }

    if (!dm_pattern_schedule(p, v, &next)) {
        snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_PATTERN);
        return false;
    }

    return publish_schedule(a, p, v, next, seconds, playing, editing, error, cap);
}

bool dm_audio_publish_pattern(DmAudio *a, const DmProject *p, const DmPatternPreview *pattern,
                              double seconds, bool playing, char *error, size_t cap) {
    return publish_pattern(a, p, pattern, seconds, playing, false, error, cap);
}

bool dm_audio_update(DmAudio *a, const DmProject *p, const DmPatternPreview *pattern, char *error,
                     size_t cap) {
    if (!a) {
        return false;
    }

    double seconds = dm_audio_position(a);
    bool playing = dm_audio_playing(a);

    if (pattern) {
        return publish_pattern(a, p, pattern, seconds, playing, true, error, cap);
    }

    DmSchedule next = {0};

    if (!dm_schedule(p, &next)) {
        return false;
    }

    return publish_schedule(a, p, NULL, next, seconds, playing, true, error, cap);
}
