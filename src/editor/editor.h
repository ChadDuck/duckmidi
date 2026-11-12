#ifndef DM_EDITOR_H
#define DM_EDITOR_H
#include "audio/audio.h"
#include "editor/midi_bindings.h"
#include "core/duck.h"
#include "ui.h"

typedef enum {
    ACT_PIANO,
    ACT_QUANTIZE,
    ACT_PLAY,
    ACT_FORWARD,
    ACT_BACK,
    ACT_FORWARD_BEAT,
    ACT_BACK_BEAT,
    ACT_HOME,
    ACT_END,
    ACT_TOP,
    ACT_BOTTOM,
    ACT_NEXT,
    ACT_PREVIOUS,
    ACT_UP,
    ACT_DOWN,
    ACT_ITEM_UP,
    ACT_ITEM_DOWN,
    ACT_OCTAVE_UP,
    ACT_OCTAVE_DOWN,
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_ZOOM_RESET,
    ACT_SHORTCUTS,
    ACT_CONFIG,
    ACT_EDITOR,
    ACT_SELECT,
    ACT_MARQUEE,
    ACT_SLICE,
    ACT_DELETE,
    ACT_BRUSH,
    ACT_PIANO_DOWN,
    ACT_PIANO_UP,
    ACT_PLAY_START,
    ACT_SONG_EVENT,
    ACT_MUTE,
    ACT_SOLO,
    ACT_RECORD,
    ACT_METRONOME,
    ACT_WAIT,
    ACT_OVERLAP,
    ACT_RECORD_GLOBAL,
    ACT_DUPLICATE,
    ACT_PLAY_MODE,
    ACT_NOTE_COLOR,
    ACT_NEXT_AUTOMATION,
    ACT_PREVIOUS_AUTOMATION,
    ACT_INSTRUMENT,
    ACT_PREFERENCES,
    ACT_COPY_NOTE_BENDS,
    ACT_CUT_NOTE_BENDS,
    ACT_CHANNEL_UP,
    ACT_CHANNEL_DOWN,
    ACT_COUNT
} Action;

typedef struct {
    SDL_Scancode key;
    SDL_Keymod mods;
    uint8_t mouse;
} Binding;

static inline bool ed_side_button(unsigned button) {
    return button == SDL_BUTTON_X1 || button == SDL_BUTTON_X2;
}

static inline bool ed_same_binding(Binding a, Binding b) {
    return a.key == b.key && a.mods == b.mods && a.mouse == b.mouse;
}

/* Every mouse gesture the editor recognizes; ed_gestures in editor/state.c defines each one. The
   first GESTURE_REBINDABLE are drags that can be rebound in Keyboard Shortcuts. The rest are fixed:
   test them with ed_gesture_held, never with UiMouse.shift, .ctrl or .alt. */
typedef enum {
    GESTURE_COPY_PLACEMENTS,
    GESTURE_COPY_NOTES,
    GESTURE_REBINDABLE,
    GESTURE_FREE = GESTURE_REBINDABLE, /* move, place and resize without snapping */
    GESTURE_MARQUEE,                   /* left drag draws a selection box */
    GESTURE_EXTEND,                    /* a click adds to or removes from the selection */
    GESTURE_MENU,                      /* right click opens the item's menu instead of erasing */
    GESTURE_ZOOM,                      /* the wheel zooms */
    GESTURE_SCROLL_ROWS,               /* the wheel scrolls rows instead of time */
    GESTURE_VELOCITY_WHEEL,            /* the wheel changes the velocity of the notes under it */
    GESTURE_NOTE_BEND,                 /* dragging from a note draws a pitch bend */
    GESTURE_BEND_EDIT,                 /* a click on a bend point selects it in the event lane */
    GESTURE_BEND_POINT,                /* a click on a bend line edits the line */
    GESTURE_BRUSH_KEY,                 /* the brush stays on the key it started on */
    GESTURE_LOCK_VALUE,                /* dragging an event point changes its time only */
    GESTURE_LOCK_TIME,                 /* dragging an event point changes its value only */
    GESTURE_CONNECT,                   /* a click in an event lane connects to the nearest point */
    GESTURE_LANE_SETTINGS,             /* a click on a channel label opens its lane settings */
    GESTURE_PLAY_RANGE,                /* the sample editor's playback range handles */
    GESTURE_DESELECT,                  /* a click on a selected pattern clears the selection */
    GESTURE_APPLY_ALL,                 /* an event lane button acts on every point */
    GESTURE_PORT_MENU,                 /* a click on a port colour offers to delete the port */
    GESTURE_PANIC,                     /* Stop also silences every MIDI note */
    GESTURE_COUNT
} Gesture;

/* MOUSE_BUTTONS counts the buttons a rebindable gesture can use; the two after it only describe
   fixed gestures. */
typedef enum {
    MOUSE_LEFT,
    MOUSE_RIGHT,
    MOUSE_MIDDLE,
    MOUSE_BUTTONS,
    MOUSE_WHEEL = MOUSE_BUTTONS,
    MOUSE_ANY
} MouseButton;

typedef struct {
    MouseButton button;
    SDL_Keymod mods; /* Ctrl, Shift and Alt only. */
} MouseBinding;

typedef struct {
    const char *name; /* preferences key */
    MouseBinding binding;
    const char *label; /* Keyboard Shortcuts row; rebindable gestures only */
} GestureInfo;

typedef struct {
    float zoom, scroll_x, scroll_y;
    int focus;
} View;

/* The section that owns the selection, the clipboard and the editing shortcuts. */
typedef enum {
    FOCUS_NONE,
    FOCUS_PLAYLIST,
    FOCUS_NOTES,
    FOCUS_EVENTS,
    FOCUS_PATTERNS,
    FOCUS_OTHER
} EditFocus;

/* The timeline the transport, scrolling and navigation shortcuts act on; derived from the focus
   by ed_context. */
typedef enum {
    CONTEXT_NONE,
    CONTEXT_PLAYLIST,
    CONTEXT_ROLL,
    CONTEXT_EVENT_EDITOR /* Only as a pan owner: the Event Editor pans on its own. */
} EditContext;

/* Toolbar tools, in the order of ACT_SELECT..ACT_BRUSH. */
typedef enum { TOOL_SELECT, TOOL_MARQUEE, TOOL_SLICE, TOOL_DELETE, TOOL_BRUSH, TOOL_COUNT } Tool;

/* The mouse drag in progress (Editor.drag). */
typedef enum {
    DRAG_NONE,
    DRAG_CLIP_MOVE,
    DRAG_CLIP_RESIZE,
    DRAG_LABEL_WIDTH, /* The playlist's channel label divider. */
    DRAG_NOTE,        /* Move, or resize when drag_edge is set. */
    DRAG_NOTE_BEND,   /* Shift + drag on a note draws its pitch bend. */
    DRAG_BEND_POINT,  /* A pitch bend point on the piano roll. */
    DRAG_BRUSH,
    DRAG_VELOCITY,
    DRAG_EVENT_PRESS, /* A press in an event lane that is not yet a point or a line. */
    DRAG_EVENT_LINE,
    DRAG_EVENT_POINT, /* One point, or the selected group. */
    DRAG_GRAPH_PAINT,
    DRAG_GRAPH_MOVE,
    DRAG_GRAPH_ERASE
} DragKind;

typedef enum { CLIPBOARD_COPY, CLIPBOARD_CUT, CLIPBOARD_PASTE } ClipboardAction;

#define DM_MIDI_LATENCY_LIMIT_MS 1000
#define ED_UNDO_LIMIT 64 /* Undo and redo steps kept, each. */

enum { ED_SONG_EVENT_ROWS = 1 + 16 };

typedef struct {
    DmBytes groups;
    uint64_t revision;
    double left, right;
    size_t events, automation;
    unsigned expanded;
} SongEventCache;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    DmAudio *audio;
    DmMidiInput *midi_input;
    struct DmEditorMidiInput *midi_input_state;
    char input_names[DM_MIDI_INPUTS][DM_MIDI_NAME]; /* Saved MIDI input devices. */
    DmMidiInputOptions input_options;
    DmInputControllerPolicy input_controllers;
    float input_latency_ms; /* NAN = one audio buffer. */
    bool input_thru, input_pickup, input_preferences_open;
    DmBytes midi_bindings;
    EdMidiLearn midi_learn;
    uint64_t midi_binding_session, midi_mix_last;
    bool midi_mix_pending, midi_mix_dirty, midi_clear_bindings, midi_learn_any;
    /* A Point / Graph switch waiting for confirmation on this pattern and event type. */
    uint32_t graph_switch_uid;
    int graph_switch_event, graph_switch_graph;
    float event_lane_height; /* Last drawn event lane height, for its value rows (0 none yet). */
    bool graph_switch;
    DmProject project;
    uint64_t mixer_peak_tick;
    float mixer_peaks[DM_PARTS];
    float mixer_peak_targets[DM_PARTS];
    /* Volume and pan the song is playing with, shown by the mixer during song playback. */
    uint8_t mixer_live_volume[DM_PARTS], mixer_live_pan[DM_PARTS];
    bool mixer_live;
    uint64_t spectrum_tick;
    float spectrum[DM_SPECTRUM_BANDS], spectrum_targets[DM_SPECTRUM_BANDS];
    float spectrum_top; /* Highest analyzer band edge; follows the sound's treble. */
    bool active, dirty, running, piano, roll, shortcuts, config, preferences, about, export_window,
        confirm, dialog_busy, record, wait_input, overwrite, metronome;
    bool smoke;
    bool song_event;
    bool font_request, font_queued;
    bool pending_save, pending_save_as;
    bool monitor;
    bool percussion_window, percussion_listening, percussion_midi_listening, percussion_reveal;
    int combine_request; /* 0 none, 1 original source ports, 2 all ports; awaiting a warning. */
    char combine_channels[64];
    int percussion_note;
    uint8_t percussion_global[128], percussion_draft[128];
    uint8_t percussion_midi_global[128], percussion_midi_draft[128]; /* Incoming note + 1. */
    char percussion_feedback[160];

    DmMetadataMode export_metadata;
    struct DmExportPlugins *export_plugins;
    struct DmEditorSamples *samples;
    struct DmSaveJob *save_job;
    struct DmPatternJob *pattern_job;
    bool generate_window;
    bool note_color_popup, pattern_color_popup;
    int pattern_color_target; /* Pattern index, or -1 for the selected placements. */
    uint8_t note_ports[16];
    uint64_t export_revision;
    char export_source[DM_PATH];
    char dialog_suggested[DM_PATH];
    char audio_export_pending[DM_PATH], audio_exported[DM_PATH];
    char audio_export_message[512]; /* Digital Audio's own result, apart from the status bar. */
    bool export_shown;              /* The export window was open last frame. */
    uint64_t ports_revision, ports_session; /* Project state the port cache describes. */
    bool ports_cached;
    bool channel_ports[16][DM_NOTE_PORTS];
    bool pattern_mode, pattern_highlight, linked_highlight, recording_take, take_snapshot,
        erase_active;
    bool duration_beats, focus_pattern, clip_moved;
    bool wait_armed, take_started;
    /* Song beat where the recording take's pattern beat 0 falls; set by the take's first note. */
    double take_origin;
    bool take_origin_set;
    int preview_program, preview_route, preview_source_channel;
    bool preview_override, channel_selected, event_marquee, midi_events;
    int midi_event_kind, midi_event_index;
    uint64_t midi_selection_revision;
    float midi_split, song_event_split; /* The event list's share of the panels' width. */
    double event_beat, song_event_anchor;
    int song_event_kind, song_event_channel;
    bool song_event_add, song_event_bound;
    int ui_size;
    bool os_cursor;
    bool repository_history; /* Export to Repository records history; else .bkp copies. */
    bool event_ruler_drag;
    bool event_free;           /* Alt was held when an event point or line was started. */
    bool event_connect;        /* Shift was held: the clicked point connects to the nearest one. */
    uint64_t event_press_tick; /* When the pending event-lane press began (drag 12). */
    bool midi_values, midi_hex;
    char midi_error[160];
    DmBytes midi_draft, midi_text;
    int midi_source_track, midi_source_port;
    bool drop_active, drop_handled;
    UiPoint drop_position;
    UiRect preferences_rect, info_rect;
    UiPoint event_marquee_start;
    UiPoint graph_last; /* Pointer on the previous frame of a graph paint or erase drag. */
    DmBytes record_original, midi_rows;
    /* Playlist Events row: one group per occupied bar in the visible range, rebuilt only when
       the project or the range changes. */
    /* Song, then one cache per MIDI channel. */
    SongEventCache song_event_groups[ED_SONG_EVENT_ROWS];
    bool curve_drag, curve_changed, curve_overlay;
    int curve_left;
    float curve_y;
    double curve_origin, curve_sensitivity;
    bool automation_all, edit_moved, roll_auto_hidden;
    EditFocus edit_focus; /* The one "where am I": see ed_context for the timeline it implies. */
    int pan_context;      /* EditContext */
    float mixer_height, label_width, automation_height;
    int piano_octave, pattern_drag, rename_pattern;
    bool pattern_drag_on_playlist; /* The dragged card hides over the playlist's drop outline. */
    char rename_text[64];
    uint32_t accent, background;
    DmChannel reset_channels[16];
    double start_beat[2], cursor_beat[2];
    bool scrub, scrub_playing;
    int scrub_context; /* EditContext */
    double scrub_target, scrub_origin;
    double loop_start[2], loop_end[2], loop_anchor;
    bool loop_enabled[2], loop_drag;
    int loop_context; /* EditContext */
    int piano_channel;
    uint8_t held_channels[SDL_SCANCODE_COUNT];
    bool automation_browser, channel_expanded[16], lane_settings, lfo_editing;
    bool remove_note_row;
    int remove_note_row_channel;
    int lane_channel, lane_index;
    bool lfo_center_drag;
    double lfo_center_origin;
    float lfo_center_y, lfo_center_range;
    int playlist_context_clip;
    int selected_clip;
    DmNote *drag_notes;
    size_t drag_note_count;
    bool automation_raw;
    bool last_automation_valid;
    DmAutomation last_automation;
    int automation_node, context_note, context_point;
    double node_beat;
    int node_value;
    float node_y, automation_center;
    SDL_DisplayID display;
    bool minimized;

    char new_name[128];
    float new_bpm;
    int new_numerator, new_denominator;
    char font_next[DM_PATH];
    char midi_folder[DM_PATH], font_folder[DM_PATH], export_folder[DM_PATH],
        preferred_font[DM_PATH], output_name[256];
    int smoke_frames;
    int pattern_channel, selected_pattern, selected_channel, instrument_channel, capture,
        drag_index, drag_edge, export_type;
    int tool; /* Tool */
    int drag; /* DragKind */
    uint32_t last_note_pattern_uid;
    DmAudioOptions audio_options;
    double drag_beat, drag_length, record_start[128];
    int drag_key;
    bool edit_feedback;
    int instrument_program;
    int instrument_port, instrument_bank;
    bool instrument_source_dirty, instrument_source_inherit;
    DmPatchReplacement instrument_source;
    DmPatchReplacement midi_sample_source;
    bool midi_sample_dirty, midi_sample_inherit, instrument_audition;
    bool instrument_new, instrument_drums, instrument_remove;
    bool instrument_preview; /* The instrument window also sets the pattern editor's dropdown. */
    int brush_quarter, brush_pitch, brush_anchor;
    double record_origin[128];
    uint64_t project_session, clipboard_session;
    int clipboard_port, clipboard_sequence;
    DmBytes event_drag, clip_drag;
    /* Bend-range controllers of one pattern, kept while a pitch-bend drawing loop runs; see
       ed_pitch_cache_begin. */
    DmBytes pitch_controls;
    int pitch_cache_pattern, pitch_cache_anchor;
    bool pitch_cache, pitch_cache_ready, pitch_cache_anchored;
    float sidebar, roll_height;
    View playlist_view, roll_view;
    UiPoint marquee_start;
    DmBytes marquee_selection, event_marquee_selection; /* Selection at mouse-down. */
    bool marquee;
    int automation_cc;
    int bend_range;
    bool automation_open;
    char path[DM_PATH], pending_path[DM_PATH], status[512], hover[256], settings_path[DM_PATH],
        font_pending[DM_PATH];
    char status_shown[512];         /* Status text when it was last posted; see ed_info_box. */
    uint64_t status_time;           /* When the status was posted, in SDL ticks. */
    char base_path[DM_PATH];        /* Folder holding the executable, ending with a separator. */
    struct DmUpdate *update;        /* Update checker, created on first use; see editor/update.c. */
    int update_mode, update_choice; /* Saved check setting (1 On Launch) and the window choice. */
    bool update_window, update_prompted, update_announced, update_install, update_restart;
    bool update_prompt_pending; /* First-launch question shown once a file is open. */
    uint64_t sample_hash;       /* Sample project fingerprint when first opened; 0 if unknown. */
    int pending_action, new_mode, device;
    Binding bindings[ACT_COUNT];
    MouseBinding gestures[GESTURE_COUNT];
    int gesture_capture; /* Gesture waiting for a mouse press in Keyboard Shortcuts, or -1. */
    bool gesture_capture_armed; /* The click that started the capture has been released. */
    /* A copy drag: the button it uses (-1 none), and whether the copies are still to be made on
       the first movement. */
    int copy_button;
    bool copy_pending, copy_was_selected;
    /* A plain press on an item already in a selection: released without moving, it selects only
       that item. */
    bool lone_click;
    DmBytes undo[ED_UNDO_LIMIT], redo[ED_UNDO_LIMIT], clipboard, event_selection;
    DmBytes event_clipboard; /* Event buttons keep their own fixed-position clipboard. */
    bool event_clipboard_all;
    int undo_count, redo_count;
    int held[SDL_SCANCODE_COUNT];
    char recent[10][DM_PATH];
    int recent_count;
} Editor;

/* base holds the bundled plugins and assets; data holds the preference files. */
void editor_init(Editor *e, SDL_Window *window, SDL_Renderer *renderer, DmAudio *audio,
                 const char *base, const char *data, bool smoke);
void editor_free(Editor *e);
void ed_update_launch(Editor *e); /* Shows the first-launch prompt or starts a launch check. */
void ed_update_finish(Editor *e); /* Starts a scheduled install as duckmidi exits. */
void editor_background(Editor *e);
void editor_event(Editor *e, const SDL_Event *event);
void editor_open(Editor *e, const char *path);
void editor_frame(Editor *e, float width, float height);
void editor_smoke(Editor *e);
#endif
