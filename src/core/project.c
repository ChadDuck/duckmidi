#include "core/project_internal.h"
#include "text/en.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool dm_pack(const DmProject *p, DmBytes *out) {
    DmBytes b = {0};
    bool ok = dm_write_body(p, &b);

    /* The body is compressed; the header and checksum describe the stored bytes. */
    DmBytes stored = {0};
    ok = ok && dm_compress(b.data, b.size, &stored) && stored.size <= UINT32_MAX;
    ok = ok && dm_bytes(out, "DUCKMIDI", 8) && dm_put_fixed32(out, DM_PROJECT_SCHEMA) &&
         dm_put_fixed32(out, (uint32_t)stored.size) &&
         dm_put_fixed32(out, dm_crc32(stored.data, stored.size)) &&
         dm_bytes(out, stored.data, stored.size);
    dm_bytes_free(&stored);
    dm_bytes_free(&b);

    if (!ok) {
        dm_bytes_free(out);
    }

    return ok;
}

static bool valid_meta(const DmBytes *m) {
    if (m->data[0] != MIDI_META) {
        return true;
    }

    if (m->data[1] == MIDI_META_TEMPO) {
        return dm_tempo_microseconds(m->data, m->size) != 0;
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

bool dm_unpack_document(DmProject *p, const unsigned char *data, size_t size, char *error,
                        size_t cap, bool allow_views) {
    if (size < 20 || memcmp(data, "DUCKMIDI", 8)) {
        snprintf(error, cap, DM_TEXT_NOT_DUCKMIDI_PROJECT);
        return false;
    }

    Input in = {data + 8, size - 8, true, 0};
    uint32_t version = dm_get_fixed32(&in), payload = dm_get_fixed32(&in),
             crc = dm_get_fixed32(&in);

    if (version < DM_PROJECT_SCHEMA_OLDEST || version > DM_PROJECT_SCHEMA) {
        snprintf(error, cap, DM_TEXT_UNSUPPORTED_PROJECT_SCHEMA, version, DM_PROJECT_SCHEMA_OLDEST,
                 DM_PROJECT_SCHEMA);
        return false;
    }

    in.schema = version;

    if (payload != in.left || crc != dm_crc32(in.p, in.left)) {
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
    bool newer = false;
    bool unpacked = version >= 20 ? dm_read_body(&next, &in, allow_views, &newer)
                                  : dm_read_body_legacy(&next, &in, allow_views);
    dm_bytes_free(&body);

    if (!unpacked) {
        dm_project_free(&next);
        snprintf(error, cap,
                 newer ? DM_TEXT_PROJECT_NEWER_DUCKMIDI : DM_TEXT_PROJECT_CONTAINS_INVALID);
        return false;
    }

    if (version < DM_PROJECT_SCHEMA) {
        dm_project_upgrade(&next, version);
    }

    /* After the upgrade: older schemas may hold data the current rules no longer allow until
       their conversion has run. */
    if (!dm_project_validate(&next, NULL, 0)) {
        dm_project_free(&next);
        snprintf(error, cap, DM_TEXT_PROJECT_CONTAINS_INVALID);
        return false;
    }

    dm_project_free(p);
    *p = next;
    return true;
}

bool dm_unpack(DmProject *p, const unsigned char *data, size_t size, char *error, size_t cap) {
    return dm_unpack_document(p, data, size, error, cap, true);
}

bool dm_validate_view(const DmProject *parent, const DmView *view) {
    DmProject child;
    dm_project_init(&child);
    char error[256];
    bool ok =
        dm_unpack_document(&child, view->state.data, view->state.size, error, sizeof(error), false);

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

int dm_view_index(const DmProject *p, int port, int sequence) {
    for (size_t i = 0; i < p->view_count; ++i) {
        if (p->views[i].port == port && p->views[i].sequence == sequence) {
            return (int)i;
        }
    }

    return -1;
}

bool dm_valid_view_key(const DmProject *p, int port, int sequence) {
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

static bool store_view(DmProject *p, DmBytes *snapshot) {
    int index = dm_view_index(p, p->port, p->sequence);
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
        bool ok =
            dm_unpack_document(&recent, view->state.data, view->state.size, error, cap, false);

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
    memcpy(next->percussion_midi, p->percussion_midi, sizeof(next->percussion_midi));
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
    if (!p->original.size || !dm_valid_view_key(p, port, sequence)) {
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
    int index = dm_view_index(p, port, sequence);
    bool ok = index >= 0 ? dm_unpack_document(&next, p->views[index].state.data,
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
