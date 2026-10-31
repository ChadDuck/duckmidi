#ifndef DM_EDITOR_INTERNAL_H
#define DM_EDITOR_INTERNAL_H
/* Shared editor implementation contracts; desktop entry points remain in editor.h. */
#include "editor.h"
#include "preferences.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLOR(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | 0xff000000u)
#define BG COLOR(12, 12, 12)
#define GRID COLOR(38, 38, 38)
uint32_t ed_port_color(Editor *e, unsigned port);
uint32_t ed_palette_color(Editor *e, unsigned color);
void ed_pattern_color_request(Editor *e, int pattern);
void ed_pattern_color_dialog(Editor *e);
unsigned ed_note_port(Editor *e);
float ed_port_buttons_width(Editor *e);
void ed_port_buttons(Editor *e);
bool ed_color_notes(Editor *e, unsigned port);
bool ed_delete_note_port(Editor *e, unsigned port);
void ed_note_color_dialog(Editor *e);
#define TEXT COLOR(217, 228, 235)
#define DIM COLOR(115, 137, 151)
#define GREEN (e->accent)
#define GOLD COLOR(245, 196, 111)

enum {
    DIALOG_OPEN = 1,
    DIALOG_SAVE,
    DIALOG_MIDI,
    DIALOG_WAVE,
    DIALOG_FONT,
    DIALOG_EXPORT_PLUGIN,
    DIALOG_REPOSITORY
};

#include "../include/duckmidi_export_plugin.h"
size_t ed_export_count(Editor *e);
const char *ed_export_name(Editor *e, size_t index);
bool ed_export_settings(Editor *e, size_t index, DmeUi *ui);
bool ed_export_set(Editor *e, size_t index, const char *id, DmeValue value);
void ed_export_start(Editor *e, size_t index, const char *destination);
void ed_export_poll(Editor *e);
void ed_export_free(Editor *e);
void ed_export_controls(Editor *e);
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
void ed_open_audio_location(Editor *e);
void ed_export_reset_results(Editor *e);
bool ed_export_has_changes(Editor *e);
void ed_export_revert(Editor *e);
bool ed_export_repository_picker(Editor *e, const char **start);
void ed_export_repository_done(Editor *e, const char *folder);
bool ed_export_picker(Editor *e, const SDL_DialogFileFilter **filter, const char **suggested);
const char *ed_export_extension(Editor *e);
void ed_export_picker_done(Editor *e, const char *path);

enum { PENDING_NEW = 1, PENDING_OPEN, PENDING_PATH, PENDING_QUIT };

typedef struct {
    int range, cents, msb, lsb;
} PitchState;

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
extern const char *ed_action_names[ACT_COUNT];
void ed_decode_source_event(Editor *e, DmEvent *event);
void ed_focus_edit(Editor *e, EditFocus focus);
bool ed_velocity_editing(Editor *e);
EditFocus ed_selection_focus(Editor *e);
void ed_toggle_roll(Editor *e);
void ed_reset_layout(Editor *e);
float ed_events_section_height(Editor *e);
int ed_channel_program(Editor *e, int channel);
void ed_preview_channel(Editor *e, int channel, bool placed);
void ed_select_placement_route(Editor *e, int index);
void ed_open_pattern(Editor *e, int id, bool created);
SDL_Keymod ed_normalized(SDL_Keymod m);
void ed_defaults(Editor *e);
void ed_message(Editor *e, const char *text);
DmPattern *ed_pattern(Editor *e);
void ed_bind_pattern_channel(Editor *e);
void ed_clear_history(DmBytes *b, int *count);
void ed_checkpoint(Editor *e);
bool ed_add_note(Editor *e, DmPattern *p, DmNote note);
double ed_pattern_length(Editor *e);
bool ed_transport_publish(Editor *e, double seconds, bool playing);
double ed_transport_length(Editor *e);
void ed_publish(Editor *e);
void ed_changed(Editor *e);
void ed_request_font(Editor *e, const char *path);
void ed_commit(Editor *e);
int ed_note_identity(const void *left, const void *right);
void ed_undo(Editor *e, bool redo);
void ed_add_recent(Editor *e, const char *path);
uint32_t ed_shortcut_schema(void);
void ed_preference_save(Editor *e);
void ed_preference_load(Editor *e);
void ed_dialog(Editor *e, int kind);
bool ed_save_to(Editor *e, const char *path);
void ed_save_poll(Editor *e);
void ed_save_free(Editor *e);
bool ed_save_busy(Editor *e);
void ed_save_cancel(Editor *e);
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
int ed_port_collisions(Editor *e, bool all, char *text, size_t capacity);
void ed_combine_dialog(Editor *e);
void ed_pending_execute(Editor *e);
void ed_request(Editor *e, int ed_action, const char *path);
void ed_update_dialog(Editor *e);
void ed_update_open(Editor *e);
void ed_cancel_wait(Editor *e);
void ed_play(Editor *e);
void ed_seek(Editor *e, double beat);
bool ed_point_editing(Editor *e);
void ed_remove_placement(Editor *e, size_t index);
void ed_delete_selection(Editor *e);
void ed_select_all(Editor *e);
void ed_duplicate_clips(Editor *e);
void ed_duplicate_notes(Editor *e);
void ed_deselect(Editor *e);
void ed_clear_pattern_selection(Editor *e);
bool ed_deselect_focused(Editor *e);
int ed_piano_key(SDL_Scancode key);
int ed_piano_index(SDL_Scancode key);
SDL_Scancode ed_piano_scancode(int index);
int ed_keyboard_note(Editor *e, SDL_Scancode key, int channel);
void ed_percussion_open(Editor *e);
void ed_percussion_dialog(Editor *e);
bool ed_percussion_key(Editor *e, const SDL_KeyboardEvent *key);
void ed_percussion_save(Editor *e, bool project);
bool ed_begin_record_take(Editor *e);
double ed_recording_origin(Editor *e, double beat);
void ed_record_note(Editor *e, int note);
void ed_stop_edit_feedback(Editor *e);
void ed_finish_edit_feedback(Editor *e);
void ed_edit_feedback(Editor *e, float pitch);
void ed_release_piano(Editor *e);
void ed_transpose(Editor *e, int steps);
bool ed_reserved(SDL_Scancode key, SDL_Keymod mods);
void ed_action_zoom(Editor *e, int a);
void ed_action(Editor *e, int a);
void ed_paste(Editor *e);
void ed_clipboard(Editor *e, int mode);
int ed_keyboard_channel(Editor *e);
void ed_piano_press(Editor *e, SDL_Scancode key, SDL_Keymod mods, bool repeat);
void ed_shortcut(Editor *e, SDL_Scancode key, SDL_Keymod raw, bool repeat);
bool ed_instrument_note(Editor *e, const SDL_KeyboardEvent *key);
uint32_t ed_track_color(int channel);
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
uint32_t ed_pattern_color(Editor *e, int id);

/* Thumbnail geometry: x = x + (beat - offset) * x_scale; y = y - (key - 48) * y_scale. */
typedef struct {
    UiRect visible;
    float x, x_scale, y, y_scale, top, bottom;
    double offset;
} NoteThumbnail;

void ed_note_thumbnail(const DmPattern *p, NoteThumbnail thumbnail);
void ed_pattern_panel(Editor *e, float width, float height);
bool ed_channel_has_clips(Editor *e, int channel);
int ed_available_channel(Editor *e);
void ed_mixer(Editor *e, float width, float height);
const bool *ed_channel_ports(Editor *e, int channel);
int ed_channels(Editor *e, int *list);
void ed_grid_scroll(Editor *e, View *v, UiRect canvas, float content_width, float content_height,
                    float row, bool consumed);
void ed_horizontal_scroll(View *v, UiRect canvas, float total);
double ed_view_playhead(Editor *e, int context, bool playing, double beat, bool *visible);
void ed_ruler(Editor *e, UiRect r, View *v, float scale, float label);
double ed_pattern_seek_origin(Editor *e);
void ed_scrub_publish(Editor *e, double beat, int context, bool playing);
bool ed_clear_loop(Editor *e);
void ed_timeline_drag(Editor *e, UiRect r, View *v, float scale, float label, int context,
                      bool hovered);
int ed_playlist_channels(Editor *e, int *list);
int ed_place_pattern(Editor *e, int id, int encoded, double beat);
void ed_split_note(DmPattern *p, size_t index, double at);
void ed_split_clip(Editor *e, size_t index, double at);
bool ed_intersects(UiRect a, UiRect b);
UiRect ed_marquee_box(Editor *e, UiMouse mouse);
bool ed_black_key(int step);
void ed_playlist_label_text(Editor *e, UiRect r, float label, float row, const int *list, int i,
                            float y, char *text, size_t capacity);
void ed_playlist_label_action(Editor *e, int channel, int lane, UiMouse mouse);
void ed_playlist_label(Editor *e, UiRect r, float label, float row, const int *list, int i, float y,
                       UiMouse mouse);
const char *ed_event_label(const DmBytes *m);
void ed_song_event_open(Editor *e, int index, bool automation);
bool ed_initial_gui_event(const DmEvent *event);
void ed_resize_clip(Editor *e, DmClip *clip, double length);
void ed_move_clip(Editor *e, DmClip *clip, int encoded, double beat);
void ed_playlist_drag(Editor *e, UiRect r, View *v, float row, float scale, const int *list,
                      int count, UiMouse mouse);
void ed_marquee_outline(Editor *e, int context, UiMouse mouse);
void ed_playlist_hit_click(Editor *e, UiMouse mouse, int hit, double beat);
void ed_playlist_click(Editor *e, UiRect r, UiRect grid, View *v, float row, float scale,
                       const int *list, int count, UiMouse mouse, int hit);
void ed_playlist_remove(Editor *e, int hit);
void ed_playlist(Editor *e, float width, float height);
PitchState ed_pattern_pitch_state(const DmProject *p, int pattern_id, double beat, unsigned port);
PitchState ed_pitch_state(Editor *e, double beat);
int ed_curve_id(Editor *e);
int ed_nearest_note(Editor *e, double beat);
int ed_curve_anchor(Editor *e, double beat);
int ed_event_value(DmAutomation a);
void ed_set_event_value(DmAutomation *a, int value);
DmAutomation ed_curve_node(Editor *e, double beat, int value, int cc, int id, int anchor);
int ed_value_before(Editor *e, double beat, int cc);
void ed_curve_range(Editor *e, double beat, int range, int cents, int msb, int lsb, int id);
void ed_automation_line(Editor *e, double start, double end, int first, int last, int cc);
void ed_follow_note_curves(Editor *e, int pattern, const DmNote *before, size_t count);
bool ed_sounding(Editor *e, int key);
void ed_resize_note_group(Editor *e, DmPattern *p, double delta, bool free);
bool ed_gesture_moved(Editor *e, UiMouse mouse);
void ed_begin_edit_move(Editor *e);
bool ed_automation_matches(Editor *e, DmAutomation au);
bool ed_same_event(DmAutomation a, DmAutomation b);
bool ed_event_selected(Editor *e, size_t index);
bool ed_pedal_event(int cc);
bool ed_point_only_event(int cc);
bool ed_automation_option(Editor *e, int cc);
void ed_automation_selector(Editor *e);
float ed_lane_step(UiRect lane);
float ed_automation_range(Editor *e, DmAutomation a);
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
void ed_paste_events(Editor *e, const DmProject *from, double at);
void ed_duplicate_events(Editor *e);
void ed_delete_events(Editor *e);
void ed_automation_pan_y(Editor *e, UiRect lane, float pixels);
void ed_automation_preview(Editor *e, int id, UiRect box, uint32_t color, double offset,
                           double length);
bool ed_transfer_automation(Editor *e);
void ed_lfo_center_drag(Editor *e, DmPattern *p, UiRect lane, UiMouse mouse, bool hovered);
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
UiPoint ed_bend_point(Editor *e, DmAutomation a, UiRect grid, View *v, float scale);
bool ed_bend_mouse(Editor *e, UiRect grid, View *v, float scale, UiMouse mouse);
void ed_piano_mouse(Editor *e, DmPattern *p, UiRect r, UiRect grid, View *v, float scale, float row,
                    UiMouse mouse, bool hovered, int hit, UiRect hit_box);
void ed_focus_first_note(Editor *e, DmPattern *p, UiRect grid);
void ed_roll(Editor *e, float width, float height);
void ed_settings(Editor *e);
void ed_choose_program(Editor *e, int channel, int program);
void ed_choose_drums(Editor *e, int channel);
bool ed_remove_empty_channel(Editor *e, int channel);
void ed_instrument(Editor *e);
void ed_preferences(Editor *e);
void ed_shortcuts(Editor *e);
void ed_song_event_remove(Editor *e);
void ed_song_event_add(Editor *e);
void ed_song_event_position(Editor *e);
void ed_song_event_dialog(Editor *e);
size_t ed_midi_rows(Editor *e);
bool ed_midi_parse_bytes(Editor *e);
void ed_midi_select(Editor *e, MidiRow row);
const char *ed_midi_row_name(Editor *e, MidiRow row);
size_t ed_midi_payload_offset(const DmBytes *m);
bool ed_midi_replace_payload(Editor *e, const uint8_t *payload, size_t size);
bool ed_midi_apply_draft(Editor *e);
void ed_midi_event_window(Editor *e);
void ed_export_dialog(Editor *e);
void ed_dialogs(Editor *e);
void ed_welcome(Editor *e, float width, float body);

#endif
