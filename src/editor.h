#ifndef DM_EDITOR_H
#define DM_EDITOR_H
#include "audio.h"
#include "duck.h"
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
    ACT_COUNT
} Action;

typedef struct {
    SDL_Scancode key;
    SDL_Keymod mods;
} Binding;

typedef struct {
    float zoom, scroll_x, scroll_y;
    int focus;
} View;

typedef enum {
    FOCUS_NONE,
    FOCUS_PLAYLIST,
    FOCUS_NOTES,
    FOCUS_EVENTS,
    FOCUS_PATTERNS,
    FOCUS_OTHER
} EditFocus;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    DmAudio *audio;
    DmProject project;
    uint64_t mixer_peak_tick;
    float mixer_peaks[DM_PARTS];
    float mixer_peak_targets[DM_PARTS];
    uint64_t spectrum_tick;
    float spectrum[DM_SPECTRUM_BANDS], spectrum_targets[DM_SPECTRUM_BANDS];
    bool active, dirty, running, piano, roll, shortcuts, config, preferences, about, export_window,
        confirm, dialog_busy, record, wait_input, overwrite, metronome;
    bool smoke;
    bool song_event;
    bool font_request, font_queued;
    bool pending_save, pending_save_as;
    bool monitor;
    bool percussion_window, percussion_listening, percussion_reveal;
    int combine_request; /* 0 none, 1 original source ports, 2 all ports; awaiting a warning. */
    char combine_channels[64];
    int percussion_note;
    uint8_t percussion_global[128], percussion_draft[128];
    char percussion_feedback[160];

    DmMetadataMode export_metadata;
    struct DmExportPlugins *export_plugins;
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
    bool export_shown;                      /* The export window was open last frame. */
    uint64_t ports_revision, ports_session; /* Project state the port cache describes. */
    bool ports_cached;
    bool channel_ports[16][DM_NOTE_PORTS];
    bool pattern_mode, pattern_highlight, linked_highlight, recording_take, take_snapshot,
        erase_active;
    bool duration_beats, focus_pattern, clip_moved;
    bool wait_armed, take_started;
    int preview_program, preview_route, preview_source_channel;
    bool preview_override, channel_selected, event_marquee, event_marquee_overlay, midi_events;
    int midi_event_kind, midi_event_index;
    float midi_split;
    int ui_size;
    bool os_cursor;
    bool event_ruler_drag;
    bool event_free;           /* Alt was held when an event point or line was started. */
    bool event_connect; /* Shift was held: the clicked point connects to the nearest one. */
    uint64_t event_press_tick; /* When the pending event-lane press began (drag 12). */
    bool midi_values, midi_hex;
    char midi_error[160];
    DmBytes midi_draft, midi_text;
    int midi_source_track, midi_source_port;
    bool drop_active, drop_handled;
    UiPoint drop_position;
    UiRect preferences_rect, info_rect;
    UiPoint event_marquee_start;
    DmBytes record_original, midi_rows;
    bool curve_drag, curve_changed, curve_overlay;
    int curve_left;
    float curve_y;
    double curve_origin, curve_sensitivity;
    bool automation_all, edit_moved, roll_auto_hidden;
    EditFocus edit_focus;
    int pan_context;
    float mixer_height, label_width, automation_height;
    int piano_octave, transient_pattern, pattern_drag, rename_pattern;
    char rename_text[64];
    uint32_t accent;
    DmChannel reset_channels[16];
    double start_beat[2], cursor_beat[2];
    bool scrub, scrub_playing;
    int scrub_context;
    double scrub_target, scrub_origin;
    double loop_start[2], loop_end[2], loop_anchor;
    bool loop_enabled[2], loop_drag;
    int loop_context;
    int piano_channel;
    uint8_t held_channels[SDL_SCANCODE_COUNT];
    bool automation_browser, channel_expanded[16], lane_settings, lfo_editing;
    int lane_channel, lane_index;
    bool lfo_center_drag;
    double lfo_center_origin;
    float lfo_center_y, lfo_center_range;
    int playlist_context_clip;
    int selected_clip;
    DmNote *drag_notes;
    size_t drag_note_count;
    bool automation_raw, recording_commit;
    bool last_automation_valid;
    DmAutomation last_automation;
    int automation_node, event_edit, event_auto_edit, event_source_kind;
    double node_beat;
    int node_value;
    float node_y, automation_center;
    SDL_DisplayID display;
    bool focused, minimized;

    char new_name[128];
    float new_bpm;
    int new_numerator, new_denominator;
    char font_next[DM_PATH];
    char midi_folder[DM_PATH], font_folder[DM_PATH], export_folder[DM_PATH],
        preferred_font[DM_PATH], output_name[256];
    float event_beat, event_bpm;
    int event_kind, event_program, event_channel, event_numerator, event_denominator, context_note,
        context_point;
    char event_marker[128];
    int smoke_frames;
    int pattern_channel, selected_pattern, selected_channel, instrument_channel, tool, context,
        capture, drag, drag_index, drag_edge, export_type;
    DmAudioOptions audio_options;
    double drag_beat, drag_length, record_start[128];
    int drag_key;
    bool edit_feedback, instrument_accept;
    bool instrument_preview; /* The instrument window also sets the pattern editor's dropdown. */
    bool preferences_raise;  /* Preferences comes to the front on its next frame. */
    int brush_quarter, brush_pitch, brush_anchor;
    double record_origin[128];
    uint64_t project_session, clipboard_session;
    int clipboard_port, clipboard_sequence;
    DmBytes event_drag, clip_drag;
    float sidebar, roll_height;
    View playlist_view, roll_view;
    UiPoint marquee_start;
    bool marquee;
    int automation_cc;
    int bend_range;
    bool automation_open;
    char path[DM_PATH], pending_path[DM_PATH], status[512], hover[256], settings_path[DM_PATH],
        font_pending[DM_PATH];
    char status_shown[512];         /* Status text when it was last posted; see ed_info_box. */
    uint64_t status_time;           /* When the status was posted, in SDL ticks. */
    char base_path[DM_PATH];        /* Folder holding the executable, ending with a separator. */
    struct DmUpdate *update;        /* Update checker, created on first use; see editor_update.c. */
    int update_mode, update_choice; /* Saved check setting (1 On Launch) and the window choice. */
    bool update_window, update_prompted, update_announced, update_install, update_restart;
    bool update_prompt_pending; /* First-launch question shown once a file is open. */
    uint64_t sample_hash;       /* Sample project fingerprint when first opened; 0 if unknown. */
    int pending_action, new_mode, device;
    Binding bindings[ACT_COUNT];
    DmBytes undo[32], redo[32], clipboard, event_selection;
    int undo_count, redo_count;
    int held[SDL_SCANCODE_COUNT];
    SDL_Keymod keymods;
    SDL_Scancode keypress;
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
void editor_frame(Editor *e, float width, float height);
void editor_smoke(Editor *e);
#endif
