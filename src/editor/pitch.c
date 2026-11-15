#include "editor/internal.h"
#include "text/en.h"

static void curve_cc(Editor *e, double beat, int cc, int value, int id);
static bool replace_curve_range(Editor *e, double start, double end, int cc, int key,
                                DmAutomation *carry);
static bool curve_overlaps(Editor *e, double start, double end, int cc);

DmPitchState ed_pattern_pitch_state(const DmProject *p, int pattern_id, double beat,
                                    unsigned port) {
    return dm_pitch_state(p->automation, p->automation_count, pattern_id, beat, port);
}

DmPitchState ed_pitch_state(Editor *e, double beat) {
    return ed_pattern_pitch_state(&e->project, e->selected_pattern, beat, ed_note_port(e));
}

int ed_curve_id(Editor *e) {
    int id = 1;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        int n = abs(e->project.automation[i].curve);

        if (n >= id && n < INT32_MAX) {
            id = n + 1;
        }
    }

    return id;
}

int ed_nearest_note(Editor *e, double beat) {
    DmPattern *p = ed_pattern(e);
    int key = p && p->kind ? p->reference : 60;
    double distance = HUGE_VAL, onset = -1;

    for (size_t i = 0; p && i < p->count; ++i) {
        if (p->notes[i].port != ed_note_port(e)) {
            continue;
        }

        double d = fabs(p->notes[i].beat - beat);

        if (beat >= p->notes[i].beat && beat <= p->notes[i].beat + p->notes[i].length) {
            d = 0;
        }

        if (d < distance || (d == 0 && distance == 0 && p->notes[i].beat > onset)) {
            distance = d;
            onset = p->notes[i].beat;
            key = p->notes[i].key;
        }
    }

    return key;
}

static bool note_has_attached_bend(const Editor *e, DmNote n, unsigned port) {
    if (!n.bend_id) {
        return false;
    }

    size_t low = 0, high = e->project.automation_count;

    while (low < high) {
        size_t mid = low + (high - low) / 2;

        if (e->project.automation[mid].beat < n.beat) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    for (size_t i = low; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.beat > n.beat + n.length) {
            break;
        }

        if (a.pattern == e->selected_pattern && a.port == port && a.type == MIDI_PITCH_BEND &&
            a.curve > 0 && a.bend_note == n.bend_id) {
            return true;
        }
    }

    return false;
}

typedef struct {
    double beat;
    size_t note;
    bool start;
} BendBoundary;

typedef struct {
    uint8_t key;
    bool attached;
} BendNoteInfo;

static int bend_boundary_compare(const void *a, const void *b) {
    const BendBoundary *ba = (const BendBoundary *)a;
    const BendBoundary *bb = (const BendBoundary *)b;

    if (ba->beat != bb->beat) {
        return ba->beat < bb->beat ? -1 : 1;
    }

    return (int)ba->start - (int)bb->start;
}

bool ed_bend_references(Editor *e, EdBendReferences *out) {
    DmPattern *p = ed_pattern(e);
    *out = (EdBendReferences){.fallback = ed_nearest_note(e, 0)};

    if (!p) {
        return true;
    }

    unsigned port = ed_note_port(e);
    size_t note_count = 0;

    for (size_t i = 0; i < p->count; ++i) {
        if (p->notes[i].port == port && p->notes[i].length > 0) {
            ++note_count;
        }
    }

    if (!note_count) {
        return true;
    }

    size_t bound_count = note_count * 2;
    BendNoteInfo *info = malloc(note_count * sizeof(*info));
    BendBoundary *bounds = malloc(bound_count * sizeof(*bounds));
    size_t *active = malloc(note_count * sizeof(*active));
    size_t *active_pos = malloc(note_count * sizeof(*active_pos));
    double *beats = malloc(bound_count * sizeof(*beats));
    int *keys = malloc(bound_count * sizeof(*keys));

    if (!info || !bounds || !active || !active_pos || !beats || !keys) {
        free(info);
        free(bounds);
        free(active);
        free(active_pos);
        free(beats);
        free(keys);
        return false;
    }

    size_t ni = 0, bi = 0;

    for (size_t i = 0; i < p->count; ++i) {
        DmNote n = p->notes[i];

        if (n.port != port || n.length <= 0) {
            continue;
        }

        info[ni] = (BendNoteInfo){n.key, note_has_attached_bend(e, n, port)};
        bounds[bi++] = (BendBoundary){n.beat, ni, true};
        bounds[bi++] = (BendBoundary){n.beat + n.length, ni, false};
        ++ni;
    }

    qsort(bounds, bound_count, sizeof(*bounds), bend_boundary_compare);
    size_t active_count = 0;
    int current_key = -1;

    for (size_t i = 0; i < bound_count;) {
        double beat = bounds[i].beat;

        while (i < bound_count && bounds[i].beat == beat) {
            size_t note = bounds[i].note;

            if (bounds[i].start) {
                active_pos[note] = active_count;
                active[active_count++] = note;
            } else {
                size_t pos = active_pos[note];
                size_t last = active[--active_count];
                active[pos] = last;
                active_pos[last] = pos;
            }

            ++i;
        }

        if (active_count > 0) {
            int key = MIDI_KEYS;

            for (size_t a = 0; a < active_count; ++a) {
                BendNoteInfo item = info[active[a]];

                if (item.attached) {
                    key = item.key;
                    break;
                }

                if (item.key < key) {
                    key = item.key;
                }
            }

            if (key != current_key) {
                beats[out->count] = beat;
                keys[out->count] = key;
                ++out->count;
                current_key = key;
            }
        }
    }

    free(info);
    free(bounds);
    free(active);
    free(active_pos);
    out->beats = beats;
    out->keys = keys;
    return true;
}

int ed_bend_reference_at(const EdBendReferences *r, double beat) {
    if (!r || !r->count) {
        return r ? r->fallback : 60;
    }

    if (beat < r->beats[0]) {
        return r->keys[0];
    }

    size_t low = 0, high = r->count;

    while (low + 1 < high) {
        size_t mid = low + (high - low) / 2;

        if (r->beats[mid] <= beat) {
            low = mid;
        } else {
            high = mid;
        }
    }

    return r->keys[low];
}

void ed_bend_references_free(EdBendReferences *r) {
    if (!r) {
        return;
    }

    free(r->beats);
    free(r->keys);
    r->beats = NULL;
    r->keys = NULL;
    r->count = 0;
}

int ed_curve_anchor(Editor *e, double beat) {
    DmPattern *p = ed_pattern(e);

    if (p && e->context_note >= 0 && (size_t)e->context_note < p->count) {
        DmNote note = p->notes[e->context_note];

        if (beat >= note.beat - .25 && beat <= note.beat + note.length + .25) {
            return note.key;
        }
    }

    return ed_nearest_note(e, beat);
}

int ed_event_value(DmAutomation a) {
    return a.type == MIDI_PITCH_BEND                                   ? a.a + (a.b << 7)
           : a.type == MIDI_PROGRAM || a.type == MIDI_CHANNEL_PRESSURE ? a.a
                                                                       : a.b;
}

void ed_set_event_value(DmAutomation *a, int value) {
    if (a->type == MIDI_PITCH_BEND) {
        a->a = value & 127;
        a->b = (value >> 7) & 127;
    } else if (a->type == MIDI_PROGRAM || a->type == MIDI_CHANNEL_PRESSURE) {
        a->a = value;
    } else {
        a->b = value;
    }
}

DmAutomation ed_curve_node(Editor *e, double beat, int value, int cc, int id, int anchor) {
    DmAutomation a = {beat,
                      cc == DM_EVENT_PROGRAM            ? MIDI_PROGRAM
                      : cc == DM_EVENT_CHANNEL_PRESSURE ? MIDI_CHANNEL_PRESSURE
                      : cc == DM_EVENT_KEY_PRESSURE     ? MIDI_KEY_PRESSURE
                      : cc < 0                          ? MIDI_PITCH_BEND
                                                        : MIDI_CONTROL,
                      e->pattern_channel,
                      cc == DM_EVENT_KEY_PRESSURE ? anchor
                      : cc < 0                    ? 0
                                                  : cc,
                      0,
                      e->selected_pattern,
                      id,
                      anchor,
                      0,
                      false,
                      (uint8_t)ed_note_port(e)};
    a.fixed_anchor = a.type == MIDI_PITCH_BEND;
    ed_set_event_value(&a, value);
    return a;
}

int ed_setting_default(Editor *e, int cc) {
    return cc == DM_EVENT_BEND      ? MIDI_BEND_CENTER
           : cc == DM_EVENT_PROGRAM ? (int)fmax(0, e->project.channels[e->pattern_channel].program)
           : cc == 11               ? 127
           : cc == 7                ? e->project.channels[e->pattern_channel].volume
           : cc == 10               ? e->project.channels[e->pattern_channel].pan
           : cc >= 70 && cc <= 79   ? 64
                                    : 0;
}

static bool setting_matches(DmAutomation a, int cc) {
    return (cc == DM_EVENT_BEND && a.type == MIDI_PITCH_BEND) ||
           (cc == DM_EVENT_PROGRAM && a.type == MIDI_PROGRAM) ||
           (cc == DM_EVENT_CHANNEL_PRESSURE && a.type == MIDI_CHANNEL_PRESSURE) ||
           (cc == DM_EVENT_KEY_PRESSURE && a.type == MIDI_KEY_PRESSURE) ||
           (a.type == MIDI_CONTROL && a.a == cc);
}

/* A Line's first node seen so far, with the pattern's state just before it. */
typedef struct {
    int curve, value;
    bool has;
} LineStart;

typedef struct {
    LineStart *lines;
    size_t line_count, line_capacity;
    int current;
    bool has;
} PatternWalk;

/* Applies one point to the walk: a restoration returns to the state before its Line, or releases;
   a Line's first node remembers that state. */
static bool walk_point(Editor *e, PatternWalk *w, DmAutomation a, int cc, bool pedal) {
    if (a.curve < 0 && !pedal) {
        size_t at = 0;

        while (at < w->line_count && w->lines[at].curve != -a.curve) {
            ++at;
        }

        w->has = at < w->line_count && w->lines[at].has;
        w->current = w->has ? w->lines[at].value : ed_setting_default(e, cc);

        if (at < w->line_count) {
            w->lines[at] = w->lines[--w->line_count];
        }

        return true;
    }

    if (a.curve > 0) {
        size_t at = 0;

        while (at < w->line_count && w->lines[at].curve != a.curve) {
            ++at;
        }

        if (at == w->line_count) {
            if (w->line_count == w->line_capacity) {
                size_t capacity = w->line_capacity ? w->line_capacity * 2 : 8;
                LineStart *grown = realloc(w->lines, capacity * sizeof(*w->lines));

                if (!grown) {
                    return false;
                }

                w->lines = grown;
                w->line_capacity = capacity;
            }

            w->lines[w->line_count++] = (LineStart){a.curve, w->current, w->has};
        }
    }

    w->current = ed_event_value(a);
    w->has = true;
    return true;
}

/* The selected pattern's state for setting cc on the note port after its points up to beat, taken
   in the order they play (beat, then dm_automation_phase, then array position). With stop_line
   above 0 the walk ends before that Line's first node instead. */
static bool pattern_value(Editor *e, double beat, int stop_line, int cc, int *value) {
    bool pedal = cc == MIDI_CC_SUSTAIN || cc == MIDI_CC_SOSTENUTO;
    PatternWalk w = {.current = ed_setting_default(e, cc)};
    const DmAutomation *items = e->project.automation;
    size_t total = e->project.automation_count;
    bool stopped = false, ok = true;

    for (size_t i = 0; ok && !stopped && i < total && items[i].beat <= beat;) {
        size_t end = i, matching = 0;

        while (end < total && items[end].beat == items[i].beat) {
            DmAutomation a = items[end++];
            matching += a.pattern == e->selected_pattern && a.port == ed_note_port(e) &&
                        setting_matches(a, cc);
        }

        /* Only same-beat points of this setting need the play order. */
        for (int phase = matching > 1 ? 1 : 3; ok && !stopped && phase <= 3; ++phase) {
            for (size_t k = i; ok && k < end; ++k) {
                DmAutomation a = items[k];

                if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) ||
                    !setting_matches(a, cc) ||
                    (matching > 1 && dm_automation_phase(&e->project, a) != phase)) {
                    continue;
                }

                if (stop_line > 0 && a.curve == stop_line) {
                    stopped = true;
                    break;
                }

                ok = walk_point(e, &w, a, cc, pedal);
            }
        }

        i = end;
    }

    free(w.lines);

    if (value) {
        *value = w.has ? w.current : ed_setting_default(e, cc);
    }

    return w.has;
}

bool ed_pattern_value_at(Editor *e, double beat, int cc, int *value) {
    return pattern_value(e, beat, 0, cc, value);
}

bool ed_pattern_value_before_line(Editor *e, int line, int cc, int *value) {
    return pattern_value(e, HUGE_VAL, line, cc, value);
}

int ed_value_before(Editor *e, double beat, int cc) {
    int value;
    ed_pattern_value_at(e, beat, cc, &value);
    return value;
}

static void curve_cc(Editor *e, double beat, int cc, int value, int id) {
    size_t at = e->project.automation_count;
    DmAutomation added = {
        beat, MIDI_CONTROL, e->pattern_channel,      cc, value, e->selected_pattern, id, 0,
        0,    false,        (uint8_t)ed_note_port(e)};

    while (at && dm_automation_later(e->project.automation[at - 1], added)) {
        --at;
    }

    if (!dm_automation_add(&e->project, added)) {
        return;
    }

    /* Range messages are inserted in time order while a pitch group is being dragged. */
    EventDragItem *items = (EventDragItem *)e->event_drag.data;

    for (size_t i = 0; i < e->event_drag.size / sizeof(*items); ++i) {
        items[i].index += items[i].index >= at;
    }

    if (e->automation_node >= (int)at) {
        ++e->automation_node;
    }
}

void ed_curve_range(Editor *e, double beat, int range, int cents, int msb, int lsb, int id) {
    curve_cc(e, beat, 101, 0, id);
    curve_cc(e, beat, 100, 0, id);
    curve_cc(e, beat, 6, range, id);
    curve_cc(e, beat, 38, cents, id);
    curve_cc(e, beat, 101, msb, id);
    curve_cc(e, beat, 100, lsb, id);
}

/* Removes the points of setting cc (and of key, for poly pressure) inside [start, end] and
   returns the last removed plain point in *carry. Points of other Lines are kept: a Line may only
   touch the range at its ends, and its last node and return belong to it. */
static bool replace_curve_range(Editor *e, double start, double end, int cc, int key,
                                DmAutomation *carry) {
    bool has_carry = false;

    for (size_t i = e->project.automation_count; i > 0; --i) {
        DmAutomation a = e->project.automation[i - 1];

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) ||
            !setting_matches(a, cc) || (cc == DM_EVENT_KEY_PRESSURE && a.a != key) ||
            a.beat < start || a.beat > end || a.curve) {
            continue;
        }

        if (!ed_pedal_event(cc) && (!has_carry || a.beat > carry->beat)) {
            *carry = a;
            has_carry = true;
        }

        memmove(e->project.automation + i - 1, e->project.automation + i,
                (e->project.automation_count - i) * sizeof(a));
        --e->project.automation_count;
    }

    return has_carry;
}

static bool curve_overlaps(Editor *e, double start, double end, int cc) {
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) || a.curve <= 0) {
            continue;
        }

        bool matches = setting_matches(a, cc);

        if (!matches) {
            continue;
        }

        double first = a.beat, last = a.beat;

        for (size_t j = 0; j < e->project.automation_count; ++j) {
            DmAutomation b = e->project.automation[j];

            if (b.pattern == a.pattern && b.port == a.port && b.curve == a.curve &&
                b.type == a.type) {
                first = fmin(first, b.beat);
                last = fmax(last, b.beat);
            }
        }

        if (fmax(first, start) < fmin(last, end) - 1e-9) {
            return true;
        }
    }

    return false;
}

/* Whether a drawn section would overlap an existing one; the order of start and end is free. */
bool ed_automation_line_overlaps(Editor *e, double start, double end, int cc) {
    double low = fmin(start, end), high = fmax(fmax(start, end), low + 1.0 / DM_PPQ);
    return curve_overlaps(e, low, high, cc);
}

static void automation_line(Editor *e, double start, double end, int first, int last, int cc,
                            DmNote *note) {
    bool reverse = end < start;

    if (reverse) {
        double b = start;
        start = end;
        end = b;
        int v = first;
        first = last;
        last = v;
    }

    end = fmax(end, start + 1.0 / DM_PPQ);

    if (curve_overlaps(e, start, end, cc)) {
        ed_message(e, DM_TEXT_SECTION_OVERLAPS_EXISTING);
        return;
    }

    size_t added = (cc == DM_EVENT_BEND ? 15 : 3) + 1;

    if (!dm_reserve((void **)&e->project.automation, &e->project.automation_capacity,
                    e->project.automation_count + added, sizeof(DmAutomation))) {
        ed_message(e, DM_TEXT_CANNOT_ALLOCATE_AUTOMATION_SECTION);
        return;
    }

    int id = ed_curve_id(e);
    int anchor = note ? note->key : cc < 0 ? e->drag_key : ed_nearest_note(e, start);
    int baseline = cc == 64 || cc == 66 ? 0 : ed_value_before(e, start, cc);
    DmPitchState previous = ed_pitch_state(e, start);
    DmAutomation carry = {0};
    bool has_carry = replace_curve_range(e, start, end, cc, anchor, &carry);
    bool point_at_end = false;

    if (has_carry) {
        for (size_t i = 0; i < e->project.automation_count; ++i) {
            DmAutomation a = e->project.automation[i];
            bool match = setting_matches(a, cc) && (cc != DM_EVENT_KEY_PRESSURE || a.a == anchor);

            if (a.pattern == e->selected_pattern && a.port == ed_note_port(e) && match &&
                a.curve >= 0 && a.beat == end) {
                point_at_end = true;
                break;
            }
        }
    }

    if (cc == DM_EVENT_BEND) { /* Pitch bends carry their bend range. */
        ed_curve_range(e, start, e->bend_range, 0, 127, 127, id);
    }

    DmAutomation from = ed_curve_node(e, start, first, cc, id, anchor);
    DmAutomation to = ed_curve_node(e, end, last, cc, id, anchor);

    DmAutomation *root = reverse ? &to : &from;

    if (cc == DM_EVENT_BEND) {
        root->bend_root = note || !ed_automation_raw(e) ? DM_BEND_ROOT_PITCH : DM_BEND_ROOT_MIDI;
    }

    if (note) {
        if (!note->bend_id) {
            note->bend_id = ed_next_bend_id(e);
        }

        root->bend_note = note->bend_id;
    }

    dm_automation_add(&e->project, from);
    dm_automation_add(&e->project, to);
    /* A user-created section is bounded; imported discrete MIDI is untouched outside it. */
    double after = end;
    dm_automation_add(&e->project, ed_curve_node(e, after, baseline, cc, -id, anchor));

    if (cc == DM_EVENT_BEND) {
        ed_curve_range(e, after, previous.range, previous.cents, previous.msb, previous.lsb, -id);
    }

    if (has_carry && !point_at_end) {
        carry.beat = end;
        carry.curve = 0;
        carry.bend = 0;
        carry.ramp = false;
        carry.hold = false;
        carry.bend_note = 0;

        if (carry.type != MIDI_PITCH_BEND || carry.pattern < 0 || !carry.fixed_anchor) {
            carry.bend_root = DM_BEND_ROOT_NONE;
        }

        dm_automation_add(&e->project, carry);
    }

    dm_pattern_fit(&e->project, ed_pattern(e));
}

void ed_automation_line(Editor *e, double start, double end, int first, int last, int cc) {
    automation_line(e, start, end, first, last, cc, NULL);
}

void ed_note_bend_line(Editor *e, double start, double end, int value, DmNote *note) {
    automation_line(e, start, end, MIDI_BEND_CENTER, value, -2, note);
}

uint32_t ed_next_bend_id(Editor *e) {
    DmPattern *p = ed_pattern(e);
    uint32_t last = 0;

    for (size_t i = 0; i < p->count; ++i) {
        if (p->notes[i].bend_id > last) {
            last = p->notes[i].bend_id;
        }
    }

    /* A deleted note's curve can remain in the pattern. Do not give its ID to another note. */
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern == e->selected_pattern && a.bend_note > last) {
            last = a.bend_note;
        }
    }

    return last < UINT32_MAX ? last + 1 : 0;
}

static bool same_bend(DmAutomation a, DmAutomation root) {
    return a.pattern == root.pattern && a.port == root.port && abs(a.curve) == root.curve;
}

static size_t bend_note_index(DmAutomation root, const DmNote *notes, size_t count) {
    for (size_t n = 0; n < count; ++n) {
        if (notes[n].bend_id == root.bend_note && notes[n].port == root.port) {
            return n;
        }
    }

    return count;
}

/* A backwards bend can extend to the left of its note. Stop the whole drag at beat zero. */
double ed_note_bend_offset(Editor *e, const DmNote *notes, size_t count, double offset) {
    DmProject *p = &e->project;

    for (size_t i = 0; offset < 0 && i < p->automation_count; ++i) {
        DmAutomation root = p->automation[i];

        if (!root.bend_note || root.pattern != e->selected_pattern) {
            continue;
        }

        size_t n = bend_note_index(root, notes, count);

        if (n == count || !notes[n].selected) {
            continue;
        }

        for (size_t j = 0; j < p->automation_count; ++j) {
            if (same_bend(p->automation[j], root)) {
                offset = fmax(offset, -p->automation[j].beat);
            }
        }
    }

    return offset;
}

/* Move the section with its note, including range setup and the restoration at the end. */
void ed_follow_note_curves(Editor *e, int pattern, const DmNote *before, size_t count,
                           double previous_offset) {
    DmProject *project = &e->project;

    if (pattern < 0 || (size_t)pattern >= project->pattern_count) {
        return;
    }

    DmPattern *p = &project->patterns[pattern];
    count = count < p->count ? count : p->count;
    bool changed = false;

    for (size_t i = 0; i < project->automation_count; ++i) {
        DmAutomation root = project->automation[i];

        if (!root.bend_note || root.pattern != pattern) {
            continue;
        }

        size_t n = bend_note_index(root, before, count);

        if (n == count) {
            continue;
        }

        DmNote old = before[n], now = p->notes[n];
        double shift = now.beat - old.beat - (old.selected ? previous_offset : 0);

        if (shift == 0 && now.key == root.anchor) {
            continue;
        }

        changed = true;

        for (size_t j = 0; j < project->automation_count; ++j) {
            DmAutomation *node = &project->automation[j];

            if (same_bend(*node, root)) {
                node->beat = fmax(0, node->beat + shift);

                if (node->type == MIDI_PITCH_BEND) {
                    node->anchor = now.key;
                }
            }
        }
    }

    if (changed) {
        ed_sort_automation(e);
    }
}
