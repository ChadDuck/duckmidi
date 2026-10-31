#include "editor_internal.h"
#include "text/en.h"

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

void ed_choose_program(Editor *e, int channel, int program) {
    if (channel < 0 || channel >= 16 || program < 0 || program > 127) {
        return;
    }

    if (dm_set_initial_program(&e->project, channel, program, false)) {
        ed_checkpoint(e);
        dm_set_initial_program(&e->project, channel, program, true);
        ed_commit(e);
    }

    if (e->preview_route == channel) {
        dm_audio_keyboard_program(e->audio, channel, -1);
        e->preview_route = -1;
    }

    dm_audio_preview(e->audio, channel, 60, 0, false);
    dm_audio_program(e->audio, channel, program);
    dm_audio_audition(e->audio, channel, 60, 100, .3);
}

bool ed_remove_empty_channel(Editor *e, int channel) {
    if (!e->project.channels[channel].enabled || ed_channel_has_clips(e, channel)) {
        return false;
    }

    ed_checkpoint(e);
    ed_release_piano(e);
    e->project.channels[channel] = (DmChannel){false, false, false, -1, 100, 64};
    e->channel_expanded[channel] = false;

    if (e->selected_channel == channel) {
        e->channel_selected = false;
    }

    ed_commit(e);
    return true;
}

/* Moves a channel's placements, settings and events to another, unused channel. */
static bool move_channel(Editor *e, int from, int to) {
    DmProject *p = &e->project;

    if (from == to || ed_channel_has_clips(e, to)) {
        return false;
    }

    ed_checkpoint(e);
    ed_release_piano(e);
    p->channels[to] = p->channels[from];
    p->channels[from] = (DmChannel){false, false, false, -1, 100, 64};
    p->lane_count[to] = p->lane_count[from];
    p->lane_count[from] = 0;
    memcpy(p->lane_names[to], p->lane_names[from], sizeof(p->lane_names[to]));
    memset(p->lane_names[from], 0, sizeof(p->lane_names[from]));
    e->channel_expanded[to] = e->channel_expanded[from];
    e->channel_expanded[from] = false;

    for (size_t i = 0; i < p->clip_count; ++i) {
        if (p->clips[i].channel == from) {
            p->clips[i].channel = to;
        }
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        if (p->automation[i].pattern < 0 && p->automation[i].channel == from) {
            p->automation[i].channel = to;
        }
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        DmBytes *m = &p->events[i].message;

        if (m->size && m->data[0] >= MIDI_STATUS && m->data[0] < MIDI_SYSTEM &&
            (m->data[0] & MIDI_CHANNEL_MASK) == from) {
            m->data[0] = (uint8_t)((m->data[0] & MIDI_STATUS_MASK) | to);
        }
    }

    for (size_t i = 0; i < p->port_mix_count; ++i) {
        if (p->port_mix[i].channel == from) {
            p->port_mix[i].channel = to;
        }
    }

    e->selected_channel = e->selected_channel == from ? to : e->selected_channel;
    e->pattern_channel = e->pattern_channel == from ? to : e->pattern_channel;
    ed_commit(e);
    return true;
}

void ed_choose_drums(Editor *e, int channel) {
    if (channel == DM_DRUM_CHANNEL) {
        return;
    }

    if (!move_channel(e, channel, DM_DRUM_CHANNEL)) {
        ed_message(e, DM_TEXT_DRUM_CHANNEL_USE);
        return;
    }

    ed_choose_program(e, DM_DRUM_CHANNEL, 0);
    e->instrument_channel = DM_DRUM_CHANNEL;
}

/* Moves the drum channel's part to a free melodic channel playing program. */
static void choose_melodic(Editor *e, int program) {
    int to = ed_available_channel(e);

    if (to < 0 || to == DM_DRUM_CHANNEL || !move_channel(e, DM_DRUM_CHANNEL, to)) {
        ed_message(e, DM_TEXT_NO_FREE_INSTRUMENT_CHANNEL);
        return;
    }

    e->project.channels[to].enabled = true;
    ed_choose_program(e, to, program);
    e->instrument_channel = to;
}

/* General MIDI plays drums on channel 10. Its programs select drum kits; other channels offer a
   Drum kit entry that moves them to channel 10. */
static void instrument_combo(Editor *e, int c) {
    char text[128];
    int program = e->project.channels[c].program;
    bool drums = c == DM_DRUM_CHANNEL;
    const char *kit = dm_drum_kit_name(program);

    if (drums) {
        snprintf(text, sizeof(text), DM_TEXT_PROGRAM_NUMBERED, kit ? kit : DM_TEXT_DRUM_KIT,
                 program);
    } else {
        snprintf(text, sizeof(text), DM_TEXT_PROGRAM_NUMBERED, dm_program_name(program), program);
    }

    if (!ui_combo(DM_TEXT_PROGRAM, text)) {
        return;
    }

    if (!drums && ui_select(DM_TEXT_DRUM_KIT, false, 0, 0)) {
        ed_choose_drums(e, c);
    }

    for (int i = 0; i < 128; ++i) {
        const char *name = drums ? dm_drum_kit_name(i) : dm_program_name(i);

        if (!name) {
            continue;
        }

        snprintf(text, sizeof(text), DM_TEXT_PROGRAM_NUMBERED, name, i);

        if (ui_select(text, program == i, 0, 0)) {
            ed_choose_program(e, c, i);
        }
    }

    /* The drum channel also lists instruments, which move its part to a free channel. */
    if (drums) {
        ui_separator();

        for (int i = 0; i < 128; ++i) {
            snprintf(text, sizeof(text), DM_TEXT_PROGRAM_NUMBERED, dm_program_name(i), i);
            ui_id(1000 + i);

            if (ui_select(text, false, 0, 0)) {
                choose_melodic(e, i);
            }

            ui_pop_id();
        }
    }

    ui_combo_end();
}

void ed_instrument(Editor *e) {
    if (e->instrument_channel < 0) {
        return;
    }

    bool open = true;
    bool show = ui_fitted_dialog(DM_TEXT_INSTRUMENT, &open, 460);
    int c = e->instrument_channel;

    if (show) {
        char text[128];
        snprintf(text, sizeof(text), DM_TEXT_CHANNEL_NUMBER, c + 1,
                 c == MIDI_DRUM_CHANNEL ? DM_TEXT_GM_PERCUSSION_SUFFIX : "");
        ui_text(text);
        ui_separator();
        instrument_combo(e, c);

        if (c == MIDI_DRUM_CHANNEL) {
            ui_dim(DM_TEXT_CHANNEL_10_USES);
        }

        if (e->project.channels[c].enabled && !ed_channel_has_clips(e, c) &&
            ui_button(DM_TEXT_REMOVE_EMPTY_CHANNEL, 0, 0) && ed_remove_empty_channel(e, c)) {
            open = false;
        }

        if (ui_button(DM_TEXT_APPLY, ui_available().x, 0) || e->instrument_accept) {
            open = false;
        }
    }

    ui_end();

    if (!open) {
        /* Opened with the instrument shortcut, the choice also becomes the pattern editor's
           instrument, which then follows the channel. */
        if (e->instrument_preview && e->project.channels[c].enabled) {
            ed_preview_channel(e, c, true);
        }

        e->instrument_accept = false;
        e->instrument_preview = false;
        e->instrument_channel = -1;
    }
}

static void midi_preferences(Editor *e) {
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
}

void ed_preferences(Editor *e) {
    if (!e->preferences) {
        return;
    }

    if (e->preferences_raise) {
        ui_raise_next();
        e->preferences_raise = false;
    }

    bool show = ui_dialog(DM_TEXT_PREFERENCES, &e->preferences, 630, 400);
    e->preferences_rect = ui_window_rect();

    if (show) {
        if (ui_color(DM_TEXT_APPLICATION_COLOR, &e->accent)) {
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

        if (ui_button(DM_TEXT_RETRY_AUDIO_DEVICE, 160, 0)) {
            if (!e->audio) {
                e->audio = dm_audio_create(e->status, sizeof(e->status));
            } else {
                dm_audio_retry(e->audio, e->status, sizeof(e->status));
            }

            ed_publish(e);
        }

        if (e->device >= 0 && ui_check(DM_TEXT_ALSO_MONITOR_SOUNDFONT, &e->monitor)) {
            dm_audio_monitor(e->audio, e->monitor);
            ed_preference_save(e);
            ed_publish(e);
        }

        /* An external MIDI output does not use the SoundFont choice. */
        if (e->device < 0) {
            if (ui_button(DM_TEXT_CHOOSE_SOUNDFONT, 170, 0)) {
                ed_dialog(e, DIALOG_FONT);
            }

            ui_text(*e->project.soundfont ? e->project.soundfont
                                          : DM_TEXT_SOUNDFONT_SELECTED_CHOOSE);
        }

        if (dm_audio_busy(e->audio)) {
            ui_dim(DM_TEXT_BACKGROUND_OPERATION_PROGRESS);

            if (ui_button(DM_TEXT_CANCEL_OPERATION, 140, 0)) {
                dm_audio_cancel(e->audio);
            }
        }

        ui_separator();

        if (ui_button(DM_TEXT_CLOSE, ui_available().x, 0)) {
            e->preferences = false;
        }
    }

    ui_end();
}

static void shortcut_row(Editor *e, int i) {
    ui_id(i);

    /* Action names are also preference keys; these labels changed after release. */
    ui_text(i == ACT_NOTE_COLOR   ? DM_TEXT_COLOR_NOTES_PATTERNS
            : i == ACT_SONG_EVENT ? DM_TEXT_EVENTS_SHORTCUT
                                  : ed_action_names[i]);
    ui_same();
    UiPoint cursor = ui_cursor(), window = ui_window();
    ui_position(window.x + 360, cursor.y);
    Binding b = e->bindings[i];
    char label[128];
    snprintf(label, sizeof(label), "%s%s%s%s%s", b.mods & SDL_KMOD_CTRL ? DM_TEXT_CTRL : "",
             b.mods & SDL_KMOD_SHIFT ? DM_TEXT_SHIFT : "", b.mods & SDL_KMOD_ALT ? DM_TEXT_ALT : "",
             b.mods & SDL_KMOD_GUI ? DM_TEXT_WIN_CMD : "", SDL_GetScancodeName(b.key));

    if (ui_button(e->capture == i ? DM_TEXT_PRESS_KEYS : label, 190, 0)) {
        e->capture = i;
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
    }
}
