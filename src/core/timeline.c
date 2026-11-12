/* The project as a time-ordered event list: notes, curves, pitch bends, LFOs and song events
   collected for export and playback. */
#include "core/midi_internal.h"

bool dm_midi_add_message(Midi *m, double beat, int track, const uint8_t *data, size_t size) {
    if (!isfinite(beat) || beat < 0 || beat * DM_PPQ > UINT32_MAX) {
        return false;
    }

    Raw r = {0};
    r.tick = (uint64_t)llround(beat * DM_PPQ);
    r.track = track;

    if (!dm_bytes(&r.bytes, data, size) || !dm_midi_raw_add(m, &r)) {
        dm_bytes_free(&r.bytes);
        return false;
    }

    return true;
}

static bool muted(const DmProject *p, int c) {
    bool solo = false;

    for (int i = 0; i < 16; ++i) {
        solo |= p->channels[i].enabled && p->channels[i].solo;
    }

    return p->channels[c].mute || (solo && !p->channels[c].solo);
}

static bool collect_channels(const DmProject *p, Midi *m) {
    for (int c = 0; c < 16; ++c) {
        if (p->channels[c].enabled) {
            uint8_t program[] = {
                MIDI_PROGRAM | c,
                (uint8_t)(p->channels[c].program < 0 ? 0 : p->channels[c].program)};
            uint8_t vol[] = {MIDI_CONTROL | c, MIDI_CC_VOLUME, (uint8_t)p->channels[c].volume},
                    pan[] = {MIDI_CONTROL | c, MIDI_CC_PAN, (uint8_t)p->channels[c].pan};

            if (!dm_midi_add_message(m, 0, c + 1, program, 2) ||
                !dm_midi_add_message(m, 0, c + 1, vol, 3) ||
                !dm_midi_add_message(m, 0, c + 1, pan, 3)) {
                return false;
            }

            m->items[m->count - 1].generated_default = p->channels[c].pan == 64;
        }
    }

    return true;
}

static bool skip_source_event(const uint8_t *b, double beat) {
    if (beat == 0 && (b[0] == MIDI_SYSEX || b[0] == MIDI_SYSEX_END)) {
        return true;
    }

    if (b[0] == MIDI_META && (b[1] == MIDI_META_END || b[1] == MIDI_META_PORT ||
                              (beat == 0 && (b[1] == MIDI_META_TEMPO || b[1] == MIDI_META_TIME)))) {
        return true;
    }

    if (b[0] < MIDI_SYSTEM && beat == 0 &&
        ((b[0] & MIDI_STATUS_MASK) == MIDI_CONTROL &&
         (b[1] == MIDI_CC_VOLUME || b[1] == MIDI_CC_PAN))) {
        return true;
    }

    return false;
}

static bool collect_source_events(const DmProject *p, Midi *m) {
    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        const uint8_t *b = e->message.data;

        if (skip_source_event(b, e->beat)) {
            continue;
        }

        int track = b[0] < MIDI_SYSTEM ? (b[0] & MIDI_CHANNEL_MASK) + 1 : 0;

        if ((b[0] & MIDI_STATUS_MASK) == MIDI_PROGRAM && e->beat == 0) {
            uint8_t program[] = {b[0], p->channels[b[0] & MIDI_CHANNEL_MASK].program < 0
                                           ? 0
                                           : p->channels[b[0] & MIDI_CHANNEL_MASK].program};

            if (!dm_midi_add_message(m, 0, track, program, 2)) {
                return false;
            }

            continue;
        }

        if (!dm_midi_add_message(m, e->beat, track, b, e->message.size)) {
            return false;
        }
    }

    return true;
}

static bool collect_song_automation(const DmProject *p, Midi *m) {
    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];

        if (a.pattern >= 0) {
            continue;
        }

        uint8_t b[] = {a.type | a.channel, a.a, a.b};

        if (!dm_midi_add_message(m, a.beat, a.channel + 1, b,
                                 a.type == MIDI_PROGRAM || a.type == MIDI_CHANNEL_PRESSURE ? 2
                                                                                           : 3)) {
            return false;
        }
    }

    return true;
}

/* Converts a pattern beat to a song beat. Controls before a placement's offset are sent at its
   start, in order, so the placement begins with the pattern's state at that point. */
static double clip_time(const DmClip *clip, double beat) {
    return clip->beat + fmax(0, beat - clip->offset);
}

static double clip_end(const DmProject *p, const DmClip *clip) {
    return clip->offset + dm_clip_length(p, *clip);
}

static bool curve_sample(Midi *m, const DmClip *clip, DmAutomation a, double beat, int value) {
    /* Program and channel pressure carry their value in the first data byte. */
    uint8_t first = a.type == MIDI_PITCH_BEND ? value & MIDI_DATA_MASK
                    : a.type == MIDI_PROGRAM || a.type == MIDI_CHANNEL_PRESSURE ? value
                                                                                : a.a;
    uint8_t second = a.type == MIDI_PITCH_BEND ? (value >> 7) & MIDI_DATA_MASK : value;
    uint8_t message[] = {a.type | clip->channel, first, second};
    return dm_midi_add_message(m, clip_time(clip, beat), clip->channel + 1, message,
                               (a.type & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3);
}

typedef struct {
    int curve, root, range;
    uint8_t port;
    double scale;
} BendRebase;

static const DmAutomation *curve_next(const DmProject *p, size_t index) {
    DmAutomation a = p->automation[index];

    for (size_t j = index + 1; j < p->automation_count; ++j) {
        const DmAutomation *b = &p->automation[j];

        if (b->pattern == a.pattern && b->port == a.port && b->curve == a.curve &&
            b->type == a.type && (a.type != MIDI_CONTROL || b->a == a.a)) {
            return b;
        }
    }

    return NULL;
}

static int bend_value(DmAutomation point) {
    return point.a + (point.b << 7);
}

static bool bend_needs_rebase(DmAutomation root) {
    return root.bend_root == DM_BEND_ROOT_PITCH && bend_value(root) != MIDI_BEND_CENTER;
}

/* Compare in wheel units so an authored semitone can differ by one unit after quantization. */
static bool bend_root_matches(const DmPattern *pattern, DmAutomation root, double range) {
    bool matches = false;
    double offset = (bend_value(root) - MIDI_BEND_CENTER) * range;

    for (size_t i = 0; i < pattern->count; ++i) {
        DmNote note = pattern->notes[i];

        if (note.port != root.port) {
            continue;
        }

        if (root.bend_note && note.bend_id == root.bend_note) {
            return false;
        }

        if (note.beat <= root.beat && root.beat < note.beat + note.length &&
            fabs((note.key - root.anchor) * MIDI_BEND_CENTER - offset) <= range + 1e-9) {
            matches = true;
        }
    }

    return matches;
}

static int rebase_wheel(const BendRebase *rebase, double value) {
    return (int)lround(
        fmax(0, fmin(MIDI_BEND_MAX, MIDI_BEND_CENTER + (value - rebase->root) * rebase->scale)));
}

/* Increase the emitted sensitivity only when the translated curve would exceed the wheel range.
   Include the outgoing curve's extremum so a shaped segment keeps its bend. */
static BendRebase bend_rebase(const DmProject *p, DmAutomation root, double range) {
    BendRebase rebase = {
        .curve = root.curve, .root = bend_value(root), .port = root.port, .scale = 1};
    double low = rebase.root, high = rebase.root;
    bool has_range = false;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];

        if (a.pattern != root.pattern || a.port != root.port || a.curve != root.curve) {
            continue;
        }

        if (a.type == MIDI_CONTROL && a.a == MIDI_CC_DATA) {
            has_range = true;
        }

        if (a.type != MIDI_PITCH_BEND) {
            continue;
        }

        double value = bend_value(a);
        low = fmin(low, value);
        high = fmax(high, value);

        if (!dm_automation_curves(a) || a.bend == 0) {
            continue;
        }

        const DmAutomation *next = curve_next(p, i);

        if (next) {
            double t = .5 + (bend_value(*next) - value) / (8 * a.bend);

            if (t > 0 && t < 1) {
                value = dm_curve_value(a, *next, t);
                low = fmin(low, value);
                high = fmax(high, value);
            }
        }
    }

    double factor = fmax((rebase.root - low) / MIDI_BEND_CENTER,
                         (high - rebase.root) / (MIDI_BEND_MAX - MIDI_BEND_CENTER));

    if (has_range && factor > 1 && range < 127) {
        rebase.range = (int)fmin(127, ceil(range * factor));
        rebase.scale = range / rebase.range;
    }

    return rebase;
}

static bool collect_bend_rebases(const DmProject *p, int pattern_id, DmBytes *rebases) {
    const DmPattern *pattern = &p->patterns[pattern_id];

    if (pattern->kind || dm_graph_resolution(pattern, DM_EVENT_BEND)) {
        return true;
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation root = p->automation[i];

        if (root.pattern != pattern_id || root.curve <= 0 || !bend_needs_rebase(root)) {
            continue;
        }

        double range = dm_pitch_range(p->automation, p->automation_count, root);

        if (bend_root_matches(pattern, root, range)) {
            BendRebase rebase = bend_rebase(p, root, range);

            if (!dm_bytes(rebases, &rebase, sizeof(rebase))) {
                return false;
            }
        }
    }

    return true;
}

static const BendRebase *find_bend_rebase(const DmBytes *rebases, DmAutomation point) {
    const BendRebase *items = (const BendRebase *)rebases->data;

    for (size_t i = 0; point.curve > 0 && i < rebases->size / sizeof(*items); ++i) {
        if (items[i].curve == point.curve && items[i].port == point.port) {
            return &items[i];
        }
    }

    return NULL;
}

static DmAutomation rebase_discrete_bend(const DmProject *p, DmAutomation point) {
    if (point.curve || !bend_needs_rebase(point) || p->patterns[point.pattern].kind ||
        dm_graph_resolution(&p->patterns[point.pattern], DM_EVENT_BEND)) {
        return point;
    }

    double range = dm_pitch_range(p->automation, p->automation_count, point);

    if (bend_root_matches(&p->patterns[point.pattern], point, range)) {
        point.a = MIDI_BEND_CENTER & MIDI_DATA_MASK;
        point.b = MIDI_BEND_CENTER >> 7;
    }

    return point;
}

static bool curve_segment(const DmProject *p, Midi *m, const DmClip *clip, size_t index,
                          const BendRebase *rebase) {
    DmAutomation a = p->automation[index];

    if (!dm_automation_curves(a)) {
        return true;
    }

    const DmAutomation *next = curve_next(p, index);

    if (!next) {
        return true;
    }

    DmAutomation b = *next;

    if (a.type == MIDI_PITCH_BEND && !a.curve) {
        a = rebase_discrete_bend(p, a);
        b = rebase_discrete_bend(p, b);
    }

    int steps = (int)fmin(8192, fmax(1, ceil((b.beat - a.beat) * 48)));

    for (int step = 1; step < steps; ++step) {
        double t = (double)step / steps, value = dm_curve_value(a, b, t);
        int wheel =
            rebase && a.type == MIDI_PITCH_BEND ? rebase_wheel(rebase, value) : (int)llround(value);

        if (!curve_sample(m, clip, a, a.beat + (b.beat - a.beat) * t, wheel)) {
            return false;
        }
    }

    return true;
}

static bool curve_setup(DmAutomation a) {
    return a.type == MIDI_CONTROL && (a.a == MIDI_CC_DATA || a.a == MIDI_CC_DATA_LSB ||
                                      a.a == MIDI_CC_RPN_LSB || a.a == MIDI_CC_RPN_MSB);
}

static int curve_phase(const DmProject *p, DmAutomation a) {
    if (a.curve < 0) {
        return 2;
    }

    if (a.curve == 0 || curve_setup(a)) {
        return 3;
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation next = p->automation[i];

        if (next.pattern != a.pattern || next.curve != a.curve || next.type != a.type) {
            continue;
        }

        if (a.type == MIDI_CONTROL && a.a != next.a) {
            continue;
        }

        if (next.beat > a.beat) {
            return 3;
        }
    }

    return 1;
}

/* Authored glide sections own their enable switch; imported discrete CC5 stays literal. */
static bool collect_portamento_gate(const DmProject *p, Midi *m, const DmClip *clip, size_t index) {
    DmAutomation a = p->automation[index];

    if (a.type != MIDI_CONTROL || a.a != MIDI_CC_PORTAMENTO_TIME || a.curve <= 0) {
        return true;
    }

    double end = a.beat;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation b = p->automation[i];

        if (b.pattern != a.pattern || b.type != MIDI_CONTROL || b.a != MIDI_CC_PORTAMENTO_TIME ||
            b.curve != a.curve) {
            continue;
        }

        if (i < index) {
            return true;
        }

        end = fmax(end, b.beat);
    }

    end = fmin(end, clip_end(p, clip));

    if (end <= a.beat || end <= clip->offset) {
        return true;
    }

    uint8_t on[] = {MIDI_CONTROL | clip->channel, MIDI_CC_PORTAMENTO, 127};
    uint8_t off[] = {MIDI_CONTROL | clip->channel, MIDI_CC_PORTAMENTO, 0};

    if (!dm_midi_add_message(m, clip_time(clip, a.beat), clip->channel + 1, on, 3)) {
        return false;
    }

    m->items[m->count - 1].phase = 3;

    if (!dm_midi_add_message(m, clip_time(clip, end), clip->channel + 1, off, 3)) {
        return false;
    }

    m->items[m->count - 1].phase = 2;
    return true;
}

/* What a graph lane returns to after its last bar: the values the editor starts a lane from
   (ed_value_before), with no pitch bend. */
static int graph_default(const DmProject *p, const DmClip *clip, int lane) {
    switch (lane) {
    case 128:
        return MIDI_BEND_CENTER;
    case MIDI_CC_VOLUME:
        return p->channels[clip->channel].volume;
    case MIDI_CC_PAN:
        return p->channels[clip->channel].pan;
    case MIDI_CC_EXPRESSION:
        return 127;
    default:
        return lane >= 70 && lane <= 79 ? 64 : 0;
    }
}

/* A graph lane's last played bar lasts one cell; then the lane returns to its default, as a Point
   section's restoration does, instead of holding for the rest of the song. Earlier bars hold
   until the next bar. */
static bool graph_returns(const DmProject *p, Midi *m, const DmClip *clip) {
    const DmPattern *pat = &p->patterns[clip->pattern];

    if (!dm_pattern_has_graph(pat)) {
        return true;
    }

    uint8_t seen[DM_NOTE_PORTS][(DM_GRAPH_LANES + 7) / 8] = {{0}};
    double end = clip_end(p, clip);

    for (size_t j = p->automation_count; j-- > 0;) {
        DmAutomation a = p->automation[j];
        int lane = dm_automation_graph_lane(a);

        if (a.pattern != clip->pattern || a.beat > end || lane < 0 ||
            pat->graph_lanes[lane] <= DM_GRAPH_POINT || a.curve || a.port >= DM_NOTE_PORTS ||
            seen[a.port][lane / 8] & (1u << lane % 8)) {
            continue;
        }

        seen[a.port][lane / 8] |= (uint8_t)(1u << lane % 8);
        double cell = 4.0 / pat->graph_lanes[lane];

        if (!curve_sample(m, clip, a, fmin(a.beat + cell, end), graph_default(p, clip, lane))) {
            return false;
        }

        m->items[m->count - 1].phase = 2; /* Before a following placement's values. */
    }

    return true;
}

static bool collect_pattern_automation(const DmProject *p, Midi *m, const DmClip *clip) {
    DmBytes rebases = {0};
    bool ok = graph_returns(p, m, clip) && collect_bend_rebases(p, clip->pattern, &rebases);

    for (size_t j = 0; ok && j < p->automation_count; ++j) {
        DmAutomation a = p->automation[j];

        if (a.pattern != clip->pattern || a.beat > clip_end(p, clip)) {
            continue;
        }

        const BendRebase *rebase = find_bend_rebase(&rebases, a);

        if (!collect_portamento_gate(p, m, clip, j) || !curve_segment(p, m, clip, j, rebase)) {
            ok = false;
            break;
        }

        if (a.type == MIDI_PITCH_BEND) {
            if (rebase) {
                int wheel = rebase_wheel(rebase, bend_value(a));
                a.a = wheel & MIDI_DATA_MASK;
                a.b = wheel >> 7;
            } else {
                a = rebase_discrete_bend(p, a);
            }
        } else if (rebase && rebase->range && a.type == MIDI_CONTROL) {
            if (a.a == MIDI_CC_DATA) {
                a.b = rebase->range;
            } else if (a.a == MIDI_CC_DATA_LSB) {
                a.b = 0;
            }
        }

        uint8_t message[] = {a.type | clip->channel, a.a, a.b};
        ok = dm_midi_add_message(m, clip_time(clip, a.beat), clip->channel + 1, message,
                                 (a.type & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3);

        if (ok) {
            m->items[m->count - 1].phase = curve_phase(p, a);
        }
    }

    dm_bytes_free(&rebases);
    return ok;
}

static bool collect_clip_notes(const DmProject *p, Midi *m, const DmClip *clip) {
    const DmPattern *pat = &p->patterns[clip->pattern];
    double start = clip->offset, length = clip_end(p, clip);

    /* Notes crossing a placement edge are shortened to the visible part. */
    for (size_t j = 0; j < pat->count; ++j) {
        DmNote n = pat->notes[j];

        if (n.beat >= length || n.beat + n.length <= start) {
            continue;
        }

        uint8_t on[] = {MIDI_NOTE_ON | clip->channel, n.key, n.velocity},
                off[] = {MIDI_NOTE_OFF | clip->channel, n.key, 0};
        double end = fmin(n.beat + n.length, length);

        if (!dm_midi_add_message(m, clip_time(clip, n.beat), clip->channel + 1, on, 3) ||
            !dm_midi_add_message(m, clip_time(clip, end), clip->channel + 1, off, 3)) {
            return false;
        }
    }

    return true;
}

static bool collect_note_clips(const DmProject *p, Midi *m, bool mixer) {
    for (size_t i = 0; i < p->clip_count; ++i) {
        const DmClip *clip = &p->clips[i];

        if (mixer && muted(p, clip->channel)) {
            continue;
        }

        if (!collect_pattern_automation(p, m, clip) || !collect_clip_notes(p, m, clip)) {
            return false;
        }
    }

    return true;
}

/* Resolve modular automation once for both MIDI export and audible schedules. */
typedef struct {
    uint64_t tick;
    int key, owner, value, priority;
    bool active;
    double start;
    size_t order;
    size_t source_index;
    int rpn_msb, rpn_lsb;
    bool generated_default;
} ModEvent;

typedef struct {
    ModEvent *items;
    size_t count, capacity, baseline_count;
} ModList;

enum { MOD_PROGRAM = 258 };

static int mod_key(const uint8_t *b) {
    int type = b[0] & MIDI_STATUS_MASK, control = -1;

    if (type == MIDI_CONTROL) {
        if (b[1] == MIDI_CC_DATA || b[1] == MIDI_CC_DATA_LSB ||
            (b[1] >= MIDI_CC_INCREMENT && b[1] <= MIDI_CC_RPN_MSB)) {
            return -1;
        }

        control = b[1];
    }

    if (type == MIDI_PITCH_BEND) {
        control = 128;
    }

    if (type == MIDI_CHANNEL_PRESSURE) {
        control = 129;
    }

    if (type == MIDI_KEY_PRESSURE) {
        control = 130 + b[1];
    }

    if (type == MIDI_PROGRAM) {
        control = MOD_PROGRAM;
    }

    return control < 0 ? -1 : (b[0] & MIDI_CHANNEL_MASK) * 260 + control;
}

static int mod_value(const uint8_t *b) {
    int type = b[0] & MIDI_STATUS_MASK;
    return type == MIDI_PITCH_BEND                                   ? b[1] + (b[2] << 7)
           : (type == MIDI_PROGRAM || type == MIDI_CHANNEL_PRESSURE) ? b[1]
                                                                     : b[2];
}

static bool mod_patch_key(int key) {
    int cc = key % 260;
    return cc == MIDI_CC_BANK || cc == MIDI_CC_BANK_LSB || cc == MOD_PROGRAM;
}

static bool mod_add(ModList *list, ModEvent e) {
    if (list->count >= 4000000 ||
        !dm_reserve((void **)&list->items, &list->capacity, list->count + 1, sizeof(e))) {
        return false;
    }

    e.order = list->count;
    list->items[list->count++] = e;
    return true;
}

static int mod_compare(const void *a, const void *b) {
    const ModEvent *x = a, *y = b;

    if (x->key != y->key) {
        return x->key < y->key ? -1 : 1;
    }

    if (x->tick != y->tick) {
        return x->tick < y->tick ? -1 : 1;
    }

    return x->order < y->order ? -1 : x->order > y->order;
}

static int mod_default(const DmProject *p, int key) {
    int cc = key % 260, c = key / 260;

    if (cc == 259) {
        return 2 * 128;
    }

    if (cc >= 70 && cc <= 79) {
        return 64;
    }

    if (cc == 128) {
        return MIDI_BEND_CENTER;
    }

    if (cc == MIDI_CC_VOLUME) {
        return p->channels[c].volume;
    }

    if (cc == MIDI_CC_PAN) {
        return p->channels[c].pan;
    }

    if (cc == MIDI_CC_DATA) {
        return 2;
    }

    if (cc == MIDI_CC_RPN_LSB || cc == MIDI_CC_RPN_MSB) {
        return 127;
    }

    if (cc == MIDI_CC_EXPRESSION) {
        return 127;
    }

    if (cc == MOD_PROGRAM) {
        return p->channels[c].program < 0 ? 0 : p->channels[c].program;
    }

    return 0;
}

static bool mod_range_emit(Midi *m, ModEvent e, int value) {
    const int controls[] = {
        101, 100, 6, 38, e.rpn_msb >= 128 ? 99 : 101, e.rpn_msb >= 128 ? 98 : 100};
    int values[] = {0, 0, value / 128, value % 128, e.rpn_msb & 127, e.rpn_lsb};
    int c = e.key / 260;

    for (int i = 0; i < 6; ++i) {
        uint8_t bytes[] = {MIDI_CONTROL | c, controls[i], values[i]};

        if (!dm_midi_add_message(m, (double)e.tick / DM_PPQ, c + 1, bytes, 3)) {
            return false;
        }

        m->items[m->count - 1].phase = 3;
    }

    return true;
}

static bool mod_emit(Midi *m, ModEvent e, int value) {
    int cc = e.key % 260, c = e.key / 260, type = MIDI_CONTROL, a = cc, b = value;

    if (cc == 259) {
        return mod_range_emit(m, e, value);
    }

    if (cc == 128) {
        type = MIDI_PITCH_BEND;
        a = value & 127;
        b = value >> 7;
    }

    if (cc == 129) {
        type = MIDI_CHANNEL_PRESSURE;
        a = value;
        b = 0;
    }

    if (cc >= 130 && cc < MOD_PROGRAM) {
        type = MIDI_KEY_PRESSURE;
        a = cc - 130;
    }

    if (cc == MOD_PROGRAM) {
        type = MIDI_PROGRAM;
        a = value;
        b = 0;
    }

    uint8_t bytes[] = {type | c, a, b};

    if (!dm_midi_add_message(m, (double)e.tick / DM_PPQ, c + 1, bytes,
                             (type & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3)) {
        return false;
    }

    if (cc == 128) {
        m->items[m->count - 1].phase = 5;
    }

    m->items[m->count - 1].generated_default = e.generated_default;

    return true;
}

static ModEvent mod_winner(ModEvent *states, size_t used, ModEvent fallback) {
    ModEvent *winner = NULL;

    for (size_t k = 0; k < used; ++k) {
        ModEvent *v = &states[k];

        if (!v->active) {
            continue;
        }

        if (!winner || v->priority > winner->priority ||
            (v->priority == winner->priority &&
             (v->start > winner->start ||
              (v->start == winner->start && v->owner > winner->owner)))) {
            winner = v;
        }
    }

    return winner ? *winner : fallback;
}

static bool mod_resolve_key(const DmProject *p, Midi *m, const ModEvent *events, size_t count) {
    ModEvent *states = calloc(count + 1, sizeof(*states));

    if (!states) {
        return false;
    }

    size_t used = 0;
    ModEvent last = {.value = -1, .owner = -1};
    bool ok = true;
    bool patch = mod_patch_key(events[0].key);
    bool authored = false;

    for (size_t i = 0; i < count; i++) {
        authored |= events[i].key % 260 == MIDI_CC_PAN && !events[i].generated_default;
    }

    for (size_t i = 0; i < count && ok;) {
        size_t first = i;
        uint64_t tick = events[i].tick;

        do {
            ModEvent e = events[i++];
            size_t k = 0;

            while (k < used && states[k].owner != e.owner) {
                ++k;
            }

            if (k == used) {
                ++used;
            }

            states[k] = e;
        } while (i < count && events[i].tick == tick);
        ModEvent fallback = events[0];
        fallback.owner = -1;
        fallback.active = false;
        fallback.value = mod_default(p, events[0].key);
        fallback.rpn_msb = fallback.rpn_lsb = 127;
        ModEvent chosen = mod_winner(states, used, fallback);
        chosen.generated_default &= !authored;
        bool patch_selection =
            patch && ((chosen.active && chosen.tick == tick) || chosen.owner != last.owner);
        chosen.tick = tick;
        /* Reassert the winning transaction when source parameters change underneath it. */
        bool range_changed = chosen.key % 260 == 259;
        bool forwarded = false;

        if (patch) {
            /* Keep authored patch messages in place so banks and SysEx can change the
               meaning of a repeated Program Change. An active clip still owns its lane. */
            for (size_t j = first; j < i; ++j) {
                if (events[j].owner < 0) {
                    if (chosen.owner < 0) {
                        forwarded = true;
                    } else {
                        dm_bytes_free(&m->items[events[j].source_index].bytes);
                    }
                }
            }
        }

        if (!forwarded && (chosen.value != last.value || range_changed || patch_selection)) {
            ok = mod_emit(m, chosen, chosen.value);
        }

        last = chosen;
    }

    free(states);
    return ok;
}

typedef struct {
    int msb, lsb, range, cents;
    bool owned, nrpn;
    int nrpn_msb, nrpn_lsb;
} ModPitch;

static void mod_parameter_select(ModPitch *state, int cc, int value) {
    switch (cc) {
    case MIDI_CC_RPN_MSB:
        state->msb = value;
        state->nrpn = false;
        break;
    case MIDI_CC_RPN_LSB:
        state->lsb = value;
        state->nrpn = false;
        break;
    case MIDI_CC_NRPN_MSB:
        state->nrpn_msb = value;
        state->nrpn = true;
        break;
    case MIDI_CC_NRPN_LSB:
        state->nrpn_lsb = value;
        state->nrpn = true;
        break;
    }
}

static bool mod_parameter(ModPitch *state, const uint8_t *bytes) {
    if ((bytes[0] & MIDI_STATUS_MASK) != MIDI_CONTROL) {
        return false;
    }

    int cc = bytes[1], value = bytes[2];
    mod_parameter_select(state, cc, value);

    if (!state->nrpn && state->msb == 0 && state->lsb == 0) {
        if (cc == MIDI_CC_DATA) {
            state->range = value;
        }

        if (cc == MIDI_CC_DATA_LSB) {
            state->cents = value;
        }

        if (cc == MIDI_CC_INCREMENT) {
            state->range = state->range < 127 ? state->range + 1 : 127;
        }

        if (cc == MIDI_CC_DECREMENT) {
            state->range = state->range > 0 ? state->range - 1 : 0;
        }
    }

    return cc == MIDI_CC_DATA || cc == MIDI_CC_DATA_LSB ||
           (cc >= MIDI_CC_INCREMENT && cc <= MIDI_CC_RPN_MSB);
}

static bool mod_capture_event(ModList *list, ModEvent e, uint64_t boundary) {
    if (!mod_add(list, e)) {
        return false;
    }

    if (e.owner < 0) {
        return true;
    }

    e.tick = boundary;
    e.active = false;
    return mod_add(list, e);
}

static void mod_initial_pitch(const ModList *list, ModPitch *states, double start) {
    for (int c = 0; c < 16; ++c) {
        states[c] =
            (ModPitch){.msb = 127, .lsb = 127, .range = 2, .nrpn_msb = 127, .nrpn_lsb = 127};
    }

    uint64_t tick = (uint64_t)llround(start * DM_PPQ);

    for (size_t i = 0; i < list->baseline_count; ++i) {
        ModEvent e = list->items[i];

        if (e.key % 260 != 259 || e.tick > tick) {
            continue;
        }

        states[e.key / 260] = (ModPitch){.msb = e.rpn_msb & 127,
                                         .lsb = e.rpn_lsb,
                                         .range = e.value / 128,
                                         .cents = e.value % 128,
                                         .nrpn = e.rpn_msb >= 128,
                                         .nrpn_msb = e.rpn_msb & 127,
                                         .nrpn_lsb = e.rpn_lsb};
    }
}

static bool mod_capture_range(ModList *list, const Raw *r, int owner, int priority, double start,
                              uint64_t boundary, ModPitch state, bool active) {
    ModEvent e = {.tick = r->tick,
                  .key = (r->bytes.data[0] & MIDI_CHANNEL_MASK) * 260 + 259,
                  .owner = owner,
                  .value = state.range * 128 + state.cents,
                  .priority = priority,
                  .active = active,
                  .start = start,
                  .rpn_msb = owner < 0 ? (state.nrpn ? state.nrpn_msb + 128 : state.msb) : 127,
                  .rpn_lsb = owner < 0 ? (state.nrpn ? state.nrpn_lsb : state.lsb) : 127};
    return mod_capture_event(list, e, boundary);
}

static bool mod_capture(ModList *list, Midi *source, int owner, int priority, double start,
                        double end) {
    uint64_t boundary = (uint64_t)llround(end * DM_PPQ);
    ModPitch states[16];
    mod_initial_pitch(list, states, start);
    qsort(source->items, source->count, sizeof(*source->items), dm_midi_export_compare);

    for (size_t j = 0; j < source->count; ++j) {
        const Raw *r = &source->items[j];

        if (r->tick > boundary) {
            continue;
        }

        const uint8_t *bytes = r->bytes.data;
        int c = bytes[0] & MIDI_CHANNEL_MASK;
        ModPitch *state = &states[c];
        bool parameter = mod_parameter(state, bytes);
        bool active = r->persistent || (r->phase != 2 && (owner < 0 || r->tick < boundary));

        if (parameter && (owner < 0 || bytes[1] == MIDI_CC_DATA || bytes[1] == MIDI_CC_DATA_LSB)) {
            state->owned = true;

            if (!mod_capture_range(list, r, owner, priority, start, boundary, *state, active)) {
                return false;
            }
        }

        int key = mod_key(bytes);

        if (key < 0) {
            continue;
        }

        if (key % 260 == 128 && !state->owned) {
            state->owned = true;

            if (!mod_capture_range(list, r, owner, priority, start, boundary, *state, active)) {
                return false;
            }
        }

        ModEvent e = {.tick = r->tick,
                      .key = key,
                      .owner = owner,
                      .value = mod_value(bytes),
                      .priority = priority,
                      .active = active,
                      .start = start,
                      .source_index = j,
                      .generated_default = r->generated_default};

        if (!mod_capture_event(list, e, boundary)) {
            return false;
        }
    }

    return true;
}

static bool mod_lfo(const DmProject *p, Midi *m, const DmClip *clip) {
    const DmPattern *pat = &p->patterns[clip->pattern];
    double length = dm_clip_length(p, *clip);
    double samples = ceil(length * fmax(48, pat->speed * 48));

    if (samples > 1000000) {
        return false;
    }

    int steps = (int)fmax(1, samples), last = -1;

    if (pat->event == DM_EVENT_BEND) {
        const int cc[] = {101, 100, 6, 38, 101, 100},
                  values[] = {0, 0, dm_lfo_range(pat), 0, 127, 127};

        for (int i = 0; i < 6; ++i) {
            uint8_t bytes[] = {MIDI_CONTROL | clip->channel, cc[i], values[i]};

            if (!dm_midi_add_message(m, clip->beat, clip->channel + 1, bytes, 3)) {
                return false;
            }
        }
    }

    DmAutomation a = {0,
                      pat->event == DM_EVENT_BEND ? MIDI_PITCH_BEND : MIDI_CONTROL,
                      clip->channel,
                      pat->event < 0 ? 0 : pat->event,
                      0,
                      clip->pattern,
                      0,
                      0,
                      0,
                      false,
                      (uint8_t)pat->port};

    for (int i = 0; i < steps; ++i) {
        double beat = clip->offset + length * i / steps;
        int value = dm_lfo_value(pat, beat);

        if (value != last && !curve_sample(m, clip, a, beat, value)) {
            return false;
        }

        last = value;
    }

    return true;
}

static bool collect_mod_clip(const DmProject *p, Midi *m, ModList *list, size_t index) {
    const DmClip *clip = &p->clips[index];
    const DmPattern *pat = &p->patterns[clip->pattern];
    Midi temp = {0};
    bool ok = pat->kind && pat->mode ? mod_lfo(p, &temp, clip)
                                     : collect_pattern_automation(p, &temp, clip);
    ok = ok && mod_capture(list, &temp, (int)index, !pat->kind || pat->control_scope == 1 ? 2 : 1,
                           clip->beat, clip->beat + dm_clip_length(p, *clip));
    dm_midi_free(&temp);

    if (!ok) {
        return false;
    }

    return collect_clip_notes(p, m, clip);
}

static bool collect_clips(const DmProject *p, Midi *m, bool mixer) {
    bool modular = false;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];
        modular |=
            a.pattern >= 0 && a.curve != 0 && a.type == MIDI_CONTROL &&
            (a.a == MIDI_CC_PORTAMENTO_TIME || a.a == MIDI_CC_SUSTAIN || a.a == MIDI_CC_SOSTENUTO);
    }

    for (size_t i = 0; i < p->clip_count; ++i) {
        modular |= p->patterns[p->clips[i].pattern].kind != 0 || p->clips[i].length > 0 ||
                   p->clips[i].offset > 0;
    }

    if (!modular) {
        return collect_note_clips(p, m, mixer);
    }

    ModList list = {0};
    /* Detached controls from an unscoped source retain their original latch
       behavior, even when note extraction introduces trimmed placements. */
    for (size_t i = 0; i < p->clip_count; i++) {
        const DmClip *clip = &p->clips[i];

        if (p->patterns[clip->pattern].control_scope == 2 && (!mixer || !muted(p, clip->channel))) {
            size_t first = m->count;

            if (!collect_pattern_automation(p, m, clip)) {
                return false;
            }

            for (size_t j = first; j < m->count; j++) {
                m->items[j].persistent = true;
            }
        }
    }

    bool ok = mod_capture(&list, m, -1, 0, 0, 1e12);
    list.baseline_count = list.count;

    for (size_t i = 0; i < m->count; ++i) {
        int key = mod_key(m->items[i].bytes.data);

        if (key >= 0 && !mod_patch_key(key)) {
            dm_bytes_free(&m->items[i].bytes);
        }
    }

    for (size_t i = 0; i < p->clip_count && ok; ++i) {
        if (p->patterns[p->clips[i].pattern].control_scope == 2) {
            continue;
        }

        if (!mixer || !muted(p, p->clips[i].channel)) {
            ok = collect_mod_clip(p, m, &list, i);
        }
    }

    qsort(list.items, list.count, sizeof(*list.items), mod_compare);

    for (size_t i = 0; i < list.count && ok;) {
        size_t end = i + 1;

        while (end < list.count && list.items[end].key == list.items[i].key) {
            ++end;
        }

        ok = mod_resolve_key(p, m, list.items + i, end - i);
        i = end;
    }

    free(list.items);
    size_t write = 0;

    for (size_t i = 0; i < m->count; ++i) {
        if (m->items[i].bytes.size) {
            m->items[write++] = m->items[i];
        }
    }

    m->count = write;
    return ok;
}

static bool collect_initial_sysex(const DmProject *p, Midi *m) {
    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];

        if (e->beat != 0) {
            break;
        }

        uint8_t status = e->message.data[0];

        if (status == MIDI_SYSEX || status == MIDI_SYSEX_END) {
            if (!dm_midi_add_message(m, 0, 0, e->message.data, e->message.size)) {
                return false;
            }
        }
    }

    return true;
}

/* A percussion part other than channel 10, or a melodic channel 10, is announced with a GS
   "use for rhythm part" message after the opening SysEx, unless the source already sends it. The
   synthesizer, an external device and exported files then agree with the channel's setting. */
static bool collect_drum_parts(const DmProject *p, Midi *m) {
    for (int c = 0; c < 16; ++c) {
        bool drums = p->channels[c].drums, sent = false;

        if (!p->channels[c].enabled || drums == (c == MIDI_DRUM_CHANNEL)) {
            continue;
        }

        for (size_t i = 0; i < p->event_count && p->events[i].beat == 0; ++i) {
            int channel;
            bool value;
            const DmBytes *message = &p->events[i].message;
            sent |=
                dm_midi_rhythm_part(message, &channel, &value) && channel == c && value == drums;
        }

        if (sent) {
            continue;
        }

        uint8_t block = (uint8_t)(c == MIDI_DRUM_CHANNEL ? 0 : c < MIDI_DRUM_CHANNEL ? c + 1 : c);
        uint8_t message[] = {MIDI_SYSEX, 10,           0x41, 0x10,          0x42, 0x12,
                             0x40,       0x10 | block, 0x15, drums ? 1 : 0, 0,    MIDI_SYSEX_END};
        unsigned sum = 0;

        for (int i = 6; i < 10; ++i) {
            sum += message[i];
        }

        message[10] = (uint8_t)((128 - sum % 128) & 0x7F);

        if (!dm_midi_add_message(m, 0, 0, message, sizeof(message))) {
            return false;
        }
    }

    return true;
}

bool dm_midi_collect_timing(const DmProject *p, Midi *m) {
    uint32_t us = (uint32_t)llround(60000000.0 / p->bpm);
    uint8_t tempo[] = {MIDI_META, MIDI_META_TEMPO, 3, us >> 16, us >> 8, us};

    if (!dm_midi_add_message(m, 0, 0, tempo, sizeof(tempo))) {
        return false;
    }

    int power = 0;

    while ((1 << power) < p->denominator) {
        ++power;
    }

    uint8_t sig[] = {MIDI_META, MIDI_META_TIME, 4, (uint8_t)p->numerator, (uint8_t)power, 24, 8};

    if (!dm_midi_add_message(m, 0, 0, sig, sizeof(sig))) {
        return false;
    }

    return true;
}

static bool collect_single(const DmProject *p, Midi *m, bool mixer) {
    if (!dm_midi_collect_timing(p, m)) {
        return false;
    }

    if (!collect_initial_sysex(p, m) || !collect_drum_parts(p, m) || !collect_channels(p, m)) {
        return false;
    }

    if (!collect_source_events(p, m)) {
        return false;
    }

    if (!collect_song_automation(p, m)) {
        return false;
    }

    if (!collect_clips(p, m, mixer)) {
        return false;
    }

    return true;
}

/* Resolve each logical port independently, including curve restoration and LFO
   precedence. The ordinary single-port arrangement takes the direct path. */
bool dm_midi_collect(const DmProject *p, Midi *m, bool mixer) {
    bool used[DM_NOTE_PORTS] = {true};
    bool colored = false;
    bool *placed = calloc(p->pattern_count + 1, sizeof(*placed));

    if (!placed) {
        return false;
    }

    for (size_t clip = 0; clip < p->clip_count; clip++) {
        size_t i = (size_t)p->clips[clip].pattern;

        if (i >= p->pattern_count) {
            free(placed);
            return false;
        }

        if (placed[i]) {
            continue;
        }

        placed[i] = true;
        const DmPattern *pat = &p->patterns[i];

        if (pat->port < 0 || pat->port >= DM_NOTE_PORTS) {
            free(placed);
            return false;
        }

        if (pat->kind && pat->mode) {
            used[pat->port] = true;
            colored |= pat->port != 0;
        }

        for (size_t j = 0; j < pat->count; j++) {
            if (pat->notes[j].port >= DM_NOTE_PORTS) {
                free(placed);
                return false;
            }

            used[pat->notes[j].port] = true;
            colored |= pat->notes[j].port != 0;
        }
    }

    for (size_t i = 0; i < p->automation_count; i++) {
        DmAutomation a = p->automation[i];

        if (a.port >= DM_NOTE_PORTS) {
            free(placed);
            return false;
        }

        if (a.pattern < 0 || ((size_t)a.pattern < p->pattern_count && placed[a.pattern])) {
            used[a.port] = true;
            colored |= a.port != 0;
        }
    }

    /* Channel events, such as a port's opening program changes, also select its output. */
    for (size_t i = 0; i < p->event_count; i++) {
        const DmEvent *event = &p->events[i];

        if (event->port > 0 && event->port < DM_NOTE_PORTS && event->message.size &&
            event->message.data[0] < MIDI_SYSTEM) {
            used[event->port] = true;
            colored = true;
        }
    }

    free(placed);

    if (!colored) {
        return collect_single(p, m, mixer);
    }

    bool any_solo = false;

    for (int c = 0; c < 16; c++) {
        any_solo |= p->channels[c].enabled && p->channels[c].solo;
    }

    for (size_t i = 0; i < p->port_mix_count; i++) {
        any_solo |= used[p->port_mix[i].port] && p->channels[p->port_mix[i].channel].enabled &&
                    p->port_mix[i].solo;
    }

    for (int port = 0; port < DM_NOTE_PORTS; port++) {
        if (used[port]) {
            DmProject route = *p;
            Midi stream = {0};
            bool ok = true;

            for (int c = 0; c < 16; c++) {
                DmPortMix mix = dm_port_mix(p, port, c);
                route.channels[c].volume = mix.volume;
                route.channels[c].pan = mix.pan;
                route.channels[c].mute = mix.mute || (any_solo && !mix.solo);
                route.channels[c].solo = false;

                if (mix.program) {
                    route.channels[c].program = mix.program - 1;
                }
            }

            route.patterns = calloc(p->pattern_count, sizeof(*route.patterns));
            route.automation = malloc(p->automation_count * sizeof(*route.automation));
            route.clips = malloc(p->clip_count * sizeof(*route.clips));
            route.events = malloc(p->event_count * sizeof(*route.events));

            if ((p->pattern_count && !route.patterns) ||
                (p->automation_count && !route.automation) || (p->clip_count && !route.clips) ||
                (p->event_count && !route.events)) {
                ok = false;
            }

            for (size_t i = 0; i < p->pattern_count && ok; i++) {
                route.patterns[i] = p->patterns[i];
                DmPattern *pat = &route.patterns[i];
                pat->notes = NULL;
                pat->count = 0;

                if (p->patterns[i].count) {
                    pat->notes = malloc(p->patterns[i].count * sizeof(*pat->notes));

                    if (!pat->notes) {
                        ok = false;
                        break;
                    }
                }

                for (size_t j = 0; j < p->patterns[i].count; j++) {
                    if (p->patterns[i].notes[j].port == port) {
                        pat->notes[pat->count++] = p->patterns[i].notes[j];
                    }
                }
            }

            route.automation_count = route.clip_count = route.event_count = 0;

            if (ok) {
                for (size_t i = 0; i < p->automation_count; i++) {
                    if (p->automation[i].port == port) {
                        route.automation[route.automation_count++] = p->automation[i];
                    }
                }

                for (size_t i = 0; i < p->clip_count; i++) {
                    const DmPattern *pat = &p->patterns[p->clips[i].pattern];

                    if (!pat->kind || !pat->mode || pat->port == port) {
                        route.clips[route.clip_count++] = p->clips[i];
                    }
                }

                for (size_t i = 0; i < p->event_count; i++) {
                    if (p->events[i].port == port || dm_conductor(&p->events[i].message)) {
                        route.events[route.event_count++] = p->events[i];
                    }
                }

                ok = collect_single(&route, &stream, mixer);
            }

            for (size_t i = 0; i < stream.count && ok; i++) {
                Raw raw = stream.items[i];

                if (port && raw.bytes.data[0] >= MIDI_SYSTEM) {
                    continue;
                }

                raw.port = port;

                if (!dm_midi_raw_add(m, &raw)) {
                    ok = false;
                    break;
                }

                stream.items[i].bytes = (DmBytes){0};
            }

            if (route.patterns) {
                for (size_t i = 0; i < p->pattern_count; i++) {
                    free(route.patterns[i].notes);
                }
            }

            free(route.patterns);
            free(route.automation);
            free(route.clips);
            free(route.events);
            dm_midi_free(&stream);

            if (!ok) {
                return false;
            }
        }
    }

    return true;
}

static int event_priority(uint8_t status) {
    switch (status & MIDI_STATUS_MASK) {
    case MIDI_NOTE_OFF:
        return 0;
    case MIDI_NOTE_ON:
        return 4;
    case MIDI_KEY_PRESSURE:
        return 5;
    default:
        return 3;
    }
}

int dm_midi_export_compare(const void *a, const void *b) {
    const Raw *x = a, *y = b;

    if (x->tick != y->tick) {
        return x->tick < y->tick ? -1 : 1;
    }

    int px = x->phase ? x->phase : event_priority(x->bytes.data[0]);
    int py = y->phase ? y->phase : event_priority(y->bytes.data[0]);

    if (px != py) {
        return px < py ? -1 : 1;
    }

    return x->order < y->order ? -1 : x->order > y->order;
}

/* SMF track order puts SysEx on track zero before the channel tracks. Share its track
   when a patch message must precede a same-tick SysEx. */
void dm_midi_sort(Midi *m) {
    qsort(m->items, m->count, sizeof(Raw), dm_midi_export_compare);

    for (size_t first = 0; first < m->count;) {
        size_t end = first;
        bool patch = false, share_track = false;

        do {
            const Raw *r = &m->items[end++];
            uint8_t status = r->bytes.data[0];
            share_track |= patch && (status == MIDI_SYSEX || status == MIDI_SYSEX_END);
            patch |= r->port == 0 && mod_patch_key(mod_key(r->bytes.data));
        } while (end < m->count && m->items[end].tick == m->items[first].tick);

        if (share_track) {
            for (size_t i = first; i < end; ++i) {
                Raw *r = &m->items[i];

                if (r->port == 0 && mod_patch_key(mod_key(r->bytes.data))) {
                    r->track = 0;
                }
            }
        }

        first = end;
    }
}

/* Opening MIDI pan can have become a mixer setting during import. Its source identity survives
   in the retained MIDI, so an explicit centre pan remains different from an unset mixer. This
   reads the retained MIDI again, so only exports call it, not the timeline itself. */
unsigned dm_export_authored_pan(const DmProject *p) {
    unsigned mask = 0;

    for (size_t i = 0; i < p->event_count; i++) {
        const DmEvent *e = &p->events[i];

        if (e->beat == 0 && e->message.size >= 3 &&
            (e->message.data[0] & MIDI_STATUS_MASK) == MIDI_CONTROL &&
            e->message.data[1] == MIDI_CC_PAN) {
            mask |= 1u << (e->message.data[0] & MIDI_CHANNEL_MASK);
        }
    }

    if (!p->original.size) {
        return mask;
    }

    Midi source = {.scan_pan = true, .scan_port = p->port, .scan_sequence = p->sequence};

    for (unsigned ch = 0; ch < 16; ch++) {
        source.first_note[ch] = source.first_pan[ch] = UINT64_MAX;
    }

    /* Unreadable source MIDI keeps every channel's pan authoritative. */
    if (!dm_midi_parse(&source, p->original.data, p->original.size)) {
        mask = 0xffff;
    }

    for (unsigned ch = 0; ch < 16; ch++) {
        if (source.first_pan[ch] != UINT64_MAX && source.first_pan[ch] <= source.first_note[ch]) {
            mask |= 1u << ch;
        }
    }

    dm_midi_free(&source);
    return mask;
}

bool dm_export_timeline(const DmProject *p, DmExportEvent **events, size_t *count,
                        uint64_t *duration, char *error, size_t cap) {
    *events = NULL;
    *count = 0;
    *duration = 0;
    Midi m = {0};
    bool ok = dm_midi_collect(p, &m, true);
    DmExportEvent *out = ok && m.count ? calloc(m.count, sizeof(*out)) : NULL;

    if (m.count && !out) {
        ok = false;
    }

    if (ok) {
        qsort(m.items, m.count, sizeof(*m.items), dm_midi_export_compare);
        *duration = (uint64_t)llround(dm_duration(p) * DM_PPQ);

        for (size_t i = 0; i < m.count; ++i) {
            Raw *r = &m.items[i];
            const uint8_t *b = r->bytes.data;
            DmExportEvent e = {0};
            e.tick = r->tick;
            e.source_id = i + 1;
            e.status = b[0];
            e.port = (uint32_t)r->port;
            e.flags = r->generated_default ? DM_EXPORT_DEFAULT_CONTROL : 0;

            if (b[0] < MIDI_SYSTEM) {
                e.a = b[1];
                e.b = r->bytes.size > 2 ? b[2] : 0;
            } else if (dm_tempo_microseconds(b, r->bytes.size)) {
                e.a = dm_tempo_microseconds(b, r->bytes.size);
            } else if (b[0] == MIDI_SYSEX || b[0] == MIDI_SYSEX_END) {
                e.status = MIDI_SYSEX;
            } else {
                continue; /* Lyrics, markers, meter, and editor metadata do not play audio. */
            }

            out[(*count)++] = e;

            if (e.tick > *duration) {
                *duration = e.tick;
            }
        }
    }

    dm_midi_free(&m);

    if (!ok) {
        free(out);
        *count = 0;
        snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_EXPORT_TIMELINE);
        return false;
    }

    *events = out;
    return true;
}
