#include "duck.h"
#include "text/en.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t left;
    bool ok;
    uint32_t schema; /* Schema of the document being read. */
} Input;

/* Fields added in schema 10 are absent from schema-9 documents; project_compat.c fills in their
   meaning after loading. */
static bool schema_10(const Input *in) {
    return in->schema >= 10;
}

/* Schema 10 stores numbers compactly. Integers are variable-length (7 bits per byte). A real
   that is an exact multiple of 1/REAL_STEPS, as beats on any common MIDI resolution are, is
   stored as that small integer; any other real keeps its full 8 bytes. Schema 9 used fixed
   4-byte integers and 8-byte reals; get32() and getreal() still read that layout. */
#define REAL_STEPS 3840.0

static bool fixed32(DmBytes *b, uint32_t n) {
    uint8_t v[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    return dm_bytes(b, v, 4);
}

static bool varint(DmBytes *b, uint64_t n) {
    uint8_t v[10];
    size_t size = 0;

    do {
        v[size++] = (uint8_t)((n & 127) | (n > 127 ? 128 : 0));
        n >>= 7;
    } while (n);

    return dm_bytes(b, v, size);
}

static bool u32(DmBytes *b, uint32_t n) {
    return varint(b, n);
}

static bool real(DmBytes *b, double n) {
    if (isfinite(n) && fabs(n) < 1e12) {
        int64_t steps = llround(n * REAL_STEPS);

        if ((double)steps / REAL_STEPS == n) {
            uint64_t zigzag =
                steps < 0 ? ((uint64_t)(-(steps + 1)) << 1) | 1 : (uint64_t)steps << 1;
            return varint(b, zigzag << 1);
        }
    }

    uint64_t bits;
    memcpy(&bits, &n, 8);
    return varint(b, 1) && fixed32(b, (uint32_t)bits) && fixed32(b, (uint32_t)(bits >> 32));
}

/* Times in time-ordered lists are stored as the step difference from the previous time, which
   is usually one or two bytes. Times that are not exact steps are stored in full. */
static bool exact_steps(double n, int64_t *steps) {
    if (!isfinite(n) || fabs(n) >= 1e12) {
        return false;
    }

    *steps = llround(n * REAL_STEPS);
    return (double)*steps / REAL_STEPS == n;
}

static bool time_after(DmBytes *b, double n, double previous) {
    int64_t now, before;

    if (exact_steps(n, &now) && exact_steps(previous, &before)) {
        int64_t delta = now - before;
        uint64_t zigzag = delta < 0 ? ((uint64_t)(-(delta + 1)) << 1) | 1 : (uint64_t)delta << 1;
        return varint(b, zigzag << 1);
    }

    uint64_t bits;
    memcpy(&bits, &n, 8);
    return varint(b, 1) && fixed32(b, (uint32_t)bits) && fixed32(b, (uint32_t)(bits >> 32));
}

static bool block(DmBytes *b, const void *data, size_t size) {
    return size <= UINT32_MAX && u32(b, (uint32_t)size) && dm_bytes(b, data, size);
}

static bool string(DmBytes *b, const char *s) {
    return block(b, s, strlen(s));
}

static uint32_t getfixed32(Input *in) {
    if (in->left < 4) {
        in->ok = false;
        return 0;
    }

    const uint8_t *p = in->p;
    uint32_t n = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    in->p += 4;
    in->left -= 4;
    return n;
}

static uint64_t getvarint(Input *in) {
    uint64_t n = 0;

    for (int shift = 0; shift < 64; shift += 7) {
        if (!in->left) {
            break;
        }

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

static uint32_t get32(Input *in) {
    if (in->schema < 10) {
        return getfixed32(in);
    }

    uint64_t n = getvarint(in);

    if (n > UINT32_MAX) {
        in->ok = false;
        return 0;
    }

    return (uint32_t)n;
}

static double getreal(Input *in) {
    double n;

    if (in->schema >= 10) {
        uint64_t tag = getvarint(in);

        if (!(tag & 1)) {
            uint64_t zigzag = tag >> 1;
            int64_t steps = zigzag & 1 ? -(int64_t)(zigzag >> 1) - 1 : (int64_t)(zigzag >> 1);
            return (double)steps / REAL_STEPS;
        }

        if (tag != 1) {
            in->ok = false;
            return 0;
        }
    }

    uint64_t bits = getfixed32(in);
    bits |= (uint64_t)getfixed32(in) << 32;
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

    uint64_t tag = getvarint(in);
    int64_t before;

    if (!(tag & 1)) {
        uint64_t zigzag = tag >> 1;
        int64_t delta = zigzag & 1 ? -(int64_t)(zigzag >> 1) - 1 : (int64_t)(zigzag >> 1);

        if (!exact_steps(previous, &before)) {
            in->ok = false;
            return 0;
        }

        return (double)(before + delta) / REAL_STEPS;
    }

    if (tag != 1) {
        in->ok = false;
        return 0;
    }

    uint64_t bits = getfixed32(in);
    bits |= (uint64_t)getfixed32(in) << 32;
    double n;
    memcpy(&n, &bits, 8);

    if (!isfinite(n)) {
        in->ok = false;
    }

    return n;
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

/* Standard CRC-32, one table lookup per byte. Undo snapshots checksum the whole project on every
   edit, so this matters for large songs. */
static uint32_t crc32(const uint8_t *p, size_t size) {
    static uint32_t table[256];
    static bool ready;

    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;

            for (int b = 0; b < 8; ++b) {
                c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
            }

            table[n] = c;
        }

        ready = true;
    }

    uint32_t crc = ~0u;

    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }

    return ~crc;
}

static bool pack_pattern_mode(const DmPattern *p, DmBytes *b) {
    return u32(b, p->kind) && u32(b, p->mode) && u32(b, (uint32_t)p->event) && real(b, p->speed) &&
           real(b, p->tension) && real(b, p->skew) && real(b, p->pulse) && real(b, p->amount) &&
           real(b, p->center) && real(b, p->pitch_center) && u32(b, p->reference) &&
           u32(b, p->inverted) && u32(b, p->control_scope) && u32(b, p->port) && u32(b, p->color);
}

static bool pack_patterns(const DmProject *p, DmBytes *b) {
    if (!u32(b, p->next_pattern_uid) || !u32(b, (uint32_t)p->pattern_count)) {
        return false;
    }

    for (size_t i = 0; i < p->pattern_count; ++i) {
        const DmPattern *pat = &p->patterns[i];

        if (!u32(b, pat->uid) || !string(b, pat->name) || !string(b, pat->base_name) ||
            !u32(b, pat->duplicate_index) || !real(b, pat->length) ||
            !u32(b, (uint32_t)pat->count) || !pack_pattern_mode(pat, b)) {
            return false;
        }

        for (size_t j = 0; j < pat->count; ++j) {
            DmNote n = pat->notes[j];

            if (!time_after(b, n.beat, j ? pat->notes[j - 1].beat : 0) || !real(b, n.length) ||
                !u32(b, n.key) || !u32(b, n.velocity) || !u32(b, n.selected) || !u32(b, n.port)) {
                return false;
            }
        }
    }

    return true;
}

static bool pack_metadata(const DmProject *p, DmBytes *target) {
    DmBytes b = *target;
    bool ok = string(&b, p->name) && string(&b, p->soundfont) && real(&b, p->bpm) &&
              u32(&b, p->numerator) && u32(&b, p->denominator) && u32(&b, p->port) &&
              u32(&b, p->sequence) && u32(&b, p->source_format) && u32(&b, p->sequence_count) &&
              u32(&b, p->port_count);

    for (int i = 0; i < p->port_count && ok; ++i) {
        ok = u32(&b, p->ports[i]);
    }

    ok = ok && u32(&b, p->percussion_override);

    for (unsigned i = 0; i < 128 && ok; i++) {
        ok = u32(&b, p->percussion_keys[i]);
    }

    *target = b;
    return ok;
}

static bool pack_channels(const DmProject *p, DmBytes *target) {
    DmBytes b = *target;
    bool ok = true;

    for (int i = 0; i < 16 && ok; ++i) {
        DmChannel c = p->channels[i];
        ok = u32(&b, c.enabled) && u32(&b, c.mute) && u32(&b, c.solo) && u32(&b, c.program) &&
             u32(&b, c.volume) && u32(&b, c.pan);
    }

    for (int c = 0; c < 16 && ok; ++c) {
        ok = u32(&b, p->lane_count[c]);

        for (int i = 0; i < p->lane_count[c] && ok; ++i) {
            ok = string(&b, p->lane_names[c][i]);
        }
    }

    *target = b;
    return ok;
}

static bool pack_clips(const DmProject *p, DmBytes *target) {
    DmBytes b = *target;
    bool ok = u32(&b, (uint32_t)p->clip_count);

    for (size_t i = 0; i < p->clip_count && ok; ++i) {
        DmClip c = p->clips[i];
        ok = time_after(&b, c.beat, i ? p->clips[i - 1].beat : 0) && u32(&b, c.pattern) &&
             u32(&b, c.channel) && u32(&b, c.selected) && real(&b, c.length) && u32(&b, c.lane) &&
             real(&b, c.offset);
    }

    *target = b;
    return ok;
}

static bool pack_automation(const DmProject *p, DmBytes *target) {
    DmBytes b = *target;
    bool ok = u32(&b, (uint32_t)p->automation_count);

    for (size_t i = 0; i < p->automation_count && ok; ++i) {
        DmAutomation a = p->automation[i];
        ok = time_after(&b, a.beat, i ? p->automation[i - 1].beat : 0) && u32(&b, a.type) &&
             u32(&b, a.channel) && u32(&b, a.a) && u32(&b, a.b) && u32(&b, (uint32_t)a.pattern) &&
             u32(&b, (uint32_t)a.curve) && u32(&b, a.anchor) && real(&b, a.bend) &&
             u32(&b, a.ramp) && u32(&b, a.port);
    }

    *target = b;
    return ok;
}

static bool pack_events(const DmProject *p, DmBytes *target) {
    DmBytes b = *target;
    bool ok = u32(&b, (uint32_t)p->event_count);

    for (size_t i = 0; i < p->event_count && ok; ++i) {
        const DmEvent *e = &p->events[i];
        ok = time_after(&b, e->beat, i ? p->events[i - 1].beat : 0) && u32(&b, e->track) &&
             u32(&b, e->port) && block(&b, e->message.data, e->message.size);
    }

    *target = b;
    return ok;
}

static bool pack_views(const DmProject *p, DmBytes *b) {
    if (!u32(b, (uint32_t)p->view_count)) {
        return false;
    }

    for (size_t i = 0; i < p->view_count; ++i) {
        const DmView *view = &p->views[i];

        if (!u32(b, view->port) || !u32(b, view->sequence) ||
            !block(b, view->state.data, view->state.size)) {
            return false;
        }
    }

    return true;
}

bool dm_pack(const DmProject *p, DmBytes *out) {
    DmBytes b = {0};
    bool ok = pack_metadata(p, &b);
    ok = ok && pack_channels(p, &b);
    ok = ok && block(&b, p->original.data, p->original.size) && pack_patterns(p, &b);
    ok = ok && pack_clips(p, &b);
    ok = ok && pack_automation(p, &b);
    ok = ok && pack_events(p, &b);
    ok = ok && pack_views(p, &b);
    ok = ok && u32(&b, (uint32_t)p->port_mix_count);

    for (size_t i = 0; i < p->port_mix_count && ok; i++) {
        DmPortMix mix = p->port_mix[i];
        ok = u32(&b, mix.port) && u32(&b, mix.channel) && u32(&b, mix.volume) && u32(&b, mix.pan) &&
             u32(&b, mix.mute) && u32(&b, mix.solo) && u32(&b, (uint32_t)mix.program);
    }

    /* The body is compressed; the header and checksum describe the stored bytes. */
    DmBytes stored = {0};
    ok = ok && dm_compress(b.data, b.size, &stored) && stored.size <= UINT32_MAX;
    ok = ok && dm_bytes(out, "DUCKMIDI", 8) && fixed32(out, DM_PROJECT_SCHEMA) &&
         fixed32(out, (uint32_t)stored.size) && fixed32(out, crc32(stored.data, stored.size)) &&
         dm_bytes(out, stored.data, stored.size);
    dm_bytes_free(&stored);
    dm_bytes_free(&b);

    if (!ok) {
        dm_bytes_free(out);
    }

    return ok;
}

static bool unpack_note(DmPattern *pat, Input *in) {
    DmNote n = {0};
    n.beat = get_time_after(in, pat->count ? pat->notes[pat->count - 1].beat : 0);
    n.length = getreal(in);
    uint32_t key = get32(in), vel = get32(in), selected = get32(in), port = get32(in);

    if (!in->ok || key > 127 || vel < 1 || vel > 127 || selected > 1 || port >= DM_NOTE_PORTS) {
        return false;
    }

    n.key = (uint8_t)key;
    n.velocity = (uint8_t)vel;
    n.selected = (uint8_t)selected;
    n.port = (uint8_t)port;

    if (!dm_note_add(pat, n)) {
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

static bool automation_event_valid(int event) {
    return event == -1 || event == -2 || event == -4 || event == -5 || (event >= 0 && event <= 127);
}

static bool unpack_lfo_center(DmPattern *p, Input *in) {
    p->center = getreal(in);
    p->pitch_center = getreal(in);
    p->reference = (int)get32(in);
    uint32_t inverted = get32(in);
    p->inverted = inverted != 0;

    if (inverted > 1 || p->center != floor(p->center) || p->center < 0 || p->center > 127 ||
        p->pitch_center < 0 || p->pitch_center > 127 || p->reference < 0 || p->reference > 127) {
        return false;
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

    p->control_scope = (int)get32(in);
    p->port = (int)get32(in);
    uint32_t color = schema_10(in) ? get32(in) : 0;

    if (color > DM_PATTERN_COLORS) {
        return false;
    }

    p->color = (uint8_t)color;
    return in->ok && p->port >= 0 && p->port < DM_NOTE_PORTS && p->control_scope >= 0 &&
           p->control_scope <= 2 && p->kind >= 0 && p->kind <= 1 && p->mode >= 0 && p->mode <= 1 &&
           !(p->mode && (p->event == -1 || p->event == -4 || p->event == -5)) &&
           !(p->mode && (p->event == 5 || p->event == 64 || p->event == 66)) &&
           automation_event_valid(p->event) && p->speed >= .03125 && p->speed <= 16 &&
           fabs(p->tension) <= 1 && fabs(p->skew) <= 1 && p->pulse >= .05 && p->pulse <= .95 &&
           p->amount >= 0 && p->amount <= 1;
}

static bool unpack_pattern_notes(DmPattern *pat, Input *in, uint32_t notes) {
    for (uint32_t j = 0; j < notes; ++j) {
        if (!unpack_note(pat, in)) {
            return false;
        }
    }

    return true;
}

static int uid_compare(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static bool pattern_uids_valid(const DmProject *p) {
    uint32_t *ids = malloc((p->pattern_count + 1) * sizeof(uint32_t));

    if (!ids) {
        return false;
    }

    for (size_t i = 0; i < p->pattern_count; ++i) {
        ids[i] = p->patterns[i].uid;
    }

    qsort(ids, p->pattern_count, sizeof(uint32_t), uid_compare);
    bool valid = true;

    for (size_t i = 1; i < p->pattern_count; ++i) {
        valid &= ids[i - 1] != ids[i];
    }

    free(ids);
    return valid;
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
    return in->ok && pattern_uids_valid(p);
}

static bool valid_meta(const DmBytes *m) {
    if (m->data[0] != MIDI_META) {
        return true;
    }

    if (m->data[1] == MIDI_META_TEMPO) {
        return m->size == 6 && m->data[2] == 3 && (m->data[3] || m->data[4] || m->data[5]);
    }

    if (m->data[1] == MIDI_META_TIME) {
        return m->size == 7 && m->data[2] == 4 && m->data[3] > 0 && m->data[4] <= 7;
    }

    return true;
}

static bool valid_channel_message(const DmBytes *m) {
    size_t count = (m->data[0] & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3;

    if (m->size != count) {
        return false;
    }

    for (size_t i = 1; i < count; ++i) {
        if (m->data[i] > 127) {
            return false;
        }
    }

    return true;
}

bool dm_message_valid(const DmBytes *m) {
    if (m->size < 2) {
        return false;
    }

    int status = m->data[0];

    if (status >= MIDI_STATUS && status < MIDI_SYSTEM) {
        return valid_channel_message(m);
    }

    if (status != MIDI_META && status != MIDI_SYSEX && status != MIDI_SYSEX_END) {
        return false;
    }

    size_t i = status == MIDI_META ? 2 : 1;
    uint32_t n = 0;
    int count = 0;
    uint8_t v;

    do {
        if (i >= m->size || ++count > 4) {
            return false;
        }

        v = m->data[i++];
        n = (n << 7) | (v & 127);
    } while (v & 128);
    return n == m->size - i && valid_meta(m);
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
        bool valid_lane =
            c.channel >= 0 && c.channel < 16 && c.lane >= 0 && c.lane <= p->lane_count[c.channel];
        bool valid_pattern = c.pattern >= 0 && (size_t)c.pattern < p->pattern_count;

        if (!in->ok || selected > 1 || !valid_lane || !valid_pattern ||
            (p->patterns[c.pattern].kind != 0) != (c.lane > 0) || !dm_clip_add(p, c)) {
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

        if (a.pattern >= 0 && (size_t)a.pattern < p->pattern_count && p->patterns[a.pattern].kind &&
            a.beat > p->patterns[a.pattern].length) {
            return false;
        }

        if (!in->ok || !dm_automation_add(p, a)) {
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
                 volume = get32(in), pan = get32(in);

        if (enabled > 1 || mute > 1 || solo > 1 || (program > 127 && program != UINT32_MAX) ||
            volume > 127 || pan > 127) {
            return false;
        }

        p->channels[i] = (DmChannel){enabled != 0,     mute != 0,   solo != 0,
                                     (int32_t)program, (int)volume, (int)pan};
    }

    for (int c = 0; c < 16; ++c) {
        uint32_t count = get32(in);

        if (count > DM_LANES) {
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

static bool timing_valid(const DmProject *p) {
    if (p->bpm < 60000000.0 / 16777215 || p->bpm > 60000000) {
        return false;
    }

    if (p->numerator < 1 || p->numerator > 255) {
        return false;
    }

    return p->denominator >= 1 && p->denominator <= 128 &&
           (p->denominator & (p->denominator - 1)) == 0;
}

static bool source_valid(const DmProject *p) {
    if (p->port < 0 || p->port > DM_PORT_COMBINED || p->source_format < 0 || p->source_format > 2) {
        return false;
    }

    if (p->sequence_count < 1 || p->sequence_count > 65535 || p->sequence < 0 ||
        p->sequence >= p->sequence_count) {
        return false;
    }

    return p->port_count >= 1 && p->port_count <= 256;
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

    if (!timing_valid(p) || !source_valid(p)) {
        return false;
    }

    return in->ok;
}

static bool unpack_document(DmProject *p, const unsigned char *data, size_t size, char *error,
                            size_t cap, bool allow_views);

static int view_index(const DmProject *p, int port, int sequence) {
    for (size_t i = 0; i < p->view_count; ++i) {
        if (p->views[i].port == port && p->views[i].sequence == sequence) {
            return (int)i;
        }
    }

    return -1;
}

static bool valid_view_key(const DmProject *p, int port, int sequence) {
    if (sequence < 0 || sequence >= p->sequence_count) {
        return false;
    }

    if (p->port == DM_PORT_COMBINED) {
        return port == DM_PORT_COMBINED;
    }

    for (int i = 0; i < p->port_count; ++i) {
        if (p->ports[i] == port) {
            return true;
        }
    }

    return false;
}

static bool validate_view(const DmProject *parent, const DmView *view) {
    DmProject child;
    dm_project_init(&child);
    char error[256];
    bool ok =
        unpack_document(&child, view->state.data, view->state.size, error, sizeof(error), false);

    if (ok) {
        ok = child.original.size == 0 && child.port == view->port &&
             child.sequence == view->sequence && child.source_format == parent->source_format &&
             child.sequence_count == parent->sequence_count;
    }

    if (ok) {
        ok = child.port_count == parent->port_count &&
             memcmp(child.ports, parent->ports, (size_t)parent->port_count * sizeof(int)) == 0;
    }

    dm_project_free(&child);
    return ok;
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

        if (!valid_view_key(p, view.port, view.sequence) ||
            view_index(p, view.port, view.sequence) >= 0) {
            return false;
        }

        if (!getblock(in, &view.state)) {
            return false;
        }

        if (!validate_view(p, &view)) {
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

static bool unpack_body(DmProject *p, Input *in, bool allow_views) {
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
    bool assigned[DM_PIANO_KEYS + 1] = {0};

    for (unsigned i = 0; i < 128; i++) {
        uint32_t key = get32(in);

        if (key > DM_PIANO_KEYS || (!override && key) || (key && assigned[key])) {
            return false;
        }

        p->percussion_keys[i] = (uint8_t)key;
        assigned[key] = key != 0;
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

        if (program > 128) {
            return false;
        }

        if (!in->ok || !port || port >= DM_NOTE_PORTS || channel > 15 || volume > 127 ||
            pan > 127 || mute > 1 || solo > 1) {
            return false;
        }

        unsigned part = port * 16 + channel;

        if (seen[part]) {
            return false;
        }

        seen[part] = true;

        if (!dm_port_mix_set(p, (DmPortMix){(int)port, (int)channel, (int)volume, (int)pan,
                                            mute != 0, solo != 0, (int)program})) {
            return false;
        }
    }

    if (p->view_count && !p->original.size) {
        return false;
    }

    return in->ok && in->left == 0;
}

static bool unpack_document(DmProject *p, const unsigned char *data, size_t size, char *error,
                            size_t cap, bool allow_views) {
    if (size < 20 || memcmp(data, "DUCKMIDI", 8)) {
        snprintf(error, cap, DM_TEXT_NOT_DUCKMIDI_PROJECT);
        return false;
    }

    Input in = {data + 8, size - 8, true, 0};
    uint32_t version = getfixed32(&in), payload = getfixed32(&in), crc = getfixed32(&in);

    if (version != DM_PROJECT_SCHEMA && version != DM_PROJECT_SCHEMA_PREVIOUS) {
        snprintf(error, cap, DM_TEXT_UNSUPPORTED_PROJECT_SCHEMA, version,
                 DM_PROJECT_SCHEMA_PREVIOUS, DM_PROJECT_SCHEMA);
        return false;
    }

    in.schema = version;

    if (payload != in.left || crc != crc32(in.p, in.left)) {
        snprintf(error, cap, DM_TEXT_PROJECT_TRUNCATED_CHECKSUM);
        return false;
    }

    /* Schema 10 bodies are compressed. */
    DmBytes body = {0};

    if (version >= 10) {
        if (!dm_decompress(in.p, in.left, 1024u * 1024u * 1024u, &body)) {
            snprintf(error, cap, DM_TEXT_PROJECT_CONTAINS_INVALID);
            return false;
        }

        in.p = body.data;
        in.left = body.size;
    }

    DmProject next;
    dm_project_init(&next);
    bool unpacked = unpack_body(&next, &in, allow_views);
    dm_bytes_free(&body);

    if (!unpacked) {
        dm_project_free(&next);
        snprintf(error, cap, DM_TEXT_PROJECT_CONTAINS_INVALID);
        return false;
    }

    if (version == DM_PROJECT_SCHEMA_PREVIOUS) {
        dm_project_upgrade_previous(&next);
    }

    dm_project_free(p);
    *p = next;
    return true;
}

bool dm_unpack(DmProject *p, const unsigned char *data, size_t size, char *error, size_t cap) {
    return unpack_document(p, data, size, error, cap, true);
}

static bool store_view(DmProject *p, DmBytes *snapshot) {
    int index = view_index(p, p->port, p->sequence);
    DmView updated = {p->port, p->sequence, *snapshot};

    if (index < 0) {
        if (p->view_count >= 4096 ||
            !dm_reserve((void **)&p->views, &p->view_capacity, p->view_count + 1, sizeof(DmView))) {
            return false;
        }

        p->views[p->view_count++] = updated;
    } else {
        dm_bytes_free(&p->views[index].state);
        memmove(p->views + index, p->views + index + 1,
                (p->view_count - (size_t)index - 1) * sizeof(DmView));
        p->views[p->view_count - 1] = updated;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}

static bool copy_conductor(DmProject *to, const DmProject *from) {
    to->bpm = from->bpm;
    to->numerator = from->numerator;
    to->denominator = from->denominator;

    for (size_t i = 0; i < to->event_count; ++i) {
        if (dm_conductor(&to->events[i].message)) {
            dm_bytes_free(&to->events[i].message);
        }
    }

    size_t write = 0;

    for (size_t i = 0; i < to->event_count; ++i) {
        if (to->events[i].message.size) {
            to->events[write++] = to->events[i];
        }
    }

    to->event_count = write;

    for (size_t i = 0; i < from->event_count; ++i) {
        const DmEvent *e = &from->events[i];

        if (dm_conductor(&e->message) &&
            !dm_event_add(to, e->beat, e->track, e->port, &e->message)) {
            return false;
        }
    }

    return true;
}

static bool sync_conductor(const DmProject *p, DmProject *next, char *error, size_t cap) {
    if (next->sequence == p->sequence) {
        return copy_conductor(next, p);
    }

    for (size_t i = p->view_count; i > 0; --i) {
        const DmView *view = &p->views[i - 1];

        if (view->sequence != next->sequence) {
            continue;
        }

        if (view->port == next->port) {
            return true;
        }

        DmProject recent;
        dm_project_init(&recent);
        bool ok = unpack_document(&recent, view->state.data, view->state.size, error, cap, false);

        if (ok) {
            ok = copy_conductor(next, &recent);
        }

        dm_project_free(&recent);
        return ok;
    }

    return true;
}

static void activate_view(DmProject *p, DmProject *next) {
    DmBytes original = p->original;
    DmView *views = p->views;
    size_t count = p->view_count, capacity = p->view_capacity;
    snprintf(next->name, sizeof(next->name), "%s", p->name);
    snprintf(next->soundfont, sizeof(next->soundfont), "%s", p->soundfont);
    next->percussion_override = p->percussion_override;
    memcpy(next->percussion_keys, p->percussion_keys, sizeof(next->percussion_keys));
    p->original = (DmBytes){0};
    p->views = NULL;
    p->view_count = p->view_capacity = 0;
    dm_project_free(p);
    dm_bytes_free(&next->original);
    next->original = original;
    next->views = views;
    next->view_count = count;
    next->view_capacity = capacity;
    *p = *next;
    memset(next, 0, sizeof(*next));
}

bool dm_view_switch(DmProject *p, int port, int sequence, char *error, size_t cap) {
    if (!p->original.size || !valid_view_key(p, port, sequence)) {
        snprintf(error, cap, DM_TEXT_INVALID_SOURCE_PORT);
        return false;
    }

    if (port == p->port && sequence == p->sequence) {
        return true;
    }

    DmProject current = *p;
    current.original = (DmBytes){0};
    current.views = NULL;
    current.view_count = current.view_capacity = 0;
    DmBytes snapshot = {0};

    if (!dm_pack(&current, &snapshot)) {
        snprintf(error, cap, DM_TEXT_CANNOT_RETAIN_CURRENT);
        return false;
    }

    DmProject next;
    dm_project_init(&next);
    int index = view_index(p, port, sequence);
    bool ok = index >= 0 ? unpack_document(&next, p->views[index].state.data,
                                           p->views[index].state.size, error, cap, false)
                         : dm_import(&next, p->original.data, p->original.size, port, sequence,
                                     error, cap);

    if (ok) {
        ok = sync_conductor(p, &next, error, cap);
    }

    if (ok) {
        ok = store_view(p, &snapshot);
    }

    if (ok) {
        activate_view(p, &next);
    } else if (!*error) {
        snprintf(error, cap, DM_TEXT_UNABLE_RETAIN_VIEW);
    }

    dm_bytes_free(&snapshot);
    dm_project_free(&next);
    return ok;
}
