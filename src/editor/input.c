#include "editor/internal.h"
#include "text/en.h"

static bool escape_active(Editor *e);
static void escape(Editor *e);
static void channel_switch(Editor *e, bool solo);
static bool action_input(Editor *e, int a);
static void toggle_play_mode(Editor *e);
static bool action_global(Editor *e, int a);
static bool action_pattern(Editor *e, int a);
static void action_scroll(Editor *e, int a, View *v);
static void action_navigation(Editor *e, int a);
static void capture_binding(Editor *e, Binding binding);
static bool fixed_binding(Editor *e, SDL_Scancode key, SDL_Keymod mods, bool repeat);
static bool piano_blocks(Editor *e, int action, SDL_Scancode key, SDL_Keymod mods);
static bool dispatch_custom(Editor *e, Binding binding, bool repeat);
static void output_extension(Editor *e, char *output, size_t capacity, int kind);
static void remember_folder(Editor *e, int kind, const char *path);
static void dialog_output(Editor *e, int kind, const char *path);
static void dialog_result(Editor *e, const SDL_Event *event);
static bool file_extension(const char *path, const char *extension);
static bool midi_path(const char *path);
static void drop_path(Editor *e, const char *path, UiPoint at);
static bool file_drop(Editor *e, const SDL_Event *event);
static bool instrument_key(Editor *e, const SDL_KeyboardEvent *key);

/* Escape first closes an open menu, then leaves the field being edited (or closes its dialog,
   when the dialog exists for that field), then cancels a shortcut capture. */
static bool escape_active(Editor *e) {
    if (ui_cancel_popup()) {
        return true;
    }

    if (ui_text_active() || ui_any_active()) {
        ui_dialog_close_front(true);
        ui_escape();
        return true;
    }

    if (e->capture >= 0) {
        e->capture = -1;
        return true;
    }

    return false;
}

static void escape(Editor *e) {
    if (escape_active(e) || ui_dialog_close_front(false)) {
        return;
    }

    bool hidden = e->roll_auto_hidden;

    if (ed_deselect_focused(e)) {
        e->roll_auto_hidden = hidden;
        return;
    }

    if (ed_selection_focus(e) == FOCUS_PLAYLIST && e->channel_selected) {
        ed_release_piano(e);
        e->channel_selected = false;
        return;
    }

    if (ed_clear_loop(e)) {
        return;
    }

    if (e->roll) {
        e->roll = false;
        e->roll_auto_hidden = false;
        ed_focus_edit(e, FOCUS_PLAYLIST);
    }
}

bool ed_reserved(SDL_Scancode key, SDL_Keymod mods) {
    return key == SDL_SCANCODE_ESCAPE || (key == SDL_SCANCODE_A && mods == SDL_KMOD_CTRL);
}

static void channel_switch(Editor *e, bool solo) {
    if (!e->active || ed_context(e) != CONTEXT_PLAYLIST || !e->channel_selected) {
        return;
    }

    ed_checkpoint(e);
    DmPortMix mix =
        dm_port_mix(&e->project, e->note_ports[e->selected_channel], e->selected_channel);

    if (solo) {
        mix.solo = !mix.solo;
        mix.mute = false;
    } else {
        mix.mute = !mix.mute;
        mix.solo = false;
    }

    if (!dm_port_mix_set(&e->project, mix)) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_PORT_MIXER);
        return;
    }

    ed_commit(e);
}

static int swapped_channel(int channel, int a, int b) {
    return channel == a ? b : channel == b ? a : channel;
}

static void move_channel(Editor *e, int direction) {
    if (!e->active || e->edit_focus != FOCUS_PLAYLIST || !e->channel_selected) {
        return;
    }

    int channels[MIDI_CHANNELS], count = ed_channels(e, channels), a = e->selected_channel;
    int b = -1;

    for (int i = 0; i < count; ++i) {
        if (channels[i] == a && i + direction >= 0 && i + direction < count) {
            b = channels[i + direction];
            break;
        }
    }

    if (b < 0) {
        return;
    }

    ed_release_piano(e);
    ed_midi_input_finish(e, false);

    if (!ed_checkpoint(e)) {
        return;
    }

    dm_channel_swap(&e->project, a, b);
    bool expanded = e->channel_expanded[a];
    e->channel_expanded[a] = e->channel_expanded[b];
    e->channel_expanded[b] = expanded;
    uint8_t port = e->note_ports[a];
    e->note_ports[a] = e->note_ports[b];
    e->note_ports[b] = port;
    DmChannel reset = e->reset_channels[a];
    e->reset_channels[a] = e->reset_channels[b];
    e->reset_channels[b] = reset;

    for (int p = 0; p < DM_NOTE_PORTS; ++p) {
        int left = p * MIDI_CHANNELS + a, right = p * MIDI_CHANNELS + b;
        float peak = e->mixer_peaks[left], target = e->mixer_peak_targets[left];
        e->mixer_peaks[left] = e->mixer_peaks[right];
        e->mixer_peaks[right] = peak;
        e->mixer_peak_targets[left] = e->mixer_peak_targets[right];
        e->mixer_peak_targets[right] = target;
        uint8_t volume = e->mixer_live_volume[left], pan = e->mixer_live_pan[left];
        e->mixer_live_volume[left] = e->mixer_live_volume[right];
        e->mixer_live_volume[right] = volume;
        e->mixer_live_pan[left] = e->mixer_live_pan[right];
        e->mixer_live_pan[right] = pan;
    }

    e->selected_channel = b;
    e->pattern_channel = swapped_channel(e->pattern_channel, a, b);
    e->piano_channel = swapped_channel(e->piano_channel, a, b);
    e->preview_source_channel = swapped_channel(e->preview_source_channel, a, b);
    e->song_event_channel = swapped_channel(e->song_event_channel, a, b);
    e->instrument_channel = swapped_channel(e->instrument_channel, a, b);
    e->lane_channel = swapped_channel(e->lane_channel, a, b);
    e->remove_note_row_channel = swapped_channel(e->remove_note_row_channel, a, b);
    e->last_automation.channel = swapped_channel(e->last_automation.channel, a, b);
    e->instrument_source.key.channel =
        (uint8_t)swapped_channel(e->instrument_source.key.channel, a, b);
    e->midi_sample_source.key.channel =
        (uint8_t)swapped_channel(e->midi_sample_source.key.channel, a, b);
    ed_keyboard_channel(e);
    ed_commit(e);
}

/* Shift+E: with a note pattern in the pattern editor, show or hide its Events section; with the
   Event editor showing, hide it; with the pattern editor hidden, open the Event editor on the
   selected automation pattern, or the first one (created if there is none). */
static void events_shortcut(Editor *e) {
    if (e->roll && !e->roll_auto_hidden) {
        if (ed_pattern(e) && !ed_pattern(e)->kind) {
            e->automation_open = !e->automation_open;
        } else {
            e->roll = false;
        }

        return;
    }

    if (!ed_pattern(e) || !ed_pattern(e)->kind) {
        for (size_t i = 0; i < e->project.pattern_count; ++i) {
            if (e->project.patterns[i].kind) {
                ed_open_pattern(e, (int)i, false);
                ed_bind_pattern_channel(e);
                break;
            }
        }

        if (!ed_pattern(e) || !ed_pattern(e)->kind) {
            ed_step_pattern(e, true, 1);
        }
    }

    if (ed_pattern(e) && ed_pattern(e)->kind) {
        e->roll = true;
        e->roll_auto_hidden = false;
    }
}

/* Opens the instrument window for the pattern editor's channel when the pattern editor or its
   events have focus, or for the selected playlist channel. The choice sets both the channel and
   the pattern editor's instrument dropdown. */
static void instrument_shortcut(Editor *e) {
    EditFocus focus = ed_selection_focus(e);

    if (!e->active || (focus != FOCUS_PLAYLIST && focus != FOCUS_NOTES && focus != FOCUS_EVENTS)) {
        return;
    }

    int channel = focus == FOCUS_PLAYLIST ? e->selected_channel : e->pattern_channel;

    if (channel < 0 || channel >= MIDI_CHANNELS) {
        return;
    }

    ed_open_instrument(e, channel, true);
}

static bool action_input(Editor *e, int a) {
    switch (a) {
    case ACT_SONG_EVENT:
        if (!e->active) {
            break;
        }

        events_shortcut(e);
        break;
    case ACT_INSTRUMENT:
        instrument_shortcut(e);
        break;
    case ACT_CHANNEL_UP:
    case ACT_CHANNEL_DOWN:
        move_channel(e, a == ACT_CHANNEL_UP ? -1 : 1);
        break;
    case ACT_MUTE:
        channel_switch(e, false);
        break;
    case ACT_SOLO:
        channel_switch(e, true);
        break;
    case ACT_RECORD:
    case ACT_RECORD_GLOBAL:
        if (e->active) {
            ed_toggle_record(e);
        }
        break;
    case ACT_METRONOME:
        e->metronome = !e->metronome;
        dm_audio_metronome(e->audio, e->metronome, e->project.bpm);
        break;
    case ACT_WAIT:
        e->wait_input = !e->wait_input;
        ed_cancel_wait(e);
        break;
    case ACT_OVERLAP:
        e->overwrite = !e->overwrite;
        break;
    default:
        return false;
    }

    return true;
}

static void toggle_play_mode(Editor *e) {
    ed_release_piano(e);
    e->pattern_mode = !e->pattern_mode && ed_pattern(e);

    if (e->active) {
        ed_transport_publish(e, dm_beat_seconds(&e->project, e->cursor_beat[e->pattern_mode]),
                             dm_audio_playing(e->audio));
    }
}

static bool action_global(Editor *e, int a) {
    if (action_input(e, a)) {
        return true;
    }

    switch (a) {
    case ACT_PIANO_DOWN:
    case ACT_PIANO_UP:
        ed_release_piano(e);
        e->piano_octave = (int)fmax(-5, fmin(4, e->piano_octave + (a == ACT_PIANO_UP ? 1 : -1)));
        break;
    case ACT_PLAY_START:
        e->start_beat[e->pattern_mode] = 0;
        ed_seek(e, 0);

        if (!dm_audio_playing(e->audio)) {
            ed_play(e);
        }
        break;
    case ACT_PIANO:
        ed_release_piano(e);
        e->piano = !e->piano;
        break;
    case ACT_PLAY:
        ed_play(e);
        break;
    case ACT_PLAY_MODE:
        toggle_play_mode(e);
        break;
    case ACT_PREFERENCES:
        ed_release_piano(e);
        e->preferences = true;
        ui_dialog_raise(DM_TEXT_PREFERENCES);
        break;
    case ACT_SHORTCUTS:
        e->shortcuts = true;
        break;
    case ACT_CONFIG:
        if (!e->active) {
            e->new_mode = 1;
        }

        e->config = true;
        break;
    case ACT_EDITOR:
        ed_toggle_roll(e);
        break;
    default:
        return false;
    }

    return true;
}

static bool action_pattern(Editor *e, int a) {
    switch (a) {
    case ACT_COPY_NOTE_BENDS:
    case ACT_CUT_NOTE_BENDS:
        if (ed_selection_focus(e) == FOCUS_NOTES) {
            ed_clipboard_note_bends(e, a == ACT_CUT_NOTE_BENDS);
        }
        break;
    case ACT_NOTE_COLOR:
        if (ui_popup_open()) {
            break;
        }

        /* Alt+C colors notes in the pattern editor and patterns in the pattern section or
           playlist. */
        if (ed_selection_focus(e) == FOCUS_NOTES && ed_pattern(e) && !ed_pattern(e)->kind) {
            e->note_color_popup = true;
        } else if (ed_selection_focus(e) == FOCUS_PATTERNS && e->pattern_highlight) {
            ed_pattern_color_request(e, e->selected_pattern);
        } else if (ed_selection_focus(e) == FOCUS_PLAYLIST) {
            ed_pattern_color_request(e, -1);
        }

        break;
    case ACT_DUPLICATE:
        if (ed_selection_focus(e) == FOCUS_PATTERNS) {
            if (e->pattern_highlight) {
                ed_duplicate_pattern(e, e->selected_pattern);
            }
        } else if (ed_selection_focus(e) == FOCUS_PLAYLIST) {
            ed_duplicate_clips(e);
        } else if (ed_selection_focus(e) == FOCUS_EVENTS) {
            ed_duplicate_events(e);
        } else if (ed_selection_focus(e) != FOCUS_OTHER) {
            ed_duplicate_notes(e);
        }
        break;
    case ACT_QUANTIZE:
        if (ed_selection_focus(e) == FOCUS_EVENTS && ed_graph_resolution(e)) {
            ed_graph_snap(e); /* Snap bars. */
        } else if (ed_context(e) == CONTEXT_ROLL && ed_pattern(e)) {
            ed_quantize_notes(e);
        }
        break;
    case ACT_NEXT:
    case ACT_NEXT_AUTOMATION:
        ed_step_pattern(e, e->automation_browser || a == ACT_NEXT_AUTOMATION, 1);
        break;
    case ACT_PREVIOUS:
    case ACT_PREVIOUS_AUTOMATION:
        ed_step_pattern(e, e->automation_browser || a == ACT_PREVIOUS_AUTOMATION, -1);
        break;
    case ACT_ITEM_UP:
        ed_transpose(e, 1);
        break;
    case ACT_ITEM_DOWN:
        ed_transpose(e, -1);
        break;
    case ACT_OCTAVE_UP:
        if (ed_context(e) == CONTEXT_ROLL) {
            ed_transpose(e, 12);
        }
        break;
    case ACT_OCTAVE_DOWN:
        if (ed_context(e) == CONTEXT_ROLL) {
            ed_transpose(e, -12);
        }
        break;
    default:
        return false;
    }

    return true;
}

static void action_scroll(Editor *e, int a, View *v) {
    if (a == ACT_TOP) {
        v->scroll_y = 0;
    }

    if (a == ACT_BOTTOM) {
        v->scroll_y = 100000;
    }

    if (a == ACT_UP) {
        v->scroll_y = fmaxf(0, v->scroll_y - (ed_context(e) == CONTEXT_ROLL ? 20 : 64));
    }

    if (a == ACT_DOWN) {
        v->scroll_y += (ed_context(e) == CONTEXT_ROLL ? 20 : 64);
    }
}

static void action_navigation(Editor *e, int a) {
    View *v = ed_context(e) == CONTEXT_ROLL ? &e->roll_view : &e->playlist_view;

    switch (a) {
    case ACT_FORWARD:
    case ACT_BACK: {
        double seconds = dm_audio_position(e->audio) + (a == ACT_FORWARD ? 1 : -1);
        ed_seek(e, dm_seconds_beat(&e->project, fmax(0, seconds)));
        break;
    }
    case ACT_FORWARD_BEAT:
    case ACT_BACK_BEAT:
        ed_seek(e, dm_seconds_beat(&e->project, dm_audio_position(e->audio)) +
                       (a == ACT_FORWARD_BEAT ? 1 : -1) * dm_bar(&e->project) / 4);
        break;
    case ACT_HOME:
        ed_seek(e, 0);
        break;
    case ACT_END:
        ed_seek(e, ed_context(e) == CONTEXT_ROLL && ed_pattern(e) ? ed_pattern_length(e)
                                                               : dm_duration(&e->project));
        break;
    default:
        break;
    }

    action_scroll(e, a, v);
}

void ed_action_zoom(Editor *e, int a) {
    if (ed_context(e) == CONTEXT_ROLL && ed_pattern(e) && ed_pattern(e)->kind && ed_pattern(e)->mode) {
        return;
    }

    View *v = ed_context(e) == CONTEXT_ROLL ? &e->roll_view : &e->playlist_view;

    if (a == ACT_ZOOM_IN) {
        v->zoom = fminf(ED_ZOOM_MAX, v->zoom * 1.2f);
    }

    if (a == ACT_ZOOM_OUT) {
        v->zoom = fmaxf(ED_ZOOM_MIN, v->zoom / 1.2f);
    }

    if (a == ACT_ZOOM_RESET) {
        v->zoom = 1;
    }
}

void ed_action(Editor *e, int a) {
    if (action_global(e, a)) {
        return;
    }

    if (a == ACT_NEXT || a == ACT_PREVIOUS || a == ACT_NEXT_AUTOMATION ||
        a == ACT_PREVIOUS_AUTOMATION) {
        ed_release_piano(e);
    }

    if (a >= ACT_SELECT && a <= ACT_BRUSH) {
        e->tool = TOOL_SELECT + (a - ACT_SELECT);
        return;
    }

    if (!e->active) {
        return;
    }

    if (action_pattern(e, a)) {
        return;
    }

    if (a >= ACT_ZOOM_IN && a <= ACT_ZOOM_RESET) {
        ed_action_zoom(e, a);
        return;
    }

    action_navigation(e, a);
}

static void capture_binding(Editor *e, Binding binding) {
    if (!binding.mouse) {
        SDL_Scancode key = binding.key;

        if (key == SDL_SCANCODE_LSHIFT || key == SDL_SCANCODE_RSHIFT || key == SDL_SCANCODE_LCTRL ||
            key == SDL_SCANCODE_RCTRL || key == SDL_SCANCODE_LALT || key == SDL_SCANCODE_RALT ||
            key == SDL_SCANCODE_LGUI || key == SDL_SCANCODE_RGUI) {
            return;
        }

        if (ed_reserved(key, binding.mods)) {
            ed_message(e, DM_TEXT_KEY_RESERVED_FIXED);
            return;
        }
    }

    for (int i = 0; i < ACT_COUNT; ++i) {
        if (i != e->capture && ed_same_binding(e->bindings[i], binding)) {
            ed_message(e, DM_TEXT_BINDING_ALREADY_USE);
            return;
        }
    }

    e->bindings[e->capture] = binding;
    e->capture = -1;
    ed_preference_save(e);
}

/* Save and Export also work while a text field is being edited. The save runs at the end of the
   frame, once no drag is in progress. A custom shortcut on the same keys takes their place. */
static bool file_shortcut(Editor *e, SDL_Scancode key, SDL_Keymod mods, bool repeat) {
    bool shift = mods == (SDL_KMOD_CTRL | SDL_KMOD_SHIFT);

    if (mods != SDL_KMOD_CTRL && !(shift && key == SDL_SCANCODE_S)) {
        return false;
    }

    if (key != SDL_SCANCODE_S && key != SDL_SCANCODE_E) {
        return false;
    }

    for (int i = 0; i < ACT_COUNT; ++i) {
        if (ed_same_binding(e->bindings[i], (Binding){.key = key, .mods = mods})) {
            return false;
        }
    }

    if (key == SDL_SCANCODE_S && !repeat) {
        e->pending_save = true;
        e->pending_save_as = shift;
    } else if (key == SDL_SCANCODE_E && e->active) {
        e->export_window = true;
    }

    return true;
}

static bool fixed_binding(Editor *e, SDL_Scancode key, SDL_Keymod mods, bool repeat) {
    if (repeat) {
        return false;
    }

    if (mods != SDL_KMOD_CTRL && mods != (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) {
        return false;
    }

    bool shift = (mods & SDL_KMOD_SHIFT) != 0;

    switch (key) {
    case SDL_SCANCODE_Z:
        ed_undo(e, shift);
        break;
    case SDL_SCANCODE_Y:
        ed_undo(e, true);
        break;
    case SDL_SCANCODE_C:
        if (shift) {
            return false;
        }

        ed_clipboard(e, CLIPBOARD_COPY);
        break;
    case SDL_SCANCODE_X:
        if (shift) {
            return false;
        }

        ed_clipboard(e, CLIPBOARD_CUT);
        break;
    case SDL_SCANCODE_V:
        ed_clipboard(e, CLIPBOARD_PASTE);
        break;
    default:
        return false;
    }

    return true;
}

static void keyboard_mix(Editor *e, int route, int channel) {
    int port = e->note_ports[channel];
    DmPortMix mix = dm_port_mix(&e->project, port, channel);
    bool solo = false;

    for (int c = 0; c < 16; c++) {
        solo |= e->project.channels[c].enabled && e->project.channels[c].solo;
    }

    for (size_t i = 0; i < e->project.port_mix_count; i++) {
        solo |= e->project.channels[e->project.port_mix[i].channel].enabled &&
                e->project.port_mix[i].solo;
    }

    dm_audio_keyboard_mix(e->audio, route, port * 16 + channel,
                          mix.mute || (solo && !mix.solo) ? 0 : mix.volume, mix.pan);
}

int ed_pattern_keyboard_channel(Editor *e) {
    ed_follow_preview(e);

    int channel = e->preview_program == 128 ? MIDI_DRUM_CHANNEL : 0;

    if (e->preview_route >= 0 && e->preview_route != channel) {
        dm_audio_keyboard_program(e->audio, e->preview_route, -1, false);
    }

    e->preview_route = channel;
    dm_audio_keyboard_program(e->audio, channel, e->preview_program == 128 ? 0 : e->preview_program,
                              e->preview_program == 128);
    keyboard_mix(e, channel, e->pattern_channel);
    return channel;
}

int ed_keyboard_channel(Editor *e) {
    if (ed_context(e) == CONTEXT_ROLL && ed_pattern(e)) {
        return ed_pattern_keyboard_channel(e);
    }

    if (e->preview_route >= 0) {
        dm_audio_keyboard_program(e->audio, e->preview_route, -1, false);
        e->preview_route = -1;
    }

    keyboard_mix(e, e->selected_channel, e->selected_channel);
    return e->selected_channel;
}

bool ed_record_start(Editor *e) {
    if (e->wait_armed && e->record && !dm_audio_playing(e->audio)) {
        /* Prepare/clear the take while stopped. Publishing it after starting
           playback could briefly schedule the old take and then cut it off. */
        if (!ed_begin_record_take(e)) {
            return false;
        }

        if (!ed_transport_publish(e, dm_beat_seconds(&e->project, e->start_beat[e->pattern_mode]),
                                  true)) {
            return false;
        }

        e->wait_armed = false;
        e->recording_take = true;
    }

    return !e->record || !dm_audio_playing(e->audio) || ed_begin_record_take(e);
}

void ed_piano_press(Editor *e, SDL_Scancode key, SDL_Keymod mods, bool repeat) {
    int note = ed_piano_key(key);

    if (ed_context(e) == CONTEXT_PLAYLIST && !e->channel_selected) {
        return;
    }

    if (e->piano && e->active && note >= 0 && !mods && !repeat) {
        e->piano_channel = ed_keyboard_channel(e);
        note = ed_keyboard_note(e, key, ed_route_drums(e, e->piano_channel));

        if (note < 0 || note > 127) {
            return;
        }

        if (!ed_record_start(e)) {
            return;
        }

        e->held[key] = note;
        e->piano_channel = ed_keyboard_channel(e);
        e->held_channels[key] = e->piano_channel;

        if (ed_context(e) == CONTEXT_ROLL || e->project.channels[e->piano_channel].program >= 0) {
            dm_audio_preview(e->audio, e->piano_channel, note, 100, true);
        }

        e->record_start[note] = e->record && dm_audio_playing(e->audio)
                                    ? dm_seconds_beat(&e->project, dm_audio_position(e->audio))
                                    : -1;
        /* The take's first note fixes where the pattern starts; a loop back restarts it. */
        if (e->record_start[note] >= 0) {
            ed_record_origin(e, e->record_start[note]);
        }

        e->record_origin[note] = e->take_origin_set ? e->take_origin : 0;
    }
}

static bool piano_blocks(Editor *e, int action, SDL_Scancode key, SDL_Keymod mods) {
    if (!e->piano) {
        return false;
    }

    if (ed_actions[action].flags & ACTION_PIANO_OFF) {
        return true;
    }

    return !mods && ed_piano_key(key) >= 0 && action != ACT_PIANO;
}

static bool dispatch_custom(Editor *e, Binding binding, bool repeat) {
    for (int i = 0; i < ACT_COUNT; ++i) {
        if (!ed_same_binding(e->bindings[i], binding) ||
            (!binding.mouse && piano_blocks(e, i, binding.key, binding.mods))) {
            continue;
        }

        if ((ed_actions[i].flags & ACTION_WORKSPACE) && ui_dialog_open() &&
            !(i == ACT_NOTE_COLOR && ui_workspace_focused())) {
            return true;
        }

        if (repeat && !(ed_actions[i].flags & ACTION_REPEATS)) {
            return true;
        }

        ed_action(e, i);
        return true;
    }

    return false;
}

void ed_shortcut(Editor *e, SDL_Scancode key, SDL_Keymod raw, bool repeat) {
    SDL_Keymod mods = ed_normalized(raw);
    Binding binding = {.key = key, .mods = mods};

    if (key == SDL_SCANCODE_ESCAPE) {
        escape(e);
        return;
    }

    if (e->capture >= 0) {
        capture_binding(e, binding);
        return;
    }

    if (file_shortcut(e, key, mods, repeat)) {
        return;
    }

    if (ui_text_active()) {
        return;
    }

    if (key == SDL_SCANCODE_A && mods == SDL_KMOD_CTRL) {
        if (!ui_dialog_open()) {
            ed_select_all(e);
        }

        return;
    }

    if (dispatch_custom(e, binding, repeat)) {
        return;
    }

    if (fixed_binding(e, key, mods, repeat)) {
        return;
    }

    if (key == SDL_SCANCODE_DELETE && !repeat && !ui_dialog_open()) {
        ed_delete_selection(e);
        return;
    }

    if (!ui_dialog_open()) {
        ed_piano_press(e, key, mods, repeat);
    }
}

static void output_extension(Editor *e, char *output, size_t capacity, int kind) {
    char audio_extension[12];
    snprintf(audio_extension, sizeof(audio_extension), ".%s",
             dm_audio_codec_extension(e->audio_options.codec));
    char plugin_extension[20];
    snprintf(plugin_extension, sizeof(plugin_extension), ".%s",
             kind == DIALOG_EXPORT_PLUGIN ? ed_export_extension(e) : "");
    const char *extension = kind == DIALOG_SAVE            ? ".duck"
                            : kind == DIALOG_MIDI          ? ".mid"
                            : kind == DIALOG_WAVE          ? audio_extension
                            : kind == DIALOG_EXPORT_PLUGIN ? plugin_extension
                                                           : "";
    const char *leaf = strrchr(output, '\\');

    if (!leaf) {
        leaf = strrchr(output, '/');
    }

    if (!strchr(leaf ? leaf + 1 : output, '.') && strlen(output) + strlen(extension) < capacity) {
        strcat(output, extension);
    }
}

static void remember_folder(Editor *e, int kind, const char *path) {
    if (kind == DIALOG_OPEN || kind == DIALOG_FONT || kind == DIALOG_MIDI || kind == DIALOG_WAVE ||
        kind == DIALOG_EXPORT_PLUGIN) {
        char *folder = kind == DIALOG_OPEN   ? e->midi_folder
                       : kind == DIALOG_FONT ? e->font_folder
                                             : e->export_folder;
        snprintf(folder, DM_PATH, "%s", path);
        char *slash = strrchr(folder, '/'), *backslash = strrchr(folder, '\\');

        if (!slash || (backslash && backslash > slash)) {
            slash = backslash;
        }

        if (slash) {
            slash[1] = 0;
        } else {
            folder[0] = 0;
        }

        ed_preference_save(e);
    }
}

static void dialog_output(Editor *e, int kind, const char *path) {
    if (kind == DIALOG_EXPORT_PLUGIN && !*ed_export_extension(e)) {
        ed_export_error(e, DM_TEXT_NO_PENDING_EXPORTER_SAVE);
        return;
    }

    if (strlen(path) + 18 >= DM_PATH) {
        if (kind == DIALOG_EXPORT_PLUGIN) {
            ed_export_error(e, DM_TEXT_PATH_EXCEEDS_SUPPORTED);
            ed_export_picker_done(e, NULL);
        } else {
            ed_message(e, DM_TEXT_PATH_EXCEEDS_SUPPORTED);
        }

        e->pending_action = 0;
        return;
    }

    char output[DM_PATH];
    snprintf(output, sizeof(output), "%s", path);
    output_extension(e, output, sizeof(output), kind);
    remember_folder(e, kind, path);

    if (kind == DIALOG_OPEN) {
        ed_request(e, PENDING_PATH, path);
    } else if (kind == DIALOG_SAVE) {
        if (!ed_save_to(e, output) && e->pending_action) {
            e->confirm = true;
        }
    } else if (kind == DIALOG_FONT) {
        ed_request_font(e, path);
    } else if (kind == DIALOG_WAVE) {
        if (dm_audio_export(e->audio, &e->project, output, &e->audio_options, e->status,
                            sizeof(e->status))) {
            snprintf(e->audio_export_pending, DM_PATH, "%s", output);
        }

        snprintf(e->audio_export_message, sizeof(e->audio_export_message), "%s", e->status);
    } else if (kind == DIALOG_MIDI) {
        DmBytes b = {0};
        bool encoded =
            dm_midi_export(&e->project, &b, true, e->export_metadata, e->status, sizeof(e->status));

        if (encoded && dm_write(output, b.data, b.size, e->status, sizeof(e->status))) {
            ed_message(e, DM_TEXT_MIDI_EXPORT_COMPLETE);
        }

        dm_bytes_free(&b);
    } else if (kind == DIALOG_EXPORT_PLUGIN) {
        ed_export_picker_done(e, output);
    } else if (kind == DIALOG_REPOSITORY) {
        ed_export_repository_done(e, path);
    } else if (kind == DIALOG_PLUGIN_FOLDER) {
        ed_export_folder_done(e, path);
    }
}

static void dialog_result(Editor *e, const SDL_Event *event) {
    e->dialog_busy = false;

    if (event->user.code == DIALOG_SAMPLE_FILES || event->user.code == DIALOG_SAMPLE_FOLDER) {
        ed_samples_picker_result(e, event);
        return;
    }

    const char *path = event->user.data1, *error = event->user.data2;

    if (error) {
        if (event->user.code == DIALOG_EXPORT_PLUGIN) {
            ed_export_error(e, error);
        } else {
            ed_message(e, error);
        }
    }

    if (path) {
        dialog_output(e, event->user.code, path);
    } else if (event->user.code == DIALOG_EXPORT_PLUGIN) {
        ed_export_picker_done(e, NULL);
    } else if (event->user.code == DIALOG_REPOSITORY) {
        ed_export_repository_done(e, NULL);
    } else if (event->user.code == DIALOG_PLUGIN_FOLDER) {
        ed_export_folder_done(e, NULL);
    } else if (event->user.code == DIALOG_SAVE) {
        e->pending_action = 0;
    }

    SDL_free(event->user.data1);
    SDL_free(event->user.data2);
}

bool ed_instrument_topmost(Editor *e) {
    return e->instrument_channel >= 0 && ui_dialog_front(DM_TEXT_INSTRUMENT);
}

static bool file_extension(const char *path, const char *extension) {
    const char *dot = strrchr(path, '.');
    return dot && SDL_strcasecmp(dot, extension) == 0;
}

static bool midi_path(const char *path) {
    const char *extensions[] = {".mid", ".midi", ".rmi", ".mus", ".duck"};

    for (int i = 0; i < 5; ++i) {
        if (file_extension(path, extensions[i])) {
            return true;
        }
    }

    return false;
}

static void drop_path(Editor *e, const char *path, UiPoint at) {
    if (e->dialog_busy) {
        ed_message(e, DM_TEXT_CLOSE_FILE_PICKER);
    } else if (e->preferences && ui_window_front_at(DM_TEXT_PREFERENCES, at)) {
        if (file_extension(path, ".sf2") || file_extension(path, ".sf3")) {
            ed_request_font(e, path);
        } else {
            ed_message(e, DM_TEXT_DROP_SF2_SF3);
        }
    } else if (midi_path(path)) {
        ed_request(e, PENDING_PATH, path);
    } else {
        ed_message(e, DM_TEXT_DROP_MIDI_PROJECT);
    }
}

static bool file_drop(Editor *e, const SDL_Event *event) {
    if (event->type < SDL_EVENT_DROP_FILE || event->type > SDL_EVENT_DROP_POSITION) {
        return false;
    }

    if (event->drop.windowID && event->drop.windowID != SDL_GetWindowID(e->window)) {
        return true;
    }

    if (ed_samples_drop(e, &event->drop)) {
        return true;
    }

    if (event->type == SDL_EVENT_DROP_BEGIN) {
        e->drop_active = true;
        e->drop_handled = false;
        e->drop_position = event->drop.x || event->drop.y
                               ? (UiPoint){event->drop.x / ui_scale(), event->drop.y / ui_scale()}
                               : (UiPoint){-1, -1};
    } else if (event->type == SDL_EVENT_DROP_COMPLETE) {
        e->drop_active = e->drop_handled = false;
    } else if (event->type == SDL_EVENT_DROP_POSITION) {
        e->drop_active = true;
        e->drop_position = (UiPoint){event->drop.x / ui_scale(), event->drop.y / ui_scale()};
    } else if (event->type == SDL_EVENT_DROP_FILE && event->drop.data) {
        if (e->drop_active && e->drop_handled) {
            return true;
        }

        e->drop_handled = true;
        UiPoint at = {event->drop.x / ui_scale(), event->drop.y / ui_scale()};
        drop_path(e, event->drop.data, at);
    }

    return true;
}

bool ed_instrument_note(Editor *e, const SDL_KeyboardEvent *key) {
    int c = e->instrument_channel;
    int note = ed_keyboard_note(e, key->scancode, e->instrument_drums);

    if (c < 0 || key->repeat || note < 0 || note > 127) {
        return false;
    }

    if (e->preview_route >= 0 && e->preview_route != c) {
        dm_audio_keyboard_program(e->audio, e->preview_route, -1, false);
    }

    e->held[key->scancode] = note;
    e->record_start[note] = -1;
    e->held_channels[key->scancode] = c;
    dm_audio_keyboard_program(e->audio, c, e->instrument_program, e->instrument_drums);
    dm_audio_preview(e->audio, c, note, 100, true);
    e->preview_route = c;
    return true;
}

static bool instrument_key(Editor *e, const SDL_KeyboardEvent *key) {
    if (!ed_instrument_topmost(e) || ed_normalized(key->mod)) {
        return false;
    }

    if (key->scancode == SDL_SCANCODE_ESCAPE) {
        ui_cancel_popup();
        ed_close_instrument(e);
        return true;
    }

    if (key->scancode == SDL_SCANCODE_RETURN || key->scancode == SDL_SCANCODE_KP_ENTER) {
        if (!key->repeat) {
            ui_cancel_popup();
            ed_apply_instrument(e);
        }

        return true;
    }

    if (e->instrument_remove) {
        return true;
    }

    if (key->scancode == SDL_SCANCODE_UP || key->scancode == SDL_SCANCODE_DOWN) {
        ed_step_instrument(e, key->scancode == SDL_SCANCODE_DOWN ? 1 : -1);
        return true;
    }

    return !ui_popup_open() && !ui_text_active() && ed_instrument_note(e, key);
}

void editor_event(Editor *e, const SDL_Event *event) {
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_ESCAPE &&
        ed_midi_bindings_cancel(e)) {
        return;
    }

    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ed_side_button(event->button.button)) {
        Binding binding = {.mods = ed_normalized(SDL_GetModState()), .mouse = event->button.button};

        if (e->capture >= 0) {
            capture_binding(e, binding);
        } else if (!ui_text_active() && e->gesture_capture < 0) {
            dispatch_custom(e, binding, false);
        }

        return;
    }

    if (event->type == SDL_EVENT_KEY_DOWN &&
        (ed_samples_key(e, &event->key) || instrument_key(e, &event->key) ||
         ed_percussion_key(e, &event->key))) {
        return;
    }

    if (event->type == ed_dialog_event) {
        dialog_result(e, event);
        return;
    }

    if (event->type == SDL_EVENT_QUIT) {
        ed_request(e, PENDING_QUIT, NULL);
        return;
    }

    if (file_drop(e, event)) {
        return;
    }

    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        ed_release_piano(e);
        return;
    }

    if (event->type == SDL_EVENT_KEY_DOWN) {
        ed_shortcut(e, event->key.scancode, event->key.mod, event->key.repeat);
    }

    if (event->type == SDL_EVENT_KEY_UP) {
        int note = e->held[event->key.scancode];

        if (note >= 0) {
            dm_audio_preview(e->audio, e->held_channels[event->key.scancode], note, 0, false);
            e->held[event->key.scancode] = -1;
            ed_record_note(e, note);
        }
    }
}
