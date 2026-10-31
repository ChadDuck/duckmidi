#include "editor_internal.h"
#include "text/en.h"

static bool escape_active(Editor *e);
static int floating_topmost(void);
static bool close_floating(Editor *e);
static void escape(Editor *e);
static void channel_switch(Editor *e, bool solo);
static bool action_input(Editor *e, int a);
static void toggle_play_mode(Editor *e);
static bool action_global(Editor *e, int a);
static void discard_unused_pattern(Editor *e);
static void navigate_pattern(Editor *e, int direction);
static bool action_pattern(Editor *e, int a);
static void action_scroll(Editor *e, int a, View *v);
static void action_navigation(Editor *e, int a);
static void capture_binding(Editor *e, SDL_Scancode key, SDL_Keymod mods);
static bool fixed_binding(Editor *e, SDL_Scancode key, SDL_Keymod mods, bool repeat);
static bool piano_record_start(Editor *e);
static bool floating_open(Editor *e);
static bool context_action(int a);
static bool piano_blocks(Editor *e, int action, SDL_Scancode key, SDL_Keymod mods);
static bool dispatch_custom(Editor *e, SDL_Scancode key, SDL_Keymod mods, bool repeat);
static void output_extension(Editor *e, char *output, size_t capacity, int kind);
static void remember_folder(Editor *e, int kind, const char *path);
static void dialog_output(Editor *e, int kind, const char *path);
static void dialog_result(Editor *e, const SDL_Event *event);
static bool fixed_global_key(Editor *e, const SDL_Event *event);
static bool instrument_topmost(Editor *e);
static bool file_extension(const char *path, const char *extension);
static bool midi_path(const char *path);
static void drop_path(Editor *e, const char *path, UiPoint at);
static bool file_drop(Editor *e, const SDL_Event *event);
static bool instrument_key(Editor *e, const SDL_KeyboardEvent *key);

static bool escape_active(Editor *e) {
    if (ui_cancel_popup()) {
        return true;
    }

    int top = floating_topmost();

    if ((top == 9 && e->rename_pattern >= 0) || (top == 10 && e->lane_settings && e->lane_index)) {
        ui_escape();

        if (top == 9) {
            e->rename_pattern = -1;
        } else {
            e->lane_settings = false;
        }

        return true;
    }

    if (ui_text_active() || ui_any_active()) {
        ui_escape();
        return true;
    }

    if (e->capture >= 0) {
        e->capture = -1;
        return true;
    }

    return false;
}

static int floating_topmost(void) {
    const char *titles[] = {DM_TEXT_UNSAVED_PROJECT,
                            DM_TEXT_SONG_EVENT,
                            DM_TEXT_EXPORT,
                            DM_TEXT_KEYBOARD_SHORTCUTS,
                            DM_TEXT_PREFERENCES,
                            DM_TEXT_PROJECT_SETTINGS,
                            DM_TEXT_NEW_PROJECT,
                            DM_TEXT_ABOUT_DUCKMIDI,
                            DM_TEXT_INSTRUMENT,
                            DM_TEXT_RENAME_PATTERN,
                            DM_TEXT_AUTOMATION_LANE,
                            DM_TEXT_MIDI_EVENTS,
                            DM_TEXT_EXPORT_WARNINGS,
                            DM_TEXT_GENERATE_PATTERNS,
                            DM_TEXT_PERCUSSION_KEYBOARD_MAPPING,
                            DM_TEXT_REVIEW_EXPORT_MAPPINGS};
    return ui_topmost(titles, 16);
}

static bool close_floating(Editor *e) {
    int top = floating_topmost();
    bool *flags[] = {&e->confirm,     &e->song_event, &e->export_window, &e->shortcuts,
                     &e->preferences, &e->config,     &e->config,        &e->about};

    if (top < 0) {
        return false;
    }

    switch (top) {
    case 15:
        ed_export_review_decide(e, 0);
        break;
    case 14:
        e->percussion_window = false;
        e->percussion_listening = false;
        break;
    case 13:
        e->generate_window = false;
        break;
    case 12:
        ed_export_warning_close(e);
        break;
    case 8:
        e->instrument_channel = -1;
        break;
    case 9:
        e->rename_pattern = -1;
        break;
    case 10:
        e->lane_settings = false;
        break;
    case 11:
        e->midi_events = false;
        break;
    default:
        *flags[top] = false;
        break;
    }

    if (top == 0) {
        e->pending_action = 0;
    }

    if (top == 5 || top == 6) {
        e->new_mode = 0;
    }

    return true;
}

static void escape(Editor *e) {
    if (escape_active(e) || close_floating(e)) {
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
    if (!e->active || e->context != 1 || !e->channel_selected) {
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

static bool action_input(Editor *e, int a) {
    switch (a) {
    case ACT_SONG_EVENT:
        if (!e->active) {
            break;
        }

        if (e->song_event) {
            e->song_event = false;
        } else {
            ed_new_song_event(e);
        }
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

static void discard_unused_pattern(Editor *e) {
    if (e->selected_pattern == e->transient_pattern && e->transient_pattern > 0 &&
        (size_t)e->selected_pattern + 1 == e->project.pattern_count) {
        ed_checkpoint(e);
        dm_pattern_remove(&e->project, e->transient_pattern);
        e->transient_pattern = -1;
        ed_changed(e);
    }
}

static void navigate_pattern(Editor *e, int direction) {
    if (direction < 0) {
        discard_unused_pattern(e);
    }

    /* From a pattern of the other list, step from that list's first or last pattern. */
    int from = e->selected_pattern, count = (int)e->project.pattern_count;

    if (from < 0 || from >= count ||
        (e->project.patterns[from].kind != 0) != e->automation_browser) {
        from = direction > 0 ? -1 : count;
    }

    for (int i = from + direction; i >= 0 && i < count; i += direction) {
        if ((e->project.patterns[i].kind != 0) != e->automation_browser) {
            continue;
        }

        ed_open_pattern(e, i, false);
        ed_bind_pattern_channel(e);
        return;
    }

    if (direction < 0) {
        return;
    }

    ed_checkpoint(e);
    int id = dm_pattern_add(&e->project, NULL);

    if (id < 0) {
        return;
    }

    if (e->automation_browser) {
        e->project.patterns[id].kind = 1;
        e->project.patterns[id].length = dm_bar(&e->project);
        char name[64];
        snprintf(name, sizeof(name), DM_TEXT_AUTOMATION_NUMBER, id + 1);
        dm_pattern_name(&e->project, id, name);
    }

    ed_open_pattern(e, id, true);
    e->pattern_channel = e->selected_channel;
    e->transient_pattern = id;
    ed_commit(e);
}

static bool action_pattern(Editor *e, int a) {
    switch (a) {
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
        } else {
            ed_duplicate_notes(e);
        }
        break;
    case ACT_QUANTIZE:
        if (e->context == 2 && ed_pattern(e)) {
            ed_checkpoint(e);
            dm_quantize(ed_pattern(e), .25);
            dm_pattern_fit(&e->project, ed_pattern(e));
            ed_commit(e);
        }
        break;
    case ACT_NEXT:
        navigate_pattern(e, 1);
        break;
    case ACT_PREVIOUS:
        navigate_pattern(e, -1);
        break;
    case ACT_NEXT_AUTOMATION:
    case ACT_PREVIOUS_AUTOMATION:
        /* Steps through automation patterns from anywhere, showing the automation list. */
        e->automation_browser = true;
        navigate_pattern(e, a == ACT_NEXT_AUTOMATION ? 1 : -1);
        break;
    case ACT_ITEM_UP:
        ed_transpose(e, 1);
        break;
    case ACT_ITEM_DOWN:
        ed_transpose(e, -1);
        break;
    case ACT_OCTAVE_UP:
        if (e->context == 2) {
            ed_transpose(e, 12);
        }
        break;
    case ACT_OCTAVE_DOWN:
        if (e->context == 2) {
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
        v->scroll_y = fmaxf(0, v->scroll_y - (e->context == 2 ? 20 : 64));
    }

    if (a == ACT_DOWN) {
        v->scroll_y += (e->context == 2 ? 20 : 64);
    }
}

static void action_navigation(Editor *e, int a) {
    View *v = e->context == 2 ? &e->roll_view : &e->playlist_view;

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
        ed_seek(e,
                e->context == 2 && ed_pattern(e) ? ed_pattern_length(e) : dm_duration(&e->project));
        break;
    default:
        break;
    }

    action_scroll(e, a, v);
}

void ed_action_zoom(Editor *e, int a) {
    if (e->context == 2 && ed_pattern(e) && ed_pattern(e)->kind && ed_pattern(e)->mode) {
        return;
    }

    View *v = e->context == 2 ? &e->roll_view : &e->playlist_view;

    if (a == ACT_ZOOM_IN) {
        v->zoom = fminf(8, v->zoom * 1.2f);
    }

    if (a == ACT_ZOOM_OUT) {
        v->zoom = fmaxf(.15f, v->zoom / 1.2f);
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
        e->tool = a - ACT_SELECT;
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

static void capture_binding(Editor *e, SDL_Scancode key, SDL_Keymod mods) {
    if (e->capture >= 0) {
        if (key == SDL_SCANCODE_LSHIFT || key == SDL_SCANCODE_RSHIFT || key == SDL_SCANCODE_LCTRL ||
            key == SDL_SCANCODE_RCTRL || key == SDL_SCANCODE_LALT || key == SDL_SCANCODE_RALT ||
            key == SDL_SCANCODE_LGUI || key == SDL_SCANCODE_RGUI) {
            return;
        }

        if (ed_reserved(key, mods)) {
            ed_message(e, DM_TEXT_KEY_RESERVED_FIXED);
            return;
        }

        for (int i = 0; i < ACT_COUNT; ++i) {
            if (i != e->capture && e->bindings[i].key == key && e->bindings[i].mods == mods) {
                ed_message(e, DM_TEXT_BINDING_ALREADY_USE);
                return;
            }
        }

        e->bindings[e->capture] = (Binding){key, mods};
        e->capture = -1;
        ed_preference_save(e);
        return;
    }
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
    case SDL_SCANCODE_S:
        ed_save(e, shift);
        break;
    case SDL_SCANCODE_E:
        if (e->active) {
            e->export_window = true;
        }
        break;
    case SDL_SCANCODE_Z:
        ed_undo(e, shift);
        break;
    case SDL_SCANCODE_Y:
        ed_undo(e, true);
        break;
    case SDL_SCANCODE_C:
        ed_clipboard(e, 0);
        break;
    case SDL_SCANCODE_X:
        ed_clipboard(e, 1);
        break;
    case SDL_SCANCODE_V:
        ed_clipboard(e, 2);
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

int ed_keyboard_channel(Editor *e) {
    if (e->context == 2) {
        if (!e->preview_override && e->preview_source_channel >= 0) {
            e->preview_program = ed_channel_program(e, e->preview_source_channel);
        }

        int channel = e->preview_program == 128 ? MIDI_DRUM_CHANNEL : 0;

        if (e->preview_route >= 0 && e->preview_route != channel) {
            dm_audio_keyboard_program(e->audio, e->preview_route, -1);
        }

        e->preview_route = channel;
        dm_audio_keyboard_program(e->audio, channel,
                                  e->preview_program == 128 ? 0 : e->preview_program);
        keyboard_mix(e, channel, e->pattern_channel);
        return channel;
    }

    if (e->preview_route >= 0) {
        dm_audio_keyboard_program(e->audio, e->preview_route, -1);
        e->preview_route = -1;
    }

    keyboard_mix(e, e->selected_channel, e->selected_channel);
    return e->selected_channel;
}

static bool piano_record_start(Editor *e) {
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

    if (e->context == 1 && !e->channel_selected) {
        return;
    }

    if (e->piano && e->active && note >= 0 && !mods && !repeat) {
        e->piano_channel = ed_keyboard_channel(e);
        note = ed_keyboard_note(e, key, e->piano_channel);

        if (note < 0 || note > 127) {
            return;
        }

        if (!piano_record_start(e)) {
            return;
        }

        e->held[key] = note;
        e->piano_channel = ed_keyboard_channel(e);
        e->held_channels[key] = e->piano_channel;

        if (e->context == 2 || e->project.channels[e->piano_channel].program >= 0) {
            dm_audio_preview(e->audio, e->piano_channel, note, 100, true);
        }

        e->record_start[note] = e->record && dm_audio_playing(e->audio)
                                    ? dm_seconds_beat(&e->project, dm_audio_position(e->audio))
                                    : -1;
        e->record_origin[note] = ed_recording_origin(e, e->record_start[note]);
    }
}

static bool floating_open(Editor *e) {
    return e->generate_window || e->percussion_window || ed_export_review_open(e) ||
           ed_export_warning_open(e) || e->lane_settings || e->confirm || e->config ||
           e->preferences || e->shortcuts || e->about || e->export_window || e->song_event ||
           e->midi_events || e->instrument_channel >= 0 || e->rename_pattern >= 0;
}

static bool context_action(int a) {
    return a == ACT_NOTE_COLOR || a == ACT_DUPLICATE || a == ACT_QUANTIZE ||
           (a >= ACT_FORWARD && a <= ACT_BOTTOM) || (a >= ACT_UP && a <= ACT_ZOOM_RESET);
}

static bool piano_blocks(Editor *e, int ed_action, SDL_Scancode key, SDL_Keymod mods) {
    if (!e->piano || ed_action == ACT_BRUSH) {
        return false;
    }

    if (ed_action >= ACT_ZOOM_IN && ed_action <= ACT_DELETE) {
        return true;
    }

    if (ed_action == ACT_MUTE || ed_action == ACT_SOLO || ed_action == ACT_RECORD) {
        return true;
    }

    return !mods && ed_piano_key(key) >= 0 && ed_action != ACT_PIANO;
}

static bool dispatch_custom(Editor *e, SDL_Scancode key, SDL_Keymod mods, bool repeat) {
    for (int i = 0; i < ACT_COUNT; ++i) {
        if (piano_blocks(e, i, key, mods)) {
            continue;
        }

        if (e->bindings[i].key != key || e->bindings[i].mods != mods) {
            continue;
        }

        if (context_action(i) && floating_open(e) &&
            !(i == ACT_NOTE_COLOR && ui_workspace_focused())) {
            return true;
        }

        if (repeat && (i < ACT_FORWARD || i > ACT_OCTAVE_DOWN)) {
            return true;
        }

        ed_action(e, i);
        return true;
    }

    return false;
}

void ed_shortcut(Editor *e, SDL_Scancode key, SDL_Keymod raw, bool repeat) {
    SDL_Keymod mods = ed_normalized(raw);

    if (key == SDL_SCANCODE_ESCAPE) {
        escape(e);
        return;
    }

    if (e->capture >= 0) {
        capture_binding(e, key, mods);
        return;
    }

    if (ui_text_active()) {
        return;
    }

    if (key == SDL_SCANCODE_A && mods == SDL_KMOD_CTRL) {
        if (!floating_open(e)) {
            ed_select_all(e);
        }

        return;
    }

    if (dispatch_custom(e, key, mods, repeat)) {
        return;
    }

    if (fixed_binding(e, key, mods, repeat)) {
        return;
    }

    if (key == SDL_SCANCODE_DELETE && !repeat && !floating_open(e)) {
        ed_delete_selection(e);
        return;
    }

    if (!floating_open(e)) {
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
        ed_load(e, path);
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
    }
}

static void dialog_result(Editor *e, const SDL_Event *event) {
    e->dialog_busy = false;
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
    } else if (event->user.code == DIALOG_SAVE) {
        e->pending_action = 0;
    }

    SDL_free(event->user.data1);
    SDL_free(event->user.data2);
}

static bool fixed_global_key(Editor *e, const SDL_Event *event) {
    for (int i = 0; i < ACT_COUNT; ++i) {
        if (e->bindings[i].key == event->key.scancode &&
            e->bindings[i].mods == ed_normalized(event->key.mod)) {
            return false;
        }
    }

    if (event->key.scancode == SDL_SCANCODE_S && (event->key.mod & SDL_KMOD_CTRL) &&
        !event->key.repeat && e->capture < 0) {
        e->pending_save = true;
        e->pending_save_as = (event->key.mod & SDL_KMOD_SHIFT) != 0;
        return true;
    }

    if (event->key.scancode == SDL_SCANCODE_E && ed_normalized(event->key.mod) == SDL_KMOD_CTRL &&
        e->capture < 0) {
        if (e->active) {
            e->export_window = true;
        }

        return true;
    }

    return false;
}

static bool instrument_topmost(Editor *e) {
    if (e->instrument_channel < 0 || ui_popup_open()) {
        return false;
    }

    const char *titles[] = {DM_TEXT_UNSAVED_PROJECT,
                            DM_TEXT_SONG_EVENT,
                            DM_TEXT_EXPORT,
                            DM_TEXT_KEYBOARD_SHORTCUTS,
                            DM_TEXT_PREFERENCES,
                            DM_TEXT_PROJECT_SETTINGS,
                            DM_TEXT_NEW_PROJECT,
                            DM_TEXT_ABOUT_DUCKMIDI,
                            DM_TEXT_RENAME_PATTERN,
                            DM_TEXT_INSTRUMENT,
                            DM_TEXT_MIDI_EVENTS,
                            DM_TEXT_AUTOMATION_LANE,
                            DM_TEXT_GENERATE_PATTERNS,
                            DM_TEXT_PERCUSSION_KEYBOARD_MAPPING,
                            DM_TEXT_EXPORT_WARNINGS,
                            DM_TEXT_REVIEW_EXPORT_MAPPINGS};
    return ui_topmost(titles, 16) == 9;
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
    } else if (e->preferences && ed_inside(at, e->preferences_rect)) {
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

    if (event->type == SDL_EVENT_DROP_BEGIN) {
        e->drop_active = true;
        e->drop_handled = false;
        e->drop_position = (UiPoint){-1, -1};
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
    int c = e->instrument_channel, route = c;
    int base = ed_piano_key(key->scancode),
        note = ed_keyboard_note(e, key->scancode, c == MIDI_DRUM_CHANNEL ? MIDI_DRUM_CHANNEL : 0);

    if (base < 0 || key->repeat || note < 0 || note > 127) {
        return false;
    }

    if (e->preview_route >= 0 && e->preview_route != route) {
        dm_audio_keyboard_program(e->audio, e->preview_route, -1);
    }

    e->held[key->scancode] = note;
    e->record_start[note] = -1;
    e->held_channels[key->scancode] = route;
    dm_audio_keyboard_program(e->audio, route, (int)fmax(0, e->project.channels[c].program));
    dm_audio_preview(e->audio, route, note, 100, true);
    e->preview_route = route;
    return true;
}

static bool instrument_key(Editor *e, const SDL_KeyboardEvent *key) {
    if (!instrument_topmost(e) || ed_normalized(key->mod)) {
        return false;
    }

    if (key->scancode == SDL_SCANCODE_RETURN || key->scancode == SDL_SCANCODE_KP_ENTER) {
        ed_release_piano(e);
        e->instrument_accept = true;
        return true;
    }

    if (ui_text_active()) {
        return false;
    }

    if (key->scancode == SDL_SCANCODE_UP || key->scancode == SDL_SCANCODE_DOWN) {
        int c = e->instrument_channel;
        int program = (int)fmax(0, fmin(127, e->project.channels[c].program +
                                                 (key->scancode == SDL_SCANCODE_DOWN ? 1 : -1)));
        ed_choose_program(e, c, program);
        return true;
    }

    return ed_instrument_note(e, key);
}

void editor_event(Editor *e, const SDL_Event *event) {
    if (event->type == SDL_EVENT_KEY_DOWN && ed_percussion_key(e, &event->key)) {
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

    if (event->type == SDL_EVENT_KEY_DOWN && instrument_key(e, &event->key)) {
        return;
    }

    if (event->type == SDL_EVENT_KEY_DOWN) {
        if (fixed_global_key(e, event)) {
            return;
        }

        e->keypress = event->key.scancode;
        e->keymods = event->key.mod;
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
