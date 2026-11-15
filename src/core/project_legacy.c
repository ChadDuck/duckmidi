/* Frozen legacy reader for .duck project schemas 9 through 19. New fields belong in the tagged
   format instead; do not add new schema branches here. */
#include "core/project_internal.h"
#include <limits.h>
#include <math.h>
#include <string.h>

/* Fields added in schema 10 are absent from schema-9 documents; project_compat.c fills in their
   meaning after loading. */
static bool schema_10(const Input *in) {
    return in->schema >= 10;
}

/* The point hold flag was added in schema 11. */
static bool schema_11(const Input *in) {
    return in->schema >= 11;
}

/* The channel percussion flag was added in schema 12. */
static bool schema_12(const Input *in) {
    return in->schema >= 12;
}

/* The pattern graph resolution and the percussion MIDI mapping were added in schema 15. */
static bool schema_15(const Input *in) {
    return in->schema >= 15;
}

static uint32_t get32(Input *in) {
    if (in->schema < 10) {
        return dm_get_fixed32(in);
    }

    uint64_t n = dm_get_varint(in);

    if (n > UINT32_MAX) {
        in->ok = false;
        return 0;
    }

    return (uint32_t)n;
}

static double getreal(Input *in) {
    if (in->schema >= 10) {
        return dm_get_real10(in);
    }

    uint64_t bits = dm_get_fixed32(in);
    bits |= (uint64_t)dm_get_fixed32(in) << 32;
    double n;
    memcpy(&n, &bits, 8);

    if (!isfinite(n)) {
        in->ok = false;
    }

    return n;
}

static double get_time_after(Input *in, double previous) {
    if (in->schema < 10) {
        return getreal(in);
    }

    return dm_get_time_after10(in, previous);
}

/* Smallest possible size of a record, used to reject impossible counts before allocating. */
static size_t least(const Input *in, size_t fixed, size_t compact) {
    return in->schema >= 10 ? compact : fixed;
}

static bool getblock(Input *in, DmBytes *b) {
    uint32_t n = get32(in);

    if (!in->ok || n > in->left) {
        in->ok = false;
        return false;
    }

    if (!dm_bytes(b, in->p, n)) {
        in->ok = false;
        return false;
    }

    in->p += n;
    in->left -= n;
    return true;
}

static bool getstring(Input *in, char *s, size_t cap) {
    uint32_t n = get32(in);

    if (!in->ok || n >= cap || n > in->left || memchr(in->p, 0, n)) {
        in->ok = false;
        return false;
    }

    memcpy(s, in->p, n);
    s[n] = 0;
    in->p += n;
    in->left -= n;
    return true;
}

static bool unpack_note(DmPattern *pat, Input *in) {
    DmNote n = {0};
    n.beat = get_time_after(in, pat->count ? pat->notes[pat->count - 1].beat : 0);
    n.length = getreal(in);
    uint32_t key = get32(in), vel = get32(in), selected = get32(in), port = get32(in);

    if (!in->ok || !dm_note_fields_valid(key, vel, selected, port)) {
        return false;
    }

    n.key = (uint8_t)key;
    n.velocity = (uint8_t)vel;
    n.selected = (uint8_t)selected;
    n.port = (uint8_t)port;
    n.bend_id = in->schema >= 17 ? get32(in) : 0;

    if (!in->ok || !dm_note_add(pat, n)) {
        return false;
    }

    return in->ok;
}

static bool pattern_length_valid(const DmPattern *p, double length) {
    for (size_t i = 0; i < p->count; ++i) {
        if (p->notes[i].beat + p->notes[i].length > length + 1e-7) {
            return false;
        }
    }

    return true;
}

static bool unpack_lfo_center(DmPattern *p, Input *in) {
    p->center = getreal(in);
    p->pitch_center = getreal(in);
    p->reference = (int)get32(in);
    uint32_t inverted = get32(in);

    if (inverted > 1) {
        return false;
    }

    p->inverted = inverted != 0;
    return in->ok;
}

static bool unpack_graph_lanes(DmPattern *p, Input *in) {
    if (in->schema < 16) {
        uint32_t graph = schema_15(in) ? get32(in) : 0;

        if (!in->ok || !dm_graph_resolution_valid(graph) ||
            (graph && (p->mode || (p->kind && dm_point_only_event(p->event))))) {
            return false;
        }

        /* Pass the old pattern-wide value to upgrade_15_to_16. */
        p->graph_lanes[DM_GRAPH_PITCH_LANE] = (uint16_t)graph;
        return true;
    }

    uint32_t count = get32(in);

    if (count > DM_GRAPH_LANES) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t lane = get32(in), setting = get32(in);

        if (lane >= DM_GRAPH_LANES || p->graph_lanes[lane] || !setting || setting > UINT16_MAX) {
            return false;
        }

        p->graph_lanes[lane] = (uint16_t)setting;
    }

    return in->ok;
}

static bool unpack_pattern_mode(DmPattern *p, Input *in) {
    p->kind = (int)get32(in);
    p->mode = (int)get32(in);
    p->event = (int32_t)get32(in);
    p->speed = getreal(in);
    p->tension = getreal(in);
    p->skew = getreal(in);
    p->pulse = getreal(in);
    p->amount = getreal(in);

    if (!unpack_lfo_center(p, in)) {
        return false;
    }

    uint32_t control_scope = get32(in); /* Retired in schema 23 (project_compat.c). */

    if (control_scope > 2) {
        return false;
    }

    p->port = (int)get32(in);
    uint32_t color = schema_10(in) ? get32(in) : 0;

    if (color > DM_PATTERN_COLORS) {
        return false;
    }

    p->color = (uint8_t)color;
    return unpack_graph_lanes(p, in) && dm_pattern_settings_valid(p);
}

static bool unpack_pattern_notes(DmPattern *pat, Input *in, uint32_t notes) {
    for (uint32_t j = 0; j < notes; ++j) {
        if (!unpack_note(pat, in)) {
            return false;
        }
    }

    return true;
}

static bool unpack_pattern(DmProject *p, Input *in, uint32_t next_uid) {
    int id = dm_pattern_add(p, NULL);

    if (id < 0) {
        return false;
    }

    DmPattern *pat = &p->patterns[id];
    pat->uid = get32(in);

    if (!pat->uid || pat->uid > next_uid) {
        return false;
    }

    if (!getstring(in, pat->name, sizeof(pat->name))) {
        return false;
    }

    if (!getstring(in, pat->base_name, sizeof(pat->base_name))) {
        return false;
    }

    pat->duplicate_index = get32(in);

    if (!*pat->base_name || !pat->duplicate_index) {
        return false;
    }

    double length = getreal(in);
    uint32_t notes = get32(in);

    if (!unpack_pattern_mode(pat, in) || (pat->kind && notes)) {
        return false;
    }

    if (!in->ok || length <= 0 || length > 10000000 || notes > in->left / least(in, 32, 6)) {
        return false;
    }

    if (!unpack_pattern_notes(pat, in, notes)) {
        return false;
    }

    if (!pattern_length_valid(pat, length)) {
        return false;
    }

    pat->length = length;
    return in->ok;
}

static bool unpack_patterns(DmProject *p, Input *in) {
    uint32_t next_uid = get32(in);
    uint32_t count = get32(in);

    if (count > 100000 || count > in->left / least(in, 16, 10)) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (!unpack_pattern(p, in, next_uid)) {
            return false;
        }
    }

    p->next_pattern_uid = next_uid;
    return in->ok;
}

static bool unpack_clips(DmProject *p, Input *in) {
    uint32_t clips = get32(in);

    if (clips > in->left / least(in, 32, 6)) {
        return false;
    }

    for (uint32_t i = 0; i < clips; ++i) {
        DmClip c = {0};
        c.beat = get_time_after(in, p->clip_count ? p->clips[p->clip_count - 1].beat : 0);
        c.pattern = (int)get32(in);
        c.channel = (int)get32(in);
        uint32_t selected = get32(in);
        c.selected = selected != 0;
        c.length = getreal(in);
        c.lane = (int)get32(in);
        c.offset = schema_10(in) ? getreal(in) : 0;
        c.row = in->schema >= 19 ? (int)get32(in) : 0;

        if (!in->ok || selected > 1 || !dm_clip_add(p, c)) {
            return false;
        }
    }

    return in->ok;
}

static bool unpack_automation(DmProject *p, Input *in) {
    uint32_t autos = get32(in);

    if (autos > in->left / least(in, 52, 12)) {
        return false;
    }

    double previous_beat = 0;

    for (uint32_t i = 0; i < autos; ++i) {
        DmAutomation a = {0};
        a.beat = get_time_after(in, previous_beat);
        previous_beat = a.beat;
        a.type = (int)get32(in);
        a.channel = (int)get32(in);
        a.a = (int)get32(in);
        a.b = (int)get32(in);
        a.pattern = (int32_t)get32(in);
        a.curve = (int32_t)get32(in);
        a.anchor = (int)get32(in);
        a.bend = getreal(in);
        uint32_t ramp = get32(in);

        if (ramp > 1) {
            return false;
        }

        a.ramp = ramp != 0;
        uint32_t port = get32(in);

        if (port >= DM_NOTE_PORTS) {
            return false;
        }

        a.port = (uint8_t)port;
        uint32_t hold = schema_11(in) ? get32(in) : 0;

        if (hold > 1) {
            return false;
        }

        a.hold = hold != 0;

        if (in->schema >= 17) {
            uint32_t fixed_anchor = get32(in);
            a.bend_note = get32(in);

            if (fixed_anchor > 1) {
                return false;
            }

            a.fixed_anchor = fixed_anchor != 0;
        }

        if (in->schema >= 18) {
            uint32_t root = get32(in);

            if (root > DM_BEND_ROOT_MIDI) {
                return false;
            }

            a.bend_root = (uint8_t)root;
        }

        if (!dm_automation_fields_valid(a)) {
            return false;
        }

        if (a.pattern >= 0 && (size_t)a.pattern < p->pattern_count && p->patterns[a.pattern].kind &&
            a.beat > p->patterns[a.pattern].length) {
            return false;
        }

        if (!in->ok || !dm_automation_insert(p, a)) {
            return false;
        }
    }

    return in->ok;
}

static bool unpack_events(DmProject *p, Input *in) {
    uint32_t events = get32(in);

    if (events > in->left / least(in, 20, 4)) {
        return false;
    }

    double last = 0;

    for (uint32_t i = 0; i < events; ++i) {
        DmEvent e = {0};
        e.beat = get_time_after(in, last);
        e.track = (int)get32(in);
        e.port = (int)get32(in);

        if (!in->ok || e.beat < last || e.track < 0 || e.port < 0 || e.port > 255 ||
            !getblock(in, &e.message) || !dm_message_valid(&e.message)) {
            dm_bytes_free(&e.message);
            return false;
        }

        last = e.beat;

        if (!dm_reserve((void **)&p->events, &p->event_capacity, p->event_count + 1, sizeof(e))) {
            dm_bytes_free(&e.message);
            return false;
        }

        p->events[p->event_count++] = e;
    }

    return in->ok;
}

static bool unpack_channels(DmProject *p, Input *in) {
    for (int i = 0; i < 16; ++i) {
        uint32_t enabled = get32(in), mute = get32(in), solo = get32(in), program = get32(in),
                 volume = get32(in), pan = get32(in), drums = schema_12(in) ? get32(in) : 0;

        if (enabled > 1 || mute > 1 || solo > 1 || drums > 1) {
            return false;
        }

        DmChannel ch = {enabled != 0, mute != 0, solo != 0, (int32_t)program,
                        (int)volume,  (int)pan,  drums != 0};

        if (!dm_channel_settings_valid(&ch)) {
            return false;
        }

        p->channels[i] = ch;
    }

    for (int c = 0; c < 16; ++c) {
        if (in->schema >= 19) {
            uint32_t rows = get32(in);

            if (rows > INT_MAX || !dm_note_rows_valid((int)rows)) {
                return false;
            }

            p->note_rows[c] = (int)rows;
        }

        uint32_t count = get32(in);

        if (count > INT_MAX || !dm_lane_count_valid((int)count)) {
            return false;
        }

        p->lane_count[c] = (int)count;

        for (uint32_t i = 0; i < count; ++i) {
            if (!getstring(in, p->lane_names[c][i], 64)) {
                return false;
            }
        }
    }

    return in->ok;
}

static bool unpack_metadata(DmProject *p, Input *in) {
    p->bpm = getreal(in);
    p->numerator = (int)get32(in);
    p->denominator = (int)get32(in);
    p->port = (int)get32(in);
    p->sequence = (int)get32(in);
    p->source_format = (int)get32(in);
    p->sequence_count = (int)get32(in);
    p->port_count = (int)get32(in);

    if (!dm_metadata_valid(p)) {
        return false;
    }

    return in->ok;
}

static bool unpack_views(DmProject *p, Input *in, bool allow_views) {
    uint32_t count = get32(in);

    if (!in->ok || count > 4096 || count > in->left / least(in, 32, 3)) {
        return false;
    }

    if (count && !allow_views) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        DmView view = {0};
        view.port = (int)get32(in);
        view.sequence = (int)get32(in);

        if (!dm_valid_view_key(p, view.port, view.sequence) ||
            dm_view_index(p, view.port, view.sequence) >= 0) {
            return false;
        }

        if (!getblock(in, &view.state)) {
            return false;
        }

        if (!dm_validate_view(p, &view)) {
            dm_bytes_free(&view.state);
            return false;
        }

        if (!dm_reserve((void **)&p->views, &p->view_capacity, p->view_count + 1, sizeof(DmView))) {
            dm_bytes_free(&view.state);
            return false;
        }

        p->views[p->view_count++] = view;
    }

    return true;
}

static bool unpack_ports(DmProject *p, Input *in) {
    for (int i = 0; i < p->port_count; ++i) {
        p->ports[i] = (int)get32(in);

        if (p->ports[i] < 0 || p->ports[i] > 255) {
            return false;
        }
    }

    return in->ok;
}

static bool unpack_content(DmProject *p, Input *in) {
    if (!getblock(in, &p->original) || !unpack_patterns(p, in)) {
        return false;
    }

    if (!unpack_clips(p, in)) {
        return false;
    }

    if (!unpack_automation(p, in)) {
        return false;
    }

    if (!unpack_events(p, in)) {
        return false;
    }

    return in->ok;
}

static bool unpack_patch_replacements(DmProject *p, Input *in) {
    uint32_t count = get32(in);

    if (!in->ok || count > DM_PATCH_REPLACEMENTS_MAX || count > in->left / 9) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t port = get32(in), channel = get32(in), bank = get32(in), program = get32(in);
        uint32_t drums = get32(in), generation = get32(in);

        if (!in->ok || port >= DM_NOTE_PORTS || channel > 15 || bank > 16383 || program > 127 ||
            drums > 1 || (in->schema >= 14 && generation > 1)) {
            return false;
        }

        DmPatchReplacement r = {
            .key = {(uint16_t)bank, (uint8_t)port, (uint8_t)channel, (uint8_t)program, drums != 0},
            .generation = generation};

        if (!getstring(in, r.locator, sizeof(r.locator)) ||
            !getstring(in, r.set_id, sizeof(r.set_id)) ||
            !getstring(in, r.preset_id, sizeof(r.preset_id)) ||
            dm_patch_replacement_exact(p, r.key) || !dm_patch_replacement_set(p, &r)) {
            return false;
        }
    }

    return true;
}

bool dm_read_body_legacy(DmProject *p, Input *in, bool allow_views) {
    if (!getstring(in, p->name, sizeof(p->name)) ||
        !getstring(in, p->soundfont, sizeof(p->soundfont))) {
        return false;
    }

    if (!unpack_metadata(p, in)) {
        return false;
    }

    if (!unpack_ports(p, in)) {
        return false;
    }

    uint32_t override = get32(in);

    if (override > 1) {
        return false;
    }

    p->percussion_override = override != 0;

    for (unsigned i = 0; i < 128; i++) {
        uint32_t key = get32(in);

        if (key > DM_PIANO_KEYS) {
            return false;
        }

        p->percussion_keys[i] = (uint8_t)key;
    }

    for (unsigned i = 0; i < 128 && schema_15(in); i++) {
        uint32_t note = get32(in);

        if (note > MIDI_KEYS) {
            return false;
        }

        p->percussion_midi[i] = (uint8_t)note;
    }

    if (!dm_percussion_valid(p)) {
        return false;
    }

    if (!unpack_channels(p, in)) {
        return false;
    }

    if (!unpack_content(p, in)) {
        return false;
    }

    if (!allow_views && p->original.size) {
        return false;
    }

    if (!unpack_views(p, in, allow_views)) {
        return false;
    }

    uint32_t mixes = get32(in);

    if (mixes > (DM_NOTE_PORTS - 1) * 16 || mixes > in->left / least(in, 24, 7)) {
        return false;
    }

    bool seen[DM_PARTS] = {0};

    for (uint32_t i = 0; i < mixes; i++) {
        uint32_t port = get32(in), channel = get32(in), volume = get32(in), pan = get32(in),
                 mute = get32(in), solo = get32(in);
        uint32_t program = schema_10(in) ? get32(in) : 0;

        if (!in->ok || mute > 1 || solo > 1 || port > INT_MAX || channel > INT_MAX ||
            volume > INT_MAX || pan > INT_MAX || program > INT_MAX) {
            return false;
        }

        DmPortMix mix = {(int)port, (int)channel, (int)volume, (int)pan,
                         mute != 0, solo != 0,    (int)program};

        if (!dm_port_mix_valid(mix)) {
            return false;
        }

        unsigned part = (unsigned)mix.port * 16u + (unsigned)mix.channel;

        if (seen[part]) {
            return false;
        }

        seen[part] = true;

        if (!dm_port_mix_set(p, mix)) {
            return false;
        }
    }

    if (p->view_count && !p->original.size) {
        return false;
    }

    if (in->schema >= 13 && !unpack_patch_replacements(p, in)) {
        return false;
    }

    return in->ok && in->left == 0;
}
