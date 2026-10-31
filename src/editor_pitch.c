#include "editor_internal.h"
#include "text/en.h"

static void curve_cc(Editor *e, double beat, int cc, int value, int id);
static void replace_curve_range(Editor *e, double start, double end, int cc);
static bool curve_overlaps(Editor *e, double start, double end, int cc);

PitchState ed_pattern_pitch_state(const DmProject *p, int pattern_id, double beat, unsigned port) {
    PitchState state = {2, 0, 127, 127};

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];

        if (a.beat > beat) {
            continue;
        }

        if (a.pattern != pattern_id || a.type != MIDI_CONTROL || a.port != port) {
            continue;
        }

        if (a.a == MIDI_CC_RPN_MSB) {
            state.msb = a.b;
        } else if (a.a == MIDI_CC_RPN_LSB) {
            state.lsb = a.b;
        } else if (state.msb == 0 && state.lsb == 0) {
            if (a.a == MIDI_CC_DATA) {
                state.range = a.b;
            }

            if (a.a == MIDI_CC_DATA_LSB) {
                state.cents = a.b;
            }
        }
    }

    return state;
}

PitchState ed_pitch_state(Editor *e, double beat) {
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
    double distance = HUGE_VAL;

    for (size_t i = 0; p && i < p->count; ++i) {
        if (p->notes[i].port != ed_note_port(e)) {
            continue;
        }

        double d = fabs(p->notes[i].beat - beat);

        if (beat >= p->notes[i].beat && beat <= p->notes[i].beat + p->notes[i].length) {
            d = 0;
        }

        if (d < distance) {
            distance = d;
            key = p->notes[i].key;
        }
    }

    return key;
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
                      cc == -1   ? MIDI_PROGRAM
                      : cc == -4 ? MIDI_CHANNEL_PRESSURE
                      : cc == -5 ? MIDI_KEY_PRESSURE
                      : cc < 0   ? MIDI_PITCH_BEND
                                 : MIDI_CONTROL,
                      e->pattern_channel,
                      cc == -5 ? anchor
                      : cc < 0 ? 0
                               : cc,
                      0,
                      e->selected_pattern,
                      id,
                      anchor,
                      0,
                      false,
                      (uint8_t)ed_note_port(e)};
    ed_set_event_value(&a, value);
    return a;
}

int ed_value_before(Editor *e, double beat, int cc) {
    int value = cc == -2   ? MIDI_BEND_CENTER
                : cc == -1 ? (int)fmax(0, e->project.channels[e->pattern_channel].program)
                : cc == 11 ? 127
                : cc == 7  ? e->project.channels[e->pattern_channel].volume
                : cc == 10 ? e->project.channels[e->pattern_channel].pan
                : cc >= 70 && cc <= 79 ? 64
                                       : 0;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.beat > beat) {
            break;
        }

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e)) {
            continue;
        }

        if ((cc == -2 && a.type == MIDI_PITCH_BEND) || (cc == -1 && a.type == MIDI_PROGRAM) ||
            (cc == -4 && a.type == MIDI_CHANNEL_PRESSURE) ||
            (cc == -5 && a.type == MIDI_KEY_PRESSURE) || (a.type == MIDI_CONTROL && a.a == cc)) {
            value = ed_event_value(a);
        }
    }

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

static void replace_curve_range(Editor *e, double start, double end, int cc) {
    for (size_t i = e->project.automation_count; i > 0; --i) {
        DmAutomation a = e->project.automation[i - 1];
        bool match = (cc < 0 && a.type == MIDI_PITCH_BEND) || (a.type == MIDI_CONTROL && a.a == cc);

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) || !match ||
            a.beat < start || a.beat > end) {
            continue;
        }

        memmove(e->project.automation + i - 1, e->project.automation + i,
                (e->project.automation_count - i) * sizeof(a));
        --e->project.automation_count;
    }
}

static bool curve_overlaps(Editor *e, double start, double end, int cc) {
    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern != e->selected_pattern || a.port != ed_note_port(e) || a.curve <= 0) {
            continue;
        }

        bool matches = cc < 0 ? a.type == MIDI_PITCH_BEND : a.type == MIDI_CONTROL && a.a == cc;

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

void ed_automation_line(Editor *e, double start, double end, int first, int last, int cc) {
    if (end < start) {
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

    int id = ed_curve_id(e), anchor = cc < 0 ? e->drag_key : ed_nearest_note(e, start);
    int baseline = cc == 64 || cc == 66 ? 0 : ed_value_before(e, start, cc);
    PitchState previous = ed_pitch_state(e, start);
    replace_curve_range(e, start, end, cc);

    if (cc == -2) { /* Pitch bends carry their bend range. */
        ed_curve_range(e, start, e->bend_range, 0, 127, 127, id);
    }

    dm_automation_add(&e->project, ed_curve_node(e, start, first, cc, id, anchor));
    dm_automation_add(&e->project, ed_curve_node(e, end, last, cc, id, anchor));
    /* A user-created section is bounded; imported discrete MIDI is untouched outside it. */
    double after = end;
    dm_automation_add(&e->project, ed_curve_node(e, after, baseline, cc, -id, anchor));

    if (cc == -2) {
        ed_curve_range(e, after, previous.range, previous.cents, previous.msb, previous.lsb, -id);
    }

    e->transient_pattern = -1;
    dm_pattern_fit(&e->project, ed_pattern(e));
}

/* Finds the note a drawn curve was attached to: its anchor key and the time of its first point. */
static bool curve_on_note(const DmProject *project, int pattern, int curve, DmNote note) {
    for (size_t i = 0; i < project->automation_count; ++i) {
        DmAutomation a = project->automation[i];

        if (a.pattern == pattern && a.curve == curve && a.port == note.port) {
            return a.anchor == note.key && a.beat >= note.beat - .25 &&
                   a.beat <= note.beat + note.length + .25;
        }
    }

    return false;
}

/* Curves drawn on a note follow it when the note moves: they shift by the same time and take the
   note's new key as their anchor, so bends keep their register relative to the moved note.
   before holds the pattern's notes from before the edit, in the same order. */
void ed_follow_note_curves(Editor *e, int pattern, const DmNote *before, size_t count) {
    DmProject *project = &e->project;

    if (pattern < 0 || (size_t)pattern >= project->pattern_count) {
        return;
    }

    DmPattern *p = &project->patterns[pattern];
    size_t automation = project->automation_count;
    bool *moved = calloc(automation + 1, sizeof(*moved));

    if (!moved) {
        return;
    }

    for (size_t n = 0; n < count && n < p->count; ++n) {
        DmNote old = before[n], now = p->notes[n];
        double shift = now.beat - old.beat;

        if (shift == 0 && now.key == old.key) {
            continue;
        }

        for (size_t i = 0; i < automation; ++i) {
            DmAutomation a = project->automation[i];

            if (moved[i] || a.pattern != pattern || a.curve <= 0 ||
                !curve_on_note(project, pattern, a.curve, old)) {
                continue;
            }

            /* Move every node of this curve once, including setup and restoration nodes. */
            for (size_t j = 0; j < automation; ++j) {
                DmAutomation *node = &project->automation[j];

                if (!moved[j] && node->pattern == pattern && abs(node->curve) == a.curve) {
                    node->beat = fmax(0, node->beat + shift);
                    node->anchor = node->anchor == old.key ? now.key : node->anchor;
                    moved[j] = true;
                }
            }
        }
    }

    free(moved);
    ed_sort_automation(e);
}
