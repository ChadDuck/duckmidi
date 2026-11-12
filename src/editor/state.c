#include "editor/internal.h"
#include "text/en.h"

/* Every rebindable action: its name (also its key in the preferences file), default shortcut,
   group in Keyboard Shortcuts and dispatch rules. */
const ActionInfo ed_actions[ACT_COUNT] = {
    [ACT_PIANO] = {DM_TEXT_COMPUTER_KEYBOARD_PIANO, SDL_SCANCODE_F1, 0, ACTIONS_TRANSPORT, 0},
    [ACT_QUANTIZE] = {DM_TEXT_QUANTIZE, SDL_SCANCODE_Q, SDL_KMOD_CTRL, ACTIONS_PATTERNS,
                      ACTION_WORKSPACE},
    [ACT_PLAY] = {DM_TEXT_PLAY_PAUSE, SDL_SCANCODE_SPACE, 0, ACTIONS_TRANSPORT, 0},
    [ACT_FORWARD] = {DM_TEXT_FORWARD_ONE_SECOND, SDL_SCANCODE_RIGHT, 0, ACTIONS_NAVIGATION,
                     ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_BACK] = {DM_TEXT_BACK_ONE_SECOND, SDL_SCANCODE_LEFT, 0, ACTIONS_NAVIGATION,
                  ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_FORWARD_BEAT] = {DM_TEXT_FORWARD_QUARTER_BAR, SDL_SCANCODE_RIGHT, SDL_KMOD_SHIFT,
                          ACTIONS_NAVIGATION, ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_BACK_BEAT] = {DM_TEXT_BACK_QUARTER_BAR, SDL_SCANCODE_LEFT, SDL_KMOD_SHIFT,
                       ACTIONS_NAVIGATION, ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_HOME] = {DM_TEXT_GO_BEGINNING, SDL_SCANCODE_HOME, 0, ACTIONS_NAVIGATION,
                  ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_END] = {DM_TEXT_GO_END, SDL_SCANCODE_END, 0, ACTIONS_NAVIGATION,
                 ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_TOP] = {DM_TEXT_SCROLL_TOP, SDL_SCANCODE_PAGEUP, 0, ACTIONS_NAVIGATION,
                 ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_BOTTOM] = {DM_TEXT_SCROLL_BOTTOM, SDL_SCANCODE_PAGEDOWN, 0, ACTIONS_NAVIGATION,
                    ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_NEXT] = {DM_TEXT_NEXT_CREATE_PATTERN, SDL_SCANCODE_KP_PLUS, 0, ACTIONS_PATTERNS,
                  ACTION_REPEATS},
    [ACT_PREVIOUS] = {DM_TEXT_PREVIOUS_PATTERN, SDL_SCANCODE_KP_MINUS, 0, ACTIONS_PATTERNS,
                      ACTION_REPEATS},
    [ACT_UP] = {DM_TEXT_SCROLL_UP, SDL_SCANCODE_UP, 0, ACTIONS_NAVIGATION,
                ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_DOWN] = {DM_TEXT_SCROLL_DOWN, SDL_SCANCODE_DOWN, 0, ACTIONS_NAVIGATION,
                  ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_ITEM_UP] = {DM_TEXT_MOVE_SELECTION_UP, SDL_SCANCODE_UP, SDL_KMOD_SHIFT, ACTIONS_PATTERNS,
                     ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_ITEM_DOWN] = {DM_TEXT_MOVE_SELECTION_DOWN, SDL_SCANCODE_DOWN, SDL_KMOD_SHIFT,
                       ACTIONS_PATTERNS, ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_OCTAVE_UP] = {DM_TEXT_TRANSPOSE_OCTAVE_UP, SDL_SCANCODE_UP, SDL_KMOD_CTRL,
                       ACTIONS_PATTERNS, ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_OCTAVE_DOWN] = {DM_TEXT_TRANSPOSE_OCTAVE_DOWN, SDL_SCANCODE_DOWN, SDL_KMOD_CTRL,
                         ACTIONS_PATTERNS, ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_ZOOM_IN] = {DM_TEXT_ZOOM, SDL_SCANCODE_LEFTBRACKET, 0, ACTIONS_ZOOM,
                     ACTION_WORKSPACE | ACTION_PIANO_OFF},
    [ACT_ZOOM_OUT] = {DM_TEXT_ZOOM_OUT, SDL_SCANCODE_RIGHTBRACKET, 0, ACTIONS_ZOOM,
                      ACTION_WORKSPACE | ACTION_PIANO_OFF},
    [ACT_ZOOM_RESET] = {DM_TEXT_RESET_ZOOM, SDL_SCANCODE_BACKSLASH, 0, ACTIONS_ZOOM,
                        ACTION_WORKSPACE | ACTION_PIANO_OFF},
    [ACT_SHORTCUTS] = {DM_TEXT_KEYBOARD_SHORTCUTS, SDL_SCANCODE_K, 0, ACTIONS_WINDOWS,
                       ACTION_PIANO_OFF},
    [ACT_CONFIG] = {DM_TEXT_PROJECT_SETTINGS, SDL_SCANCODE_SLASH, 0, ACTIONS_WINDOWS,
                    ACTION_PIANO_OFF},
    [ACT_EDITOR] = {DM_TEXT_TOGGLE_PIANO_ROLL, SDL_SCANCODE_E, 0, ACTIONS_WINDOWS,
                    ACTION_PIANO_OFF},
    [ACT_SELECT] = {DM_TEXT_SELECT_TOOL, SDL_SCANCODE_1, 0, ACTIONS_WINDOWS, ACTION_PIANO_OFF},
    [ACT_MARQUEE] = {DM_TEXT_MARQUEE_TOOL, SDL_SCANCODE_2, 0, ACTIONS_WINDOWS, ACTION_PIANO_OFF},
    [ACT_SLICE] = {DM_TEXT_SLICE_TOOL, SDL_SCANCODE_3, 0, ACTIONS_WINDOWS, ACTION_PIANO_OFF},
    [ACT_DELETE] = {DM_TEXT_DELETE_TOOL, SDL_SCANCODE_4, 0, ACTIONS_WINDOWS, ACTION_PIANO_OFF},
    [ACT_BRUSH] = {DM_TEXT_BRUSH_TOOL, SDL_SCANCODE_5, 0, ACTIONS_WINDOWS, ACTION_PIANO_OFF},
    [ACT_PIANO_DOWN] = {DM_TEXT_KEYBOARD_OCTAVE_DOWN, SDL_SCANCODE_KP_1, 0, ACTIONS_TRANSPORT, 0},
    [ACT_PIANO_UP] = {DM_TEXT_KEYBOARD_OCTAVE_UP, SDL_SCANCODE_KP_2, 0, ACTIONS_TRANSPORT, 0},
    [ACT_PLAY_START] = {DM_TEXT_PLAY_BEGINNING, SDL_SCANCODE_SPACE, SDL_KMOD_SHIFT,
                        ACTIONS_TRANSPORT, 0},
    [ACT_SONG_EVENT] = {DM_TEXT_SONG_EVENTS, SDL_SCANCODE_E, SDL_KMOD_SHIFT, ACTIONS_WINDOWS, 0},
    [ACT_CHANNEL_UP] = {DM_TEXT_MOVE_CHANNEL_UP, SDL_SCANCODE_UP, SDL_KMOD_ALT, ACTIONS_PATTERNS,
                        ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_CHANNEL_DOWN] = {DM_TEXT_MOVE_CHANNEL_DOWN, SDL_SCANCODE_DOWN, SDL_KMOD_ALT,
                          ACTIONS_PATTERNS, ACTION_REPEATS | ACTION_WORKSPACE},
    [ACT_MUTE] = {DM_TEXT_MUTE_SELECTED_CHANNEL, SDL_SCANCODE_M, 0, ACTIONS_PATTERNS,
                  ACTION_PIANO_OFF},
    [ACT_SOLO] = {DM_TEXT_SOLO_SELECTED_CHANNEL, SDL_SCANCODE_S, 0, ACTIONS_PATTERNS,
                  ACTION_PIANO_OFF},
    [ACT_RECORD] = {DM_TEXT_RECORD_KEYBOARD_PIANO, SDL_SCANCODE_R, 0, ACTIONS_TRANSPORT,
                    ACTION_PIANO_OFF},
    [ACT_METRONOME] = {DM_TEXT_METRONOME, SDL_SCANCODE_F2, 0, ACTIONS_TRANSPORT, 0},
    [ACT_WAIT] = {DM_TEXT_WAIT_INPUT, SDL_SCANCODE_F3, 0, ACTIONS_TRANSPORT, 0},
    [ACT_OVERLAP] = {DM_TEXT_OVERLAPPING_NOTES, SDL_SCANCODE_F4, 0, ACTIONS_TRANSPORT, 0},
    [ACT_RECORD_GLOBAL] = {DM_TEXT_RECORD, SDL_SCANCODE_F5, 0, ACTIONS_TRANSPORT, 0},
    [ACT_DUPLICATE] = {DM_TEXT_DUPLICATE_SELECTION, SDL_SCANCODE_D, SDL_KMOD_CTRL, ACTIONS_PATTERNS,
                       ACTION_WORKSPACE},
    [ACT_PLAY_MODE] = {DM_TEXT_SONG_PATTERN_PLAYBACK, SDL_SCANCODE_GRAVE, 0, ACTIONS_TRANSPORT, 0},
    [ACT_NOTE_COLOR] = {DM_TEXT_COLOR_SELECTED_NOTES, SDL_SCANCODE_C, SDL_KMOD_ALT,
                        ACTIONS_PATTERNS, ACTION_WORKSPACE},
    [ACT_NEXT_AUTOMATION] = {DM_TEXT_NEXT_CREATE_AUTOMATION, SDL_SCANCODE_KP_PLUS, SDL_KMOD_SHIFT,
                             ACTIONS_PATTERNS, 0},
    [ACT_PREVIOUS_AUTOMATION] = {DM_TEXT_PREVIOUS_AUTOMATION, SDL_SCANCODE_KP_MINUS, SDL_KMOD_SHIFT,
                                 ACTIONS_PATTERNS, 0},
    [ACT_INSTRUMENT] = {DM_TEXT_CHOOSE_CHANNEL_INSTRUMENT, SDL_SCANCODE_I, 0, ACTIONS_WINDOWS, 0},
    [ACT_PREFERENCES] = {DM_TEXT_PREFERENCES, SDL_SCANCODE_P, 0, ACTIONS_WINDOWS, 0},
    [ACT_COPY_NOTE_BENDS] = {DM_TEXT_COPY_NOTE_BENDS, SDL_SCANCODE_C,
                             SDL_KMOD_CTRL | SDL_KMOD_SHIFT, ACTIONS_PATTERNS, ACTION_WORKSPACE},
    [ACT_CUT_NOTE_BENDS] = {DM_TEXT_CUT_NOTE_BENDS, SDL_SCANCODE_X, SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
                            ACTIONS_PATTERNS, ACTION_WORKSPACE},
};

static void preview_open_pattern(Editor *e, int id);
static bool push(DmBytes *stack, int *count, const DmProject *p);
static void publish_loop(Editor *e);
static int clip_identity(const void *left, const void *right);
static void retain_note_selection(DmPattern *next, const DmPattern *live);
static void retain_selection(DmProject *next, const DmProject *live);

void ed_focus_edit(Editor *e, EditFocus focus) {
    if (ed_selection_focus(e) != focus) {
        e->marquee = e->event_marquee = false;
    }

    e->edit_focus = focus;
}

EditContext ed_context(const Editor *e) {
    return e->edit_focus == FOCUS_NONE                                     ? CONTEXT_NONE
           : e->edit_focus == FOCUS_NOTES || e->edit_focus == FOCUS_EVENTS ? CONTEXT_ROLL
                                                                           : CONTEXT_PLAYLIST;
}

void ed_focus_timeline(Editor *e, EditContext context) {
    if (ed_context(e) == context) {
        return;
    }

    if (context == CONTEXT_ROLL) {
        ed_focus_edit(e, FOCUS_NOTES);
    } else if (context == CONTEXT_PLAYLIST) {
        ed_focus_edit(e, FOCUS_PLAYLIST);
    }
}

bool ed_velocity_editing(Editor *e) {
    return e->automation_cc == DM_EVENT_VELOCITY && e->selected_pattern >= 0 &&
           (size_t)e->selected_pattern < e->project.pattern_count &&
           !e->project.patterns[e->selected_pattern].kind;
}

EditFocus ed_selection_focus(Editor *e) {
    return e->edit_focus ? e->edit_focus : FOCUS_PLAYLIST;
}

void ed_reset_layout(Editor *e) {
    e->sidebar = 465;
    e->mixer_height = 420;
    e->label_width = 250; /* Fits the longest GM name, channel number and expander. */
    e->automation_height = 360;
    e->roll_height = 420; /* Matches mixer_height, so both dividers meet in a cross. */
}

void ed_toggle_roll(Editor *e) {
    DmPattern *selected = ed_pattern(e);

    if (e->roll && !e->roll_auto_hidden && (!selected || !selected->kind)) {
        e->roll = false;
        ed_focus_edit(e, FOCUS_PLAYLIST);
        return;
    }

    if (!selected || selected->kind) {
        int target = -1;

        for (size_t i = 0; i < e->project.pattern_count; ++i) {
            const DmPattern *pattern = &e->project.patterns[i];

            if (pattern->kind) {
                continue;
            }

            if (target < 0) {
                target = (int)i;
            }

            if (pattern->uid == e->last_note_pattern_uid) {
                target = (int)i;
                break;
            }
        }

        if (target >= 0) {
            bool browser = e->automation_browser;
            ed_open_pattern(e, target, false);
            e->automation_browser = browser;
            ed_bind_pattern_channel(e);
        } else if (selected) {
            return;
        }
    }

    e->roll = true;
    e->roll_auto_hidden = false;
    /* The opened editor takes focus, so its shortcuts work at once. */
    ed_focus_edit(e, FOCUS_NOTES);
}

int ed_channel_program(Editor *e, int channel) {
    return dm_channel_drums(&e->project, channel)
               ? 128
               : (int)fmax(0, e->project.channels[channel].program);
}

/* Song beat the pattern editor is at: the pattern position within its region in pattern
   playback, otherwise the song position. */
static double placed_song_beat(Editor *e) {
    bool playing = dm_audio_playing(e->audio);
    double now = playing ? dm_seconds_beat(&e->project, dm_audio_position(e->audio)) : 0;

    if (e->pattern_mode && ed_pattern(e)) {
        return ed_pattern_region_origin(e) + (playing ? now : e->cursor_beat[1]);
    }

    return playing ? now : e->cursor_beat[0];
}

/* The instrument the placed pattern's channel plays at the current position: the channel
   program, then the latest program change from imported events, song events or the open
   pattern's own events. */
int ed_placed_program(Editor *e) {
    int channel = e->preview_source_channel;

    if (channel < 0 || channel > 15 || dm_channel_drums(&e->project, channel)) {
        return channel < 0 || channel > 15 ? e->preview_program : 128;
    }

    double at = placed_song_beat(e) + 1e-7, latest = -1;
    int program = (int)fmax(0, e->project.channels[channel].program);

    for (size_t i = 0; i < e->project.event_count; ++i) {
        const DmEvent *event = &e->project.events[i];
        const uint8_t *m = event->message.data;

        if (event->message.size == 2 && m[0] == (MIDI_PROGRAM | channel) && event->beat > 0 &&
            event->beat <= at && event->beat >= latest) {
            latest = event->beat;
            program = m[1];
        }
    }

    double origin = ed_pattern(e) ? ed_pattern_region_origin(e) : 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        /* Pattern events play on the channel of the placement they belong to. */
        bool song = a.pattern < 0 && a.channel == channel,
             local = a.pattern >= 0 && a.pattern == e->selected_pattern;
        double beat = song ? a.beat : origin + a.beat;

        if ((song || local) && a.type == MIDI_PROGRAM && beat <= at && beat >= latest) {
            latest = beat;
            program = a.a;
        }
    }

    return program;
}

void ed_preview_channel(Editor *e, int channel, bool placed) {
    e->preview_source_channel = placed ? channel : -1;
    e->preview_override = false;
    e->preview_program = ed_channel_program(e, channel);
}

bool ed_pattern_placed(Editor *e) {
    for (size_t i = 0; ed_pattern(e) && i < e->project.clip_count; ++i) {
        if (e->project.clips[i].pattern == e->selected_pattern) {
            return true;
        }
    }

    return false;
}

bool ed_preview_instrument_shown(Editor *e) {
    return e->pattern_mode || !ed_pattern_placed(e);
}

bool ed_follow_preview(Editor *e) {
    if (e->preview_override) {
        return false;
    }

    int channel = e->pattern_channel, program = e->preview_program;

    if (ed_pattern(e) && !ed_pattern_placed(e)) {
        e->preview_source_channel = -1;

        if (e->channel_selected) {
            e->pattern_channel = e->selected_channel;
            e->preview_program = ed_channel_program(e, e->selected_channel);
        }
    } else if (e->preview_source_channel >= 0) {
        e->preview_program = ed_channel_program(e, e->preview_source_channel);
    }

    return channel != e->pattern_channel || program != e->preview_program;
}

void ed_follow_preview_frame(Editor *e) {
    int channel = e->pattern_channel;

    if (!ed_follow_preview(e)) {
        return;
    }

    if (channel != e->pattern_channel) {
        ed_release_piano(e);
    }

    ed_keyboard_channel(e);

    if (e->pattern_mode) {
        ed_publish(e);
    }
}

void ed_select_placement_route(Editor *e, int index) {
    DmClip clip = e->project.clips[index];
    ed_release_piano(e);
    e->selected_clip = index;
    e->pattern_channel = e->selected_channel = clip.channel;
    e->channel_selected = true;

    if (!e->preview_override) {
        ed_preview_channel(e, clip.channel, true);
    }
}

static void preview_open_pattern(Editor *e, int id) {
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].pattern == id) {
            ed_preview_channel(e, e->project.clips[i].channel, true);
            return;
        }
    }

    ed_preview_channel(e, e->selected_channel, false);
}

/* Drops a chosen preview instrument so the pattern follows its placement's channel again. */
void ed_reset_preview_instrument(Editor *e) {
    ed_release_piano(e);
    preview_open_pattern(e, e->selected_pattern);
    ed_keyboard_channel(e);

    if (e->pattern_mode) {
        ed_publish(e);
    }
}

void ed_open_pattern(Editor *e, int id, bool created) {
    e->curve_drag = e->marquee = e->event_marquee = false;

    if (created || id != e->selected_pattern || !e->preview_override) {
        preview_open_pattern(e, id);
    }

    e->event_selection.size = 0;
    DmPattern *next =
        id >= 0 && (size_t)id < e->project.pattern_count ? &e->project.patterns[id] : NULL;

    if (id != e->selected_pattern) {
        ed_deselect_notes(ed_pattern(e));
        ed_deselect_notes(next);
        e->loop_enabled[1] = false;

        if (e->pattern_mode) {
            dm_audio_loop(e->audio, false, 0, 0);
        }
    }

    if (next) {
        e->automation_browser = next->kind != 0;

        if (e->automation_browser) {
            e->automation_cc = next->event;
        } else {
            e->last_note_pattern_uid = next->uid;
        }
    }

    e->selected_pattern = id;
    e->pattern_highlight = id >= 0;
    e->linked_highlight = id >= 0;
    e->roll_auto_hidden = false;

    if (created) {
        e->roll = true;
    }

    e->focus_pattern = true;
    e->automation_all = false;
}

SDL_Keymod ed_normalized(SDL_Keymod m) {
    return (SDL_Keymod)(((m & SDL_KMOD_CTRL) ? SDL_KMOD_CTRL : 0) |
                        ((m & SDL_KMOD_SHIFT) ? SDL_KMOD_SHIFT : 0) |
                        ((m & SDL_KMOD_ALT) ? SDL_KMOD_ALT : 0) |
                        ((m & SDL_KMOD_GUI) ? SDL_KMOD_GUI : 0));
}

/* Every mouse gesture: its preferences key and default. Rows up to GESTURE_REBINDABLE can be
   changed in Keyboard Shortcuts and are saved; the rest are fixed. MOUSE_ANY rows are modifiers
   that change what a press, drag or wheel turn already under way does. */
#define CTRL SDL_KMOD_CTRL
#define SHIFT SDL_KMOD_SHIFT
#define ALT SDL_KMOD_ALT
const GestureInfo ed_gestures[GESTURE_COUNT] = {
    [GESTURE_COPY_PLACEMENTS] = {"copy_placements_drag",
                                 {MOUSE_LEFT, SHIFT},
                                 DM_TEXT_COPY_PLACEMENTS_DRAG},
    [GESTURE_COPY_NOTES] = {"copy_notes_drag", {MOUSE_MIDDLE, 0}, DM_TEXT_COPY_NOTES_DRAG},
    [GESTURE_FREE] = {"free", {MOUSE_ANY, ALT}},
    [GESTURE_MARQUEE] = {"marquee", {MOUSE_LEFT, CTRL}},
    [GESTURE_EXTEND] = {"extend_selection", {MOUSE_LEFT, SHIFT}},
    [GESTURE_MENU] = {"item_menu", {MOUSE_RIGHT, SHIFT}},
    [GESTURE_ZOOM] = {"zoom", {MOUSE_WHEEL, CTRL}},
    [GESTURE_SCROLL_ROWS] = {"scroll_rows", {MOUSE_WHEEL, SHIFT}},
    [GESTURE_VELOCITY_WHEEL] = {"velocity_wheel", {MOUSE_WHEEL, ALT}},
    [GESTURE_NOTE_BEND] = {"note_bend", {MOUSE_LEFT, SHIFT}},
    [GESTURE_BEND_EDIT] = {"bend_edit", {MOUSE_LEFT, SHIFT}},
    [GESTURE_BEND_POINT] = {"bend_point", {MOUSE_LEFT, ALT}},
    [GESTURE_BRUSH_KEY] = {"brush_key", {MOUSE_ANY, CTRL}},
    [GESTURE_LOCK_VALUE] = {"lock_value", {MOUSE_ANY, SHIFT}},
    [GESTURE_LOCK_TIME] = {"lock_time", {MOUSE_ANY, CTRL}},
    [GESTURE_CONNECT] = {"connect_point", {MOUSE_LEFT, SHIFT}},
    [GESTURE_LANE_SETTINGS] = {"lane_settings", {MOUSE_LEFT, SHIFT}},
    [GESTURE_PLAY_RANGE] = {"play_range", {MOUSE_LEFT, SHIFT}},
    [GESTURE_DESELECT] = {"deselect_pattern", {MOUSE_LEFT, CTRL}},
    [GESTURE_APPLY_ALL] = {"apply_all", {MOUSE_LEFT, SHIFT}},
    [GESTURE_PORT_MENU] = {"port_menu", {MOUSE_LEFT, SHIFT}},
    [GESTURE_PANIC] = {"panic", {MOUSE_LEFT, SHIFT}},
};
#undef CTRL
#undef SHIFT
#undef ALT

static SDL_Keymod mouse_mods(UiMouse mouse) {
    return (SDL_Keymod)((mouse.ctrl ? SDL_KMOD_CTRL : 0) | (mouse.shift ? SDL_KMOD_SHIFT : 0) |
                        (mouse.alt ? SDL_KMOD_ALT : 0));
}

bool ed_mouse_down(UiMouse mouse, int button) {
    return button == MOUSE_LEFT    ? mouse.left
           : button == MOUSE_RIGHT ? mouse.right
                                   : button == MOUSE_MIDDLE && mouse.middle;
}

bool ed_gesture_held(const Editor *e, Gesture gesture, UiMouse mouse) {
    SDL_Keymod mods = e->gestures[gesture].mods;
    return (mouse_mods(mouse) & mods) == mods;
}

void ed_gesture_set(const Editor *e, Gesture gesture, UiMouse *mouse, bool held) {
    SDL_Keymod mods = e->gestures[gesture].mods;
    mouse->ctrl = mods & SDL_KMOD_CTRL ? held : mouse->ctrl;
    mouse->shift = mods & SDL_KMOD_SHIFT ? held : mouse->shift;
    mouse->alt = mods & SDL_KMOD_ALT ? held : mouse->alt;
}

bool ed_gesture_pressed(Editor *e, Gesture gesture, UiMouse mouse) {
    MouseBinding b = e->gestures[gesture];
    bool pressed = b.button == MOUSE_LEFT    ? mouse.clicked
                   : b.button == MOUSE_RIGHT ? mouse.right_clicked
                                             : mouse.middle_clicked;
    return pressed && mouse_mods(mouse) == b.mods;
}

/* Plain left and right drags select, move and erase, so a gesture needs another button or a
   modifier. */
static bool ed_gesture_allowed(MouseBinding b) {
    return b.button == MOUSE_MIDDLE || b.mods;
}

bool ed_gesture_capture(Editor *e, UiMouse mouse) {
    if (e->gesture_capture < 0) {
        return false;
    }

    if (!e->gesture_capture_armed) {
        e->gesture_capture_armed = !mouse.left && !mouse.right && !mouse.middle;
        return true;
    }

    MouseBinding b = {mouse.clicked         ? MOUSE_LEFT
                      : mouse.right_clicked ? MOUSE_RIGHT
                                            : MOUSE_MIDDLE,
                      mouse_mods(mouse)};

    if (!mouse.clicked && !mouse.right_clicked && !mouse.middle_clicked) {
        return true;
    }

    if (!ed_gesture_allowed(b)) {
        ed_message(e, DM_TEXT_GESTURE_NEEDS_MODIFIER);
    } else {
        e->gestures[e->gesture_capture] = b;
        ed_preference_save(e);
    }

    e->gesture_capture = -1;
    return true;
}

bool ed_gesture_format(MouseBinding b, char *out, size_t cap) {
    static const char *buttons[MOUSE_BUTTONS] = {DM_TEXT_MOUSE_LEFT, DM_TEXT_MOUSE_RIGHT,
                                                 DM_TEXT_MOUSE_MIDDLE};

    if (b.button < 0 || b.button >= MOUSE_BUTTONS) {
        return false;
    }

    int n = snprintf(out, cap, "%s%s%s%s", b.mods & SDL_KMOD_CTRL ? DM_TEXT_CTRL : "",
                     b.mods & SDL_KMOD_SHIFT ? DM_TEXT_SHIFT : "",
                     b.mods & SDL_KMOD_ALT ? DM_TEXT_ALT : "", buttons[b.button]);
    return n > 0 && (size_t)n < cap;
}

bool ed_gesture_parse(const char *text, MouseBinding *out) {
    for (int button = 0; button < MOUSE_BUTTONS; ++button) {
        for (int mods = 0; mods < 8; ++mods) {
            MouseBinding b = {(MouseButton)button, (SDL_Keymod)((mods & 1 ? SDL_KMOD_CTRL : 0) |
                                                                (mods & 2 ? SDL_KMOD_SHIFT : 0) |
                                                                (mods & 4 ? SDL_KMOD_ALT : 0))};
            char name[64];

            if (ed_gesture_format(b, name, sizeof(name)) && !strcmp(name, text) &&
                ed_gesture_allowed(b)) {
                *out = b;
                return true;
            }
        }
    }

    return false;
}

void ed_defaults(Editor *e) {
    for (int i = 0; i < ACT_COUNT; ++i) {
        e->bindings[i] = (Binding){.key = ed_actions[i].key, .mods = ed_actions[i].mods};
    }

    for (int i = 0; i < GESTURE_COUNT; ++i) {
        e->gestures[i] = ed_gestures[i].binding;
    }
}

void ed_message(Editor *e, const char *text) {
    snprintf(e->status, sizeof(e->status), "%s", text);
    e->status_time = SDL_GetTicks();
}

DmPattern *ed_pattern(Editor *e) {
    return e->selected_pattern >= 0 && (size_t)e->selected_pattern < e->project.pattern_count
               ? &e->project.patterns[e->selected_pattern]
               : NULL;
}

void ed_bind_pattern_channel(Editor *e) {
    ed_release_piano(e);
    e->pattern_channel = e->selected_channel;
    e->automation_node = -1;
    e->last_automation_valid = false;
    e->focus_pattern = true;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].pattern == e->selected_pattern) {
            e->pattern_channel = e->project.clips[i].channel;

            if (!e->preview_override) {
                ed_preview_channel(e, e->pattern_channel, true);
            }

            return;
        }
    }

    if (!e->preview_override) {
        ed_preview_channel(e, e->pattern_channel, false);
    }
}

void ed_clear_history(DmBytes *b, int *count) {
    for (int i = 0; i < *count; ++i) {
        dm_bytes_free(&b[i]);
    }

    *count = 0;
}

static bool push(DmBytes *stack, int *count, const DmProject *p) {
    DmBytes bytes = {0};

    if (!dm_pack(p, &bytes)) {
        return false;
    }

    size_t total = bytes.size;

    for (int i = 0; i < *count; ++i) {
        total += stack[i].size;
    }

    while (*count && (*count == ED_UNDO_LIMIT || total > 512u * 1024u * 1024u)) {
        total -= stack[0].size;
        dm_bytes_free(&stack[0]);
        memmove(stack, stack + 1, (size_t)(*count - 1) * sizeof(*stack));
        --*count;
    }

    stack[(*count)++] = bytes;
    return true;
}

bool ed_checkpoint(Editor *e) {
    e->midi_mix_last = 0;

    if (e->recording_take && e->take_snapshot) {
        return false;
    }

    bool pushed = push(e->undo, &e->undo_count, &e->project);

    if (!pushed) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_UNDO);
    } else if (e->recording_take) {
        e->take_snapshot = true;
    }

    ed_clear_history(e->redo, &e->redo_count);
    return pushed;
}

bool ed_right_free(const Editor *e, UiMouse mouse) {
    return !ed_gesture_held(e, GESTURE_MENU, mouse) && !e->loop_drag && !e->curve_drag &&
           e->copy_button < 0;
}

void ed_erase_step(Editor *e) {
    if (!e->erase_active) {
        ed_checkpoint(e);
    }

    e->erase_active = true;
}

void ed_checkpoint_abandon(Editor *e, bool pushed) {
    if (!pushed || !e->undo_count) {
        return;
    }

    DmBytes b = e->undo[--e->undo_count];
    DmProject saved;
    dm_project_init(&saved);

    if (dm_unpack(&saved, b.data, b.size, e->status, sizeof(e->status))) {
        saved.next_pattern_uid =
            (uint32_t)fmax(saved.next_pattern_uid, e->project.next_pattern_uid);
        retain_selection(&saved, &e->project);
        dm_project_free(&e->project);
        e->project = saved;
    } else {
        dm_project_free(&saved);
    }

    if (e->recording_take) {
        e->take_snapshot = false;
    }

    dm_bytes_free(&b);
}

bool ed_add_note(Editor *e, DmPattern *p, DmNote note) {
    double previous = p->length;

    if (!dm_note_add(p, note)) {
        return false;
    }

    if (p->length > previous) {
        dm_pattern_fit(&e->project, p);
    }

    return true;
}

double ed_pattern_length(Editor *e) {
    if (!ed_pattern(e)) {
        return dm_bar(&e->project);
    }

    if (ed_pattern(e)->kind && e->selected_clip >= 0 &&
        (size_t)e->selected_clip < e->project.clip_count &&
        e->project.clips[e->selected_clip].pattern == e->selected_pattern) {
        return dm_clip_length(&e->project, e->project.clips[e->selected_clip]);
    }

    return ed_pattern(e)->length;
}

static void publish_loop(Editor *e) {
    int mode = e->pattern_mode;
    dm_audio_loop(e->audio, e->loop_enabled[mode],
                  dm_beat_seconds(&e->project, e->loop_start[mode]),
                  dm_beat_seconds(&e->project, e->loop_end[mode]));
}

/* A placed pattern follows its region's program changes unless a preview instrument is chosen. */
static DmPatternPreview pattern_preview(Editor *e) {
    ed_follow_preview(e);
    bool placed = !e->preview_override && e->preview_source_channel >= 0;

    return (DmPatternPreview){e->selected_pattern,         e->pattern_channel,
                              e->preview_program,          placed,
                              ed_pattern_region_origin(e), ed_pattern_length(e)};
}

bool ed_transport_publish(Editor *e, double seconds, bool playing) {
    bool ok;

    if (e->pattern_mode && ed_pattern(e)) {
        DmPatternPreview preview = pattern_preview(e);
        ok = dm_audio_publish_pattern(e->audio, &e->project, &preview, seconds, playing, e->status,
                                      sizeof(e->status));
    } else {
        ok =
            dm_audio_publish(e->audio, &e->project, seconds, playing, e->status, sizeof(e->status));
    }

    if (ok) {
        publish_loop(e);
    }

    return ok;
}

double ed_transport_length(Editor *e) {
    return e->pattern_mode && ed_pattern(e) ? ed_pattern_length(e) : dm_duration(&e->project);
}

void ed_publish(Editor *e) {
    e->midi_mix_pending = false;
    ++e->export_revision;

    if (e->active) {
        bool pattern = e->pattern_mode && ed_pattern(e);
        DmPatternPreview preview = {0};

        if (pattern) {
            preview = pattern_preview(e);
        } else {
            ed_follow_preview(e);
        }

        if (dm_audio_update(e->audio, &e->project, pattern ? &preview : NULL, e->status,
                            sizeof(e->status))) {
            publish_loop(e);
        }
    }
}

void ed_changed(Editor *e) {
    ++e->export_revision;
    dm_patterns_fit(&e->project);
    e->dirty = true;
}

void ed_request_font(Editor *e, const char *path) {
    if (dm_audio_busy(e->audio)) {
        if (e->font_request) {
            dm_audio_cancel(e->audio);
        }

        e->font_request = false;
        e->font_queued = true;
        snprintf(e->font_next, sizeof(e->font_next), "%s", path);
        return;
    }

    if (dm_audio_soundfont(e->audio, path, e->status, sizeof(e->status))) {
        e->font_request = true;
        snprintf(e->font_pending, sizeof(e->font_pending), "%s", path);
        ed_message(e, DM_TEXT_LOADING_SOUNDFONT);
    }
}

void ed_commit(Editor *e) {
    ed_changed(e);
    ed_publish(e);
}

int ed_note_identity(const void *left, const void *right) {
    const DmNote *a = left, *b = right;

    if (a->port != b->port) {
        return (int)a->port - b->port;
    }

    if (a->beat != b->beat) {
        return a->beat < b->beat ? -1 : 1;
    }

    if (a->key != b->key) {
        return a->key < b->key ? -1 : 1;
    }

    if (a->length != b->length) {
        return a->length < b->length ? -1 : 1;
    }

    return (int)a->velocity - b->velocity;
}

static int clip_identity(const void *left, const void *right) {
    const DmClip *a = left, *b = right;

    if (a->beat != b->beat) {
        return a->beat < b->beat ? -1 : 1;
    }

    if (a->pattern != b->pattern) {
        return a->pattern < b->pattern ? -1 : 1;
    }

    if (a->channel != b->channel) {
        return a->channel - b->channel;
    }

    if (a->lane != b->lane) {
        return a->lane - b->lane;
    }

    return a->row - b->row;
}

static void retain_note_selection(DmPattern *next, const DmPattern *live) {
    DmNote *selected = malloc((live->count + 1) * sizeof(*selected));
    size_t count = 0;

    for (size_t i = 0; i < live->count && selected; ++i) {
        if (live->notes[i].selected) {
            selected[count++] = live->notes[i];
        }
    }

    if (selected) {
        qsort(selected, count, sizeof(*selected), ed_note_identity);
    }

    for (size_t i = 0; i < next->count; ++i) {
        next->notes[i].selected = selected && bsearch(next->notes + i, selected, count,
                                                      sizeof(*selected), ed_note_identity);
    }

    free(selected);
}

static void retain_selection(DmProject *next, const DmProject *live) {
    for (size_t i = 0; i < next->pattern_count; ++i) {
        DmPattern empty = {0};
        const DmPattern *before = i < live->pattern_count ? live->patterns + i : &empty;
        retain_note_selection(next->patterns + i, before);
    }

    DmClip *selected = malloc((live->clip_count + 1) * sizeof(*selected));
    size_t count = 0;

    for (size_t i = 0; i < live->clip_count && selected; ++i) {
        if (live->clips[i].selected) {
            selected[count++] = live->clips[i];
        }
    }

    if (selected) {
        qsort(selected, count, sizeof(*selected), clip_identity);
    }

    for (size_t i = 0; i < next->clip_count; ++i) {
        next->clips[i].selected =
            selected && bsearch(next->clips + i, selected, count, sizeof(*selected), clip_identity);
    }

    free(selected);
}

void ed_undo(Editor *e, bool redo) {
    if (ed_samples_history(e, redo, true)) {
        return;
    }

    e->midi_mix_last = 0;
    ed_midi_bindings_reset_pickup(e);
    ed_close_instrument(e);
    e->midi_event_index = -1;
    e->song_event = false;
    e->song_event_channel = -1;
    e->curve_drag = e->marquee = e->event_marquee = false;
    e->wait_armed = e->take_started = false;
    e->event_selection.size = 0;
    ed_release_piano(e);
    e->recording_take = e->take_snapshot = false;
    char old_font[DM_PATH];
    snprintf(old_font, sizeof(old_font), "%s", e->project.soundfont);
    DmBytes *from = redo ? e->redo : e->undo, *to = redo ? e->undo : e->redo;
    int *n = redo ? &e->redo_count : &e->undo_count, *m = redo ? &e->undo_count : &e->redo_count;

    if (!*n) {
        return;
    }

    if (!push(to, m, &e->project)) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_UNDO);
        return;
    }

    DmBytes b = from[--*n];
    DmProject next;
    dm_project_init(&next);

    if (dm_unpack(&next, b.data, b.size, e->status, sizeof(e->status))) {
        /* Identities cannot be reused when an undone creation is replaced by another edit. */
        next.next_pattern_uid = (uint32_t)fmax(next.next_pattern_uid, e->project.next_pattern_uid);
        retain_selection(&next, &e->project);

        if (next.port != e->project.port || next.sequence != e->project.sequence) {
            e->last_note_pattern_uid = 0;
        }

        dm_project_free(&e->project);
        e->project = next;
        dm_project_init(&next);

        if ((size_t)e->selected_pattern >= e->project.pattern_count) {
            e->selected_pattern = -1;
            ed_clear_pattern_selection(e);
        }

        e->automation_node = -1;

        /* The event editor shows the restored controller of an automation pattern. */
        if (ed_pattern(e) && ed_pattern(e)->kind) {
            e->automation_cc = ed_pattern(e)->event;
        }

        ed_commit(e);

        if (strcmp(old_font, e->project.soundfont)) {
            ed_request_font(e, e->project.soundfont);
        }
    }

    dm_project_free(&next);
    dm_bytes_free(&b);
}
