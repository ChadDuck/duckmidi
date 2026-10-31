#include "duck.h"
#include "text/en.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *p, *end;
    bool ok;
} Reader;

typedef struct {
    uint64_t tick;
    int track, port;
    size_t order;
    int phase;       /* Endpoints, restoration, setup, then notes at a shared tick. */
    bool persistent; /* Detached source points retain explicit restoration values. */
    DmBytes bytes;
} Raw;

typedef struct {
    Raw *items;
    size_t count, capacity;
    int format, tracks, division;
    uint64_t end_tick;
} Midi;

static uint32_t read_be(Reader *r, int count) {
    if ((size_t)(r->end - r->p) < (size_t)count) {
        r->ok = false;
        return 0;
    }

    uint32_t v = 0;

    while (count--) {
        v = v * 256 + *r->p++;
    }

    return v;
}

static uint32_t read_vlq(Reader *r) {
    uint32_t value = 0;

    for (int i = 0; i < 4; ++i) {
        uint32_t b = read_be(r, 1);
        value = (value << 7) | (b & 127);

        if (!r->ok || !(b & 128)) {
            return value;
        }
    }

    r->ok = false;
    return 0;
}

static bool byte(DmBytes *b, unsigned v) {
    uint8_t n = (uint8_t)v;
    return dm_bytes(b, &n, 1);
}

static bool be(DmBytes *b, uint32_t v, int count) {
    for (int i = count - 1; i >= 0; --i) {
        if (!byte(b, v >> (i * 8))) {
            return false;
        }
    }

    return true;
}

static bool vlq(DmBytes *b, uint32_t v) {
    if (v > 0x0fffffff) {
        return false;
    }

    uint8_t n[4];
    int count = 0;
    n[count++] = v & 127;

    while ((v >>= 7)) {
        n[count++] = (v & 127) | 128;
    }

    while (count) {
        if (!byte(b, n[--count])) {
            return false;
        }
    }

    return true;
}

static void midi_free(Midi *m) {
    for (size_t i = 0; i < m->count; ++i) {
        dm_bytes_free(&m->items[i].bytes);
    }

    free(m->items);
    memset(m, 0, sizeof(*m));
}

static bool raw_add(Midi *m, Raw *raw) {
    if (!dm_reserve((void **)&m->items, &m->capacity, m->count + 1, sizeof(*raw))) {
        return false;
    }

    raw->order = m->count;
    m->items[m->count++] = *raw;
    return true;
}

static bool parse_channel_data(Reader *r, unsigned status, DmBytes *msg) {
    int count = (status & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 1 : 2;

    for (int i = 0; i < count; ++i) {
        unsigned value = read_be(r, 1);

        if (!r->ok || value > 127 || !byte(msg, value)) {
            return false;
        }
    }

    return true;
}

static bool parse_system_data(Reader *r, unsigned status, DmBytes *msg) {
    if (status == MIDI_META) {
        if (!byte(msg, read_be(r, 1))) {
            return false;
        }
    } else if (status != MIDI_SYSEX && status != MIDI_SYSEX_END) {
        return false;
    }

    uint32_t size = read_vlq(r);

    if (!r->ok || (size_t)(r->end - r->p) < size) {
        return false;
    }

    if (!vlq(msg, size) || !dm_bytes(msg, r->p, size)) {
        return false;
    }

    r->p += size;
    return true;
}

static bool parse_message(Reader *r, unsigned *running, DmBytes *msg) {
    if (r->p >= r->end) {
        return false;
    }

    unsigned status = *r->p;

    if (status & 128) {
        ++r->p;
    } else {
        status = *running;

        if (status < MIDI_STATUS || status >= MIDI_SYSTEM) {
            return false;
        }
    }

    if (!byte(msg, status)) {
        return false;
    }

    if (status < MIDI_SYSTEM) {
        *running = status;
        return parse_channel_data(r, status, msg);
    }

    *running = 0;
    return parse_system_data(r, status, msg);
}

static bool parse_track(Reader *r, Midi *m, int track) {
    uint64_t tick = 0;
    unsigned running = 0;
    int port = 0;

    while (r->ok && r->p < r->end) {
        tick += read_vlq(r);

        if (tick > UINT32_MAX) {
            return false;
        }

        Raw raw = {0};
        raw.tick = tick;
        raw.track = track;
        raw.port = port;

        if (!r->ok || !parse_message(r, &running, &raw.bytes)) {
            dm_bytes_free(&raw.bytes);
            return false;
        }

        if (raw.bytes.size == 4 && raw.bytes.data[0] == MIDI_META &&
            raw.bytes.data[1] == MIDI_META_PORT && raw.bytes.data[2] == 1) {
            port = raw.bytes.data[3];
            raw.port = port;
        }

        bool eot = raw.bytes.size >= 3 && raw.bytes.data[0] == MIDI_META &&
                   raw.bytes.data[1] == MIDI_META_END;

        if (!raw_add(m, &raw)) {
            dm_bytes_free(&raw.bytes);
            return false;
        }

        m->end_tick = tick > m->end_tick ? tick : m->end_tick;

        if (eot) {
            return true;
        }
    }

    return r->ok;
}

static bool unwrap_rmid(const uint8_t **input, size_t *length) {
    const uint8_t *data = *input;
    size_t size = *length;

    if (size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, DM_TEXT_RMID, 4)) {
        size_t pos = 12;
        bool found = false;

        while (pos + 8 <= size) {
            uint32_t n = (uint32_t)data[pos + 4] | (uint32_t)data[pos + 5] << 8 |
                         (uint32_t)data[pos + 6] << 16 | (uint32_t)data[pos + 7] << 24;

            if (n > size - pos - 8) {
                return false;
            }

            if (!memcmp(data + pos, "data", 4)) {
                data += pos + 8;
                size = n;
                found = true;
                break;
            }

            pos += 8 + (size_t)n + (n & 1);
        }

        if (!found) {
            return false;
        }
    }

    *input = data;
    *length = size;
    return true;
}

static bool division_valid(int division) {
    if (division & 0x8000) {
        int fps = -(int)(int8_t)(division >> 8);

        if (!(division & 255) || (fps != 24 && fps != 25 && fps != 29 && fps != 30)) {
            return false;
        }
    }

    return true;
}

static bool parse_header(Midi *m, Reader *r, size_t size) {
    if (size < 14 || memcmp(r->p, "MThd", 4)) {
        return false;
    }

    r->p += 4;
    uint32_t header = read_be(r, 4);

    if (header < 6 || (size_t)(r->end - r->p) < header) {
        return false;
    }

    m->format = (int)read_be(r, 2);
    m->tracks = (int)read_be(r, 2);
    m->division = (int)read_be(r, 2);
    r->p += header - 6;

    if (m->format > 2 || m->tracks < 1 || !m->division || (m->format == 0 && m->tracks != 1)) {
        return false;
    }

    if (!division_valid(m->division)) {
        return false;
    }

    return true;
}

static bool parse_midi(Midi *m, const uint8_t *data, size_t size) {
    if (!unwrap_rmid(&data, &size)) {
        return false;
    }

    Reader r = {data, data + size, true};

    if (!parse_header(m, &r, size)) {
        return false;
    }

    int tracks = 0;

    while (r.p < r.end && tracks < m->tracks) {
        if (r.end - r.p < 8) {
            return false;
        }

        bool track = !memcmp(r.p, "MTrk", 4);
        r.p += 4;
        uint32_t n = read_be(&r, 4);

        if ((size_t)(r.end - r.p) < n) {
            return false;
        }

        Reader sub = {r.p, r.p + n, true};
        r.p += n;

        if (track && !parse_track(&sub, m, tracks++)) {
            return false;
        }
    }

    return tracks == m->tracks;
}

static int raw_compare(const void *a, const void *b) {
    const Raw *x = a, *y = b;

    if (x->tick != y->tick) {
        return x->tick < y->tick ? -1 : 1;
    }

    return x->order < y->order ? -1 : x->order > y->order;
}

/* Non-commercial/development ID followed by an application namespace. See
   agents/MIDI-METADATA.md for the current, deliberately single-version format. */
static const uint8_t editor_tag[] = {0x7d, 'D', 'U', 'C', 'K', 'M', 'I', 'D', 'I'};

static Reader meta_payload(const DmBytes *b) {
    Reader r = {b->data, b->data + b->size, true};

    if (b->size < 3 || read_be(&r, 1) != MIDI_META || read_be(&r, 1) != MIDI_META_SEQUENCER) {
        r.ok = false;
        return r;
    }

    uint32_t size = read_vlq(&r);
    r.ok = r.ok && size == (size_t)(r.end - r.p);
    return r;
}

static bool editor_metadata(const DmBytes *b) {
    Reader r = meta_payload(b);
    return r.ok && (size_t)(r.end - r.p) >= sizeof(editor_tag) &&
           !memcmp(r.p, editor_tag, sizeof(editor_tag));
}

/* Hash decoded events, rather than file bytes: running-status and VLQ encoding
   differences do not invalidate the snapshot. Track order/timing remain significant. */
static void fingerprint_bytes(uint64_t *hash, const uint8_t *p, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        *hash = (*hash ^ p[i]) * UINT64_C(1099511628211);
    }
}

static void fingerprint_number(uint64_t *hash, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        uint8_t b = (uint8_t)(value >> (i * 8));
        fingerprint_bytes(hash, &b, 1);
    }
}

static bool sequencer_metadata(const DmBytes *b) {
    return b->size >= 2 && b->data[0] == MIDI_META && b->data[1] == MIDI_META_SEQUENCER;
}

static uint64_t midi_fingerprint(const Midi *m, bool strip_foreign) {
    uint64_t hash = UINT64_C(14695981039346656037);
    fingerprint_number(&hash, m->format);
    fingerprint_number(&hash, m->tracks);
    fingerprint_number(&hash, m->division);

    for (size_t i = 0; i < m->count; ++i) {
        const Raw *r = &m->items[i];

        if (editor_metadata(&r->bytes) || (strip_foreign && sequencer_metadata(&r->bytes))) {
            continue;
        }

        fingerprint_number(&hash, r->track);
        fingerprint_number(&hash, r->tick);
        fingerprint_number(&hash, r->bytes.size);
        fingerprint_bytes(&hash, r->bytes.data, r->bytes.size);
    }

    return hash;
}

/* Re-encode in source track order without changing timing, division or routing.
   A replacement editor event is inserted at tick zero of track zero. */
static bool metadata_encode(const Midi *m, DmBytes *out, bool strip, const DmBytes *replacement) {
    bool ok = dm_bytes(out, "MThd", 4) && be(out, 6, 4) && be(out, m->format, 2) &&
              be(out, m->tracks, 2) && be(out, m->division, 2);

    for (int t = 0; t < m->tracks && ok; ++t) {
        DmBytes track = {0};
        uint64_t last = 0;

        if (!t && replacement && replacement->size) {
            ok = byte(&track, 0) && dm_bytes(&track, replacement->data, replacement->size);
        }

        for (size_t i = 0; i < m->count && ok; ++i) {
            const Raw *r = &m->items[i];

            if (r->track != t || editor_metadata(&r->bytes) ||
                (strip && sequencer_metadata(&r->bytes))) {
                continue;
            }

            ok = r->tick >= last && r->tick - last <= 0x0fffffff &&
                 vlq(&track, (uint32_t)(r->tick - last)) &&
                 dm_bytes(&track, r->bytes.data, r->bytes.size);
            last = r->tick;
        }

        ok = ok && track.size <= UINT32_MAX && dm_bytes(out, "MTrk", 4) &&
             be(out, (uint32_t)track.size, 4) && dm_bytes(out, track.data, track.size);
        dm_bytes_free(&track);
    }

    if (!ok) {
        dm_bytes_free(out);
    }

    return ok;
}

static bool remove_embedded_source(DmBytes *source, bool strip_foreign) {
    if (!source->size) {
        return true;
    }

    Midi m = {0};
    bool ok = parse_midi(&m, source->data, source->size), found = false;

    for (size_t i = 0; i < m.count; ++i) {
        found |= editor_metadata(&m.items[i].bytes) ||
                 (strip_foreign && sequencer_metadata(&m.items[i].bytes));
    }

    DmBytes clean = {0};

    if (ok && found) {
        ok = metadata_encode(&m, &clean, strip_foreign, NULL);

        if (ok) {
            dm_bytes_free(source);
            *source = clean;
        }
    }

    midi_free(&m);
    return ok;
}

static void remove_embedded_events(DmProject *p, bool strip_foreign) {
    size_t write = 0;

    for (size_t i = 0; i < p->event_count; ++i) {
        if (editor_metadata(&p->events[i].message) ||
            (strip_foreign && sequencer_metadata(&p->events[i].message))) {
            dm_bytes_free(&p->events[i].message);
        } else {
            p->events[write++] = p->events[i];
        }
    }

    p->event_count = write;
}

/* 0: absent; 1: restored; 2: stale (ordinary import); -1: malformed/unsupported. */
static int restore_editor(DmProject *p, const Midi *m, int port, int sequence, char *error,
                          size_t cap) {
    const DmBytes *metadata = NULL;

    for (size_t i = 0; i < m->count; ++i) {
        if (!editor_metadata(&m->items[i].bytes)) {
            continue;
        }

        if (metadata || m->items[i].track != 0 || m->items[i].tick != 0) {
            snprintf(error, cap, DM_TEXT_INVALID_DUCKMIDI_METADATA);
            return -1;
        }

        metadata = &m->items[i].bytes;
    }

    if (!metadata) {
        return 0;
    }

    Reader r = meta_payload(metadata);
    r.p += sizeof(editor_tag);
    uint32_t version = read_be(&r, 4);

    if (!r.ok || version != 1) {
        snprintf(error, cap, DM_TEXT_UNSUPPORTED_MIDI_EDITOR);
        return -1;
    }

    uint64_t hash = (uint64_t)read_be(&r, 4) << 32;
    hash |= read_be(&r, 4);
    DmProject next;
    dm_project_init(&next);
    bool ok = r.ok && dm_unpack(&next, r.p, (size_t)(r.end - r.p), error, cap);

    if (!ok) {
        dm_project_free(&next);

        if (!r.ok) {
            snprintf(error, cap, DM_TEXT_TRUNCATED_MIDI_EDITOR);
        }

        return -1;
    }

    if (hash != midi_fingerprint(m, false)) {
        dm_project_free(&next);
        snprintf(error, cap, DM_TEXT_MIDI_IMPORTED_STALE);
        return 2;
    }

    /* A normal open (port -1) restores the saved active view. Explicit view
       requests retain the existing source-view switching semantics. */
    ok = remove_embedded_source(&next.original, false);

    if (!ok) {
        snprintf(error, cap, DM_TEXT_INVALID_MIDI_SOURCE);
    }

    if (ok && port >= 0 && (port != next.port || sequence != next.sequence)) {
        ok = dm_view_switch(&next, port, sequence, error, cap);
    }

    if (!ok) {
        dm_project_free(&next);
        return -1;
    }

    dm_project_free(p);
    *p = next;
    snprintf(error, cap, DM_TEXT_MIDI_IMPORTED_DUCKMIDI);
    return 1;
}

static double tick_beat(const Midi *m, uint64_t tick) {
    if (!(m->division & 0x8000)) {
        return (double)tick / m->division;
    }

    int code = -(int)(int8_t)(m->division >> 8);
    double fps = code == 29 ? 30000.0 / 1001 : code;
    return (double)tick / (fps * (m->division & 255)) * 2;
}

static bool active_raw(const DmProject *p, const Raw *r) {
    if (p->source_format == 2 && r->track != p->sequence) {
        return false;
    }

    return r->bytes.data[0] == MIDI_META || p->port == DM_PORT_COMBINED || r->port == p->port;
}

bool dm_event_add(DmProject *p, double beat, int track, int port, const DmBytes *b) {
    if (!isfinite(beat) || beat < 0 || !b->size) {
        return false;
    }

    if (!dm_reserve((void **)&p->events, &p->event_capacity, p->event_count + 1, sizeof(DmEvent))) {
        return false;
    }

    DmEvent e = {0};
    e.beat = beat;
    e.track = track;
    e.port = port;

    if (!dm_bytes(&e.message, b->data, b->size)) {
        return false;
    }

    size_t at = p->event_count;

    while (at > 0 && p->events[at - 1].beat > beat) {
        p->events[at] = p->events[at - 1];
        --at;
    }

    p->events[at] = e;
    ++p->event_count;
    return true;
}

typedef struct {
    int next, pattern;
    size_t note;
} Pending;

typedef struct {
    int pattern[16], head[16][128], tail[16][128];
    Pending *pending;
    size_t count, capacity;
} ImportState;

static void import_init(DmProject *p, ImportState *state) {
    memset(state, 0, sizeof(*state));

    for (int c = 0; c < 16; ++c) {
        state->pattern[c] = -1;
        p->channels[c].enabled = false;

        for (int k = 0; k < 128; ++k) {
            state->head[c][k] = state->tail[c][k] = -1;
        }
    }
}

static bool import_note_on(DmProject *p, ImportState *s, const uint8_t *b, double beat) {
    int c = b[0] & MIDI_CHANNEL_MASK;

    if (s->pattern[c] < 0) {
        char name[64];
        snprintf(name, sizeof(name), DM_TEXT_CHANNEL, c + 1);
        s->pattern[c] = dm_pattern_add(p, name);

        if (s->pattern[c] < 0) {
            return false;
        }
    }

    DmPattern *pat = &p->patterns[s->pattern[c]];

    if (!dm_note_add(pat, (DmNote){beat, .25, b[1], b[2], 0, 0})) {
        return false;
    }

    if (!dm_reserve((void **)&s->pending, &s->capacity, s->count + 1, sizeof(Pending))) {
        return false;
    }

    s->pending[s->count] = (Pending){-1, s->pattern[c], pat->count - 1};
    int *first = &s->head[c][b[1]], *last = &s->tail[c][b[1]];

    if (*last >= 0) {
        s->pending[*last].next = (int)s->count;
    } else {
        *first = (int)s->count;
    }

    *last = (int)s->count++;
    p->channels[c].enabled = true;

    if (p->channels[c].program < 0) {
        p->channels[c].program = 0;
    }

    return true;
}

static void import_note_off(DmProject *p, ImportState *s, const uint8_t *b, double beat) {
    int c = b[0] & MIDI_CHANNEL_MASK, n = s->head[c][b[1]];

    if (n < 0) {
        return;
    }

    DmNote *note = &p->patterns[s->pending[n].pattern].notes[s->pending[n].note];
    note->length = fmax(1.0 / DM_PPQ, beat - note->beat);
    s->head[c][b[1]] = s->pending[n].next;

    if (s->head[c][b[1]] < 0) {
        s->tail[c][b[1]] = -1;
    }
}

static bool import_meta(DmProject *p, const Midi *m, const Raw *r, double beat) {
    const uint8_t *b = r->bytes.data;

    if (editor_metadata(&r->bytes)) {
        return true;
    }

    if (b[1] == MIDI_META_END || b[1] == MIDI_META_PORT) {
        return true;
    }

    if (b[1] == MIDI_META_TEMPO) {
        if (r->bytes.size != 6) {
            return false;
        }

        if (m->division & 0x8000) {
            return true;
        }

        int us = (b[3] << 16) | (b[4] << 8) | b[5];

        if (!us) {
            return false;
        }

        if (beat == 0) {
            p->bpm = 60000000.0 / us;
        }
    }

    if (b[1] == MIDI_META_TIME) {
        if (r->bytes.size != 7 || !b[3] || b[4] > 7) {
            return false;
        }

        if (beat == 0) {
            p->numerator = b[3];
            p->denominator = 1 << b[4];
        }
    }

    return dm_event_add(p, beat, r->track, 0, &r->bytes); /* Event ports name note ports. */
}

static bool import_one(DmProject *p, const Midi *m, ImportState *state, const Raw *r) {
    if (!active_raw(p, r)) {
        return true;
    }

    const uint8_t *b = r->bytes.data;
    int type = b[0] & MIDI_STATUS_MASK, c = b[0] & MIDI_CHANNEL_MASK;
    double beat = tick_beat(m, r->tick);

    if (type == MIDI_NOTE_ON && b[2]) {
        return import_note_on(p, state, b, beat);
    }

    if (type == MIDI_NOTE_OFF || (type == MIDI_NOTE_ON && !b[2])) {
        import_note_off(p, state, b, beat);
        return true;
    }

    if (b[0] == MIDI_META) {
        return import_meta(p, m, r, beat);
    }

    if (b[0] < MIDI_SYSTEM) {
        p->channels[c].enabled = true;

        if (type == MIDI_PROGRAM && (beat == 0 || state->pattern[c] < 0)) {
            p->channels[c].program = b[1];
        }
    }

    return dm_event_add(p, beat, r->track, 0, &r->bytes);
}

static bool pattern_controller(int type, int controller) {
    if (type == MIDI_KEY_PRESSURE || type == MIDI_CHANNEL_PRESSURE || type == MIDI_PITCH_BEND) {
        return true;
    }

    return type == MIDI_CONTROL && controller != MIDI_CC_BANK && controller != MIDI_CC_BANK_LSB;
}

static void compact_events(DmProject *p) {
    size_t write = 0;

    for (size_t i = 0; i < p->event_count; ++i) {
        if (p->events[i].message.size) {
            p->events[write++] = p->events[i];
        }
    }

    p->event_count = write;
}

static bool import_controllers(DmProject *p, const ImportState *s) {
    for (size_t i = 0; i < p->event_count; ++i) {
        DmEvent event = p->events[i];
        const uint8_t *b = event.message.data;
        int c = b[0] & MIDI_CHANNEL_MASK, type = b[0] & MIDI_STATUS_MASK;

        if (s->pattern[c] < 0 || !pattern_controller(type, b[1])) {
            continue;
        }

        if (type == MIDI_CONTROL && event.beat == 0 &&
            (b[1] == MIDI_CC_VOLUME || b[1] == MIDI_CC_PAN)) {
            if (b[1] == MIDI_CC_VOLUME) {
                p->channels[c].volume = b[2];
            } else {
                p->channels[c].pan = b[2];
            }
        } else {
            DmAutomation a = {
                event.beat, type, c,     b[1], event.message.size > 2 ? b[2] : 0, s->pattern[c], 0,
                0,          0,    false, 0};

            if (!dm_automation_add(p, a)) {
                return false;
            }
        }

        dm_bytes_free(&p->events[i].message);
    }

    compact_events(p);
    return true;
}

static double source_end(const DmProject *p, const Midi *m) {
    uint64_t tick = 0;

    for (size_t i = 0; i < m->count; ++i) {
        if (m->format != 2 || m->items[i].track == p->sequence) {
            if (m->items[i].tick > tick) {
                tick = m->items[i].tick;
            }
        }
    }

    return tick_beat(m, tick);
}

static void import_initial_controls(DmProject *p) {
    for (size_t i = 0; i < p->clip_count; ++i) {
        DmClip clip = p->clips[i];
        DmPattern *pat = &p->patterns[clip.pattern];
        double first = HUGE_VAL;

        for (size_t n = 0; n < pat->count; ++n) {
            first = fmin(first, pat->notes[n].beat);
        }

        for (size_t j = 0; j < p->automation_count; ++j) {
            DmAutomation a = p->automation[j];

            if (a.pattern != clip.pattern || a.type != MIDI_CONTROL || a.beat > first) {
                continue;
            }

            if (a.a == MIDI_CC_VOLUME) {
                p->channels[clip.channel].volume = a.b;
            }

            if (a.a == MIDI_CC_PAN) {
                p->channels[clip.channel].pan = a.b;
            }
        }
    }
}

static void discard_unused_programs(DmProject *p, const ImportState *state) {
    bool content[16] = {0};

    for (int c = 0; c < 16; ++c) {
        content[c] = state->pattern[c] >= 0;
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        const uint8_t *b = p->events[i].message.data;

        if (b[0] >= MIDI_SYSTEM) {
            continue;
        }

        int type = b[0] & MIDI_STATUS_MASK;
        bool setup = type == MIDI_PROGRAM ||
                     (type == MIDI_CONTROL &&
                      (b[1] == MIDI_CC_BANK || b[1] == MIDI_CC_BANK_LSB || b[1] == MIDI_CC_VOLUME ||
                       b[1] == MIDI_CC_PAN || b[1] == MIDI_CC_REVERB || b[1] == MIDI_CC_CHORUS));
        content[b[0] & MIDI_CHANNEL_MASK] |= !setup;
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        DmBytes *message = &p->events[i].message;

        if ((message->data[0] & MIDI_STATUS_MASK) == MIDI_PROGRAM &&
            !content[message->data[0] & MIDI_CHANNEL_MASK]) {
            dm_bytes_free(message);
        }
    }

    compact_events(p);

    for (int c = 0; c < 16; ++c) {
        if (!content[c]) {
            p->channels[c].enabled = false;
            p->channels[c].program = -1;
        }
    }
}

static bool import_finish(DmProject *p, ImportState *s, const Midi *m) {
    for (size_t i = 0; i < s->count; ++i) {
        DmPattern *pat = &p->patterns[s->pending[i].pattern];
        DmNote *n = &pat->notes[s->pending[i].note];
        pat->length = fmax(pat->length, n->beat + n->length);
    }

    for (int c = 0; c < 16; ++c) {
        if (s->pattern[c] >= 0 && !dm_clip_add(p, (DmClip){0, s->pattern[c], c, 0, 0, 0})) {
            return false;
        }
    }

    discard_unused_programs(p, s);

    if (!import_controllers(p, s)) {
        return false;
    }

    import_initial_controls(p);
    double end = source_end(p, m);

    for (size_t i = 0; i < p->pattern_count; ++i) {
        dm_pattern_fit(p, &p->patterns[i]);
    }

    /* Note-pattern fitting follows notes. Imported placements must also cover
       controller tails, including a message on the source's final tick. */
    for (size_t i = 0; i < p->clip_count; ++i) {
        DmClip *clip = &p->clips[i];
        double length = fmax(p->patterns[clip->pattern].length, end);

        for (size_t j = 0; j < p->automation_count; ++j) {
            DmAutomation a = p->automation[j];

            if (a.pattern == clip->pattern) {
                length = fmax(length, a.beat + 1.0 / DM_PPQ);
            }
        }

        if (length > p->patterns[clip->pattern].length) {
            clip->length = length;
        }
    }

    return true;
}

static bool import_events(DmProject *p, Midi *m) {
    ImportState state;
    import_init(p, &state);
    bool ok = true;

    for (size_t i = 0; i < m->count; ++i) {
        if (!import_one(p, m, &state, &m->items[i])) {
            ok = false;
            break;
        }
    }

    if (ok) {
        ok = import_finish(p, &state, m);
    }

    free(state.pending);
    return ok;
}

static bool port_phase(const Raw *r, int phase) {
    const uint8_t *b = r->bytes.data;

    if (phase == 0) {
        return (b[0] & MIDI_STATUS_MASK) == MIDI_NOTE_ON && b[2] > 0;
    }

    if (phase == 1) {
        return b[0] < MIDI_SYSTEM;
    }

    return b[0] == MIDI_SYSEX || b[0] == MIDI_SYSEX_END;
}

static void discover_ports(DmProject *p, const Midi *m) {
    p->port_count = 0;

    for (int phase = 0; phase < 3; ++phase) {
        for (size_t i = 0; i < m->count; ++i) {
            const Raw *r = &m->items[i];

            if (!port_phase(r, phase)) {
                continue;
            }

            bool seen = false;

            for (int j = 0; j < p->port_count; ++j) {
                seen |= p->ports[j] == r->port;
            }

            if (!seen && p->port_count < 256) {
                p->ports[p->port_count++] = r->port;
            }
        }
    }

    if (!p->port_count) {
        p->ports[p->port_count++] = 0;
    }
}

typedef struct {
    Reader input;
    DmBytes track;
    int velocity[16], channels;
    uint32_t delay;
} Mus;

static bool mus_message(Mus *mus, int status, int a, int b, int size) {
    bool ok = vlq(&mus->track, mus->delay) && byte(&mus->track, status) && byte(&mus->track, a);
    mus->delay = 0;
    return ok && (size == 2 || byte(&mus->track, b));
}

static bool mus_controller(Mus *mus, int channel, bool system) {
    static const int controls[] = {0, 0, 1, 7, 10, 11, 91, 93, 64, 67, 120, 123, 126, 127, 121};
    int control = (int)read_be(&mus->input, 1);

    if (control > 14 || (system && control < 10) || (!system && control > 9)) {
        return false;
    }

    int value = system ? (control == 12 ? mus->channels + 1 : 0) : (int)read_be(&mus->input, 1);

    if (value > 127) {
        return false;
    }

    return mus_message(mus, (control == 0 ? MIDI_PROGRAM : MIDI_CONTROL) | channel,
                       control == 0 ? value : controls[control], value, control == 0 ? 2 : 3);
}

static bool mus_event(Mus *mus, int type, int channel) {
    if (type == 3 || type == 4) {
        return mus_controller(mus, channel, type == 3);
    }

    int data = (int)read_be(&mus->input, 1);

    switch (type) {
    case 0:
        return data < 128 && mus_message(mus, MIDI_NOTE_OFF | channel, data, 0, 3);
    case 1:
        if (data & 128) {
            mus->velocity[channel] = (int)read_be(&mus->input, 1);
        }

        return mus->velocity[channel] < 128 &&
               mus_message(mus, MIDI_NOTE_ON | channel, data & MIDI_DATA_MASK,
                           mus->velocity[channel], 3);
    case 2:
        return mus_message(mus, MIDI_PITCH_BEND | channel, (data & 1) << 6, data >> 1, 3);
    default:
        return false;
    }
}

static bool mus_track(Mus *mus) {
    for (int c = 0; c < 16; ++c) {
        mus->velocity[c] = 64;
    }

    const uint8_t tempo[] = {0, MIDI_META, MIDI_META_TEMPO, 3, 7, 0xa1, 0x20};

    if (!dm_bytes(&mus->track, tempo, sizeof(tempo))) {
        return false;
    }

    while (mus->input.ok && mus->input.p < mus->input.end) {
        int descriptor = (int)read_be(&mus->input, 1), type = (descriptor >> 4) & 7;
        int channel = descriptor & 15;
        channel = channel == 15 ? MIDI_DRUM_CHANNEL : channel == MIDI_DRUM_CHANNEL ? 15 : channel;

        if (type == 6) {
            return vlq(&mus->track, mus->delay) && byte(&mus->track, MIDI_META) &&
                   byte(&mus->track, 0x2f) && byte(&mus->track, 0);
        }

        if (!mus_event(mus, type, channel)) {
            return false;
        }

        if (descriptor & 128) {
            mus->delay = read_vlq(&mus->input);
        }
    }

    return false;
}

static unsigned little16(const uint8_t *p) {
    return p[0] | ((unsigned)p[1] << 8);
}

static bool convert_mus(const uint8_t *data, size_t size, DmBytes *out) {
    if (size < 16 || memcmp(data, "MUS\x1a", 4)) {
        return false;
    }

    unsigned length = little16(data + 4), start = little16(data + 6), channels = little16(data + 8);
    unsigned instruments = little16(data + 12);

    if (channels > 15 || start < 16 + instruments * 2 || start > size || length > size - start) {
        return false;
    }

    Mus mus = {0};
    mus.input = (Reader){data + start, data + start + length, true};
    mus.channels = (int)channels;
    bool ok = mus_track(&mus) && mus.input.ok;
    const uint8_t header[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 70};

    if (ok) {
        ok = dm_bytes(out, header, sizeof(header)) && dm_bytes(out, "MTrk", 4) &&
             be(out, (uint32_t)mus.track.size, 4) && dm_bytes(out, mus.track.data, mus.track.size);
    }

    dm_bytes_free(&mus.track);
    return ok;
}

static void select_import_port(DmProject *next, const Midi *m, int port) {
    discover_ports(next, m);
    bool port_exists = false;

    for (int j = 0; j < next->port_count; ++j) {
        port_exists |= next->ports[j] == port;
    }

    if (!port_exists) {
        next->port = next->ports[0];
    }
}

bool dm_import(DmProject *p, const unsigned char *data, size_t size, int port, int sequence,
               char *error, size_t cap) {
    if (cap) {
        error[0] = 0;
    }

    if (size >= 4 && !memcmp(data, "MUS\x1a", 4)) {
        DmBytes converted = {0};
        bool ok = convert_mus(data, size, &converted);

        if (ok) {
            ok = dm_import(p, converted.data, converted.size, port, sequence, error, cap);
        } else {
            snprintf(error, cap, DM_TEXT_INVALID_TRUNCATED_DOOM);
        }

        dm_bytes_free(&converted);
        return ok;
    }

    Midi m = {0};

    if (size > 128u * 1024u * 1024u) {
        snprintf(error, cap, DM_TEXT_MIDI_IMPORT_LIMITED);
        return false;
    }

    if (!parse_midi(&m, data, size)) {
        midi_free(&m);
        snprintf(error, cap, DM_TEXT_INVALID_UNSUPPORTED_MIDI);
        return false;
    }

    int restored = restore_editor(p, &m, port, sequence, error, cap);

    if (restored == 1 || restored < 0) {
        midi_free(&m);
        return restored > 0;
    }

    DmProject next;
    dm_project_init(&next);
    next.source_format = m.format;
    next.port = port;
    next.sequence = sequence;
    next.sequence_count = m.format == 2 ? m.tracks : 1;
    select_import_port(&next, &m, port);

    /* Without an explicit port, every source port joins one arrangement on its own note port. */
    if ((port < 0 || port == DM_PORT_COMBINED) && next.port_count > 1) {
        int ports[256], count = next.port_count;
        memcpy(ports, next.ports, sizeof(ports));
        midi_free(&m);
        dm_project_free(&next);
        return dm_import_ports(p, data, size, ports, count, sequence, error, cap);
    }

    bool ok = sequence >= 0 && sequence < next.sequence_count;
    qsort(m.items, m.count, sizeof(Raw), raw_compare);

    if (ok) {
        ok = dm_bytes(&next.original, data, size) &&
             (restored != 2 || remove_embedded_source(&next.original, false)) &&
             import_events(&next, &m);
    }

    midi_free(&m);

    if (!ok) {
        dm_project_free(&next);
        snprintf(error, cap, DM_TEXT_MIDI_IMPORT_FAILED);
        return false;
    }

    dm_project_free(p);
    *p = next;
    return true;
}

static bool add_message(Midi *m, double beat, int track, const uint8_t *data, size_t size) {
    if (!isfinite(beat) || beat < 0 || beat * DM_PPQ > UINT32_MAX) {
        return false;
    }

    Raw r = {0};
    r.tick = (uint64_t)llround(beat * DM_PPQ);
    r.track = track;

    if (!dm_bytes(&r.bytes, data, size) || !raw_add(m, &r)) {
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

            if (!add_message(m, 0, c + 1, program, 2) || !add_message(m, 0, c + 1, vol, 3) ||
                !add_message(m, 0, c + 1, pan, 3)) {
                return false;
            }
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

            if (!add_message(m, 0, track, program, 2)) {
                return false;
            }

            continue;
        }

        if (!add_message(m, e->beat, track, b, e->message.size)) {
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

        if (!add_message(m, a.beat, a.channel + 1, b,
                         a.type == MIDI_PROGRAM || a.type == MIDI_CHANNEL_PRESSURE ? 2 : 3)) {
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
    return add_message(m, clip_time(clip, beat), clip->channel + 1, message,
                       (a.type & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3);
}

static bool curve_segment(const DmProject *p, Midi *m, const DmClip *clip, size_t index) {
    DmAutomation a = p->automation[index];

    if (!dm_automation_curves(a)) {
        return true;
    }

    for (size_t j = index + 1; j < p->automation_count; ++j) {
        DmAutomation b = p->automation[j];

        if (b.pattern != a.pattern || b.curve != a.curve || b.type != a.type ||
            (a.type == MIDI_CONTROL && b.a != a.a)) {
            continue;
        }

        int steps = (int)fmin(8192, fmax(1, ceil((b.beat - a.beat) * 48)));

        for (int step = 1; step < steps; ++step) {
            double t = (double)step / steps;
            int value = (int)llround(dm_curve_value(a, b, t));

            if (!curve_sample(m, clip, a, a.beat + (b.beat - a.beat) * t, value)) {
                return false;
            }
        }
        break;
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

    if (!add_message(m, clip_time(clip, a.beat), clip->channel + 1, on, 3)) {
        return false;
    }

    m->items[m->count - 1].phase = 3;

    if (!add_message(m, clip_time(clip, end), clip->channel + 1, off, 3)) {
        return false;
    }

    m->items[m->count - 1].phase = 2;
    return true;
}

static bool collect_pattern_automation(const DmProject *p, Midi *m, const DmClip *clip) {
    for (size_t j = 0; j < p->automation_count; ++j) {
        DmAutomation a = p->automation[j];

        if (a.pattern != clip->pattern || a.beat > clip_end(p, clip)) {
            continue;
        }

        if (!collect_portamento_gate(p, m, clip, j) || !curve_segment(p, m, clip, j)) {
            return false;
        }

        uint8_t message[] = {a.type | clip->channel, a.a, a.b};

        if (!add_message(m, clip_time(clip, a.beat), clip->channel + 1, message,
                         (a.type & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3)) {
            return false;
        }

        m->items[m->count - 1].phase = curve_phase(p, a);
    }

    return true;
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

        if (!add_message(m, clip_time(clip, n.beat), clip->channel + 1, on, 3) ||
            !add_message(m, clip_time(clip, end), clip->channel + 1, off, 3)) {
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
    int rpn_msb, rpn_lsb;
} ModEvent;

typedef struct {
    ModEvent *items;
    size_t count, capacity, baseline_count;
} ModList;

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
        control = 258;
    }

    return control < 0 ? -1 : (b[0] & MIDI_CHANNEL_MASK) * 260 + control;
}

static int mod_value(const uint8_t *b) {
    int type = b[0] & MIDI_STATUS_MASK;
    return type == MIDI_PITCH_BEND                                   ? b[1] + (b[2] << 7)
           : (type == MIDI_PROGRAM || type == MIDI_CHANNEL_PRESSURE) ? b[1]
                                                                     : b[2];
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

    if (cc == 7) {
        return p->channels[c].volume;
    }

    if (cc == 10) {
        return p->channels[c].pan;
    }

    if (cc == 6) {
        return 2;
    }

    if (cc == 100 || cc == 101) {
        return 127;
    }

    if (cc == 11) {
        return 127;
    }

    if (cc == 258) {
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

        if (!add_message(m, (double)e.tick / DM_PPQ, c + 1, bytes, 3)) {
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

    if (cc >= 130 && cc < 258) {
        type = MIDI_KEY_PRESSURE;
        a = cc - 130;
    }

    if (cc == 258) {
        type = MIDI_PROGRAM;
        a = value;
        b = 0;
    }

    uint8_t bytes[] = {type | c, a, b};

    if (!add_message(m, (double)e.tick / DM_PPQ, c + 1, bytes,
                     (type & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3)) {
        return false;
    }

    if (cc == 128) {
        m->items[m->count - 1].phase = 5;
    }

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
    int last = -1;
    bool ok = true;

    for (size_t i = 0; i < count && ok;) {
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
        fallback.value = mod_default(p, events[0].key);
        fallback.rpn_msb = fallback.rpn_lsb = 127;
        ModEvent chosen = mod_winner(states, used, fallback);
        chosen.tick = tick;
        /* Reassert the winning transaction when source parameters change underneath it. */
        bool range_changed = chosen.key % 260 == 259;

        if (chosen.value != last || range_changed) {
            ok = mod_emit(m, chosen, chosen.value);
            last = chosen.value;
        }
    }

    free(states);
    return ok;
}

typedef struct {
    int msb, lsb, range, cents;
    bool owned, nrpn;
    int nrpn_msb, nrpn_lsb;
} ModPitch;

static int export_compare(const void *a, const void *b);

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
        if (cc == 6) {
            state->range = value;
        }

        if (cc == 38) {
            state->cents = value;
        }

        if (cc == 96) {
            state->range = state->range < 127 ? state->range + 1 : 127;
        }

        if (cc == 97) {
            state->range = state->range > 0 ? state->range - 1 : 0;
        }
    }

    return cc == 6 || cc == 38 || (cc >= 96 && cc <= 101);
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
    qsort(source->items, source->count, sizeof(*source->items), export_compare);

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
                      .start = start};

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

    if (pat->event == -2) {
        const int cc[] = {101, 100, 6, 38, 101, 100},
                  values[] = {0, 0, dm_lfo_range(pat), 0, 127, 127};

        for (int i = 0; i < 6; ++i) {
            uint8_t bytes[] = {MIDI_CONTROL | clip->channel, cc[i], values[i]};

            if (!add_message(m, clip->beat, clip->channel + 1, bytes, 3)) {
                return false;
            }
        }
    }

    DmAutomation a = {0,
                      pat->event == -2 ? MIDI_PITCH_BEND : MIDI_CONTROL,
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
    midi_free(&temp);

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
    size_t write = 0;

    for (size_t i = 0; i < m->count; ++i) {
        Raw r = m->items[i];

        if (mod_key(r.bytes.data) >= 0) {
            dm_bytes_free(&r.bytes);
        } else {
            m->items[write++] = r;
        }
    }

    m->count = write;

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
            if (!add_message(m, 0, 0, e->message.data, e->message.size)) {
                return false;
            }
        }
    }

    return true;
}

static bool collect_timing(const DmProject *p, Midi *m) {
    uint32_t us = (uint32_t)llround(60000000.0 / p->bpm);
    uint8_t tempo[] = {MIDI_META, MIDI_META_TEMPO, 3, us >> 16, us >> 8, us};

    if (!add_message(m, 0, 0, tempo, sizeof(tempo))) {
        return false;
    }

    int power = 0;

    while ((1 << power) < p->denominator) {
        ++power;
    }

    uint8_t sig[] = {MIDI_META, MIDI_META_TIME, 4, (uint8_t)p->numerator, (uint8_t)power, 24, 8};

    if (!add_message(m, 0, 0, sig, sizeof(sig))) {
        return false;
    }

    return true;
}

static bool collect_single(const DmProject *p, Midi *m, bool mixer) {
    if (!collect_timing(p, m)) {
        return false;
    }

    if (!collect_initial_sysex(p, m) || !collect_channels(p, m)) {
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
static bool collect(const DmProject *p, Midi *m, bool mixer) {
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

                if (!raw_add(m, &raw)) {
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
            midi_free(&stream);

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

static int export_compare(const void *a, const void *b) {
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

bool dm_export_timeline(const DmProject *p, DmExportEvent **events, size_t *count,
                        uint64_t *duration, char *error, size_t cap) {
    *events = NULL;
    *count = 0;
    *duration = 0;
    Midi m = {0};
    bool ok = collect(p, &m, true);
    DmExportEvent *out = ok && m.count ? calloc(m.count, sizeof(*out)) : NULL;

    if (m.count && !out) {
        ok = false;
    }

    if (ok) {
        qsort(m.items, m.count, sizeof(*m.items), export_compare);
        *duration = (uint64_t)llround(dm_duration(p) * DM_PPQ);

        for (size_t i = 0; i < m.count; ++i) {
            Raw *r = &m.items[i];
            const uint8_t *b = r->bytes.data;
            DmExportEvent e = {0};
            e.tick = r->tick;
            e.source_id = i + 1;
            e.status = b[0];
            e.port = (uint32_t)r->port;

            if (b[0] < MIDI_SYSTEM) {
                e.a = b[1];
                e.b = r->bytes.size > 2 ? b[2] : 0;
            } else if (b[0] == MIDI_META && r->bytes.size == 6 && b[1] == MIDI_META_TEMPO) {
                e.a = (b[3] << 16) | (b[4] << 8) | b[5];
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

    midi_free(&m);

    if (!ok) {
        free(out);
        *count = 0;
        snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_EXPORT_TIMELINE);
        return false;
    }

    *events = out;
    return true;
}

static bool encode_track(const DmProject *p, Midi *m, int track, DmBytes *out) {
    DmBytes bytes = {0};
    bool ok = true;
    uint64_t last = 0;
    int port = 0;

    for (size_t i = 0; i < m->count && ok; ++i) {
        if (m->items[i].track == track) {
            Raw *r = &m->items[i];

            if (r->bytes.data[0] < MIDI_SYSTEM && r->port != port) {
                uint8_t prefix[] = {MIDI_META, MIDI_META_PORT, 1, (uint8_t)r->port};
                ok = vlq(&bytes, (uint32_t)(r->tick - last)) &&
                     dm_bytes(&bytes, prefix, sizeof(prefix));
                last = r->tick;
                port = r->port;
            }

            ok = vlq(&bytes, (uint32_t)(r->tick - last)) &&
                 dm_bytes(&bytes, r->bytes.data, r->bytes.size);
            last = r->tick;
        }
    }

    uint8_t end[] = {MIDI_META, MIDI_META_END, 0};
    uint64_t duration = (uint64_t)llround(dm_duration(p) * DM_PPQ);

    if (duration < last) {
        duration = last;
    }

    ok = ok && vlq(&bytes, (uint32_t)(duration - last)) && dm_bytes(&bytes, end, 3) &&
         dm_bytes(out, "MTrk", 4) && be(out, (uint32_t)bytes.size, 4) &&
         dm_bytes(out, bytes.data, bytes.size);
    dm_bytes_free(&bytes);
    return ok;
}

static bool midi_single(const DmProject *p, DmBytes *out, bool mixer, char *error, size_t cap) {
    Midi m = {0};
    bool ok = collect(p, &m, mixer);

    if (ok) {
        qsort(m.items, m.count, sizeof(Raw), export_compare);
    }

    ok = ok && dm_bytes(out, "MThd", 4) && be(out, 6, 4) && be(out, 1, 2) && be(out, 17, 2) &&
         be(out, DM_PPQ, 2);

    for (int track = 0; track < 17 && ok; ++track) {
        ok = encode_track(p, &m, track, out);
    }

    midi_free(&m);

    if (!ok) {
        dm_bytes_free(out);
        snprintf(error, cap, DM_TEXT_CANNOT_ENCODE_MIDI);
    }

    return ok;
}

typedef struct {
    Midi source, output;
    uint8_t *edited, *sequences;
    uint64_t *ends;
    int format, tracks, sequence_count, group_count;
} Merge;

static bool merge_key(Merge *m, int port, int sequence, bool set) {
    if (sequence < 0 || sequence >= m->sequence_count) {
        return false;
    }

    if (port == DM_PORT_COMBINED && set) {
        memset(m->edited + (size_t)sequence * 32, 255, 32);
        m->sequences[sequence] = 1;
        return true;
    }

    if (port < 0 || port > 255) {
        return false;
    }

    size_t bit = (size_t)sequence * 256 + (unsigned)port;
    uint8_t mask = (uint8_t)(1u << (bit & 7));

    if (set) {
        m->edited[bit >> 3] |= mask;
        m->sequences[sequence] = 1;
        return true;
    }

    return (m->edited[bit >> 3] & mask) != 0;
}

static void merge_free(Merge *m) {
    midi_free(&m->source);
    midi_free(&m->output);
    free(m->edited);
    free(m->sequences);
    free(m->ends);
}

static bool merge_init(Merge *m, const DmProject *p) {
    if (!parse_midi(&m->source, p->original.data, p->original.size)) {
        return false;
    }

    m->format = m->source.format == 2 ? 2 : 1;
    m->sequence_count = m->format == 2 ? m->source.tracks : 1;
    m->group_count = 1;

    for (size_t i = 0; i < p->view_count; ++i) {
        if (p->views[i].port != p->port || p->views[i].sequence != p->sequence) {
            ++m->group_count;
        }
    }

    m->tracks = m->format == 2 ? m->source.tracks : m->source.tracks + m->group_count * 17;

    if (m->tracks > 65535) {
        return false;
    }

    m->edited = calloc((size_t)m->sequence_count * 32, 1);
    m->sequences = calloc((size_t)m->sequence_count, 1);
    m->ends = calloc((size_t)m->tracks, sizeof(uint64_t));

    if (!m->edited || !m->sequences || !m->ends) {
        return false;
    }

    if (!merge_key(m, p->port, p->sequence, true)) {
        return false;
    }

    for (size_t i = 0; i < p->view_count; ++i) {
        if (!merge_key(m, p->views[i].port, p->views[i].sequence, true)) {
            return false;
        }
    }

    return true;
}

static bool merge_original_event(Merge *m, Raw *raw) {
    int sequence = m->format == 2 ? raw->track : 0;
    double ticks = tick_beat(&m->source, raw->tick) * DM_PPQ;

    if (!isfinite(ticks) || ticks > UINT32_MAX) {
        return false;
    }

    uint64_t tick = (uint64_t)llround(ticks);

    if (tick > m->ends[raw->track]) {
        m->ends[raw->track] = tick;
    }

    const uint8_t *b = raw->bytes.data;

    if (b[0] == MIDI_META) {
        if (b[1] == MIDI_META_END || b[1] == MIDI_META_PORT) {
            return true;
        }

        if ((m->source.division & 0x8000) && b[1] == MIDI_META_TEMPO) {
            return true;
        }

        if (m->sequences[sequence] && dm_conductor(&raw->bytes)) {
            return true;
        }
    } else if (merge_key(m, raw->port, sequence, false)) {
        return true;
    }

    Raw moved = *raw;
    moved.tick = tick;

    if (!raw_add(&m->output, &moved)) {
        return false;
    }

    raw->bytes = (DmBytes){0};
    return true;
}

static bool merge_original(Merge *m) {
    for (size_t i = 0; i < m->source.count; ++i) {
        if (!merge_original_event(m, &m->source.items[i])) {
            return false;
        }
    }

    return true;
}

static bool merge_take(Merge *m, Raw *raw, int track, int port) {
    Raw moved = *raw;
    moved.track = track;
    moved.port = port >= 0 && raw->port ? raw->port : port;

    if (!raw_add(&m->output, &moved)) {
        return false;
    }

    raw->bytes = (DmBytes){0};

    if (moved.tick > m->ends[track]) {
        m->ends[track] = moved.tick;
    }

    return true;
}

static bool merge_group(Merge *m, const DmProject *p, int group, bool mixer) {
    Midi content = {0};
    bool ok = collect(p, &content, mixer);
    int base = m->source.tracks + group * 17;
    double end = dm_duration(p) * DM_PPQ;

    if (end > UINT32_MAX) {
        ok = false;
    }

    for (size_t i = 0; i < content.count && ok; ++i) {
        Raw *raw = &content.items[i];

        if (raw->bytes.data[0] == MIDI_META) {
            continue;
        }

        int track = m->format == 2 ? p->sequence : base + raw->track;
        ok = merge_take(m, raw, track, p->port == DM_PORT_COMBINED ? 0 : p->port);
    }

    if (ok) {
        if (m->format == 2) {
            m->ends[p->sequence] = fmax((double)m->ends[p->sequence], end);
        } else {
            for (int i = 0; i < 17; ++i) {
                m->ends[base + i] = fmax((double)m->ends[base + i], end);
            }
        }
    }

    midi_free(&content);
    return ok;
}

static bool merge_edited_groups(Merge *m, const DmProject *p, bool mixer, char *error, size_t cap) {
    if (!merge_group(m, p, 0, mixer)) {
        return false;
    }

    int group = 1;

    for (size_t i = 0; i < p->view_count; ++i) {
        const DmView *view = &p->views[i];

        if (view->port == p->port && view->sequence == p->sequence) {
            continue;
        }

        DmProject restored;
        dm_project_init(&restored);
        bool ok = dm_unpack(&restored, view->state.data, view->state.size, error, cap);

        if (ok) {
            ok = merge_group(m, &restored, group++, mixer);
        }

        dm_project_free(&restored);

        if (!ok) {
            return false;
        }
    }

    return true;
}

static bool merge_conductor(Merge *m, const DmProject *p, int sequence) {
    Midi content = {0};
    bool ok = collect_timing(p, &content);

    for (size_t i = 0; i < p->event_count && ok; ++i) {
        const DmEvent *event = &p->events[i];

        if (!dm_conductor(&event->message)) {
            continue;
        }

        if (event->beat == 0 && event->message.data[1] != MIDI_META_MARKER) {
            continue;
        }

        ok = add_message(&content, event->beat, 0, event->message.data, event->message.size);
    }

    int track = m->format == 2 ? sequence : 0;

    for (size_t i = 0; i < content.count && ok; ++i) {
        ok = merge_take(m, &content.items[i], track, -1);
    }

    midi_free(&content);
    return ok;
}

static bool merge_conductors(Merge *m, const DmProject *p, char *error, size_t cap) {
    for (int sequence = 0; sequence < m->sequence_count; ++sequence) {
        if (!m->sequences[sequence]) {
            continue;
        }

        if (sequence == p->sequence) {
            if (!merge_conductor(m, p, sequence)) {
                return false;
            }

            continue;
        }

        const DmView *latest = NULL;

        for (size_t i = p->view_count; i > 0; --i) {
            if (p->views[i - 1].sequence == sequence) {
                latest = &p->views[i - 1];
                break;
            }
        }

        if (!latest) {
            return false;
        }

        DmProject state;
        dm_project_init(&state);
        bool ok = dm_unpack(&state, latest->state.data, latest->state.size, error, cap);

        if (ok) {
            ok = merge_conductor(m, &state, sequence);
        }

        dm_project_free(&state);

        if (!ok) {
            return false;
        }
    }

    return true;
}

static int merged_compare(const void *a, const void *b) {
    const Raw *x = a, *y = b;

    if (x->track != y->track) {
        return x->track < y->track ? -1 : 1;
    }

    return export_compare(a, b);
}

static bool merge_track(Merge *m, int track, size_t *cursor, DmBytes *out) {
    DmBytes data = {0};
    uint64_t previous = 0;
    int port = 0;
    bool ok = true;

    while (*cursor < m->output.count && m->output.items[*cursor].track == track && ok) {
        Raw *raw = &m->output.items[(*cursor)++];
        uint32_t delta = (uint32_t)(raw->tick - previous);

        if (raw->port >= 0 && raw->port != port) {
            uint8_t prefix[] = {MIDI_META, MIDI_META_PORT, 1, (uint8_t)raw->port};
            ok = vlq(&data, delta) && dm_bytes(&data, prefix, 4);
            delta = 0;
            port = raw->port;
        }

        ok = ok && vlq(&data, delta) && dm_bytes(&data, raw->bytes.data, raw->bytes.size);
        previous = raw->tick;
    }

    uint64_t end = m->ends[track];

    if (end < previous) {
        end = previous;
    }

    uint8_t eot[] = {MIDI_META, MIDI_META_END, 0};
    ok = ok && vlq(&data, (uint32_t)(end - previous)) && dm_bytes(&data, eot, 3);
    ok = ok && dm_bytes(out, "MTrk", 4) && be(out, (uint32_t)data.size, 4) &&
         dm_bytes(out, data.data, data.size);
    dm_bytes_free(&data);
    return ok;
}

static bool merge_encode(Merge *m, DmBytes *out) {
    qsort(m->output.items, m->output.count, sizeof(Raw), merged_compare);
    bool ok = dm_bytes(out, "MThd", 4) && be(out, 6, 4) && be(out, m->format, 2) &&
              be(out, m->tracks, 2) && be(out, DM_PPQ, 2);
    size_t cursor = 0;

    for (int track = 0; track < m->tracks && ok; ++track) {
        ok = merge_track(m, track, &cursor, out);
    }

    return ok;
}

static bool midi_all(const DmProject *p, DmBytes *out, bool mixer, char *error, size_t cap) {
    /* A combined arrangement already holds every source port; only other sequences need merging. */
    bool combined = p->port == DM_PORT_COMBINED;

    if (!p->original.size || ((p->port_count == 1 || combined) && p->sequence_count == 1)) {
        return midi_single(p, out, mixer, error, cap);
    }

    Midi routed = {0};

    if (!collect(p, &routed, mixer)) {
        snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_INDEPENDENT_NOTE);
        return false;
    }

    bool collision = false;

    for (size_t i = 0; i < routed.count && !collision; i++) {
        if (routed.items[i].port > 0) {
            for (int j = 0; j < p->port_count; j++) {
                if (p->ports[j] == routed.items[i].port) {
                    collision = true;
                    break;
                }
            }
        }
    }

    midi_free(&routed);

    if (collision && !combined) {
        snprintf(error, cap, DM_TEXT_COLORED_NOTE_PORTS_OVERLAP);
        return false;
    }

    Merge m = {0};
    bool ok = merge_init(&m, p);

    if (ok) {
        ok = merge_original(&m);
    }

    if (ok) {
        ok = merge_edited_groups(&m, p, mixer, error, cap);
    }

    if (ok) {
        ok = merge_conductors(&m, p, error, cap);
    }

    if (ok) {
        ok = merge_encode(&m, out);
    }

    merge_free(&m);

    if (!ok) {
        dm_bytes_free(out);
        snprintf(error, cap, DM_TEXT_CANNOT_MERGE_MIDI);
    }

    return ok;
}

bool dm_midi(const DmProject *p, DmBytes *out, char *error, size_t cap) {
    return midi_single(p, out, false, error, cap);
}

bool dm_midi_all(const DmProject *p, DmBytes *out, char *error, size_t cap) {
    return midi_all(p, out, false, error, cap);
}

static bool editor_snapshot(const DmProject *p, bool all_views, bool strip_foreign, DmBytes *out,
                            char *error, size_t cap) {
    DmBytes packed = {0};
    DmProject copy;
    dm_project_init(&copy);
    bool ok = dm_pack(p, &packed) && dm_unpack(&copy, packed.data, packed.size, error, cap);
    dm_bytes_free(&packed);

    if (!ok) {
        dm_project_free(&copy);
        return false;
    }

    remove_embedded_events(&copy, strip_foreign);

    if (!all_views) {
        dm_bytes_free(&copy.original);

        for (size_t i = 0; i < copy.view_count; ++i) {
            dm_bytes_free(&copy.views[i].state);
        }

        free(copy.views);
        copy.views = NULL;
        copy.view_count = copy.view_capacity = 0;
        copy.port = copy.sequence = copy.ports[0] = 0;
        copy.source_format = copy.port_count = copy.sequence_count = 1;

        for (size_t i = 0; i < copy.event_count; ++i) {
            copy.events[i].port = 0;
        }
    } else if (!copy.view_count && copy.sequence_count <= 1) {
        /* The source file is only needed to rebuild other sequences or source views. A song
           with neither is completely described by the exported MIDI and the project, so its
           source copy is left out instead of roughly doubling the file. */
        dm_bytes_free(&copy.original);
    } else {
        ok = remove_embedded_source(&copy.original, strip_foreign);

        for (size_t i = 0; i < copy.view_count && ok; ++i) {
            DmProject view;
            dm_project_init(&view);
            DmBytes *state = &copy.views[i].state;
            ok = dm_unpack(&view, state->data, state->size, error, cap);

            if (ok) {
                remove_embedded_events(&view, strip_foreign);
                dm_bytes_free(state);
                ok = dm_pack(&view, state);
            }

            dm_project_free(&view);
        }
    }

    ok = ok && dm_pack(&copy, out);
    dm_project_free(&copy);
    return ok;
}

bool dm_midi_export(const DmProject *p, DmBytes *out, bool all_views, DmMetadataMode metadata,
                    char *error, size_t cap) {
    if (metadata < DM_METADATA_KEEP_ALL || metadata > DM_METADATA_STRIP_ALL) {
        dm_bytes_free(out);
        snprintf(error, cap, DM_TEXT_INVALID_MIDI_EDITOR);
        return false;
    }

    bool strip_foreign = metadata != DM_METADATA_KEEP_ALL;
    DmBytes midi = {0}, snapshot = {0}, event = {0};
    Midi m = {0};
    /* Files are exported as the arrangement sounds: muted channels and ports are left out. */
    bool ok =
        all_views ? midi_all(p, &midi, true, error, cap) : midi_single(p, &midi, true, error, cap);
    ok = ok && parse_midi(&m, midi.data, midi.size);

    if (ok && metadata != DM_METADATA_STRIP_ALL) {
        ok = editor_snapshot(p, all_views, strip_foreign, &snapshot, error, cap);
        uint64_t hash = midi_fingerprint(&m, strip_foreign);
        size_t payload = sizeof(editor_tag) + 12 + snapshot.size;
        ok = ok && payload <= 0x0fffffff && byte(&event, MIDI_META) &&
             byte(&event, MIDI_META_SEQUENCER) && vlq(&event, (uint32_t)payload) &&
             dm_bytes(&event, editor_tag, sizeof(editor_tag)) && be(&event, 1, 4) &&
             be(&event, (uint32_t)(hash >> 32), 4) && be(&event, (uint32_t)hash, 4) &&
             dm_bytes(&event, snapshot.data, snapshot.size);
    }

    ok = ok && metadata_encode(&m, out, strip_foreign, &event);
    /* The editor must be able to reopen every successfully exported file. */
    ok = ok && out->size <= 128u * 1024u * 1024u;
    midi_free(&m);
    dm_bytes_free(&midi);
    dm_bytes_free(&snapshot);
    dm_bytes_free(&event);

    if (!ok) {
        dm_bytes_free(out);
        snprintf(error, cap, DM_TEXT_CANNOT_EXPORT_MIDI);
    }

    return ok;
}

static double tempo_event(const DmEvent *e) {
    const uint8_t *b = e->message.data;

    if (e->message.size != 6 || b[0] != MIDI_META || b[1] != MIDI_META_TEMPO || b[2] != 3 ||
        e->beat <= 0) {
        return 0;
    }

    return ((b[3] << 16) | (b[4] << 8) | b[5]) / 1000000.0;
}

DmMeter dm_meter(const DmProject *p, double beat) {
    DmMeter meter = {dm_bar(p), 0, 0, INFINITY};

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        const uint8_t *b = e->message.data;

        if (e->message.size != 7 || b[0] != MIDI_META || b[1] != MIDI_META_TIME || e->beat <= 0) {
            continue;
        }

        if (e->beat > beat) {
            meter.next_change = e->beat;
            break;
        }

        meter.completed += ceil((e->beat - meter.origin) / meter.bar - 1e-9);
        meter.origin = e->beat;
        meter.bar = b[3] * 4.0 / (1 << b[4]);
    }

    return meter;
}

double dm_beat_seconds(const DmProject *p, double beat) {
    double last = 0, seconds = 0, rate = 60 / p->bpm;

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        double next = tempo_event(e);

        if (!next || e->beat > beat) {
            continue;
        }

        seconds += (e->beat - last) * rate;
        last = e->beat;
        rate = next;
    }

    return seconds + (beat - last) * rate;
}

double dm_seconds_beat(const DmProject *p, double seconds) {
    double last = 0, elapsed = 0, rate = 60 / p->bpm;

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        double next = tempo_event(e);

        if (!next) {
            continue;
        }

        double span = (e->beat - last) * rate;

        if (elapsed + span > seconds) {
            return last + (seconds - elapsed) / rate;
        }

        elapsed += span;
        last = e->beat;
        rate = next;
    }

    return last + (seconds - elapsed) / rate;
}

static bool schedule_append(DmSchedule *s, const Raw *raw, double seconds) {
    if (!dm_reserve((void **)&s->items, &s->capacity, s->count + 1, sizeof(DmScheduled))) {
        return false;
    }

    DmScheduled event = {0};
    event.seconds = seconds;
    event.status = raw->bytes.data[0];
    event.port = (uint8_t)raw->port;
    event.a = raw->bytes.data[1];
    event.b = raw->bytes.size > 2 ? raw->bytes.data[2] : 0;

    if (event.status >= MIDI_SYSTEM) {
        Reader input = {raw->bytes.data + 1, raw->bytes.data + raw->bytes.size, true};
        uint32_t length = read_vlq(&input);

        if (!input.ok || !dm_bytes(&event.sysex, input.p, length)) {
            return false;
        }
    }

    s->items[s->count++] = event;
    return true;
}

static bool schedule_events(const DmProject *p, const Midi *m, DmSchedule *s) {
    double previous = 0, seconds = 0, rate = 60 / p->bpm;

    for (size_t i = 0; i < m->count; ++i) {
        const Raw *raw = &m->items[i];
        const uint8_t *message = raw->bytes.data;
        double beat = (double)raw->tick / DM_PPQ;
        seconds += (beat - previous) * rate;
        previous = beat;

        if (message[0] == MIDI_META) {
            if (message[1] == MIDI_META_TEMPO && raw->bytes.size == 6 && beat > 0) {
                rate = ((message[3] << 16) | (message[4] << 8) | message[5]) / 1000000.0;
            }

            continue;
        }

        if (!schedule_append(s, raw, seconds)) {
            return false;
        }
    }

    return true;
}

static bool schedule_beats(const DmProject *p, DmSchedule *s) {
    double length = dm_duration(p);
    s->duration = dm_beat_seconds(p, length);
    s->beat_count = (size_t)ceil(length);

    if (s->beat_count > 10000000) {
        return false;
    }

    s->beats = malloc(s->beat_count * sizeof(double));

    if (!s->beats) {
        return false;
    }

    double previous = 0, seconds = 0, rate = 60 / p->bpm;
    size_t event = 0;

    for (size_t beat = 0; beat < s->beat_count; ++beat) {
        while (event < p->event_count && p->events[event].beat <= (double)beat) {
            const DmEvent *e = &p->events[event++];
            double next = tempo_event(e);

            if (!next) {
                continue;
            }

            seconds += (e->beat - previous) * rate;
            previous = e->beat;
            rate = next;
        }

        s->beats[beat] = seconds + ((double)beat - previous) * rate;
    }

    s->next_beat = dm_beat_seconds(p, (double)s->beat_count);

    for (; event < p->event_count; ++event) {
        double next = tempo_event(&p->events[event]);

        if (next) {
            rate = next;
        }
    }

    s->final_rate = rate;
    return true;
}

bool dm_schedule(const DmProject *p, DmSchedule *s) {
    Midi m = {0};
    bool ok = collect(p, &m, true);

    if (ok) {
        qsort(m.items, m.count, sizeof(Raw), export_compare);
        ok = schedule_events(p, &m, s);
    }

    if (ok) {
        ok = schedule_beats(p, s);
    }

    midi_free(&m);

    if (!ok) {
        dm_schedule_free(s);
    }

    return ok;
}

void dm_schedule_free(DmSchedule *s) {
    for (size_t i = 0; i < s->count; ++i) {
        dm_bytes_free(&s->items[i].sysex);
    }

    free(s->items);
    free(s->beats);
    memset(s, 0, sizeof(*s));
}
