/* The project as a time-ordered event list: notes, curves, pitch bends, LFOs and song events
   collected for export and playback. */
#include "core/midi_internal.h"

bool dm_midi_add_message(Midi *m, double beat, int track, const uint8_t *data, size_t size) {
    if (!isfinite(beat) || beat < 0 || beat * DM_PPQ > UINT32_MAX) {
        return false;
    }

    Raw r = {0};
    r.tick = (uint64_t)llround(beat * DM_PPQ);
    r.placed = r.tick;
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
    /* A song's own program change at beat 0 already plays the channel's program
       (collect_source_events), after any bank select there; the setup does not send it twice. */
    bool song_program[16] = {false};

    for (size_t i = 0; i < p->event_count && p->events[i].beat == 0; ++i) {
        const DmBytes *b = &p->events[i].message;

        if (b->size == 2 && (b->data[0] & MIDI_STATUS_MASK) == MIDI_PROGRAM) {
            song_program[b->data[0] & MIDI_CHANNEL_MASK] = true;
        }
    }

    for (int c = 0; c < 16; ++c) {
        if (p->channels[c].enabled) {
            uint8_t program[] = {
                MIDI_PROGRAM | c,
                (uint8_t)(p->channels[c].program < 0 ? 0 : p->channels[c].program)};
            uint8_t vol[] = {MIDI_CONTROL | c, MIDI_CC_VOLUME, (uint8_t)p->channels[c].volume},
                    pan[] = {MIDI_CONTROL | c, MIDI_CC_PAN, (uint8_t)p->channels[c].pan};

            if ((!song_program[c] && !dm_midi_add_message(m, 0, c + 1, program, 2)) ||
                !dm_midi_add_message(m, 0, c + 1, vol, 3) ||
                !dm_midi_add_message(m, 0, c + 1, pan, 3)) {
                return false;
            }

            m->items[m->count - 1].generated_default = p->channels[c].pan == 64;
        }
    }

    return true;
}

static bool sysex_reset(const DmBytes *message);

/* A SysEx reset before the song's first note is part of the song's setup: it is sent at beat 0
   with the opening SysEx, before the channel setup it would otherwise wipe (opening volume, pan
   and program are sent at beat 0 too). */
static bool opening_reset(const DmEvent *e, double first_note) {
    return e->beat > 0 && e->beat < first_note && sysex_reset(&e->message);
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
    double first_note = dm_first_note(p, -1);

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        const uint8_t *b = e->message.data;

        if (skip_source_event(b, e->beat) || opening_reset(e, first_note)) {
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

/* Converts a pattern beat to a song beat. Controls before a placement's offset are sent at its
   start, in order, so the placement begins with the pattern's state at that point. */
static double clip_time(const DmClip *clip, double beat) {
    return clip->beat + fmax(0, beat - clip->offset);
}

/* Raw.source: where a pattern message sits in its pattern, so messages a slice moves onto its
   first tick keep the pattern's order. */
static uint64_t pattern_tick(double beat) {
    return (uint64_t)llround(fmax(0, beat) * DM_PPQ);
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

    if (!dm_midi_add_message(m, clip_time(clip, beat), clip->channel + 1, message,
                             (a.type & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3)) {
        return false;
    }

    m->items[m->count - 1].source = pattern_tick(beat);
    return true;
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
    uint64_t placed = (uint64_t)llround(clip_time(clip, a.beat) * DM_PPQ);

    for (int step = 1; step < steps; ++step) {
        double t = (double)step / steps, value = dm_curve_value(a, b, t);
        int wheel =
            rebase && a.type == MIDI_PITCH_BEND ? rebase_wheel(rebase, value) : (int)llround(value);

        if (!curve_sample(m, clip, a, a.beat + (b.beat - a.beat) * t, wheel)) {
            return false;
        }

        m->items[m->count - 1].placed = placed;
    }

    return true;
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
    m->items[m->count - 1].source = pattern_tick(a.beat);

    if (!dm_midi_add_message(m, clip_time(clip, end), clip->channel + 1, off, 3)) {
        return false;
    }

    m->items[m->count - 1].phase = 2;
    m->items[m->count - 1].source = pattern_tick(end);
    return true;
}

/* A graph lane's last played bar lasts one cell; then the lane returns to its default, as a Point
   section's restoration does, instead of holding for the rest of the song. Earlier bars hold
   until the next bar. A lane set to hold (graph_hold) keeps its last bar until the placement
   ends, where ownership releases it. */
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
            pat->graph_lanes[lane] <= DM_GRAPH_POINT || pat->graph_hold[lane] || a.curve ||
            a.port >= DM_NOTE_PORTS || seen[a.port][lane / 8] & (1u << lane % 8)) {
            continue;
        }

        seen[a.port][lane / 8] |= (uint8_t)(1u << lane % 8);
        double cell = 4.0 / pat->graph_lanes[lane];

        /* A release: the owner gives up the lane here (phase 2), so the last bar's value is only
           a placeholder that nothing plays. */
        int last = a.type == MIDI_PITCH_BEND ? a.a + (a.b << 7) : a.b;

        if (!curve_sample(m, clip, a, fmin(a.beat + cell, end), last)) {
            return false;
        }

        m->items[m->count - 1].placed = (uint64_t)llround(clip_time(clip, a.beat) * DM_PPQ);
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
            Raw *raw = &m->items[m->count - 1];
            raw->phase = dm_automation_phase(p, a);
            raw->restores = a.curve < 0;
            raw->line = abs(a.curve);
            raw->source = pattern_tick(a.beat);
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

/* Resolve modular automation once for both MIDI export and audible schedules. */
typedef struct {
    uint64_t tick, placed;
    int key, owner, value, priority, row;
    bool active;
    double start;
    size_t order;
    size_t source_index;
    bool generated_default;
    bool implicit; /* A bend range captured because a bend arrived without its own range. */
    bool yields;   /* A pedal section's pedal-up: any newer message for the key beats it. */
} ModEvent;

typedef struct {
    ModEvent *items;
    size_t count, capacity, baseline_count;
} ModList;

enum { MOD_PROGRAM = 258 };

/* Keys: a channel's controllers, bend, pressures and program are channel * MOD_CONTROLS +
   control; an RPN or NRPN parameter is MOD_PARAMS + channel * MOD_PARAM_CHANNEL + (NRPN ?
   16384 : 0) + MSB * 128 + LSB, a setting of its own. */
enum { MOD_CONTROLS = 260, MOD_PARAMS = 16 * MOD_CONTROLS, MOD_PARAM_CHANNEL = 32768 };

static bool mod_param(int key) {
    return key >= MOD_PARAMS;
}

static int mod_channel(int key) {
    return mod_param(key) ? (key - MOD_PARAMS) / MOD_PARAM_CHANNEL : key / MOD_CONTROLS;
}

/* The controller, bend (128), pressure or program of a key; -1 for a parameter. */
static int mod_control(int key) {
    return mod_param(key) ? -1 : key % MOD_CONTROLS;
}

static int mod_param_key(int channel, bool nrpn, int msb, int lsb) {
    return MOD_PARAMS + channel * MOD_PARAM_CHANNEL + (nrpn ? 16384 : 0) + msb * 128 + lsb;
}

/* Pitch bend range: RPN 0/0. */
static int mod_range_key(int channel) {
    return mod_param_key(channel, false, 0, 0);
}

static bool mod_range(int key) {
    return mod_param(key) && key == mod_range_key(mod_channel(key));
}

/* Channel mode messages (all sound off, reset all controllers, all notes off, mono and poly mode,
   ...) are commands, not settings: they are never owned or de-duplicated, and always sent. */
static bool mod_command(const uint8_t *b) {
    return (b[0] & MIDI_STATUS_MASK) == MIDI_CONTROL && b[1] >= MIDI_CC_SOUND_OFF;
}

/* A GS reset, XG System On, or GM/GM2 System On: the synth puts its parts back to defaults. */
static bool sysex_reset(const DmBytes *message) {
    if (message->size < 3 || message->data[0] != MIDI_SYSEX || message->data[1] & 0x80) {
        return false;
    }

    const uint8_t *d = message->data + 2; /* After the status and a one-byte length. */
    size_t n = message->size - 2;
    bool gs = n >= 7 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12 && d[4] == 0x40 && !d[5] &&
              d[6] == 0x7f;
    bool xg = n >= 6 && d[0] == 0x43 && (d[1] & 0xf0) == 0x10 && d[2] == 0x4c && !d[3] && !d[4] &&
              d[5] == 0x7e;
    bool gm = n >= 4 && d[0] == 0x7e && d[2] == 0x09 && (d[3] == 1 || d[3] == 3);
    return gs || xg || gm;
}

static int mod_key(const uint8_t *b) {
    int type = b[0] & MIDI_STATUS_MASK, control = -1;

    if (mod_command(b)) {
        return -1;
    }

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

    return control < 0 ? -1 : (b[0] & MIDI_CHANNEL_MASK) * MOD_CONTROLS + control;
}

static int mod_value(const uint8_t *b) {
    int type = b[0] & MIDI_STATUS_MASK;
    return type == MIDI_PITCH_BEND                                   ? b[1] + (b[2] << 7)
           : (type == MIDI_PROGRAM || type == MIDI_CHANNEL_PRESSURE) ? b[1]
                                                                     : b[2];
}

static bool mod_patch_key(int key) {
    int cc = mod_control(key);
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

/* The value a setting falls back to when nothing plays it, or -1 when there is none to send (an
   RPN or NRPN whose default the synth decides keeps its last value). */
static int mod_default(const DmProject *p, int key) {
    int cc = mod_control(key), c = mod_channel(key);

    if (mod_param(key)) {
        int number = (key - MOD_PARAMS) % MOD_PARAM_CHANNEL;
        /* GM: bend range 2 semitones (RPN 0/0), fine and coarse tuning centred (0/1, 0/2). */
        return number == 0 ? 2 * 128 : number == 1 || number == 2 ? 64 * 128 : -1;
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

/* A parameter value as one complete message group: select, data entry (with its fine part for the
   bend range or when it has one), then the null select, so no later data entry lands on it. */
static bool mod_param_emit(Midi *m, ModEvent e, int value) {
    int number = (e.key - MOD_PARAMS) % MOD_PARAM_CHANNEL, c = mod_channel(e.key);
    bool nrpn = number >= 16384, fine = mod_range(e.key) || value % 128;
    int msb_cc = nrpn ? MIDI_CC_NRPN_MSB : MIDI_CC_RPN_MSB,
        lsb_cc = nrpn ? MIDI_CC_NRPN_LSB : MIDI_CC_RPN_LSB;
    const int controls[] = {msb_cc,           lsb_cc,          MIDI_CC_DATA,
                            MIDI_CC_DATA_LSB, MIDI_CC_RPN_MSB, MIDI_CC_RPN_LSB};
    const int values[] = {(number >> 7) & 127, number & 127, value / 128, value % 128, 127, 127};

    for (int i = 0; i < 6; ++i) {
        if (i == 3 && !fine) {
            continue;
        }

        uint8_t bytes[] = {MIDI_CONTROL | c, controls[i], values[i]};

        if (!dm_midi_add_message(m, (double)e.tick / DM_PPQ, c + 1, bytes, 3)) {
            return false;
        }

        m->items[m->count - 1].phase = 3;
    }

    return true;
}

static bool mod_emit(Midi *m, ModEvent e, int value) {
    int cc = mod_control(e.key), c = mod_channel(e.key), type = MIDI_CONTROL, a = cc, b = value;

    if (mod_param(e.key)) {
        return mod_param_emit(m, e, value);
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

/* Highest priority wins. Among automation patterns (priority 1), the most recently placed
   point wins, then the higher row (smaller lane number), then later start, then higher owner. */
static bool mod_beats(const ModEvent *a, const ModEvent *b) {
    /* A pedal section's pedal-up lasts only until the next pedal message from anything, even a
       lower-ranked owner such as a Channel Event; against older values the usual order holds. */
    if (a->yields && b->tick > a->tick) {
        return false;
    }

    if (b->yields && a->tick > b->tick) {
        return true;
    }

    if (a->priority != b->priority) {
        return a->priority > b->priority;
    }

    if (a->priority == 1) {
        if (a->placed != b->placed) {
            return a->placed > b->placed;
        }

        if (a->row != b->row) {
            return a->row < b->row;
        }
    }

    if (a->start != b->start) {
        return a->start > b->start;
    }

    return a->owner > b->owner;
}

static ModEvent mod_winner(ModEvent *states, size_t used, ModEvent fallback) {
    ModEvent *winner = NULL;

    for (size_t k = 0; k < used; ++k) {
        ModEvent *v = &states[k];

        if (!v->active) {
            continue;
        }

        if (!winner || mod_beats(v, winner)) {
            winner = v;
        }
    }

    return winner ? *winner : fallback;
}

/* A point after which the next value the song sends is sent even when it repeats: a reset the
   synth applies (Reset All Controllers on channel, a SysEx reset on every channel), or the loop
   start (channel -1 too), where a looped player jumps back to values the song re-sends. */
typedef struct {
    uint64_t tick;
    int channel;
    bool controllers; /* Reset All Controllers: only the settings it resets. */
} ModForget;

/* Whether Reset All Controllers resets key: not volume, pan, bank, program, effect depths or
   sound controllers (MIDI RP-015). */
static bool reset_controller(int key) {
    int cc = mod_control(key); /* It resets the parameter selection, not parameters (-1). */
    return !(cc < 0 || cc == MIDI_CC_VOLUME || cc == MIDI_CC_PAN || cc == MIDI_CC_BANK ||
             cc == MIDI_CC_BANK_LSB || cc == MOD_PROGRAM || (cc >= 70 && cc <= 79) ||
             (cc >= 91 && cc <= 95));
}

static int forget_compare(const void *a, const void *b) {
    const ModForget *x = a, *y = b;
    return x->tick < y->tick ? -1 : x->tick > y->tick;
}

/* The owner of a leading setting from tick on: the bend leads the bend range, the program leads
   the bank. owner is -2 while nobody plays the setting. */
typedef struct {
    uint64_t tick;
    int owner;
} ModLead;

/* The setting a follower key goes with, or -1: a bend range goes with its channel's bend, a bank
   (MSB or LSB) with its program. */
static int mod_leader_key(int key) {
    int cc = mod_control(key), c = mod_channel(key);
    return mod_range(key)                                 ? c * MOD_CONTROLS + 128
           : cc == MIDI_CC_BANK || cc == MIDI_CC_BANK_LSB ? c * MOD_CONTROLS + MOD_PROGRAM
                                                          : -1;
}

/* The winners of one key over time, as mod_resolve_key picks them. */
static bool mod_leads(const ModEvent *events, size_t count, ModLead **out, size_t *lead_count) {
    ModEvent *states = calloc(count + 1, sizeof(*states));
    ModLead *leads = malloc((count + 1) * sizeof(*leads));
    *out = NULL;
    *lead_count = 0;

    if (!states || !leads) {
        free(states);
        free(leads);
        return false;
    }

    size_t used = 0, n = 0;
    ModEvent none = {.owner = -2, .active = false};

    for (size_t i = 0; i < count;) {
        uint64_t tick = events[i].tick;

        do {
            ModEvent e = events[i++];
            size_t k = 0;

            while (k < used && states[k].owner != e.owner) {
                ++k;
            }

            used += k == used;
            states[k] = e;
        } while (i < count && events[i].tick == tick);

        int owner = mod_winner(states, used, none).owner;

        if (!n || leads[n - 1].owner != owner) {
            leads[n++] = (ModLead){tick, owner};
        }
    }

    free(states);
    *out = leads;
    *lead_count = n;
    return true;
}

/* The leader's owner at tick, or -2. */
static int mod_lead_at(const ModLead *leads, size_t count, uint64_t tick) {
    size_t low = 0, high = count;

    while (low < high) {
        size_t mid = low + (high - low) / 2;

        if (leads[mid].tick <= tick) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    return low ? leads[low - 1].owner : -2;
}

/* last_sound holds each channel's last note-off tick in this route (UINT64_MAX for a channel
   without notes). A release to the song's own value or the default (the baseline, owner -1,
   taking over because every placement owning the controller ended) at or after it is not
   written: no note hears it, and a plain import would otherwise gain a reset per controller at
   its end. Control passing to another playing placement is always written, and so are pedal
   releases, since a held pedal keeps notes sounding. */
/* With leads (the leading setting's owners over time), the follower plays the lead owner's own
   value while it has one (an implicit bend range does not count); otherwise the usual winner. */
static bool mod_resolve_key(const DmProject *p, Midi *m, const ModEvent *events, size_t count,
                            const uint64_t last_sound[16], const ModLead *leads, size_t lead_count,
                            const ModForget *forget, size_t forget_count) {
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
        authored |= mod_control(events[i].key) == MIDI_CC_PAN && !events[i].generated_default;
    }

    size_t li = 0, fi = 0;
    bool forgotten = false;
    int channel = mod_channel(events[0].key);

    for (size_t i = 0; (i < count || li < lead_count) && ok;) {
        size_t first = i;
        /* A change of the lead's owner is a reason to choose again, with or without a message of
           this key at that tick. */
        uint64_t next_event = i < count ? events[i].tick : UINT64_MAX;
        uint64_t next_lead = li < lead_count ? leads[li].tick : UINT64_MAX;
        uint64_t tick = next_event < next_lead ? next_event : next_lead;

        while (li < lead_count && leads[li].tick <= tick) {
            ++li;
        }

        while (i < count && events[i].tick == tick) {
            ModEvent e = events[i++];
            size_t k = 0;

            while (k < used && states[k].owner != e.owner) {
                ++k;
            }

            if (k == used) {
                ++used;
            }

            states[k] = e;
        }

        bool lead_only = first == i;

        for (; fi < forget_count && forget[fi].tick <= tick; ++fi) {
            forgotten |= (forget[fi].channel < 0 || forget[fi].channel == channel) &&
                         (!forget[fi].controllers || reset_controller(events[0].key));
        }

        if (!used) {
            continue; /* Nothing of this key has been placed yet. */
        }

        ModEvent fallback = events[0];
        fallback.owner = -1;
        fallback.active = false;
        fallback.value = mod_default(p, events[0].key);
        ModEvent chosen = mod_winner(states, used, fallback);
        int lead = leads ? mod_lead_at(leads, lead_count, tick) : -2;

        if (leads) {
            /* The lead's own value; else, when the lead has none of its own, the usual winner
               among owners that set one (an implicit bend range is not one). */
            const ModEvent *own = NULL, *best = NULL;

            for (size_t k = 0; k < used; ++k) {
                const ModEvent *v = &states[k];

                if (!v->active || v->implicit) {
                    continue;
                }

                if (lead != -2 && v->owner == lead) {
                    own = v;
                }

                if (!best || mod_beats(v, best)) {
                    best = v;
                }
            }

            if (own || best) {
                chosen = own ? *own : *best;
            }
        }

        /* Kept by its lead (a losing pattern's message at this tick, or the lead changing hands)
           with nothing new to send. A song message at this tick still plays as written, so the
           winner is then sent again after it, as below. */
        bool song_message = false;

        for (size_t j = first; j < i; ++j) {
            song_message |= events[j].owner < 0;
        }

        /* After a reset or the loop start, a value the song sends again is sent again. */
        bool resend = forgotten && chosen.active && chosen.tick == tick;
        bool kept = chosen.owner == last.owner && chosen.value == last.value &&
                    chosen.tick != tick && !song_message && !resend;

        if ((lead_only || (leads && chosen.owner == lead)) && kept) {
            continue;
        }

        chosen.generated_default &= !authored;
        bool authored_tick = chosen.tick == tick;
        bool patch_selection =
            patch && ((chosen.active && authored_tick) || chosen.owner != last.owner);
        chosen.tick = tick;
        /* Reassert the winning transaction when source parameters change underneath it. An
           implicit range (an owner's bends without a range of their own) is written only when it
           differs from the range in effect, which starts at the device default. */
        bool range_key = mod_range(chosen.key);
        int range_now = last.value < 0 ? mod_default(p, chosen.key) : last.value;
        bool range_unchanged = range_key && chosen.implicit && chosen.value == range_now;
        bool range_changed = range_key && !range_unchanged;
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

        int cc = mod_control(chosen.key);
        bool silent = chosen.owner < 0 && !(chosen.active && authored_tick) &&
                      tick >= last_sound[mod_channel(chosen.key)] && cc != MIDI_CC_SUSTAIN &&
                      cc != MIDI_CC_SOSTENUTO;
        /* A parameter with no default to fall back to keeps its last value. */
        silent |= chosen.value < 0;

        if (!forwarded && !silent && !range_unchanged &&
            (chosen.value != last.value || range_changed || patch_selection || resend)) {
            ok = mod_emit(m, chosen, chosen.value);
            forgotten = false;
        }

        last = chosen;
    }

    free(states);
    return ok;
}

/* A channel's parameter state in one owner's messages, in their order: the selected RPN or NRPN
   (msb = lsb = 127 is none, the null select) and the bend range in effect (RPN 0/0, semitones *
   128 + cents), which the owner's bends play at when it sets no range of its own. */
typedef struct {
    bool nrpn;
    int msb, lsb, range;
    bool owned;         /* The owner has a bend range of its own, or one was taken for its bends. */
    size_t pending[16]; /* Selects since the last data entry (indices into the capture's source). */
    size_t pending_count;
} ModParams;

static bool mod_param_message(const uint8_t *b) {
    int cc = b[1];
    return (b[0] & MIDI_STATUS_MASK) == MIDI_CONTROL &&
           (cc == MIDI_CC_DATA || cc == MIDI_CC_DATA_LSB ||
            (cc >= MIDI_CC_INCREMENT && cc <= MIDI_CC_RPN_MSB));
}

static void mod_param_select(ModParams *state, int cc, int value) {
    bool nrpn = cc == MIDI_CC_NRPN_MSB || cc == MIDI_CC_NRPN_LSB;

    if (nrpn != state->nrpn) {
        state->nrpn = nrpn;
        state->msb = state->lsb = 127;
    }

    if (cc == MIDI_CC_RPN_MSB || cc == MIDI_CC_NRPN_MSB) {
        state->msb = value;
    } else {
        state->lsb = value;
    }
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

static void mod_initial_params(const ModList *list, ModParams *states, double start) {
    for (int c = 0; c < 16; ++c) {
        states[c] = (ModParams){.msb = 127, .lsb = 127, .range = 2 * 128};
    }

    uint64_t tick = (uint64_t)llround(start * DM_PPQ);

    for (size_t i = 0; i < list->baseline_count; ++i) {
        ModEvent e = list->items[i];

        if (mod_range(e.key) && e.tick <= tick) {
            states[mod_channel(e.key)].range = e.value;
        }
    }
}

/* The owner's last value for key among this capture's events (from first on), or -1. */
static int mod_param_known(const ModList *list, size_t first, int key, uint64_t boundary) {
    for (size_t i = list->count; i-- > first;) {
        const ModEvent *e = &list->items[i];

        if (e->key == key && e->tick < boundary) {
            return e->active ? e->value : -1;
        }
    }

    return -1;
}

/* The value a data entry, its fine part, an increment or a decrement gives the selected parameter
   (current is its value in effect, -1 when unknown), or -1 when it cannot be known. */
static int mod_param_value(int cc, int data, int current) {
    switch (cc) {
    case MIDI_CC_DATA:
        return data * 128 + (current >= 0 ? current % 128 : 0);
    case MIDI_CC_DATA_LSB:
        return current >= 0 ? current / 128 * 128 + data : -1;
    case MIDI_CC_INCREMENT:
        return current >= 0 ? (current / 128 < 127 ? current + 128 : current) : -1;
    case MIDI_CC_DECREMENT:
        return current >= 0 ? (current / 128 > 0 ? current - 128 : current) : -1;
    default:
        return -1;
    }
}

/* A parameter message duckmidi cannot resolve to a value plays as written: a data entry with
   nothing selected, or an increment of a parameter whose value is not known (sent with its
   select, then the null select). */
static bool mod_param_pass(Midi *passed, const Raw *r, const ModParams *state) {
    int c = r->bytes.data[0] & MIDI_CHANNEL_MASK;
    bool selected = !(state->msb == 127 && state->lsb == 127);
    int msb_cc = state->nrpn ? MIDI_CC_NRPN_MSB : MIDI_CC_RPN_MSB,
        lsb_cc = state->nrpn ? MIDI_CC_NRPN_LSB : MIDI_CC_RPN_LSB;
    const uint8_t group[][3] = {{MIDI_CONTROL | c, msb_cc, state->msb},
                                {MIDI_CONTROL | c, lsb_cc, state->lsb},
                                {r->bytes.data[0], r->bytes.data[1], r->bytes.data[2]},
                                {MIDI_CONTROL | c, MIDI_CC_RPN_MSB, 127},
                                {MIDI_CONTROL | c, MIDI_CC_RPN_LSB, 127}};
    double beat = (double)r->tick / DM_PPQ;

    for (int i = selected ? 0 : 2; i < (selected ? 5 : 3); ++i) {
        if (!dm_midi_add_message(passed, beat, c + 1, group[i], 3)) {
            return false;
        }

        passed->items[passed->count - 1].phase = 3;
    }

    return true;
}

/* The owner's value for key in effect before its section started: its latest event among the
   events this capture added before index limit (from first on). False when that event released
   the controller (an earlier restoration that released, a Graph return) or there is none. Events
   at the boundary are the placement's end markers, not values. */
static bool mod_before(const ModList *list, size_t first, size_t limit, int key, uint64_t boundary,
                       ModEvent *out) {
    for (size_t i = limit; i-- > first;) {
        const ModEvent *e = &list->items[i];

        if (e->key == key && e->tick < boundary) {
            *out = *e;
            return e->active;
        }
    }

    return false;
}

/* Within one placement, messages on the same tick (all those a slice moves onto its first tick)
   keep the order they have in the pattern. */
static int mod_capture_compare(const void *a, const void *b) {
    const Raw *x = a, *y = b;

    if (x->tick != y->tick) {
        return x->tick < y->tick ? -1 : 1;
    }

    if (x->source != y->source) {
        return x->source < y->source ? -1 : 1;
    }

    return dm_midi_export_compare(a, b);
}

/* A Line's first message seen by mod_capture: how many events the capture had then, and its
   tick, which its restoration's events carry as placed. */
typedef struct {
    int line;
    size_t index;
    uint64_t placed;
} ModLine;

/* Captures source's messages as owner's events. A data entry or step becomes a value of the
   parameter its owner has selected, sent later as a whole message group with its select, so the
   selects before it are not sent again; one that cannot be resolved goes to out as written. A
   select no data entry follows plays as written, unless it leaves nothing selected (every group
   already ends deselected). The song's own messages (owner < 0, source is out) that are sent
   again are taken out of out. */
static bool mod_capture(ModList *list, Midi *source, Midi *out, int owner, int priority, int row,
                        double start, double end) {
    uint64_t boundary = (uint64_t)llround(end * DM_PPQ);
    size_t first = list->count;
    ModParams states[16];
    mod_initial_params(list, states, start);
    Midi passed = {0};
    qsort(source->items, source->count, sizeof(*source->items), mod_capture_compare);
    ModLine *lines = NULL;
    size_t line_count = 0, line_capacity = 0;
    bool ok = true;

    for (size_t j = 0; ok && j < source->count; ++j) {
        Raw r = source->items[j];

        if (r.tick > boundary || !r.bytes.size) { /* A command already passed through. */
            continue;
        }

        /* A restoration returns to what the owner played before its Line's first message, in
           this capture's order, and ranks by that message's tick. */
        size_t limit = list->count;

        if (r.line > 0) {
            size_t at = 0;

            while (at < line_count && lines[at].line != r.line) {
                ++at;
            }

            if (at < line_count && r.restores) {
                limit = lines[at].index;
                r.placed = lines[at].placed;
            } else if (at == line_count && !r.restores) {
                if (!dm_reserve((void **)&lines, &line_capacity, line_count + 1, sizeof(*lines))) {
                    ok = false;
                    break;
                }

                lines[line_count++] = (ModLine){r.line, list->count, r.placed};
            }
        }

        const uint8_t *bytes = r.bytes.data;
        int c = bytes[0] & MIDI_CHANNEL_MASK;
        ModParams *state = &states[c];
        /* Phase 2 is a release (a Graph return, a glide gate closing): the owner gives up the
           controller. A restoration ends a section: it returns to the owner's value from before
           the section and keeps owning the controller, or releases it when the owner had no value
           before. A pedal section always ends with the pedal up: its restoration keeps its own
           value (0). */
        bool pedal = (bytes[0] & MIDI_STATUS_MASK) == MIDI_CONTROL &&
                     (bytes[1] == MIDI_CC_SUSTAIN || bytes[1] == MIDI_CC_SOSTENUTO);
        bool restoration = r.restores && owner >= 0 && !pedal;
        bool active = (r.phase != 2 || r.restores) && (owner < 0 || r.tick < boundary);
        ModEvent range = {0};
        bool range_before =
            restoration && mod_before(list, first, limit, mod_range_key(c), boundary, &range);

        if (mod_param_message(bytes)) {
            int cc = bytes[1];

            if (cc >= MIDI_CC_NRPN_LSB) {
                mod_param_select(state, cc, bytes[2]);

                if (state->pending_count < 16) {
                    state->pending[state->pending_count++] = j;
                }

                continue;
            }

            /* This data entry's group carries its select; the song's own copy is sent as that. */
            for (size_t k = 0; owner < 0 && k < state->pending_count; ++k) {
                dm_bytes_free(&source->items[state->pending[k]].bytes);
            }

            state->pending_count = 0;

            if (owner < 0) {
                dm_bytes_free(&source->items[j].bytes);
            }

            bool selected = !(state->msb == 127 && state->lsb == 127);
            int pkey = selected ? mod_param_key(c, state->nrpn, state->msb, state->lsb) : -1;
            int current = pkey < 0          ? -1
                          : mod_range(pkey) ? state->range
                                            : mod_param_known(list, first, pkey, boundary);
            int value = pkey < 0 ? -1 : mod_param_value(cc, bytes[2], current);

            if (value < 0) {
                ok = mod_param_pass(&passed, &r, state);
                continue;
            }

            /* A Line's range restoration returns to the owner's range from before the section. */
            bool returns = restoration && mod_range(pkey);

            if (mod_range(pkey)) {
                state->range = returns && range_before ? range.value : value;
                state->owned = true;
            }

            ModEvent e = {.tick = r.tick,
                          .placed = r.placed,
                          .key = pkey,
                          .owner = owner,
                          .value = returns && range_before ? range.value : value,
                          .priority = priority,
                          .row = row,
                          .active = active && (!returns || range_before),
                          .start = start,
                          .source_index = j};
            ok = mod_capture_event(list, e, boundary);
            continue;
        }

        int key = mod_key(bytes);

        if (!ok || key < 0) {
            continue;
        }

        if (mod_control(key) == 128 && !state->owned) {
            /* Bends without a range of their own play at the range in effect (implicit). */
            state->owned = true;
            ModEvent e = {.tick = r.tick,
                          .placed = r.placed,
                          .key = mod_range_key(c),
                          .owner = owner,
                          .value = range_before ? range.value : state->range,
                          .priority = priority,
                          .row = row,
                          .active = active && (!restoration || range_before),
                          .start = start,
                          .source_index = j,
                          .implicit = true};
            ok = mod_capture_event(list, e, boundary);
        }

        ModEvent previous = {0};
        bool before = restoration && mod_before(list, first, limit, key, boundary, &previous);
        ModEvent e = {.tick = r.tick,
                      .placed = r.placed,
                      .key = key,
                      .owner = owner,
                      .value = before ? previous.value : mod_value(bytes),
                      .priority = priority,
                      .row = row,
                      .active = active && (!restoration || before),
                      .start = start,
                      .source_index = j,
                      .generated_default = r.generated_default,
                      .yields = r.restores && pedal && owner >= 0};
        ok = ok && mod_capture_event(list, e, boundary);
    }

    /* Selects no data entry followed. */
    for (int c = 0; ok && c < 16; ++c) {
        const ModParams *state = &states[c];
        bool none = state->msb == 127 && state->lsb == 127;

        for (size_t k = 0; ok && k < state->pending_count; ++k) {
            Raw *r = &source->items[state->pending[k]];

            if (owner < 0 && none) {
                dm_bytes_free(&r->bytes);
            } else if (owner >= 0 && !none) {
                ok = dm_midi_add_message(&passed, (double)r->tick / DM_PPQ, c + 1, r->bytes.data,
                                         r->bytes.size);
            }
        }
    }

    for (size_t i = 0; ok && i < passed.count; ++i) {
        ok = dm_midi_raw_add(out, &passed.items[i]);
        passed.items[i].bytes = (DmBytes){0};
    }

    dm_midi_free(&passed);
    free(lines);
    return ok;
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

    uint64_t placed = (uint64_t)llround(clip->beat * DM_PPQ);

    for (int i = 0; i < steps; ++i) {
        double beat = clip->offset + length * i / steps;
        int value = dm_lfo_value(pat, beat);

        if (value != last) {
            if (!curve_sample(m, clip, a, beat, value)) {
                return false;
            }

            m->items[m->count - 1].placed = placed;
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
    uint64_t boundary = (uint64_t)llround((clip->beat + dm_clip_length(p, *clip)) * DM_PPQ);

    /* Commands play as written while the placement plays. */
    for (size_t i = 0; ok && i < temp.count; ++i) {
        Raw *r = &temp.items[i];

        if (r->bytes.size && mod_command(r->bytes.data) && r->tick <= boundary) {
            ok = dm_midi_raw_add(m, r);
            r->bytes = (DmBytes){0};
        }
    }
    /* Note patterns outrank automation patterns; among automation patterns the newest placed
       point wins, then the higher row (mod_beats). */
    ok = ok && mod_capture(list, &temp, m, (int)index, !pat->kind ? 2 : 1, clip->lane, clip->beat,
                           clip->beat + dm_clip_length(p, *clip));
    dm_midi_free(&temp);

    if (!ok) {
        return false;
    }

    return collect_clip_notes(p, m, clip);
}

/* Every placement owns its pattern's values until it ends (one rule set): the baseline is the
   song's own messages and channel setup, captured first; each placement's values then compete
   with it and with each other per controller (mod_winner). */
static bool collect_clips(const DmProject *p, Midi *m, bool mixer) {
    ModList list = {0};
    bool ok = mod_capture(&list, m, m, -1, 0, 0, 0, 1e12);
    list.baseline_count = list.count;

    for (size_t i = 0; i < m->count; ++i) {
        int key = m->items[i].bytes.size ? mod_key(m->items[i].bytes.data) : -1;

        if (key >= 0 && !mod_patch_key(key)) {
            dm_bytes_free(&m->items[i].bytes);
        }
    }

    for (size_t i = 0; i < p->clip_count && ok; ++i) {
        if (!mixer || !muted(p, p->clips[i].channel)) {
            ok = collect_mod_clip(p, m, &list, i);
        }
    }

    /* A channel without notes never drops a release: it may drive external MIDI gear. */
    uint64_t last_sound[16];

    for (int c = 0; c < 16; ++c) {
        last_sound[c] = UINT64_MAX;
    }

    for (size_t i = 0; i < m->count; ++i) {
        const DmBytes *b = &m->items[i].bytes;
        int type = b->size ? b->data[0] & MIDI_STATUS_MASK : 0;

        if (type == MIDI_NOTE_OFF || (type == MIDI_NOTE_ON && b->size > 2 && !b->data[2])) {
            int c = b->data[0] & MIDI_CHANNEL_MASK;
            last_sound[c] = last_sound[c] == UINT64_MAX || m->items[i].tick > last_sound[c]
                                ? m->items[i].tick
                                : last_sound[c];
        }
    }

    ModForget *forget = malloc((m->count + 2) * sizeof(*forget));
    size_t forget_count = 0;
    double loop_start = -1, loop_end = -1;
    ok = ok && forget;

    /* Nothing is sent before beat 0, so a reset or loop start there forgets nothing. */
    for (size_t i = 0; ok && i < m->count; ++i) {
        const DmBytes *b = &m->items[i].bytes;

        if (!m->items[i].tick) {
            continue;
        }

        if (b->size >= 2 && (b->data[0] & MIDI_STATUS_MASK) == MIDI_CONTROL &&
            b->data[1] == MIDI_CC_RESET) {
            forget[forget_count++] =
                (ModForget){m->items[i].tick, b->data[0] & MIDI_CHANNEL_MASK, true};
        } else if (sysex_reset(b)) {
            forget[forget_count++] = (ModForget){m->items[i].tick, -1, false};
        }
    }

    if (ok && dm_loop_points(p, &loop_start, &loop_end) && loop_start > 0) {
        forget[forget_count++] = (ModForget){(uint64_t)llround(loop_start * DM_PPQ), -1, false};
    }

    if (ok) {
        qsort(forget, forget_count, sizeof(*forget), forget_compare);
    }

    qsort(list.items, list.count, sizeof(*list.items), mod_compare);

    for (size_t i = 0; i < list.count && ok;) {
        size_t end = i + 1;

        while (end < list.count && list.items[end].key == list.items[i].key) {
            ++end;
        }

        /* A follower (bend range, bank) goes with whoever plays its leading setting. */
        int leader = mod_leader_key(list.items[i].key);
        ModLead *leads = NULL;
        size_t lead_count = 0;

        if (leader >= 0) {
            size_t low = 0, high = list.count;

            while (low < high) {
                size_t mid = low + (high - low) / 2;

                if (list.items[mid].key < leader) {
                    low = mid + 1;
                } else {
                    high = mid;
                }
            }

            size_t lead_end = low;

            while (lead_end < list.count && list.items[lead_end].key == leader) {
                ++lead_end;
            }

            ok =
                lead_end == low || mod_leads(list.items + low, lead_end - low, &leads, &lead_count);
        }

        ok = ok && mod_resolve_key(p, m, list.items + i, end - i, last_sound, leads, lead_count,
                                   forget, forget_count);
        free(leads);
        i = end;
    }

    free(forget);
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
    double first_note = dm_first_note(p, -1);

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        uint8_t status = e->message.data[0];

        if ((e->beat == 0 && (status == MIDI_SYSEX || status == MIDI_SYSEX_END)) ||
            opening_reset(e, first_note)) {
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

        if (a.pattern >= 0 && (size_t)a.pattern < p->pattern_count && placed[a.pattern]) {
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

static bool export_midi(const DmProject *p, int channel, DmExportEvent **events, size_t *count,
                        uint64_t *duration, char *error, size_t cap) {
    *events = NULL;
    *count = 0;

    if (duration) {
        *duration = 0;
    }

    Midi m = {0};
    bool ok = dm_midi_collect(p, &m, true);
    DmExportEvent *out = ok && m.count ? calloc(m.count, sizeof(*out)) : NULL;

    if (m.count && !out) {
        ok = false;
    }

    if (ok) {
        qsort(m.items, m.count, sizeof(*m.items), dm_midi_export_compare);
        uint64_t end = (uint64_t)llround(dm_duration(p) * DM_PPQ);

        for (size_t i = 0; i < m.count; ++i) {
            Raw *r = &m.items[i];
            const uint8_t *b = r->bytes.data;

            if (channel >= 0 && b[0] < MIDI_SYSTEM && (b[0] & MIDI_CHANNEL_MASK) != channel) {
                continue;
            }

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

            if (e.tick > end) {
                end = e.tick;
            }
        }

        if (duration) {
            *duration = end;
        }
    }

    dm_midi_free(&m);

    if (!ok) {
        free(out);
        *count = 0;

        if (duration) {
            *duration = 0;
        }

        if (error && cap) {
            snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_EXPORT_TIMELINE);
        }

        return false;
    }

    *events = out;
    return true;
}

bool dm_export_timeline(const DmProject *p, DmExportEvent **events, size_t *count,
                        uint64_t *duration, char *error, size_t cap) {
    return export_midi(p, -1, events, count, duration, error, cap);
}

bool dm_export_channel(const DmProject *p, int channel, DmExportEvent **events, size_t *count,
                       char *error, size_t cap) {
    *events = NULL;
    *count = 0;

    if (!p || channel < 0 || channel >= 16) {
        if (error && cap) {
            snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_EXPORT_TIMELINE);
        }

        return false;
    }

    DmProject route = *p;
    route.clips = p->clip_count ? malloc(p->clip_count * sizeof(*route.clips)) : NULL;
    route.events = p->event_count ? malloc(p->event_count * sizeof(*route.events)) : NULL;

    if ((p->clip_count && !route.clips) || (p->event_count && !route.events)) {
        free(route.clips);
        free(route.events);

        if (error && cap) {
            snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_EXPORT_TIMELINE);
        }

        return false;
    }

    route.clip_count = 0;

    for (size_t i = 0; i < p->clip_count; ++i) {
        if (p->clips[i].channel == channel) {
            route.clips[route.clip_count++] = p->clips[i];
        }
    }

    route.event_count = 0;

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        const uint8_t *b = e->message.data;
        bool is_channel = e->message.size && b[0] < MIDI_SYSTEM;

        if (!is_channel || dm_conductor(&e->message) || (b[0] & MIDI_CHANNEL_MASK) == channel) {
            route.events[route.event_count++] = *e;
        }
    }

    bool ok = export_midi(&route, channel, events, count, NULL, error, cap);
    free(route.clips);
    free(route.events);
    return ok;
}
