/* The tagged project body, schema 20 and later.

   The body is a sequence of records and each record's payload a sequence of fields, both stored
   as tag (varint), length (varint), value. One table per record type names each field's tag,
   type, struct member and default. The writer leaves out a field equal to its default; the
   reader starts every record from the defaults and refuses a tag it does not know, so a project
   saved by a newer duckmidi is never misread.

   The tables are the file format: a tag is never reused, and a default never changes. Adding a
   saved field is one new row (a new tag); older files without it read the default. The schema
   changes only when the meaning of existing data changes (project_compat.c). */
#include "core/project_internal.h"
#include "text/en.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    FIELD_INT,   /* int, zigzag varint */
    FIELD_U8,    /* uint8_t, varint */
    FIELD_U16,   /* uint16_t, varint */
    FIELD_U32,   /* uint32_t, varint */
    FIELD_BOOL,  /* bool, varint 0 or 1 */
    FIELD_REAL,  /* double, dm_put_real */
    FIELD_TIME,  /* double beat, step difference from the previous record in its list */
    FIELD_TEXT,  /* char array, UTF-8 without the terminator */
    FIELD_BYTES, /* DmBytes */
} FieldType;

typedef struct {
    uint8_t tag;     /* 1..63 within its record */
    uint8_t type;    /* FieldType */
    bool always;     /* written even when equal to the default: identities and list times */
    uint16_t offset; /* offsetof the member */
    uint16_t size;   /* sizeof the member, checked against the type */
    double fallback; /* the default of a number; text and bytes default to empty */
} Field;

#define FIELD(tag, type, s, member, fallback)                                                      \
    {tag, type, false, (uint16_t)offsetof(s, member), (uint16_t)sizeof(((s *)0)->member), fallback}
#define ALWAYS(tag, type, s, member)                                                               \
    {tag, type, true, (uint16_t)offsetof(s, member), (uint16_t)sizeof(((s *)0)->member), 0}
#define COUNT(table) (sizeof(table) / sizeof((table)[0]))

/* Top-level record tags, in the order they must appear. */
enum {
    RECORD_PROJECT = 1,
    RECORD_CHANNEL,
    RECORD_ORIGINAL,
    RECORD_PATTERN,
    RECORD_PLACEMENT,
    RECORD_AUTOMATION,
    RECORD_EVENT,
    RECORD_VIEW,
    RECORD_PORT_MIX,
    RECORD_PATCH,
};

/* Nested records and list fields inside a record. */
enum {
    PROJECT_PORT = 20,       /* repeated: ports[], in order; their number is port_count */
    PROJECT_PERCUSSION = 21, /* nested PercussionRecord, one per assigned key */
    CHANNEL_LANE_NAME = 20,  /* repeated: lane_names[channel][], in subrow order */
    PATTERN_CONTROL_SCOPE = 17, /* retired in schema 23: read from older files and discarded */
    PATTERN_NOTE = 30,          /* nested DmNote */
    PATTERN_GRAPH_LANE = 31, /* nested GraphLaneRecord */
};

typedef struct {
    int index;
    DmChannel channel;
    int note_rows, lane_count;
} ChannelRecord;

typedef struct {
    int index;
    uint8_t key, midi;
} PercussionRecord;

typedef struct {
    int lane;
    uint16_t setting;
    bool hold;
} GraphLaneRecord;

typedef struct {
    DmBytes data;
} OriginalRecord;

static const Field project_fields[] = {
    FIELD(1, FIELD_TEXT, DmProject, name, 0),
    FIELD(2, FIELD_TEXT, DmProject, soundfont, 0),
    FIELD(3, FIELD_REAL, DmProject, bpm, 120),
    FIELD(4, FIELD_INT, DmProject, numerator, 4),
    FIELD(5, FIELD_INT, DmProject, denominator, 4),
    FIELD(6, FIELD_INT, DmProject, port, 0),
    FIELD(7, FIELD_INT, DmProject, sequence, 0),
    FIELD(8, FIELD_INT, DmProject, source_format, 0),
    FIELD(9, FIELD_INT, DmProject, sequence_count, 1),
    FIELD(10, FIELD_BOOL, DmProject, percussion_override, 0),
    FIELD(11, FIELD_U32, DmProject, next_pattern_uid, 0),
};

static const Field percussion_fields[] = {
    ALWAYS(1, FIELD_INT, PercussionRecord, index),
    FIELD(2, FIELD_U8, PercussionRecord, key, 0),
    FIELD(3, FIELD_U8, PercussionRecord, midi, 0),
};

static const Field channel_fields[] = {
    ALWAYS(1, FIELD_INT, ChannelRecord, index),
    FIELD(2, FIELD_BOOL, ChannelRecord, channel.enabled, 0),
    FIELD(3, FIELD_BOOL, ChannelRecord, channel.mute, 0),
    FIELD(4, FIELD_BOOL, ChannelRecord, channel.solo, 0),
    FIELD(5, FIELD_INT, ChannelRecord, channel.program, -1),
    FIELD(6, FIELD_INT, ChannelRecord, channel.volume, 100),
    FIELD(7, FIELD_INT, ChannelRecord, channel.pan, 64),
    FIELD(8, FIELD_BOOL, ChannelRecord, channel.drums, 0),
    FIELD(9, FIELD_INT, ChannelRecord, note_rows, 1),
    FIELD(10, FIELD_INT, ChannelRecord, lane_count, 0),
};

static const Field original_fields[] = {
    FIELD(1, FIELD_BYTES, OriginalRecord, data, 0),
};

static const Field pattern_fields[] = {
    FIELD(1, FIELD_U32, DmPattern, uid, 0),
    FIELD(2, FIELD_TEXT, DmPattern, name, 0),
    FIELD(3, FIELD_TEXT, DmPattern, base_name, 0),
    FIELD(4, FIELD_U32, DmPattern, duplicate_index, 1),
    FIELD(5, FIELD_REAL, DmPattern, length, 1),
    FIELD(6, FIELD_INT, DmPattern, kind, 0),
    FIELD(7, FIELD_INT, DmPattern, mode, 0),
    FIELD(8, FIELD_INT, DmPattern, event, DM_EVENT_BEND),
    FIELD(9, FIELD_REAL, DmPattern, speed, 1),
    FIELD(10, FIELD_REAL, DmPattern, tension, 0),
    FIELD(11, FIELD_REAL, DmPattern, skew, 0),
    FIELD(12, FIELD_REAL, DmPattern, pulse, .5),
    FIELD(13, FIELD_REAL, DmPattern, amount, .5),
    FIELD(14, FIELD_REAL, DmPattern, center, 64),
    FIELD(15, FIELD_REAL, DmPattern, pitch_center, 60),
    FIELD(16, FIELD_INT, DmPattern, reference, 60),
    FIELD(18, FIELD_INT, DmPattern, port, 0),
    FIELD(19, FIELD_BOOL, DmPattern, inverted, 0),
    FIELD(20, FIELD_U8, DmPattern, color, 0),
};

static const Field note_fields[] = {
    ALWAYS(1, FIELD_TIME, DmNote, beat),     FIELD(2, FIELD_REAL, DmNote, length, 0),
    FIELD(3, FIELD_U8, DmNote, key, 0),      FIELD(4, FIELD_U8, DmNote, velocity, 0),
    FIELD(5, FIELD_U8, DmNote, selected, 0), FIELD(6, FIELD_U8, DmNote, port, 0),
    FIELD(7, FIELD_U32, DmNote, bend_id, 0),
};

static const Field graph_lane_fields[] = {
    ALWAYS(1, FIELD_INT, GraphLaneRecord, lane),
    ALWAYS(2, FIELD_U16, GraphLaneRecord, setting),
    FIELD(3, FIELD_BOOL, GraphLaneRecord, hold, 0),
};

static const Field placement_fields[] = {
    ALWAYS(1, FIELD_TIME, DmClip, beat),     FIELD(2, FIELD_INT, DmClip, pattern, 0),
    FIELD(3, FIELD_INT, DmClip, channel, 0), FIELD(4, FIELD_U8, DmClip, selected, 0),
    FIELD(5, FIELD_REAL, DmClip, length, 0), FIELD(6, FIELD_INT, DmClip, lane, 0),
    FIELD(7, FIELD_INT, DmClip, row, 0),     FIELD(8, FIELD_REAL, DmClip, offset, 0),
};

static const Field automation_fields[] = {
    ALWAYS(1, FIELD_TIME, DmAutomation, beat),
    FIELD(2, FIELD_INT, DmAutomation, type, 0),
    FIELD(3, FIELD_INT, DmAutomation, channel, 0),
    FIELD(4, FIELD_INT, DmAutomation, a, 0),
    FIELD(5, FIELD_INT, DmAutomation, b, 0),
    FIELD(6, FIELD_INT, DmAutomation, pattern, 0),
    FIELD(7, FIELD_INT, DmAutomation, curve, 0),
    FIELD(8, FIELD_INT, DmAutomation, anchor, 0),
    FIELD(9, FIELD_REAL, DmAutomation, bend, 0),
    FIELD(10, FIELD_BOOL, DmAutomation, ramp, 0),
    FIELD(11, FIELD_U8, DmAutomation, port, 0),
    FIELD(12, FIELD_BOOL, DmAutomation, hold, 0),
    FIELD(13, FIELD_BOOL, DmAutomation, fixed_anchor, 0),
    FIELD(14, FIELD_U32, DmAutomation, bend_note, 0),
    FIELD(15, FIELD_U8, DmAutomation, bend_root, 0),
};

static const Field event_fields[] = {
    ALWAYS(1, FIELD_TIME, DmEvent, beat),
    FIELD(2, FIELD_INT, DmEvent, track, 0),
    FIELD(3, FIELD_INT, DmEvent, port, 0),
    FIELD(4, FIELD_BYTES, DmEvent, message, 0),
};

static const Field view_fields[] = {
    FIELD(1, FIELD_INT, DmView, port, 0),
    FIELD(2, FIELD_INT, DmView, sequence, 0),
    FIELD(3, FIELD_BYTES, DmView, state, 0),
};

static const Field port_mix_fields[] = {
    FIELD(1, FIELD_INT, DmPortMix, port, 0),    FIELD(2, FIELD_INT, DmPortMix, channel, 0),
    FIELD(3, FIELD_INT, DmPortMix, volume, 0),  FIELD(4, FIELD_INT, DmPortMix, pan, 0),
    FIELD(5, FIELD_BOOL, DmPortMix, mute, 0),   FIELD(6, FIELD_BOOL, DmPortMix, solo, 0),
    FIELD(7, FIELD_INT, DmPortMix, program, 0),
};

static const Field patch_fields[] = {
    FIELD(1, FIELD_U16, DmPatchReplacement, key.bank, 0),
    FIELD(2, FIELD_U8, DmPatchReplacement, key.port, 0),
    FIELD(3, FIELD_U8, DmPatchReplacement, key.channel, 0),
    FIELD(4, FIELD_U8, DmPatchReplacement, key.program, 0),
    FIELD(5, FIELD_BOOL, DmPatchReplacement, key.drums, 0),
    FIELD(6, FIELD_U32, DmPatchReplacement, generation, 0),
    FIELD(7, FIELD_TEXT, DmPatchReplacement, locator, 0),
    FIELD(8, FIELD_TEXT, DmPatchReplacement, set_id, 0),
    FIELD(9, FIELD_TEXT, DmPatchReplacement, preset_id, 0),
    FIELD(10, FIELD_U8, DmPatchReplacement, key.drum_key, 0),
    FIELD(11, FIELD_TEXT, DmPatchReplacement, sample_id, 0),
};

/* The member size each numeric type stores; a table row with another size is a bug that makes
   every save and load fail, which the round-trip tests catch. */
static size_t type_size(int type) {
    switch (type) {
    case FIELD_INT:
        return sizeof(int);
    case FIELD_U8:
        return sizeof(uint8_t);
    case FIELD_U16:
        return sizeof(uint16_t);
    case FIELD_U32:
        return sizeof(uint32_t);
    case FIELD_BOOL:
        return sizeof(bool);
    case FIELD_REAL:
    case FIELD_TIME:
        return sizeof(double);
    case FIELD_BYTES:
        return sizeof(DmBytes);
    default:
        return 0; /* FIELD_TEXT: any array size */
    }
}

static bool numeric(int type) {
    return type != FIELD_TEXT && type != FIELD_BYTES;
}

/* A number field's value widened to a double; every stored integer type fits exactly. */
static double number_get(const Field *f, const void *object) {
    const uint8_t *at = (const uint8_t *)object + f->offset;
    int i;
    uint16_t u16;
    uint32_t u32;
    bool b;
    double d;

    switch (f->type) {
    case FIELD_INT:
        memcpy(&i, at, sizeof(i));
        return i;
    case FIELD_U8:
        return *at;
    case FIELD_U16:
        memcpy(&u16, at, sizeof(u16));
        return u16;
    case FIELD_U32:
        memcpy(&u32, at, sizeof(u32));
        return u32;
    case FIELD_BOOL:
        memcpy(&b, at, sizeof(b));
        return b;
    default:
        memcpy(&d, at, sizeof(d));
        return d;
    }
}

static void number_set(const Field *f, void *object, double value) {
    uint8_t *at = (uint8_t *)object + f->offset;

    /* Each cast happens only for its own type: value is in that type's range (get_number checks
       it), and a cast out of range would be undefined. */
    switch (f->type) {
    case FIELD_INT: {
        int i = (int)value;
        memcpy(at, &i, sizeof(i));
        break;
    }
    case FIELD_U8:
        *at = (uint8_t)value;
        break;
    case FIELD_U16: {
        uint16_t u16 = (uint16_t)value;
        memcpy(at, &u16, sizeof(u16));
        break;
    }
    case FIELD_U32: {
        uint32_t u32 = (uint32_t)value;
        memcpy(at, &u32, sizeof(u32));
        break;
    }
    case FIELD_BOOL: {
        bool b = value != 0;
        memcpy(at, &b, sizeof(b));
        break;
    }
    default:
        memcpy(at, &value, sizeof(value));
        break;
    }
}

/* Sets every table field of object to its default; text and bytes become empty (bytes must not
   own memory yet). */
static void defaults(const Field *fields, size_t count, void *object) {
    for (size_t i = 0; i < count; ++i) {
        const Field *f = &fields[i];

        if (f->type == FIELD_TEXT) {
            ((char *)object + f->offset)[0] = 0;
        } else if (f->type == FIELD_BYTES) {
            *(DmBytes *)((uint8_t *)object + f->offset) = (DmBytes){0};
        } else {
            number_set(f, object, f->fallback);
        }
    }
}

static uint64_t zigzag(int64_t n) {
    return n < 0 ? ((uint64_t)(-(n + 1)) << 1) | 1 : (uint64_t)n << 1;
}

static int64_t unzigzag(uint64_t n) {
    return n & 1 ? -(int64_t)(n >> 1) - 1 : (int64_t)(n >> 1);
}

/* ---- Writing ---- */

/* Scratch buffers reused across records, one per nesting level. */
typedef struct {
    DmBytes record, nested;
} Writer;

static size_t encode_varint(uint8_t *out, uint64_t n) {
    size_t size = 0;

    do {
        out[size++] = (uint8_t)((n & 127) | (n > 127 ? 128 : 0));
        n >>= 7;
    } while (n);

    return size;
}

/* A number's value bytes, the same encodings dm_put_real and dm_put_time_after write: an exact
   multiple of 1/DM_REAL_STEPS (or step difference) as a varint, else 1 and the 8 raw bytes. */
static size_t encode_number(uint8_t *out, const Field *f, double value, double previous) {
    int64_t steps, before;

    switch (f->type) {
    case FIELD_INT:
        return encode_varint(out, zigzag((int64_t)value));
    case FIELD_REAL:
        if (dm_exact_steps(value, &steps)) {
            return encode_varint(out, zigzag(steps) << 1);
        }

        break;
    case FIELD_TIME:
        if (dm_exact_steps(value, &steps) && dm_exact_steps(previous, &before)) {
            return encode_varint(out, zigzag(steps - before) << 1);
        }

        break;
    default:
        return encode_varint(out, (uint64_t)value);
    }

    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    out[0] = 1;

    for (int i = 0; i < 8; ++i) {
        out[1 + i] = (uint8_t)(bits >> (8 * i));
    }

    return 9;
}

static bool put_entry(DmBytes *out, unsigned tag, const void *data, size_t size) {
    uint8_t header[20];
    size_t n = encode_varint(header, tag);
    n += encode_varint(header + n, size);
    return dm_bytes(out, header, n) && dm_bytes(out, data, size);
}

/* A number field in one append: tag, length and value are at most 1 + 1 + 10 bytes. */
static bool put_number(DmBytes *out, const Field *f, double value, double previous) {
    uint8_t entry[32];
    size_t n = encode_varint(entry, f->tag);
    size_t size = encode_number(entry + n + 1, f, value, previous);
    entry[n] = (uint8_t)size;
    return dm_bytes(out, entry, n + 1 + size);
}

/* Appends one record's fields that differ from their defaults, plus its "always" fields.
   previous is the time of the record before it in its list, for FIELD_TIME. */
static bool put_fields(DmBytes *out, const Field *fields, size_t count, const void *object,
                       double previous) {
    for (size_t i = 0; i < count; ++i) {
        const Field *f = &fields[i];
        const uint8_t *at = (const uint8_t *)object + f->offset;

        if (type_size(f->type) && type_size(f->type) != f->size) {
            return false;
        }

        if (f->type == FIELD_TEXT) {
            size_t size = strnlen((const char *)at, f->size);

            if (size == f->size || ((size || f->always) && !put_entry(out, f->tag, at, size))) {
                return false;
            }
        } else if (f->type == FIELD_BYTES) {
            const DmBytes *bytes = (const DmBytes *)at;

            if ((bytes->size || f->always) && !put_entry(out, f->tag, bytes->data, bytes->size)) {
                return false;
            }
        } else {
            double value = number_get(f, object);

            if ((f->always || value != f->fallback) && !put_number(out, f, value, previous)) {
                return false;
            }
        }
    }

    return true;
}

/* Appends payload as a record of tag and empties payload for reuse. */
static bool put_record(DmBytes *out, unsigned tag, DmBytes *payload) {
    bool ok = put_entry(out, tag, payload->data, payload->size);
    payload->size = 0;
    return ok;
}

static bool put_int_entry(DmBytes *out, unsigned tag, int n) {
    const Field f = {(uint8_t)tag, FIELD_INT, true, 0, sizeof(int), 0};
    return put_number(out, &f, n, 0);
}

static bool put_project(Writer *w, DmBytes *out, const DmProject *p) {
    DmBytes *r = &w->record;
    bool ok = put_fields(r, project_fields, COUNT(project_fields), p, 0);

    for (int i = 0; i < p->port_count && ok; ++i) {
        ok = put_int_entry(r, PROJECT_PORT, p->ports[i]);
    }

    for (int i = 0; i < 128 && ok; ++i) {
        if (p->percussion_keys[i] || p->percussion_midi[i]) {
            PercussionRecord entry = {i, p->percussion_keys[i], p->percussion_midi[i]};
            ok = put_fields(&w->nested, percussion_fields, COUNT(percussion_fields), &entry, 0) &&
                 put_record(r, PROJECT_PERCUSSION, &w->nested);
        }
    }

    return ok && put_record(out, RECORD_PROJECT, r);
}

static bool put_channels(Writer *w, DmBytes *out, const DmProject *p) {
    for (int c = 0; c < 16; ++c) {
        ChannelRecord channel = {c, p->channels[c], p->note_rows[c], p->lane_count[c]};
        bool ok = put_fields(&w->record, channel_fields, COUNT(channel_fields), &channel, 0);

        for (int i = 0; i < p->lane_count[c] && i < DM_LANES && ok; ++i) {
            size_t size = strnlen(p->lane_names[c][i], 64);
            ok = size < 64 && put_entry(&w->record, CHANNEL_LANE_NAME, p->lane_names[c][i], size);
        }

        if (!ok || !put_record(out, RECORD_CHANNEL, &w->record)) {
            return false;
        }
    }

    return true;
}

static bool put_pattern(Writer *w, DmBytes *out, const DmPattern *pattern) {
    DmBytes *r = &w->record;
    bool ok = put_fields(r, pattern_fields, COUNT(pattern_fields), pattern, 0);

    for (unsigned lane = 0; lane < DM_GRAPH_LANES && ok; ++lane) {
        if (pattern->graph_lanes[lane]) {
            GraphLaneRecord entry = {(int)lane, pattern->graph_lanes[lane],
                                     pattern->graph_hold[lane]};
            ok = put_fields(&w->nested, graph_lane_fields, COUNT(graph_lane_fields), &entry, 0) &&
                 put_record(r, PATTERN_GRAPH_LANE, &w->nested);
        }
    }

    for (size_t i = 0; i < pattern->count && ok; ++i) {
        ok = put_fields(&w->nested, note_fields, COUNT(note_fields), &pattern->notes[i],
                        i ? pattern->notes[i - 1].beat : 0) &&
             put_record(r, PATTERN_NOTE, &w->nested);
    }

    return ok && put_record(out, RECORD_PATTERN, r);
}

/* Appends one record per item of a list; records with a FIELD_TIME store it against the
   previous item's time, which each list keeps as its first member (beat). */
static bool put_list(Writer *w, DmBytes *out, unsigned tag, const Field *fields, size_t count,
                     const void *items, size_t n, size_t width) {
    double previous = 0;

    for (size_t i = 0; i < n; ++i) {
        const void *item = (const uint8_t *)items + i * width;

        if (!put_fields(&w->record, fields, count, item, previous) ||
            !put_record(out, tag, &w->record)) {
            return false;
        }

        if (fields[0].type == FIELD_TIME) {
            memcpy(&previous, (const uint8_t *)item + fields[0].offset, sizeof(previous));
        }
    }

    return true;
}

bool dm_write_body(const DmProject *p, DmBytes *out) {
    Writer w = {0};
    OriginalRecord original = {p->original};
    bool ok = put_project(&w, out, p) && put_channels(&w, out, p);

    if (ok && p->original.size) {
        ok = put_fields(&w.record, original_fields, COUNT(original_fields), &original, 0) &&
             put_record(out, RECORD_ORIGINAL, &w.record);
    }

    for (size_t i = 0; i < p->pattern_count && ok; ++i) {
        ok = put_pattern(&w, out, &p->patterns[i]);
    }

    ok = ok && put_list(&w, out, RECORD_PLACEMENT, placement_fields, COUNT(placement_fields),
                        p->clips, p->clip_count, sizeof(DmClip));
    ok = ok && put_list(&w, out, RECORD_AUTOMATION, automation_fields, COUNT(automation_fields),
                        p->automation, p->automation_count, sizeof(DmAutomation));
    ok = ok && put_list(&w, out, RECORD_EVENT, event_fields, COUNT(event_fields), p->events,
                        p->event_count, sizeof(DmEvent));
    ok = ok && put_list(&w, out, RECORD_VIEW, view_fields, COUNT(view_fields), p->views,
                        p->view_count, sizeof(DmView));
    ok = ok && put_list(&w, out, RECORD_PORT_MIX, port_mix_fields, COUNT(port_mix_fields),
                        p->port_mix, p->port_mix_count, sizeof(DmPortMix));
    ok = ok && p->patch_replacement_count <= DM_PATCH_REPLACEMENTS_MAX;

    for (size_t i = 0; i < p->patch_replacement_count && ok; ++i) {
        /* generation is a runtime cache token; a project stores only whether it is set. */
        DmPatchReplacement patch = p->patch_replacements[i];
        patch.generation = patch.generation != 0;
        ok = dm_patch_replacement_valid(&patch) &&
             put_fields(&w.record, patch_fields, COUNT(patch_fields), &patch, 0) &&
             put_record(out, RECORD_PATCH, &w.record);
    }

    dm_bytes_free(&w.record);
    dm_bytes_free(&w.nested);
    return ok;
}

/* ---- Reading ---- */

typedef struct {
    bool newer; /* a tag this duckmidi does not know: the file comes from a newer version */
    /* The defaults of the records that come in tens of thousands, built once per load. */
    DmNote note;
    DmClip placement;
    DmAutomation automation;
    DmEvent event;
} Reader;

/* Reads the next tag and its value; value covers exactly the value's bytes. */
/* The varint at in, decoded here rather than through project_codec.c: loading reads two or
   three per field, so the call overhead dominates. Sets in->ok false when it runs out. */
static inline uint64_t read_varint(Input *in) {
    uint64_t n = 0;

    for (int shift = 0; shift < 64 && in->left; shift += 7) {
        uint8_t byte = *in->p++;
        --in->left;
        n |= (uint64_t)(byte & 127) << shift;

        if (!(byte & 128)) {
            return n;
        }
    }

    in->ok = false;
    return 0;
}

static bool next_entry(Input *in, uint64_t *tag, Input *value) {
    *tag = read_varint(in);
    uint64_t size = read_varint(in);

    if (!in->ok || !*tag || size > in->left) {
        in->ok = false;
        return false;
    }

    *value = (Input){in->p, (size_t)size, true, in->schema};
    in->p += size;
    in->left -= size;
    return true;
}

static const Field *find_field(const Field *fields, size_t count, uint64_t tag) {
    /* Tables list tags 1, 2, 3, ... in order, so the row is usually at tag - 1. */
    if (tag && tag <= count && fields[tag - 1].tag == tag) {
        return &fields[tag - 1];
    }

    for (size_t i = 0; i < count; ++i) {
        if (fields[i].tag == tag) {
            return &fields[i];
        }
    }

    return NULL;
}

static bool get_number(Input *value, const Field *f, void *object, double previous) {
    double n;

    switch (f->type) {
    case FIELD_INT: {
        int64_t v = unzigzag(read_varint(value));

        if (v < INT32_MIN || v > INT32_MAX) {
            return false;
        }

        n = (double)v;
        break;
    }
    case FIELD_REAL:
    case FIELD_TIME: {
        /* Even: a step count (a step difference for a time); 1: the 8 raw bytes follow. */
        uint64_t v = read_varint(value);
        int64_t before = 0;

        if (!(v & 1)) {
            if (f->type == FIELD_TIME && !dm_exact_steps(previous, &before)) {
                return false;
            }

            n = (double)(before + unzigzag(v >> 1)) / DM_REAL_STEPS;
        } else if (v == 1 && value->left == 8) {
            uint64_t bits = 0;

            for (int i = 0; i < 8; ++i) {
                bits |= (uint64_t)value->p[i] << (8 * i);
            }

            memcpy(&n, &bits, sizeof(n));
            value->p += 8;
            value->left = 0;
        } else {
            return false;
        }

        break;
    }
    default: {
        uint64_t v = read_varint(value);
        uint64_t max = f->type == FIELD_U8    ? UINT8_MAX
                       : f->type == FIELD_U16 ? UINT16_MAX
                       : f->type == FIELD_U32 ? UINT32_MAX
                                              : 1;

        if (v > max) {
            return false;
        }

        n = (double)v;
        break;
    }
    }

    if (!value->ok || value->left || !isfinite(n)) {
        return false;
    }

    number_set(f, object, n);
    return true;
}

/* Stores one field's value into object. Bytes are copied, so a failed record must be freed by
   the caller. */
static bool get_field(Input *value, const Field *f, void *object, double previous) {
    uint8_t *at = (uint8_t *)object + f->offset;

    if (type_size(f->type) && type_size(f->type) != f->size) {
        return false;
    }

    if (f->type == FIELD_TEXT) {
        if (value->left >= f->size || memchr(value->p, 0, value->left)) {
            return false;
        }

        memcpy(at, value->p, value->left);
        at[value->left] = 0;
        return true;
    }

    if (f->type == FIELD_BYTES) {
        return dm_bytes((DmBytes *)at, value->p, value->left);
    }

    return get_number(value, f, object, previous);
}

/* Handles an entry that is not a table field of its record: a nested record or list field.
   Returns false for an invalid entry; sets *known when the tag was recognised. */
typedef bool (*EntryHandler)(Reader *r, void *context, uint64_t tag, Input *value, bool *known);

/* Reads a record's payload: table fields into object (which starts at the defaults), any other
   tag through handler. A repeated table field, an unknown tag or a malformed value fails. */
static bool get_record(Reader *r, Input *payload, const Field *fields, size_t count, void *object,
                       double previous, EntryHandler handler, void *context) {
    uint64_t seen = 0;

    while (payload->left) {
        uint64_t tag;
        Input value;

        if (!next_entry(payload, &tag, &value)) {
            return false;
        }

        const Field *f = find_field(fields, count, tag);

        if (f) {
            uint64_t bit = 1ull << (f->tag & 63);

            if ((seen & bit) || !get_field(&value, f, object, previous)) {
                return false;
            }

            seen |= bit;
            continue;
        }

        bool known = false;

        if (!handler || !handler(r, context, tag, &value, &known)) {
            r->newer |= !known;
            return false;
        }
    }

    return payload->ok;
}

/* Project record: ports and percussion map entries. */
typedef struct {
    DmProject *p;
    int ports;
    bool keys[128];
} ProjectContext;

static bool project_entry(Reader *r, void *context, uint64_t tag, Input *value, bool *known) {
    ProjectContext *c = context;

    if (tag == PROJECT_PORT) {
        *known = true;
        int64_t port = unzigzag(dm_get_varint(value));

        if (!value->ok || value->left || port < 0 || port > 255 || c->ports >= 256) {
            return false;
        }

        c->p->ports[c->ports++] = (int)port;
        return true;
    }

    if (tag == PROJECT_PERCUSSION) {
        *known = true;
        PercussionRecord entry;
        defaults(percussion_fields, COUNT(percussion_fields), &entry);

        if (!get_record(r, value, percussion_fields, COUNT(percussion_fields), &entry, 0, NULL,
                        NULL) ||
            entry.index < 0 || entry.index >= 128 || c->keys[entry.index] ||
            entry.key > DM_PIANO_KEYS || entry.midi > MIDI_KEYS) {
            return false;
        }

        c->keys[entry.index] = true;
        c->p->percussion_keys[entry.index] = entry.key;
        c->p->percussion_midi[entry.index] = entry.midi;
        return true;
    }

    return false;
}

static bool get_project(Reader *r, Input *payload, DmProject *p) {
    ProjectContext context = {p, 0, {0}};
    defaults(project_fields, COUNT(project_fields), p);

    if (!get_record(r, payload, project_fields, COUNT(project_fields), p, 0, project_entry,
                    &context)) {
        return false;
    }

    /* Without port entries the project keeps one port, 0, as a new project does. */
    p->port_count = context.ports ? context.ports : 1;
    return dm_metadata_valid(p) && dm_percussion_valid(p);
}

typedef struct {
    char names[DM_LANES][64];
    int count;
} LaneNames;

static bool channel_entry(Reader *r, void *context, uint64_t tag, Input *value, bool *known) {
    LaneNames *names = context;
    (void)r;

    if (tag != CHANNEL_LANE_NAME) {
        return false;
    }

    *known = true;

    if (names->count >= DM_LANES || value->left >= 64 || memchr(value->p, 0, value->left)) {
        return false;
    }

    memcpy(names->names[names->count], value->p, value->left);
    names->names[names->count++][value->left] = 0;
    return true;
}

static bool get_channel(Reader *r, Input *payload, DmProject *p, bool *seen) {
    ChannelRecord channel;
    LaneNames names = {.count = 0};
    defaults(channel_fields, COUNT(channel_fields), &channel);
    channel.index = -1;

    if (!get_record(r, payload, channel_fields, COUNT(channel_fields), &channel, 0, channel_entry,
                    &names)) {
        return false;
    }

    int c = channel.index;

    if (c < 0 || c > 15 || seen[c] || !dm_channel_settings_valid(&channel.channel) ||
        !dm_note_rows_valid(channel.note_rows) || !dm_lane_count_valid(channel.lane_count) ||
        names.count > channel.lane_count) {
        return false;
    }

    seen[c] = true;
    p->channels[c] = channel.channel;
    p->note_rows[c] = channel.note_rows;
    p->lane_count[c] = channel.lane_count;
    memset(p->lane_names[c], 0, sizeof(p->lane_names[c]));
    memcpy(p->lane_names[c], names.names, sizeof(names.names));
    return true;
}

/* Pattern record: notes and graph lanes. Notes are collected without changing the pattern's
   stored length; the length is checked against them once the record is complete. */
typedef struct {
    DmPattern *pattern;
    double previous; /* beat of the previous note */
} PatternContext;

static bool pattern_entry(Reader *r, void *context, uint64_t tag, Input *value, bool *known) {
    PatternContext *c = context;
    DmPattern *pattern = c->pattern;

    if (tag == PATTERN_NOTE) {
        *known = true;
        DmNote note = r->note;

        if (!get_record(r, value, note_fields, COUNT(note_fields), &note, c->previous, NULL,
                        NULL) ||
            !dm_note_valid(note) ||
            !dm_reserve((void **)&pattern->notes, &pattern->capacity, pattern->count + 1,
                        sizeof(DmNote))) {
            return false;
        }

        pattern->notes[pattern->count++] = note;
        c->previous = note.beat;
        return true;
    }

    if (tag == PATTERN_GRAPH_LANE) {
        *known = true;
        GraphLaneRecord entry;
        defaults(graph_lane_fields, COUNT(graph_lane_fields), &entry);

        if (!get_record(r, value, graph_lane_fields, COUNT(graph_lane_fields), &entry, 0, NULL,
                        NULL) ||
            entry.lane < 0 || entry.lane >= DM_GRAPH_LANES || !entry.setting ||
            pattern->graph_lanes[entry.lane]) {
            return false;
        }

        pattern->graph_lanes[entry.lane] = entry.setting;
        pattern->graph_hold[entry.lane] = entry.hold;
        return true;
    }

    if (tag == PATTERN_CONTROL_SCOPE && value->schema <= 22) {
        *known = true;
        int64_t scope = unzigzag(read_varint(value));
        return value->ok && !value->left && scope >= 0 && scope <= 2;
    }

    return false;
}

static bool get_pattern(Reader *r, Input *payload, DmProject *p) {
    DmPattern pattern;
    memset(&pattern, 0, sizeof(pattern));
    defaults(pattern_fields, COUNT(pattern_fields), &pattern);
    PatternContext context = {&pattern, 0};
    bool ok =
        p->pattern_count < 100000 && get_record(r, payload, pattern_fields, COUNT(pattern_fields),
                                                &pattern, 0, pattern_entry, &context);

    for (size_t i = 0; i < pattern.count && ok; ++i) {
        DmNote n = pattern.notes[i];
        ok = n.beat + n.length <= pattern.length + 1e-7;
    }

    ok = ok && pattern.uid && pattern.uid <= p->next_pattern_uid && *pattern.base_name &&
         pattern.duplicate_index && pattern.length > 0 && pattern.length <= 10000000 &&
         !(pattern.kind && pattern.count) && dm_pattern_settings_valid(&pattern);
    ok = ok && dm_reserve((void **)&p->patterns, &p->pattern_capacity, p->pattern_count + 1,
                          sizeof(DmPattern));

    if (!ok) {
        free(pattern.notes);
        return false;
    }

    p->patterns[p->pattern_count++] = pattern;
    return true;
}

static bool get_placement(Reader *r, Input *payload, DmProject *p, double *previous) {
    DmClip clip = r->placement;

    if (!get_record(r, payload, placement_fields, COUNT(placement_fields), &clip, *previous, NULL,
                    NULL) ||
        !dm_clip_add(p, clip)) {
        return false;
    }

    *previous = clip.beat;
    return true;
}

static bool get_automation(Reader *r, Input *payload, DmProject *p, double *previous) {
    DmAutomation a = r->automation;

    if (!get_record(r, payload, automation_fields, COUNT(automation_fields), &a, *previous, NULL,
                    NULL) ||
        !dm_automation_fields_valid(a)) {
        return false;
    }

    /* A point may not lie past its automation pattern; dm_automation_add would extend it. */
    if (a.pattern >= 0 && (size_t)a.pattern < p->pattern_count && p->patterns[a.pattern].kind &&
        a.beat > p->patterns[a.pattern].length) {
        return false;
    }

    *previous = a.beat;
    return dm_automation_insert(p, a);
}

static bool get_event(Reader *r, Input *payload, DmProject *p, double *previous) {
    DmEvent e = r->event;
    bool ok =
        get_record(r, payload, event_fields, COUNT(event_fields), &e, *previous, NULL, NULL) &&
        e.beat >= *previous && e.track >= 0 && e.port >= 0 && e.port <= 255 &&
        dm_message_valid(&e.message) &&
        dm_reserve((void **)&p->events, &p->event_capacity, p->event_count + 1, sizeof(e));

    if (!ok) {
        dm_bytes_free(&e.message);
        return false;
    }

    *previous = e.beat;
    p->events[p->event_count++] = e;
    return true;
}

static bool get_view(Reader *r, Input *payload, DmProject *p, bool allow_views) {
    DmView view;
    memset(&view, 0, sizeof(view));
    defaults(view_fields, COUNT(view_fields), &view);
    bool ok = allow_views && p->view_count < 4096 &&
              get_record(r, payload, view_fields, COUNT(view_fields), &view, 0, NULL, NULL) &&
              dm_valid_view_key(p, view.port, view.sequence) &&
              dm_view_index(p, view.port, view.sequence) < 0 && dm_validate_view(p, &view) &&
              dm_reserve((void **)&p->views, &p->view_capacity, p->view_count + 1, sizeof(DmView));

    if (!ok) {
        dm_bytes_free(&view.state);
        return false;
    }

    p->views[p->view_count++] = view;
    return true;
}

static bool get_port_mix(Reader *r, Input *payload, DmProject *p) {
    DmPortMix mix;
    memset(&mix, 0, sizeof(mix));
    defaults(port_mix_fields, COUNT(port_mix_fields), &mix);

    if (!get_record(r, payload, port_mix_fields, COUNT(port_mix_fields), &mix, 0, NULL, NULL) ||
        !dm_port_mix_valid(mix)) {
        return false;
    }

    for (size_t i = 0; i < p->port_mix_count; ++i) {
        if (p->port_mix[i].port == mix.port && p->port_mix[i].channel == mix.channel) {
            return false;
        }
    }

    return dm_port_mix_set(p, mix);
}

static bool get_patch(Reader *r, Input *payload, DmProject *p) {
    DmPatchReplacement patch;
    memset(&patch, 0, sizeof(patch));
    defaults(patch_fields, COUNT(patch_fields), &patch);

    return p->patch_replacement_count < DM_PATCH_REPLACEMENTS_MAX &&
           get_record(r, payload, patch_fields, COUNT(patch_fields), &patch, 0, NULL, NULL) &&
           patch.key.port < DM_NOTE_PORTS && patch.key.channel <= 15 && patch.key.bank <= 16383 &&
           patch.key.program <= 127 && patch.generation <= 1 &&
           !dm_patch_replacement_exact(p, patch.key) && dm_patch_replacement_set(p, &patch);
}

bool dm_read_body(DmProject *p, Input *in, bool allow_views, bool *newer) {
    Reader r;
    memset(&r, 0, sizeof(r));
    defaults(note_fields, COUNT(note_fields), &r.note);
    defaults(placement_fields, COUNT(placement_fields), &r.placement);
    defaults(automation_fields, COUNT(automation_fields), &r.automation);
    defaults(event_fields, COUNT(event_fields), &r.event);
    bool channels[16] = {0};
    double placement = 0, automation = 0, event = 0;
    uint64_t last = 0;
    bool ok = true;

    /* Channels without a record keep the format's defaults, not dm_project_init's. */
    for (int c = 0; c < 16; ++c) {
        ChannelRecord channel;
        defaults(channel_fields, COUNT(channel_fields), &channel);
        p->channels[c] = channel.channel;
        p->note_rows[c] = channel.note_rows;
        p->lane_count[c] = channel.lane_count;
    }

    while (ok && in->left) {
        uint64_t tag;
        Input payload;

        if (!next_entry(in, &tag, &payload)) {
            ok = false;
            break;
        }

        /* Records come in tag order and the project record comes first, exactly once. */
        bool project = tag == RECORD_PROJECT;

        if (project == (last != 0) || tag < last ||
            (tag == RECORD_ORIGINAL && last == RECORD_ORIGINAL)) {
            r.newer = tag > RECORD_PATCH;
            ok = false;
            break;
        }

        last = tag;

        switch (tag) {
        case RECORD_PROJECT:
            ok = get_project(&r, &payload, p);
            break;
        case RECORD_CHANNEL:
            ok = get_channel(&r, &payload, p, channels);
            break;
        case RECORD_ORIGINAL: {
            OriginalRecord original = {{0}};
            ok = get_record(&r, &payload, original_fields, COUNT(original_fields), &original, 0,
                            NULL, NULL);
            dm_bytes_free(&p->original);
            p->original = original.data;
            break;
        }
        case RECORD_PATTERN:
            ok = get_pattern(&r, &payload, p);
            break;
        case RECORD_PLACEMENT:
            ok = get_placement(&r, &payload, p, &placement);
            break;
        case RECORD_AUTOMATION:
            ok = get_automation(&r, &payload, p, &automation);
            break;
        case RECORD_EVENT:
            ok = get_event(&r, &payload, p, &event);
            break;
        case RECORD_VIEW:
            ok = get_view(&r, &payload, p, allow_views);
            break;
        case RECORD_PORT_MIX:
            ok = get_port_mix(&r, &payload, p);
            break;
        case RECORD_PATCH:
            ok = get_patch(&r, &payload, p);
            break;
        default:
            r.newer = true;
            ok = false;
            break;
        }
    }

    *newer = r.newer;
    return ok && in->ok && last && !(!allow_views && p->original.size) &&
           !(p->view_count && !p->original.size);
}
