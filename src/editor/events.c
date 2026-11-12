#include "editor/internal.h"
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
static size_t selected_curve(Editor *e, const bool *selected, int curve, double *first,
                             double *last);
static bool copy_event_dependencies(Editor *e, DmProject *to, const bool *selected,
                                    DmAutomation node, double origin);
static DmAutomation copied_connection(Editor *e, const bool *selected, size_t index);
static bool copy_event_points(Editor *e, DmProject *to, const bool *selected, double origin);
static void paste_discrete_pitch(Editor *e, const DmProject *from, DmAutomation *a,
                                 double source_beat, int source_port);
static double pasted_section_start(const DmProject *from, int curve);
static bool later_selector(const DmProject *from, size_t index);
static DmAutomation *prepare_pasted_events(Editor *e, const DmProject *from, double at);
static bool paste_one_event(Editor *e, const DmProject *from, DmAutomation a, double at, int offset,
                            bool all, bool note_bends);
static bool paste_events(Editor *e, const DmProject *from, double at, bool all, bool note_bends);

/* The event-type value (as in automation_cc) that an authored point belongs to, or INT32_MIN
   for types the event selector does not offer. */
static int event_type_of(DmAutomation a) {
    switch (a.type) {
    case MIDI_PITCH_BEND:
        return -2;
    case MIDI_PROGRAM:
        return -1;
    case MIDI_CHANNEL_PRESSURE:
        return -4;
    case MIDI_KEY_PRESSURE:
        return -5;
    case MIDI_CONTROL:
        return a.a;
    default:
        return INT32_MIN;
    }
}

/* Marks which of `types` hold data on the current pattern and note-color route, in one pass. */
static void ed_event_types_used(Editor *e, const int *types, int count, bool *used) {
    DmPattern *p = ed_pattern(e);
    int port = ed_note_port(e);
    bool edited = false;
    bool populated[133] = {false}; /* Event selector values -5 through 127. */

    for (int t = 0; t < count; ++t) {
        used[t] = p && p->kind && p->mode && p->event == types[t]; /* An LFO pattern's type. */
    }

    if (!p) {
        return;
    }

    /* Every note has a velocity, so only edited velocities count as data. */
    for (size_t i = 0; i < p->count && !edited; ++i) {
        edited = p->notes[i].port == port && p->notes[i].velocity != ED_DEFAULT_VELOCITY;
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.port != port || a.curve < 0) {
            continue;
        }

        int type = event_type_of(a);

        if (type >= -5 && type <= 127) {
            populated[type + 5] = true;
        }
    }

    for (int t = 0; t < count; ++t) {
        used[t] |= types[t] == DM_EVENT_VELOCITY && edited;

        if (types[t] >= -5 && types[t] <= 127) {
            used[t] |= populated[types[t] + 5];
        }
    }
}

bool ed_event_type_used(Editor *e, int cc) {
    bool used;
    ed_event_types_used(e, &cc, 1, &used);
    return used;
}

bool ed_automation_matches(Editor *e, DmAutomation au) {
    if (au.pattern != e->selected_pattern || au.port != ed_note_port(e)) {
        return false;
    }

    if (e->automation_cc == DM_EVENT_BEND) {
        return au.type == MIDI_PITCH_BEND;
    }

    if (e->automation_cc == DM_EVENT_PROGRAM) {
        return au.type == MIDI_PROGRAM;
    }

    if (e->automation_cc == DM_EVENT_CHANNEL_PRESSURE) {
        return au.type == MIDI_CHANNEL_PRESSURE;
    }

    if (e->automation_cc == DM_EVENT_KEY_PRESSURE) {
        return au.type == MIDI_KEY_PRESSURE;
    }

    return au.type == MIDI_CONTROL && au.a == e->automation_cc;
}

bool ed_same_event(DmAutomation a, DmAutomation b) {
    return a.pattern == b.pattern && a.beat == b.beat && a.type == b.type &&
           a.channel == b.channel && a.a == b.a && a.b == b.b && a.curve == b.curve &&
           a.anchor == b.anchor && a.bend == b.bend && a.ramp == b.ramp && a.port == b.port &&
           a.hold == b.hold && a.fixed_anchor == b.fixed_anchor && a.bend_note == b.bend_note &&
           a.bend_root == b.bend_root;
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

    if (a->ramp != b->ramp) {
        return (int)a->ramp - b->ramp;
    }

    if (a->hold != b->hold) {
        return (int)a->hold - b->hold;
    }

    if (a->fixed_anchor != b->fixed_anchor) {
        return (int)a->fixed_anchor - b->fixed_anchor;
    }

    if (a->bend_root != b->bend_root) {
        return (int)a->bend_root - b->bend_root;
    }

    return (a->bend_note > b->bend_note) - (a->bend_note < b->bend_note);
}

static bool event_selection_contains(const DmBytes *selection, DmAutomation a) {
    return selection->size &&
           bsearch(&a, selection->data, selection->size / sizeof(a), sizeof(a), event_identity);
}

bool ed_event_selected(Editor *e, size_t index) {
    DmAutomation a = e->project.automation[index];

    if (!ed_automation_matches(e, a) || a.curve < 0) {
        return false;
    }

    if (e->automation_all || e->automation_node == (int)index) {
        return true;
    }

    return event_selection_contains(&e->event_selection, a);
}

static const char *automation_help(int controller) {
    switch (controller) {
    case DM_EVENT_VELOCITY:
        return DM_TEXT_VELOCITY_BELONGS_EACH;
    case DM_EVENT_BEND:
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
    case DM_EVENT_VELOCITY:
        return DM_TEXT_VELOCITY;
    case DM_EVENT_PROGRAM:
        return DM_TEXT_PROGRAM_CHANGE;
    case DM_EVENT_CHANNEL_PRESSURE:
        return DM_TEXT_CHANNEL_PRESSURE;
    case DM_EVENT_KEY_PRESSURE:
        return DM_TEXT_POLY_PRESSURE;
    case DM_EVENT_BEND:
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
    return cc == DM_EVENT_PROGRAM;
}

bool ed_point_only_event(int cc) {
    return ed_pedal_event(cc) || cc == 5 || cc == DM_EVENT_PROGRAM ||
           cc == DM_EVENT_CHANNEL_PRESSURE || cc == DM_EVENT_KEY_PRESSURE;
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
    e->marquee = e->event_marquee = false;
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

    /* Velocity belongs to note patterns only. */
    const DmPattern *pattern = ed_pattern(e);
    int values[TYPES], count = 0, current = -1;

    for (int i = 0; i < TYPES; ++i) {
        if (all_values[i] != -3 || (pattern && !pattern->kind)) {
            current = all_values[i] == e->automation_cc ? count : current;
            values[count++] = i;
        }
    }

    const char *preview = automation_name(e->automation_cc);
    ui_width((pattern && pattern->kind && pattern->mode) || ui_available().x < 600 ? 130 : 220);

    bool populated = ed_event_type_used(e, e->automation_cc);
    UiPoint at = ui_cursor();

    if (ui_combo("##Event type", preview)) {
        int shown[TYPES];
        bool used[TYPES];

        for (int i = 0; i < count; ++i) {
            shown[i] = all_values[values[i]];
        }

        ed_event_types_used(e, shown, count, used);

        for (int i = 0; i < count; ++i) {
            UiPoint row = ui_cursor();

            if (used[i]) {
                ui_rect((UiRect){row.x, row.y, ui_available().x,
                                 ui_text_height(all_labels[values[i]], 0)},
                        (e->accent & 0xffffffu) | 0x40000000u, 0);
            }

            if (ui_select(all_labels[values[i]], i == current, 0, 0)) {
                choose_event_type(e, all_values[values[i]]);
            }
        }

        ui_combo_end();
    }

    if (populated) {
        ui_rect((UiRect){at.x, at.y, 3, ui_frame_height()}, e->accent, 0);
    }

    ui_tip(DM_TEXT_CHOOSE_EVENT_TYPE);
    ui_same();
    ui_info(automation_help(e->automation_cc));
}

static int lane_maximum(Editor *e) {
    return e->automation_cc == DM_EVENT_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;
}

int ed_value_step(Editor *e, float height) {
    int cc = e->automation_cc;

    if (cc == DM_EVENT_VELOCITY || ed_pedal_event(cc) || discrete_event(cc) ||
        (cc == DM_EVENT_BEND && !ed_automation_raw(e))) {
        return 0;
    }

    /* Rows land between 1/sqrt(2) and sqrt(2) times the pitch row height. */
    int maximum = lane_maximum(e), step = 1;
    float per_value = fmaxf(1, height - 8) / maximum;

    while (step < maximum && step * per_value < ED_LANE_STEP * .7071f) {
        step *= 2;
    }

    return step;
}

int ed_value_snap(Editor *e, int value, int step) {
    if (step <= 0) {
        return value;
    }

    long snapped = lround((double)value / step) * step;
    return (int)(snapped > lane_maximum(e) ? lane_maximum(e) : snapped < 0 ? 0 : snapped);
}

/* Drawing pitch bends asks for each point's bend range and anchor note, and each lookup scans the
   whole project. Between begin and end, lookups read a copy of the pattern's RPN and data entry
   controllers gathered once, and the anchor note is found once. The automation must not change
   in between; edits that only touch a point's curve shape (bend, ramp, hold) are fine. */
void ed_pitch_cache_begin(Editor *e) {
    e->pitch_cache = true;
    e->pitch_cache_ready = false;
    e->pitch_cache_anchored = false;
}

void ed_pitch_cache_end(Editor *e) {
    e->pitch_cache = false;
}

static bool range_controller(DmAutomation a) {
    return a.type == MIDI_CONTROL && (a.a == MIDI_CC_RPN_MSB || a.a == MIDI_CC_RPN_LSB ||
                                      a.a == MIDI_CC_DATA || a.a == MIDI_CC_DATA_LSB);
}

/* The pattern's range controllers in project order, or NULL to scan the whole project. */
static const DmAutomation *range_controllers(Editor *e, int pattern, size_t *count) {
    if (!e->pitch_cache) {
        return NULL;
    }

    if (!e->pitch_cache_ready || e->pitch_cache_pattern != pattern) {
        size_t n = 0;
        e->pitch_cache_ready = false;

        for (size_t i = 0; i < e->project.automation_count; ++i) {
            n += e->project.automation[i].pattern == pattern &&
                 range_controller(e->project.automation[i]);
        }

        if (!dm_reserve((void **)&e->pitch_controls.data, &e->pitch_controls.capacity, n + 1,
                        sizeof(DmAutomation))) {
            return NULL;
        }

        DmAutomation *items = (DmAutomation *)e->pitch_controls.data;
        n = 0;

        for (size_t i = 0; i < e->project.automation_count; ++i) {
            DmAutomation a = e->project.automation[i];

            if (a.pattern == pattern && range_controller(a)) {
                items[n++] = a;
            }
        }

        e->pitch_controls.size = n * sizeof(DmAutomation);
        e->pitch_cache_pattern = pattern;
        e->pitch_cache_ready = true;
    }

    *count = e->pitch_controls.size / sizeof(DmAutomation);
    return (const DmAutomation *)e->pitch_controls.data;
}

/* The note a discrete pitch bend is drawn around. */
static int bend_anchor(Editor *e) {
    if (!e->pitch_cache) {
        return ed_nearest_note(e, 0);
    }

    if (!e->pitch_cache_anchored) {
        e->pitch_cache_anchor = ed_nearest_note(e, 0);
        e->pitch_cache_anchored = true;
    }

    return e->pitch_cache_anchor;
}

float ed_automation_range(Editor *e, DmAutomation a) {
    size_t count = e->project.automation_count;
    const DmAutomation *items = range_controllers(e, a.pattern, &count);
    items = items ? items : e->project.automation;
    return (float)dm_pitch_range(items, count, a);
}

int ed_pitch_anchor(Editor *e, DmAutomation a) {
    return a.curve || a.fixed_anchor ? a.anchor : bend_anchor(e);
}

void ed_pitch_edit_units(Editor *e, DmAutomation *point, bool raw) {
    if (point->type != MIDI_PITCH_BEND || point->curve < 0 || point->pattern < 0) {
        return;
    }

    if (!point->curve) {
        if (raw && !point->fixed_anchor) {
            return;
        }

        point->anchor = ed_pitch_anchor(e, *point);
        point->fixed_anchor = true;
        point->bend_root = raw ? DM_BEND_ROOT_MIDI : DM_BEND_ROOT_PITCH;
    } else if (!raw) {
        if (point->bend_root) {
            point->bend_root = DM_BEND_ROOT_PITCH;
        }
    } else {
        /* Translating a section would change a raw value entered anywhere in that section. */
        for (size_t i = 0; i < e->project.automation_count; ++i) {
            DmAutomation *root = &e->project.automation[i];

            if (root->bend_root && root->pattern == point->pattern && root->port == point->port &&
                root->curve == point->curve) {
                root->bend_root = DM_BEND_ROOT_MIDI;
                break;
            }
        }
    }
}

float ed_automation_pitch(Editor *e, DmAutomation a) {
    float range = ed_automation_range(e, a);
    int anchor = ed_pitch_anchor(e, a);
    return anchor + (ed_event_value(a) - MIDI_BEND_CENTER) * range / MIDI_BEND_CENTER;
}

UiPoint ed_automation_point(Editor *e, DmAutomation a, UiRect lane, View *v, float scale) {
    float value = ed_event_value(a),
          maximum = a.type == MIDI_PITCH_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;
    float y = lane.y + lane.h - 8 - value / maximum * (lane.h - 8);

    if (a.type == MIDI_PITCH_BEND && !ed_automation_raw(e)) {
        y = lane.y + lane.h * .5f -
            (ed_automation_pitch(e, a) - e->automation_center) * ED_LANE_STEP;
    }

    if (ed_pedal_event(e->automation_cc)) {
        y = lane.y + lane.h * .5f;
    }

    return (UiPoint){lane.x + (float)a.beat * scale - v->scroll_x, y};
}

float ed_curve_units(Editor *e, DmAutomation a, UiRect lane, bool overlay) {
    if (a.type == MIDI_PITCH_BEND && (overlay || !ed_automation_raw(e))) {
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

typedef struct {
    UiPoint first, last;
    float low, high, column;
    unsigned count;
    bool hint;
    uint32_t color;
} DenseLaneColumn;

/* Keep one matching predecessor for a segment crossing the viewport's left edge. */
size_t ed_automation_visible_first(Editor *e, double beat, bool overlay) {
    size_t low = 0, high = e->project.automation_count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (e->project.automation[middle].beat < beat) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    size_t first = low;

    while (first) {
        DmAutomation a = e->project.automation[--first];
        bool visible = overlay ? a.pattern == e->selected_pattern && a.port == ed_note_port(e) &&
                                     a.type == MIDI_PITCH_BEND
                               : ed_automation_matches(e, a);

        if (visible && a.curve >= 0) {
            return first;
        }
    }

    return low;
}

/* Discrete samples within one display pixel share an envelope. Keep hit testing on every
   original point; only the overlapping drawing is batched. Authored curves and selections
   keep their individual markers. */
static void dense_lane_flush(DenseLaneColumn *column, bool outline) {
    if (!column->count) {
        return;
    }

    if (outline && column->count > 2 && !column->hint) {
        /* At this density step handles overlap the point markers. Draw their hints near the
           pointer, retaining every original handle for hit testing below. */
        column->count = 0;
        column->hint = false;
        return;
    }

    if (outline) {
        if (column->count <= 2) {
            ui_circle_outline(column->first, 5, column->color);

            if (column->count == 2) {
                ui_circle_outline(column->last, 5, column->color);
            }
        } else {
            ui_circle_outline((UiPoint){column->first.x, column->low}, 5, column->color);

            if (column->high != column->low) {
                ui_circle_outline((UiPoint){column->first.x, column->high}, 5, column->color);
            }
        }
    } else if (column->count <= 2) {
        ui_circle(column->first, 5, column->color);

        if (column->count == 2) {
            ui_line(column->first, (UiPoint){column->last.x, column->first.y}, column->color, 1.5f);
            ui_line((UiPoint){column->last.x, column->first.y}, column->last, column->color, 1.5f);
            ui_circle(column->last, 5, column->color);
        }
    } else {
        UiPoint low = {column->first.x, column->low}, high = {column->first.x, column->high};
        ui_line(low, high, column->color, 1.5f);
        ui_circle(low, 5, column->color);

        if (column->high != column->low) {
            ui_circle(high, 5, column->color);
        }
    }

    column->count = 0;
    column->hint = false;
}

int ed_automation_plot(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    if (ed_graph_resolution(e)) {
        return ed_graph_plot(e, lane, v, scale, mouse);
    }

    UiPoint previous = {0};
    bool has = false;
    int hit = -1;
    DmAutomation before = {0};
    DenseLaneColumn dense = {.color = ed_point_color(e)};
    ed_pitch_cache_begin(e);
    size_t first = ed_automation_visible_first(e, (v->scroll_x - 8) / scale, false);

    for (size_t i = first; i < e->project.automation_count; ++i) {
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

        bool selected = ed_event_selected(e, i);
        bool raw = !pedal && !a.curve && !a.ramp && !selected;
        float column = floorf(point.x * ui_scale());
        bool joined = raw && dense.count && column == dense.column;

        if (!joined) {
            dense_lane_flush(&dense, false);
        }

        if (has && !joined) {
            if (pedal) {
                if (a.curve > 0 && a.curve == before.curve) {
                    ui_line(previous, point, ed_point_color(e), 4);
                }
            } else {
                ed_automation_connection(previous, point, before, a,
                                         ed_curve_units(e, before, lane, false), ed_point_color(e));
            }
        }

        bool end = pedal && has && a.curve > 0 && a.curve == before.curve;
        uint32_t color = selected ? TEXT : ed_point_color(e);

        if (raw) {
            if (!dense.count) {
                dense.first = point;
                dense.low = dense.high = point.y;
                dense.column = column;
            }

            dense.last = point;
            dense.low = fminf(dense.low, point.y);
            dense.high = fmaxf(dense.high, point.y);
            ++dense.count;
        } else if (end) {
            ui_circle_outline(point, selected ? 7 : 5, color);
        } else {
            ui_circle(point, selected ? 7 : 5, color);
        }

        if (fabsf(mouse.pos.x - point.x) < 9 && fabsf(mouse.pos.y - point.y) < 9) {
            hit = (int)i;
        }

        previous = point;
        has = true;
        before = a;
    }

    dense_lane_flush(&dense, false);
    ed_pitch_cache_end(e);
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

    if (a.type == MIDI_PITCH_BEND && !ed_automation_raw(e)) {
        float pitch =
            ed_clampf(e->automation_center + (lane.y + lane.h * .5f - y) / ED_LANE_STEP, 0, 127);

        if (!free) {
            pitch = roundf(pitch);
        }

        float range = a.curve == INT32_MAX ? e->bend_range : ed_automation_range(e, a);
        return (int)ed_clampf(MIDI_BEND_CENTER +
                                  (pitch - ed_pitch_anchor(e, a)) * MIDI_BEND_CENTER / range,
                              0, MIDI_BEND_MAX);
    }

    float value = (lane.y + lane.h - 8 - y) / (lane.h - 8) * max;
    int result = (int)ed_clampf(value, 0, (float)max), step = free ? 0 : ed_value_step(e, lane.h);
    return step ? ed_value_snap(e, (int)lroundf(value), step) : result;
}

static double next_pitch_range(Editor *e, double beat) {
    DmPitchState state = {2, 0, 127, 127};

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
    int anchor = ed_pitch_anchor(e, node);

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
        DmPitchState previous = ed_pitch_state(e, node.beat);
        ed_curve_range(e, fmax(0, node.beat - .5 / DM_PPQ), 127, 0, previous.msb, previous.lsb, 0);
    }

    return updated;
}

static DmAutomation lane_edited(Editor *e, DmAutomation a, UiRect lane, float y, bool free) {
    if (a.type == MIDI_PITCH_BEND && !ed_automation_raw(e)) {
        float pitch =
            ed_clampf(e->automation_center + (lane.y + lane.h * .5f - y) / ED_LANE_STEP, 0, 127);
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

        if (a.type == MIDI_PITCH_BEND && (e->curve_overlay || !ed_automation_raw(e))) {
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

        DmAutomation *point = &e->project.automation[left];

        if (!e->curve_overlay && ed_automation_raw(e)) {
            ed_pitch_edit_units(e, point, true);
        }

        point->bend = fmax(-MIDI_BEND_MAX, fmin(MIDI_BEND_MAX, bend));
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

    if (hit && !e->drag && !e->event_marquee) {
        ui_gesture_cursor(true);
    }

    if (!hit || ed_gesture_held(e, GESTURE_EXTEND, mouse) ||
        ed_gesture_held(e, GESTURE_MARQUEE, mouse) || e->tool == TOOL_MARQUEE ||
        (overlay && ed_gesture_held(e, GESTURE_BEND_POINT, mouse))) {
        return false;
    }

    if (mouse.middle_clicked) {
        ed_pitch_cache_end(e);
        e->pan_context = CONTEXT_NONE;
        ed_checkpoint(e);
        e->project.automation[index].bend = 0;
        e->project.automation[index].ramp = true;
        e->project.automation[index].hold = false;
        ed_commit(e);
        return true;
    }

    if (mouse.clicked) {
        ed_pitch_cache_end(e);
        ed_focus_edit(e, FOCUS_EVENTS);
        e->curve_drag = true;
        e->drag = DRAG_NONE;
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

    ed_pitch_cache_begin(e);
    DenseLaneColumn dense = {.color = ed_point_color(e)};
    size_t first_visible = ed_automation_visible_first(e, (v->scroll_x - 8) / scale, overlay);

    for (size_t i = first_visible; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (lane.x + (float)a.beat * scale - v->scroll_x > lane.x + lane.w + 8) {
            break;
        }
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

        bool raw = !a.curve && !a.ramp && !ed_event_selected(e, i);
        float column = floorf(mid.x * ui_scale());

        if (!raw || (dense.count && column != dense.column)) {
            dense_lane_flush(&dense, true);
        }

        if (raw) {
            if (!dense.count) {
                dense.first = mid;
                dense.low = dense.high = mid.y;
                dense.column = column;
            }

            dense.low = fminf(dense.low, mid.y);
            dense.high = fmaxf(dense.high, mid.y);
            dense.last = mid;
            dense.hint |= hovered && fabsf(mouse.pos.x - mid.x) < 8;
            ++dense.count;
        } else {
            ui_circle_outline(mid, 5, ed_point_color(e));
        }

        if (curve_handle_action(e, i, lane, mid, mouse, overlay,
                                step ? ed_event_value(a) - linear : a.bend, hovered)) {
            dense_lane_flush(&dense, true);
            return true; /* The action ended the pitch cache before editing. */
        }
    }

    dense_lane_flush(&dense, true);
    ed_pitch_cache_end(e);
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
            candidate.a == edited.a && candidate.b == edited.b &&
            candidate.bend_note == edited.bend_note) {
            e->automation_node = (int)i;
            break;
        }
    }
}

static void event_drag_begin(Editor *e, int hit) {
    bool group = ed_event_selected(e, (size_t)hit);
    e->lone_click = false;

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
        hi = fmin(hi, (pitch || e->automation_cc != DM_EVENT_BEND ? 127 : MIDI_BEND_MAX) - value);
    }

    if (!ed_gesture_held(e, GESTURE_LOCK_VALUE, mouse)) {
        delta = (e->node_y - mouse.pos.y) *
                (pitch ? 1 / ED_LANE_STEP
                       : (e->automation_cc == DM_EVENT_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX) /
                             (lane.h - 8));
        int step = pitch || ed_gesture_held(e, GESTURE_FREE, mouse) ? 0 : ed_value_step(e, lane.h);
        /* Raw values move by whole rows, keeping the points' offsets from them. */
        delta = ed_gesture_held(e, GESTURE_FREE, mouse) && pitch ? delta
                : step                                           ? round(delta / step) * step
                                                                 : round(delta);
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
    bool pitch = e->automation_cc == DM_EVENT_BEND && !ed_automation_raw(e);
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
                ? (int)lround(MIDI_BEND_CENTER + (items[i].pitch + delta - ed_pitch_anchor(e, *a)) *
                                                     MIDI_BEND_CENTER / ed_automation_range(e, *a))
                : ed_event_value(items[i].original) + (int)delta;
        if (delta != 0 && value != ed_event_value(*a)) {
            a->bend_note = 0;
            ed_pitch_edit_units(e, a, !pitch);
        }

        ed_set_event_value(a, value);
    }
}

static bool event_group_drag(Editor *e, UiRect lane, float scale, UiMouse mouse) {
    size_t count = e->event_drag.size / sizeof(EventDragItem);

    if (e->drag != DRAG_EVENT_POINT || count < 2) {
        return false;
    }

    EventDragItem *items = (EventDragItem *)e->event_drag.data;

    if (mouse.left && ed_gesture_moved(e, mouse)) {
        ed_begin_edit_move(e);
        double delta = ed_gesture_held(e, GESTURE_LOCK_TIME, mouse)
                           ? 0
                           : ed_snap(e->node_beat + (mouse.pos.x - e->marquee_start.x) / scale,
                                     ed_event_snap(e), ed_gesture_held(e, GESTURE_FREE, mouse)) -
                                 e->node_beat;

        for (size_t i = 0; i < count; ++i) {
            delta = fmax(delta, -items[i].original.beat);
        }

        for (size_t i = 0; i < count; ++i) {
            DmAutomation *a = &e->project.automation[items[i].index];

            if (delta != 0 && a->beat == items[i].original.beat) {
                a->bend_note = 0;
                ed_pitch_edit_units(e, a, ed_automation_raw(e));
            }

            a->beat = items[i].original.beat + delta;
        }

        event_group_values(e, lane, mouse);
        e->automation_all = false;
        event_group_selection(e);

        if (e->automation_cc == DM_EVENT_BEND) {
            ed_edit_feedback(e, ed_automation_pitch(e, e->project.automation[e->automation_node]));
        }

        ed_changed(e);
    }

    if (mouse.released) {
        bool lone = e->lone_click && !e->edit_moved;

        if (e->edit_moved) {
            for (size_t i = 0; i < count; ++i) {
                ed_curve_restore_position(e, items[i].original.curve);
            }

            event_group_selection(e);
            ed_sort_automation(e);
            dm_pattern_fit(&e->project, ed_pattern(e));
            ed_commit(e);
        }

        /* A plain click on a selected point selects only that point. */
        if (lone) {
            e->automation_all = false;
            e->event_selection.size = 0;
        } else {
            e->automation_node = -1;
        }

        e->lone_click = false;
        e->drag = DRAG_NONE;
        e->event_drag.size = 0;
    }

    return true;
}

void ed_automation_node_drag(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    if (event_group_drag(e, lane, scale, mouse)) {
        return;
    }

    if (e->drag != DRAG_EVENT_POINT || e->automation_node < 0 ||
        (size_t)e->automation_node >= e->project.automation_count) {
        return;
    }

    DmAutomation *a = &e->project.automation[e->automation_node];

    if (mouse.left && ed_gesture_moved(e, mouse)) {
        ed_begin_edit_move(e);
        DmAutomation edited =
            ed_gesture_held(e, GESTURE_LOCK_VALUE, mouse)
                ? *a
                : lane_edited(e, *a, lane, mouse.pos.y, ed_gesture_held(e, GESTURE_FREE, mouse));
        e->automation_node = -1;
        ed_select_edited_node(e, edited);
        a = &e->project.automation[e->automation_node];

        if (!ed_gesture_held(e, GESTURE_LOCK_TIME, mouse)) {
            double beat = ed_snap(e->node_beat + (mouse.pos.x - e->marquee_start.x) / scale,
                                  ed_event_snap(e), ed_gesture_held(e, GESTURE_FREE, mouse));

            if (beat != a->beat) {
                a->bend_note = 0;
                ed_pitch_edit_units(e, a, ed_automation_raw(e));
                a->beat = beat;
            }
        }

        if (!ed_gesture_held(e, GESTURE_LOCK_VALUE, mouse) && !ed_pedal_event(e->automation_cc)) {
            int value =
                ed_lane_value(e, lane, mouse.pos.y, *a, ed_gesture_held(e, GESTURE_FREE, mouse));

            if (value != ed_event_value(*a)) {
                a->bend_note = 0;
                ed_pitch_edit_units(e, a, ed_automation_raw(e));
                ed_set_event_value(a, value);
            }
        }

        if (a->type == MIDI_PITCH_BEND) {
            ed_edit_feedback(e, ed_automation_pitch(e, *a));
        }

        ed_changed(e);
    }

    if (mouse.released && !e->edit_moved) {
        e->drag = DRAG_NONE;
        return;
    }

    if (mouse.released) {
        DmAutomation edited = *a;
        ed_curve_restore_position(e, a->curve);
        ed_sort_automation(e);
        e->automation_node = -1;
        ed_select_edited_node(e, edited);
        dm_pattern_fit(&e->project, ed_pattern(e));
        e->drag = DRAG_NONE;
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
    node.bend_note = 0;
    node.bend_root = DM_BEND_ROOT_NONE;
    ed_set_event_value(&node, (int)llround(dm_curve_value(first, last, t)));
    e->project.automation[left].bend = first.bend * t * t;
    node.bend = first.bend * (1 - t) * (1 - t);
    return node;
}

static void detach_bend(Editor *e, DmAutomation node) {
    if (node.type != MIDI_PITCH_BEND || node.curve <= 0) {
        return;
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation *a = &e->project.automation[i];

        if (a->pattern == node.pattern && a->port == node.port && a->curve == node.curve &&
            a->bend_note) {
            a->bend_note = 0;
            return;
        }
    }
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
        detach_bend(e, a);

        if (ed_automation_raw(e)) {
            ed_pitch_edit_units(e, &e->project.automation[i], true);
        }

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
    int anchor =
        e->automation_cc == DM_EVENT_BEND ? ed_nearest_note(e, beat) : ed_curve_anchor(e, beat);
    DmAutomation point = ed_curve_node(e, beat, 0, e->automation_cc, 0, anchor);

    if (point.type == MIDI_PITCH_BEND) {
        ed_pitch_edit_units(e, &point, ed_automation_raw(e));
        point = lane_edited(e, point, lane, mouse.pos.y, ed_gesture_held(e, GESTURE_FREE, mouse));
    }

    ed_set_event_value(&point, ed_lane_value(e, lane, mouse.pos.y, point,
                                             ed_gesture_held(e, GESTURE_FREE, mouse)));
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
    detach_bend(e, a);
    a.bend_note = 0;
    a.bend_root = DM_BEND_ROOT_NONE;
    bool raw = ed_automation_raw(e);
    double original_beat = a.beat;
    bool converted = !a.curve && a.type != MIDI_PROGRAM;
    DmPitchState incoming = ed_pitch_state(e, fmin(a.beat, beat));
    int baseline = 0;

    if (converted) {
        baseline = ed_value_before(e, fmax(0, fmin(a.beat, beat) - 1e-8), e->automation_cc);
        a.curve = ed_curve_id(e);
        a.anchor = ed_pitch_anchor(e, e->project.automation[nearest]);
        e->project.automation[nearest].curve = a.curve;
        e->project.automation[nearest].anchor = a.anchor;

        if (a.type == MIDI_PITCH_BEND) {
            e->project.automation[nearest].bend_root = raw ? DM_BEND_ROOT_MIDI : DM_BEND_ROOT_PITCH;
            ed_curve_range(e, fmin(a.beat, beat), incoming.range, incoming.cents, incoming.msb,
                           incoming.lsb, a.curve);
        }
    } else if (raw) {
        ed_pitch_edit_units(e, &e->project.automation[nearest], true);
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
    connect_point(e, lane, nearest, mouse.pos.y, beat, ed_gesture_held(e, GESTURE_FREE, mouse));
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

    DmAutomation a = connect_point(e, lane, nearest, e->marquee_start.y, start,
                                   ed_gesture_held(e, GESTURE_FREE, mouse));

    if (a.curve <= 0 || fabs(end - start) < 1.0 / DM_PPQ) {
        return true;
    }

    DmAutomation b = a;
    b.beat = end;
    b = lane_edited(e, b, lane, mouse.pos.y, ed_gesture_held(e, GESTURE_FREE, mouse));
    ed_set_event_value(
        &b, ed_lane_value(e, lane, mouse.pos.y, b, ed_gesture_held(e, GESTURE_FREE, mouse)));
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
    double beat = fmax(0, ed_snap((at.x - lane.x + v->scroll_x) / scale, ed_event_snap(e), free));
    float y, middle = lane.y + lane.h * .5f;

    if (ed_pedal_event(e->automation_cc)) {
        y = middle;
    } else if (e->automation_cc == DM_EVENT_BEND && !ed_automation_raw(e)) {
        float step = ED_LANE_STEP;
        float pitch = ed_clampf(e->automation_center + (middle - at.y) / step, 0, 127);
        pitch = free ? pitch : roundf(pitch);
        y = middle - (pitch - e->automation_center) * step;
    } else {
        float maximum = e->automation_cc == DM_EVENT_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX;
        float value = ed_clampf((lane.y + lane.h - 8 - at.y) / (lane.h - 8) * maximum, 0, maximum);
        int step = free ? 0 : ed_value_step(e, lane.h);
        value = step ? (float)ed_value_snap(e, (int)lroundf(value), step) : floorf(value);
        y = lane.y + lane.h - 8 - value / maximum * (lane.h - 8);
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
        a->bend_note = 0;
        ed_pitch_edit_units(e, a, true);
        ed_set_event_value(a, value);
        ed_commit(e);
    }

    /* Curve: Line moves smoothly to the next point; Hold keeps this value until it. */
    if (a->curve >= 0 && a->type != MIDI_PROGRAM && ui_menu(DM_TEXT_CURVE)) {
        bool line = dm_automation_curves(*a);

        if (ui_menu_check(DM_TEXT_CURVE_LINE, line) && !line) {
            ed_checkpoint(e);
            a->bend_note = 0;
            a->ramp = !a->curve;
            a->hold = false;
            ed_commit(e);
        }

        if (ui_menu_check(DM_TEXT_CURVE_HOLD, !line) && line) {
            ed_checkpoint(e);
            a->bend_note = 0;
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
        e->lone_click = !ed_gesture_held(e, GESTURE_EXTEND, mouse);
        e->automation_all = false;
        e->edit_moved = false;
        e->drag = DRAG_EVENT_POINT;
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
    e->event_free = ed_gesture_held(e, GESTURE_FREE,
                                    mouse); /* Alt places without snapping, as everywhere else. */
    e->event_connect = ed_gesture_held(
        e, GESTURE_CONNECT, mouse); /* Shift + click connects the point to the nearest one. */
    e->marquee_start = mouse.pos;

    /* Pedal events are ranges, so a press always draws one. */
    if (ed_pedal_event(e->automation_cc)) {
        ed_checkpoint(e);
        e->drag = DRAG_EVENT_LINE;
        return;
    }

    /* Discrete events such as program changes never form lines: a press places one point. */
    if (discrete_event(e->automation_cc)) {
        e->event_connect = false;
    }

    /* Otherwise the press waits: moving past a small buffer draws a line, releasing adds a
       single point (connected with Shift); see ed_automation_press. */
    e->drag = DRAG_EVENT_PRESS;
    e->event_press_tick = SDL_GetTicks();
}

/* A quick click may wobble a little before release, so it has a larger buffer than a slow press
   that starts moving. */
void ed_automation_press(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    if (e->drag != DRAG_EVENT_PRESS) {
        return;
    }

    float moved = hypotf(mouse.pos.x - e->marquee_start.x, mouse.pos.y - e->marquee_start.y);
    float buffer = SDL_GetTicks() - e->event_press_tick < 250 ? 8.f : 4.f;

    if (moved > buffer && !discrete_event(e->automation_cc)) {
        ed_checkpoint(e);
        e->drag = DRAG_EVENT_LINE; /* ed_automation_drag draws the line and places it on release. */
        return;
    }

    if (mouse.left && !mouse.released) {
        return;
    }

    e->drag = DRAG_NONE;
    UiMouse at = mouse;
    at.pos = e->marquee_start;
    ed_gesture_set(e, GESTURE_FREE, &at, e->event_free || ed_gesture_held(e, GESTURE_FREE, mouse));
    double beat = ed_snap((e->marquee_start.x - lane.x + v->scroll_x) / scale, ed_event_snap(e),
                          ed_gesture_held(e, GESTURE_FREE, at));
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
    if (e->drag != DRAG_EVENT_LINE) {
        return;
    }

    /* The modifier at the press or at the release disables snapping. */
    ed_gesture_set(e, GESTURE_FREE, &mouse,
                   e->event_free || ed_gesture_held(e, GESTURE_FREE, mouse));

    if (ed_pedal_event(e->automation_cc)) {
        float y = lane.y + lane.h * .5f;
        ui_line((UiPoint){e->marquee_start.x, y}, (UiPoint){mouse.pos.x, y}, ed_point_color(e), 4);
    } else {
        /* The preview shows the points where they will land: snapped unless Alt is held. With
           Shift, a dimmer line shows the start joining its nearest point. */
        UiPoint from = placed_point(e, lane, v, scale, e->marquee_start,
                                    ed_gesture_held(e, GESTURE_FREE, mouse));
        UiPoint to =
            placed_point(e, lane, v, scale, mouse.pos, ed_gesture_held(e, GESTURE_FREE, mouse));
        double start_beat = ed_snap((e->marquee_start.x - lane.x + v->scroll_x) / scale,
                                    ed_event_snap(e), ed_gesture_held(e, GESTURE_FREE, mouse));
        int nearest = e->event_connect ? ed_closest_node(e, start_beat) : -1;

        if (nearest >= 0) {
            UiPoint joined = ed_automation_point(e, e->project.automation[nearest], lane, v, scale);
            ui_line(joined, from, (ed_point_color(e) & 0xffffffu) | 0x88000000u, 2);
        }

        ui_line(from, to, ed_point_color(e), 2);
        ui_circle(from, 4, ed_point_color(e));
        ui_circle(to, 4, ed_point_color(e));
    }

    if (!mouse.released) {
        return;
    }

    double start = ed_snap((e->marquee_start.x - lane.x + v->scroll_x) / scale, ed_event_snap(e),
                           ed_gesture_held(e, GESTURE_FREE, mouse));
    double end = ed_snap((mouse.pos.x - lane.x + v->scroll_x) / scale, ed_event_snap(e),
                         ed_gesture_held(e, GESTURE_FREE, mouse));
    double anchor_beat = fmin(start, end);
    e->drag_key = e->automation_cc == DM_EVENT_BEND ? ed_nearest_note(e, anchor_beat)
                                                    : ed_curve_anchor(e, start);
    DmAutomation a = ed_curve_node(e, start, 0, e->automation_cc, 1, e->drag_key);

    if (e->automation_cc < 0) {
        e->bend_range = 127;
        a.curve = INT32_MAX;
    }

    int first =
        ed_lane_value(e, lane, e->marquee_start.y, a, ed_gesture_held(e, GESTURE_FREE, mouse));
    int last = ed_lane_value(e, lane, mouse.pos.y, a, ed_gesture_held(e, GESTURE_FREE, mouse));
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
            ed_pitch_edit_units(e, &point, ed_automation_raw(e));
            point = lane_edited(e, point, lane, e->marquee_start.y,
                                ed_gesture_held(e, GESTURE_FREE, mouse));
        }

        ed_set_event_value(&point, ed_lane_value(e, lane, e->marquee_start.y, point,
                                                 ed_gesture_held(e, GESTURE_FREE, mouse)));
        dm_automation_add(&e->project, point);
        e->last_automation = point;
        e->last_automation_valid = true;
    } else {
        ed_automation_line(e, start, end, first, last, e->automation_cc);
    }

    e->drag = DRAG_NONE;
    ed_commit(e);
}

static bool event_marquee_begin(Editor *e, UiMouse mouse) {
    DmBytes *selection = &e->event_marquee_selection;
    selection->size = 0;

    if (e->automation_all || e->automation_node >= 0 || e->event_selection.size) {
        for (size_t i = 0; i < e->project.automation_count; ++i) {
            if (ed_event_selected(e, i) &&
                !dm_bytes(selection, &e->project.automation[i], sizeof(DmAutomation))) {
                selection->size = 0;
                return false;
            }
        }

        if (selection->size) {
            qsort(selection->data, selection->size / sizeof(DmAutomation), sizeof(DmAutomation),
                  event_identity);
        }
    }

    ed_focus_edit(e, FOCUS_EVENTS);
    ed_deselect_events(e);
    e->event_marquee = true;
    e->event_marquee_start = mouse.pos;
    e->drag = DRAG_NONE;
    e->curve_drag = false;
    return true;
}

void ed_event_box_select(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    UiPoint start = e->event_marquee_start;
    UiRect box = {fminf(start.x, mouse.pos.x), fminf(start.y, mouse.pos.y),
                  fabsf(start.x - mouse.pos.x), fabsf(start.y - mouse.pos.y)};
    e->event_selection.size = 0;
    size_t bytes = e->project.automation_count * sizeof(DmAutomation);
    bool graph = ed_graph_resolution(e) != 0; /* Graph lanes select the bars the box touches. */

    if (dm_reserve((void **)&e->event_selection.data, &e->event_selection.capacity, bytes, 1)) {
        for (size_t i = 0; i < e->project.automation_count; ++i) {
            DmAutomation a = e->project.automation[i];

            if (!ed_automation_matches(e, a) || a.curve < 0) {
                continue;
            }

            bool selected = ed_gesture_held(e, GESTURE_EXTEND, mouse) &&
                            event_selection_contains(&e->event_marquee_selection, a);

            if (!selected && graph) {
                UiRect bar = ed_graph_bar(e, a, lane, v, scale, INFINITY);
                selected = bar.x <= box.x + box.w && bar.x + bar.w >= box.x &&
                           bar.y <= box.y + box.h && bar.y + bar.h >= box.y;
            } else if (!selected) {
                selected = ed_inside(ed_automation_point(e, a, lane, v, scale), box);
            }

            if (!selected) {
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
        e->event_marquee_selection.size = 0;
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
    if (!mouse.left || e->event_marquee || e->tool == TOOL_MARQUEE || !ed_inside(mouse.pos, lane) ||
        !hovered) {
        return;
    }

    char text[64];

    if (e->automation_cc == DM_EVENT_BEND && !ed_automation_raw(e)) {
        float pitch = e->automation_center + (lane.y + lane.h * .5f - mouse.pos.y) / ED_LANE_STEP;

        if (e->drag == DRAG_EVENT_POINT && e->automation_node >= 0) {
            pitch = ed_automation_pitch(e, e->project.automation[e->automation_node]);
        }

        ed_pitch_text(text, sizeof(text), pitch);
    } else {
        DmAutomation a = ed_curve_node(e, 0, 0, e->automation_cc, 0, ed_nearest_note(e, 0));
        int value = ed_lane_value(e, lane, mouse.pos.y, a, ed_gesture_held(e, GESTURE_FREE, mouse));

        if (e->drag == DRAG_EVENT_POINT && e->automation_node >= 0) {
            value = ed_event_value(e->project.automation[e->automation_node]);
        }

        /* Program changes name the instrument instead of its number. */
        if (e->automation_cc == DM_EVENT_PROGRAM) {
            snprintf(text, sizeof(text), "%s", dm_program_name(value));
        } else {
            snprintf(text, sizeof(text), "%d", value);
        }
    }

    ui_placement_tip(text);
}

/* Graph lanes: a left drag paints bars, Shift + drag on a selected bar moves the selected values,
   and a right drag erases the bars it crosses. A click while bars are selected only clears the
   selection, as in point lanes. */
static void graph_input(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse, bool hovered,
                        int hit) {
    bool inside = hovered && ed_inside(mouse.pos, lane);

    if (inside && mouse.clicked && !e->drag) {
        ed_focus_edit(e, FOCUS_EVENTS);

        if (ed_gesture_held(e, GESTURE_EXTEND, mouse) && hit >= 0 &&
            ed_event_selected(e, (size_t)hit)) {
            event_drag_begin(e, hit);
            e->marquee_start = mouse.pos;
            e->node_y = mouse.pos.y;
            e->edit_moved = false;
            e->drag = DRAG_GRAPH_MOVE;
        } else if (events_selected(e)) {
            ed_deselect_events(e);
        } else {
            ed_checkpoint(e);
            e->graph_last = mouse.pos;
            e->event_free = ed_gesture_held(e, GESTURE_FREE, mouse);
            e->drag = DRAG_GRAPH_PAINT;
        }
    }

    if (e->drag == DRAG_GRAPH_PAINT) {
        if (mouse.left) {
            ed_graph_paint(e, lane, v, scale, e->graph_last, mouse.pos,
                           ed_gesture_held(e, GESTURE_FREE, mouse) || e->event_free);
            e->graph_last = mouse.pos;
            ed_changed(e);
        } else {
            e->drag = DRAG_NONE;
            ed_commit(e);
        }
    }

    if (e->drag == DRAG_GRAPH_MOVE) {
        if (mouse.left && ed_gesture_moved(e, mouse)) {
            ed_begin_edit_move(e);
            UiMouse values = mouse;
            /* The modifier chose this gesture; it does not hold the values. */
            ed_gesture_set(e, GESTURE_LOCK_VALUE, &values, false);
            event_group_values(e, lane, values);
            event_group_selection(e);
            ed_changed(e);
        } else if (!mouse.left) {
            if (e->edit_moved) {
                ed_commit(e);
            }

            e->drag = DRAG_NONE;
            e->event_drag.size = 0;
        }
    }

    if (inside && mouse.right && !ed_gesture_held(e, GESTURE_MENU, mouse) && !e->drag) {
        ed_focus_edit(e, FOCUS_EVENTS);

        /* A right click clears the bar selection; holding it erases bars. */
        if (mouse.right_clicked) {
            ed_deselect_events(e);
        }

        e->graph_last = mouse.pos;
        e->drag = DRAG_GRAPH_ERASE;
    }

    if (e->drag == DRAG_GRAPH_ERASE) {
        if (mouse.right && ed_graph_erase(e, lane, v, scale, e->graph_last, mouse.pos)) {
            ed_deselect_events(e);
            ed_commit(e);
        }

        e->graph_last = mouse.pos;
        e->drag = mouse.right ? DRAG_GRAPH_ERASE : DRAG_NONE;
    }

    event_placement_tip(e, lane, mouse, hovered);
}

void ed_automation_input(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse, bool hovered,
                         int hit) {
    /* Over a point only while nothing else holds the mouse. */
    if ((hovered && hit >= 0 && !e->drag && !e->event_marquee && !e->curve_drag) ||
        e->drag == DRAG_EVENT_POINT || e->curve_drag) {
        ui_gesture_cursor(true);
    }

    bool start = hovered && ed_inside(mouse.pos, lane) && mouse.clicked &&
                 (ed_gesture_held(e, GESTURE_MARQUEE, mouse) || e->tool == TOOL_MARQUEE);

    if (start && !event_marquee_begin(e, mouse)) {
        return;
    }

    if (e->event_marquee) {
        ed_event_box_select(e, lane, v, scale, mouse);
        return;
    }

    if (ed_graph_resolution(e)) {
        graph_input(e, lane, v, scale, mouse, hovered, hit);
        return;
    }

    bool pedal = ed_pedal_event(e->automation_cc);
    bool handle = !pedal && ed_curve_handles(e, lane, v, scale, mouse, false, hovered);

    if (!handle && hovered && ed_inside(mouse.pos, lane) &&
        (!pedal || hit >= 0 || fabsf(mouse.pos.y - (lane.y + lane.h * .5f)) < 12) &&
        mouse.clicked) {
        ed_automation_lane_click(e, lane, v, scale, mouse, hit);
    }

    /* A right click clears the point selection. */
    if (hovered && ed_inside(mouse.pos, lane) && mouse.right_clicked && !e->drag &&
        ed_right_free(e, mouse)) {
        ed_focus_edit(e, FOCUS_EVENTS);
        ed_deselect_events(e);
    }

    /* Right-dragging erases points, like notes in the piano roll and playlist placements. */
    if (hovered && ed_inside(mouse.pos, lane) && mouse.right && hit >= 0 && !e->drag &&
        ed_right_free(e, mouse)) {
        ed_focus_edit(e, FOCUS_EVENTS);
        ed_erase_node(e, hit);
    }

    point_menu(e, hovered && ed_inside(mouse.pos, lane) && mouse.right_clicked &&
                          ed_gesture_held(e, GESTURE_MENU, mouse)
                      ? hit
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

    ed_erase_step(e);
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

static size_t selected_curve(Editor *e, const bool *selected, int curve, double *first,
                             double *last) {
    size_t count = 0;
    *first = HUGE_VAL;
    *last = 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.curve == curve && selected[i]) {
            *first = fmin(*first, a.beat);
            *last = fmax(*last, a.beat);
            ++count;
        }
    }

    return count;
}

static bool copy_event_dependencies(Editor *e, DmProject *to, const bool *selected,
                                    DmAutomation node, double origin) {
    double first, last;
    size_t count = selected_curve(e, selected, node.curve, &first, &last);

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != node.pattern || a.port != node.port || abs(a.curve) != node.curve ||
            (a.curve >= 0 && (a.type != MIDI_CONTROL || (a.type == node.type && a.a == node.a)))) {
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

static DmAutomation copied_connection(Editor *e, const bool *selected, size_t index) {
    DmAutomation a = e->project.automation[index];
    bool connected = false;

    for (size_t i = index + 1; i < e->project.automation_count; ++i) {
        DmAutomation b = e->project.automation[i];

        if (b.pattern != a.pattern || b.port != a.port || b.type != a.type || b.curve < 0 ||
            (a.type == MIDI_CONTROL && a.a != b.a)) {
            continue;
        }

        connected = selected[i] && a.curve == b.curve && b.beat > a.beat;
        break;
    }

    if (!connected) {
        a.bend = 0;
        a.ramp = false;
    }

    return a;
}

bool ed_copy_events(Editor *e, DmProject *to) {
    double first = HUGE_VAL;
    bool *selected = calloc(e->project.automation_count + 1, sizeof(*selected));

    if (!selected) {
        return false;
    }

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        selected[i] = ed_event_selected(e, i);

        if (selected[i]) {
            first = fmin(first, e->project.automation[i].beat);
        }
    }

    if (!isfinite(first) || dm_pattern_add(to, DM_TEXT_EVENTS) < 0) {
        free(selected);
        return false;
    }

    snprintf(to->name, sizeof(to->name), DM_TEXT_EVENTS);
    to->patterns[0].event = e->automation_cc;
    bool ok = copy_event_points(e, to, selected, first);
    free(selected);
    return ok;
}

/* Each selected note contributes its own time interval; gaps in an arbitrary selection do not
   collect bends. Section endpoints at the note's end are included. */
bool ed_copy_note_bends(Editor *e, DmProject *to, double origin, bool *selected) {
    DmPattern *p = ed_pattern(e);

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.type != MIDI_PITCH_BEND || a.curve < 0) {
            continue;
        }

        uint32_t owner = 0;

        for (size_t j = 0; a.curve > 0 && j < e->project.automation_count; ++j) {
            DmAutomation root = e->project.automation[j];

            if (root.pattern == a.pattern && root.port == a.port && root.curve == a.curve &&
                root.bend_note) {
                owner = root.bend_note;
                break;
            }
        }

        for (size_t n = 0; n < p->count; ++n) {
            DmNote note = p->notes[n];
            bool contains = owner ? owner == note.bend_id
                                  : a.beat >= note.beat && a.beat <= note.beat + note.length;

            if (note.selected && note.port == a.port && contains) {
                selected[i] = true;
                break;
            }
        }
    }

    to->patterns[0].event = DM_EVENT_BEND;
    return copy_event_points(e, to, selected, origin);
}

static bool copy_event_points(Editor *e, DmProject *to, const bool *selected, double first) {
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (!selected[i]) {
            continue;
        }

        DmAutomation a = copied_connection(e, selected, i);

        if (a.curve > 0) {
            double begin, end;
            size_t count = selected_curve(e, selected, a.curve, &begin, &end);

            if (a.beat == begin && !copy_event_dependencies(e, to, selected, a, first)) {
                return false;
            }

            if (count == 1) {
                a.curve = 0;
                a.bend_note = 0;
                a.fixed_anchor |= a.bend_root != DM_BEND_ROOT_NONE;
            }
        } else if (a.type == MIDI_PITCH_BEND) {
            DmPitchState state = ed_pattern_pitch_state(&e->project, a.pattern, a.beat, a.port);
            const int cc[] = {MIDI_CC_RPN_MSB,  MIDI_CC_RPN_LSB, MIDI_CC_DATA,
                              MIDI_CC_DATA_LSB, MIDI_CC_RPN_MSB, MIDI_CC_RPN_LSB};
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
                                 double source_beat, int source_port) {
    DmPitchState source = ed_pattern_pitch_state(from, 0, source_beat, source_port);
    double bend = (ed_event_value(*a) - MIDI_BEND_CENTER) * (source.range + source.cents * .01) /
                  MIDI_BEND_CENTER;
    float pitch = ed_pitch_anchor(e, *a) + (float)bend;
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
        DmPitchState prior = ed_pitch_state(e, start);

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

static bool paste_one_event(Editor *e, const DmProject *from, DmAutomation a, double at, int offset,
                            bool all, bool note_bends) {
    if (!all && e->automation_cc == DM_EVENT_BEND && a.type == MIDI_CONTROL && !a.curve) {
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

    if (!note_bends) {
        a.bend_note = 0;
    }

    a.beat += at;
    a.pattern = e->selected_pattern;
    a.channel = e->pattern_channel;
    int source_port = a.port;
    a.port = (uint8_t)ed_note_port(e);

    if (a.type == MIDI_PITCH_BEND && !a.curve) {
        paste_discrete_pitch(e, from, &a, source_beat, source_port);
    }

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
    paste_events(e, from, at, false, false);
}

/* Bends keep their notes' original note-color routes. The paste helpers read pitch ranges and
   restoration values from the focused route, so each route's bends are pasted while that route is
   focused, and the user's route, event type and selection are restored afterwards. */
bool ed_paste_note_bends(Editor *e, const DmProject *from, double at) {
    DmProject route;
    dm_project_init(&route);
    bool ok = dm_pattern_add(&route, DM_TEXT_EVENTS) >= 0;
    int port = ed_note_port(e), cc = e->automation_cc;

    if (ok) {
        route.patterns[0].event = DM_EVENT_BEND;

        for (int p = 0; p < DM_NOTE_PORTS && ok; ++p) {
            route.automation_count = 0;

            for (size_t i = 0; i < from->automation_count && ok; ++i) {
                if (from->automation[i].port == p) {
                    ok = dm_automation_add(&route, from->automation[i]);
                }
            }

            if (ok && route.automation_count) {
                e->note_ports[e->pattern_channel] = p;
                ok = paste_events(e, &route, at, false, true);
            }
        }
    }

    e->note_ports[e->pattern_channel] = port;
    e->automation_cc = cc;
    ed_deselect_events(e); /* The pasted notes carry the selection. */
    dm_project_free(&route);
    return ok;
}

static bool paste_events(Editor *e, const DmProject *from, double at, bool all, bool note_bends) {
    size_t bytes = from->automation_count * sizeof(DmAutomation);

    if (!ed_pattern(e) ||
        !dm_reserve((void **)&e->event_selection.data, &e->event_selection.capacity, bytes, 1)) {
        return false;
    }

    DmAutomation *items = prepare_pasted_events(e, from, at);

    if (!items) {
        return false;
    }

    if (!all) {
        e->automation_cc = from->patterns[0].event;
    }

    if (ed_pattern(e)->kind) {
        ed_pattern(e)->event = e->automation_cc;
    }

    e->automation_node = -1;
    e->automation_all = false;
    e->event_selection.size = 0;
    int offset = ed_curve_id(e) - 1;
    bool ok = true;

    for (size_t i = 0; i < from->automation_count; ++i) {
        if (!paste_one_event(e, from, items[i], at, offset, all, note_bends)) {
            ok = false;
            break;
        }
    }

    free(items);
    qsort(e->event_selection.data, e->event_selection.size / sizeof(DmAutomation),
          sizeof(DmAutomation), event_identity);

    if (ok) {
        ok = ed_graph_convert(e, e->selected_pattern);
    }

    dm_pattern_fit(&e->project, ed_pattern(e));
    return ok;
}

void ed_copy_event_lane(Editor *e, bool all) {
    DmPattern *p = ed_pattern(e);

    if (!e->active || !p || (p->kind && p->mode)) {
        return;
    }

    DmProject copy;
    dm_project_init(&copy);
    bool *selected = calloc(e->project.automation_count + 1, sizeof(*selected));
    bool ok = selected && dm_pattern_add(&copy, DM_TEXT_EVENTS) >= 0;

    if (ok) {
        /* Velocity is not a storable event type; the copied notes carry it. */
        copy.patterns[0].event = e->automation_cc == DM_EVENT_VELOCITY ? -2 : e->automation_cc;

        for (size_t i = 0; i < e->project.automation_count && ok; ++i) {
            DmAutomation a = e->project.automation[i];

            if (a.pattern != e->selected_pattern || a.port != ed_note_port(e)) {
                continue;
            }

            if (all) {
                a.pattern = 0;
                ok = dm_automation_add(&copy, a);
            } else {
                selected[i] = a.curve >= 0 && ed_automation_matches(e, a);
            }
        }

        if (!all && ok) {
            ok = copy_event_points(e, &copy, selected, 0);
        }

        if (all || e->automation_cc == DM_EVENT_VELOCITY) {
            for (size_t i = 0; i < p->count && ok; ++i) {
                if (p->notes[i].port == ed_note_port(e)) {
                    ok = dm_note_add(&copy.patterns[0], p->notes[i]);
                }
            }
        }
    }

    DmBytes bytes = {0};

    if (ok) {
        if (!copy.automation_count && !copy.patterns[0].count) {
            dm_bytes_free(&e->event_clipboard);
        } else if (dm_pack(&copy, &bytes)) {
            dm_bytes_free(&e->event_clipboard);
            e->event_clipboard = bytes;
            e->event_clipboard_all = all;
        }
    }

    free(selected);
    dm_project_free(&copy);
}

void ed_paste_event_lane(Editor *e) {
    DmPattern *p = ed_pattern(e);

    if (!e->active || !p || (p->kind && p->mode) || !e->event_clipboard.size) {
        return;
    }

    DmProject from;
    dm_project_init(&from);

    if (!dm_unpack(&from, e->event_clipboard.data, e->event_clipboard.size, e->status,
                   sizeof(e->status))) {
        dm_project_free(&from);
        return;
    }

    bool pushed = ed_checkpoint(e);

    if (!pushed) {
        dm_project_free(&from);
        return;
    }

    bool ok = true;

    if (from.automation_count) {
        ok = paste_events(e, &from, 0, e->event_clipboard_all, false);
    }

    /* Velocity is a note property: apply it to matching notes without creating notes. */
    if (!p->kind) {
        for (size_t i = 0; i < p->count; ++i) {
            if (p->notes[i].port != ed_note_port(e)) {
                continue;
            }

            for (size_t j = 0; j < from.patterns[0].count; ++j) {
                DmNote n = from.patterns[0].notes[j];

                if (n.beat == p->notes[i].beat && n.key == p->notes[i].key) {
                    p->notes[i].velocity = n.velocity;
                    break;
                }
            }
        }
    }

    if (ok) {
        ed_commit(e);
    } else {
        ed_checkpoint_abandon(e, pushed);
    }

    dm_project_free(&from);
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
        double span = ed_duplicate_span(e, first, end, true);

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

    ed_checkpoint(e);
    ed_remove_events(e, remove, e->automation_cc);
    free(remove);
    e->event_selection.size = 0;
    e->automation_node = -1;
    ed_commit(e);
}

/* Removes the flagged points of event type `type` (as in automation_cc), plus each section's
   setup and restoration nodes once none of that section's points of that type remain.
   `remove` has one entry per automation point. */
void ed_remove_events(Editor *e, bool *remove, int type) {
    size_t count = e->project.automation_count;

    for (size_t i = 0; i < count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (!a.curve || remove[i]) {
            continue;
        }

        bool remains = false, removed = false;

        for (size_t j = 0; j < count && !remains; ++j) {
            DmAutomation b = e->project.automation[j];

            if (b.pattern == a.pattern && b.port == a.port && b.curve == abs(a.curve) &&
                event_type_of(b) == type) {
                remains |= !remove[j];
                removed |= remove[j];
            }
        }

        remove[i] = removed && !remains;
    }

    size_t kept = 0;

    for (size_t i = 0; i < count; ++i) {
        if (!remove[i]) {
            e->project.automation[kept++] = e->project.automation[i];
        }
    }

    e->project.automation_count = kept;
    e->last_automation_valid = false;
}

/* Point lines are a lighter tint of the application color, so they stay apart from notes drawn
   in that color; a color already near white is darkened instead. */
uint32_t ed_point_color(Editor *e) {
    uint32_t color = e->accent;
    float r = (float)(color & 0xffu), g = (float)((color >> 8) & 0xffu),
          b = (float)((color >> 16) & 0xffu);
    float target = (.299f * r + .587f * g + .114f * b) / 255.0f > .9f ? 0 : 255;
    r += (target - r) * .5f;
    g += (target - g) * .5f;
    b += (target - b) * .5f;
    return COLOR((uint32_t)r, (uint32_t)g, (uint32_t)b);
}

uint32_t ed_graph_color(Editor *e) {
    return e->accent | 0xff000000u;
}
