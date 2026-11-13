/* Reading MIDI and MUS: the parser, duckmidi's embedded editor data, and import into a project. */
#include "core/midi_internal.h"

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

uint32_t dm_midi_read_vlq(Reader *r) {
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

bool dm_midi_byte(DmBytes *b, unsigned v) {
    uint8_t n = (uint8_t)v;
    return dm_bytes(b, &n, 1);
}

bool dm_midi_be(DmBytes *b, uint32_t v, int count) {
    for (int i = count - 1; i >= 0; --i) {
        if (!dm_midi_byte(b, v >> (i * 8))) {
            return false;
        }
    }

    return true;
}

bool dm_midi_vlq(DmBytes *b, uint32_t v) {
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
        if (!dm_midi_byte(b, n[--count])) {
            return false;
        }
    }

    return true;
}

void dm_midi_free(Midi *m) {
    for (size_t i = 0; i < m->count; ++i) {
        dm_bytes_free(&m->items[i].bytes);
    }

    free(m->items);
    memset(m, 0, sizeof(*m));
}

bool dm_midi_raw_add(Midi *m, Raw *raw) {
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

        if (!r->ok || value > 127 || !dm_midi_byte(msg, value)) {
            return false;
        }
    }

    return true;
}

static bool parse_system_data(Reader *r, unsigned status, DmBytes *msg) {
    if (status == MIDI_META) {
        if (!dm_midi_byte(msg, read_be(r, 1))) {
            return false;
        }
    } else if (status != MIDI_SYSEX && status != MIDI_SYSEX_END) {
        return false;
    }

    uint32_t size = dm_midi_read_vlq(r);

    if (!r->ok || (size_t)(r->end - r->p) < size) {
        return false;
    }

    if (!dm_midi_vlq(msg, size) || !dm_bytes(msg, r->p, size)) {
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

    if (!dm_midi_byte(msg, status)) {
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
        tick += dm_midi_read_vlq(r);

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

        if (m->scan_pan) {
            const uint8_t *b = raw.bytes.data;

            if (b[0] < MIDI_SYSTEM && (m->format != 2 || track == m->scan_sequence) &&
                (m->scan_port == DM_PORT_COMBINED || port == m->scan_port)) {
                unsigned ch = b[0] & MIDI_CHANNEL_MASK;

                if ((b[0] & MIDI_STATUS_MASK) == MIDI_NOTE_ON && b[2]) {
                    m->first_note[ch] = tick < m->first_note[ch] ? tick : m->first_note[ch];
                } else if ((b[0] & MIDI_STATUS_MASK) == MIDI_CONTROL && b[1] == MIDI_CC_PAN) {
                    m->first_pan[ch] = tick < m->first_pan[ch] ? tick : m->first_pan[ch];
                }
            }

            dm_bytes_free(&raw.bytes);
        } else if (!dm_midi_raw_add(m, &raw)) {
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

bool dm_midi_parse(Midi *m, const uint8_t *data, size_t size) {
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

static Reader meta_payload(const DmBytes *b) {
    Reader r = {b->data, b->data + b->size, true};

    if (b->size < 3 || read_be(&r, 1) != MIDI_META || read_be(&r, 1) != MIDI_META_SEQUENCER) {
        r.ok = false;
        return r;
    }

    uint32_t size = dm_midi_read_vlq(&r);
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

uint64_t dm_midi_fingerprint(const Midi *m, bool strip_foreign) {
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
bool dm_midi_metadata_encode(const Midi *m, DmBytes *out, bool strip, const DmBytes *replacement) {
    bool ok = dm_bytes(out, "MThd", 4) && dm_midi_be(out, 6, 4) && dm_midi_be(out, m->format, 2) &&
              dm_midi_be(out, m->tracks, 2) && dm_midi_be(out, m->division, 2);

    for (int t = 0; t < m->tracks && ok; ++t) {
        DmBytes track = {0};
        uint64_t last = 0;

        if (!t && replacement && replacement->size) {
            ok = dm_midi_byte(&track, 0) && dm_bytes(&track, replacement->data, replacement->size);
        }

        for (size_t i = 0; i < m->count && ok; ++i) {
            const Raw *r = &m->items[i];

            if (r->track != t || editor_metadata(&r->bytes) ||
                (strip && sequencer_metadata(&r->bytes))) {
                continue;
            }

            ok = r->tick >= last && r->tick - last <= 0x0fffffff &&
                 dm_midi_vlq(&track, (uint32_t)(r->tick - last)) &&
                 dm_bytes(&track, r->bytes.data, r->bytes.size);
            last = r->tick;
        }

        ok = ok && track.size <= UINT32_MAX && dm_bytes(out, "MTrk", 4) &&
             dm_midi_be(out, (uint32_t)track.size, 4) && dm_bytes(out, track.data, track.size);
        dm_bytes_free(&track);
    }

    if (!ok) {
        dm_bytes_free(out);
    }

    return ok;
}

bool dm_midi_remove_embedded_source(DmBytes *source, bool strip_foreign) {
    if (!source->size) {
        return true;
    }

    Midi m = {0};
    bool ok = dm_midi_parse(&m, source->data, source->size), found = false;

    for (size_t i = 0; i < m.count; ++i) {
        found |= editor_metadata(&m.items[i].bytes) ||
                 (strip_foreign && sequencer_metadata(&m.items[i].bytes));
    }

    DmBytes clean = {0};

    if (ok && found) {
        ok = dm_midi_metadata_encode(&m, &clean, strip_foreign, NULL);

        if (ok) {
            dm_bytes_free(source);
            *source = clean;
        }
    }

    dm_midi_free(&m);
    return ok;
}

void dm_midi_remove_embedded_events(DmProject *p, bool strip_foreign) {
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

/* Only volume and pan can separate repeat program changes without affecting the patch
   or triggering it. Other messages on the route conservatively start a new group. */
static int program_repeat(DmProgramHistory history[16], double position, int track, int index,
                          const uint8_t *message, size_t size) {
    uint8_t status = message[0];

    if (status == MIDI_SYSEX || status == MIDI_SYSEX_END) {
        memset(history, 0, 16 * sizeof(*history));
    }

    if (status >= MIDI_SYSTEM) {
        return -1;
    }

    int type = status & MIDI_STATUS_MASK;
    DmProgramHistory *last = &history[status & MIDI_CHANNEL_MASK];

    if (type == MIDI_PROGRAM && size == 2) {
        uint8_t program = message[1] + 1;

        if (last->program_plus_one == program && last->position == position &&
            last->track == track) {
            return last->index;
        }

        *last = (DmProgramHistory){position, track, index, program};
    } else if (type != MIDI_CONTROL || size != 3 ||
               (message[1] != MIDI_CC_VOLUME && message[1] != MIDI_CC_PAN)) {
        last->program_plus_one = 0;
    }

    return -1;
}

int dm_project_program_repeat(DmProgramHistory history[DM_PORT_COMBINED][16], const DmProject *p,
                              int index, bool automation) {
    if (!automation) {
        const DmEvent *event = &p->events[index];
        return program_repeat(history[event->port], event->beat, event->track, index,
                              event->message.data, event->message.size);
    }

    const DmAutomation *a = &p->automation[index];

    if (a->pattern >= 0) {
        return -1;
    }

    if (a->type == MIDI_PROGRAM &&
        (a->curve || a->anchor || a->bend || a->ramp || a->hold || a->b)) {
        history[a->port][a->channel].program_plus_one = 0;
        return -1;
    }

    const uint8_t message[] = {a->type | a->channel, a->a, a->b};
    size_t size = a->type == MIDI_PROGRAM || a->type == MIDI_CHANNEL_PRESSURE ? 2 : 3;
    return program_repeat(history[a->port], a->beat, -1, index, message, size);
}

/* Index -1 collapses the entire stream. A selected row collapses only its own copies. */
bool dm_collapse_program_repeats(DmProject *p, int index, bool automation) {
    DmProgramHistory history[DM_PORT_COMBINED][16] = {0};
    size_t *count = automation ? &p->automation_count : &p->event_count;
    size_t first = index < 0 ? 0 : (size_t)index;

    if (first >= *count) {
        return false;
    }

    double start = automation ? p->automation[first].beat : p->events[first].beat;
    size_t write = first, i = first;

    for (; i < *count; ++i) {
        double beat = automation ? p->automation[i].beat : p->events[i].beat;

        if (index >= 0 && beat != start) {
            break;
        }

        int prior = dm_project_program_repeat(history, p, (int)i, automation);

        if (prior >= 0 && (index < 0 || prior == index)) {
            if (!automation) {
                dm_bytes_free(&p->events[i].message);
            }

            continue;
        }

        if (write != i) {
            if (automation) {
                p->automation[write] = p->automation[i];
            } else {
                p->events[write] = p->events[i];
            }
        }

        ++write;
    }

    if (write == i) {
        return false;
    }

    if (i < *count) {
        if (automation) {
            memmove(p->automation + write, p->automation + i, (*count - i) * sizeof(DmAutomation));
        } else {
            memmove(p->events + write, p->events + i, (*count - i) * sizeof(DmEvent));
        }
    }

    *count -= i - write;
    return true;
}

/* Keep the original indices in the history until the scan finishes. Marks follow the
   compacted stream, while the history carries whether each original prior was picked. */
void dm_collapse_marked_program_repeats(DmProject *p, bool *marks, bool automation) {
    DmProgramHistory history[DM_PORT_COMBINED][16] = {0};
    bool picked[DM_PORT_COMBINED][16] = {0};
    size_t *count = automation ? &p->automation_count : &p->event_count;
    size_t write = 0;

    for (size_t i = 0; i < *count; ++i) {
        int prior = dm_project_program_repeat(history, p, (int)i, automation);
        int port = automation ? p->automation[i].port : p->events[i].port;
        const DmBytes *message = automation ? NULL : &p->events[i].message;
        int channel = automation      ? p->automation[i].channel
                      : message->size ? message->data[0] & MIDI_CHANNEL_MASK
                                      : 0;
        DmProgramHistory *last = &history[port][channel];
        bool remove = prior >= 0 && picked[port][channel];

        if (last->program_plus_one && last->index == (int)i) {
            picked[port][channel] = marks[i];
        }

        if (remove) {
            if (!automation) {
                dm_bytes_free(&p->events[i].message);
            }

            continue;
        }

        if (automation) {
            p->automation[write] = p->automation[i];
        } else {
            p->events[write] = p->events[i];
        }

        marks[write++] = marks[i];
    }

    *count = write;
}

static bool collapse_restored_programs(DmProject *p, char *error, size_t cap) {
    dm_collapse_program_repeats(p, -1, false);
    dm_collapse_program_repeats(p, -1, true);

    for (size_t i = 0; i < p->view_count; ++i) {
        DmBytes *state = &p->views[i].state;
        DmProject view;
        dm_project_init(&view);
        bool ok = dm_unpack(&view, state->data, state->size, error, cap);

        if (ok) {
            bool source = dm_collapse_program_repeats(&view, -1, false);
            bool automation = dm_collapse_program_repeats(&view, -1, true);

            if (source || automation) {
                DmBytes packed = {0};
                ok = dm_pack(&view, &packed);

                if (ok) {
                    dm_bytes_free(state);
                    *state = packed;
                } else {
                    snprintf(error, cap, DM_TEXT_CANNOT_ALLOCATE_MIDI);
                }
            }
        }

        dm_project_free(&view);

        if (!ok) {
            return false;
        }
    }

    return true;
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

    if (hash != dm_midi_fingerprint(m, false)) {
        dm_project_free(&next);
        snprintf(error, cap, DM_TEXT_MIDI_IMPORTED_STALE);
        return 2;
    }

    /* A normal open (port -1) restores the saved active view. Explicit view
       requests retain the existing source-view switching semantics. */
    ok = dm_midi_remove_embedded_source(&next.original, false);

    if (!ok) {
        snprintf(error, cap, DM_TEXT_INVALID_MIDI_SOURCE);
    }

    if (ok && port >= 0 && (port != next.port || sequence != next.sequence)) {
        ok = dm_view_switch(&next, port, sequence, error, cap);
    }

    if (!ok || !collapse_restored_programs(&next, error, cap)) {
        dm_project_free(&next);
        return -1;
    }

    dm_project_free(p);
    *p = next;
    snprintf(error, cap, DM_TEXT_MIDI_IMPORTED_DUCKMIDI);
    return 1;
}

double dm_midi_tick_beat(const Midi *m, uint64_t tick) {
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
        p->channels[c].drums = c == MIDI_DRUM_CHANNEL;

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

/* The body of a stored SysEx message: after F0 and its length. */
static bool sysex_body(const uint8_t *b, size_t size, const uint8_t **body, uint32_t *length) {
    if (!size || (b[0] != MIDI_SYSEX && b[0] != MIDI_SYSEX_END)) {
        return false;
    }

    Reader r = {b + 1, b + size, true};
    *length = dm_midi_read_vlq(&r);
    *body = r.p;
    return r.ok && *length <= (size_t)(r.end - r.p);
}

/* Roland GS "use for rhythm part" (address 40 1x 15): the MIDI channel of part block x and
   whether it now plays a drum map. Block 0 is channel 10, 1-9 channels 1-9, A-F channels 11-16. */
bool dm_midi_rhythm_part(const DmBytes *message, int *channel, bool *drums) {
    const uint8_t *d;
    uint32_t n;

    if (!sysex_body(message->data, message->size, &d, &n) || n < 8 || d[0] != 0x41 ||
        d[2] != 0x42 || d[3] != 0x12 || d[4] != 0x40 || (d[5] & 0xF0) != 0x10 || d[6] != 0x15) {
        return false;
    }

    int block = d[5] & 0x0F;
    *channel = block == 0 ? MIDI_DRUM_CHANNEL : block <= 9 ? block - 1 : block;
    *drums = d[7] != 0;
    return true;
}

/* GM System On and GS Reset return every part to the General MIDI layout. */
static bool layout_reset(const uint8_t *b, size_t size) {
    const uint8_t *d;
    uint32_t n;

    if (!sysex_body(b, size, &d, &n) || n < 4) {
        return false;
    }

    bool gm = d[0] == 0x7E && d[2] == 0x09 && (d[3] == 0x01 || d[3] == 0x03);
    bool gs = n >= 8 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12 && d[4] == 0x40 &&
              d[5] == 0x00 && d[6] == 0x7F;
    return gm || gs;
}

/* Percussion parts from the file's setup: GS rhythm parts, and XG (bank 127) or GM2 (bank 120)
   drum banks selected before a channel's first note. */
static void import_drum_parts(DmProject *p, const ImportState *state, const DmBytes *message) {
    const uint8_t *b = message->data;
    int channel;
    bool drums;

    if (dm_midi_rhythm_part(message, &channel, &drums)) {
        p->channels[channel].drums = drums;
    } else if (layout_reset(b, message->size)) {
        for (int c = 0; c < 16; ++c) {
            p->channels[c].drums = c == MIDI_DRUM_CHANNEL;
        }
    } else if (b[0] < MIDI_SYSTEM && (b[0] & MIDI_STATUS_MASK) == MIDI_CONTROL &&
               b[1] == MIDI_CC_BANK && (b[2] == 127 || b[2] == 120) &&
               state->pattern[b[0] & MIDI_CHANNEL_MASK] < 0) {
        p->channels[b[0] & MIDI_CHANNEL_MASK].drums = true;
    }
}

static bool import_one(DmProject *p, const Midi *m, ImportState *state, const Raw *r) {
    const uint8_t *b = r->bytes.data;
    int type = b[0] & MIDI_STATUS_MASK, c = b[0] & MIDI_CHANNEL_MASK;
    double beat = dm_midi_tick_beat(m, r->tick);

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

    import_drum_parts(p, state, &r->bytes);

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

    return dm_midi_tick_beat(m, tick);
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

        size_t placements = 0;

        for (size_t j = 0; j < p->clip_count; ++j) {
            placements += p->clips[j].pattern == clip.pattern;
        }

        /* The opening volume and pan became the channel settings. Removing them leaves the mixer
           in control until the song changes them; nothing sounds before the first note. */
        if (placements != 1 || pat->kind || !isfinite(first)) {
            continue;
        }

        size_t write = 0;

        for (size_t j = 0; j < p->automation_count; ++j) {
            DmAutomation a = p->automation[j];

            if (a.pattern == clip.pattern && a.type == MIDI_CONTROL && a.beat <= first &&
                (a.a == MIDI_CC_VOLUME || a.a == MIDI_CC_PAN)) {
                continue;
            }

            p->automation[write++] = a;
        }

        p->automation_count = write;
    }
}

/* A tempo sent before the first note can supply the starting BPM when the file has no
   explicit tick-zero tempo. Keep subsequent events at their original beats. */
static void import_initial_tempo(DmProject *p) {
    double first_note = INFINITY;

    for (size_t i = 0; i < p->pattern_count; ++i) {
        const DmPattern *pattern = &p->patterns[i];

        for (size_t n = 0; n < pattern->count; ++n) {
            first_note = fmin(first_note, pattern->notes[n].beat);
        }
    }

    if (!isfinite(first_note) || first_note <= 0) {
        return;
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        DmEvent *event = &p->events[i];
        const uint8_t *b = event->message.data;

        if (event->beat > first_note) {
            break;
        }

        uint32_t tempo = dm_tempo_microseconds(b, event->message.size);

        if (tempo) {
            if (event->beat > 0) {
                p->bpm = 60000000.0 / tempo;
                dm_bytes_free(&event->message);
                memmove(event, event + 1, (p->event_count - i - 1) * sizeof(*event));
                --p->event_count;
            }

            break;
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
    import_initial_tempo(p);
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
    /* Each import pass handles one source port and sequence. */
    DmProgramHistory history[16] = {0};
    bool ok = true;

    for (size_t i = 0; i < m->count; ++i) {
        const Raw *raw = &m->items[i];

        if (!active_raw(p, raw) || program_repeat(history, raw->tick, raw->track, (int)i,
                                                  raw->bytes.data, raw->bytes.size) >= 0) {
            continue;
        }

        if (!import_one(p, m, &state, raw)) {
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
    bool ok = dm_midi_vlq(&mus->track, mus->delay) && dm_midi_byte(&mus->track, status) &&
              dm_midi_byte(&mus->track, a);
    mus->delay = 0;
    return ok && (size == 2 || dm_midi_byte(&mus->track, b));
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
            return dm_midi_vlq(&mus->track, mus->delay) && dm_midi_byte(&mus->track, MIDI_META) &&
                   dm_midi_byte(&mus->track, 0x2f) && dm_midi_byte(&mus->track, 0);
        }

        if (!mus_event(mus, type, channel)) {
            return false;
        }

        if (descriptor & 128) {
            mus->delay = dm_midi_read_vlq(&mus->input);
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
             dm_midi_be(out, (uint32_t)mus.track.size, 4) &&
             dm_bytes(out, mus.track.data, mus.track.size);
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

    if (!dm_midi_parse(&m, data, size)) {
        dm_midi_free(&m);
        snprintf(error, cap, DM_TEXT_INVALID_UNSUPPORTED_MIDI);
        return false;
    }

    int restored = restore_editor(p, &m, port, sequence, error, cap);

    if (restored == 1 || restored < 0) {
        dm_midi_free(&m);
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
        dm_midi_free(&m);
        dm_project_free(&next);
        return dm_import_ports(p, data, size, ports, count, sequence, error, cap);
    }

    bool ok = sequence >= 0 && sequence < next.sequence_count;
    qsort(m.items, m.count, sizeof(Raw), raw_compare);

    if (ok) {
        ok = dm_bytes(&next.original, data, size) &&
             (restored != 2 || dm_midi_remove_embedded_source(&next.original, false)) &&
             import_events(&next, &m);
    }

    dm_midi_free(&m);

    if (!ok) {
        dm_project_free(&next);
        snprintf(error, cap, DM_TEXT_MIDI_IMPORT_FAILED);
        return false;
    }

    dm_project_free(p);
    *p = next;
    return true;
}
