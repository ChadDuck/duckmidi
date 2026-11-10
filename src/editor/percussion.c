#include "editor/internal.h"
#include "text/en.h"
/* Stable project key indices, independent of SDL's binary representation. */
static const SDL_Scancode piano_keys[DM_PIANO_KEYS] = {
    SDL_SCANCODE_Z,           SDL_SCANCODE_S, SDL_SCANCODE_X,           SDL_SCANCODE_D,
    SDL_SCANCODE_C,           SDL_SCANCODE_V, SDL_SCANCODE_G,           SDL_SCANCODE_B,
    SDL_SCANCODE_H,           SDL_SCANCODE_N, SDL_SCANCODE_J,           SDL_SCANCODE_M,
    SDL_SCANCODE_COMMA,       SDL_SCANCODE_L, SDL_SCANCODE_PERIOD,      SDL_SCANCODE_SEMICOLON,
    SDL_SCANCODE_SLASH,       SDL_SCANCODE_Q, SDL_SCANCODE_2,           SDL_SCANCODE_W,
    SDL_SCANCODE_3,           SDL_SCANCODE_E, SDL_SCANCODE_R,           SDL_SCANCODE_5,
    SDL_SCANCODE_T,           SDL_SCANCODE_6, SDL_SCANCODE_Y,           SDL_SCANCODE_7,
    SDL_SCANCODE_U,           SDL_SCANCODE_I, SDL_SCANCODE_9,           SDL_SCANCODE_O,
    SDL_SCANCODE_0,           SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_EQUALS,
    SDL_SCANCODE_RIGHTBRACKET};

int ed_piano_index(SDL_Scancode key) {
    for (int i = 0; i < DM_PIANO_KEYS; i++) {
        if (piano_keys[i] == key) {
            return i;
        }
    }

    return -1;
}

SDL_Scancode ed_piano_scancode(int index) {
    return index >= 0 && index < DM_PIANO_KEYS ? piano_keys[index] : SDL_SCANCODE_UNKNOWN;
}

int ed_piano_key(SDL_Scancode key) {
    int i = ed_piano_index(key);
    return i < 0 ? -1 : i < 17 ? 48 + i : 60 + i - 17;
}

/* The project's own mappings when it overrides the global ones. */
static const uint8_t *active_keys(const Editor *e) {
    return e->project.percussion_override ? e->project.percussion_keys : e->percussion_global;
}

static const uint8_t *active_midi(const Editor *e) {
    return e->project.percussion_override ? e->project.percussion_midi : e->percussion_midi_global;
}

static void load_drafts(Editor *e) {
    memcpy(e->percussion_draft, active_keys(e), 128);
    memcpy(e->percussion_midi_draft, active_midi(e), 128);
}

static bool drafts_saved(const Editor *e) {
    return !memcmp(active_keys(e), e->percussion_draft, 128) &&
           !memcmp(active_midi(e), e->percussion_midi_draft, 128);
}

static void stop_listening(Editor *e) {
    e->percussion_listening = e->percussion_midi_listening = false;
}

/* The pattern editor previews drums on the keyboard synth's channel 10; elsewhere the route is
   the project channel, which plays drums when it is marked as percussion. */
bool ed_route_drums(const Editor *e, int route) {
    return e->context == 2 ? route == MIDI_DRUM_CHANNEL : dm_channel_drums(&e->project, route);
}

int ed_keyboard_note(Editor *e, SDL_Scancode key, bool drums) {
    int index = ed_piano_index(key);

    if (index < 0) {
        return -1;
    }

    if (drums) {
        const uint8_t *mapping = active_keys(e);

        for (int note = 0; note < 128; note++) {
            if (mapping[note] == index + 1) {
                return note;
            }
        }
    }

    return ed_piano_key(key) + e->piano_octave * 12;
}

/* An incoming note assigned to a drum plays that drum; unassigned notes play as themselves. */
static void midi_map(const uint8_t *midi, uint8_t map[MIDI_KEYS]) {
    for (unsigned note = 0; note < MIDI_KEYS; ++note) {
        map[note] = (uint8_t)note;
    }

    for (unsigned drum = 0; drum < MIDI_KEYS; ++drum) {
        if (midi[drum]) {
            map[midi[drum] - 1] = (uint8_t)drum;
        }
    }
}

void ed_percussion_midi_map(const Editor *e, uint8_t map[MIDI_KEYS]) {
    midi_map(active_midi(e), map);
}

void ed_percussion_midi_draft_map(const Editor *e, uint8_t map[MIDI_KEYS]) {
    midi_map(e->percussion_midi_draft, map);
}

static void midi_source(char *out, size_t cap, int note) {
    char name[16];
    ed_pitch_text(name, sizeof(name), (float)note);
    snprintf(out, cap, DM_TEXT_PERCUSSION_MIDI_SOURCE, name, note);
}

/* While Listen for MIDI is on, the next Note On assigns the selected drum and is not recorded.
   Other messages leave listening on and continue to their usual destination. */
bool ed_percussion_midi_listen(Editor *e, DmMidiInputEvent event) {
    unsigned type = event.status & MIDI_STATUS_MASK;

    if (!e->percussion_window || !e->percussion_midi_listening ||
        (event.flags & (DM_MIDI_EVENT_RELEASE | DM_MIDI_EVENT_BOUND)) || type == MIDI_NOTE_OFF) {
        return false;
    }

    if (type != MIDI_NOTE_ON) {
        snprintf(e->percussion_feedback, sizeof(e->percussion_feedback), DM_TEXT_USE_KEY_OR_PAD);
        return false;
    }

    for (unsigned i = 0; i < 128; i++) {
        if (e->percussion_midi_draft[i] == event.a + 1) {
            e->percussion_midi_draft[i] = 0;
        }
    }

    e->percussion_midi_draft[e->percussion_note] = (uint8_t)(event.a + 1);
    e->percussion_midi_listening = false;
    char source[48];
    midi_source(source, sizeof(source), event.a);
    snprintf(e->percussion_feedback, sizeof(e->percussion_feedback),
             DM_TEXT_ASSIGNED_CHOOSE_WHERE_SAVE, source);
    return true;
}

void ed_percussion_open(Editor *e) {
    ed_release_piano(e);
    e->percussion_window = true;
    stop_listening(e);
    e->percussion_reveal = true;
    load_drafts(e);
    e->percussion_feedback[0] = 0;
}

/* While the window is open, piano keys play the drums of the mapping being edited, as the
   instrument window plays its instrument. */
static bool percussion_preview(Editor *e, const SDL_KeyboardEvent *key) {
    int index = ed_piano_index(key->scancode);

    if (index < 0 || ed_normalized(key->mod)) {
        return false;
    }

    if (key->repeat) {
        return true;
    }

    for (int note = 0; note < 128; note++) {
        if (e->percussion_draft[note] != index + 1) {
            continue;
        }

        if (e->preview_route >= 0 && e->preview_route != MIDI_DRUM_CHANNEL) {
            dm_audio_keyboard_program(e->audio, e->preview_route, -1, false);
        }

        e->held[key->scancode] = note;
        e->record_start[note] = -1;
        e->held_channels[key->scancode] = MIDI_DRUM_CHANNEL;
        dm_audio_keyboard_program(e->audio, MIDI_DRUM_CHANNEL, 0, true);
        dm_audio_preview(e->audio, MIDI_DRUM_CHANNEL, note, 100, true);
        e->preview_route = MIDI_DRUM_CHANNEL;
        break;
    }

    return true;
}

bool ed_percussion_key(Editor *e, const SDL_KeyboardEvent *key) {
    if (!e->percussion_window) {
        return false;
    }

    if (key->scancode == SDL_SCANCODE_ESCAPE &&
        (e->percussion_listening || e->percussion_midi_listening)) {
        stop_listening(e);
        e->percussion_feedback[0] = 0;
        return true;
    }

    if (!e->percussion_listening && !ed_normalized(key->mod) && !key->repeat &&
        (key->scancode == SDL_SCANCODE_RETURN || key->scancode == SDL_SCANCODE_KP_ENTER)) {
        ed_percussion_accept(e);
        return true;
    }

    if (!e->percussion_listening) {
        return percussion_preview(e, key);
    }

    if (key->repeat) {
        return true;
    }

    int index = ed_piano_index(key->scancode);

    if (index < 0 || ed_normalized(key->mod)) {
        snprintf(e->percussion_feedback, sizeof(e->percussion_feedback),
                 DM_TEXT_PRESS_PIANO_KEY_WITHOUT);
        return true;
    }

    for (unsigned i = 0; i < 128; i++) {
        if (e->percussion_draft[i] == index + 1) {
            e->percussion_draft[i] = 0;
        }
    }

    e->percussion_draft[e->percussion_note] = (uint8_t)(index + 1);
    e->percussion_listening = false;
    char source[64];
    snprintf(source, sizeof(source), DM_TEXT_PERCUSSION_KEY_SOURCE,
             SDL_GetScancodeName(key->scancode));
    snprintf(e->percussion_feedback, sizeof(e->percussion_feedback),
             DM_TEXT_ASSIGNED_CHOOSE_WHERE_SAVE, source);
    return true;
}

void ed_percussion_save(Editor *e, bool project) {
    ed_release_piano(e);
    stop_listening(e);

    if (project) {
        ed_checkpoint(e);
        e->project.percussion_override = true;
        memcpy(e->project.percussion_keys, e->percussion_draft, 128);
        memcpy(e->project.percussion_midi, e->percussion_midi_draft, 128);
        ed_commit(e);
        snprintf(e->percussion_feedback, sizeof(e->percussion_feedback),
                 DM_TEXT_SAVED_THIS_PROJECT_SAVE);
    } else {
        uint8_t keys[128], midi[128];
        memcpy(keys, e->percussion_global, 128);
        memcpy(midi, e->percussion_midi_global, 128);
        memcpy(e->percussion_global, e->percussion_draft, 128);
        memcpy(e->percussion_midi_global, e->percussion_midi_draft, 128);

        if (!dm_preferences_save(e, ed_shortcut_schema(), ed_action_names)) {
            memcpy(e->percussion_global, keys, 128);
            memcpy(e->percussion_midi_global, midi, 128);
            snprintf(e->percussion_feedback, sizeof(e->percussion_feedback),
                     DM_TEXT_COULD_NOT_SAVE_GLOBAL);
            return;
        }

        snprintf(e->percussion_feedback, sizeof(e->percussion_feedback), "%s",
                 e->project.percussion_override ? DM_TEXT_SAVED_GLOBALLY_THIS_PROJECT
                                                : DM_TEXT_SAVED_GLOBAL_PREFERENCES);
    }
}

/* Enter saves the drafts where this project's mappings come from (its own mappings, or the global
   preferences) and closes the window, as Apply does in the instrument window. */
void ed_percussion_accept(Editor *e) {
    if (!drafts_saved(e)) {
        ed_percussion_save(e, e->project.percussion_override);

        if (!drafts_saved(e)) {
            return; /* The preferences could not be saved; the window shows why. */
        }
    }

    ed_percussion_close(e);
}

/* Closes the window and forgets its unsaved drafts, as when a project is opened or created. */
void ed_percussion_close(Editor *e) {
    ed_release_piano(e);
    e->percussion_window = false;
    stop_listening(e);
    load_drafts(e);
    e->percussion_feedback[0] = 0;
}

/* "36: Bass Drum 1  ->  Key Z, MIDI C2 (36)" */
static void assigned_mapping(const Editor *e, unsigned note) {
    char key[64] = "", midi[48] = "", label[240];

    if (e->percussion_draft[note]) {
        snprintf(key, sizeof(key), DM_TEXT_PERCUSSION_KEY_SOURCE,
                 SDL_GetScancodeName(ed_piano_scancode(e->percussion_draft[note] - 1)));
    }

    if (e->percussion_midi_draft[note]) {
        midi_source(midi, sizeof(midi), e->percussion_midi_draft[note] - 1);
    }

    snprintf(label, sizeof(label), DM_TEXT_PERCUSSION_ASSIGNED, note, dm_drum_name((int)note), key,
             *key && *midi ? ", " : "", midi);
    ui_wrap(label);
}

static void listen_buttons(Editor *e) {
    if (ui_button(e->percussion_listening ? DM_TEXT_LISTENING : DM_TEXT_LISTEN_KEY,
                  ui_available().x, 0)) {
        stop_listening(e);
        e->percussion_listening = true;
        e->percussion_feedback[0] = 0;
    }

    ui_disabled(!e->midi_input);

    if (ui_button(e->percussion_midi_listening ? DM_TEXT_LISTENING : DM_TEXT_LISTEN_MIDI,
                  ui_available().x, 0)) {
        stop_listening(e);
        ed_midi_bindings_cancel(e);
        e->percussion_midi_listening = true;
        e->percussion_feedback[0] = 0;
    }

    ui_disabled_end();
}

void ed_percussion_dialog(Editor *e) {
    if (!e->percussion_window) {
        stop_listening(e);
        return;
    }

    /* The window's height follows its contents: fixed-height lists, then the buttons. */
    if (ui_fitted_dialog(DM_TEXT_PERCUSSION_KEYBOARD_MAPPING, &e->percussion_window, 780)) {
        ui_wrap(e->project.percussion_override ? DM_TEXT_THIS_PROJECT_USES_ITS
                                               : DM_TEXT_THIS_PROJECT_USES_GLOBAL);
        float width = ui_available().x, left = width * .42f, height = 380;

        if (ui_child("percussion-sources", left, height, true)) {
            float origin = ui_cursor().y;

            for (int note = 0; note < 128; note++) {
                if (e->percussion_reveal && note == e->percussion_note) {
                    ui_scroll_to(fmaxf(0, ui_cursor().y - origin - 50));
                }

                char label[160];
                snprintf(label, sizeof(label), "%d: %s", note, dm_drum_name(note));
                ui_id(note);

                if (ui_select(label, e->percussion_note == note, 0, 0)) {
                    e->percussion_note = note;
                    stop_listening(e);
                    dm_audio_keyboard_program(e->audio, MIDI_DRUM_CHANNEL, 0, true);
                    dm_audio_audition(e->audio, 9, note, 100, .25);
                }

                ui_pop_id();
            }

            e->percussion_reveal = false;
        }

        ui_child_end();
        ui_same();

        if (ui_child("percussion-keys", width - left - ui_item_spacing_x(), height, true)) {
            ui_wrap(dm_drum_name(e->percussion_note));
            listen_buttons(e);

            if (*e->percussion_feedback) {
                ui_wrap(e->percussion_feedback);
            }

            ui_separator();
            ui_text(DM_TEXT_ASSIGNED_MAPPINGS);

            for (unsigned note = 0; note < 128; note++) {
                if (e->percussion_draft[note] || e->percussion_midi_draft[note]) {
                    assigned_mapping(e, note);
                }
            }

            if (ui_button(DM_TEXT_RESET_MAPPINGS, ui_available().x, 0)) {
                memset(e->percussion_draft, 0, 128);
                memset(e->percussion_midi_draft, 0, 128);
                stop_listening(e);
                snprintf(e->percussion_feedback, sizeof(e->percussion_feedback),
                         DM_TEXT_DEFAULT_PIANO_BEHAVIOR_RESTORED);
            }
        }

        ui_child_end();
        float button = (ui_available().x - ui_item_spacing_x()) * .5f;

        if (ui_button(DM_TEXT_SAVE_CURRENT_PROJECT, button, 0)) {
            ed_percussion_save(e, true);
        }

        ui_same();

        if (ui_button(DM_TEXT_SAVE_PREFERENCES_GLOBAL, button, 0)) {
            ed_percussion_save(e, false);
        }

        if (e->project.percussion_override &&
            ui_button(DM_TEXT_USE_GLOBAL_MAPPING_THIS, ui_available().x, 0)) {
            ed_release_piano(e);
            ed_checkpoint(e);
            e->project.percussion_override = false;
            memset(e->project.percussion_keys, 0, 128);
            memset(e->project.percussion_midi, 0, 128);
            ed_commit(e);
            ed_percussion_open(e);
        }
    }

    ui_end();

    if (!e->percussion_window) {
        stop_listening(e);
    }
}
