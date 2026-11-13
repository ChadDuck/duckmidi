#ifndef DM_EDITOR_INTERNAL_H
#define DM_EDITOR_INTERNAL_H
/* Shared editor implementation contracts; desktop entry points remain in editor.h. */
#include "editor/editor.h"
#include "editor/preferences_file.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLOR(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | 0xff000000u)
#define BG ui_background_shade(12)
#define GRID ui_background_shade(38)
bool ed_record_start(Editor *e);
double ed_record_origin(Editor *e, double beat);
bool ed_record_insert(Editor *e, DmPattern *pattern, int key, int velocity, unsigned port,
                      double start, double end, double origin);
void ed_midi_input_init(Editor *e);
void ed_midi_input_attach(Editor *e, DmMidiInput *input);
void ed_midi_input_restore(Editor *e);
void ed_midi_input_apply(Editor *e);
bool ed_midi_input_activity(const Editor *e, DmMidiInputEvent *event, char name[DM_MIDI_NAME]);
void ed_midi_input_names(Editor *e);
void ed_midi_input_free(Editor *e);
void ed_midi_input_frame(Editor *e);
/* A MIDI key is held that sounds this key (after the percussion mapping), for the piano keys. */
bool ed_midi_input_lit(const Editor *e, int key);
/* Ends held recorded MIDI notes; publish rebuilds playback when the transport keeps running. */
void ed_midi_input_finish(Editor *e, bool publish);
void ed_midi_input_take(Editor *e);
/* Writes pending MIDI recording to the project; see editor/midi_input.c for when it plays. */
void ed_midi_input_flush(Editor *e);
void ed_midi_input_bound_event(Editor *e, DmMidiInputEvent event);
bool ed_midi_target_event(EdMidiTarget target);
bool ed_midi_target_continuous(EdMidiTarget target);
void ed_midi_target_id(EdMidiTarget target, char *id, size_t cap);
bool ed_midi_target_parse(const char *id, EdMidiTarget *target);
void ed_midi_target_label(EdMidiTarget target, char *text, size_t cap);
void ed_midi_source_label(const DmMidiBindingSource *source, char *text, size_t cap);
size_t ed_midi_binding_count(const Editor *e);
EdMidiBinding *ed_midi_binding_at(Editor *e, size_t index);
EdMidiBinding *ed_midi_binding_find(Editor *e, EdMidiTarget target);
bool ed_midi_binding_append(DmBytes *list, EdMidiBinding binding);
bool ed_midi_bindings_attach(Editor *e, DmMidiInput *input);
bool ed_midi_bindings_replace(Editor *e, DmBytes *list);
bool ed_midi_bindings_cancel(Editor *e);
void ed_midi_binding_clear(Editor *e, EdMidiTarget target);
void ed_midi_bindings_clear_all(Editor *e);
void ed_midi_binding_learn(Editor *e, EdMidiTarget target, bool any_device);
bool ed_midi_binding_learning(const Editor *e, EdMidiTarget target);
void ed_midi_bindings_reset_pickup(Editor *e);
void ed_midi_bindings_flush(Editor *e, uint64_t now);
void ed_midi_bindings_tick(Editor *e, uint64_t now);
void ed_midi_binding_event(Editor *e, DmMidiInputEvent event, const char *device, bool events_only);
bool ed_mixer_apply(Editor *e, DmPortMix before, DmPortMix mix);
size_t ed_automation_visible_first(Editor *e, double beat, bool overlay);
uint32_t ed_port_color(Editor *e, unsigned port);
void ed_pattern_color_request(Editor *e, int pattern);
void ed_pattern_color_dialog(Editor *e);
unsigned ed_note_port(Editor *e);
float ed_port_buttons_width(Editor *e);
void ed_port_buttons(Editor *e);
bool ed_color_notes(Editor *e, unsigned port);
bool ed_delete_note_port(Editor *e, unsigned port);
void ed_note_color_dialog(Editor *e);
#define TEXT ui_theme_text(false)
#define DIM ui_theme_text(true)
#define GREEN (e->accent)
#define GOLD COLOR(245, 196, 111)
#define ED_DEFAULT_VELOCITY 100 /* Velocity of notes drawn or recorded in the editor. */

enum {
    DIALOG_OPEN = 1,
    DIALOG_SAVE,
    DIALOG_MIDI,
    DIALOG_WAVE,
    DIALOG_FONT,
    DIALOG_EXPORT_PLUGIN,
    DIALOG_REPOSITORY,
    DIALOG_PLUGIN_FOLDER, /* Preferences: an exporter's repository folder */
    DIALOG_SAMPLE_FILES,
    DIALOG_SAMPLE_FOLDER
};

#include "../../include/duckmidi_export_plugin.h"
bool ed_samples_available(Editor *e);
void ed_samples_open(Editor *e);
void ed_samples_close(Editor *e);
void ed_samples_free(Editor *e);
void ed_samples_poll(Editor *e);
void ed_samples_draw(Editor *e);
bool ed_samples_key(Editor *e, const SDL_KeyboardEvent *key);
bool ed_samples_history(Editor *e, bool redo, bool apply);
bool ed_samples_drop(Editor *e, const SDL_DropEvent *event);
bool ed_samples_drop_target(Editor *e, UiPoint at, UiRect *rect);
void ed_samples_picker_result(Editor *e, const SDL_Event *event);
void ed_sample_catalog_refresh(Editor *e);
bool ed_sample_source_picker(Editor *e, DmPatchKey key, const DmPatchReplacement *current,
                             DmPatchReplacement *selected, bool *inherit);
bool ed_sample_store_replacement(Editor *e, const DmPatchReplacement *selection, bool inherit);
void ed_sample_clear_route(DmProject *project, int port, int channel);
void ed_sample_program_source(Editor *e);
size_t ed_export_count(Editor *e);
const char *ed_export_name(Editor *e, size_t index);
bool ed_export_settings(Editor *e, size_t index, DmeUi *ui);
bool ed_export_set(Editor *e, size_t index, const char *id, DmeValue value);
void ed_export_start(Editor *e, size_t index, const char *destination);
void ed_export_poll(Editor *e);
void ed_export_free(Editor *e);
void ed_export_controls(Editor *e);
void ed_export_history_toggle(Editor *e);
void ed_export_history_panel(Editor *e, UiRect main);
bool ed_export_history_visible(Editor *e);
void ed_export_catalog_changed(Editor *e);
void ed_export_warning_dialog(Editor *e);
bool ed_export_review_open(Editor *e);
void ed_export_review_decide(Editor *e, int decision);
void ed_export_review_dialog(Editor *e);
bool ed_export_warning_open(Editor *e);
void ed_export_warning_close(Editor *e);
void ed_export_error(Editor *e, const char *message);
void ed_export_plugin_errors(Editor *e);
bool ed_export_busy(Editor *e);
bool ed_export_suggest(Editor *e, const char *extension, char *out, size_t capacity);
void ed_open_file_location(Editor *e, const char *source);
void ed_open_audio_location(Editor *e);
void ed_export_reset_results(Editor *e);
bool ed_export_repository_picker(Editor *e, const char **start);
void ed_export_repository_done(Editor *e, const char *folder);
void ed_export_to_repository(Editor *e);
bool ed_export_installs(Editor *e, size_t index);
const char *ed_export_repository(Editor *e, size_t index);
void ed_export_set_repository(Editor *e, size_t index, const char *folder);
bool ed_export_folder_picker(Editor *e, size_t index);
const char *ed_export_folder_start(Editor *e);
void ed_export_folder_done(Editor *e, const char *folder);
bool ed_export_picker(Editor *e, const SDL_DialogFileFilter **filter, const char **suggested);
const char *ed_export_extension(Editor *e);
void ed_export_picker_done(Editor *e, const char *path);

enum { PENDING_NEW = 1, PENDING_OPEN, PENDING_PATH, PENDING_QUIT };

typedef struct {
    size_t index;
    DmAutomation original;
    float pitch;
} EventDragItem;

typedef struct {
    double beat;
    int index;
    bool automation;
} MidiRow;

extern uint32_t ed_dialog_event;

typedef enum {
    ACTIONS_TRANSPORT,
    ACTIONS_NAVIGATION,
    ACTIONS_PATTERNS,
    ACTIONS_ZOOM,
    ACTIONS_WINDOWS,
    ACTION_GROUPS
} ActionGroup;

enum {
    ACTION_REPEATS = 1,   /* Runs again while its key is held. */
    ACTION_WORKSPACE = 2, /* Acts on the playlist or pattern editor: off while a window is open. */
    ACTION_PIANO_OFF = 4  /* Only while the computer keyboard piano is off. */
};

typedef struct {
    const char *name;
    SDL_Scancode key;
    SDL_Keymod mods;
    ActionGroup group;
    unsigned flags;
} ActionInfo;

extern const ActionInfo ed_actions[ACT_COUNT];
extern const GestureInfo ed_gestures[GESTURE_COUNT];
bool ed_mouse_down(UiMouse mouse, int button);
bool ed_gesture_pressed(Editor *e, Gesture gesture, UiMouse mouse);
/* The gesture's modifier keys are down. The caller tests the button, drag or wheel it applies to.
 */
bool ed_gesture_held(const Editor *e, Gesture gesture, UiMouse mouse);
/* Makes a copy of the mouse state report the gesture's modifiers as held or released. */
void ed_gesture_set(const Editor *e, Gesture gesture, UiMouse *mouse, bool held);
bool ed_gesture_capture(Editor *e, UiMouse mouse);
bool ed_gesture_format(MouseBinding binding, char *out, size_t cap);
bool ed_gesture_parse(const char *text, MouseBinding *binding);
void ed_focus_edit(Editor *e, EditFocus focus);
/* The timeline the focused section belongs to: the pattern editor for notes and events, otherwise
   the playlist. */
EditContext ed_context(const Editor *e);
/* Moves focus to the timeline's own section unless a section of that timeline already has it. */
void ed_focus_timeline(Editor *e, EditContext context);
bool ed_velocity_editing(Editor *e);
EditFocus ed_selection_focus(Editor *e);
void ed_toggle_roll(Editor *e);
void ed_reset_layout(Editor *e);
int ed_channel_program(Editor *e, int channel);
int ed_placed_program(Editor *e);
void ed_preview_channel(Editor *e, int channel, bool placed);
bool ed_pattern_placed(Editor *e); /* The open pattern has a placement on the playlist. */
/* The pattern editor's instrument dropdown and its buttons are shown for a pattern not on the
   playlist, and for a placed one only in pattern playback. */
bool ed_preview_instrument_shown(Editor *e);
/* Updates the preview instrument from its source unless one was chosen in the dropdown: a placed
   pattern follows its placement's channel, and a pattern not on the playlist follows the selected
   playlist channel. Reports whether the pattern channel or instrument changed. */
bool ed_follow_preview(Editor *e);
/* Applies ed_follow_preview once per frame, updating the keyboard and pattern playback. */
void ed_follow_preview_frame(Editor *e);
void ed_select_placement_route(Editor *e, int index);
void ed_open_pattern(Editor *e, int id, bool created);
SDL_Keymod ed_normalized(SDL_Keymod m);
void ed_defaults(Editor *e);
void ed_message(Editor *e, const char *text);
DmPattern *ed_pattern(Editor *e);
void ed_bind_pattern_channel(Editor *e);
void ed_clear_history(EdHistory *steps, int *count);
/* Records the project as an undo step before an edit. False when it could not be recorded: the
   caller must then leave the project unchanged. During a recording take every edit joins the
   take's step. */
bool ed_checkpoint(Editor *e);
/* ed_checkpoint for an edit that exchanges editor state kept per channel between channels a and
   b; undo and redo exchange it back. */
bool ed_checkpoint_swap(Editor *e, int a, int b);
/* The right button may clear a selection or erase: Shift (context menus) is up and no loop,
   curve or copy drag is using the mouse. */
bool ed_right_free(const Editor *e, UiMouse mouse);
/* Before removing an item under a held right button: the whole stroke is one undo step.
   False when the stroke's undo step could not be recorded: erase nothing. */
bool ed_erase_step(Editor *e);
/* Restores the project saved by the checkpoint just taken and drops it, for an edit that failed
   before completing. Does nothing when that checkpoint joined a recording take's step. */
void ed_checkpoint_abandon(Editor *e);
bool ed_add_note(Editor *e, DmPattern *p, DmNote note);
double ed_pattern_length(Editor *e);
bool ed_transport_publish(Editor *e, double seconds, bool playing);
double ed_transport_length(Editor *e);
void ed_publish(Editor *e);
void ed_changed(Editor *e);
void ed_request_font(Editor *e, const char *path);
void ed_commit(Editor *e);
void ed_commit_channels(Editor *e);
int ed_note_identity(const void *left, const void *right);
void ed_undo(Editor *e, bool redo);
void ed_add_recent(Editor *e, const char *path);
void ed_preference_save(Editor *e);
void ed_preference_load(Editor *e);
void ed_dialog(Editor *e, int kind);
bool ed_save_to(Editor *e, const char *path);
void ed_save_poll(Editor *e);
void ed_save_free(Editor *e);
bool ed_save_busy(Editor *e);
void ed_save_cancel(Editor *e);
enum { ED_AUTOSAVE_INTERVAL_MS = 5 * 60 * 1000, ED_AUTOSAVE_KEEP = 5 };
void ed_autosave_init(Editor *e, const char *settings_folder);
/* The background poll supplies elapsed wall-clock milliseconds, independent of playback. */
void ed_autosave_step(Editor *e, uint64_t now);
bool ed_autosave_busy(const Editor *e);
const char *ed_autosave_question(const Editor *e);
bool ed_autosave_restore(Editor *e);
void ed_autosave_dialog(Editor *e);
void ed_autosave_free(Editor *e);
bool ed_load_autosave(Editor *e, const char *path, const char *original);
void ed_generate_start(Editor *e);
void ed_generate_poll(Editor *e);
void ed_generate_free(Editor *e);
bool ed_generate_busy(Editor *e);
bool ed_generate_apply(Editor *e);
const DmProject *ed_generate_preview(Editor *e);
void ed_generate_dialog(Editor *e);
void ed_save(Editor *e, bool as);
void ed_new_project(Editor *e);
void ed_load(Editor *e, const char *path);
void ed_combine_source_ports(Editor *e, bool all);
void ed_request_combine(Editor *e, bool all);
void ed_combine_dialog(Editor *e);
void ed_pending_execute(Editor *e);
void ed_request(Editor *e, int ed_action, const char *path);
void ed_update_dialog(Editor *e);
void ed_update_open(Editor *e);
void ed_cancel_wait(Editor *e);
/* Play / pause from the playhead. Pausing returns to the set playhead position; with hold
   (MIDI transport controls) it stays where playback stopped, and the next play continues there. */
void ed_play(Editor *e);
void ed_play_hold(Editor *e, bool hold);
void ed_recording_finished(Editor *e);
void ed_seek(Editor *e, double beat);
bool ed_point_editing(Editor *e);
void ed_remove_placement(Editor *e, size_t index);
void ed_delete_selection(Editor *e);
void ed_select_all(Editor *e);
/* Beats between a selection spanning first..end and its duplicate: whole beats, and whole bars
   once the selection is a bar long when bars is set (notes and events). */
double ed_duplicate_span(Editor *e, double first, double end, bool bars);
void ed_duplicate_clips(Editor *e);
void ed_duplicate_notes(Editor *e);
void ed_deselect(Editor *e);
bool ed_deselect_notes(DmPattern *p);
void ed_clear_pattern_selection(Editor *e);
void ed_deselect_patterns(Editor *e); /* Keeps the pattern editor shown or hidden. */
bool ed_deselect_focused(Editor *e);
int ed_piano_key(SDL_Scancode key);
int ed_piano_index(SDL_Scancode key);
SDL_Scancode ed_piano_scancode(int index);
bool ed_route_drums(const Editor *e, int route);
int ed_keyboard_note(Editor *e, SDL_Scancode key, bool drums);
void ed_percussion_open(Editor *e);
void ed_percussion_dialog(Editor *e);
bool ed_percussion_key(Editor *e, const SDL_KeyboardEvent *key);
void ed_percussion_save(Editor *e, bool project);
void ed_percussion_close(Editor *e);
/* Incoming MIDI note to sounding note for the active percussion mapping. */
void ed_percussion_midi_map(const Editor *e, uint8_t map[MIDI_KEYS]);
/* The same map from the Percussion window's unsaved MIDI mapping. */
void ed_percussion_midi_draft_map(const Editor *e, uint8_t map[MIDI_KEYS]);
/* The instrument window receives the piano keys: it is open and in front. */
bool ed_instrument_topmost(Editor *e);
bool ed_percussion_midi_listen(Editor *e, DmMidiInputEvent event);
bool ed_begin_record_take(Editor *e);
double ed_recording_origin(Editor *e, double beat);
void ed_record_note(Editor *e, int note);
double ed_heard_beat(Editor *e);
void ed_stop_edit_feedback(Editor *e);
void ed_finish_edit_feedback(Editor *e);
void ed_edit_feedback(Editor *e, float pitch);
void ed_release_piano(Editor *e);
void ed_transpose(Editor *e, int steps);
void ed_quantize_notes(Editor *e);
bool ed_reserved(SDL_Scancode key, SDL_Keymod mods);
void ed_action_zoom(Editor *e, int a);
void ed_action(Editor *e, int a);
void ed_paste(Editor *e);
void ed_clipboard(Editor *e, ClipboardAction action);
void ed_clipboard_note_bends(Editor *e, bool cut);
int ed_pattern_keyboard_channel(Editor *e);
int ed_keyboard_channel(Editor *e);
void ed_piano_press(Editor *e, SDL_Scancode key, SDL_Keymod mods, bool repeat);
void ed_shortcut(Editor *e, SDL_Scancode key, SDL_Keymod raw, bool repeat);
bool ed_instrument_note(Editor *e, const SDL_KeyboardEvent *key);
bool ed_inside(UiPoint p, UiRect r);
float ed_clampf(float n, float lo, float hi);
double ed_snap(double beat, double grid, bool free);
void ed_hover(Editor *e, const char *text);
void ed_info_box(Editor *e);
void ed_toggle_record(Editor *e);
void ed_new_song_event(Editor *e);
void ed_action_tip(Editor *e, const char *name, Action ed_action);
const char *ed_binding_text(Editor *e, Action action, char *out, size_t capacity);
void ed_toolbar(Editor *e);
void ed_menu(Editor *e);
void ed_choose_pattern_card(Editor *e, int id, bool selected);
void ed_duplicate_pattern(Editor *e, int pattern);
void ed_step_pattern(Editor *e, bool automation, int direction);
uint32_t ed_pattern_color(Editor *e, int id);

/* Thumbnail geometry: x = x + (beat - offset) * x_scale; y = y - (key - 48) * y_scale. */
typedef struct {
    UiRect visible;
    float x, x_scale, y, y_scale, top, bottom;
    double offset;
    uint32_t color;
} NoteThumbnail;

void ed_note_thumbnail(const DmPattern *p, NoteThumbnail thumbnail);
void ed_pattern_panel(Editor *e, float width, float height);
bool ed_channel_has_clips(Editor *e, int channel);
int ed_available_channel(Editor *e);
void ed_mixer(Editor *e, float width, float height);
const bool *ed_channel_ports(Editor *e, int channel);
int ed_channels(Editor *e, int *list);
#define ED_ZOOM_MIN .15f
#define ED_ZOOM_MAX 8.0f
/* Zooms a timeline with the wheel, keeping the beat x pixels from its left edge in place. */
void ed_wheel_zoom(View *v, float wheel, float x);
void ed_grid_scroll(Editor *e, View *v, UiRect canvas, float content_width, float content_height,
                    float row, bool consumed);
void ed_horizontal_scroll(View *v, UiRect canvas, float total);
double ed_view_playhead(Editor *e, int context, bool playing, double beat, bool *visible);
void ed_ruler(Editor *e, UiRect r, View *v, float scale, float label);
uint32_t ed_grid_color(Editor *e, double beat);
double ed_pattern_seek_origin(Editor *e);
double ed_pattern_region_origin(Editor *e);
void ed_scrub_publish(Editor *e, double beat, int context, bool playing);
bool ed_clear_loop(Editor *e);
void ed_timeline_drag(Editor *e, UiRect r, View *v, float scale, float label, int context,
                      bool hovered);
int ed_place_pattern(Editor *e, int id, int encoded, double beat);
void ed_split_note(DmPattern *p, size_t index, double at);
void ed_split_clip(Editor *e, size_t index, double at);
bool ed_intersects(UiRect a, UiRect b);
/* Modifier clicks (see SelectPick). A press that may be a click calls ed_pick_begin, which keeps
   the selection at the press; ed_pick_selects then decides each item from that selection,
   whether the item is the one clicked, and whether it lies in the click's range from the
   anchor. Items without an order to range over pass ranged as before || clicked, so a range
   click adds them. */
SelectPick ed_select_pick(const Editor *e, UiMouse mouse);
bool ed_pick_selects(SelectPick pick, bool before, bool clicked, bool ranged);
bool ed_pick_begin(Editor *e, EditFocus focus, UiMouse mouse, int hit);
bool ed_pick_moved(Editor *e, UiPoint start, UiMouse mouse); /* Stays true once moved. */
bool ed_pick_clicked(Editor *e, UiPoint start, UiMouse mouse); /* Released without moving. */
bool ed_picked_before(const Editor *e, size_t index);
/* A marquee press with hit the item under it: until the pointer moves it is a click, and the
   items keep their selection until the release applies the click's pick. */
void ed_marquee_begin(Editor *e, EditFocus focus, UiMouse mouse, int hit);
bool ed_marquee_retains(Editor *e, size_t index, bool shift);
bool ed_marquee_selects(Editor *e, size_t index, bool inside, bool ranged, UiMouse mouse);
/* Right-drag erasing covers the pointer's whole path, not only where each frame lands it:
   ed_sweep gives this frame's stretch, from where the pointer was on the previous frame (or
   from here when the drag starts or did not continue through the previous frame), and each
   surface erases the items whose boxes ed_sweep_hits. */
void ed_sweep(Editor *e, UiMouse mouse, UiPoint *from, UiPoint *to);
bool ed_sweep_hits(UiPoint from, UiPoint to, UiRect box);
UiRect ed_marquee_box(Editor *e, UiMouse mouse);
bool ed_black_key(int step);

/* Low bits preserve channel + lane * 16; the note row occupies a separate digit.
   The reserved lane holds channel events. */
enum {
    ED_ROW_CHANNEL_EVENTS = DM_LANES + 1,
    ED_ROW_LANES,
    ED_PLAYLIST_ROWS = 16 * (DM_NOTE_ROWS + DM_LANES + 1)
};

static inline int ed_row_code(int channel, int lane, int note_row) {
    return channel + 16 * (lane + ED_ROW_LANES * note_row);
}

static inline int ed_row_channel(int code) {
    return code % 16;
}

static inline int ed_row_lane(int code) {
    return code / 16 % ED_ROW_LANES;
}

static inline int ed_row_note(int code) {
    return code / (16 * ED_ROW_LANES);
}

int ed_playlist_channels(Editor *e, int *list);
void ed_lane_label(const Editor *e, int channel, int lane, char *text, size_t capacity);

/* Pixels between one channel's playlist rows and the next, and between the sections of one
   channel (note rows, Events row, automation subrows). */
enum { ED_CHANNEL_GAP = 6, ED_SECTION_GAP = 3 };

typedef struct {
    float top, height;
    float gap; /* Separator space above the row; nonzero on a channel's first row. */
} PlaylistRow;

/* Index zero is Song Events; count + 1 returns the content's bottom edge. */
PlaylistRow ed_playlist_row(const int *list, int count, int index, float row);
int ed_playlist_row_at(const int *list, int count, float y, float row);
bool ed_add_note_row(Editor *e, int channel);
bool ed_remove_note_row(Editor *e, int channel);
void ed_playlist_label_action(Editor *e, int channel, int lane, UiMouse mouse);
void ed_playlist_label(Editor *e, UiRect r, float label, float row, const int *list, int i, float y,
                       UiMouse mouse);
const char *ed_event_label(const DmBytes *m);
void ed_song_event_open_bar(Editor *e, double beat, bool add, int channel);
double ed_song_event_bar(const Editor *e, double beat, double *start, double *end);
size_t ed_song_event_rows(Editor *e, double start, double end, int channel);
void ed_song_event_pick(Editor *e, size_t clicked, SelectPick pick);
size_t ed_song_event_pick_count(Editor *e);
bool ed_song_event_picked(Editor *e, MidiRow row);
bool ed_song_events_move(Editor *e, double value, bool absolute);
void ed_song_events_move_panel(Editor *e);
void ed_resize_clip(Editor *e, DmClip *clip, double length);
void ed_move_clip(Editor *e, DmClip *clip, int encoded, double beat);
void ed_playlist_drag(Editor *e, UiRect r, View *v, float row, float scale, const int *list,
                      int count, UiMouse mouse);
void ed_marquee_outline(Editor *e, EditFocus focus, UiMouse mouse);
void ed_playlist_hit_click(Editor *e, UiMouse mouse, int hit, double beat);
void ed_playlist_click(Editor *e, UiRect r, UiRect grid, View *v, float row, float scale,
                       const int *list, int count, UiMouse mouse, int hit);
void ed_playlist_remove(Editor *e, int hit);
void ed_playlist(Editor *e, float width, float height);
DmPitchState ed_pattern_pitch_state(const DmProject *p, int pattern_id, double beat, unsigned port);
DmPitchState ed_pitch_state(Editor *e, double beat);
int ed_curve_id(Editor *e);
int ed_nearest_note(Editor *e, double beat);
int ed_curve_anchor(Editor *e, double beat);
int ed_event_value(DmAutomation a);
void ed_set_event_value(DmAutomation *a, int value);
DmAutomation ed_curve_node(Editor *e, double beat, int value, int cc, int id, int anchor);
int ed_value_before(Editor *e, double beat, int cc);
void ed_curve_range(Editor *e, double beat, int range, int cents, int msb, int lsb, int id);
void ed_automation_line(Editor *e, double start, double end, int first, int last, int cc);
void ed_note_bend_line(Editor *e, double start, double end, int value, DmNote *note);
uint32_t ed_next_bend_id(Editor *e);
double ed_note_bend_offset(Editor *e, const DmNote *notes, size_t count, double offset);
bool ed_automation_line_overlaps(Editor *e, double start, double end, int cc);
void ed_follow_note_curves(Editor *e, int pattern, const DmNote *before, size_t count,
                           double previous_offset);
void ed_resize_note_group(Editor *e, DmPattern *p, double delta, bool free);
bool ed_gesture_moved(Editor *e, UiMouse mouse);
bool ed_begin_edit_move(Editor *e);
bool ed_automation_matches(Editor *e, DmAutomation au);
bool ed_same_event(DmAutomation a, DmAutomation b);
bool ed_event_selected(Editor *e, size_t index);
bool ed_pedal_event(int cc);
bool ed_point_only_event(int cc);
/* Graph editing (editor/event_graph.c). The resolution is 0 unless the displayed lane is a
   graph; ed_event_snap is its cell in beats, or the 0.25-beat point grid. */
int ed_graph_resolution(Editor *e);
double ed_event_snap(Editor *e);
bool ed_automation_raw(Editor *e);
bool ed_graph_convert(Editor *e, int pattern);
void ed_graph_mode(Editor *e, int mode, int graph);
/* Asks before a switch to or from Graph when the pattern has events it would change. */
void ed_graph_mode_request(Editor *e, int mode, int graph);
void ed_graph_mode_dialog(Editor *e);
void ed_graph_snap(Editor *e);
void ed_graph_paint(Editor *e, UiRect lane, View *v, float scale, UiPoint from, UiPoint to,
                    bool free);
bool ed_graph_erase(Editor *e, UiRect lane, View *v, float scale, UiPoint from, UiPoint to);
UiRect ed_graph_bar(Editor *e, DmAutomation a, UiRect lane, View *v, float scale, double next_beat);
int ed_graph_plot(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse);
/* Event lane colors from the application color: point lines and markers, and graph bars. */
uint32_t ed_point_color(Editor *e);
uint32_t ed_graph_color(Editor *e);
bool ed_graph_controls(Editor *e, DmPattern *p, bool lfo);
void ed_graph_resolution_controls(Editor *e);
void ed_automation_selector(Editor *e);
#define ED_LANE_STEP 20.0f /* Pixels between pitch rows in an event lane. */
/* Raw value lanes (controllers, raw pitch bends and graph bars) have rows of a power-of-two value
   step about ED_LANE_STEP pixels apart in a lane `height` tall; values snap to them unless Alt is
   held. 0 for the displayed lane when it has no value rows (pedals, program changes, pitch rows,
   velocity). */
int ed_value_step(Editor *e, float height);
int ed_value_snap(Editor *e, int value, int step);
void ed_pitch_cache_begin(Editor *e);
void ed_pitch_cache_end(Editor *e);
float ed_automation_range(Editor *e, DmAutomation a);
int ed_pitch_anchor(Editor *e, DmAutomation a);
void ed_pitch_edit_units(Editor *e, DmAutomation *point, bool raw);
float ed_automation_pitch(Editor *e, DmAutomation a);
UiPoint ed_automation_point(Editor *e, DmAutomation a, UiRect lane, View *v, float scale);
float ed_curve_units(Editor *e, DmAutomation a, UiRect lane, bool overlay);
void ed_automation_connection(UiPoint previous, UiPoint point, DmAutomation before, DmAutomation a,
                              float units, uint32_t color);
int ed_automation_plot(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse);
void ed_sort_automation(Editor *e);
int ed_lane_value(Editor *e, UiRect lane, float y, DmAutomation a, bool free);
DmAutomation ed_expand_pitch_range(Editor *e, DmAutomation node, float pitch);
bool ed_connection_pair(Editor *e, DmAutomation a, DmAutomation b, bool overlay);
bool ed_curve_handles(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse, bool overlay,
                      bool hovered);
void ed_curve_restore_position(Editor *e, int id);
void ed_select_edited_node(Editor *e, DmAutomation edited);
void ed_automation_node_drag(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse);
int ed_closest_node(Editor *e, double beat);
DmAutomation ed_split_curve_node(Editor *e, int left, DmAutomation last, double beat);
bool ed_insert_lane_node(Editor *e, double beat);
void ed_automation_lane_click(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse, int hit);
void ed_automation_press(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse);
void ed_automation_drag(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse);
void ed_event_box_select(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse);
void ed_pitch_text(char *out, size_t cap, float pitch);
void ed_automation_input(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse, bool hovered,
                         int hit);
void ed_remove_selected_node(Editor *e);
void ed_erase_node(Editor *e, int index);
void ed_deselect_events(Editor *e);
void ed_automation_clear(Editor *e);
bool ed_copy_events(Editor *e, DmProject *to);
bool ed_copy_note_bends(Editor *e, DmProject *to, double origin, bool *selected);
void ed_copy_event_lane(Editor *e, bool all);
void ed_paste_event_lane(Editor *e);
bool ed_event_type_used(Editor *e, int cc);
void ed_paste_events(Editor *e, const DmProject *from, double at);
bool ed_paste_note_bends(Editor *e, const DmProject *from, double at);
void ed_duplicate_events(Editor *e);
void ed_delete_events(Editor *e);
void ed_remove_events(Editor *e, bool *remove, int type);
void ed_automation_pan_y(Editor *e, UiRect lane, float pixels);
/* Draws automation pattern id in box: its height holds the value range and its width the part
   drawn. Beat b sits at x + (b - offset) * x_scale, the same mapping as note thumbnails, so the
   drawing keeps its shape when the box is resized. */
void ed_automation_preview(Editor *e, int id, UiRect box, uint32_t color, float x, float x_scale,
                           double offset);
bool ed_transfer_automation(Editor *e);
void ed_event_editor(Editor *e, float width, float height);
int ed_velocity_delta(DmPattern *p, int hit, int delta, const DmNote *source);
bool ed_note_velocity_wheel(Editor *e, DmPattern *p, UiRect r, View *v, float label, float row,
                            float scale, UiMouse mouse, bool hovered);
void ed_automation_panel(Editor *e, UiRect r, View *v, float scale, float label, UiMouse mouse);
void ed_brush_drag(Editor *e, DmPattern *p, UiRect grid, View *v, float scale, float row,
                   UiMouse mouse);
void ed_brush_begin(Editor *e, DmPattern *p, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse);
void ed_piano_click(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse, int hit, UiRect hit_box);
void ed_select_preview_instrument(Editor *e, int program);
void ed_reset_preview_instrument(Editor *e);
UiPoint ed_bend_point(Editor *e, DmAutomation a, UiRect grid, View *v, float scale);
bool ed_bend_mouse(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse);
void ed_piano_mouse(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse, bool hovered, int hit, UiRect hit_box);
void ed_focus_first_note(Editor *e, DmPattern *p, UiRect grid);
void ed_roll(Editor *e, float width, float height);
void ed_settings(Editor *e);
void ed_open_instrument(Editor *e, int channel, bool preview);
void ed_close_instrument(Editor *e);
void ed_apply_instrument(Editor *e);
void ed_step_instrument(Editor *e, int direction);
bool ed_remove_channel(Editor *e, int channel);
bool ed_swap_channels(Editor *e, int a, int b);
/* Exchanges the editor state kept per channel (expanded rows, note ports, mixer meters, channel
   references) between channels a and b, without touching the project. */
void ed_swap_channel_state(Editor *e, int a, int b);
int ed_initial_bank(const DmProject *project, int port, int channel);
const char *ed_sample_channel_name(Editor *e, int channel);
void ed_instrument(Editor *e);
void ed_preferences(Editor *e);
void ed_shortcuts(Editor *e);
bool ed_song_event_add(Editor *e);
bool ed_song_event_position_valid(Editor *e);
void ed_song_event_position(Editor *e, double *beat, const char *label, float column);
void ed_song_event_dialog(Editor *e);
size_t ed_midi_rows(Editor *e);
size_t ed_midi_rows_between(Editor *e, double start, double end);
bool ed_midi_parse_bytes(Editor *e);
void ed_midi_select(Editor *e, MidiRow row);
const char *ed_midi_row_name(Editor *e, MidiRow row);
size_t ed_midi_payload_offset(const DmBytes *m);
bool ed_midi_replace_payload(Editor *e, const uint8_t *payload, size_t size);
void ed_midi_event_fields(Editor *e);
/* The list and details panels, height tall; 0 fills the rest of the window. */
void ed_midi_event_panels(Editor *e, size_t count, float *split, float height);
void ed_midi_apply_initial(Editor *e, const uint8_t *message);
bool ed_midi_apply_draft(Editor *e);
void ed_midi_event_window(Editor *e);
void ed_export_dialog(Editor *e);
void ed_dialogs(Editor *e);

#endif
