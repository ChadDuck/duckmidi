#include "editor/internal.h"
#include "text/en.h"

static void automation_grid(Editor *e, UiRect lane, View *v, float scale);
static void automation_axis(Editor *e, UiPoint at, UiRect lane);
static void automation_value_input(Editor *e);
static void automation_scroll(Editor *e, View *v, UiRect lane, UiMouse mouse, bool hovered);
static bool event_setup(DmAutomation a);
static int transfer_event(Editor *e, int source);
static bool transfer_prepare(Editor *e, int source, bool *used);
static void event_header_buttons(Editor *e, float right, bool transfer);
static double lfo_center_change(DmPattern *p, double value, bool free);
static double lfo_bar(Editor *e);
static void lfo_controls(Editor *e, DmPattern *p);
static UiPoint lfo_point(Editor *e, DmPattern *p, UiRect lane, View *v, float scale, double beat);
static void lfo_plot(Editor *e, DmPattern *p, UiRect lane, View *v, float scale);
static void event_editor_header(Editor *e, DmPattern *p);
static void event_editor_scroll(Editor *e, UiRect r, UiRect lane, View *v, UiMouse mouse,
                                bool hovered);
static void focus_event_pattern(Editor *e, UiRect lane, float scale);
static void event_editor_vertical(Editor *e, UiRect r, UiRect lane);
static UiPoint velocity_point(DmNote note, UiRect lane, View *v, float scale);
static void velocity_drag(Editor *e, DmPattern *p, UiRect lane, UiMouse mouse);
static void velocity_select(Editor *e, DmPattern *p, UiRect lane, View *v, float scale,
                            UiMouse mouse);
static void velocity_input(Editor *e, DmPattern *p, UiMouse mouse, bool hovered, int hit);
static void velocity_panel(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse,
                           bool hovered);

static void automation_grid(Editor *e, UiRect lane, View *v, float scale) {
    if (ed_pedal_event(e->automation_cc)) {
        float y = lane.y + lane.h * .5f;
        ui_line((UiPoint){lane.x, y}, (UiPoint){lane.x + lane.w, y}, DIM, 1);
        return;
    }

    float step = (float)ed_event_snap(e); /* Graph lanes draw their cells. */

    while (step * scale < 8) {
        step *= 2;
    }

    int first = (int)floor(v->scroll_x / (scale * step));

    for (int i = first; i < first + (int)(lane.w / (scale * step)) + 2; ++i) {
        float beat = i * step, x = lane.x + beat * scale - v->scroll_x;
        ui_line((UiPoint){x, lane.y}, (UiPoint){x, lane.y + lane.h}, ed_grid_color(e, beat), 1);
    }

    if (e->automation_cc != DM_EVENT_BEND || ed_automation_raw(e)) {
        /* Value rows where drawn values snap, about as far apart as pitch rows; the top line is
           the maximum. */
        int maximum = e->automation_cc == DM_EVENT_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;
        int step = ed_value_step(e, lane.h);
        e->event_lane_height = lane.h;

        if (!step) {
            step = (maximum + 1) / 8;
        }

        for (int value = 0; value < maximum + step; value += step) {
            int row = value < maximum ? value : maximum;
            float y = lane.y + lane.h - 8 - (float)row / maximum * (lane.h - 8);
            ui_line((UiPoint){lane.x, y}, (UiPoint){lane.x + lane.w, y}, GRID, 1);
        }

        return;
    }

    float shift = (e->automation_center - roundf(e->automation_center)) * ED_LANE_STEP;

    for (float y = lane.y + lane.h * .5f + shift; y < lane.y + lane.h; y += ED_LANE_STEP) {
        ui_line((UiPoint){lane.x, y}, (UiPoint){lane.x + lane.w, y}, GRID, 1);
    }

    for (float y = lane.y + lane.h * .5f + shift - ED_LANE_STEP; y > lane.y; y -= ED_LANE_STEP) {
        ui_line((UiPoint){lane.x, y}, (UiPoint){lane.x + lane.w, y}, GRID, 1);
    }

    (void)e;
}

static void automation_axis(Editor *e, UiPoint at, UiRect lane) {
    if (ed_pedal_event(e->automation_cc)) {
        ui_label((UiPoint){at.x + 4, lane.y + lane.h * .5f - 22}, DIM, DM_TEXT_127);
        ui_label((UiPoint){at.x + 4, lane.y + lane.h * .5f + 10}, DIM, DM_TEXT_OFF_0);
        return;
    }

    static const char *notes[] = {DM_TEXT_C,       DM_TEXT_C_SHARP, DM_TEXT_D,
                                  DM_TEXT_D_SHARP, DM_TEXT_E,       DM_TEXT_F,
                                  DM_TEXT_F_SHARP, DM_TEXT_G,       DM_TEXT_G_SHARP,
                                  DM_TEXT_A,       DM_TEXT_A_SHARP, DM_TEXT_B};

    if (e->automation_cc == DM_EVENT_BEND && !ed_automation_raw(e)) {
        int center = (int)roundf(e->automation_center);
        int visible = (int)ceilf(lane.h / 40) + 1;

        for (int offset = -visible; offset <= visible; ++offset) {
            int key = center + offset;
            float y = lane.y + lane.h * .5f - (key - e->automation_center) * ED_LANE_STEP;

            if (key < 0 || key > 127 || y < lane.y + 16 || y > lane.y + lane.h - 12) {
                continue;
            }

            char name[16];
            snprintf(name, sizeof(name), "%s%d", notes[key % 12], key / 12 - 1);
            ui_label((UiPoint){at.x + 4, y - 6}, DIM, name);
        }
    } else {
        ui_label((UiPoint){at.x + 4, lane.y + 2}, DIM,
                 e->automation_cc == DM_EVENT_BEND ? DM_TEXT_BEND_MAXIMUM : DM_TEXT_VALUE_MAXIMUM);
        ui_label((UiPoint){at.x + 4, lane.y + lane.h - 15}, DIM, DM_TEXT_VALUE_MINIMUM);
        int maximum = e->automation_cc == DM_EVENT_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;
        int step = ed_value_step(e, lane.h);
        float spacing = step * (lane.h - 8) / maximum;

        /* Row values between the ends, every other row when rows are close, as pitch rows are
           named. */
        for (int value = step; step && value < maximum; value += spacing < 18 ? 2 * step : step) {
            float y = lane.y + lane.h - 8 - (float)value / maximum * (lane.h - 8);

            if (y < lane.y + 26 || y > lane.y + lane.h - 26) {
                continue;
            }

            char text[16];
            snprintf(text, sizeof(text), DM_TEXT_VALUE_ROW, value);
            ui_label((UiPoint){at.x + 4, y - 6}, DIM, text);
        }
    }
}

static void automation_value_input(Editor *e) {
    if (ed_pedal_event(e->automation_cc)) {
        return;
    }

    int index = e->automation_node;

    if ((e->automation_cc == DM_EVENT_BEND && !ed_automation_raw(e)) || index < 0 ||
        (size_t)index >= e->project.automation_count) {
        return;
    }

    DmAutomation *a = &e->project.automation[index];

    if (!ed_automation_matches(e, *a) || a->curve < 0) {
        return;
    }

    ui_same();
    int value = ed_event_value(*a);

    if (ui_int(DM_TEXT_VALUE, &value, 0,
               a->type == MIDI_PITCH_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX)) {
        ed_checkpoint(e);
        a->bend_note = 0;
        ed_pitch_edit_units(e, a, true);
        ed_set_event_value(a, value);
        ed_commit(e);
    }
}

void ed_automation_pan_y(Editor *e, UiRect lane, float pixels) {
    if (e->automation_cc == DM_EVENT_BEND && !ed_automation_raw(e)) {
        e->automation_center = ed_clampf(e->automation_center + pixels / ED_LANE_STEP, 0, 127);
    }
}

static void automation_scroll(Editor *e, View *v, UiRect lane, UiMouse mouse, bool hovered) {
    bool lfo = ed_pattern(e) && ed_pattern(e)->kind && ed_pattern(e)->mode;

    if (hovered && mouse.wheel) {
        if (!ed_gesture_held(e, GESTURE_SCROLL_ROWS, mouse) && !lfo) {
            v->scroll_x = fmaxf(0, v->scroll_x - mouse.wheel * 60);
        } else {
            ed_automation_pan_y(e, lane, mouse.wheel * ED_LANE_STEP * 3);
        }
    }

    if (mouse.middle_clicked && hovered) {
        e->pan_context = CONTEXT_EVENT_EDITOR;
    }

    if (e->pan_context == CONTEXT_EVENT_EDITOR) {
        if (!mouse.middle) {
            e->pan_context = CONTEXT_NONE;
        } else {
            ui_pan_cursor();
            v->scroll_x = fmaxf(0, v->scroll_x - mouse.delta.x);
            ed_automation_pan_y(e, lane, mouse.delta.y);
        }
    }
}

static bool event_setup(DmAutomation a) {
    return a.type == MIDI_CONTROL && (a.a == MIDI_CC_DATA || a.a == MIDI_CC_DATA_LSB ||
                                      a.a == MIDI_CC_RPN_LSB || a.a == MIDI_CC_RPN_MSB);
}

void ed_automation_preview(Editor *e, int id, UiRect box, uint32_t color, float x, float x_scale,
                           double offset) {
    DmPattern *p = &e->project.patterns[id];

    if (x_scale <= 0 || box.w <= 0) {
        return;
    }

    if (p->mode) {
        /* One sample per pixel, as the LFO view samples, so fast waves are not aliased. */
        int samples = (int)fminf(8192, ceilf(box.w));
        double maximum = p->event == DM_EVENT_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;
        UiPoint previous = {0};

        for (int i = 0; i <= samples; ++i) {
            float px = box.x + box.w * i / samples;
            double value = dm_lfo_value(p, offset + (px - x) / x_scale);
            UiPoint next = {px, box.y + box.h * (.05f + (1 - (float)(value / maximum)) * .9f)};

            if (i) {
                ui_line(previous, next, color, 1.5f);
            }

            previous = next;
        }

        return;
    }

    UiPoint previous = {0};
    DmAutomation before = {0};
    bool has = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != id || a.curve < 0 || event_setup(a)) {
            continue;
        }

        float value =
            (float)ed_event_value(a) / (a.type == MIDI_PITCH_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX);
        UiPoint next = {x + (float)(a.beat - offset) * x_scale, box.y + (1 - value) * box.h};

        if (has && before.type == a.type && (a.type != MIDI_CONTROL || before.a == a.a)) {
            ed_automation_connection(
                previous, next, before, a,
                box.h / (a.type == MIDI_PITCH_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX), color);
        }

        ui_circle(next, 2, color);
        previous = next;
        before = a;
        has = true;
    }
}

static int transfer_event(Editor *e, int source) {
    int event = -999;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != source) {
            continue;
        }

        if (event == -999) {
            event = DM_EVENT_BEND;
        }

        if (a.type == MIDI_CONTROL && !event_setup(a)) {
            event = a.a;
        }
    }

    return event;
}

static bool transfer_prepare(Editor *e, int source, bool *used) {
    DmProject *p = &e->project;
    size_t placements = 0;

    for (size_t i = 0; i < p->clip_count; ++i) {
        if (p->clips[i].pattern == source) {
            ++placements;
            used[p->clips[i].channel] = true;
        }
    }

    for (int c = 0; c < 16; ++c) {
        if (used[c] && p->lane_count[c] >= DM_LANES) {
            ed_message(e, DM_TEXT_FREE_AUTOMATION_SUBROW);
            return false;
        }
    }

    return dm_reserve((void **)&p->patterns, &p->pattern_capacity, p->pattern_count + 1,
                      sizeof(DmPattern)) &&
           dm_reserve((void **)&p->clips, &p->clip_capacity, p->clip_count + placements,
                      sizeof(DmClip));
}

bool ed_transfer_automation(Editor *e) {
    int source = e->selected_pattern;
    DmProject *p = &e->project;

    if (source < 0 || (size_t)source >= p->pattern_count || p->patterns[source].kind) {
        return false;
    }

    int event = transfer_event(e, source);

    if (event == -999) {
        ed_message(e, DM_TEXT_PATTERN_HAS_EVENTS);
        return false;
    }

    bool used[16] = {0};

    if (!transfer_prepare(e, source, used)) {
        return false;
    }

    ed_checkpoint(e);
    char name[64];
    snprintf(name, sizeof(name), DM_TEXT_48S_EVENTS, p->patterns[source].name);
    int id = dm_pattern_add(p, name);

    if (id < 0) {
        return false;
    }

    p->patterns[id].kind = 1;
    p->patterns[id].reference = p->patterns[source].count ? p->patterns[source].notes[0].key : 60;
    p->patterns[id].pitch_center = p->patterns[id].reference;
    p->patterns[id].event = event;
    memcpy(p->patterns[id].graph_lanes, p->patterns[source].graph_lanes,
           sizeof(p->patterns[id].graph_lanes));
    p->patterns[id].length = p->patterns[source].length;
    int lanes[16] = {0};

    for (int c = 0; c < 16; ++c) {
        if (used[c]) {
            lanes[c] = ++p->lane_count[c];
            e->channel_expanded[c] = true;
        }
    }

    size_t count = p->clip_count;

    for (size_t i = 0; i < count; ++i) {
        if (p->clips[i].pattern == source) {
            DmClip clip = p->clips[i];
            clip.pattern = id;
            clip.lane = lanes[clip.channel];
            clip.selected = 0;
            dm_clip_add(p, clip);
        }
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        if (p->automation[i].pattern == source) {
            p->automation[i].pattern = id;
            p->automation[i].bend_note = 0;
        }
    }

    ed_open_pattern(e, id, true);
    ed_bind_pattern_channel(e);
    e->automation_cc = event;
    ed_commit(e);
    return true;
}

/* Copy, Paste and, beside them, Transfer Automation sit together at the right edge of the
   header row, wrapping to a new row when the row has no room for them. */
static void event_header_buttons(Editor *e, float right, bool transfer) {
    const float button = 78, transfer_width = 180;
    float gap = ui_item_spacing_x();
    float total = button * 2 + gap + (transfer ? gap + transfer_width : 0);
    ui_same();
    UiPoint row = ui_cursor();

    if (row.x + total > right) {
        ui_newline();
        row = ui_cursor();
    }

    ui_position(fmaxf(row.x, right - total), row.y);
    bool all = ed_gesture_held(e, GESTURE_APPLY_ALL, ui_mouse());
    bool points = !ed_pattern(e)->kind || !ed_pattern(e)->mode;
    ui_disabled(!points);

    if (ui_button(all ? DM_TEXT_COPY_ALL : DM_TEXT_COPY, button, 0)) {
        ed_copy_event_lane(e, all);
    }

    ui_disabled_end();
    ui_same();
    ui_disabled(!points || !e->event_clipboard.size);

    if (ui_button(DM_TEXT_PASTE, button, 0)) {
        ed_paste_event_lane(e);
    }

    ui_disabled_end();

    if (transfer) {
        ui_same();

        if (ui_button(DM_TEXT_TRANSFER_AUTOMATION, transfer_width, 0)) {
            ed_transfer_automation(e);
        }
    }
}

static double lfo_center_change(DmPattern *p, double value, bool free) {
    double snapped = dm_lfo_center_snap(value, p->event == DM_EVENT_BEND, free);

    if (p->event == DM_EVENT_BEND) {
        p->pitch_center = snapped;
    } else {
        p->center = snapped;
    }

    return snapped;
}

static double lfo_bar(Editor *e) {
    double beat = e->selected_clip >= 0 && (size_t)e->selected_clip < e->project.clip_count
                      ? e->project.clips[e->selected_clip].beat
                      : e->cursor_beat[0];
    return dm_meter(&e->project, beat).bar;
}

static void lfo_controls(Editor *e, DmPattern *p) {
    const char *names[] = {DM_TEXT_CENTER, DM_TEXT_SPEED,       DM_TEXT_TENSION,
                           DM_TEXT_SKEW,   DM_TEXT_PULSE_WIDTH, DM_TEXT_AMOUNT};
    double *center = p->event == DM_EVENT_BEND ? &p->pitch_center : &p->center;
    double *values[] = {center, &p->speed, &p->tension, &p->skew, &p->pulse, &p->amount};
    double lows[] = {0, .03125, -1, -1, .05, 0}, highs[] = {127, 16, 1, 1, .95, 1},
           ed_defaults[] = {p->event == DM_EVENT_BEND ? p->reference : 64, 1, 0, 0, .5, .5};
    UiPoint at = ui_cursor();
    float width = ui_available().x;
    int columns = (int)ed_clampf(floorf((width - 68) / 68), 1, 6);
    float left = fmaxf(at.x, at.x + width - (columns * 68 + 68));

    for (int i = 0; i < 6; ++i) {
        if (i % columns) {
            ui_same();
        } else {
            ui_position(left, at.y + (i / columns) * 80);
        }

        double value = *values[i];

        if (ui_knob(names[i], &value, lows[i], highs[i], ed_defaults[i])) {
            if (!e->lfo_editing) {
                ed_checkpoint(e);
            }

            e->lfo_editing = true;

            if (i == 0) {
                value = lfo_center_change(p, value,
                                          ed_gesture_held(e, GESTURE_FREE, ui_mouse()) ||
                                              ui_mouse().middle_clicked);
            }

            if (i == 1 && !ed_gesture_held(e, GESTURE_FREE, ui_mouse())) {
                value = dm_lfo_speed_snap(value, lfo_bar(e));
            }

            *values[i] = value;
            ed_changed(e);
        }
    }

    ui_same();
    UiPoint invert = ui_cursor();
    ui_position(invert.x, invert.y + 9);
    bool inverted = p->inverted;

    if (ui_frame_toggle(DM_TEXT_INVERT, &inverted)) {
        ed_checkpoint(e);
        p->inverted = inverted;
        ed_commit(e);
    }

    ui_tip(DM_TEXT_INVERT_WAVEFORM_AROUND);
    ui_position(at.x, at.y + ((5 / columns) + 1) * 80);

    if (e->lfo_editing && !ui_mouse().left) {
        e->lfo_editing = false;
        ed_publish(e);
    }
}

static UiPoint lfo_point(Editor *e, DmPattern *p, UiRect lane, View *v, float scale, double beat) {
    double wave = dm_lfo(p, beat),
           value = p->event == DM_EVENT_BEND ? dm_lfo_value(p, beat)
                                             : fmax(0, fmin(127, p->center + 63.5 * wave)),
           maximum = p->event == DM_EVENT_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;
    float y = lane.y + lane.h - 8 - (float)(value / maximum) * (lane.h - 8);

    if (p->event == DM_EVENT_BEND && !ed_automation_raw(e)) {
        y = lane.y + lane.h * .5f -
            (float)(fmax(0, fmin(127, p->pitch_center + 2 * wave)) - e->automation_center) *
                ED_LANE_STEP;
    }

    return (UiPoint){lane.x + (float)beat * scale - v->scroll_x, y};
}

static void lfo_plot(Editor *e, DmPattern *p, UiRect lane, View *v, float scale) {
    UiPoint previous = {0};
    int samples = (int)fmin(8192, lane.w * 2);

    for (int i = 0; i <= samples; ++i) {
        double beat = (v->scroll_x + lane.w * i / samples) / scale;
        UiPoint point = lfo_point(e, p, lane, v, scale, beat);

        if (i) {
            ui_line(previous, point, e->accent, 2);
        }

        previous = point;
    }
}

static void ed_lfo_center_drag(Editor *e, DmPattern *p, UiRect lane, UiMouse mouse, bool hovered) {
    if (hovered && mouse.clicked && ed_inside(mouse.pos, lane)) {
        e->lfo_center_drag = true;
        e->edit_moved = false;
        e->lfo_center_y = mouse.pos.y;
        e->lfo_center_origin = p->event == DM_EVENT_BEND ? p->pitch_center : p->center;
        e->lfo_center_range = (float)dm_lfo_range(p);
        ed_focus_edit(e, FOCUS_EVENTS);
    }

    if (!e->lfo_center_drag) {
        return;
    }

    if (mouse.left) {
        double units = p->event == DM_EVENT_BEND ? 1 / ED_LANE_STEP : 127 / (lane.h - 8);

        if (p->event == DM_EVENT_BEND && e->automation_raw) {
            units = e->lfo_center_range * 2 / (lane.h - 8);
        }

        double value =
            dm_lfo_center_snap(e->lfo_center_origin + (e->lfo_center_y - mouse.pos.y) * units,
                               p->event == DM_EVENT_BEND, ed_gesture_held(e, GESTURE_FREE, mouse));
        double current = p->event == DM_EVENT_BEND ? p->pitch_center : p->center;

        if (value != current) {
            if (!e->edit_moved) {
                ed_checkpoint(e);
            }

            e->edit_moved = true;
            lfo_center_change(p, value, true);
            ed_changed(e);
        }
    } else {
        e->lfo_center_drag = false;

        if (e->edit_moved) {
            ed_publish(e);
        }
    }
}

static void event_editor_header(Editor *e, DmPattern *p) {
    UiPoint header = ui_cursor();
    float width = ui_available().x;
    ui_group_begin();

    if (p->mode) {
        ui_position(header.x, header.y + 7);
    }

    ui_align_frame();
    ui_text(DM_TEXT_EVENT_EDITOR);
    ui_same();
    ed_port_buttons(e);
    ed_graph_controls(e, p, true);

    if (ui_available().x < 400) {
        ui_newline();
    } else {
        ui_same();
    }

    ed_automation_selector(e);

    /* An automation pattern's controller is part of the pattern, so switching it is an undo
       step; a note pattern only remembers which lane is shown. */
    if (e->automation_cc != p->event) {
        if (p->kind) {
            ed_checkpoint(e);
        }

        p->event = e->automation_cc;

        if (p->kind) {
            ed_commit(e);
        }
    }

    if (e->automation_cc == DM_EVENT_BEND && !ed_graph_resolution(e)) {
        ui_same();
        ui_frame_toggle(DM_TEXT_MIDI_VALUES, &e->automation_raw);
    }

    ed_graph_resolution_controls(e);

    if (!p->mode) {
        automation_value_input(e);
        event_header_buttons(e, header.x + width, false);
    }

    ui_group_end();

    if (p->mode) {
        UiPoint next = ui_cursor();
        ui_position(fmaxf(header.x, header.x + width - 476), width >= 980 ? header.y : next.y);
        lfo_controls(e, p);
        UiPoint bottom = ui_cursor();
        ui_position(header.x, bottom.y);
    }
}

static void event_editor_scroll(Editor *e, UiRect r, UiRect lane, View *v, UiMouse mouse,
                                bool hovered) {
    if (hovered && mouse.wheel &&
        (ed_gesture_held(e, GESTURE_ZOOM, mouse) || mouse.pos.y < lane.y)) {
        ed_wheel_zoom(v, mouse.wheel, mouse.pos.x - lane.x);
    } else {
        automation_scroll(e, v, lane, mouse, hovered && ed_inside(mouse.pos, lane));
    }

    float total = (float)ed_pattern_length(e) * 80 * v->zoom;
    v->scroll_x = fmaxf(0, v->scroll_x);
    ed_horizontal_scroll(v, (UiRect){lane.x, r.y, lane.w, r.h}, total);
}

static void focus_event_pattern(Editor *e, UiRect lane, float scale) {
    if (!e->focus_pattern) {
        return;
    }

    e->focus_pattern = false;
    double first = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.curve < 0 || event_setup(a)) {
            continue;
        }

        first = a.beat;
        break;
    }

    e->roll_view.scroll_x = fmaxf(0, (float)first * scale - lane.w * .5f);
    e->roll_view.scroll_y = 0;
}

static void event_editor_vertical(Editor *e, UiRect r, UiRect lane) {
    if (e->automation_cc != DM_EVENT_BEND || ed_automation_raw(e)) {
        return;
    }

    float span = 127 * ED_LANE_STEP;
    float position = (127 - e->automation_center) * ED_LANE_STEP;
    float before = roundf(position);
    position = before;
    ui_scrollbar(DM_TEXT_EVENT_VERTICAL, (UiRect){r.x + r.w + 2, lane.y, 12, lane.h}, true,
                 &position, lane.h, span + lane.h);

    if (position != before) {
        e->automation_center = 127 - position / ED_LANE_STEP;
    }
}

void ed_event_editor(Editor *e, float width, float height) {
    ui_child(DM_TEXT_EVENT_EDITOR_TITLE, width, height, false);

    if (ui_panel_clicked()) {
        ed_focus_edit(e, FOCUS_EVENTS);
    }

    DmPattern *p = ed_pattern(e);

    if (!p) {
        ui_child_end();
        return;
    }

    event_editor_header(e, p);
    UiPoint at = ui_cursor(), space = ui_available();
    float label = 64, scale = 80 * e->roll_view.zoom;
    UiRect r = {at.x, at.y, space.x - 14, fmaxf(50, space.y - 24)};
    ui_canvas(DM_TEXT_EVENT_EDITOR_CANVAS, r.w, r.h);
    bool hovered = ui_hover();
    UiMouse mouse = ui_mouse();
    View *v = &e->roll_view, fit;
    UiRect lane = {r.x + label, r.y + 28, r.w - label, r.h - 28};
    focus_event_pattern(e, lane, scale);

    /* An LFO pattern shows one bar from its start, whatever its length: the wave repeats, so
       more bars add nothing. Only note-pitch bends scroll, vertically. */
    if (p->mode) {
        fit = *v;
        fit.scroll_x = 0;
        fit.zoom = lane.w / (80 * (float)fmax(1.0 / DM_PPQ, lfo_bar(e)));
        automation_scroll(e, &fit, lane, mouse, hovered && ed_inside(mouse.pos, lane));
        fit.scroll_x = 0;
        v = &fit;
    } else {
        event_editor_scroll(e, r, lane, v, mouse, hovered);
    }

    scale = 80 * v->zoom;
    ui_rect(r, BG, 0);
    ui_clip((UiRect){r.x, r.y + 28, label, r.h - 28});
    automation_axis(e, (UiPoint){r.x, lane.y}, lane);
    ui_unclip();
    ui_clip(lane);
    automation_grid(e, lane, v, scale);

    if (p->mode) {
        ed_lfo_center_drag(e, p, lane, mouse, hovered);
        lfo_plot(e, p, lane, v, scale);
    } else {
        int hit = ed_automation_plot(e, lane, v, scale, mouse);
        ed_automation_input(e, lane, v, scale, mouse, hovered, hit);
    }

    ui_unclip();
    ed_ruler(e, r, v, scale, label);
    event_editor_vertical(e, r, lane);

    ed_timeline_drag(e, r, v, scale, label, 2, hovered);
    ui_child_end();
}

int ed_velocity_delta(DmPattern *p, int hit, int delta, const DmNote *source) {
    bool group = source[hit].selected;
    int lo = -127, hi = 127;

    for (size_t i = 0; i < p->count; ++i) {
        if ((group && source[i].selected) || (int)i == hit) {
            lo = (int)fmax(lo, 1 - source[i].velocity);
            hi = (int)fmin(hi, 127 - source[i].velocity);
        }
    }

    delta = (int)fmax(lo, fmin(hi, delta));

    for (size_t i = 0; i < p->count; ++i) {
        if ((group && source[i].selected) || (int)i == hit) {
            p->notes[i].velocity = source[i].velocity + delta;
        }
    }

    return delta;
}

bool ed_note_velocity_wheel(Editor *e, DmPattern *p, UiRect r, View *v, float label, float row,
                            float scale, UiMouse mouse, bool hovered) {
    if (!hovered || !ed_gesture_held(e, GESTURE_VELOCITY_WHEEL, mouse) || !mouse.wheel ||
        mouse.pos.x < r.x + label) {
        return false;
    }

    double beat = (mouse.pos.x - r.x - label + v->scroll_x) / scale;
    int key = 127 - (int)floorf((mouse.pos.y - r.y - 28 + v->scroll_y) / row);

    for (size_t i = p->count; i > 0; --i) {
        DmNote n = p->notes[i - 1];

        if (n.key != key || beat < n.beat || beat > n.beat + n.length) {
            continue;
        }

        ed_checkpoint(e);
        ed_velocity_delta(p, (int)i - 1, (int)roundf(mouse.wheel * 5), p->notes);
        ed_commit(e);
        ed_focus_edit(e, FOCUS_NOTES);
        return true;
    }

    return false;
}

static UiPoint velocity_point(DmNote note, UiRect lane, View *v, float scale) {
    return (UiPoint){lane.x + (float)note.beat * scale - v->scroll_x,
                     lane.y + lane.h - 8 - note.velocity * (lane.h - 8) / 127.0f};
}

static void velocity_drag(Editor *e, DmPattern *p, UiRect lane, UiMouse mouse) {
    if (e->drag != DRAG_VELOCITY || e->drag_note_count != p->count || e->drag_index < 0) {
        return;
    }

    if (mouse.left && ed_gesture_moved(e, mouse)) {
        int target = (int)roundf(e->node_value + (e->node_y - mouse.pos.y) * 127 / (lane.h - 8));

        if (!ed_gesture_held(e, GESTURE_FREE, mouse)) {
            target = (int)lround(target / 10.0) * 10;
        }

        ed_begin_edit_move(e);
        ed_velocity_delta(p, e->drag_index, target - e->node_value, e->drag_notes);
        ed_changed(e);
    }

    if (mouse.released) {
        e->drag = DRAG_NONE;

        if (e->edit_moved) {
            ed_publish(e);
        }
    }
}

static void velocity_select(Editor *e, DmPattern *p, UiRect lane, View *v, float scale,
                            UiMouse mouse) {
    UiRect box = ed_marquee_box(e, mouse);

    for (size_t i = 0; i < p->count; ++i) {
        p->notes[i].selected =
            ed_marquee_retains(e, i, ed_gesture_held(e, GESTURE_EXTEND, mouse)) ||
            ed_inside(velocity_point(p->notes[i], lane, v, scale), box);
    }

    ui_border(box, e->accent, 0, 1);

    if (mouse.released || !mouse.left) {
        e->marquee = false;
    }
}

static void velocity_input(Editor *e, DmPattern *p, UiMouse mouse, bool hovered, int hit) {
    if (hovered && mouse.clicked) {
        ed_focus_edit(e, FOCUS_EVENTS);

        if (ed_gesture_held(e, GESTURE_MARQUEE, mouse) || e->tool == TOOL_MARQUEE) {
            ed_marquee_begin(e, FOCUS_EVENTS, mouse);
        } else if (hit >= 0) {
            if (!p->notes[hit].selected && !ed_gesture_held(e, GESTURE_EXTEND, mouse)) {
                ed_deselect(e);
            }

            p->notes[hit].selected = 1;
            free(e->drag_notes);
            e->drag_notes = malloc(p->count * sizeof(DmNote));
            e->drag_note_count = e->drag_notes ? p->count : 0;

            if (e->drag_notes) {
                memcpy(e->drag_notes, p->notes, p->count * sizeof(DmNote));
                e->drag = DRAG_VELOCITY;
                e->drag_index = hit;
                e->edit_moved = false;
                e->node_y = mouse.pos.y;
                e->node_value = p->notes[hit].velocity;
                e->marquee_start = mouse.pos;
            }
        }
    }
}

static void velocity_panel(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse,
                           bool hovered) {
    DmPattern *p = ed_pattern(e);
    ui_rect(lane, ui_background_shade(18), 0);

    /* Lines every 10, plus one at 127 along the top edge. */
    for (int value = 0; value <= 127; value = value == 120 ? 127 : value + 10) {
        float y = lane.y + lane.h - 8 - value * (lane.h - 8) / 127;
        ui_line((UiPoint){lane.x, y}, (UiPoint){lane.x + lane.w, y}, GRID, 1);
    }

    int hit = -1;
    ui_clip(lane);

    for (size_t i = 0; i < p->count; ++i) {
        UiPoint at = velocity_point(p->notes[i], lane, v, scale);
        uint32_t color = p->notes[i].selected ? e->accent : ed_point_color(e);
        ui_line((UiPoint){at.x, lane.y + lane.h - 8}, at, color, 2);
        ui_circle(at, 5, color);

        if (fabsf(at.x - mouse.pos.x) < 8 && fabsf(at.y - mouse.pos.y) < 8) {
            hit = (int)i;
        }
    }

    /* Over a point only while nothing else holds the mouse. */
    if ((hovered && hit >= 0 && !e->drag && !e->marquee) || e->drag == DRAG_VELOCITY) {
        ui_gesture_cursor(true);
    }

    velocity_input(e, p, mouse, hovered, hit);

    if (e->marquee && ed_selection_focus(e) == FOCUS_EVENTS) {
        velocity_select(e, p, lane, v, scale, mouse);
    }

    velocity_drag(e, p, lane, mouse);
    ui_unclip();
    ui_label((UiPoint){lane.x - 48, lane.y + 16}, DIM, DM_TEXT_VALUE_MAXIMUM);
    ui_label((UiPoint){lane.x - 48, lane.y + lane.h - 8}, DIM, DM_TEXT_VALUE_MINIMUM);
}

void ed_automation_panel(Editor *e, UiRect r, View *v, float scale, float label, UiMouse mouse) {
    if (!e->automation_open) {
        return;
    }

    if (ui_panel_clicked() && mouse.pos.y >= r.y + r.h + 22) {
        ed_focus_edit(e, FOCUS_EVENTS);
    }

    ui_position(r.x, r.y + r.h + 22);
    ui_canvas(DM_TEXT_EVENT_LANE_DIVIDER, r.w, 8);
    UiPoint divider = {r.x + r.w * .5f, r.y + r.h + 26};
    ui_line((UiPoint){r.x, divider.y}, (UiPoint){r.x + r.w, divider.y}, GRID, 1);
    ui_rect((UiRect){divider.x - 24, divider.y - 2, 48, 4}, DIM, 2);

    if (ui_hover() || ui_active()) {
        ui_resize_cursor(true);
    }

    /* Moving this divider trades height between the piano roll and the events section. */
    if (ui_active()) {
        float before = e->automation_height;
        e->automation_height =
            ed_clampf(before - mouse.delta.y, 100, fmaxf(100, before + e->roll_height - 116));
        e->roll_height -= e->automation_height - before;
    }

    float header_y = ui_cursor().y;
    DmPattern *pattern = ed_pattern(e);

    if (pattern && ed_graph_controls(e, pattern, false)) {
        ui_same();
    }

    ed_automation_selector(e);

    if (e->automation_cc == DM_EVENT_BEND && !ed_graph_resolution(e)) {
        ui_same();
        ui_frame_toggle(DM_TEXT_MIDI_VALUES, &e->automation_raw);
    }

    ed_graph_resolution_controls(e);

    if (e->automation_cc != DM_EVENT_VELOCITY) {
        automation_value_input(e);
    }

    event_header_buttons(e, r.x + r.w - 6, e->automation_cc != DM_EVENT_VELOCITY);

    UiPoint at = ui_cursor();
    UiRect lane = {at.x + label, at.y + 28, fmaxf(50, r.w - label),
                   fmaxf(35, e->automation_height - (at.y - header_y) - 53)};
    UiRect event_ruler = {at.x, at.y, r.w, lane.h + 28};
    ui_position(at.x, at.y);
    ui_canvas("automation", r.w, lane.h + 28);
    bool hovered = ui_hover();

    if (hovered && mouse.wheel &&
        (ed_gesture_held(e, GESTURE_ZOOM, mouse) || mouse.pos.y < lane.y)) {
        ed_wheel_zoom(v, mouse.wheel, mouse.pos.x - lane.x);
    } else {
        automation_scroll(e, v, lane, mouse, hovered && ed_inside(mouse.pos, lane));
    }

    scale = 80 * v->zoom;

    if (hovered && mouse.pos.y < lane.y && (mouse.clicked || mouse.right_clicked)) {
        e->event_ruler_drag = true;
    }

    if (e->event_ruler_drag) {
        ed_timeline_drag(e, event_ruler, v, scale, label, 2, hovered);
    }

    if (!mouse.left && !mouse.right) {
        e->event_ruler_drag = false;
    }

    if (e->automation_cc == DM_EVENT_VELOCITY) {
        velocity_panel(e, lane, v, scale, mouse, hovered && ed_inside(mouse.pos, lane));
        ed_ruler(e, event_ruler, v, scale, label);
        return;
    }

    ui_rect(lane, ui_background_shade(18), 0);
    ui_clip((UiRect){at.x, lane.y, label, lane.h});
    automation_axis(e, at, lane);
    ui_unclip();
    ui_clip(lane);
    automation_grid(e, lane, v, scale);
    int hit = ed_automation_plot(e, lane, v, scale, mouse);
    ed_automation_input(e, lane, v, scale, mouse, hovered, hit);
    ui_unclip();
    ed_ruler(e, event_ruler, v, scale, label);
}
