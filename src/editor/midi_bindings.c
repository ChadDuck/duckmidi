#include "editor/internal.h"
#include "text/en.h"

#define LEARN_NS (10 * SDL_NS_PER_SECOND)
#define MIX_GESTURE_NS (500 * SDL_NS_PER_MS)
#define MIX_PUBLISH_NS (250 * SDL_NS_PER_MS)

static const struct {
    const char *id, *label;
    int event;
} targets[ED_MIDI_TARGETS] = {
    [ED_MIDI_VOLUME] = {"mixer.volume", DM_TEXT_VOLUME},
    [ED_MIDI_PAN] = {"mixer.pan", DM_TEXT_PAN},
    [ED_MIDI_MUTE] = {"mixer.mute", DM_TEXT_MUTE},
    [ED_MIDI_SOLO] = {"mixer.solo", DM_TEXT_SOLO},
    [ED_MIDI_PLAY] = {"action.play", DM_TEXT_PLAY_PAUSE},
    [ED_MIDI_STOP] = {"action.stop", DM_TEXT_STOP},
    [ED_MIDI_RECORD] = {"action.record", DM_TEXT_RECORD},
    [ED_MIDI_METRONOME] = {"action.metronome", DM_TEXT_METRONOME},
    [ED_MIDI_QUANTIZE] = {"action.quantize", DM_TEXT_QUANTIZE},
    [ED_MIDI_UNDO] = {"action.undo", DM_TEXT_UNDO},
    [ED_MIDI_REDO] = {"action.redo", DM_TEXT_REDO},
    [ED_MIDI_CHANNEL_PREVIOUS] = {"action.channel_previous", DM_TEXT_MIDI_BIND_PREVIOUS_CHANNEL},
    [ED_MIDI_CHANNEL_NEXT] = {"action.channel_next", DM_TEXT_MIDI_BIND_NEXT_CHANNEL},
    [ED_MIDI_EVENT_BEND] = {"event.pitch_bend", DM_TEXT_PITCH_BEND, DM_EVENT_BEND},
    [ED_MIDI_EVENT_MODULATION] = {"event.modulation", DM_TEXT_MODULATION, MIDI_CC_MODULATION},
    [ED_MIDI_EVENT_EXPRESSION] = {"event.expression", DM_TEXT_EXPRESSION, MIDI_CC_EXPRESSION},
    [ED_MIDI_EVENT_SUSTAIN] = {"event.sustain", DM_TEXT_SUSTAIN, MIDI_CC_SUSTAIN},
    [ED_MIDI_EVENT_SOSTENUTO] = {"event.sostenuto", DM_TEXT_SOSTENUTO, MIDI_CC_SOSTENUTO},
    [ED_MIDI_EVENT_PORTAMENTO] = {"event.portamento", DM_TEXT_PORTAMENTO, MIDI_CC_PORTAMENTO_TIME},
    [ED_MIDI_EVENT_REVERB] = {"event.reverb", DM_TEXT_REVERB_SEND, MIDI_CC_REVERB},
    [ED_MIDI_EVENT_CHORUS] = {"event.chorus", DM_TEXT_CHORUS_SEND, MIDI_CC_CHORUS},
    [ED_MIDI_EVENT_BRIGHTNESS] = {"event.brightness", DM_TEXT_BRIGHTNESS, MIDI_CC_BRIGHTNESS},
    [ED_MIDI_EVENT_RESONANCE] = {"event.resonance", DM_TEXT_RESONANCE, MIDI_CC_RESONANCE},
    [ED_MIDI_EVENT_TREMOLO_DEPTH] = {"event.tremolo_depth", DM_TEXT_TREMOLO_DEPTH,
                                     MIDI_CC_TREMOLO_DEPTH},
    [ED_MIDI_EVENT_VIBRATO_RATE] = {"event.vibrato_rate", DM_TEXT_VIBRATO_RATE,
                                    MIDI_CC_VIBRATO_RATE},
    [ED_MIDI_EVENT_VIBRATO_DEPTH] = {"event.vibrato_depth", DM_TEXT_VIBRATO_DEPTH,
                                     MIDI_CC_VIBRATO_DEPTH},
};

static bool mixer(EdMidiTarget target) {
    return target.kind < ED_MIDI_PLAY;
}

bool ed_midi_target_event(EdMidiTarget target) {
    return target.kind >= ED_MIDI_EVENT_BEND && target.kind < ED_MIDI_TARGETS;
}

bool ed_midi_target_continuous(EdMidiTarget target) {
    return target.kind == ED_MIDI_VOLUME || target.kind == ED_MIDI_PAN ||
           ed_midi_target_event(target);
}

static bool same_target(EdMidiTarget a, EdMidiTarget b) {
    return a.kind == b.kind && (!mixer(a) || (a.channel == b.channel && a.port == b.port));
}

static bool same_source(const DmMidiBindingSource *a, const DmMidiBindingSource *b) {
    return a->kind == b->kind && a->channel == b->channel && a->number == b->number &&
           (!*a->device || !*b->device || !strcmp(a->device, b->device));
}

void ed_midi_target_id(EdMidiTarget target, char *id, size_t cap) {
    if (mixer(target)) {
        snprintf(id, cap, "%s.%u.%u", targets[target.kind].id, target.channel + 1u,
                 (unsigned)target.port);
    } else {
        snprintf(id, cap, "%s", targets[target.kind].id);
    }
}

bool ed_midi_target_parse(const char *id, EdMidiTarget *target) {
    for (unsigned i = 0; i < ED_MIDI_TARGETS; ++i) {
        const char *base = targets[i].id;
        size_t length = strlen(base);

        if (i >= ED_MIDI_PLAY) {
            if (!strcmp(base, id)) {
                *target = (EdMidiTarget){.kind = i};
                return true;
            }

            continue;
        }

        if (strncmp(id, base, length) || id[length] != '.' ||
            !isdigit((unsigned char)id[length + 1])) {
            continue;
        }

        char *end;
        unsigned long channel = strtoul(id + length + 1, &end, 10);

        if (channel < 1 || channel > MIDI_CHANNELS || *end != '.' ||
            !isdigit((unsigned char)end[1])) {
            return false;
        }

        unsigned long port = strtoul(end + 1, &end, 10);

        if (*end || port >= DM_NOTE_PORTS) {
            return false;
        }

        *target = (EdMidiTarget){i, (uint8_t)(channel - 1), (uint8_t)port};
        return true;
    }

    return false;
}

void ed_midi_target_label(EdMidiTarget target, char *text, size_t cap) {
    if (mixer(target)) {
        snprintf(text, cap, DM_TEXT_MIDI_BIND_STRIP, targets[target.kind].label,
                 target.channel + 1u, (unsigned)target.port);
    } else {
        snprintf(text, cap, "%s", targets[target.kind].label);
    }
}

void ed_midi_source_label(const DmMidiBindingSource *source, char *text, size_t cap) {
    if (source->kind == DM_MIDI_SOURCE_BEND || source->kind == DM_MIDI_SOURCE_PRESSURE) {
        snprintf(
            text, cap, DM_TEXT_MIDI_BIND_CHANNEL_SOURCE,
            *source->device ? source->device : DM_TEXT_MIDI_BIND_ANY_DEVICE, source->channel + 1u,
            source->kind == DM_MIDI_SOURCE_BEND ? DM_TEXT_PITCH_BEND : DM_TEXT_CHANNEL_PRESSURE);
        return;
    }

    snprintf(text, cap, DM_TEXT_MIDI_BIND_SOURCE,
             *source->device ? source->device : DM_TEXT_MIDI_BIND_ANY_DEVICE, source->channel + 1u,
             source->kind == DM_MIDI_SOURCE_CC ? DM_TEXT_MIDI_BIND_CC : DM_TEXT_MIDI_BIND_NOTE,
             (unsigned)source->number);
}

size_t ed_midi_binding_count(const Editor *e) {
    return e->midi_bindings.size / sizeof(EdMidiBinding);
}

EdMidiBinding *ed_midi_binding_at(Editor *e, size_t index) {
    return (EdMidiBinding *)e->midi_bindings.data + index;
}

EdMidiBinding *ed_midi_binding_find(Editor *e, EdMidiTarget target) {
    for (size_t i = 0; i < ed_midi_binding_count(e); ++i) {
        EdMidiBinding *binding = ed_midi_binding_at(e, i);

        if (same_target(binding->target, target)) {
            return binding;
        }
    }

    return NULL;
}

bool ed_midi_binding_append(DmBytes *list, EdMidiBinding binding) {
    const EdMidiBinding *items = (const EdMidiBinding *)list->data;

    for (size_t i = 0; i < list->size / sizeof(*items); ++i) {
        if (same_target(items[i].target, binding.target) ||
            same_source(&items[i].source, &binding.source)) {
            return false;
        }
    }

    return dm_bytes(list, &binding, sizeof(binding));
}

static bool publish_sources(DmMidiInput *input, const DmBytes *list) {
    if (!input) {
        return true;
    }

    const EdMidiBinding *bindings = (const EdMidiBinding *)list->data;
    size_t count = list->size / sizeof(*bindings);
    DmMidiBindingSource *sources = count ? malloc(count * sizeof(*sources)) : NULL;

    if (count && !sources) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        sources[i] = bindings[i].source;
    }

    bool ok = dm_midi_input_bind_sources(input, sources, count);
    free(sources);
    return ok;
}

bool ed_midi_bindings_attach(Editor *e, DmMidiInput *input) {
    return publish_sources(input, &e->midi_bindings);
}

bool ed_midi_bindings_replace(Editor *e, DmBytes *list) {
    if (!publish_sources(e->midi_input, list)) {
        return false;
    }

    dm_bytes_free(&e->midi_bindings);
    e->midi_bindings = *list;
    *list = (DmBytes){0};
    return true;
}

/* Replaces the binding of `target` (and any binding of new_binding's source) with new_binding.
   A NULL target removes every binding. */
static void change_binding(Editor *e, const EdMidiTarget *target,
                           const EdMidiBinding *new_binding) {
    DmBytes next = {0};
    bool ok = true;

    for (size_t i = 0; i < ed_midi_binding_count(e) && ok && target; ++i) {
        EdMidiBinding *old = ed_midi_binding_at(e, i);

        if (!same_target(old->target, *target) &&
            (!new_binding || !same_source(&old->source, &new_binding->source))) {
            ok = dm_bytes(&next, old, sizeof(*old));
        }
    }

    ok = ok && (!new_binding || dm_bytes(&next, new_binding, sizeof(*new_binding))) &&
         ed_midi_bindings_replace(e, &next);
    dm_bytes_free(&next);

    if (ok) {
        if (new_binding) {
            ed_message(e, DM_TEXT_MIDI_BIND_LEARNED);
        }

        ed_preference_save(e);
    } else {
        ed_message(e, DM_TEXT_MIDI_BIND_MEMORY);
    }
}

bool ed_midi_bindings_cancel(Editor *e) {
    if (!e->midi_learn.started) {
        return false;
    }

    e->midi_learn.started = 0;

    if (e->midi_input) {
        dm_midi_input_learn(e->midi_input, DM_MIDI_LEARN_OFF);
    }

    return true;
}

void ed_midi_binding_clear(Editor *e, EdMidiTarget target) {
    if (same_target(e->midi_learn.target, target)) {
        ed_midi_bindings_cancel(e);
    }

    change_binding(e, &target, NULL);
}

void ed_midi_bindings_clear_all(Editor *e) {
    ed_midi_bindings_cancel(e);
    change_binding(e, NULL, NULL);
}

void ed_midi_binding_learn(Editor *e, EdMidiTarget target, bool any_device) {
    if (!e->midi_input) {
        ed_message(e, DM_TEXT_MIDI_INPUT_FAILED);
        return;
    }

    e->percussion_midi_listening = false;
    e->midi_learn = (EdMidiLearn){target, SDL_GetTicksNS(), any_device};
    dm_midi_input_learn(e->midi_input, ed_midi_target_event(target)        ? DM_MIDI_LEARN_EVENT
                                       : ed_midi_target_continuous(target) ? DM_MIDI_LEARN_CC
                                                                           : DM_MIDI_LEARN_BUTTON);
}

bool ed_midi_binding_learning(const Editor *e, EdMidiTarget target) {
    return e->midi_learn.started && same_target(e->midi_learn.target, target);
}

static void learn(Editor *e, DmMidiInputEvent event, DmMidiBindingSource source) {
    EdMidiLearn session = e->midi_learn;

    if (!session.started || event.ticks_ns < session.started) {
        return;
    }

    EdMidiBinding *old = ed_midi_binding_find(e, session.target);

    if (session.any_device) {
        source.device[0] = 0;
    }

    EdMidiBinding binding = {
        .source = source, .target = session.target, .toggle = old && old->toggle};

    if (!ed_midi_target_continuous(session.target) && source.kind == DM_MIDI_SOURCE_CC) {
        binding.input[event.device].previous = event.b;
    }

    change_binding(e, &session.target, &binding);
    ed_midi_bindings_cancel(e);
}

void ed_midi_bindings_reset_pickup(Editor *e) {
    for (size_t i = 0; i < ed_midi_binding_count(e); ++i) {
        EdMidiBinding *binding = ed_midi_binding_at(e, i);

        if (ed_midi_target_continuous(binding->target)) {
            memset(binding->input, 0, sizeof(binding->input));
        }
    }
}

static bool pickup(Editor *e, EdMidiPickup *state, int value, int current) {
    if (state->seen && state->expected != current) {
        *state = (EdMidiPickup){0};
    }

    bool crossed = state->seen && ((state->previous <= current && value >= current) ||
                                   (state->previous >= current && value <= current));
    state->picked |= !e->input_pickup || abs(value - current) <= 2 || crossed;
    state->seen = true;
    state->expected = state->picked ? value : current;
    state->previous = value;
    return state->picked;
}

void ed_midi_bindings_flush(Editor *e, uint64_t now) {
    if (e->midi_mix_dirty) {
        ed_changed(e);
        e->midi_mix_dirty = false;
    }

    if (e->midi_mix_pending && now >= e->midi_mix_last &&
        now - e->midi_mix_last >= MIX_PUBLISH_NS) {
        ed_publish(e);
    }
}

void ed_midi_bindings_tick(Editor *e, uint64_t now) {
    if (e->midi_binding_session != e->project_session) {
        e->midi_binding_session = e->project_session;
        e->midi_mix_last = 0;
        e->midi_mix_pending = e->midi_mix_dirty = false;
        ed_midi_bindings_reset_pickup(e);
    }

    if (e->midi_learn.started && now - e->midi_learn.started >= LEARN_NS) {
        ed_midi_bindings_cancel(e);
        ed_message(e, DM_TEXT_MIDI_BIND_TIMEOUT);
    }
}

static void mixer_event(Editor *e, EdMidiBinding *binding, DmMidiInputEvent event) {
    EdMidiTarget target = binding->target;

    if (!e->active || !e->project.channels[target.channel].enabled) {
        return;
    }

    DmPortMix before = dm_port_mix(&e->project, target.port, target.channel), next = before;
    bool continuous = ed_midi_target_continuous(target);
    EdMidiPickup *state = &binding->input[event.device];

    if (continuous) {
        int *value = target.kind == ED_MIDI_VOLUME ? &next.volume : &next.pan;

        if (!pickup(e, state, event.b, *value) || *value == event.b) {
            return;
        }

        *value = event.b;
    } else {
        bool *value = target.kind == ED_MIDI_MUTE ? &next.mute : &next.solo;
        *value = !*value;
    }

    /* Continuous moves share an undo step until they pause; every mute or solo press is its own. */
    uint64_t ns = event.ticks_ns;
    bool new_gesture = !continuous || !e->midi_mix_last ||
                       (ns >= e->midi_mix_last && ns - e->midi_mix_last >= MIX_GESTURE_NS);
    bool checkpoint = false;

    if (new_gesture) {
        ed_midi_bindings_flush(e, ns);

        if (!(e->recording_take && e->take_snapshot)) {
            checkpoint = ed_checkpoint(e);

            if (!checkpoint) {
                return;
            }
        }
    }

    if (!ed_mixer_apply(e, before, next)) {
        ed_checkpoint_abandon(e, checkpoint);
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_PORT_MIXER);
        return;
    }

    if (ns > e->midi_mix_last) {
        e->midi_mix_last = ns;
    }

    e->midi_mix_dirty = e->midi_mix_pending = true;

    /* A mute or solo press affects which parts the scheduler plays, so apply it immediately. */
    if (!continuous) {
        ed_midi_bindings_flush(e, ns);
        ed_publish(e);
        e->midi_mix_last = 0;
    }
}

static bool pressed(EdMidiBinding *binding, DmMidiInputEvent event) {
    EdMidiPickup *state = &binding->input[event.device];
    unsigned type = event.status & MIDI_STATUS_MASK;
    unsigned value = type == MIDI_CONTROL ? event.b : type == MIDI_NOTE_ON ? MIDI_DATA_MAX : 0;
    unsigned previous = state->previous;
    state->previous = value;

    if (binding->toggle) {
        return value != previous;
    }

    return type == MIDI_NOTE_ON || (type == MIDI_CONTROL && value >= 64 && previous < 64);
}

static void select_channel(Editor *e, int step) {
    if (!e->active) {
        return;
    }

    int channel = e->selected_channel;

    for (int i = 0; i < MIDI_CHANNELS; ++i) {
        channel = (channel + MIDI_CHANNELS + step) % MIDI_CHANNELS;

        if (e->project.channels[channel].enabled) {
            if (!e->channel_selected || channel != e->selected_channel) {
                ed_release_piano(e);
                e->selected_channel = channel;
                e->channel_selected = true;
            }

            return;
        }
    }
}

static void action(Editor *e, EdMidiTargetKind kind) {
    static const Action actions[] = {
        [ED_MIDI_RECORD] = ACT_RECORD,
        [ED_MIDI_METRONOME] = ACT_METRONOME,
        [ED_MIDI_QUANTIZE] = ACT_QUANTIZE,
    };

    switch (kind) {
    case ED_MIDI_PLAY:
        ed_play_hold(e, true);
        break;
    case ED_MIDI_STOP: {
        /* The first press returns to the set playhead; another press, already stopped there,
           returns to the beginning. */
        double beat = e->start_beat[e->pattern_mode];
        bool home = !dm_audio_playing(e->audio) && !e->wait_armed &&
                    fabs(e->cursor_beat[e->pattern_mode] - beat) < 1e-9;

        if (dm_audio_playing(e->audio)) {
            ed_play(e);
        } else {
            ed_cancel_wait(e);
        }

        ed_seek(e, home ? 0 : beat);
        break;
    }
    case ED_MIDI_UNDO:
    case ED_MIDI_REDO:
        ed_midi_input_finish(e, false);
        ed_undo(e, kind == ED_MIDI_REDO);
        break;
    case ED_MIDI_CHANNEL_PREVIOUS:
    case ED_MIDI_CHANNEL_NEXT:
        select_channel(e, kind == ED_MIDI_CHANNEL_NEXT ? 1 : -1);
        break;
    default:
        ed_action(e, actions[kind]);
        break;
    }
}

void ed_midi_binding_event(Editor *e, DmMidiInputEvent event, const char *device,
                           bool events_only) {
    if (event.flags & DM_MIDI_EVENT_DEVICE_RELEASE) {
        for (size_t i = 0; i < ed_midi_binding_count(e); ++i) {
            ed_midi_binding_at(e, i)->input[event.device] = (EdMidiPickup){0};
        }

        return;
    }

    unsigned type = event.status & MIDI_STATUS_MASK;
    DmMidiBindingSource source = {
        .kind = type == MIDI_CONTROL            ? DM_MIDI_SOURCE_CC
                : type == MIDI_PITCH_BEND       ? DM_MIDI_SOURCE_BEND
                : type == MIDI_CHANNEL_PRESSURE ? DM_MIDI_SOURCE_PRESSURE
                                                : DM_MIDI_SOURCE_NOTE,
        .channel = event.status & MIDI_CHANNEL_MASK,
        .number = type == MIDI_PITCH_BEND || type == MIDI_CHANNEL_PRESSURE ? 0 : event.a,
    };
    snprintf(source.device, sizeof(source.device), "%s", device);

    if (event.flags & DM_MIDI_EVENT_LEARN) {
        if (!events_only) {
            learn(e, event, source);
        }
        return;
    }

    for (size_t i = 0; i < ed_midi_binding_count(e); ++i) {
        EdMidiBinding *binding = ed_midi_binding_at(e, i);

        if (!same_source(&binding->source, &source)) {
            continue;
        }

        if (events_only && !ed_midi_target_event(binding->target)) {
            return;
        }

        if (ed_midi_target_event(binding->target)) {
            unsigned value = type == MIDI_PITCH_BEND         ? event.a | (event.b << 7)
                             : type == MIDI_CHANNEL_PRESSURE ? event.a
                                                             : event.b;
            int target = targets[binding->target.kind].event;

            if (target == DM_EVENT_BEND) {
                if (type != MIDI_PITCH_BEND) {
                    value = value * MIDI_BEND_MAX / MIDI_DATA_MAX;
                }

                event.status = MIDI_PITCH_BEND | source.channel;
                event.a = value & MIDI_DATA_MASK;
                event.b = value >> 7;
            } else {
                if (type == MIDI_PITCH_BEND) {
                    value = value * MIDI_DATA_MAX / MIDI_BEND_MAX;
                }

                event.status = MIDI_CONTROL | source.channel;
                event.a = target;
                event.b = value;
            }

            ed_midi_input_bound_event(e, event);
        } else if (mixer(binding->target)) {
            if (ed_midi_target_continuous(binding->target) || pressed(binding, event)) {
                mixer_event(e, binding, event);
            }
        } else if (pressed(binding, event)) {
            ed_midi_bindings_flush(e, event.ticks_ns);
            action(e, binding->target.kind);
        }

        break;
    }
}
