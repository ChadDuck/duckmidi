#include "editor_internal.h"
#include "text/en.h"

#define STATUS_DURATION_MS 5000 /* How long a status message stays in the status bar. */

static void record_button(Editor *e, bool playing);
static void tempo_control(Editor *e);
static void duration_control(Editor *e);
static void playback_selector(Editor *e);
static void file_menu(Editor *e);
static void edit_menu(Editor *e);
static void ui_size_menu(Editor *e);
static void view_menu(Editor *e);
static void help_menu(Editor *e);

uint32_t ed_track_color(int channel) {
    if (channel == MIDI_DRUM_CHANNEL) {
        return COLOR(231, 211, 101);
    }

    static const uint32_t colors[] = {
        COLOR(83, 193, 156),  COLOR(102, 169, 224), COLOR(191, 144, 223), COLOR(235, 175, 108),
        COLOR(118, 201, 206), COLOR(221, 132, 154), COLOR(160, 193, 109), COLOR(167, 160, 232)};
    return colors[channel % 8];
}

bool ed_inside(UiPoint p, UiRect r) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

float ed_clampf(float n, float lo, float hi) {
    return fmaxf(lo, fminf(hi, n));
}

double ed_snap(double beat, double grid, bool free) {
    return fmax(0, free ? beat : round(beat / grid) * grid);
}

void ed_hover(Editor *e, const char *text) {
    snprintf(e->hover, sizeof(e->hover), "%s", text);
}

void ed_info_box(Editor *e) {
    UiRect r = e->info_rect;

    if (r.w < 40) {
        return;
    }

    /* Status messages show for a few seconds after they are posted; hover help shows while the
       pointer stays on its item. Text written straight into e->status counts as posted when it
       changes. */
    uint64_t now = SDL_GetTicks();

    if (strcmp(e->status_shown, e->status)) {
        snprintf(e->status_shown, sizeof(e->status_shown), "%s", e->status);
        e->status_time = now;
    }

    bool recent = *e->status && now - e->status_time < STATUS_DURATION_MS;
    const char *text = *e->hover ? e->hover : recent ? e->status : "";

    if (*text) {
        ui_label_fit((UiPoint){r.x + 12, r.y + (r.h - ui_font_size()) * .5f}, r.w - 24,
                     *e->hover ? TEXT : DIM, text);
    }
}

void ed_new_song_event(Editor *e) {
    e->midi_events = false;
    e->event_edit = e->event_auto_edit = -1;
    e->event_source_kind = e->event_kind = 0;
    e->event_beat = (float)e->cursor_beat[0];
    e->event_bpm = (float)e->project.bpm;
    e->event_channel = e->selected_channel;
    e->event_numerator = e->project.numerator;
    e->event_denominator = e->project.denominator;
    e->song_event = true;
}

const char *ed_binding_text(Editor *e, Action action, char *out, size_t capacity) {
    Binding binding = e->bindings[action];
    snprintf(out, capacity, "%s%s%s%s%s", binding.mods & SDL_KMOD_CTRL ? DM_TEXT_CTRL_PLUS : "",
             binding.mods & SDL_KMOD_SHIFT ? DM_TEXT_SHIFT_PLUS : "",
             binding.mods & SDL_KMOD_ALT ? DM_TEXT_ALT_PLUS : "",
             binding.mods & SDL_KMOD_GUI ? DM_TEXT_WIN_CMD_PLUS : "",
             SDL_GetScancodeName(binding.key));
    return out;
}

/* Short button help goes to the status bar info text; tooltips are kept for longer
   explanations, so the same text never shows twice. */
void ed_action_tip(Editor *e, const char *name, Action ed_action) {
    if (!ui_hover()) {
        return;
    }

    char binding[96], text[256];
    snprintf(text, sizeof(text), "%s (%s)", name,
             ed_binding_text(e, ed_action, binding, sizeof(binding)));
    ed_hover(e, text);
}

static void record_button(Editor *e, bool playing) {
    (void)playing;
    ui_accent(e->record);

    if (ui_icon_button("##Record", e->record ? UI_RECORD_ON : UI_RECORD, 74, 32)) {
        ed_toggle_record(e);
    }

    ui_accent_end();
    ed_action_tip(e, e->record ? DM_TEXT_STOP_RECORDING : DM_TEXT_RECORD, ACT_RECORD_GLOBAL);
}

static void tempo_control(Editor *e) {
    float bpm = (float)e->project.bpm;

    if (ui_float(DM_TEXT_BPM, &bpm, DM_BPM_MIN, DM_BPM_MAX) && e->active) {
        ed_checkpoint(e);
        e->project.bpm = bpm;
        ed_commit(e);
    }

    bool variable = false;

    for (size_t i = 0; i < e->project.event_count; ++i) {
        DmBytes *m = &e->project.events[i].message;

        if (m->size >= 2 && m->data[0] == MIDI_META && m->data[1] == MIDI_META_TEMPO &&
            e->project.events[i].beat > 0) {
            variable = true;
        }
    }

    if (variable) {
        ui_tip(DM_TEXT_MIDI_HAS_TEMPO);
    }
}

static void duration_control(Editor *e) {
    char text[128];
    double pos = dm_audio_position(e->audio),
           duration = dm_beat_seconds(&e->project, ed_transport_length(e));

    if (e->duration_beats) {
        snprintf(text, sizeof(text), DM_TEXT_BEATS,
                 fmin(ed_transport_length(e), dm_seconds_beat(&e->project, pos) + 1),
                 ed_transport_length(e));
    } else {
        snprintf(text, sizeof(text), "%02d:%05.2f / %02d:%05.2f", (int)pos / 60, fmod(pos, 60),
                 (int)duration / 60, fmod(duration, 60));
    }

    ui_text(text);

    if (ui_hover() && ui_mouse().clicked) {
        e->duration_beats = !e->duration_beats;
    }
}

static void playback_selector(Editor *e) {
    ui_width(100);

    if (ui_bar_combo("##Playback mode", e->pattern_mode ? DM_TEXT_PATTERN : DM_TEXT_SONG)) {
        if (ui_select(DM_TEXT_SONG, !e->pattern_mode, 0, 0) && e->pattern_mode) {
            ed_action(e, ACT_PLAY_MODE);
        }

        ui_disabled(!ed_pattern(e));

        if (ui_select(DM_TEXT_PATTERN, e->pattern_mode, 0, 0) && !e->pattern_mode) {
            ed_action(e, ACT_PLAY_MODE);
        }

        ui_disabled_end();
        ui_combo_end();
    }
}

void ed_toolbar(Editor *e) {
    ui_bar_begin();
    UiPoint start = ui_cursor();
    float full_width = ui_available().x;
    ui_disabled(!e->active);
    bool playing = dm_audio_playing(e->audio);

    if (ui_icon_button("##Play", playing || e->wait_armed ? UI_PAUSE : UI_PLAY, 58, 32)) {
        ed_play(e);
    }

    ed_action_tip(e, playing || e->wait_armed ? DM_TEXT_PAUSE : DM_TEXT_PLAY, ACT_PLAY);
    ui_same();

    if (ui_icon_button("##Stop", UI_STOP, 52, 32)) {
        e->wait_armed = false;
        ed_release_piano(e);
        dm_audio_transport(e->audio, false);
        e->record = false;
        e->recording_take = e->take_snapshot = false;
        dm_audio_recording(e->audio, false);
        ed_seek(e, 0);
        ed_release_piano(e);

        if (ui_mouse().shift) {
            ed_stop_edit_feedback(e);
            dm_audio_panic(e->audio);
            ed_message(e, DM_TEXT_MIDI_PANIC_SENT);
        }
    }

    if (ui_hover()) {
        ed_hover(e, DM_TEXT_STOP_PANIC_HELP);
    }

    ui_same();
    record_button(e, playing);
    ui_same();
    ui_align_frame();
    duration_control(e);
    ui_same();
    tempo_control(e);
    ui_disabled_end();
    ui_same();
    UiPoint left_end = ui_cursor();
    const char *toggles[] = {DM_TEXT_KEYS, DM_TEXT_CLICK, DM_TEXT_WAIT, DM_TEXT_OVERLAP,
                             DM_TEXT_PIANO_ROLL_TITLE};
    float x = start.x +
              fmaxf(0, full_width - (4 * 42 + 4 * 8 + ui_button_row_width(toggles + 4, 1)) - 108);

    /* The info box sits between the transport and toggle groups. Narrow windows move the toggles
       to a second row, so the box fills the space to their left. */
    float info_left = full_width < 900 ? start.x : left_end.x + 8;
    float info_top = start.y + (full_width < 900 ? 40 : 0);
    e->info_rect = (UiRect){info_left, info_top, fmaxf(0, x - 12 - info_left), 32};
    ui_newline();
    ui_position(x, start.y + (full_width < 900 ? 40 : 0));
    playback_selector(e);
    ui_same();
    ui_accent(e->piano);

    if (ui_icon_button("##Keys", UI_PIANO, 42, 32)) {
        e->piano = !e->piano;
        ed_release_piano(e);
    }

    ui_accent_end();
    ed_action_tip(e, DM_TEXT_KEYS, ACT_PIANO);
    ui_same();
    ui_accent(e->metronome);

    if (ui_icon_button("##Click", UI_TIMER, 42, 32)) {
        e->metronome = !e->metronome;
        dm_audio_metronome(e->audio, e->metronome, e->project.bpm);
    }

    ui_accent_end();
    ed_action_tip(e, DM_TEXT_CLICK_METRONOME, ACT_METRONOME);
    ui_same();
    ui_accent(e->wait_input);

    if (ui_icon_button("##Wait", UI_HOURGLASS, 42, 32)) {
        e->wait_input = !e->wait_input;
        ed_cancel_wait(e);
    }

    ui_accent_end();
    ed_action_tip(e, DM_TEXT_WAIT, ACT_WAIT);
    ui_same();
    ui_accent(e->overwrite);

    if (ui_icon_button("##Overlap", UI_OVERLAP, 42, 32)) {
        e->overwrite = !e->overwrite;
    }

    ui_accent_end();
    ed_action_tip(e, DM_TEXT_OVERLAP, ACT_OVERLAP);
    ui_same();
    bool roll_before = e->roll;

    if (ui_toggle(DM_TEXT_PIANO_ROLL_TITLE, &e->roll)) {
        e->roll = roll_before;
        ed_toggle_roll(e);
    }

    ui_newline();
    ui_position(start.x, start.y + (full_width < 900 ? 80 : 40));
    ui_space(0);
    ui_bar_end();
}

static void file_menu(Editor *e) {
    if (ui_menu(DM_TEXT_FILE)) {
        if (ui_menu_item(DM_TEXT_NEW_MIDI, NULL, true)) {
            ed_request(e, PENDING_NEW, NULL);
        }

        if (ui_menu_item(DM_TEXT_LOAD_MIDI_MUS, NULL, true)) {
            ed_request(e, PENDING_OPEN, NULL);
        }

        if (ui_menu(DM_TEXT_RECENT)) {
            for (int i = 0; i < e->recent_count; ++i) {
                const char *name = strrchr(e->recent[i], '/'), *back = strrchr(e->recent[i], '\\');

                if (!name || (back && back > name)) {
                    name = back;
                }

                name = name ? name + 1 : e->recent[i];
                char label[48];
                snprintf(label, sizeof(label), "%.24s%s", name, strlen(name) > 24 ? "..." : "");
                ui_id(i);

                if (ui_menu_item(label, NULL, true)) {
                    ed_request(e, PENDING_PATH, e->recent[i]);
                }

                ui_tip(e->recent[i]);
                ui_pop_id();
            }

            ui_menu_end();
        }

        if (ui_menu_item(DM_TEXT_SAVE, DM_TEXT_CTRL_S, e->active)) {
            ed_save(e, false);
        }

        if (ui_menu_item(DM_TEXT_SAVE_AS_MENU, DM_TEXT_CTRL_SHIFT, e->active)) {
            ed_save(e, true);
        }

        if (ed_save_busy(e) && ui_menu_item(DM_TEXT_CANCEL_PROJECT_SAVE, NULL, true)) {
            ed_save_cancel(e);
        }

        if (ui_menu_item(DM_TEXT_EXPORT_MENU, DM_TEXT_CTRL_E, e->active)) {
            e->export_window = true;
        }

        ui_separator();

        if (ui_menu_item(DM_TEXT_EXIT, NULL, true)) {
            ed_request(e, PENDING_QUIT, NULL);
        }

        ui_menu_end();
    }
}

static void edit_menu(Editor *e) {
    char key[64];

    if (ui_menu(DM_TEXT_EDIT)) {
        if (ui_menu_item(DM_TEXT_UNDO, DM_TEXT_CTRL_Z, e->undo_count > 0)) {
            ed_undo(e, false);
        }

        if (ui_menu_item(DM_TEXT_REDO, DM_TEXT_CTRL_Y, e->redo_count > 0)) {
            ed_undo(e, true);
        }

        ui_separator();

        if (ui_menu_item(DM_TEXT_CUT, DM_TEXT_CTRL_X, e->active)) {
            ed_clipboard(e, 1);
        }

        if (ui_menu_item(DM_TEXT_COPY, DM_TEXT_CTRL_C, e->active)) {
            ed_clipboard(e, 0);
        }

        if (ui_menu_item(DM_TEXT_PASTE, DM_TEXT_CTRL_V, e->clipboard.size > 0)) {
            ed_clipboard(e, 2);
        }

        ui_separator();

        if (ui_menu_item(DM_TEXT_PROJECT_SETTINGS, ed_binding_text(e, ACT_CONFIG, key, sizeof(key)),
                         e->active)) {
            e->config = true;
        }

        if (ui_menu_item(DM_TEXT_GENERATE_PATTERNS, NULL, e->active)) {
            e->generate_window = true;
        }

        if (ui_menu_item(DM_TEXT_MIDI_EVENTS, NULL, e->active)) {
            e->midi_events = true;
            e->song_event = false;
            e->midi_event_index = -1;
        }

        if (ui_menu_item(DM_TEXT_KEYBOARD_SHORTCUTS,
                         ed_binding_text(e, ACT_SHORTCUTS, key, sizeof(key)), true)) {
            e->shortcuts = true;
        }

        if (ui_menu_item(DM_TEXT_PREFERENCES, ed_binding_text(e, ACT_PREFERENCES, key, sizeof(key)),
                         true)) {
            e->preferences = true;
        }

        ui_menu_end();
    }
}

static void ui_size_menu(Editor *e) {
    if (!ui_menu(DM_TEXT_UI_SIZE)) {
        return;
    }

    const char *labels[] = {DM_TEXT_SMALL_0_85X, DM_TEXT_MEDIUM_1X, DM_TEXT_LARGE_1_25X};

    for (int i = 0; i < 3; ++i) {
        char label[64];
        snprintf(label, sizeof(label), "%s%s", i == e->ui_size ? "* " : "", labels[i]);

        if (ui_menu_item(label, NULL, true)) {
            e->ui_size = i;
            ed_preference_save(e);
        }
    }

    ui_menu_end();
}

static void view_menu(Editor *e) {
    char key[64];

    if (ui_menu(DM_TEXT_VIEW)) {
        ui_size_menu(e);

        if (ui_menu_item(DM_TEXT_SONG_EVENTS, NULL, e->active)) {
            ed_new_song_event(e);
        }

        if (ui_menu_item(DM_TEXT_PIANO_ROLL_TITLE, ed_binding_text(e, ACT_EDITOR, key, sizeof(key)),
                         true)) {
            ed_toggle_roll(e);
        }

        if (ui_menu_item(DM_TEXT_COMPUTER_KEYBOARD_PIANO,
                         ed_binding_text(e, ACT_PIANO, key, sizeof(key)), true)) {
            ed_release_piano(e);
            e->piano = !e->piano;
        }

        if (ui_menu_item(DM_TEXT_RESET_LAYOUT, NULL, true)) {
            ed_reset_layout(e);
        }

        ui_menu_end();
    }
}

static void help_menu(Editor *e) {
    if (ui_menu(DM_TEXT_HELP)) {
        if (ui_menu_item(DM_TEXT_CHECK_FOR_UPDATE, NULL, true)) {
            ed_update_open(e);
        }

        if (ui_menu_item(DM_TEXT_ABOUT_DUCKMIDI, NULL, true)) {
            e->about = true;
        }

        ui_menu_end();
    }
}

void ed_menu(Editor *e) {
    if (!ui_menu_bar()) {
        return;
    }

    file_menu(e);
    edit_menu(e);
    view_menu(e);
    help_menu(e);
    ui_menu_bar_end();
}
