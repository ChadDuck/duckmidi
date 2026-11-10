/* Graph editing: an event lane drawn as one bar per point, painted on a grid of bar cells.

   Bars are ordinary discrete points (curve 0), so playback and export are unchanged. A pattern's
   `graph` field holds the cell size as a note denominator (16 = 1/16 note = 0.25 beat). A Graph
   pattern keeps its graph-eligible lanes discrete: entering Graph converts line sections, and
   pasting converts the pasted lines. */
#include "editor/internal.h"
#include "text/en.h"

static const int graph_resolutions[] = {8, 16, 32, 64, 128, 256};

bool ed_graph_event(int cc) {
    return cc != -3 && !ed_point_only_event(cc);
}

int ed_graph_resolution(Editor *e) {
    DmPattern *p = ed_pattern(e);
    return p && !p->mode && ed_graph_event(e->automation_cc) ? p->graph : 0;
}

double ed_event_snap(Editor *e) {
    int graph = ed_graph_resolution(e);
    return graph ? 4.0 / graph : .25;
}

/* Graph lanes show pitch bends as raw MIDI values. */
bool ed_automation_raw(Editor *e) {
    return e->automation_raw || ed_graph_resolution(e);
}

/* Range setup controllers belong to pitch sections; they are not lanes of their own. */
static bool range_setup(DmAutomation a) {
    return a.type == MIDI_CONTROL && (a.a == MIDI_CC_RPN_MSB || a.a == MIDI_CC_RPN_LSB ||
                                      a.a == MIDI_CC_DATA || a.a == MIDI_CC_DATA_LSB);
}

bool ed_graph_lane(DmAutomation a) {
    return a.type == MIDI_PITCH_BEND ||
           (a.type == MIDI_CONTROL && !range_setup(a) && ed_graph_event(a.a));
}

static bool same_lane(DmAutomation a, DmAutomation b) {
    return a.pattern == b.pattern && a.type == b.type && (a.type != MIDI_CONTROL || a.a == b.a);
}

/* The point a curving segment ends at, paired the way playback pairs them (curve_segment in
   src/midi.c): the next point of the same lane and curve. */
static const DmAutomation *segment_end(const DmAutomation *items, size_t count, size_t index) {
    for (size_t j = index + 1; j < count; ++j) {
        if (items[j].curve == items[index].curve && same_lane(items[j], items[index])) {
            return &items[j];
        }
    }

    return NULL;
}

static bool restored(const DmAutomation *items, size_t count, DmAutomation node) {
    for (size_t i = 0; i < count; ++i) {
        if (items[i].curve == -node.curve && same_lane(items[i], node)) {
            return true;
        }
    }

    return false;
}

static int compare_ids(const void *left, const void *right) {
    int a = *(const int *)left, b = *(const int *)right;
    return (a > b) - (a < b);
}

static bool converted_id(const int *ids, size_t count, int curve) {
    int id = abs(curve);
    return id && bsearch(&id, ids, count, sizeof(*ids), compare_ids);
}

static DmAutomation discrete(DmAutomation a) {
    a.curve = 0;
    a.ramp = a.hold = false;
    a.bend = 0;
    return a;
}

/* Samples at every cell start strictly inside the segment from a to b. */
static bool add_samples(DmBytes *out, DmAutomation a, DmAutomation b, double cell) {
    for (double k = floor(a.beat / cell) + 1; k * cell < b.beat - 1e-9; ++k) {
        DmAutomation sample = discrete(a);
        sample.beat = k * cell;
        ed_set_event_value(&sample, (int)llround(dm_curve_value(
                                        a, b, (sample.beat - a.beat) / (b.beat - a.beat))));

        if (!dm_bytes(out, &sample, sizeof(sample))) {
            return false;
        }
    }

    return true;
}

/* Merges two sorted runs; at equal positions points from `kept` come first. */
static DmAutomation *merge_runs(const DmAutomation *kept, size_t kept_count,
                                const DmAutomation *added, size_t added_count) {
    DmAutomation *out = malloc((kept_count + added_count + 1) * sizeof(*out));
    size_t i = 0, j = 0, n = 0;

    if (!out) {
        return NULL;
    }

    while (i < kept_count || j < added_count) {
        bool take_kept =
            j == added_count || (i < kept_count && !dm_automation_later(kept[i], added[j]));
        out[n++] = take_kept ? kept[i++] : added[j++];
    }

    return out;
}

static void sort_points(DmAutomation *items, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        DmAutomation a = items[i];
        size_t j = i;

        while (j && dm_automation_later(items[j - 1], a)) {
            items[j] = items[j - 1];
            --j;
        }

        items[j] = a;
    }
}

/* The automation with the line sections and ramps of every graph-eligible lane of `pattern`
   replaced by discrete points: each node keeps its beat, and every cell start inside a curving
   segment gets the value the segment plays there. A section's range setup and restoration points
   become discrete points at their beats, so the pitch range and the value after the section are
   unchanged. Sets *points to the new array of *count points, or NULL when nothing changes.
   Returns false when memory runs out. */
static bool graph_points(Editor *e, int pattern, int graph, DmAutomation **points, size_t *count) {
    const DmAutomation *items = e->project.automation;
    size_t total = e->project.automation_count, id_count = 0, kept_count = 0;
    double cell = 4.0 / graph;
    int *ids = malloc((total + 1) * sizeof(*ids));
    DmAutomation *kept = malloc((total + 1) * sizeof(*kept));
    DmBytes added = {0};
    bool ok = ids && kept, changed = false;

    for (size_t i = 0; ok && i < total; ++i) {
        if (items[i].pattern == pattern && items[i].curve > 0 && ed_graph_lane(items[i])) {
            ids[id_count++] = items[i].curve;
        }
    }

    if (ok) {
        qsort(ids, id_count, sizeof(*ids), compare_ids);
    }

    for (size_t i = 0; ok && i < total; ++i) {
        DmAutomation a = items[i];
        bool lane = a.pattern == pattern && ed_graph_lane(a);
        bool line = lane && a.curve > 0, ramp = lane && !a.curve && a.ramp;

        if (a.pattern == pattern && a.curve && converted_id(ids, id_count, a.curve) &&
            (a.curve < 0 || range_setup(a))) {
            kept[kept_count++] = discrete(a); /* Restoration or range setup of a section. */
            changed = true;
            continue;
        }

        if (!line && !ramp) {
            kept[kept_count++] = a;
            continue;
        }

        const DmAutomation *end = segment_end(items, total, i);
        changed = true;

        /* Playback replaces a section's last value with its restoration at the same tick. */
        if (!line || end || !restored(items, total, a)) {
            kept[kept_count++] = discrete(a);
        }

        if (end && dm_automation_curves(a)) {
            ok = add_samples(&added, a, *end, cell);
        }
    }

    size_t added_count = added.size / sizeof(DmAutomation);
    *points = NULL;

    if (ok && changed) {
        sort_points((DmAutomation *)added.data, added_count);
        *points = merge_runs(kept, kept_count, (const DmAutomation *)added.data, added_count);
        *count = kept_count + added_count;
        ok = *points != NULL;
    }

    free(ids);
    free(kept);
    dm_bytes_free(&added);
    return ok;
}

static void apply_points(Editor *e, DmAutomation *points, size_t count) {
    if (points) {
        free(e->project.automation);
        e->project.automation = points;
        e->project.automation_count = e->project.automation_capacity = count;
        ed_deselect_events(e);
    }
}

/* Converts `pattern`'s lines to bars of the given resolution. The caller owns the undo step.
   Returns false, leaving the project unchanged, when memory runs out. */
bool ed_graph_convert(Editor *e, int pattern, int graph) {
    size_t count;
    DmAutomation *points;

    if (!graph_points(e, pattern, graph, &points, &count)) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_AUTOMATION_EDIT);
        return false;
    }

    apply_points(e, points, count);
    return true;
}

/* Switches the open pattern between Point (mode 0, graph 0), LFO (mode 1) and Graph (mode 0,
   graph > 0) as one undo step. Entering Graph converts lines; leaving it keeps the data. */
void ed_graph_mode(Editor *e, int mode, int graph) {
    DmPattern *p = ed_pattern(e);
    graph = mode ? 0 : graph;

    if (!p || (p->mode == mode && p->graph == graph)) {
        return;
    }

    size_t count = 0;
    DmAutomation *points = NULL;

    if (graph && !p->graph) {
        if (!graph_points(e, e->selected_pattern, graph, &points, &count)) {
            ed_message(e, DM_TEXT_CANNOT_ALLOCATE_AUTOMATION_EDIT);
            return;
        }
    }

    ed_checkpoint(e);
    apply_points(e, points, count);
    p = ed_pattern(e);
    p->mode = mode;
    p->graph = graph;
    e->automation_node = -1;
    e->automation_all = false;
    ed_commit(e);
}

void ed_graph_mode_request(Editor *e, int mode, int graph) {
    DmPattern *p = ed_pattern(e);
    bool to_graph = !mode && graph, from_graph = p && !p->mode && p->graph;
    bool data = false;

    for (size_t i = 0; p && (to_graph || from_graph) && i < e->project.automation_count && !data;
         ++i) {
        DmAutomation a = e->project.automation[i];
        data = a.pattern == e->selected_pattern && ed_graph_lane(a);
    }

    if (!data) {
        ed_graph_mode(e, mode, graph);
        return;
    }

    e->graph_switch = true;
    e->graph_switch_pattern = e->selected_pattern;
    e->graph_switch_mode = mode;
    e->graph_switch_graph = graph;
}

void ed_graph_mode_dialog(Editor *e) {
    if (!ui_modal(DM_TEXT_GRAPH_SWITCH_TITLE, &e->graph_switch, 420, 150)) {
        return;
    }

    bool to_graph = !e->graph_switch_mode && e->graph_switch_graph;
    ui_wrap(to_graph ? DM_TEXT_GRAPH_SWITCH_TO_GRAPH : DM_TEXT_GRAPH_SWITCH_TO_POINT);

    if (ui_button(DM_TEXT_CANCEL, 110, 0)) {
        e->graph_switch = false;
        ui_close_popup();
    }

    ui_same();

    if (ui_button(DM_TEXT_GRAPH_SWITCH_CONFIRM, 110, 0)) {
        /* The pattern may have changed while the question was open. */
        if (e->graph_switch_pattern == e->selected_pattern) {
            ed_graph_mode(e, e->graph_switch_mode, e->graph_switch_graph);
        }

        e->graph_switch = false;
        ui_close_popup();
    }

    ui_popup_end();
}

static bool snap_target(Editor *e, size_t index, bool selection) {
    DmAutomation a = e->project.automation[index];
    return !a.curve && (selection ? ed_event_selected(e, index) : ed_automation_matches(e, a));
}

/* Snap bars: sets the selected bars, or every bar of the displayed lane when none are selected,
   to the nearest value row. Bars keep their beats. One undo step; never applied automatically,
   so recorded values stay as played. */
void ed_graph_snap(Editor *e) {
    /* Before the lane is first drawn, rows are taken for a lane of typical height. */
    float height = e->event_lane_height > 0 ? e->event_lane_height : 200;
    int step = ed_graph_resolution(e) ? ed_value_step(e, height) : 0;
    size_t count = e->project.automation_count;

    if (!step) {
        return;
    }

    bool selection = false;

    for (size_t i = 0; i < count && !selection; ++i) {
        selection = ed_event_selected(e, i);
    }

    bool changed = false;

    for (size_t i = 0; i < count && !changed; ++i) {
        DmAutomation a = e->project.automation[i];
        changed = snap_target(e, i, selection) &&
                  ed_value_snap(e, ed_event_value(a), step) != ed_event_value(a);
    }

    if (!changed) {
        return;
    }

    ed_checkpoint(e);

    for (size_t i = 0; i < count; ++i) {
        DmAutomation *a = &e->project.automation[i];

        if (snap_target(e, i, selection)) {
            ed_set_event_value(a, ed_value_snap(e, ed_event_value(*a), step));
        }
    }

    ed_commit(e);
}

static double lane_beat(UiRect lane, View *v, float scale, float x) {
    return fmax(0, (x - lane.x + v->scroll_x) / scale);
}

/* First index whose beat is not before `beat`. */
static size_t lower_bound(Editor *e, double beat) {
    size_t low = 0, high = e->project.automation_count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (e->project.automation[middle].beat < beat) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low;
}

static bool in_range(Editor *e, size_t index, double to) {
    DmAutomation a = e->project.automation[index];
    return a.beat < to && ed_automation_matches(e, a) && a.curve >= 0;
}

/* Removes the displayed lane's points with beats in [from, to). Returns how many were removed. */
static size_t remove_range(Editor *e, double from, double to) {
    DmAutomation *items = e->project.automation;
    size_t first = lower_bound(e, from), n = first, removed = 0;

    for (size_t i = first; i < e->project.automation_count; ++i) {
        if (in_range(e, i, to)) {
            ++removed;
        } else {
            items[n++] = items[i];
        }
    }

    e->project.automation_count = n;
    return removed;
}

static bool any_in_range(Editor *e, double from, double to) {
    for (size_t i = lower_bound(e, from);
         i < e->project.automation_count && e->project.automation[i].beat < to; ++i) {
        if (in_range(e, i, to)) {
            return true;
        }
    }

    return false;
}

static void place_bar(Editor *e, UiRect lane, double beat, double from, double to, float y,
                      bool free) {
    DmAutomation point = ed_curve_node(e, beat, 0, e->automation_cc, 0, ed_curve_anchor(e, beat));
    ed_set_event_value(&point, ed_lane_value(e, lane, y, point, free));
    remove_range(e, from, to);
    dm_automation_add(&e->project, point);
}

/* Paints bars along the pointer's path since the last frame, so fast drags leave no gaps. Each
   crossed cell gets one point at its start with the pointer's value there, replacing the points
   in that cell. With Alt, points go at the exact beat of each crossed pixel and replace only the
   points under that pixel. */
void ed_graph_paint(Editor *e, UiRect lane, View *v, float scale, UiPoint from, UiPoint to,
                    bool free) {
    double cell = ed_event_snap(e);

    if (free) {
        float step = 1 / ui_scale();
        int pixels = (int)ceilf(fabsf(to.x - from.x) / step);

        for (int i = 0; i <= pixels; ++i) {
            float t = pixels ? (float)i / pixels : 1;
            float x = from.x + (to.x - from.x) * t, y = from.y + (to.y - from.y) * t;
            double beat = lane_beat(lane, v, scale, x), half = step * .5 / scale;
            place_bar(e, lane, beat, beat - half, beat + half, y, true);
        }

        return;
    }

    double first = floor(lane_beat(lane, v, scale, from.x) / cell);
    double last = floor(lane_beat(lane, v, scale, to.x) / cell);
    double direction = last >= first ? 1 : -1;

    for (double k = first;; k += direction) {
        float t = last == first ? 1 : (float)((k - first) / (last - first));
        place_bar(e, lane, k * cell, k * cell, (k + 1) * cell, from.y + (to.y - from.y) * t, false);

        if (k == last) {
            break;
        }
    }
}

/* Erases the bars whose cells the pointer's path crossed since the last frame. One undo step
   covers a whole right-drag erase, as for points. Returns whether any point was removed. */
bool ed_graph_erase(Editor *e, UiRect lane, View *v, float scale, UiPoint from, UiPoint to) {
    double cell = ed_event_snap(e);
    double low = lane_beat(lane, v, scale, fminf(from.x, to.x)) - cell + 1e-9;
    double high = lane_beat(lane, v, scale, fmaxf(from.x, to.x)) + 1e-9;

    if (!any_in_range(e, low, high)) {
        return false;
    }

    if (!e->erase_active) {
        ed_checkpoint(e);
    }

    e->erase_active = true;
    remove_range(e, low, high);
    return true;
}

/* The lane position of a bipolar lane's center value, or the bottom for other lanes. */
static float bar_base(Editor *e, UiRect lane, View *v, float scale) {
    int cc = e->automation_cc;

    if (cc != -2 && cc != MIDI_CC_PAN) {
        return lane.y + lane.h - 8;
    }

    DmAutomation center = ed_curve_node(e, 0, cc == -2 ? MIDI_BEND_CENTER : 64, cc, 0, 0);
    return ed_automation_point(e, center, lane, v, scale).y;
}

UiRect ed_graph_bar(Editor *e, DmAutomation a, UiRect lane, View *v, float scale,
                    double next_beat) {
    UiPoint top = ed_automation_point(e, a, lane, v, scale);
    float base = bar_base(e, lane, v, scale);
    double end = fmin(next_beat, a.beat + ed_event_snap(e));
    float width = (float)(end - a.beat) * scale;
    return (UiRect){top.x, fminf(top.y, base), fmaxf(width, 1 / ui_scale()),
                    fmaxf(fabsf(base - top.y), 1)};
}

typedef struct {
    float column, x, y, base;
    uint32_t color;
    bool used;
} GraphColumn;

static void column_flush(GraphColumn *column) {
    if (column->used) {
        ui_rect((UiRect){column->x, fminf(column->y, column->base), 1 / ui_scale(),
                         fmaxf(fabsf(column->base - column->y), 1)},
                column->color, 0);
    }

    column->used = false;
}

/* Draws the bar of point `a` (index `index`) up to the next point at `next_beat`. Bars narrower
   than a display pixel share one column drawn with the column's last value. */
static void draw_bar(Editor *e, UiRect lane, View *v, float scale, size_t index, double next_beat,
                     UiMouse mouse, GraphColumn *column, int *hit) {
    DmAutomation a = e->project.automation[index];
    UiRect bar = ed_graph_bar(e, a, lane, v, scale, next_beat);
    uint32_t color = ed_event_selected(e, index) ? TEXT : ed_graph_color(e);
    float top = ed_automation_point(e, a, lane, v, scale).y;
    float px = floorf(bar.x * ui_scale());

    if (bar.w * ui_scale() < 1.5f) {
        if (column->used && px != column->column) {
            column_flush(column);
        }

        *column = (GraphColumn){px, bar.x, top, bar_base(e, lane, v, scale), color, true};
    } else {
        column_flush(column);
        ui_rect((UiRect){bar.x, bar.y, bar.w > 3 ? bar.w - 1 : bar.w, bar.h}, color, 0);
        double end = a.beat + ed_event_snap(e);

        /* Past its cell the value holds until the next point, as playback does. */
        if (isfinite(next_beat) && next_beat > end) {
            float x = bar.x + bar.w, next = lane.x + (float)next_beat * scale - v->scroll_x;
            ui_line((UiPoint){x, top}, (UiPoint){next, top},
                    (ed_graph_color(e) & 0xffffffu) | 0x99000000u, 1.5f);
        }
    }

    float right = bar.x + fmaxf(bar.w, 3);

    if (mouse.pos.x >= bar.x && mouse.pos.x < right && mouse.pos.y >= bar.y - 4 &&
        mouse.pos.y <= bar.y + bar.h + 4) {
        *hit = (int)index;
    }
}

int ed_graph_plot(Editor *e, UiRect lane, View *v, float scale, UiMouse mouse) {
    int cc = e->automation_cc, hit = -1;

    if (cc == -2 || cc == MIDI_CC_PAN) {
        float y = bar_base(e, lane, v, scale);
        ui_line((UiPoint){lane.x, y}, (UiPoint){lane.x + lane.w, y}, DIM, 1);
    }

    GraphColumn column = {0};
    size_t first = ed_automation_visible_first(e, (v->scroll_x - 8) / scale, false);
    size_t previous = SIZE_MAX;
    double right = (v->scroll_x + lane.w + 8) / scale;

    for (size_t i = first; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (!ed_automation_matches(e, a) || a.curve < 0) {
            continue;
        }

        if (previous != SIZE_MAX) {
            draw_bar(e, lane, v, scale, previous, a.beat, mouse, &column, &hit);
        }

        if (a.beat > right) {
            previous = SIZE_MAX;
            break;
        }

        previous = i;
    }

    if (previous != SIZE_MAX) {
        draw_bar(e, lane, v, scale, previous, INFINITY, mouse, &column, &hit);
    }

    column_flush(&column);
    return hit;
}

/* Point / Graph for a note pattern's events section, or Point / LFO / Graph for an automation
   pattern. Returns false when the displayed event has only points (velocity, pedals). */
bool ed_graph_controls(Editor *e, DmPattern *p, bool lfo) {
    const char *modes[] = {DM_TEXT_POINT, DM_TEXT_LFO, DM_TEXT_GRAPH};
    int current = p->mode ? 1 : p->graph ? 2 : 0;

    if (!ed_graph_event(e->automation_cc)) {
        return false;
    }

    ui_width(100);

    if (ui_combo(lfo ? "##Automation mode" : "##Event mode", modes[current])) {
        for (int i = 0; i < 3; ++i) {
            if ((i != 1 || lfo) && ui_select(modes[i], current == i, 0, 0) && current != i) {
                ed_graph_mode_request(e, i == 1, i == 2 ? 16 : 0);
            }
        }

        ui_combo_end();
    }

    return true;
}

/* The resolution and Snap bars, while the displayed lane is a graph. */
void ed_graph_resolution_controls(Editor *e, DmPattern *p) {
    if (!ed_graph_resolution(e)) {
        return;
    }

    char label[16];
    snprintf(label, sizeof(label), DM_TEXT_GRAPH_RESOLUTION, p->graph);
    ui_same();
    ui_width(80);

    if (ui_combo("##Graph resolution", label)) {
        for (size_t i = 0; i < sizeof(graph_resolutions) / sizeof(*graph_resolutions); ++i) {
            snprintf(label, sizeof(label), DM_TEXT_GRAPH_RESOLUTION, graph_resolutions[i]);

            if (ui_select(label, p->graph == graph_resolutions[i], 0, 0) &&
                p->graph != graph_resolutions[i]) {
                ed_checkpoint(e);
                ed_pattern(e)->graph = graph_resolutions[i];
                ed_commit(e);
            }
        }

        ui_combo_end();
    }

    ui_tip(DM_TEXT_GRAPH_RESOLUTION_TIP);
    ui_same();

    if (ui_button(DM_TEXT_SNAP_BARS, 0, 0)) {
        ed_graph_snap(e);
    }

    ui_tip(DM_TEXT_SNAP_BARS_TIP);
}
