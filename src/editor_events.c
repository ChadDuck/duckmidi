#include "editor_internal.h"
#include "text/en.h"

static int event_identity(const void *left, const void *right);
static const char *automation_help(int controller);
static const char *automation_name(int cc);
static double next_pitch_range(Editor *e, double beat);
static void reencode_pitch(DmAutomation *a, float old);
static DmAutomation lane_edited(Editor *e, DmAutomation a, UiRect lane, float y, bool free);
static int next_connection(Editor *e, int left, bool overlay);
static int find_edited_event(Editor *e, DmAutomation a);
static void curve_drag_update(Editor *e, UiRect lane, UiMouse mouse);
static UiPoint connection_point(Editor *e, DmAutomation a, UiRect lane, View *v, float scale,
                                bool overlay);
static bool curve_handle_action(Editor *e, size_t index, UiRect lane, UiPoint mid, UiMouse mouse,
                                bool overlay, double origin, bool hovered);
static void event_drag_begin(Editor *e, int hit);
static void event_group_selection(Editor *e);
static double event_group_delta(Editor *e, UiRect lane, UiMouse mouse, EventDragItem *items,
                                size_t count, bool pitch);
static void event_group_values(Editor *e, UiRect lane, UiMouse mouse);
static bool event_group_drag(Editor *e, UiRect lane, float scale, UiMouse mouse);
static bool connected_click(UiMouse mouse);
static void event_placement_tip(Editor *e, UiRect lane, UiMouse mouse, bool hovered);
static bool curve_displayed(Editor *e, DmAutomation a);
static size_t selected_curve(Editor *e, int curve, double *first, double *last);
static bool copy_event_dependencies(Editor *e, DmProject *to, DmAutomation node, double origin);
static DmAutomation copied_connection(Editor *e, size_t index);
static void paste_discrete_pitch(Editor *e, const DmProject *from, DmAutomation *a,
                                 double source_beat);
static double pasted_section_start(const DmProject *from, int curve);
static bool later_selector(const DmProject *from, size_t index);
static DmAutomation *prepare_pasted_events(Editor *e, const DmProject *from, double at);
static bool paste_one_event(Editor *e, const DmProject *from, DmAutomation a, double at,
                            int offset);

bool ed_automation_matches(Editor *e, DmAutomation au) {
    if (au.pattern != e->selected_pattern || au.port != ed_note_port(e)) {
        return false;
    }

    if (e->automation_cc == -2) {
        return au.type == MIDI_PITCH_BEND;
    }

    if (e->automation_cc == -1) {
        return au.type == MIDI_PROGRAM;
    }

    if (e->automation_cc == -4) {
        return au.type == MIDI_CHANNEL_PRESSURE;
    }

    if (e->automation_cc == -5) {
        return au.type == MIDI_KEY_PRESSURE;
    }

    return au.type == MIDI_CONTROL && au.a == e->automation_cc;
}

bool ed_same_event(DmAutomation a, DmAutomation b) {
    return a.pattern == b.pattern && a.beat == b.beat && a.type == b.type &&
           a.channel == b.channel && a.a == b.a && a.b == b.b && a.curve == b.curve &&
           a.anchor == b.anchor && a.bend == b.bend && a.ramp == b.ramp && a.port == b.port &&
           a.hold == b.hold;
}

static int event_identity(const void *left, const void *right) {
    const DmAutomation *a = left, *b = right;

    if (a->port != b->port) {
        return (int)a->port - b->port;
    }

    if (a->beat != b->beat) {
        return a->beat < b->beat ? -1 : 1;
    }

    const int av[] = {a->pattern, a->type, a->channel, a->a, a->b, a->curve, a->anchor};
    const int bv[] = {b->pattern, b->type, b->channel, b->a, b->b, b->curve, b->anchor};

    for (int i = 0; i < 7; ++i) {
        if (av[i] != bv[i]) {
            return av[i] < bv[i] ? -1 : 1;
        }
    }

    if (a->bend != b->bend) {
        return a->bend < b->bend ? -1 : 1;
    }

    return (int)a->ramp - b->ramp;
}

bool ed_event_selected(Editor *e, size_t index) {
    DmAutomation a = e->project.automation[index];

    if (!ed_automation_matches(e, a) || a.curve < 0) {
        return false;
    }

    if (e->automation_all || e->automation_node == (int)index) {
        return true;
    }

    return e->event_selection.size &&
           bsearch(&a, e->event_selection.data, e->event_selection.size / sizeof(a), sizeof(a),
                   event_identity) != NULL;
}

static const char *automation_help(int controller) {
    switch (controller) {
    case -3:
        return DM_TEXT_VELOCITY_BELONGS_EACH;
    case -2:
        return DM_TEXT_PITCH_BEND_CHANGES;
    case 1:
        return DM_TEXT_MODULATION_CC1_COMMONLY;
    case 11:
        return DM_TEXT_EXPRESSION_CC11_SCALES;
    case 7:
        return DM_TEXT_CHANNEL_VOLUME_CC7_POINT;
    case 10:
        return DM_TEXT_PAN_CC10_0;
    case 64:
        return DM_TEXT_DRAG_ALONG_LINE_HOLD;
    case 66:
        return DM_TEXT_DRAG_ALONG_LINE_CAPTURE;
    case 5:
        return DM_TEXT_PORTAMENTO_HIGHER_VALUES;
    case 91:
        return DM_TEXT_REVERB_SEND_CC91_CONTROLS;
    case 93:
        return DM_TEXT_CHORUS_SEND_CC93_CONTROLS;
    case 74:
        return DM_TEXT_BRIGHTNESS_CC74_CHANGES;
    case 71:
        return DM_TEXT_RESONANCE_CC71_CHANGES;
    case 92:
        return DM_TEXT_TREMOLO_DEPTH_CC92_CONTROLS;
    case 76:
        return DM_TEXT_VIBRATO_RATE_CC76_CONTROLS;
    case 77:
        return DM_TEXT_VIBRATO_DEPTH_CC77_CONTROLS;
    default:
        return DM_TEXT_CONTROLLER_SECTIONS_AFFECT;
    }
}

static const char *automation_name(int cc) {
    switch (cc) {
    case -3:
        return DM_TEXT_VELOCITY;
    case -1:
        return DM_TEXT_PROGRAM_CHANGE;
    case -4:
        return DM_TEXT_CHANNEL_PRESSURE;
    case -5:
        return DM_TEXT_POLY_PRESSURE;
    case -2:
        return DM_TEXT_PITCH_BEND;
    case 1:
        return DM_TEXT_MODULATION;
    case 11:
        return DM_TEXT_EXPRESSION;
    case 7:
        return DM_TEXT_VOLUME;
    case 10:
        return DM_TEXT_PAN;
    case 64:
        return DM_TEXT_SUSTAIN;
    case 66:
        return DM_TEXT_SOSTENUTO;
    case 5:
        return DM_TEXT_PORTAMENTO;
    case 91:
        return DM_TEXT_REVERB_SEND;
    case 93:
        return DM_TEXT_CHORUS_SEND;
    case 74:
        return DM_TEXT_BRIGHTNESS;
    case 71:
        return DM_TEXT_RESONANCE;
    case 92:
        return DM_TEXT_TREMOLO_DEPTH;
    case 76:
        return DM_TEXT_VIBRATO_RATE;
    case 77:
        return DM_TEXT_VIBRATO_DEPTH;
    default:
        return DM_TEXT_CONTROLLER;
    }
}

bool ed_pedal_event(int cc) {
    return cc == 64 || cc == 66;
}

/* Events whose values never move between points (program changes select an instrument), so
   placing them never draws a line. */
static bool discrete_event(int cc) {
    return cc == -1;
}

bool ed_point_only_event(int cc) {
    return ed_pedal_event(cc) || cc == 5 || cc == -1 || cc == -4 || cc == -5;
}

bool ed_automation_option(Editor *e, int cc) {
    return (cc != 7 && cc != 10) || (ed_pattern(e) && ed_pattern(e)->kind);
}

static void choose_event_type(Editor *e, int value) {
    if (value != -3 && ed_pattern(e)->kind && ed_pattern(e)->event != value) {
        ed_checkpoint(e);
        ed_pattern(e)->event = value;

        if (ed_point_only_event(value)) {
            ed_pattern(e)->mode = 0;
        }

        ed_commit(e);
    }

    e->automation_cc = value;
    e->automation_node = -1;
    e->automation_all = false;
    e->event_selection.size = 0;
    e->event_drag.size = 0;
    e->last_automation_valid = false;
}

void ed_automation_selector(Editor *e) {
    static const int all_values[] = {-3, -2, 1,  11, 7,  10, 64, 66, 5, 91,
                                     93, 74, 71, 92, 76, 77, -1, -4, -5};
    static const char *all_labels[] = {
        DM_TEXT_VELOCITY,           DM_TEXT_PITCH_BEND,         DM_TEXT_MODULATION_CC1,
        DM_TEXT_EXPRESSION_CC11,    DM_TEXT_VOLUME_CC7,         DM_TEXT_PAN_CC10,
        DM_TEXT_SUSTAIN_CC64,       DM_TEXT_SOSTENUTO_CC66,     DM_TEXT_PORTAMENTO_CC5,
        DM_TEXT_REVERB_SEND_CC91,   DM_TEXT_CHORUS_SEND_CC93,   DM_TEXT_BRIGHTNESS_CC74,
        DM_TEXT_RESONANCE_CC71,     DM_TEXT_TREMOLO_DEPTH_CC92, DM_TEXT_VIBRATO_RATE_CC76,
        DM_TEXT_VIBRATO_DEPTH_CC77, DM_TEXT_PROGRAM_CHANGE,     DM_TEXT_CHANNEL_PRESSURE,
        DM_TEXT_POLY_PRESSURE};

    enum { TYPES = sizeof(all_values) / sizeof(*all_values) };

    /* Velocity belongs to note patterns only; other types follow the pattern's options. */
    int values[TYPES], count = 0, current = -1;

    for (int i = 0; i < TYPES; ++i) {
        bool available = all_values[i] == -3 ? ed_pattern(e) && !ed_pattern(e)->kind
                                             : ed_automation_option(e, all_values[i]);

        if (available) {
            current = all_values[i] == e->automation_cc ? count : current;
            values[count++] = i;
        }
    }

    const char *preview = automation_name(e->automation_cc);
    ui_width((ed_pattern(e) && ed_pattern(e)->kind && ed_pattern(e)->mode) || ui_available().x < 600
                 ? 130
                 : 220);

    if (ui_combo("##Event type", preview)) {
        for (int i = 0; i < count; ++i) {
            if (ui_select(all_labels[values[i]], i == current, 0, 0)) {
                choose_event_type(e, all_values[values[i]]);
            }
        }

        ui_combo_end();
    }

    ui_tip(DM_TEXT_CHOOSE_EVENT_TYPE);
    ui_same();
    ui_info(automation_help(e->automation_cc));
}

float ed_lane_step(UiRect lane) {
    (void)lane;
    return 20;
}

float ed_automation_range(Editor *e, DmAutomation a) {
    PitchState state = ed_pattern_pitch_state(&e->project, a.pattern, a.beat, a.port);

    if (a.curve > 0) {
        for (size_t i = 0; i < e->project.automation_count; ++i) {
            DmAutomation item = e->project.automation[i];

            if (item.pattern != a.pattern || item.port != a.port || item.curve != a.curve ||
                item.type != MIDI_CONTROL) {
                continue;
            }

            if (item.a == MIDI_CC_DATA) {
                state.range = item.b;
            }

            if (item.a == MIDI_CC_DATA_LSB) {
                state.cents = item.b;
            }
        }
    }

    return fmaxf(.01f, state.range + state.cents * .01f);
}

float ed_automation_pitch(Editor *e, DmAutomation a) {
    float range = ed_automation_range(e, a);
    int anchor = a.curve ? a.anchor : ed_nearest_note(e, 0);
    return anchor + (ed_event_value(a) - MIDI_BEND_CENTER) * range / MIDI_BEND_CENTER;
}

UiPoint ed_automation_point(Editor *e, DmAutomation a, UiRect lane, View *v, float scale) {
    float value = ed_event_value(a),
          maximum = a.type == MIDI_PITCH_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;
    float y = lane.y + lane.h - 8 - value / maximum * (lane.h - 8);

    if (a.type == MIDI_PITCH_BEND && !e->automation_raw) {
        y = lane.y + lane.h * .5f -
            (ed_automation_pitch(e, a) - e->automation_center) * ed_lane_step(lane);
    }

    if (ed_pedal_event(e->automation_cc)) {
        y = lane.y + lane.h * .5f;
    }

    return (UiPoint){lane.x + (float)a.beat * scale - v->scroll_x, y};
}

float ed_curve_units(Editor *e, DmAutomation a, UiRect lane, bool overlay) {
    if (a.type == MIDI_PITCH_BEND && (overlay || !e->automation_raw)) {
        return 20 * ed_automation_range(e, a) / MIDI_BEND_CENTER;
    }

    return (lane.h - 8) / (a.type == MIDI_PITCH_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX);
}

void ed_automation_connection(UiPoint previous, UiPoint point, DmAutomation before, DmAutomation a,
                              float units, uint32_t color) {
    if (a.curve == before.curve && dm_automation_curves(before)) {
        UiPoint last = previous;
        int steps = (int)fmax(8, fmin(256, fabsf(point.x - previous.x) / 4));

        for (int i = 1; i <= steps; ++i) {
            double t = (double)i / steps;
            double linear =
                ed_event_value(before) + (ed_event_value(a) - ed_event_value(before)) * t;
            UiPoint next = {previous.x + (point.x - previous.x) * (float)t,
                            previous.y + (point.y - previous.y) * (float)t -
                                (float)(dm_curve_value(before, a, t) - linear) * units};
            ui_line(last, next, color, 1.5f);
            last = next;
        }
    } else if (a.curve == before.curve && a.curve >= 0 && a.type != MIDI_PROGRAM) {
        /* Hold: the value stays until the next point, then steps to it. */
        ui_line(previous, (UiPoint){point.x, previous.y}, color, 1.5f);
        ui_line((UiPoint){point.x, previous.y}, point, color, 1.5f);
    }
}

int ed_automation_plot(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    UiPoint previous = {0};
    bool has = false;
    int hit = -1;
    DmAutomation before = {0};

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (!ed_automation_matches(e, a) || a.curve < 0) {
            continue;
        }

        UiPoint point = ed_automation_point(e, a, lane, v, scale);
        bool pedal = ed_pedal_event(e->automation_cc);

        /* Points are in time order. Off-screen points only matter as the start or end of a
           connection that crosses into view. */
        bool left = point.x < lane.x - 8, beyond = has && previous.x > lane.x + lane.w + 8;

        if (beyond) {
            break;
        }

        if (left && (!has || previous.x < lane.x - 8)) {
            previous = point;
            has = true;
            before = a;
            continue;
        }

        if (has) {
            if (pedal) {
                if (a.curve > 0 && a.curve == before.curve) {
                    ui_line(previous, point, GOLD, 4);
                }
            } else {
                ed_automation_connection(previous, point, before, a,
                                         ed_curve_units(e, before, lane, false), GOLD);
            }
        }

        bool end = pedal && has && a.curve > 0 && a.curve == before.curve;
        uint32_t color = ed_event_selected(e, i) ? TEXT : GOLD;

        if (end) {
            ui_circle_outline(point, ed_event_selected(e, i) ? 7 : 5, color);
        } else {
            ui_circle(point, ed_event_selected(e, i) ? 7 : 5, color);
        }

        if (fabsf(mouse.pos.x - point.x) < 9 && fabsf(mouse.pos.y - point.y) < 9) {
            hit = (int)i;
        }

        previous = point;
        has = true;
        before = a;
    }

    return hit;
}

void ed_sort_automation(Editor *e) {
    for (size_t i = 1; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        size_t j = i;

        while (j && dm_automation_later(e->project.automation[j - 1], a)) {
            e->project.automation[j] = e->project.automation[j - 1];
            --j;
        }

        e->project.automation[j] = a;
    }
}

int ed_lane_value(Editor *e, UiRect lane, float y, DmAutomation a, bool free) {
    int max = a.type == MIDI_PITCH_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;

    if (a.type == MIDI_PITCH_BEND && !e->automation_raw) {
        float pitch = ed_clampf(
            e->automation_center + (lane.y + lane.h * .5f - y) / ed_lane_step(lane), 0, 127);

        if (!free) {
            pitch = roundf(pitch);
        }

        float range = a.curve == INT32_MAX ? e->bend_range : ed_automation_range(e, a);
        return (int)ed_clampf(MIDI_BEND_CENTER +
                                  (pitch - (a.curve ? a.anchor : ed_nearest_note(e, 0))) *
                                      MIDI_BEND_CENTER / range,
                              0, MIDI_BEND_MAX);
    }

    float value = (lane.y + lane.h - 8 - y) / (lane.h - 8) * max;
    return (int)ed_clampf(value, 0, (float)max);
}

static double next_pitch_range(Editor *e, double beat) {
    PitchState state = {2, 0, 127, 127};

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) ||
            a.type != MIDI_CONTROL) {
            continue;
        }

        if (a.a == MIDI_CC_RPN_MSB) {
            state.msb = a.b;
        } else if (a.a == MIDI_CC_RPN_LSB) {
            state.lsb = a.b;
        } else if ((a.a == MIDI_CC_DATA || a.a == MIDI_CC_DATA_LSB) && state.msb == 0 &&
                   state.lsb == 0 && a.beat > beat) {
            return a.beat;
        }
    }

    return HUGE_VAL;
}

static void reencode_pitch(DmAutomation *a, float old) {
    if (a->type == MIDI_PITCH_BEND) {
        a->bend *= old / 127;
        ed_set_event_value(
            a, (int)lround(MIDI_BEND_CENTER + (ed_event_value(*a) - MIDI_BEND_CENTER) * old / 127));
    } else if (a->type == MIDI_CONTROL) {
        if (a->a == MIDI_CC_DATA) {
            a->b = 127;
        } else if (a->a == MIDI_CC_DATA_LSB) {
            a->b = 0;
        }
    }
}

DmAutomation ed_expand_pitch_range(Editor *e, DmAutomation node, float pitch) {
    float old = ed_automation_range(e, node);
    int anchor = node.curve ? node.anchor : ed_nearest_note(e, 0);

    if (old >= 127 || fabsf(pitch - anchor) < old - .001f) {
        return node;
    }

    double end = node.curve ? HUGE_VAL : next_pitch_range(e, node.beat);
    DmAutomation updated = node;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation *a = &e->project.automation[i];
        bool same = node.curve ? a->curve == node.curve
                               : a->curve == 0 && a->beat >= node.beat && a->beat < end;

        if (a->pattern != node.pattern || a->port != node.port || !same) {
            continue;
        }

        bool selected = ed_same_event(*a, node);

        if (node.curve || a->type == MIDI_PITCH_BEND) {
            reencode_pitch(a, old);
        }

        if (selected) {
            updated = *a;
        }
    }

    if (!node.curve) {
        PitchState previous = ed_pitch_state(e, node.beat);
        ed_curve_range(e, fmax(0, node.beat - .5 / DM_PPQ), 127, 0, previous.msb, previous.lsb, 0);
    }

    return updated;
}

static DmAutomation lane_edited(Editor *e, DmAutomation a, UiRect lane, float y, bool free) {
    if (a.type == MIDI_PITCH_BEND && !e->automation_raw) {
        float pitch = ed_clampf(
            e->automation_center + (lane.y + lane.h * .5f - y) / ed_lane_step(lane), 0, 127);
        a = ed_expand_pitch_range(e, a, free ? pitch : roundf(pitch));
    }

    return a;
}

bool ed_connection_pair(Editor *e, DmAutomation a, DmAutomation b, bool overlay) {
    bool visible = overlay ? b.pattern == e->selected_pattern && b.port == ed_note_port(e) &&
                                 b.type == MIDI_PITCH_BEND
                           : ed_automation_matches(e, b);
    return visible && a.port == b.port && b.curve >= 0 && b.curve == a.curve && b.beat > a.beat &&
           b.type == a.type && (a.type != MIDI_CONTROL || a.a == b.a);
}

static int next_connection(Editor *e, int left, bool overlay) {
    if (left < 0 || (size_t)left >= e->project.automation_count) {
        return -1;
    }

    DmAutomation a = e->project.automation[left];

    for (size_t i = (size_t)left + 1; i < e->project.automation_count; ++i) {
        DmAutomation b = e->project.automation[i];
        bool visible = overlay ? b.pattern == e->selected_pattern && b.port == ed_note_port(e) &&
                                     b.type == MIDI_PITCH_BEND
                               : ed_automation_matches(e, b);

        if (!visible || b.curve < 0) {
            continue;
        }

        return ed_connection_pair(e, a, b, overlay) ? (int)i : -1;
    }

    return -1;
}

static int find_edited_event(Editor *e, DmAutomation a) {
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (ed_same_event(e->project.automation[i], a)) {
            return (int)i;
        }
    }

    return -1;
}

static void curve_drag_update(Editor *e, UiRect lane, UiMouse mouse) {
    int left = e->curve_left, right = next_connection(e, left, e->curve_overlay);

    if (right < 0) {
        e->curve_drag = false;
        return;
    }

    if (mouse.left && (e->curve_changed || fabsf(mouse.pos.y - e->curve_y) > 1)) {
        if (!e->curve_changed) {
            ed_checkpoint(e);
            e->curve_changed = true;
        }

        DmAutomation a = e->project.automation[left], b = e->project.automation[right];
        double bend;

        if (a.type == MIDI_PITCH_BEND && (e->curve_overlay || !e->automation_raw)) {
            double semitones = e->curve_origin * e->curve_sensitivity / MIDI_BEND_CENTER +
                               (e->curve_y - mouse.pos.y) / 20;
            float pitch =
                (ed_automation_pitch(e, a) + ed_automation_pitch(e, b)) * .5f + (float)semitones;
            a = ed_expand_pitch_range(e, a, pitch);
            left = find_edited_event(e, a);

            if (left < 0) {
                e->curve_drag = false;
                return;
            }

            e->curve_left = left;
            bend = semitones * MIDI_BEND_CENTER / ed_automation_range(e, a);
        } else {
            bend = e->curve_origin + (e->curve_y - mouse.pos.y) / ed_curve_units(e, a, lane, false);
        }

        e->project.automation[left].bend = fmax(-MIDI_BEND_MAX, fmin(MIDI_BEND_MAX, bend));
        e->project.automation[left].ramp = true;
        e->project.automation[left].hold = false;
        ed_changed(e);
    }

    if (mouse.released) {
        e->curve_drag = false;

        if (e->curve_changed) {
            ed_publish(e);
        }
    }
}

static UiPoint connection_point(Editor *e, DmAutomation a, UiRect lane, View *v, float scale,
                                bool overlay) {
    if (!overlay) {
        return ed_automation_point(e, a, lane, v, scale);
    }

    return (UiPoint){lane.x + (float)a.beat * scale - v->scroll_x,
                     lane.y + (127 - ed_automation_pitch(e, a)) * 20 - v->scroll_y + 10};
}

static bool curve_handle_action(Editor *e, size_t index, UiRect lane, UiPoint mid, UiMouse mouse,
                                bool overlay, double origin, bool hovered) {
    DmAutomation a = e->project.automation[index];
    bool hit = hovered && ed_inside(mouse.pos, lane) && fabsf(mouse.pos.x - mid.x) < 8 &&
               fabsf(mouse.pos.y - mid.y) < 8;

    if (hit) {
        ui_gesture_cursor(true);
    }

    if (!hit || mouse.shift || mouse.ctrl || e->tool == 1 || (overlay && mouse.alt)) {
        return false;
    }

    if (mouse.middle_clicked) {
        e->pan_context = 0;
        ed_checkpoint(e);
        e->project.automation[index].bend = 0;
        e->project.automation[index].ramp = true;
        e->project.automation[index].hold = false;
        ed_commit(e);
        return true;
    }

    if (mouse.clicked) {
        ed_focus_edit(e, FOCUS_EVENTS);
        e->curve_drag = true;
        e->drag = 0;
        e->event_selection.size = 0;
        e->curve_changed = false;
        e->curve_overlay = overlay;
        e->curve_left = (int)index;
        e->curve_y = mouse.pos.y;
        e->curve_origin = origin;
        e->curve_sensitivity = ed_automation_range(e, a);
        return true;
    }

    return false;
}

bool ed_curve_handles(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse, bool overlay,
                      bool hovered) {
    if (e->curve_drag) {
        if (e->curve_overlay == overlay) {
            curve_drag_update(e, lane, mouse);
        }

        return true;
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        bool visible = overlay ? a.pattern == e->selected_pattern && a.type == MIDI_PITCH_BEND
                               : ed_automation_matches(e, a);

        if (!visible || a.curve < 0 || a.type == MIDI_PROGRAM) {
            continue;
        }

        int right = next_connection(e, (int)i, overlay);

        if (right < 0) {
            continue;
        }

        DmAutomation b = e->project.automation[right];
        UiPoint first = connection_point(e, a, lane, v, scale, overlay),
                last = connection_point(e, b, lane, v, scale, overlay);
        double linear = (ed_event_value(a) + ed_event_value(b)) * .5;
        bool step = !dm_automation_curves(a);
        UiPoint mid = {(first.x + last.x) * .5f,
                       step
                           ? first.y
                           : (first.y + last.y) * .5f - (float)(dm_curve_value(a, b, .5) - linear) *
                                                            ed_curve_units(e, a, lane, overlay)};

        if (!ed_inside(mid, lane)) {
            continue;
        }

        ui_circle_outline(mid, 5, GOLD);

        if (curve_handle_action(e, i, lane, mid, mouse, overlay,
                                step ? ed_event_value(a) - linear : a.bend, hovered)) {
            return true;
        }
    }

    return false;
}

void ed_curve_restore_position(Editor *e, int id) {
    if (id <= 0) {
        return;
    }

    double start = HUGE_VAL, end = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern == e->selected_pattern && a.curve == id && ed_automation_matches(e, a)) {
            start = fmin(start, a.beat);
            end = fmax(end, a.beat);
        }
    }

    if (!isfinite(start)) {
        return;
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation *a = &e->project.automation[i];

        if (a->pattern != e->selected_pattern || a->port != ed_note_port(e)) {
            continue;
        }

        if (a->curve == -id) {
            a->beat = end;
        } else if (a->curve == id && !ed_automation_matches(e, *a)) {
            a->beat = start;
        }
    }
}

void ed_select_edited_node(Editor *e, DmAutomation edited) {
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation candidate = e->project.automation[i];

        if (candidate.pattern == edited.pattern && candidate.curve == edited.curve &&
            candidate.type == edited.type && candidate.beat == edited.beat &&
            candidate.a == edited.a && candidate.b == edited.b) {
            e->automation_node = (int)i;
            break;
        }
    }
}

static void event_drag_begin(Editor *e, int hit) {
    bool group = ed_event_selected(e, (size_t)hit);

    if (!group) {
        e->event_selection.size = 0;
    }

    e->event_drag.size = 0;
    size_t count = e->project.automation_count;

    if (!dm_reserve((void **)&e->event_drag.data, &e->event_drag.capacity,
                    count * sizeof(EventDragItem), 1)) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if ((group && ed_event_selected(e, i)) || (int)i == hit) {
            EventDragItem item = {i, e->project.automation[i],
                                  ed_automation_pitch(e, e->project.automation[i])};
            memcpy(e->event_drag.data + e->event_drag.size, &item, sizeof(item));
            e->event_drag.size += sizeof(item);
        }
    }
}

static void event_group_selection(Editor *e) {
    e->event_selection.size = 0;
    size_t count = e->event_drag.size / sizeof(EventDragItem);
    EventDragItem *items = (EventDragItem *)e->event_drag.data;

    if (!dm_reserve((void **)&e->event_selection.data, &e->event_selection.capacity,
                    count * sizeof(DmAutomation), 1)) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        DmAutomation a = e->project.automation[items[i].index];
        memcpy(e->event_selection.data + e->event_selection.size, &a, sizeof(a));
        e->event_selection.size += sizeof(a);
    }

    qsort(e->event_selection.data, count, sizeof(DmAutomation), event_identity);
}

static double event_group_delta(Editor *e, UiRect lane, UiMouse mouse, EventDragItem *items,
                                size_t count, bool pitch) {
    double delta = 0, lo = -INFINITY, hi = INFINITY;

    for (size_t i = 0; i < count; ++i) {
        double value = pitch ? items[i].pitch : ed_event_value(items[i].original);
        lo = fmax(lo, -value);
        hi = fmin(hi, (pitch || e->automation_cc != -2 ? 127 : MIDI_BEND_MAX) - value);
    }

    if (!mouse.shift) {
        delta = (e->node_y - mouse.pos.y) *
                (pitch ? 1 / ed_lane_step(lane)
                       : (e->automation_cc == -2 ? MIDI_BEND_MAX : MIDI_DATA_MAX) / (lane.h - 8));
        delta = mouse.alt && pitch ? delta : round(delta);
        delta = fmax(lo, fmin(hi, delta));
    }

    return delta;
}

static void event_group_values(Editor *e, UiRect lane, UiMouse mouse) {
    if (ed_pedal_event(e->automation_cc)) {
        return;
    }

    EventDragItem *items = (EventDragItem *)e->event_drag.data;
    size_t count = e->event_drag.size / sizeof(*items);
    bool pitch = e->automation_cc == -2 && !e->automation_raw;
    double delta = event_group_delta(e, lane, mouse, items, count, pitch);

    if (pitch) {
        for (size_t i = 0; i < count; ++i) {
            DmAutomation a = e->project.automation[items[i].index];
            ed_expand_pitch_range(e, a, items[i].pitch + (float)delta);
        }
    }

    for (size_t i = 0; i < count; ++i) {
        DmAutomation *a = &e->project.automation[items[i].index];
        int value =
            pitch
                ? (int)lround(MIDI_BEND_CENTER + (items[i].pitch + delta -
                                                  (a->curve ? a->anchor : ed_nearest_note(e, 0))) *
                                                     MIDI_BEND_CENTER / ed_automation_range(e, *a))
                : ed_event_value(items[i].original) + (int)delta;
        ed_set_event_value(a, value);
    }
}

static bool event_group_drag(Editor *e, UiRect lane, float scale, UiMouse mouse) {
    size_t count = e->event_drag.size / sizeof(EventDragItem);

    if (e->drag != 5 || count < 2) {
        return false;
    }

    EventDragItem *items = (EventDragItem *)e->event_drag.data;

    if (mouse.left && ed_gesture_moved(e, mouse)) {
        ed_begin_edit_move(e);
        double delta = mouse.ctrl
                           ? 0
                           : ed_snap(e->node_beat + (mouse.pos.x - e->marquee_start.x) / scale, .25,
                                     mouse.alt) -
                                 e->node_beat;

        for (size_t i = 0; i < count; ++i) {
            delta = fmax(delta, -items[i].original.beat);
        }

        for (size_t i = 0; i < count; ++i) {
            e->project.automation[items[i].index].beat = items[i].original.beat + delta;
        }

        event_group_values(e, lane, mouse);
        e->automation_all = false;
        event_group_selection(e);

        if (e->automation_cc == -2) {
            ed_edit_feedback(e, ed_automation_pitch(e, e->project.automation[e->automation_node]));
        }

        ed_changed(e);
    }

    if (mouse.released) {
        if (e->edit_moved) {
            for (size_t i = 0; i < count; ++i) {
                ed_curve_restore_position(e, items[i].original.curve);
            }

            event_group_selection(e);
            ed_sort_automation(e);
            dm_pattern_fit(&e->project, ed_pattern(e));
            ed_commit(e);
        }

        e->drag = 0;
        e->automation_node = -1;
        e->event_drag.size = 0;
    }

    return true;
}

void ed_automation_node_drag(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    if (event_group_drag(e, lane, scale, mouse)) {
        return;
    }

    if (e->drag != 5 || e->automation_node < 0 ||
        (size_t)e->automation_node >= e->project.automation_count) {
        return;
    }

    DmAutomation *a = &e->project.automation[e->automation_node];

    if (mouse.left && ed_gesture_moved(e, mouse)) {
        ed_begin_edit_move(e);
        DmAutomation edited = mouse.shift ? *a : lane_edited(e, *a, lane, mouse.pos.y, mouse.alt);
        e->automation_node = -1;
        ed_select_edited_node(e, edited);
        a = &e->project.automation[e->automation_node];

        if (!mouse.ctrl) {
            a->beat =
                ed_snap(e->node_beat + (mouse.pos.x - e->marquee_start.x) / scale, .25, mouse.alt);
        }

        if (!mouse.shift && !ed_pedal_event(e->automation_cc)) {
            ed_set_event_value(a, ed_lane_value(e, lane, mouse.pos.y, *a, mouse.alt));
        }

        if (a->type == MIDI_PITCH_BEND) {
            ed_edit_feedback(e, ed_automation_pitch(e, *a));
        }

        ed_changed(e);
    }

    if (mouse.released && !e->edit_moved) {
        e->drag = 0;
        return;
    }

    if (mouse.released) {
        DmAutomation edited = *a;
        ed_curve_restore_position(e, a->curve);
        ed_sort_automation(e);
        e->automation_node = -1;
        ed_select_edited_node(e, edited);
        dm_pattern_fit(&e->project, ed_pattern(e));
        e->drag = 0;
        ed_commit(e);
    }

    (void)v;
}

int ed_closest_node(Editor *e, double beat) {
    int closest = -1;
    double distance = HUGE_VAL;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (!ed_automation_matches(e, a) || a.curve < 0) {
            continue;
        }

        DmAutomation last = e->last_automation;

        if (e->last_automation_valid && last.pattern == a.pattern && last.curve == a.curve &&
            last.type == a.type && last.a == a.a && last.b == a.b && last.beat == a.beat &&
            beat >= last.beat) {
            return (int)i;
        }

        double d = fabs(a.beat - beat);

        if (d < distance) {
            distance = d;
            closest = (int)i;
        }
    }

    return closest;
}

DmAutomation ed_split_curve_node(Editor *e, int left, DmAutomation last, double beat) {
    DmAutomation first = e->project.automation[left], node = first;
    double t = (beat - first.beat) / (last.beat - first.beat);
    node.beat = beat;
    ed_set_event_value(&node, (int)llround(dm_curve_value(first, last, t)));
    e->project.automation[left].bend = first.bend * t * t;
    node.bend = first.bend * (1 - t) * (1 - t);
    return node;
}

bool ed_insert_lane_node(Editor *e, double beat) {
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (!ed_automation_matches(e, a) || a.curve < 0 || (!a.curve && !a.ramp)) {
            continue;
        }

        int right = next_connection(e, (int)i, false);

        if (right < 0 || beat <= a.beat || beat >= e->project.automation[right].beat) {
            continue;
        }

        ed_checkpoint(e);
        DmAutomation node = ed_split_curve_node(e, (int)i, e->project.automation[right], beat);
        dm_automation_add(&e->project, node);
        e->last_automation = node;
        e->last_automation_valid = true;
        ed_commit(e);
        return true;
    }

    return false;
}

static bool events_selected(Editor *e) {
    if (e->automation_all || e->event_selection.size) {
        return true;
    }

    return e->automation_node >= 0 && (size_t)e->automation_node < e->project.automation_count &&
           ed_event_selected(e, (size_t)e->automation_node);
}

/* A standalone point at the cursor: a plain click, or Shift + click with no point to connect to. */
static void add_cursor_point(Editor *e, UiRect lane, UiMouse mouse, double beat) {
    DmAutomation point = ed_curve_node(e, beat, 0, e->automation_cc, 0, ed_curve_anchor(e, beat));

    if (point.type == MIDI_PITCH_BEND) {
        point = lane_edited(e, point, lane, mouse.pos.y, mouse.alt);
    }

    ed_set_event_value(&point, ed_lane_value(e, lane, mouse.pos.y, point, mouse.alt));
    ed_checkpoint(e);
    dm_automation_add(&e->project, point);
    e->last_automation = point;
    e->last_automation_valid = true;
    ed_sort_automation(e);
    ed_commit(e);
}

/* Adds a point at beat and the value at y, joined to the curve of the nearest point (index
   nearest), converting a discrete point into a curve with a restoration node. Returns the added
   point. The caller owns the undo step and sorts afterwards. */
static DmAutomation connect_point(Editor *e, UiRect lane, int nearest, float y, double beat,
                                  bool free) {
    DmAutomation a = e->project.automation[nearest];
    double original_beat = a.beat;
    bool converted = !a.curve && a.type != MIDI_PROGRAM;
    PitchState incoming = ed_pitch_state(e, fmin(a.beat, beat));
    int baseline = 0;

    if (converted) {
        baseline = ed_value_before(e, fmax(0, fmin(a.beat, beat) - 1e-8), e->automation_cc);
        a.curve = ed_curve_id(e);
        a.anchor = ed_nearest_note(e, 0);
        e->project.automation[nearest].curve = a.curve;
        e->project.automation[nearest].anchor = a.anchor;

        if (a.type == MIDI_PITCH_BEND) {
            ed_curve_range(e, fmin(a.beat, beat), incoming.range, incoming.cents, incoming.msb,
                           incoming.lsb, a.curve);
        }
    }

    a.beat = beat;
    a = lane_edited(e, a, lane, y, free);
    ed_set_event_value(&a, ed_lane_value(e, lane, y, a, free));
    dm_automation_add(&e->project, a);
    e->last_automation = a;
    e->last_automation_valid = true;

    if (converted) {
        DmAutomation restore = a;
        restore.curve = -a.curve;
        ed_set_event_value(&restore, baseline);
        dm_automation_add(&e->project, restore);

        if (a.type == MIDI_PITCH_BEND) {
            ed_curve_range(e, fmax(a.beat, original_beat), incoming.range, incoming.cents,
                           incoming.msb, incoming.lsb, -a.curve);
        }
    }

    ed_curve_restore_position(e, a.curve);
    return a;
}

/* Shift + click: a point at the cursor connected to the nearest point's curve, or a standalone
   point when the lane has none. */
static void add_connected_point(Editor *e, UiRect lane, UiMouse mouse, double beat) {
    int nearest = ed_closest_node(e, beat);

    if (nearest < 0) {
        add_cursor_point(e, lane, mouse, beat);
        return;
    }

    ed_checkpoint(e);
    connect_point(e, lane, nearest, mouse.pos.y, beat, mouse.alt);
    ed_sort_automation(e);
    ed_commit(e);
}

/* Shift + drag: the drag's start joins the nearest point's curve, and the line to the release
   continues that curve. Returns false when the lane has no point to join. */
static bool connected_line(Editor *e, UiRect lane, UiMouse mouse, double start, double end) {
    int nearest = ed_closest_node(e, start);

    if (nearest < 0) {
        return false;
    }

    DmAutomation a = connect_point(e, lane, nearest, e->marquee_start.y, start, mouse.alt);

    if (a.curve <= 0 || fabs(end - start) < 1.0 / DM_PPQ) {
        return true;
    }

    DmAutomation b = a;
    b.beat = end;
    b = lane_edited(e, b, lane, mouse.pos.y, mouse.alt);
    ed_set_event_value(&b, ed_lane_value(e, lane, mouse.pos.y, b, mouse.alt));
    dm_automation_add(&e->project, b);
    e->last_automation = b;
    e->last_automation_valid = true;
    ed_curve_restore_position(e, b.curve);
    return true;
}

/* The drawn line's end as it will be placed: snapped in time and value unless Alt is held. */
/* Screen-only: it never builds a point, since range expansion for pitch bends edits the project.
   Pitch bends snap to semitone rows and other controllers to whole values, as on release. */
static UiPoint placed_point(Editor *e, UiRect lane, View *v, float scale, UiPoint at, bool free) {
    double beat = fmax(0, ed_snap((at.x - lane.x + v->scroll_x) / scale, .25, free));
    float y, middle = lane.y + lane.h * .5f;

    if (ed_pedal_event(e->automation_cc)) {
        y = middle;
    } else if (e->automation_cc == -2 && !e->automation_raw) {
        float step = ed_lane_step(lane);
        float pitch = ed_clampf(e->automation_center + (middle - at.y) / step, 0, 127);
        pitch = free ? pitch : roundf(pitch);
        y = middle - (pitch - e->automation_center) * step;
    } else {
        float maximum = e->automation_cc == -2 ? MIDI_BEND_MAX : MIDI_DATA_MAX;
        float value = ed_clampf((lane.y + lane.h - 8 - at.y) / (lane.h - 8) * maximum, 0, maximum);
        y = lane.y + lane.h - 8 - floorf(value) / maximum * (lane.h - 8);
    }

    return (UiPoint){lane.x + (float)beat * scale - v->scroll_x, y};
}

/* Shift + right click on a point opens its menu, as notes and placements have: delete it, change
   its value, or make a discrete point curve toward the next one. */
static void point_menu(Editor *e, int hit) {
    if (hit >= 0) {
        e->context_point = hit;
        ed_focus_edit(e, FOCUS_EVENTS);
    }

    if (!ui_mouse_popup(DM_TEXT_EVENT_POINT, hit >= 0)) {
        return;
    }

    int index = e->context_point;

    if (index < 0 || (size_t)index >= e->project.automation_count) {
        ui_popup_end();
        return;
    }

    DmAutomation *a = &e->project.automation[index];
    int value = ed_event_value(*a);
    int maximum = a->type == MIDI_PITCH_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;

    if (ui_slider(DM_TEXT_VALUE, &value, 0, maximum, 180)) {
        ed_checkpoint(e);
        ed_set_event_value(a, value);
        ed_commit(e);
    }

    /* Curve: Line moves smoothly to the next point; Hold keeps this value until it. */
    if (a->curve >= 0 && a->type != MIDI_PROGRAM && ui_menu(DM_TEXT_CURVE)) {
        bool line = dm_automation_curves(*a);

        if (ui_menu_check(DM_TEXT_CURVE_LINE, line) && !line) {
            ed_checkpoint(e);
            a->ramp = !a->curve;
            a->hold = false;
            ed_commit(e);
        }

        if (ui_menu_check(DM_TEXT_CURVE_HOLD, !line) && line) {
            ed_checkpoint(e);
            a->ramp = false;
            a->hold = a->curve > 0;
            ed_commit(e);
        }

        ui_menu_end();
    }

    if (ui_menu_item(DM_TEXT_DELETE, NULL, true)) {
        ed_erase_node(e, index);
        e->context_point = -1;
    }

    ui_popup_end();
}

/* Lane clicks: a click adds a single point, Shift + click a point connected to the nearest one,
   and a drag draws a line. A left click while points are selected only clears the selection. */
void ed_automation_lane_click(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse,
                              int hit) {
    ed_focus_edit(e, FOCUS_EVENTS);

    /* The right button erases points; it never starts a left-button gesture. */
    if (mouse.right_clicked) {
        return;
    }

    if (hit >= 0) {
        event_drag_begin(e, hit);
        e->automation_all = false;
        e->edit_moved = false;
        e->drag = 5;
        e->automation_node = hit;
        e->node_beat = e->project.automation[hit].beat;
        e->node_value = ed_event_value(e->project.automation[hit]);
        e->node_y = mouse.pos.y;
        e->marquee_start = mouse.pos;
        return;
    }

    if (events_selected(e)) {
        ed_deselect_events(e);
        return;
    }

    ed_deselect_events(e);
    (void)v;
    (void)scale;
    e->event_free = mouse.alt;      /* Alt places without snapping, as everywhere else. */
    e->event_connect = mouse.shift; /* Shift + click connects the point to the nearest one. */
    e->marquee_start = mouse.pos;

    /* Pedal events are ranges, so a press always draws one. */
    if (ed_pedal_event(e->automation_cc)) {
        ed_checkpoint(e);
        e->drag = 4;
        return;
    }

    /* Discrete events such as program changes never form lines: a press places one point. */
    if (discrete_event(e->automation_cc)) {
        e->event_connect = false;
    }

    /* Otherwise the press waits: moving past a small buffer draws a line, releasing adds a
       single point (connected with Shift); see ed_automation_press. */
    e->drag = 12;
    e->event_press_tick = SDL_GetTicks();
}

/* A quick click may wobble a little before release, so it has a larger buffer than a slow press
   that starts moving. */
void ed_automation_press(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    if (e->drag != 12) {
        return;
    }

    float moved = hypotf(mouse.pos.x - e->marquee_start.x, mouse.pos.y - e->marquee_start.y);
    float buffer = SDL_GetTicks() - e->event_press_tick < 250 ? 8.f : 4.f;

    if (moved > buffer && !discrete_event(e->automation_cc)) {
        ed_checkpoint(e);
        e->drag = 4; /* ed_automation_drag draws the line and places it on release. */
        return;
    }

    if (mouse.left && !mouse.released) {
        return;
    }

    e->drag = 0;
    UiMouse at = mouse;
    at.pos = e->marquee_start;
    at.alt = e->event_free || mouse.alt;
    double beat = ed_snap((e->marquee_start.x - lane.x + v->scroll_x) / scale, .25, at.alt);
    if (e->event_connect) {
        add_connected_point(e, lane, at, beat);
    } else {
        add_cursor_point(e, lane, at, beat);
    }
}

void ed_deselect_events(Editor *e) {
    e->automation_all = false;
    e->event_selection.size = 0;
    e->event_drag.size = 0;
    e->automation_node = -1;
}

void ed_automation_drag(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    if (e->drag != 4) {
        return;
    }

    mouse.alt = mouse.alt || e->event_free; /* Alt at the press or the release disables snapping. */

    if (ed_pedal_event(e->automation_cc)) {
        float y = lane.y + lane.h * .5f;
        ui_line((UiPoint){e->marquee_start.x, y}, (UiPoint){mouse.pos.x, y}, GOLD, 4);
    } else {
        /* The preview shows the points where they will land: snapped unless Alt is held. With
           Shift, a dimmer line shows the start joining its nearest point. */
        UiPoint from = placed_point(e, lane, v, scale, e->marquee_start, mouse.alt);
        UiPoint to = placed_point(e, lane, v, scale, mouse.pos, mouse.alt);
        double start_beat =
            ed_snap((e->marquee_start.x - lane.x + v->scroll_x) / scale, .25, mouse.alt);
        int nearest = e->event_connect ? ed_closest_node(e, start_beat) : -1;

        if (nearest >= 0) {
            UiPoint joined = ed_automation_point(e, e->project.automation[nearest], lane, v, scale);
            ui_line(joined, from, (GOLD & 0xffffffu) | 0x88000000u, 2);
        }

        ui_line(from, to, GOLD, 2);
        ui_circle(from, 4, GOLD);
        ui_circle(to, 4, GOLD);
    }

    if (!mouse.released) {
        return;
    }

    double start = ed_snap((e->marquee_start.x - lane.x + v->scroll_x) / scale, .25, mouse.alt);
    double end = ed_snap((mouse.pos.x - lane.x + v->scroll_x) / scale, .25, mouse.alt);
    e->drag_key = ed_curve_anchor(e, start);
    DmAutomation a = ed_curve_node(e, start, 0, e->automation_cc, 1, e->drag_key);

    if (e->automation_cc < 0) {
        e->bend_range = 127;
        a.curve = INT32_MAX;
    }

    int first = ed_lane_value(e, lane, e->marquee_start.y, a, mouse.alt);
    int last = ed_lane_value(e, lane, mouse.pos.y, a, mouse.alt);
    bool moved =
        fabs(mouse.pos.x - e->marquee_start.x) >= 3 || fabs(mouse.pos.y - e->marquee_start.y) >= 3;

    if (e->event_connect && moved && !ed_pedal_event(e->automation_cc) &&
        !discrete_event(e->automation_cc) && connected_line(e, lane, mouse, start, end)) {
        ed_sort_automation(e);
    } else if (ed_pedal_event(e->automation_cc)) {
        if (fabs(end - start) < 1.0 / DM_PPQ) {
            end = start + .25;
        }

        ed_automation_line(e, start, end, 127, 127, e->automation_cc);
    } else if (e->automation_cc == 5 && fabs(mouse.pos.x - e->marquee_start.x) < 3 &&
               fabs(mouse.pos.y - e->marquee_start.y) < 3) {
        ed_automation_line(e, start, start + .25, first, first, 5);
    } else if (discrete_event(e->automation_cc) || (fabs(mouse.pos.x - e->marquee_start.x) < 3 &&
                                                    fabs(mouse.pos.y - e->marquee_start.y) < 3)) {
        DmAutomation point = ed_curve_node(e, start, first, e->automation_cc, 0, e->drag_key);

        if (point.type == MIDI_PITCH_BEND) {
            point = lane_edited(e, point, lane, e->marquee_start.y, mouse.alt);
        }

        ed_set_event_value(&point, ed_lane_value(e, lane, e->marquee_start.y, point, mouse.alt));
        dm_automation_add(&e->project, point);
        e->last_automation = point;
        e->last_automation_valid = true;
    } else {
        ed_automation_line(e, start, end, first, last, e->automation_cc);
    }

    e->drag = 0;
    ed_commit(e);
}

void ed_event_box_select(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    UiPoint start = e->event_marquee_start;
    UiRect box = {fminf(start.x, mouse.pos.x), fminf(start.y, mouse.pos.y),
                  fabsf(start.x - mouse.pos.x), fabsf(start.y - mouse.pos.y)};
    e->event_selection.size = 0;
    size_t bytes = e->project.automation_count * sizeof(DmAutomation);

    if (dm_reserve((void **)&e->event_selection.data, &e->event_selection.capacity, bytes, 1)) {
        for (size_t i = 0; i < e->project.automation_count; ++i) {
            DmAutomation a = e->project.automation[i];

            if (!ed_automation_matches(e, a) || a.curve < 0) {
                continue;
            }

            UiPoint at = ed_automation_point(e, a, lane, v, scale);

            if (!ed_inside(at, box)) {
                continue;
            }

            memcpy(e->event_selection.data + e->event_selection.size, &a, sizeof(a));
            e->event_selection.size += sizeof(a);
        }

        qsort(e->event_selection.data, e->event_selection.size / sizeof(DmAutomation),
              sizeof(DmAutomation), event_identity);
    }

    ui_rect(box, (e->accent & 0xffffffu) | 0x22000000u, 0);
    ui_border(box, e->accent, 0, 1);

    if (mouse.released || !mouse.left) {
        e->event_marquee = false;
    }
}

void ed_pitch_text(char *out, size_t cap, float pitch) {
    static const char *names[] = {DM_TEXT_C,       DM_TEXT_C_SHARP, DM_TEXT_D,
                                  DM_TEXT_D_SHARP, DM_TEXT_E,       DM_TEXT_F,
                                  DM_TEXT_F_SHARP, DM_TEXT_G,       DM_TEXT_G_SHARP,
                                  DM_TEXT_A,       DM_TEXT_A_SHARP, DM_TEXT_B};
    int key = (int)ed_clampf(roundf(pitch), 0, 127);
    snprintf(out, cap, "%s%d", names[key % 12], key / 12 - 1);
}

static void event_placement_tip(Editor *e, UiRect lane, UiMouse mouse, bool hovered) {
    if (!mouse.left || e->event_marquee || e->tool == 1 || !ed_inside(mouse.pos, lane) ||
        !hovered) {
        return;
    }

    char text[64];

    if (e->automation_cc == -2 && !e->automation_raw) {
        float pitch =
            e->automation_center + (lane.y + lane.h * .5f - mouse.pos.y) / ed_lane_step(lane);

        if (e->drag == 5 && e->automation_node >= 0) {
            pitch = ed_automation_pitch(e, e->project.automation[e->automation_node]);
        }

        ed_pitch_text(text, sizeof(text), pitch);
    } else {
        DmAutomation a = ed_curve_node(e, 0, 0, e->automation_cc, 0, ed_nearest_note(e, 0));
        int value = ed_lane_value(e, lane, mouse.pos.y, a, mouse.alt);

        if (e->drag == 5 && e->automation_node >= 0) {
            value = ed_event_value(e->project.automation[e->automation_node]);
        }

        /* Program changes name the instrument instead of its number. */
        if (e->automation_cc == -1) {
            snprintf(text, sizeof(text), "%s", dm_program_name(value));
        } else {
            snprintf(text, sizeof(text), "%d", value);
        }
    }

    ui_placement_tip(text);
}

void ed_automation_input(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse, bool hovered,
                         int hit) {
    if ((hovered && hit >= 0) || e->drag == 5 || e->curve_drag) {
        ui_gesture_cursor(true);
    }

    bool start =
        hovered && ed_inside(mouse.pos, lane) && mouse.clicked && (mouse.ctrl || e->tool == 1);

    if (start) {
        ed_focus_edit(e, FOCUS_EVENTS);
        e->automation_node = -1;
        e->automation_all = false;
        e->event_selection.size = 0;
        e->event_marquee = true;
        e->event_marquee_start = mouse.pos;
        e->drag = 0;
        e->curve_drag = false;
    }

    if (e->event_marquee) {
        ed_event_box_select(e, lane, v, scale, mouse);
        return;
    }

    bool pedal = ed_pedal_event(e->automation_cc);
    bool handle = !pedal && ed_curve_handles(e, lane, v, scale, mouse, false, hovered);

    if (!handle && hovered && ed_inside(mouse.pos, lane) &&
        (!pedal || hit >= 0 || fabsf(mouse.pos.y - (lane.y + lane.h * .5f)) < 12) &&
        mouse.clicked) {
        ed_automation_lane_click(e, lane, v, scale, mouse, hit);
    }

    /* Right-dragging erases points, like notes in the piano roll and playlist placements. */
    if (hovered && ed_inside(mouse.pos, lane) && mouse.right && !mouse.shift && hit >= 0 &&
        !e->drag && !e->curve_drag) {
        ed_focus_edit(e, FOCUS_EVENTS);
        ed_erase_node(e, hit);
    }

    point_menu(e, hovered && ed_inside(mouse.pos, lane) && mouse.right_clicked && mouse.shift ? hit
                                                                                              : -1);
    ed_automation_node_drag(e, lane, v, scale, mouse);

    if (!pedal) {
        event_placement_tip(e, lane, mouse, hovered);
    }

    ed_automation_press(e, lane, v, scale, mouse);
    ed_automation_drag(e, lane, v, scale, mouse);
}

static bool curve_displayed(Editor *e, DmAutomation a) {
    if (ed_automation_matches(e, a)) {
        return true;
    }

    if (!a.curve || a.pattern != e->selected_pattern || a.port != ed_note_port(e)) {
        return false;
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation node = e->project.automation[i];

        if (abs(node.curve) == abs(a.curve) && ed_automation_matches(e, node)) {
            return true;
        }
    }

    return false;
}

static void remove_node(Editor *e, int index) {
    DmAutomation removed = e->project.automation[index];
    memmove(e->project.automation + index, e->project.automation + index + 1,
            (--e->project.automation_count - index) * sizeof(DmAutomation));
    bool remains = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern == removed.pattern && a.curve == removed.curve && a.type == removed.type) {
            remains = true;
        }
    }

    if (removed.curve > 0 && !remains) {
        for (size_t i = e->project.automation_count; i > 0; --i) {
            DmAutomation a = e->project.automation[i - 1];

            if (a.pattern != removed.pattern || abs(a.curve) != removed.curve) {
                continue;
            }

            memmove(e->project.automation + i - 1, e->project.automation + i,
                    (e->project.automation_count - i) * sizeof(a));
            --e->project.automation_count;
        }
    } else if (removed.curve > 0) {
        ed_curve_restore_position(e, removed.curve);
    }

    e->automation_node = -1;
    e->event_selection.size = 0;
    ed_sort_automation(e);
}

void ed_remove_selected_node(Editor *e) {
    int index = e->automation_node;

    if (index < 0 || (size_t)index >= e->project.automation_count) {
        return;
    }

    ed_checkpoint(e);
    remove_node(e, index);
    ed_commit(e);
}

/* One undo step covers a whole right-drag erase gesture. */
void ed_erase_node(Editor *e, int index) {
    if (index < 0 || (size_t)index >= e->project.automation_count) {
        return;
    }

    if (!e->erase_active) {
        ed_checkpoint(e);
    }

    e->erase_active = true;
    remove_node(e, index);
    ed_commit(e);
}

void ed_automation_clear(Editor *e) {
    size_t count = e->project.automation_count;
    bool *remove = calloc(count + 1, sizeof(*remove));

    if (!remove) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_AUTOMATION_EDIT);
        return;
    }

    bool any = false;

    for (size_t i = 0; i < count; ++i) {
        remove[i] = curve_displayed(e, e->project.automation[i]);
        any |= remove[i];
    }

    if (!any) {
        free(remove);
        return;
    }

    ed_checkpoint(e);

    for (size_t i = count; i > 0; --i) {
        if (!remove[i - 1]) {
            continue;
        }

        memmove(e->project.automation + i - 1, e->project.automation + i,
                (e->project.automation_count - i) * sizeof(DmAutomation));
        --e->project.automation_count;
    }

    free(remove);
    e->automation_node = -1;
    e->automation_all = false;
    e->last_automation_valid = false;
    ed_commit(e);
}

static size_t selected_curve(Editor *e, int curve, double *first, double *last) {
    size_t count = 0;
    *first = HUGE_VAL;
    *last = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.curve == curve && ed_event_selected(e, i)) {
            *first = fmin(*first, a.beat);
            *last = fmax(*last, a.beat);
            ++count;
        }
    }

    return count;
}

static bool copy_event_dependencies(Editor *e, DmProject *to, DmAutomation node, double origin) {
    double first, last;
    size_t count = selected_curve(e, node.curve, &first, &last);

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != node.pattern || abs(a.curve) != node.curve ||
            (a.curve >= 0 && (a.type != MIDI_CONTROL || ed_automation_matches(e, a)))) {
            continue;
        }

        if (count == 1 && a.curve < 0) {
            continue;
        }

        a.beat = (a.curve < 0 ? last : first) - origin;
        a.pattern = 0;

        if (count == 1) {
            a.curve = 0;
        }

        if (!dm_automation_add(to, a)) {
            return false;
        }
    }

    return true;
}

static DmAutomation copied_connection(Editor *e, size_t index) {
    DmAutomation a = e->project.automation[index];
    int right = next_connection(e, (int)index, false);

    if (right < 0 || !ed_event_selected(e, (size_t)right)) {
        a.bend = 0;
        a.ramp = false;
    }

    return a;
}

bool ed_copy_events(Editor *e, DmProject *to) {
    double first = HUGE_VAL;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (ed_event_selected(e, i)) {
            first = fmin(first, e->project.automation[i].beat);
        }
    }

    if (!isfinite(first) || dm_pattern_add(to, DM_TEXT_EVENTS) < 0) {
        return false;
    }

    snprintf(to->name, sizeof(to->name), DM_TEXT_EVENTS);
    to->patterns[0].event = e->automation_cc;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (!ed_event_selected(e, i)) {
            continue;
        }

        DmAutomation a = copied_connection(e, i);

        if (a.curve > 0) {
            double begin, end;
            size_t count = selected_curve(e, a.curve, &begin, &end);

            if (a.beat == begin && !copy_event_dependencies(e, to, a, first)) {
                return false;
            }

            if (count == 1) {
                a.curve = 0;
            }
        } else if (a.type == MIDI_PITCH_BEND) {
            PitchState state = ed_pitch_state(e, a.beat);
            const int cc[] = {101, 100, 6, 38, 101, 100};
            const int value[] = {0, 0, state.range, state.cents, state.msb, state.lsb};

            for (int j = 0; j < 6; ++j) {
                DmAutomation setup = {
                    a.beat - first, MIDI_CONTROL, a.channel, cc[j], value[j], 0, 0, 0, 0,
                    false,          a.port};

                if (!dm_automation_add(to, setup)) {
                    return false;
                }
            }
        }

        a.pattern = 0;
        a.beat -= first;

        if (!dm_automation_add(to, a)) {
            return false;
        }
    }

    return true;
}

static void paste_discrete_pitch(Editor *e, const DmProject *from, DmAutomation *a,
                                 double source_beat) {
    PitchState source = ed_pattern_pitch_state(from, 0, source_beat, a->port);
    double bend = (ed_event_value(*a) - MIDI_BEND_CENTER) * (source.range + source.cents * .01) /
                  MIDI_BEND_CENTER;
    float pitch = ed_nearest_note(e, 0) + (float)bend;
    ed_expand_pitch_range(e, *a, pitch);
    float range = ed_automation_range(e, *a);
    a->bend *= (source.range + source.cents * .01) / range;
    ed_set_event_value(a, (int)ed_clampf(MIDI_BEND_CENTER + (float)bend * MIDI_BEND_CENTER / range,
                                         0, MIDI_BEND_MAX));
}

static double pasted_section_start(const DmProject *from, int curve) {
    double first = HUGE_VAL;

    for (size_t i = 0; i < from->automation_count; ++i) {
        DmAutomation a = from->automation[i];

        if (a.curve == -curve) {
            first = fmin(first, a.beat);
        }
    }

    return first;
}

static bool later_selector(const DmProject *from, size_t index) {
    DmAutomation a = from->automation[index];

    for (size_t i = index + 1; i < from->automation_count; ++i) {
        DmAutomation b = from->automation[i];

        if (b.curve == a.curve && b.type == MIDI_CONTROL && b.a == a.a) {
            return true;
        }
    }

    return false;
}

static DmAutomation *prepare_pasted_events(Editor *e, const DmProject *from, double at) {
    size_t bytes = from->automation_count * sizeof(DmAutomation);
    DmAutomation *items = malloc(bytes + 1);

    if (!items) {
        return NULL;
    }

    memcpy(items, from->automation, bytes);

    for (size_t i = 0; i < from->automation_count; ++i) {
        DmAutomation *a = &items[i];

        if (a->curve >= 0) {
            continue;
        }

        double start = fmax(0, at + pasted_section_start(from, a->curve) - 1e-8);
        PitchState prior = ed_pitch_state(e, start);

        if (a->type == MIDI_PITCH_BEND) {
            ed_set_event_value(a, ed_value_before(e, start, -2));
        } else if (a->type == MIDI_CONTROL) {
            switch (a->a) {
            case MIDI_CC_DATA:
                a->b = prior.range;
                break;
            case MIDI_CC_DATA_LSB:
                a->b = prior.cents;
                break;
            case MIDI_CC_RPN_MSB:
                a->b = later_selector(from, i) ? 0 : prior.msb;
                break;
            case MIDI_CC_RPN_LSB:
                a->b = later_selector(from, i) ? 0 : prior.lsb;
                break;
            default:
                a->b = ed_value_before(e, start, a->a);
                break;
            }
        }
    }

    return items;
}

static bool paste_one_event(Editor *e, const DmProject *from, DmAutomation a, double at,
                            int offset) {
    if (e->automation_cc == -2 && a.type == MIDI_CONTROL && !a.curve) {
        return true;
    }

    double source_beat = a.beat;

    if (abs(a.curve) > INT32_MAX - offset - 1) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_AUTOMATION_SECTION);
        return false;
    }

    if (a.curve) {
        a.curve = (a.curve < 0 ? -1 : 1) * (abs(a.curve) + offset);
    }

    a.beat += at;
    a.pattern = e->selected_pattern;
    a.channel = e->pattern_channel;

    if (a.type == MIDI_PITCH_BEND && !a.curve) {
        paste_discrete_pitch(e, from, &a, source_beat);
    }

    a.port = (uint8_t)ed_note_port(e);

    if (!dm_automation_add(&e->project, a)) {
        return false;
    }

    if (a.curve >= 0 && ed_automation_matches(e, a)) {
        memcpy(e->event_selection.data + e->event_selection.size, &a, sizeof(a));
        e->event_selection.size += sizeof(a);
    }

    return true;
}

void ed_paste_events(Editor *e, const DmProject *from, double at) {
    size_t bytes = from->automation_count * sizeof(DmAutomation);

    if (!ed_pattern(e) ||
        !dm_reserve((void **)&e->event_selection.data, &e->event_selection.capacity, bytes, 1)) {
        return;
    }

    DmAutomation *items = prepare_pasted_events(e, from, at);

    if (!items) {
        return;
    }

    e->automation_cc = from->patterns[0].event;

    if (ed_pattern(e)->kind) {
        ed_pattern(e)->event = e->automation_cc;
    }

    e->automation_node = -1;
    e->automation_all = false;
    e->event_selection.size = 0;
    int offset = ed_curve_id(e) - 1;

    for (size_t i = 0; i < from->automation_count; ++i) {
        if (!paste_one_event(e, from, items[i], at, offset)) {
            break;
        }
    }

    free(items);
    qsort(e->event_selection.data, e->event_selection.size / sizeof(DmAutomation),
          sizeof(DmAutomation), event_identity);
    dm_pattern_fit(&e->project, ed_pattern(e));
}

void ed_duplicate_events(Editor *e) {
    if (!e->active || !ed_pattern(e) || !ed_point_editing(e)) {
        return;
    }

    double first = HUGE_VAL, end = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (ed_event_selected(e, i)) {
            first = fmin(first, e->project.automation[i].beat);
            DmAutomation a = e->project.automation[i];
            end = fmax(end, a.beat + (a.curve ? 0 : 1.0 / DM_PPQ));
        }
    }

    DmProject copy;
    dm_project_init(&copy);

    if (ed_copy_events(e, &copy)) {
        double span = fmax(1, ceil(end - 1e-9) - floor(first + 1e-9));
        double bar = dm_meter(&e->project, first).bar;

        if (span >= bar) {
            span = ceil(span / bar) * bar;
        }

        ed_checkpoint(e);
        ed_paste_events(e, &copy, first + span);
        ed_commit(e);
    }

    dm_project_free(&copy);
}

void ed_delete_events(Editor *e) {
    size_t count = e->project.automation_count;
    bool *remove = calloc(count + 1, sizeof(*remove));

    if (!remove) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        remove[i] = ed_event_selected(e, i);
    }

    for (size_t i = 0; i < count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) || !a.curve ||
            remove[i]) {
            continue;
        }

        bool remains = false, selected = false;

        for (size_t j = 0; j < count; ++j) {
            DmAutomation b = e->project.automation[j];

            if (b.pattern == a.pattern && b.curve == abs(a.curve) && ed_automation_matches(e, b)) {
                remains |= !remove[j];
                selected |= remove[j];
            }
        }

        remove[i] = selected && !remains;
    }

    ed_checkpoint(e);

    for (size_t i = count; i > 0; --i) {
        if (remove[i - 1]) {
            memmove(e->project.automation + i - 1, e->project.automation + i,
                    (e->project.automation_count - i) * sizeof(DmAutomation));
            --e->project.automation_count;
        }
    }

    free(remove);
    e->event_selection.size = 0;
    e->automation_node = -1;
    e->last_automation_valid = false;
    ed_commit(e);
}
