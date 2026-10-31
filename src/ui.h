#ifndef DM_UI_H
#define DM_UI_H
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    float x, y;
} UiPoint;

typedef struct {
    float x, y, w, h;
} UiRect;

typedef struct {
    UiPoint pos, delta;
    float wheel;
    bool left, clicked, released, right, right_clicked, double_clicked, shift, ctrl, alt,
        middle_clicked, middle, middle_released;
} UiMouse;

bool ui_init(SDL_Window *window, SDL_Renderer *renderer, const char *settings);
void ui_shutdown(void);
void ui_theme(uint32_t accent);
bool ui_color(const char *label, uint32_t *color);
bool ui_port_color(const char *id, uint32_t color, bool selected, float size);
bool ui_mouse_popup(const char *name, bool request);
bool ui_toggle(const char *label, bool *value);
void ui_resize_cursor(bool vertical);
void ui_triangle(UiPoint a, UiPoint b, UiPoint c, uint32_t color);
void ui_circle(UiPoint point, float radius, uint32_t color);
void ui_circle_outline(UiPoint point, float radius, uint32_t color);
/* While a pattern card is dragged, returns true with its floating preview open for drawing;
   close it with ui_drag_pattern_end. */
bool ui_drag_pattern(int pattern);
void ui_drag_pattern_end(void);
void ui_dummy(float w, float h);
bool ui_invisible_button(const char *id, float w, float h);
int ui_drop_pattern(void);
void ui_clamp_windows(void);
void ui_keyboard_navigation(bool enabled);
bool ui_item_middle(void);
void ui_event(const SDL_Event *event);
void ui_frame(void);
void ui_set_scale(float scale);
float ui_scale(void);
UiPoint ui_display(void);
UiRect ui_window_rect(void);
void ui_drop_overlay(UiRect rect, const char *text);
void ui_render(SDL_Renderer *renderer);
void ui_root(float y, float height);
void ui_end(void);
bool ui_child(const char *id, float w, float h, bool scroll);
bool ui_mixer_child(const char *id, float w, float h, bool group);
bool ui_mixer_header(const char *label, float width);
bool ui_mixer_pan(int *value, float width);
void ui_compact_begin(void);
void ui_compact_end(void);
void ui_child_end(void);
void ui_position(float x, float y);
UiPoint ui_cursor(void);
UiPoint ui_available(void);
UiPoint ui_window(void);
UiPoint ui_scroll_value(void);
UiMouse ui_mouse(void);
float ui_delta_time(void);
void ui_same(void);
float ui_item_spacing_x(void);
float ui_item_spacing_y(void);
void ui_group_begin(void);
void ui_group_end(void);
void ui_newline(void);
void ui_width(float width);
void ui_space(float h);
void ui_separator(void);
void ui_text(const char *text);
void ui_wrap(const char *text);
void ui_love_text(const char *text);
void ui_spectrum(const float *bands, int count, float height, uint32_t color);
void ui_dim(const char *text);
void ui_tip(const char *text);
void ui_placement_tip(const char *text);
bool ui_splitter(float *width, float min, float max, float height);
void ui_info(const char *text);
float ui_scrollbar_reserved(void); /* Height the horizontal scrollbar takes when shown. */
void ui_mixer_wheel(void);
void ui_piano_icon(UiRect rect);
bool ui_frame_toggle(const char *label, bool *value);
bool ui_button(const char *label, float w, float h);

typedef enum {
    UI_PLAY,
    UI_PAUSE,
    UI_STOP,
    UI_RECORD,
    UI_POINTER,
    UI_BOX,
    UI_SCISSORS,
    UI_DELETE,
    UI_BRUSH,
    UI_RECORD_ON,
    UI_TIMER,
    UI_HOURGLASS,
    UI_OVERLAP,
    UI_CURSOR,
    UI_DRAG_HAND,
    UI_POINT_HAND,
    UI_PIANO,
    UI_ICON_COUNT
} UiIcon;

bool ui_icon_button(const char *id, UiIcon icon, float width, float height);
void ui_custom_cursor(bool enabled);
void ui_gesture_cursor(bool point);
bool ui_editor_menu(void);
bool ui_select(const char *label, bool selected, float w, float h);
bool ui_check(const char *label, bool *value);
bool ui_int(const char *label, int *value, int lo, int hi);
bool ui_float(const char *label, float *value, float lo, float hi);
bool ui_input(const char *label, char *value, size_t cap);
bool ui_appearing(void);
bool ui_dialog_enter(void);
bool ui_content_dialog(const char *title, bool *open, float width);
bool ui_mixer_value(int *value, float width);
bool ui_multiline(const char *label, char *value, size_t cap, float height);
bool ui_knob(const char *label, double *value, double lo, double hi, double reset);
bool ui_slider(const char *label, int *value, int lo, int hi, float width);
bool ui_vslider(const char *label, int *value, float height);
bool ui_combo(const char *label, const char *preview);
bool ui_bar_combo(const char *label, const char *preview);
void ui_bar_begin(void);
void ui_bar_end(void);
bool ui_panel_clicked(void);
bool ui_background_clicked(void);
bool ui_shift_context(const char *id);
bool ui_workspace_focused(void);
void ui_combo_end(void);
void ui_id(int id);
void ui_pop_id(void);
bool ui_menu_bar(void);
void ui_menu_bar_end(void);
bool ui_menu(const char *name);
void ui_menu_end(void);
bool ui_menu_item(const char *name, const char *shortcut, bool enabled);
bool ui_menu_check(const char *name, bool checked);
void ui_pan_cursor(void);
bool ui_fixed_dialog(const char *s, bool *open, float w, float h);
bool ui_fitted_dialog(const char *s, bool *open, float w);
void ui_center_text(const char *text);
int ui_welcome_content(void);
void ui_confirmation_message(const char *text);
void ui_center_row(float width);
bool ui_dialog(const char *title, bool *open, float width, float height);
bool ui_modal(const char *title, bool *open, float width, float height);
void ui_close_popup(void);
bool ui_context(const char *id);
void ui_popup_end(void);
bool ui_hover(void);
bool ui_active(void);
bool ui_focused(void);
bool ui_text_active(void);
bool ui_any_active(void);
bool ui_item_changed(void);
void ui_disabled(bool disabled);
void ui_disabled_end(void);
void ui_escape(void);
int ui_topmost(const char *const *titles, int count);
bool ui_cancel_popup(void);
bool ui_popup_open(void);
void ui_focus_next(void);
void ui_raise_next(void); /* Brings the next window in front of the others. */
void ui_canvas(const char *id, float w, float h);
void ui_scroll(float x, float y);
void ui_scroll_to(float y);
void ui_rect(UiRect r, uint32_t color, float rounding);
void ui_border(UiRect r, uint32_t color, float rounding, float width);
void ui_line(UiPoint a, UiPoint b, uint32_t color, float width);
void ui_label_fit(UiPoint at, float width, uint32_t color, const char *text);
void ui_label(UiPoint pos, uint32_t color, const char *text);
float ui_font_size(void);
float ui_text_width(const char *text);
void ui_clip(UiRect r);
void ui_unclip(void);
void ui_progress(float value, float w, float h, const char *text);
void ui_align_frame(void);
float ui_button_row_width(const char *const *labels, int count);
void ui_channel_color(uint32_t color);
void ui_channel_color_end(void);
void ui_accent(bool enabled);
void ui_accent_end(void);
void ui_scrollbar(const char *id, UiRect rect, bool vertical, float *value, float visible,
                  float total);
#ifdef __cplusplus
}
#endif
#endif
