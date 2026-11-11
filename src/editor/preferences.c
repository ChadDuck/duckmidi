#include "editor/internal.h"
#include "text/en.h"
#include "../../shared/sample/export_snapshot.h"

static bool source_port_controls(Editor *e, const char *text, int *port);
static void source_settings(Editor *e);
static void combine_controls(Editor *e);
static void project_fields(Editor *e);
static void new_project_fields(Editor *e);
static void midi_preferences(Editor *e);
static void shortcut_row(Editor *e, int i);

static bool source_port_controls(Editor *e, const char *text, int *port) {
    if (e->project.port != DM_PORT_COMBINED && ui_combo(DM_TEXT_SOURCE_PORT, text)) {
        for (int i = 0; i < e->project.port_count; ++i) {
            char s[32];
            snprintf(s, sizeof(s), DM_TEXT_PORT, e->project.ports[i]);

            if (ui_select(s, *port == e->project.ports[i], 0, 0)) {
                *port = e->project.ports[i];
            }
        }

        ui_combo_end();
    }

    return true;
}

static bool colored_ports(Editor *e) {
    const DmProject *p = &e->project;

    for (size_t i = 0; i < p->pattern_count; ++i) {
        for (size_t n = 0; n < p->patterns[i].count; ++n) {
            if (p->patterns[i].notes[n].port) {
                return true;
            }
        }
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        if (p->automation[i].port) {
            return true;
        }
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        if (p->events[i].port) {
            return true;
        }
    }

    return false;
}

/* Imported multi-port files can merge only their original ports, or every colored port. */
static void combine_controls(Editor *e) {
    if (!colored_ports(e)) {
        return;
    }

    bool imported = e->project.original.size > 0;
    bool original = imported && e->project.port == DM_PORT_COMBINED && e->project.port_count > 1;
    ui_separator();

    if (original) {
        if (ui_button(DM_TEXT_COMBINE_ORIGINAL_SOURCE, 250, 0)) {
            ed_request_combine(e, false);
        }

        ui_tip(DM_TEXT_COMBINE_ORIGINAL_HINT);
        ui_same();
    }

    if (ui_button(imported ? DM_TEXT_COMBINE_ALL_SOURCE_PORTS : DM_TEXT_COMBINE_SOURCE_PORTS, 250,
                  0)) {
        ed_request_combine(e, true);
    }

    ui_tip(DM_TEXT_COMBINE_HINT);
}

static void source_settings(Editor *e) {
    if (e->project.original.size) {
        ui_separator();
        char text[128];

        if (e->project.port == DM_PORT_COMBINED) {
            snprintf(text, sizeof(text), DM_TEXT_COMBINED_SOURCE_PORTS, e->project.port_count,
                     e->project.sequence + 1, e->project.sequence_count);
        } else {
            snprintf(text, sizeof(text), DM_TEXT_PORT_SEQUENCE, e->project.port,
                     e->project.sequence + 1, e->project.sequence_count);
        }

        if (e->project.port == DM_PORT_COMBINED) {
            ui_text(text);
        }

        ui_dim(e->project.port == DM_PORT_COMBINED ? DM_TEXT_EDITS_RETAINED_SEPARATELY_EACH_SEQUENCE
                                                   : DM_TEXT_EDITS_RETAINED_SEPARATELY_EACH_PORT);
        int port = e->project.port, sequence = e->project.sequence;

        if (!source_port_controls(e, text, &port)) {
            return;
        }

        int shown = sequence + 1;

        if (ui_int(DM_TEXT_SOURCE_SEQUENCE, &shown, 1, e->project.sequence_count)) {
            sequence = shown - 1;
        }

        if (port != e->project.port || sequence != e->project.sequence) {
            e->midi_events = false;
            e->midi_event_index = -1;
            e->song_event = false;
            ed_release_piano(e);
            ed_checkpoint(e);
            dm_audio_transport(e->audio, false);
            e->record = false;
            e->recording_take = e->take_snapshot = false;
            dm_audio_recording(e->audio, false);

            if (dm_view_switch(&e->project, port, sequence, e->status, sizeof(e->status))) {
                ed_close_instrument(e);
                e->last_note_pattern_uid = 0;
                e->selected_pattern = e->project.pattern_count ? 0 : -1;
                e->drag = 0;
                e->marquee = false;

                if (e->project.clip_count) {
                    e->selected_pattern = e->project.clips[0].pattern;
                    e->pattern_channel = e->selected_channel = e->project.clips[0].channel;
                }

                ed_changed(e);
                memcpy(e->reset_channels, e->project.channels, sizeof(e->reset_channels));
                e->pattern_mode = false;
                dm_audio_publish(e->audio, &e->project, 0, false, e->status, sizeof(e->status));
            }
        }
    }
}

static void project_fields(Editor *e) {
    char name[128];
    snprintf(name, sizeof(name), "%s", e->project.name);

    if (ui_input(DM_TEXT_NAME, name, sizeof(name))) {
        ed_checkpoint(e);
        snprintf(e->project.name, sizeof(e->project.name), "%s", name);
        ed_changed(e);
    }

    float bpm = (float)e->project.bpm;

    if (ui_float(DM_TEXT_INITIAL_TEMPO, &bpm, DM_BPM_MIN, DM_BPM_MAX)) {
        ed_checkpoint(e);
        e->project.bpm = bpm;
        ed_commit(e);
    }

    int numerator = e->project.numerator;

    if (ui_int(DM_TEXT_BEATS_PER_BAR, &numerator, 1, 255)) {
        ed_checkpoint(e);
        e->project.numerator = numerator;
        ed_commit(e);
    }

    const char *denoms[] = {
        DM_TEXT_ONE,     DM_TEXT_TWO,        DM_TEXT_FOUR,       DM_TEXT_EIGHT,
        DM_TEXT_SIXTEEN, DM_TEXT_THIRTY_TWO, DM_TEXT_SIXTY_FOUR, DM_TEXT_ONE_TWENTY_EIGHT};
    char d[16];
    snprintf(d, sizeof(d), "%d", e->project.denominator);

    if (ui_combo(DM_TEXT_BEAT_UNIT, d)) {
        for (int i = 0; i < 8; ++i) {
            if (ui_select(denoms[i], e->project.denominator == (1 << i), 0, 0)) {
                ed_checkpoint(e);
                e->project.denominator = 1 << i;
                ed_commit(e);
            }
        }

        ui_combo_end();
    }
}

static void new_project_fields(Editor *e) {
    if (!e->new_numerator) {
        snprintf(e->new_name, sizeof(e->new_name), DM_TEXT_UNTITLED);
        e->new_bpm = 120;
        e->new_numerator = e->new_denominator = 4;
    }

    ui_input(DM_TEXT_NAME, e->new_name, sizeof(e->new_name));
    ui_float(DM_TEXT_INITIAL_TEMPO, &e->new_bpm, DM_BPM_MIN, DM_BPM_MAX);
    ui_int(DM_TEXT_BEATS_PER_BAR, &e->new_numerator, 1, 255);
    char text[16];
    snprintf(text, sizeof(text), "%d", e->new_denominator);

    if (ui_combo(DM_TEXT_BEAT_UNIT, text)) {
        for (int i = 0; i < 8; ++i) {
            snprintf(text, sizeof(text), "%d", 1 << i);

            if (ui_select(text, e->new_denominator == (1 << i), 0, 0)) {
                e->new_denominator = 1 << i;
            }
        }

        ui_combo_end();
    }
}

void ed_settings(Editor *e) {
    if (!e->config) {
        return;
    }

    bool show = ui_content_dialog(e->new_mode ? DM_TEXT_NEW_PROJECT : DM_TEXT_PROJECT_SETTINGS,
                                  &e->config, 570);

    if (show) {
        ui_text(e->new_mode ? DM_TEXT_START_SOMETHING_NEW : DM_TEXT_PROJECT_MIDI_IMPORT);
        ui_separator();

        if (e->new_mode) {
            new_project_fields(e);
            ui_dim(DM_TEXT_CHANNEL_1_CHANNEL);

            if (ui_button(DM_TEXT_CREATE_PROJECT, ui_available().x, 0) || ui_dialog_enter()) {
                ed_new_project(e);
                snprintf(e->project.name, sizeof(e->project.name), "%s", e->new_name);
                e->project.bpm = e->new_bpm;
                e->project.numerator = e->new_numerator;
                e->project.denominator = e->new_denominator;
                ed_pattern(e)->length = dm_bar(&e->project);
                ed_publish(e);
                e->new_mode = 0;
                e->config = false;
            }
        } else if (e->active) {
            project_fields(e);
            source_settings(e);
            combine_controls(e);

            if (ui_button("Apply", ui_available().x, 0) || ui_dialog_enter()) {
                e->config = false;
            }
        }
    }

    ui_end();

    if (!e->config) {
        e->new_mode = 0;
    }
}

static int instrument_sample_key(bool drums) {
    return drums ? 36 : 60;
}

static void stop_instrument_preview(Editor *e) {
    ed_release_piano(e);
    int c = e->instrument_channel;

    if (e->instrument_audition) {
        dm_audio_sample_audition_stop(e->audio);
        e->instrument_audition = false;
    }

    if (c >= 0 && e->preview_route == c) {
        dm_audio_preview(e->audio, c, instrument_sample_key(e->instrument_drums), 0, false);
        dm_audio_keyboard_program(e->audio, c, -1, false);
        e->preview_route = -1;
    }
}

void ed_close_instrument(Editor *e) {
    if (e->instrument_channel >= 0) {
        stop_instrument_preview(e);
    }

    e->instrument_channel = -1;
    e->instrument_new = e->instrument_preview = e->instrument_remove = e->instrument_raise = false;
    e->instrument_source_dirty = false;
}

int ed_initial_bank(const DmProject *p, int port, int channel) {
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration;
    char error[256];
    DmSamplePatchState state = {0};
    unsigned opening_bank = 0;

    if (dm_export_timeline(p, &events, &count, &duration, error, sizeof(error))) {
        for (size_t i = 0; i < count; ++i) {
            DmExportEvent ev = events[i];

            if (ev.port != (unsigned)port || ev.status >= MIDI_SYSTEM ||
                (ev.status & MIDI_CHANNEL_MASK) != (unsigned)channel) {
                continue;
            }

            if ((ev.status & MIDI_STATUS_MASK) == MIDI_NOTE_ON && ev.b) {
                opening_bank = state.selected;
                break;
            }

            dm_sample_patch_state_event(&state, ev.status, ev.a, ev.b);

            if (!ev.tick) {
                opening_bank = state.selected;
            }
        }
    }

    free(events);
    return (int)opening_bank;
}

static void instrument_route(Editor *e, int port) {
    int c = e->instrument_channel;
    DmPortMix mix = dm_port_mix(&e->project, port, c);
    e->instrument_port = port;
    e->instrument_program = e->instrument_new ? 0
                            : mix.program     ? mix.program - 1
                                              : (int)fmax(0, e->project.channels[c].program);
    e->instrument_bank = e->instrument_new ? 0 : ed_initial_bank(&e->project, port, c);
    e->instrument_source_dirty = false;
}

static void preview_sample_source(Editor *e, const DmPatchReplacement *replacement) {
    stop_instrument_preview(e);

    DmPatchReplacement routed = *replacement;
    routed.key.port = (uint8_t)e->instrument_port;
    routed.key.channel = (uint8_t)e->instrument_channel;

    if (dm_audio_instrument_preview(e->audio, &routed, e->instrument_channel, e->status,
                                    sizeof(e->status))) {
        e->instrument_audition = true;
        e->preview_route = e->instrument_channel;
    }
}

void ed_open_instrument(Editor *e, int channel, bool preview) {
    bool create = channel < 0;
    int c = create ? ed_available_channel(e) : channel;

    if (!e->active || c < 0 || c >= 16 || (!create && !e->project.channels[c].enabled)) {
        return;
    }

    ed_close_instrument(e);
    ed_release_piano(e);
    e->instrument_channel = c;
    e->instrument_new = create;
    e->instrument_preview = preview;
    e->instrument_drums = create ? c == MIDI_DRUM_CHANNEL : e->project.channels[c].drums;
    instrument_route(e, create ? 0 : e->note_ports[c]);
    e->instrument_raise = true;
}

static void select_instrument(Editor *e, int program, bool drums) {
    stop_instrument_preview(e);
    e->instrument_source_dirty = false;
    e->instrument_program = program;
    e->instrument_drums = drums;
    int c = e->instrument_channel;

    if (e->preview_route >= 0 && e->preview_route != c) {
        dm_audio_keyboard_program(e->audio, e->preview_route, -1, false);
    }

    DmPatchKey key = {(uint16_t)e->instrument_bank, (uint8_t)e->instrument_port, (uint8_t)c,
                      (uint8_t)program, drums};
    const DmPatchReplacement *replacement = dm_patch_replacement(&e->project, key);
    DmPatchReplacement stock = {.key = key};
    preview_sample_source(e, replacement ? replacement : &stock);
}

void ed_step_instrument(Editor *e, int direction) {
    for (int program = e->instrument_program + direction; program >= 0 && program < 128;
         program += direction) {
        if (!e->instrument_drums || dm_drum_kit_name(program)) {
            select_instrument(e, program, e->instrument_drums);
            return;
        }
    }
}

/* Clear channel-owned MIDI when removing a row, or the imported mode settings when changing
   between Melody and Drums. Song-wide events and pattern-local events are kept. */
/* Whether an event belongs to channel: its voice messages, or its GS/XG rhythm-part setup. A
   type change keeps the voice messages and drops only setup that no longer applies. */
static bool channel_event(const DmProject *p, const DmEvent *event, int channel, bool remove) {
    const DmBytes *m = &event->message;
    int part;
    bool drums;
    bool rhythm = dm_midi_rhythm_part(m, &part, &drums) && part == channel;
    bool voice = m->size && m->data[0] >= MIDI_STATUS && m->data[0] < MIDI_SYSTEM &&
                 (m->data[0] & MIDI_CHANNEL_MASK) == channel;
    bool drum_bank = voice && m->size == 3 && m->data[0] == (MIDI_CONTROL | channel) &&
                     m->data[1] == MIDI_CC_BANK && (m->data[2] == 120 || m->data[2] == 127);
    return rhythm || (remove && voice) || (!p->channels[channel].drums && drum_bank);
}

static void clear_channel_events(DmProject *p, int channel, bool remove) {
    size_t write = 0;

    for (size_t i = 0; i < p->event_count; ++i) {
        DmEvent *event = &p->events[i];

        if (channel_event(p, event, channel, remove)) {
            dm_bytes_free(&event->message);
        } else {
            p->events[write++] = *event;
        }
    }

    p->event_count = write;
}

/* Removing a channel deletes more than its placements, so it asks first whenever any of it
   would be lost. */
static bool channel_has_content(Editor *e, int channel) {
    const DmProject *p = &e->project;

    if (ed_channel_has_clips(e, channel)) {
        return true;
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        if (channel_event(p, &p->events[i], channel, true)) {
            return true;
        }
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        if (p->automation[i].pattern < 0 && p->automation[i].channel == channel) {
            return true;
        }
    }

    return false;
}

bool ed_remove_channel(Editor *e, int channel) {
    if (channel < 0 || channel >= 16 || !e->project.channels[channel].enabled) {
        return false;
    }

    ed_release_piano(e);
    ed_checkpoint(e);
    DmProject *p = &e->project;

    for (size_t i = p->clip_count; i > 0; --i) {
        if (p->clips[i - 1].channel == channel) {
            ed_remove_placement(e, i - 1);
        }
    }

    size_t write = 0;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation point = p->automation[i];

        if (point.pattern >= 0 || point.channel != channel) {
            p->automation[write++] = point;
        }
    }

    p->automation_count = write;
    write = 0;

    for (size_t i = 0; i < p->port_mix_count; ++i) {
        if (p->port_mix[i].channel != channel) {
            p->port_mix[write++] = p->port_mix[i];
        }
    }

    p->port_mix_count = write;
    ed_sample_clear_route(p, -1, channel);
    clear_channel_events(p, channel, true);
    p->channels[channel] = dm_channel_default(channel);
    p->lane_count[channel] = 0;
    memset(p->lane_names[channel], 0, sizeof(p->lane_names[channel]));
    e->note_ports[channel] = 0;
    e->channel_expanded[channel] = false;
    e->song_event = e->midi_events = false;
    e->midi_event_index = -1;

    if (e->lane_settings && e->lane_channel == channel) {
        e->lane_settings = false;
    }

    if (e->selected_channel == channel) {
        int channels[16];
        e->selected_channel = ed_channels(e, channels) ? channels[0] : 0;
        e->channel_selected = false;
    }

    if (e->pattern_channel == channel || e->preview_source_channel == channel) {
        ed_bind_pattern_channel(e);
    }

    ed_commit(e);
    return true;
}

void ed_apply_instrument(Editor *e) {
    int c = e->instrument_channel;

    if (c < 0 || c >= 16 || e->project.channels[c].enabled == e->instrument_new) {
        return;
    }

    if (e->instrument_remove) {
        ed_close_instrument(e);
        ed_remove_channel(e, c);
        return;
    }

    bool create = e->instrument_new, drums = e->instrument_drums, preview = e->instrument_preview;
    int program = e->instrument_program;
    int port = e->instrument_port;
    bool type_changed = e->project.channels[c].drums != drums;
    bool assign = e->instrument_source_dirty, inherit = e->instrument_source_inherit;
    DmPatchReplacement source = e->instrument_source;
    DmPortMix mix = dm_port_mix(&e->project, port, c);
    int original = mix.program && port ? mix.program - 1 : e->project.channels[c].program;
    bool midi_change = create || type_changed || program != (int)fmax(0, original);

    if (!assign && !port) {
        midi_change |= dm_set_initial_program(&e->project, c, program, false);
    }

    if (port && midi_change &&
        !dm_reserve((void **)&e->project.port_mix, &e->project.port_mix_capacity,
                    e->project.port_mix_count + 1, sizeof(*e->project.port_mix))) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_PORT_MIXER);
        return;
    }

    ed_close_instrument(e);

    if (midi_change || assign) {
        bool pushed = ed_checkpoint(e);

        if (assign && !ed_sample_store_replacement(e, &source, inherit)) {
            ed_checkpoint_abandon(e, pushed);
            return;
        }

        if (midi_change) {
            e->project.channels[c].drums = drums;

            if (type_changed && !create) {
                clear_channel_events(&e->project, c, false);
                e->song_event = e->midi_events = false;
                e->midi_event_index = -1;
            }

            if (port) {
                mix.program = program + 1;

                if (!dm_port_mix_set(&e->project, mix)) {
                    /* Undo the type and event changes already made for this channel. */
                    ed_checkpoint_abandon(e, pushed);
                    ed_message(e, DM_TEXT_CANNOT_ALLOCATE_PORT_MIXER);
                    return;
                }
            } else {
                dm_set_initial_program(&e->project, c, program, true);
            }
        }

        if (type_changed && dm_audio_playing(e->audio)) {
            /* Replay channel setup at the current position so the new bank takes effect. */
            ed_changed(e);
            ed_transport_publish(e, dm_audio_position(e->audio), true);
        } else {
            ed_commit(e);

            if (midi_change && !port) {
                dm_audio_program(e->audio, c, program);
            }
        }
    }

    if (create) {
        e->reset_channels[c] = e->project.channels[c];
        e->selected_channel = c;
        e->channel_selected = true;
    }

    if (preview) {
        ed_preview_channel(e, c, true);
    }
}

static void instrument_fields(Editor *e) {
    char text[128];
    int c = e->instrument_channel;
    float width = ui_available().x, spacing = ui_item_spacing_x();
    float channel_width = fminf(width * .5f, 150);
    snprintf(text, sizeof(text), DM_TEXT_CHANNEL, c + 1);
    ui_width(channel_width);
    ui_disabled(!e->instrument_new);

    if (ui_edit_combo(DM_TEXT_CHANNEL_SELECT, text)) {
        for (int i = 0; i < 16; ++i) {
            snprintf(text, sizeof(text), DM_TEXT_CHANNEL, i + 1);
            ui_disabled(e->project.channels[i].enabled);

            if (ui_select(text, c == i, 0, 0) && i != c) {
                stop_instrument_preview(e);
                e->instrument_channel = i;
                instrument_route(e, 0);
            }

            ui_disabled_end();
        }

        ui_combo_end();
    }

    ui_disabled_end();
    ui_same();
    bool drums = ui_button_pair(DM_TEXT_MELODY, DM_TEXT_DRUMS, e->instrument_drums,
                                width - channel_width - spacing);

    if (drums != e->instrument_drums) {
        select_instrument(e, 0, drums);
    }

    int program = e->instrument_program;
    const char *name = drums ? dm_drum_kit_name(program) : dm_program_name(program);
    snprintf(text, sizeof(text), DM_TEXT_PROGRAM_NUMBERED, name ? name : DM_TEXT_DRUM_KIT, program);
    ui_width(ui_available().x);

    if (ui_combo(DM_TEXT_INSTRUMENT_SELECT, text)) {
        for (int i = 0; i < 128; ++i) {
            name = drums ? dm_drum_kit_name(i) : dm_program_name(i);

            if (!name) {
                continue;
            }

            snprintf(text, sizeof(text), DM_TEXT_PROGRAM_NUMBERED, name, i);

            if (ui_select(text, program == i, 0, 0)) {
                select_instrument(e, i, drums);
            }
        }

        ui_combo_end();
    }

    const bool *used = ed_channel_ports(e, c); /* Cached until the project changes. */
    bool colored = false;

    for (int port = 1; port < DM_NOTE_PORTS; ++port) {
        colored |= used[port];
    }

    if (!e->instrument_new && colored) {
        snprintf(text, sizeof(text), DM_TEXT_PORT, e->instrument_port);

        if (ui_combo(DM_TEXT_SAMPLE_ROUTE, text)) {
            for (int port = 0; port < DM_NOTE_PORTS; ++port) {
                if (!port || used[port]) {
                    snprintf(text, sizeof(text), DM_TEXT_PORT, port);

                    if (ui_select(text, e->instrument_port == port, 0, 0)) {
                        stop_instrument_preview(e);
                        instrument_route(e, port);
                    }
                }
            }

            ui_combo_end();
        }
    }

    /* The bank is not editable here; it comes from the song's opening Bank Select
       (ed_initial_bank) so sources stay keyed to the bank the channel actually plays. */
    DmPatchKey key = {(uint16_t)e->instrument_bank, (uint8_t)e->instrument_port, (uint8_t)c,
                      (uint8_t)e->instrument_program, e->instrument_drums};
    const DmPatchReplacement *current =
        e->instrument_source_dirty ? (e->instrument_source_inherit ? NULL : &e->instrument_source)
                                   : dm_patch_replacement_exact(&e->project, key);
    DmPatchReplacement next = {.key = key};
    bool inherit = false;

    if (ed_sample_source_picker(e, key, current, &next, &inherit)) {
        e->instrument_source = next;
        e->instrument_source_dirty = true;
        e->instrument_source_inherit = inherit;
        DmPatchKey inherited = key;
        inherited.port = 0;
        const DmPatchReplacement *preview =
            !inherit ? &e->instrument_source : dm_patch_replacement(&e->project, inherited);
        DmPatchReplacement stock = {.key = key};
        preview_sample_source(e, preview ? preview : &stock);
    }

    if (e->instrument_source_dirty) {
        current = inherit || e->instrument_source_inherit ? NULL : &e->instrument_source;
    }

    if (!current && key.port) {
        key.port = 0;
        current = dm_patch_replacement_exact(&e->project, key);
    }

    if (current && current->generation && ui_button(DM_TEXT_SAMPLE_PREVIEW, 0, 0)) {
        preview_sample_source(e, current);
    }
}

void ed_instrument(Editor *e) {
    if (e->instrument_channel < 0) {
        return;
    }

    if (e->instrument_raise) {
        ui_raise_next();
        e->instrument_raise = false;
    }

    bool open = true;

    if (ui_fitted_dialog(DM_TEXT_INSTRUMENT, &open, 460)) {
        if (e->instrument_remove) {
            char text[160];
            snprintf(text, sizeof(text), DM_TEXT_REMOVE_CHANNEL_WARNING, e->instrument_channel + 1);
            ui_wrap(text);
        } else {
            instrument_fields(e);
        }

        float half = (ui_available().x - ui_item_spacing_x()) * .5f;
        bool removing = e->instrument_remove;

        if (ui_button(DM_TEXT_CANCEL, half, 0)) {
            if (removing) {
                e->instrument_remove = false;
            } else {
                open = false;
            }
        }

        ui_same();
        bool unavailable = e->project.channels[e->instrument_channel].enabled == e->instrument_new;
        ui_disabled(unavailable);

        if (ui_button(removing ? DM_TEXT_REMOVE_CHANNEL : DM_TEXT_APPLY, half, 0)) {
            ed_apply_instrument(e);
        }

        ui_disabled_end();

        if (e->instrument_channel >= 0 && !unavailable && !removing && !e->instrument_new &&
            ui_button(DM_TEXT_REMOVE_CHANNEL, ui_available().x, 0)) {
            e->instrument_remove = channel_has_content(e, e->instrument_channel);

            if (!e->instrument_remove) {
                int c = e->instrument_channel;
                ed_close_instrument(e);
                ed_remove_channel(e, c);
            }
        }
    }

    ui_end();

    if (!open) {
        ed_close_instrument(e);
    }
}

static void midi_preferences(Editor *e) {
    float retry_width = ui_button_row_width((const char *const[]){DM_TEXT_RETRY_AUDIO_DEVICE}, 1);
    float combo_width = ui_available().x - retry_width - ui_text_width(DM_TEXT_MIDI_OUTPUT) -
                        3 * ui_item_spacing_x();
    ui_width(fmaxf(90, combo_width));

    if (ui_combo(DM_TEXT_MIDI_OUTPUT,
                 e->device < 0 ? DM_TEXT_SOUNDFONT_ONLY : DM_TEXT_EXTERNAL_MIDI)) {
        if (ui_select(DM_TEXT_SOUNDFONT_ONLY, e->device < 0, 0, 0) &&
            dm_audio_device(e->audio, -1, e->status, sizeof(e->status))) {
            e->device = -1;
            e->output_name[0] = 0;
            e->monitor = true;
            dm_audio_monitor(e->audio, true);
            ed_preference_save(e);
            ed_publish(e);
        }

        int count = dm_audio_device_count(e->audio);

        for (int i = 0; i < count; ++i) {
            char device[256];
            dm_audio_device_name(e->audio, i, device, sizeof(device));

            if (ui_select(device, e->device == i, 0, 0) &&
                dm_audio_device(e->audio, i, e->status, sizeof(e->status))) {
                e->device = i;
                snprintf(e->output_name, sizeof(e->output_name), "%s", device);
                e->monitor = false;
                dm_audio_monitor(e->audio, false);
                ed_preference_save(e);
                ed_publish(e);
            }
        }

        ui_combo_end();
    }

    ui_same();

    if (ui_button(DM_TEXT_RETRY_AUDIO_DEVICE, retry_width, 0)) {
        if (!e->audio) {
            e->audio = dm_audio_create(e->status, sizeof(e->status));
            dm_audio_attach_midi_input(e->audio, e->midi_input);
            ed_midi_input_apply(e);
        } else {
            dm_audio_retry(e->audio, e->status, sizeof(e->status));
        }

        ed_publish(e);
    }
}

static void midi_input_device(Editor *e, const char *name, const DmMidiInputStatus *status) {
    static const char *const states[] = {
        [DM_MIDI_CLOSED] = DM_TEXT_MIDI_INPUT_CLOSED,
        [DM_MIDI_OPENING] = DM_TEXT_MIDI_INPUT_OPENING,
        [DM_MIDI_OPEN] = DM_TEXT_MIDI_INPUT_OPEN,
        [DM_MIDI_MISSING] = DM_TEXT_MIDI_INPUT_ABSENT,
        [DM_MIDI_FAILED] = DM_TEXT_MIDI_INPUT_STATE_FAILED,
    };
    bool requested = status && status->requested;
    ui_id((int)SDL_crc32(0, name, strlen(name)));

    if (ui_check("##midi-input-device", &requested)) {
        bool ok = true;

        if (requested) {
            ok = dm_midi_input_open(e->midi_input, name);
        } else {
            dm_midi_input_close(e->midi_input, name);
        }

        if (ok) {
            ed_midi_input_names(e);
            ed_preference_save(e);
        } else {
            ed_message(e, DM_TEXT_MIDI_INPUT_DEVICE_LIMIT);
        }
    }

    ui_same();
    ui_align_frame();
    ui_text(name);
    ui_same();
    ui_dim(states[status ? status->state : DM_MIDI_CLOSED]);

    if (status && *status->message && status->state == DM_MIDI_FAILED) {
        ui_wrap(status->message);
    }

    ui_pop_id();
}

static void midi_input_devices(Editor *e, bool refresh) {
    ui_align_frame();
    ui_text(DM_TEXT_MIDI_INPUT_DEVICES);
    ui_same();
    ui_disabled(!e->midi_input);
    refresh |= ui_button(DM_TEXT_MIDI_INPUT_REFRESH, 0, 0);
    ui_disabled_end();

    if (!e->midi_input) {
        ui_dim(DM_TEXT_MIDI_INPUT_FAILED);
        return;
    }

    if (refresh) {
        dm_midi_input_refresh(e->midi_input);
    }

    char names[DM_MIDI_PORTS][DM_MIDI_NAME];
    int count = dm_midi_input_ports(e->midi_input, names, DM_MIDI_PORTS);
    DmMidiInputStatus status[DM_MIDI_INPUTS];
    dm_midi_input_status(e->midi_input, status);
    bool listed[DM_MIDI_INPUTS] = {0};
    bool any = count > 0;

    for (int i = 0; i < count; ++i) {
        const DmMidiInputStatus *port = NULL;

        for (unsigned j = 0; j < DM_MIDI_INPUTS; ++j) {
            if (!strcmp(names[i], status[j].name)) {
                port = &status[j];
                listed[j] = true;
                break;
            }
        }

        midi_input_device(e, names[i], port);
    }

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        if (status[i].requested && !listed[i]) {
            any = true;
            midi_input_device(e, status[i].name, &status[i]);
        }
    }

    if (!any) {
        ui_dim(DM_TEXT_MIDI_INPUT_NO_DEVICES);
    }
}

static bool midi_input_velocity(DmMidiInputOptions *options) {
    static const char *const curves[] = {
        [DM_MIDI_VELOCITY_LIGHT] = DM_TEXT_MIDI_INPUT_LIGHT,
        [DM_MIDI_VELOCITY_MEDIUM] = DM_TEXT_MIDI_INPUT_MEDIUM,
        [DM_MIDI_VELOCITY_HEAVY] = DM_TEXT_MIDI_INPUT_HEAVY,
    };
    static const DmMidiVelocityCurve order[] = {DM_MIDI_VELOCITY_LIGHT, DM_MIDI_VELOCITY_MEDIUM,
                                                DM_MIDI_VELOCITY_HEAVY};
    bool changed = false;
    ui_width(165);

    if (ui_combo(DM_TEXT_MIDI_INPUT_VELOCITY, curves[options->velocity])) {
        for (unsigned i = 0; i < SDL_arraysize(order); ++i) {
            DmMidiVelocityCurve curve = order[i];

            if (ui_select(curves[curve], options->velocity == curve, 0, 0)) {
                options->velocity = curve;
                changed = true;
            }
        }

        ui_combo_end();
    }

    bool fixed = options->fixed_velocity != 0;
    int velocity = fixed ? options->fixed_velocity : 100;
    changed |= ui_check(DM_TEXT_MIDI_INPUT_FIXED, &fixed);
    ui_same();
    ui_disabled(!fixed);
    changed |= ui_int("##midi-input-velocity", &velocity, 1, MIDI_DATA_MAX);
    ui_disabled_end();
    options->fixed_velocity = fixed ? (uint8_t)velocity : 0;
    return changed;
}

static bool midi_input_routing(Editor *e) {
    bool changed = ui_check(DM_TEXT_MIDI_INPUT_THRU, &e->input_thru);
    static const char *const policies[] = {DM_TEXT_MIDI_INPUT_KEYBOARD, DM_TEXT_SONG};
    ui_width(165);

    if (ui_combo(DM_TEXT_MIDI_INPUT_CONTROLLERS, policies[e->input_controllers])) {
        for (unsigned i = 0; i < SDL_arraysize(policies); ++i) {
            if (ui_select(policies[i], e->input_controllers == i, 0, 0)) {
                e->input_controllers = i;
                changed = true;
            }
        }

        ui_combo_end();
    }

    bool automatic = isnan(e->input_latency_ms);
    ui_width(165);

    if (ui_combo(DM_TEXT_MIDI_INPUT_LATENCY,
                 automatic ? DM_TEXT_MIDI_INPUT_AUTO : DM_TEXT_MIDI_INPUT_MS)) {
        if (ui_select(DM_TEXT_MIDI_INPUT_AUTO, automatic, 0, 0) && !automatic) {
            e->input_latency_ms = NAN;
            changed = true;
        }

        if (ui_select(DM_TEXT_MIDI_INPUT_MS, !automatic, 0, 0) && automatic) {
            e->input_latency_ms =
                (float)fmin(dm_audio_buffer_ms(e->audio), DM_MIDI_LATENCY_LIMIT_MS);
            changed = true;
        }

        ui_combo_end();
    }

    if (!isnan(e->input_latency_ms)) {
        ui_same();
        changed |= ui_float("##midi-input-ms", &e->input_latency_ms, -DM_MIDI_LATENCY_LIMIT_MS,
                            DM_MIDI_LATENCY_LIMIT_MS);
    }

    changed |= ui_check(DM_TEXT_MIDI_INPUT_INVERT, &e->input_options.sustain_invert);
    char channel[16];
    snprintf(channel, sizeof(channel), "%u", (unsigned)e->input_options.channel);
    ui_width(165);

    if (ui_combo(DM_TEXT_MIDI_INPUT_CHANNEL,
                 e->input_options.channel ? channel : DM_TEXT_MIDI_INPUT_ALL)) {
        if (ui_select(DM_TEXT_MIDI_INPUT_ALL, !e->input_options.channel, 0, 0)) {
            e->input_options.channel = 0;
            changed = true;
        }

        for (unsigned i = 1; i <= MIDI_CHANNELS; ++i) {
            snprintf(channel, sizeof(channel), "%u", i);

            if (ui_select(channel, e->input_options.channel == i, 0, 0)) {
                e->input_options.channel = (uint8_t)i;
                changed = true;
            }
        }

        ui_combo_end();
    }

    if (ui_check(DM_TEXT_MIDI_INPUT_PICKUP, &e->input_pickup)) {
        ed_midi_bindings_reset_pickup(e);
        changed = true;
    }

    return changed;
}

static void midi_input_activity(Editor *e) {
    char name[DM_MIDI_NAME], detail[64], line[DM_MIDI_NAME + 96];
    DmMidiInputEvent event;

    if (!ed_midi_input_activity(e, &event, name)) {
        ui_dim(DM_TEXT_MIDI_INPUT_NO_ACTIVITY);
        return;
    }

    switch (event.status & MIDI_STATUS_MASK) {
    case MIDI_NOTE_ON:
    case MIDI_NOTE_OFF:
        snprintf(detail, sizeof(detail), DM_TEXT_MIDI_INPUT_NUMBER_VALUE,
                 (event.status & MIDI_STATUS_MASK) == MIDI_NOTE_ON ? DM_TEXT_NOTE_ATTACK
                                                                   : DM_TEXT_NOTE_RELEASE,
                 event.a, event.b);
        break;
    case MIDI_CONTROL:
        snprintf(detail, sizeof(detail), DM_TEXT_MIDI_INPUT_CC_VALUE, event.a, event.b);
        break;
    case MIDI_PITCH_BEND:
        snprintf(detail, sizeof(detail), DM_TEXT_MIDI_INPUT_VALUE, DM_TEXT_PITCH_BEND,
                 (event.a | (event.b << 7)) - MIDI_BEND_CENTER);
        break;
    case MIDI_KEY_PRESSURE:
        snprintf(detail, sizeof(detail), DM_TEXT_MIDI_INPUT_NUMBER_VALUE, DM_TEXT_KEY_PRESSURE,
                 event.a, event.b);
        break;
    default:
        snprintf(detail, sizeof(detail), DM_TEXT_MIDI_INPUT_VALUE, DM_TEXT_CHANNEL_PRESSURE,
                 event.a);
        break;
    }

    snprintf(line, sizeof(line), DM_TEXT_MIDI_INPUT_ACTIVITY_VALUE, name,
             (event.status & MIDI_CHANNEL_MASK) + 1, detail);

    if (SDL_GetTicksNS() >= event.ticks_ns + 2000000000ULL) {
        ui_dim(line);
    } else {
        ui_text(line);
    }
}

static void midi_binding_mode(Editor *e, EdMidiBinding *binding) {
    bool momentary = !binding || !binding->toggle;
    ui_width(110);
    ui_disabled(!binding);

    if (ui_combo("##midi-binding-mode",
                 momentary ? DM_TEXT_MIDI_BIND_MOMENTARY : DM_TEXT_MIDI_BIND_TOGGLE)) {
        if (ui_select(DM_TEXT_MIDI_BIND_MOMENTARY, momentary, 0, 0) && !momentary) {
            binding->toggle = false;
            ed_preference_save(e);
        }

        if (ui_select(DM_TEXT_MIDI_BIND_TOGGLE, !momentary, 0, 0) && momentary) {
            binding->toggle = true;
            ed_preference_save(e);
        }

        ui_combo_end();
    }

    ui_disabled_end();
}

static bool midi_binding_row(Editor *e, EdMidiTarget target, EdMidiBinding *binding, bool action) {
    char id[48], label[64], source[DM_MIDI_NAME + 50];
    ed_midi_target_id(target, id, sizeof(id));
    ed_midi_target_label(target, label, sizeof(label));
    ui_id((int)SDL_crc32(0, id, strlen(id)));
    UiPoint at = ui_cursor();
    float controls = 260 + 3 * ui_item_spacing_x();
    ui_align_frame();
    ui_text(label);

    if (binding) {
        ed_midi_source_label(&binding->source, source, sizeof(source));
        ui_tip(source);
    }

    ui_same();
    ui_position(at.x + fmaxf(130, ui_available().x + ui_cursor().x - at.x - controls), at.y);

    if (action) {
        ui_disabled(!e->midi_input);
        bool learning = ed_midi_binding_learning(e, target);

        if (ui_button(learning ? DM_TEXT_MIDI_BIND_LISTENING : DM_TEXT_MIDI_BIND_LEARN_SHORT, 90,
                      0)) {
            if (learning) {
                ed_midi_bindings_cancel(e);
            } else {
                ed_midi_binding_learn(e, target, e->midi_learn_any);
            }
        }

        ui_disabled_end();
        ui_same();
    }

    if (!ed_midi_target_continuous(target)) {
        midi_binding_mode(e, binding);
        ui_same();
    }

    ui_disabled(!binding);
    bool clear = ui_button(DM_TEXT_CLEAR, 60, 0);
    ui_disabled_end();
    ui_pop_id();

    if (clear) {
        ed_midi_binding_clear(e, target);
    }

    return clear;
}

static void midi_controls(Editor *e) {
    if (!ui_section(DM_TEXT_MIDI_BIND_CONTROLS)) {
        return;
    }

    ui_check(DM_TEXT_MIDI_BIND_ANY_CHECK, &e->midi_learn_any);

    if (e->midi_learn.started) {
        ui_dim(DM_TEXT_MIDI_BIND_WAIT);
    }

    for (unsigned kind = ED_MIDI_PLAY; kind < ED_MIDI_TARGETS; ++kind) {
        EdMidiTarget target = {.kind = kind};
        midi_binding_row(e, target, ed_midi_binding_find(e, target), true);
    }

    for (size_t i = 0; i < ed_midi_binding_count(e); ++i) {
        EdMidiBinding *binding = ed_midi_binding_at(e, i);

        if (binding->target.kind < ED_MIDI_PLAY &&
            midi_binding_row(e, binding->target, binding, false)) {
            break;
        }
    }

    ui_disabled(!ed_midi_binding_count(e));

    if (ui_button(DM_TEXT_MIDI_BIND_CLEAR_ALL, 0, 0)) {
        e->midi_clear_bindings = true;
    }

    ui_disabled_end();
}

static void midi_clear_confirmation(Editor *e) {
    if (!ui_modal(DM_TEXT_MIDI_BIND_CLEAR_TITLE, &e->midi_clear_bindings, 390, 135)) {
        return;
    }

    ui_wrap(DM_TEXT_MIDI_BIND_CLEAR_QUESTION);

    if (ui_button(DM_TEXT_CANCEL, 110, 0)) {
        e->midi_clear_bindings = false;
        ui_close_popup();
    }

    ui_same();

    if (ui_button(DM_TEXT_MIDI_BIND_CLEAR_ALL, 110, 0)) {
        ed_midi_bindings_clear_all(e);
        e->midi_clear_bindings = false;
        ui_close_popup();
    }

    ui_popup_end();
}

static void midi_input_preferences(Editor *e) {
    bool was_open = e->input_preferences_open;
    e->input_preferences_open = ui_section(DM_TEXT_MIDI_INPUT);

    if (!e->input_preferences_open) {
        return;
    }

    midi_input_devices(e, !was_open);
    ui_separator();
    bool changed = midi_input_velocity(&e->input_options);
    changed |= midi_input_routing(e);

    if (changed) {
        ed_midi_input_apply(e);
        ed_preference_save(e);
    }

    ui_separator();
    ui_text(DM_TEXT_MIDI_INPUT_ACTIVITY);
    midi_input_activity(e);
    midi_controls(e);
}

/* Every exporter with Export to Repository gets a folder row; the picker sets it too. */
static void export_plugin_preferences(Editor *e) {
    if (!ui_section(DM_TEXT_PLUGINS)) {
        return;
    }

    bool any = false;

    for (size_t i = 0; i < ed_export_count(e); i++) {
        if (!ed_export_installs(e, i)) {
            continue;
        }

        char label[256], folder[DM_PATH];
        snprintf(label, sizeof(label), DM_TEXT_REPOSITORY_FOLDER, ed_export_name(e, i));
        snprintf(folder, sizeof(folder), "%s", ed_export_repository(e, i));
        any = true;
        ui_id((int)i);
        ui_text(label);
        float buttons = 2 * 80 + 2 * ui_item_spacing_x();
        ui_width(fmaxf(120, ui_available().x - buttons));

        if (ui_input("##repository", folder, sizeof(folder))) {
            ed_export_set_repository(e, i, folder);
        }

        ui_tip(DM_TEXT_REPOSITORY_FOLDER_HELP);
        ui_same();
        ui_disabled(e->dialog_busy);

        if (ui_button(DM_TEXT_BROWSE, 80, 0) && ed_export_folder_picker(e, i)) {
            ed_dialog(e, DIALOG_PLUGIN_FOLDER);
        }

        ui_disabled_end();
        ui_same();

        if (ui_button(DM_TEXT_CLEAR, 80, 0)) {
            ed_export_set_repository(e, i, "");
        }

        ui_pop_id();
    }

    if (!any) {
        ui_dim(DM_TEXT_NO_REPOSITORY_EXPORTERS);
    }
}

void ed_preferences(Editor *e) {
    if (!e->preferences) {
        e->input_preferences_open = false;
        return;
    }

    if (e->preferences_raise) {
        ui_raise_next();
        e->preferences_raise = false;
    }

    bool show = ui_growing_dialog(DM_TEXT_PREFERENCES, &e->preferences, 730, 540);
    e->preferences_rect = ui_window_rect();

    if (show) {
        if (ui_color(DM_TEXT_APPLICATION_COLOR, &e->accent)) {
            ed_preference_save(e);
        }

        if (ui_color(DM_TEXT_BACKGROUND_COLOR, &e->background)) {
            ed_preference_save(e);
        }

        if (ui_check(DM_TEXT_USE_DEFAULT_OS, &e->os_cursor)) {
            ui_custom_cursor(!e->os_cursor);
            ed_preference_save(e);
        }

        ui_separator();
        ui_text(DM_TEXT_SOUND_OUTPUT);
        ui_separator();
        midi_preferences(e);

        if (e->device >= 0 && ui_check(DM_TEXT_ALSO_MONITOR_SOUNDFONT, &e->monitor)) {
            dm_audio_monitor(e->audio, e->monitor);
            ed_preference_save(e);
            ed_publish(e);
        }

        /* An external MIDI output does not use the SoundFont choice. */
        if (e->device < 0) {
            ui_text(DM_TEXT_SOUNDFONT_LABEL);
            ui_width(fmaxf(120, ui_available().x - 80 - ui_item_spacing_x()));
            ui_readonly("##soundfont", *e->project.soundfont ? e->project.soundfont
                                                             : DM_TEXT_SOUNDFONT_SELECTED_CHOOSE);
            ui_same();
            ui_disabled(e->dialog_busy);

            if (ui_button(DM_TEXT_BROWSE, 80, 0)) {
                ed_dialog(e, DIALOG_FONT);
            }

            ui_disabled_end();
        }

        if (dm_audio_busy(e->audio)) {
            ui_dim(DM_TEXT_BACKGROUND_OPERATION_PROGRESS);

            if (ui_button(DM_TEXT_CANCEL_OPERATION, 140, 0)) {
                dm_audio_cancel(e->audio);
            }
        }

        midi_input_preferences(e);
        midi_clear_confirmation(e);
        export_plugin_preferences(e);
        ui_separator();

        if (ui_button(DM_TEXT_CLOSE, ui_available().x, 0)) {
            e->preferences = false;
        }
    }

    if (!show || !e->preferences) {
        e->input_preferences_open = false;
    }

    ui_end();
}

static void shortcut_row(Editor *e, int i) {
    ui_id(i);

    /* Action names are also preference keys; these labels changed after release. */
    ui_text(i == ACT_NOTE_COLOR            ? DM_TEXT_COLOR_NOTES_PATTERNS
            : i == ACT_SONG_EVENT          ? DM_TEXT_EVENTS_SHORTCUT
            : i == ACT_NEXT                ? DM_TEXT_CREATE_PATTERN
            : i == ACT_PREVIOUS            ? DM_TEXT_REMOVE_EMPTY_PATTERN
            : i == ACT_NEXT_AUTOMATION     ? DM_TEXT_CREATE_AUTOMATION
            : i == ACT_PREVIOUS_AUTOMATION ? DM_TEXT_REMOVE_EMPTY_AUTOMATION
                                           : ed_action_names[i]);
    ui_same();
    UiPoint cursor = ui_cursor(), window = ui_window();
    ui_position(window.x + 360, cursor.y);
    char label[128];
    ed_binding_text(e, (Action)i, label, sizeof(label));

    if (ui_button(e->capture == i ? DM_TEXT_PRESS_KEYS : label, 190, 0)) {
        e->gesture_capture = -1;
        e->capture = i;
    }

    ui_tip(label);
    ui_pop_id();
}

static void gesture_row(Editor *e, int i) {
    ui_id(ACT_COUNT + i);
    ui_text(i == GESTURE_COPY_PLACEMENTS ? DM_TEXT_COPY_PLACEMENTS_DRAG : DM_TEXT_COPY_NOTES_DRAG);
    ui_same();
    UiPoint cursor = ui_cursor(), window = ui_window();
    ui_position(window.x + 360, cursor.y);
    char label[128] = "";
    ed_gesture_format(e->gestures[i], label, sizeof(label));

    if (ui_button(e->gesture_capture == i ? DM_TEXT_PRESS_MOUSE : label, 190, 0)) {
        e->capture = -1;
        e->gesture_capture = i;
        e->gesture_capture_armed = false;
    }

    ui_tip(label);
    ui_pop_id();
}

void ed_shortcuts(Editor *e) {
    if (!e->shortcuts) {
        return;
    }

    bool show = ui_dialog(DM_TEXT_KEYBOARD_SHORTCUTS, &e->shortcuts, 640, 620);

    if (show) {
        ui_align_frame();
        ui_text(e->capture >= 0 ? DM_TEXT_PRESS_NEW_KEY : DM_TEXT_CLICK_BINDING_CHANGE);
        ui_same();

        if (ui_button(DM_TEXT_RESET_DEFAULTS, 130, 0)) {
            ed_defaults(e);
            ed_preference_save(e);
        }

        ui_child(DM_TEXT_SHORTCUT_LIST, 0, 0, true);

        for (int i = 0; i < ACT_COUNT; ++i) {
            static const char *categories[ACT_COUNT] = {[0] = DM_TEXT_TRANSPORT_INPUT,
                                                        [3] = DM_TEXT_NAVIGATION,
                                                        [11] = DM_TEXT_PATTERNS_SELECTION,
                                                        [19] = DM_TEXT_ZOOM_KEYBOARD_PIANO,
                                                        [22] = DM_TEXT_WINDOWS_TOOLS_KEYBOARD};

            if (categories[i]) {
                ui_separator();
                ui_dim(categories[i]);
            }

            shortcut_row(e, i);
        }

        ui_separator();
        ui_dim(DM_TEXT_MOUSE_GESTURES);
        ed_gesture_capture(e, ui_mouse());

        for (int i = 0; i < GESTURE_COUNT; ++i) {
            gesture_row(e, i);
        }

        ui_separator();
        ui_dim(DM_TEXT_FIXED_SHORTCUTS);
        ui_wrap(DM_TEXT_ESCAPE_CLOSE_UNFOCUS_ENTER);
        ui_wrap(DM_TEXT_CTRL_SELECT_ALL_DELETE);
        ui_wrap(DM_TEXT_CTRL_SAVE_CTRL_SHIFT);
        ui_wrap(DM_TEXT_CTRL_Z_Y_UNDO);
        ui_wrap(DM_TEXT_ALT_DISABLE_SNAP_RIGHT);
        ui_wrap(DM_TEXT_WHEEL_HORIZONTAL_SHIFT_WHEEL);
        ui_child_end();
    }

    ui_end();

    if (!e->shortcuts) {
        e->capture = -1;
        e->gesture_capture = -1;
    }
}
