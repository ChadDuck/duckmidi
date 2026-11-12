/* Writing Standard MIDI Files: one-track and merged multi-track output with the editor's data
   embedded. */
#include "core/midi_internal.h"

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
                ok = dm_midi_vlq(&bytes, (uint32_t)(r->tick - last)) &&
                     dm_bytes(&bytes, prefix, sizeof(prefix));
                last = r->tick;
                port = r->port;
            }

            ok = dm_midi_vlq(&bytes, (uint32_t)(r->tick - last)) &&
                 dm_bytes(&bytes, r->bytes.data, r->bytes.size);
            last = r->tick;
        }
    }

    uint8_t end[] = {MIDI_META, MIDI_META_END, 0};
    uint64_t duration = (uint64_t)llround(dm_duration(p) * DM_PPQ);

    if (duration < last) {
        duration = last;
    }

    ok = ok && dm_midi_vlq(&bytes, (uint32_t)(duration - last)) && dm_bytes(&bytes, end, 3) &&
         dm_bytes(out, "MTrk", 4) && dm_midi_be(out, (uint32_t)bytes.size, 4) &&
         dm_bytes(out, bytes.data, bytes.size);
    dm_bytes_free(&bytes);
    return ok;
}

static bool midi_single(const DmProject *p, DmBytes *out, bool mixer, char *error, size_t cap) {
    Midi m = {0};
    bool ok = dm_midi_collect(p, &m, mixer);

    if (ok) {
        dm_midi_sort(&m);
    }

    ok = ok && dm_bytes(out, "MThd", 4) && dm_midi_be(out, 6, 4) && dm_midi_be(out, 1, 2) &&
         dm_midi_be(out, 17, 2) && dm_midi_be(out, DM_PPQ, 2);

    for (int track = 0; track < 17 && ok; ++track) {
        ok = encode_track(p, &m, track, out);
    }

    dm_midi_free(&m);

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
    dm_midi_free(&m->source);
    dm_midi_free(&m->output);
    free(m->edited);
    free(m->sequences);
    free(m->ends);
}

static bool merge_init(Merge *m, const DmProject *p) {
    if (!dm_midi_parse(&m->source, p->original.data, p->original.size)) {
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
    double ticks = dm_midi_tick_beat(&m->source, raw->tick) * DM_PPQ;

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

    if (!dm_midi_raw_add(&m->output, &moved)) {
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

    if (!dm_midi_raw_add(&m->output, &moved)) {
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
    bool ok = dm_midi_collect(p, &content, mixer);
    int base = m->source.tracks + group * 17;
    double end = dm_duration(p) * DM_PPQ;

    if (end > UINT32_MAX) {
        ok = false;
    }

    if (ok && m->format != 2) {
        dm_midi_sort(&content);
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

    dm_midi_free(&content);
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
    bool ok = dm_midi_collect_timing(p, &content);

    for (size_t i = 0; i < p->event_count && ok; ++i) {
        const DmEvent *event = &p->events[i];

        if (!dm_conductor(&event->message)) {
            continue;
        }

        if (event->beat == 0 && event->message.data[1] != MIDI_META_MARKER) {
            continue;
        }

        ok =
            dm_midi_add_message(&content, event->beat, 0, event->message.data, event->message.size);
    }

    int track = m->format == 2 ? sequence : 0;

    for (size_t i = 0; i < content.count && ok; ++i) {
        ok = merge_take(m, &content.items[i], track, -1);
    }

    dm_midi_free(&content);
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

    return dm_midi_export_compare(a, b);
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
            ok = dm_midi_vlq(&data, delta) && dm_bytes(&data, prefix, 4);
            delta = 0;
            port = raw->port;
        }

        ok = ok && dm_midi_vlq(&data, delta) && dm_bytes(&data, raw->bytes.data, raw->bytes.size);
        previous = raw->tick;
    }

    uint64_t end = m->ends[track];

    if (end < previous) {
        end = previous;
    }

    uint8_t eot[] = {MIDI_META, MIDI_META_END, 0};
    ok = ok && dm_midi_vlq(&data, (uint32_t)(end - previous)) && dm_bytes(&data, eot, 3);
    ok = ok && dm_bytes(out, "MTrk", 4) && dm_midi_be(out, (uint32_t)data.size, 4) &&
         dm_bytes(out, data.data, data.size);
    dm_bytes_free(&data);
    return ok;
}

static bool merge_encode(Merge *m, DmBytes *out) {
    qsort(m->output.items, m->output.count, sizeof(Raw), merged_compare);
    bool ok = dm_bytes(out, "MThd", 4) && dm_midi_be(out, 6, 4) && dm_midi_be(out, m->format, 2) &&
              dm_midi_be(out, m->tracks, 2) && dm_midi_be(out, DM_PPQ, 2);
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

    if (!dm_midi_collect(p, &routed, mixer)) {
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

    dm_midi_free(&routed);

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

    dm_midi_remove_embedded_events(&copy, strip_foreign);

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
        ok = dm_midi_remove_embedded_source(&copy.original, strip_foreign);

        for (size_t i = 0; i < copy.view_count && ok; ++i) {
            DmProject view;
            dm_project_init(&view);
            DmBytes *state = &copy.views[i].state;
            ok = dm_unpack(&view, state->data, state->size, error, cap);

            if (ok) {
                dm_midi_remove_embedded_events(&view, strip_foreign);
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
    ok = ok && dm_midi_parse(&m, midi.data, midi.size);

    if (ok && metadata != DM_METADATA_STRIP_ALL) {
        ok = editor_snapshot(p, all_views, strip_foreign, &snapshot, error, cap);
        uint64_t hash = dm_midi_fingerprint(&m, strip_foreign);
        size_t payload = sizeof(editor_tag) + 12 + snapshot.size;
        ok = ok && payload <= 0x0fffffff && dm_midi_byte(&event, MIDI_META) &&
             dm_midi_byte(&event, MIDI_META_SEQUENCER) && dm_midi_vlq(&event, (uint32_t)payload) &&
             dm_bytes(&event, editor_tag, sizeof(editor_tag)) && dm_midi_be(&event, 1, 4) &&
             dm_midi_be(&event, (uint32_t)(hash >> 32), 4) &&
             dm_midi_be(&event, (uint32_t)hash, 4) &&
             dm_bytes(&event, snapshot.data, snapshot.size);
    }

    ok = ok && dm_midi_metadata_encode(&m, out, strip_foreign, &event);
    /* The editor must be able to reopen every successfully exported file. */
    ok = ok && out->size <= 128u * 1024u * 1024u;
    dm_midi_free(&m);
    dm_bytes_free(&midi);
    dm_bytes_free(&snapshot);
    dm_bytes_free(&event);

    if (!ok) {
        dm_bytes_free(out);
        snprintf(error, cap, DM_TEXT_CANNOT_EXPORT_MIDI);
    }

    return ok;
}
