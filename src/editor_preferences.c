#include "editor_internal.h"
#include "text/en.h"

static bool source_port_controls(Editor *e, const char *text, int *port);
static void source_settings(Editor *e);
static void project_fields(Editor *e);
static void new_project_fields(Editor *e);
static void midi_preferences(Editor *e);
static void shortcut_row(Editor *e, int i);

static bool source_port_controls(Editor *e, const char *text, int *port) {
    if (e->project.port != DM_PORT_COMBINED && ui_combo(DM_TEXT_SOURCE_PORT, text)) {
        for (int i = 0; i < e->project.port_count; ++i) {
            char s[32];
            snprintf(s, sizeof(s), DM_TEXT_PORT_D, e->project.ports[i]);
            if (ui_select(s, *port == e->project.ports[i], 0, 0)) {
                *port = e->project.ports[i];
            }
        }
        ui_combo_end();
    }
    if (e->project.port != DM_PORT_COMBINED && e->project.port_count > 1) {
        if (ui_button(DM_TEXT_COMBINE_SOURCE_PORTS, 190, 0)) {
            if (e->undo_count || e->project.view_count) {
                ed_request(e, PENDING_COMBINE, NULL);
            } else {
                ed_combine_source_ports(e);
            }
            return false;
        }
        ui_tip(DM_TEXT_IMPORT_THE_ORIGINAL_SOURCE_INTO_ONE_ARRANGEMENT_WHEN_PORTS_USE_DISTINCT_);
    } else if (e->project.port == DM_PORT_COMBINED) {
        ui_dim(DM_TEXT_ALL_SOURCE_CHANNELS_SHARE_ONE_OUTPUT);
        ui_dim(DM_TEXT_REOPEN_THE_SOURCE_TO_USE_INDIVIDUAL_PORTS);
    }
    return true;
}

static void source_settings(Editor *e) {
    if (e->project.original.size) {
        ui_separator();
        char text[128];
        if (e->project.port == DM_PORT_COMBINED) {
            snprintf(text, sizeof(text), DM_TEXT_COMBINED_D_SOURCE_PORTS_SEQUENCE_D_D,
                     e->project.port_count, e->project.sequence + 1, e->project.sequence_count);
        } else {
            snprintf(text, sizeof(text), DM_TEXT_PORT_D_SEQUENCE_D_D, e->project.port,
                     e->project.sequence + 1, e->project.sequence_count);
        }
        if (e->project.port == DM_PORT_COMBINED) {
            ui_text(text);
        }
        ui_dim(e->project.port == DM_PORT_COMBINED
                   ? DM_TEXT_EDITS_ARE_RETAINED_SEPARATELY_FOR_EACH_SEQUENCE
                   : DM_TEXT_EDITS_ARE_RETAINED_SEPARATELY_FOR_EACH_PORT_AND_SEQUENCE);
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
    bool show = ui_content_dialog(e->new_mode ? DM_TEXT_NEW_PROJECT : DM_TEXT_PROJECT_SETTINGS, &e->config, 570);
    if (show) {
        ui_text(e->new_mode ? DM_TEXT_START_SOMETHING_NEW : DM_TEXT_PROJECT_MIDI_IMPORT);
        ui_separator();
        if (e->new_mode) {
            new_project_fields(e);
            ui_dim(DM_TEXT_CHANNEL_1_AND_CHANNEL_10_PERCUSSION);
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
            if (ui_button("Apply", ui_available().x, 0) || ui_dialog_enter()) e->config = false;
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
    if (e->project.channels[channel].program != program || !e->project.channels[channel].enabled) {
        ed_checkpoint(e);
        e->project.channels[channel].enabled = true;
        e->project.channels[channel].program = program;
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

void ed_instrument(Editor *e) {
    if (e->instrument_channel < 0) {
        return;
    }
    bool open = true;
    bool show = ui_dialog(DM_TEXT_INSTRUMENT, &open, 460, 320);
    int c = e->instrument_channel;
    if (show) {
        char text[128];
        snprintf(text, sizeof(text), DM_TEXT_CHANNEL_D_S, c + 1,
                 c == 9 ? DM_TEXT_GENERAL_MIDI_PERCUSSION_1A7010 : "");
        ui_text(text);
        ui_separator();
        if (ui_combo(DM_TEXT_PROGRAM, dm_program_name(e->project.channels[c].program))) {
            for (int i = 0; i < 128; ++i) {
                if (ui_select(dm_program_name(i), e->project.channels[c].program == i, 0, 0)) {
                    ed_choose_program(e, c, i);
                }
            }
            ui_combo_end();
        }
        if (c == 9) {
            ui_dim(DM_TEXT_CHANNEL_10_USES_PERCUSSION_MAPPING_IN_GENERAL_MIDI);
        }

        if (e->project.channels[c].enabled && !ed_channel_has_clips(e, c) &&
            ui_button(DM_TEXT_REMOVE_EMPTY_CHANNEL, 0, 0) && ed_remove_empty_channel(e, c)) {
            open = false;
        }
        if (ui_button("Apply", ui_available().x, 0) || e->instrument_accept) {
            open = false;
        }
    }
    ui_end();
    if (!open) {
        e->instrument_accept = false;
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
    if (e->device >= 0 && ui_check(DM_TEXT_ALSO_MONITOR_SOUNDFONT, &e->monitor)) {
        dm_audio_monitor(e->audio, e->monitor);
        ed_preference_save(e);
        ed_publish(e);
    }
}

void ed_preferences(Editor *e) {
    if (!e->preferences) {
        return;
    }
    bool show = ui_dialog(DM_TEXT_PREFERENCES, &e->preferences, 630, 400);
    e->preferences_rect = ui_window_rect();
    if (show) {
        if (ui_color(DM_TEXT_APPLICATION_COLOR, &e->accent)) {
            ed_preference_save(e);
        }
        if (ui_check(DM_TEXT_USE_DEFAULT_OS_MOUSE_CURSOR, &e->os_cursor)) {
            ui_custom_cursor(!e->os_cursor);
            ed_preference_save(e);
        }
        ui_separator();
        ui_text(DM_TEXT_SOUND_OUTPUT);
        ui_separator();
        char text[256];
        snprintf(text, sizeof(text), DM_TEXT_AUDIO_S_48_KHZ_STEREO_64_FRAME_SYNTHESIS_BLOCKS,
                 dm_audio_driver(e->audio));
        ui_text(text);

        if (ui_button(DM_TEXT_RETRY_AUDIO_DEVICE, 160, 0)) {
            if (!e->audio) {
                e->audio = dm_audio_create(e->status, sizeof(e->status));
            } else {
                dm_audio_retry(e->audio, e->status, sizeof(e->status));
            }
            ed_publish(e);
        }

        if (ui_button(DM_TEXT_CHOOSE_SOUNDFONT, 170, 0)) {
            ed_dialog(e, DIALOG_FONT);
        }
        ui_text(*e->project.soundfont
                    ? e->project.soundfont
                    : DM_TEXT_NO_SOUNDFONT_SELECTED_CHOOSE_AN_SF2_SF3_FILE_TO_HEAR_NOTES);
        if (dm_audio_busy(e->audio)) {
            ui_dim(DM_TEXT_BACKGROUND_OPERATION_IN_PROGRESS);
            if (ui_button(DM_TEXT_CANCEL_OPERATION, 140, 0)) {
                dm_audio_cancel(e->audio);
            }
        }
        midi_preferences(e);
        ui_separator();
        if (ui_button(DM_TEXT_CLOSE, 90, 0)) {
            e->preferences = false;
        }
    }
    ui_end();
}

static void shortcut_row(Editor *e, int i) {
    ui_id(i);
    ui_text(ed_action_names[i]);
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
        ui_text(e->capture >= 0 ? DM_TEXT_PRESS_A_NEW_KEY_COMBINATION_ESCAPE_CANCELS_CAPTURE
                                : DM_TEXT_CLICK_A_BINDING_TO_CHANGE_IT);
        ui_same();
        if (ui_button(DM_TEXT_RESET_DEFAULTS, 130, 0)) {
            ed_defaults(e);
            ed_preference_save(e);
        }
        ui_child(DM_TEXT_SHORTCUT_LIST, 0, 0, true);
        for (int i = 0; i < ACT_COUNT; ++i) {
            static const char *categories[ACT_COUNT] = {
                [0] = DM_TEXT_TRANSPORT_INPUT,
                [3] = DM_TEXT_NAVIGATION,
                [11] = DM_TEXT_PATTERNS_SELECTION,
                [19] = DM_TEXT_ZOOM_KEYBOARD_PIANO_OFF,
                [22] = DM_TEXT_WINDOWS_TOOLS_KEYBOARD_PIANO_OFF};
            if (categories[i]) {
                ui_separator();
                ui_dim(categories[i]);
            }
            shortcut_row(e, i);
        }
        ui_separator();
        ui_dim(DM_TEXT_FIXED_SHORTCUTS);
        ui_wrap("Escape: close/unfocus; Enter: accept input; Up/Down: numeric step");
        ui_wrap("Ctrl+A: select all; Delete: delete selection");
        ui_wrap("Ctrl+S: save; Ctrl+Shift+S: save as; Ctrl+E: export");
        ui_wrap("Ctrl+Z/Y: undo/redo; Ctrl+C/X/V: clipboard");
        ui_wrap("Alt: disable snap; Right drag: erase; Shift+Right click: menu");
        ui_wrap("Wheel: horizontal; Shift+Wheel: vertical; ruler/Ctrl+Wheel: zoom");
        ui_child_end();
    }
    ui_end();
    if (!e->shortcuts) {
        e->capture = -1;
    }
}
