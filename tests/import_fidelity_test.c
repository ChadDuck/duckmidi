#include "core/duck.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef struct {
    uint64_t tick;
    uint32_t status, a, b, port;
    size_t order;
} FidelityEvent;

typedef struct {
    FidelityEvent *items;
    size_t count, capacity;
} EventList;

typedef struct {
    bool is_nrpn;
    uint8_t msb, lsb;
    int value;
} ParamEntry;

typedef struct {
    int port, channel;
    bool has_notes;
    int cc[120];
    int program;
    int bend;
    int channel_pressure;
    int poly_pressure[128];
    int param_kind; /* 0 = none, 1 = RPN, 2 = NRPN */
    int rpn_msb, rpn_lsb;
    int nrpn_msb, nrpn_lsb;
    ParamEntry *params;
    size_t param_count, param_capacity;
} ChannelState;

typedef struct {
    ChannelState *items;
    size_t count, capacity;
} StateMap;

typedef struct {
    uint64_t tick;
    uint32_t tempo;
} TempoEntry;

typedef struct {
    TempoEntry *items;
    size_t count, capacity;
} TempoList;

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t read_vlq(const uint8_t *data, size_t end, size_t *pos) {
    uint32_t val = 0;

    for (int i = 0; i < 4 && *pos < end; ++i) {
        uint8_t b = data[(*pos)++];
        val = (val << 7) | (b & MIDI_DATA_MASK);

        if (!(b & MIDI_STATUS)) {
            break;
        }
    }

    return val;
}

static void push_event(EventList *list, uint64_t tick, uint32_t status, uint32_t a, uint32_t b,
                       uint32_t port) {
    if (list->count == list->capacity) {
        size_t next = list->capacity ? list->capacity * 2 : 64;
        FidelityEvent *grown = realloc(list->items, next * sizeof(*grown));
        CHECK(grown != NULL);
        list->items = grown;
        list->capacity = next;
    }

    list->items[list->count] = (FidelityEvent){
        .tick = tick,
        .status = status,
        .a = a,
        .b = b,
        .port = port,
        .order = list->count,
    };
    list->count++;
}

static void free_events(EventList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int cmp_event_tick(const void *pa, const void *pb) {
    const FidelityEvent *a = pa;
    const FidelityEvent *b = pb;

    if (a->tick != b->tick) {
        return a->tick < b->tick ? -1 : 1;
    }

    if (a->order != b->order) {
        return a->order < b->order ? -1 : 1;
    }

    return 0;
}

static int cmp_event_value(const void *pa, const void *pb) {
    const FidelityEvent *a = pa;
    const FidelityEvent *b = pb;

    if (a->port != b->port) {
        return a->port < b->port ? -1 : 1;
    }

    if (a->status != b->status) {
        return a->status < b->status ? -1 : 1;
    }

    if (a->a != b->a) {
        return a->a < b->a ? -1 : 1;
    }

    if (a->b != b->b) {
        return a->b < b->b ? -1 : 1;
    }

    return 0;
}

static bool bytes_contain(const uint8_t *haystack, size_t size, const char *needle) {
    size_t nlen = strlen(needle);

    if (nlen == 0 || size < nlen) {
        return false;
    }

    for (size_t i = 0; i <= size - nlen; ++i) {
        if (memcmp(haystack + i, needle, nlen) == 0) {
            return true;
        }
    }

    return false;
}

/* Step 1: independent minimal SMF reader (format 0/1, PPQ divisions). */
static bool parse_smf(const char *name, const uint8_t *data, size_t size, EventList *out) {
    *out = (EventList){0};

    if (size < 14 || memcmp(data, "MThd", 4) != 0) {
        fprintf(stderr, "FAIL %s: invalid MThd header\n", name);
        return false;
    }

    uint32_t hdr_len = read_u32(data + 4);

    if (hdr_len < 6 || 8 + (size_t)hdr_len > size) {
        fprintf(stderr, "FAIL %s: truncated MThd header\n", name);
        return false;
    }

    uint16_t format = read_u16(data + 8);
    uint16_t num_tracks = read_u16(data + 10);
    uint16_t division = read_u16(data + 12);

    if (format > 1) {
        printf("SKIP %s: format %u\n", name, format);
        return false;
    }

    if (division & 0x8000u) {
        printf("SKIP %s: SMPTE division\n", name);
        return false;
    }

    CHECK(division > 0);
    size_t pos = 8 + (size_t)hdr_len;
    uint16_t tracks_read = 0;

    while (tracks_read < num_tracks && pos + 8 <= size) {
        uint32_t chunk_len = read_u32(data + pos + 4);
        bool is_mtrk = memcmp(data + pos, "MTrk", 4) == 0;
        pos += 8;
        size_t track_end = pos + (size_t)chunk_len;

        if (track_end > size) {
            track_end = size;
        }

        if (!is_mtrk) {
            pos = track_end;
            continue;
        }

        tracks_read++;
        uint64_t abs_tick = 0;
        uint8_t running_status = 0;
        uint32_t port = 0;

        while (pos < track_end) {
            abs_tick += read_vlq(data, track_end, &pos);

            if (pos >= track_end) {
                break;
            }

            uint64_t tick960 = (uint64_t)llround(((double)abs_tick / (double)division) * 960.0);
            uint8_t b = data[pos];

            if (b == MIDI_META) {
                pos++;
                running_status = 0;

                if (pos >= track_end) {
                    break;
                }

                uint8_t meta_type = data[pos++];
                uint32_t len = read_vlq(data, track_end, &pos);

                if (pos + len > track_end) {
                    pos = track_end;
                    break;
                }

                if (meta_type == MIDI_META_END) {
                    pos += len;
                    break;
                } else if (meta_type == MIDI_META_TEMPO && len == 3) {
                    uint32_t tempo = ((uint32_t)data[pos] << 16) | ((uint32_t)data[pos + 1] << 8) |
                                     (uint32_t)data[pos + 2];

                    if (tempo > 0) {
                        push_event(out, tick960, MIDI_META, tempo, 0, 0);
                    }
                } else if (meta_type == MIDI_META_PORT && len >= 1) {
                    port = data[pos];
                }

                pos += len;
            } else if (b == MIDI_SYSEX || b == MIDI_SYSEX_END) {
                pos++;
                running_status = 0;
                uint32_t len = read_vlq(data, track_end, &pos);
                pos = (pos + len <= track_end) ? pos + len : track_end;
            } else if (b >= 0xf1 && b <= 0xfe) {
                pos++;

                if (b < MIDI_CLOCK) {
                    running_status = 0;
                }
            } else {
                uint8_t status;

                if (b & MIDI_STATUS) {
                    status = b;
                    pos++;
                    running_status = status;
                } else {
                    CHECK(running_status >= MIDI_NOTE_OFF && running_status < MIDI_SYSTEM);
                    status = running_status;
                }

                if (pos >= track_end) {
                    break;
                }

                uint8_t d1 = data[pos++] & MIDI_DATA_MASK;
                uint8_t d2 = 0;

                if ((status & MIDI_SHORT_MASK) != MIDI_PROGRAM) {
                    if (pos >= track_end) {
                        break;
                    }

                    d2 = data[pos++] & MIDI_DATA_MASK;
                }

                if ((status & MIDI_STATUS_MASK) == MIDI_NOTE_ON && d2 == 0) {
                    status = (uint8_t)(MIDI_NOTE_OFF | (status & MIDI_CHANNEL_MASK));
                }

                push_event(out, tick960, status, d1, d2, port);
            }
        }

        pos = track_end;
    }

    if (out->count > 1) {
        qsort(out->items, out->count, sizeof(*out->items), cmp_event_tick);
    }

    return true;
}

/* Step 3: convert duckmidi timeline events with port mapping. */
static void collect_duck_events(const DmProject *p, const DmExportEvent *tl, size_t count,
                                EventList *out) {
    *out = (EventList){0};

    for (size_t i = 0; i < count; ++i) {
        DmExportEvent ev = tl[i];

        if (ev.status == MIDI_SYSEX) {
            continue;
        }

        uint32_t port = 0;

        if (ev.status != MIDI_META) {
            if (p->port == DM_PORT_COMBINED) {
                if (ev.port < (uint32_t)p->port_count) {
                    port = (uint32_t)p->ports[ev.port];
                } else {
                    port = ev.port;
                }
            } else {
                port = (uint32_t)p->port;
            }
        }

        uint32_t b = ev.b;

        if ((ev.status & MIDI_STATUS_MASK) == MIDI_NOTE_OFF) {
            b = 0;
        }

        push_event(out, ev.tick, ev.status, ev.a, b, port);
    }

    if (out->count > 1) {
        qsort(out->items, out->count, sizeof(*out->items), cmp_event_tick);
    }
}

static void init_channel_state(ChannelState *st, int port, int channel) {
    memset(st, 0, sizeof(*st));
    st->port = port;
    st->channel = channel;

    for (int i = 0; i < 120; ++i) {
        st->cc[i] = -1;
    }

    st->cc[MIDI_CC_VOLUME] = 100;
    st->cc[MIDI_CC_PAN] = 64;
    st->cc[MIDI_CC_EXPRESSION] = 127;
    st->program = 0;
    st->bend = MIDI_BEND_CENTER;
    st->channel_pressure = -1;

    for (int i = 0; i < 128; ++i) {
        st->poly_pressure[i] = -1;
    }

    st->param_kind = 0;
    st->rpn_msb = 127;
    st->rpn_lsb = 127;
    st->nrpn_msb = 127;
    st->nrpn_lsb = 127;

    st->param_capacity = 8;
    st->params = calloc(st->param_capacity, sizeof(*st->params));
    CHECK(st->params != NULL);
    st->params[0] = (ParamEntry){.is_nrpn = false, .msb = 0, .lsb = 0, .value = 2 * 128};
    st->param_count = 1;
}

static ChannelState *get_state(StateMap *map, int port, int channel) {
    for (size_t i = 0; i < map->count; ++i) {
        if (map->items[i].port == port && map->items[i].channel == channel) {
            return &map->items[i];
        }
    }

    if (map->count == map->capacity) {
        size_t next = map->capacity ? map->capacity * 2 : 32;
        ChannelState *grown = realloc(map->items, next * sizeof(*grown));
        CHECK(grown != NULL);
        map->items = grown;
        map->capacity = next;
    }

    ChannelState *st = &map->items[map->count++];
    init_channel_state(st, port, channel);
    return st;
}

static void free_state_map(StateMap *map) {
    for (size_t i = 0; i < map->count; ++i) {
        free(map->items[i].params);
    }

    free(map->items);
    *map = (StateMap){0};
}

static int get_param(const ChannelState *st, bool is_nrpn, uint8_t msb, uint8_t lsb) {
    for (size_t i = 0; i < st->param_count; ++i) {
        const ParamEntry *e = &st->params[i];

        if (e->is_nrpn == is_nrpn && e->msb == msb && e->lsb == lsb) {
            return e->value;
        }
    }

    return -1;
}

static void set_param(ChannelState *st, bool is_nrpn, uint8_t msb, uint8_t lsb, int value) {
    for (size_t i = 0; i < st->param_count; ++i) {
        if (st->params[i].is_nrpn == is_nrpn && st->params[i].msb == msb &&
            st->params[i].lsb == lsb) {
            st->params[i].value = value;
            return;
        }
    }

    if (st->param_count == st->param_capacity) {
        size_t next = st->param_capacity ? st->param_capacity * 2 : 8;
        ParamEntry *grown = realloc(st->params, next * sizeof(*grown));
        CHECK(grown != NULL);
        st->params = grown;
        st->param_capacity = next;
    }

    st->params[st->param_count++] = (ParamEntry){
        .is_nrpn = is_nrpn,
        .msb = msb,
        .lsb = lsb,
        .value = value,
    };
}

static bool is_param_cc(uint32_t cc) {
    return cc == MIDI_CC_DATA || cc == MIDI_CC_DATA_LSB || cc == MIDI_CC_INCREMENT ||
           cc == MIDI_CC_DECREMENT || cc == MIDI_CC_NRPN_LSB || cc == MIDI_CC_NRPN_MSB ||
           cc == MIDI_CC_RPN_LSB || cc == MIDI_CC_RPN_MSB;
}

/* Step 4: apply a single non-note channel event to the state simulator. */
static void apply_channel_event(StateMap *map, FidelityEvent ev) {
    int port = (int)ev.port;
    int ch = (int)(ev.status & MIDI_CHANNEL_MASK);
    uint32_t kind = ev.status & MIDI_STATUS_MASK;
    ChannelState *st = get_state(map, port, ch);

    if (kind == MIDI_KEY_PRESSURE) {
        st->poly_pressure[ev.a & MIDI_DATA_MASK] = (int)ev.b;
    } else if (kind == MIDI_PROGRAM) {
        st->program = (int)ev.a;
    } else if (kind == MIDI_CHANNEL_PRESSURE) {
        st->channel_pressure = (int)ev.a;
    } else if (kind == MIDI_PITCH_BEND) {
        st->bend = (int)ev.a | ((int)ev.b << 7);
    } else if (kind == MIDI_CONTROL && ev.a < 120) {
        int cc = (int)ev.a;
        int val = (int)ev.b;

        if (cc == MIDI_CC_RPN_MSB) {
            st->rpn_msb = val;
            st->param_kind = (st->rpn_msb == 127 && st->rpn_lsb == 127) ? 0 : 1;
        } else if (cc == MIDI_CC_RPN_LSB) {
            st->rpn_lsb = val;
            st->param_kind = (st->rpn_msb == 127 && st->rpn_lsb == 127) ? 0 : 1;
        } else if (cc == MIDI_CC_NRPN_MSB) {
            st->nrpn_msb = val;
            st->param_kind = (st->nrpn_msb == 127 && st->nrpn_lsb == 127) ? 0 : 2;
        } else if (cc == MIDI_CC_NRPN_LSB) {
            st->nrpn_lsb = val;
            st->param_kind = (st->nrpn_msb == 127 && st->nrpn_lsb == 127) ? 0 : 2;
        } else if (cc == MIDI_CC_DATA || cc == MIDI_CC_DATA_LSB || cc == MIDI_CC_INCREMENT ||
                   cc == MIDI_CC_DECREMENT) {
            if (st->param_kind != 0) {
                bool is_nrpn = st->param_kind == 2;
                uint8_t msb = (uint8_t)(is_nrpn ? st->nrpn_msb : st->rpn_msb);
                uint8_t lsb = (uint8_t)(is_nrpn ? st->nrpn_lsb : st->rpn_lsb);
                int cur = get_param(st, is_nrpn, msb, lsb);
                int base = cur < 0 ? 0 : cur;

                if (cc == MIDI_CC_DATA) {
                    set_param(st, is_nrpn, msb, lsb, (val << 7) | (base & 0x7f));
                } else if (cc == MIDI_CC_DATA_LSB) {
                    set_param(st, is_nrpn, msb, lsb, (base & 0x3f80) | val);
                } else if (cc == MIDI_CC_INCREMENT) {
                    int p_msb = (base >> 7) & 0x7f;

                    if (p_msb < 127) {
                        p_msb++;
                    }

                    set_param(st, is_nrpn, msb, lsb, (p_msb << 7) | (base & 0x7f));
                } else {
                    int p_msb = (base >> 7) & 0x7f;

                    if (p_msb > 0) {
                        p_msb--;
                    }

                    set_param(st, is_nrpn, msb, lsb, (p_msb << 7) | (base & 0x7f));
                }
            }
        } else {
            st->cc[cc] = val;
        }
    }
}

static void check_channel_states_equal(const char *file, uint64_t tick, int port, int ch,
                                       const ChannelState *fa, const ChannelState *da) {
    ChannelState default_state;

    if (!fa) {
        init_channel_state(&default_state, port, ch);
        fa = &default_state;
    }

    if (!da) {
        init_channel_state(&default_state, port, ch);
        da = &default_state;
    }

    if (fa->program != da->program) {
        fprintf(stderr, "FAIL %s tick=%llu port=%d ch=%d program: file=%d duck=%d\n", file,
                (unsigned long long)tick, port, ch, fa->program, da->program);
        exit(1);
    }

    if (fa->bend != da->bend) {
        fprintf(stderr, "FAIL %s tick=%llu port=%d ch=%d bend: file=%d duck=%d\n", file,
                (unsigned long long)tick, port, ch, fa->bend, da->bend);
        exit(1);
    }

    if (fa->channel_pressure != da->channel_pressure) {
        fprintf(stderr, "FAIL %s tick=%llu port=%d ch=%d channel_pressure: file=%d duck=%d\n", file,
                (unsigned long long)tick, port, ch, fa->channel_pressure, da->channel_pressure);
        exit(1);
    }

    for (int cc = 0; cc < 120; ++cc) {
        if (is_param_cc((uint32_t)cc)) {
            continue;
        }

        if (fa->cc[cc] != da->cc[cc]) {
            fprintf(stderr, "FAIL %s tick=%llu port=%d ch=%d CC%d: file=%d duck=%d\n", file,
                    (unsigned long long)tick, port, ch, cc, fa->cc[cc], da->cc[cc]);
            exit(1);
        }
    }

    for (int k = 0; k < 128; ++k) {
        if (fa->poly_pressure[k] != da->poly_pressure[k]) {
            fprintf(stderr, "FAIL %s tick=%llu port=%d ch=%d poly_pressure[%d]: file=%d duck=%d\n",
                    file, (unsigned long long)tick, port, ch, k, fa->poly_pressure[k],
                    da->poly_pressure[k]);
            exit(1);
        }
    }

    for (size_t i = 0; i < fa->param_count; ++i) {
        ParamEntry e = fa->params[i];
        int dv = get_param(da, e.is_nrpn, e.msb, e.lsb);

        if (e.value != dv) {
            fprintf(stderr, "FAIL %s tick=%llu port=%d ch=%d %s(%u/%u): file=%d duck=%d\n", file,
                    (unsigned long long)tick, port, ch, e.is_nrpn ? "NRPN" : "RPN", e.msb, e.lsb,
                    e.value, dv);
            exit(1);
        }
    }

    for (size_t i = 0; i < da->param_count; ++i) {
        ParamEntry e = da->params[i];
        int fv = get_param(fa, e.is_nrpn, e.msb, e.lsb);

        if (fv != e.value) {
            fprintf(stderr, "FAIL %s tick=%llu port=%d ch=%d %s(%u/%u): file=%d duck=%d\n", file,
                    (unsigned long long)tick, port, ch, e.is_nrpn ? "NRPN" : "RPN", e.msb, e.lsb,
                    fv, e.value);
            exit(1);
        }
    }

    if (fa == &default_state || da == &default_state) {
        free(default_state.params);
    }
}

static void collect_tempos(const EventList *events, TempoList *out) {
    *out = (TempoList){0};
    uint32_t last_tempo = 500000;
    size_t i = 0;

    while (i < events->count) {
        uint64_t tick = events->items[i].tick;
        uint32_t tick_tempo = 0;
        bool has_tempo = false;

        while (i < events->count && events->items[i].tick == tick) {
            if (events->items[i].status == MIDI_META) {
                tick_tempo = events->items[i].a;
                has_tempo = true;
            }

            i++;
        }

        if (has_tempo && tick_tempo != last_tempo) {
            if (out->count == out->capacity) {
                size_t next = out->capacity ? out->capacity * 2 : 16;
                TempoEntry *grown = realloc(out->items, next * sizeof(*grown));
                CHECK(grown != NULL);
                out->items = grown;
                out->capacity = next;
            }

            out->items[out->count++] = (TempoEntry){.tick = tick, .tempo = tick_tempo};
            last_tempo = tick_tempo;
        }
    }
}

static void compare_tempos(const char *file, const EventList *file_events,
                           const EventList *duck_events) {
    TempoList ft, dt;
    collect_tempos(file_events, &ft);
    collect_tempos(duck_events, &dt);

    /* The import starts the song at the file's first tempo when it comes no later than the first
       note (import_initial_tempo); the file plays its default tempo only before it. */
    uint64_t first_note = UINT64_MAX;
    uint64_t first_tempo = UINT64_MAX;

    for (size_t i = 0; i < file_events->count; ++i) {
        const FidelityEvent *e = &file_events->items[i];

        if (e->status == MIDI_META && first_tempo == UINT64_MAX) {
            first_tempo = e->tick;
        } else if ((e->status & MIDI_STATUS_MASK) == MIDI_NOTE_ON && e->b) {
            first_note = e->tick;
            break;
        }
    }

    if (ft.count && ft.items[0].tick == first_tempo && first_tempo <= first_note &&
        first_note != UINT64_MAX) {
        ft.items[0].tick = 0;
    }
    size_t n = ft.count < dt.count ? ft.count : dt.count;

    for (size_t i = 0; i < n; ++i) {
        if (ft.items[i].tick != dt.items[i].tick || ft.items[i].tempo != dt.items[i].tempo) {
            fprintf(stderr, "FAIL %s tick=%llu tempo: file=(%llu, %u) duck=(%llu, %u)\n", file,
                    (unsigned long long)ft.items[i].tick, (unsigned long long)ft.items[i].tick,
                    ft.items[i].tempo, (unsigned long long)dt.items[i].tick, dt.items[i].tempo);
            exit(1);
        }
    }

    if (ft.count != dt.count) {
        uint64_t tick = ft.count > n ? ft.items[n].tick : dt.items[n].tick;
        fprintf(stderr, "FAIL %s tick=%llu tempo count: file=%zu duck=%zu\n", file,
                (unsigned long long)tick, ft.count, dt.count);
        exit(1);
    }

    free(ft.items);
    free(dt.items);
}

static void compare_multisets(const char *file, const char *what, uint64_t tick, EventList *fa,
                              EventList *da) {
    if (fa->count > 1) {
        qsort(fa->items, fa->count, sizeof(*fa->items), cmp_event_value);
    }

    if (da->count > 1) {
        qsort(da->items, da->count, sizeof(*da->items), cmp_event_value);
    }

    if (fa->count != da->count) {
        fprintf(stderr, "FAIL %s tick=%llu %s count: file=%zu duck=%zu\n", file,
                (unsigned long long)tick, what, fa->count, da->count);
        exit(1);
    }

    for (size_t i = 0; i < fa->count; ++i) {
        if (cmp_event_value(&fa->items[i], &da->items[i]) != 0) {
            fprintf(stderr,
                    "FAIL %s tick=%llu port=%u ch=%u %s: file=(0x%02x %u %u) duck=(0x%02x %u %u)\n",
                    file, (unsigned long long)tick, fa->items[i].port,
                    fa->items[i].status & MIDI_CHANNEL_MASK, what, fa->items[i].status,
                    fa->items[i].a, fa->items[i].b, da->items[i].status, da->items[i].a,
                    da->items[i].b);
            exit(1);
        }
    }
}

/* Removes from *from the first event equal in status, key and port to ev; false when none is. */
static bool take_event(EventList *from, FidelityEvent ev) {
    for (size_t i = 0; i < from->count; ++i) {
        FidelityEvent x = from->items[i];

        if (x.status == ev.status && x.a == ev.a && x.port == ev.port) {
            from->items[i] = from->items[--from->count];
            return true;
        }
    }

    return false;
}

/* Note-offs at tick: duckmidi gives every note at least one tick, so a file note-off may come one
   tick later. carried holds the file's unmatched note-offs from the previous tick (at
   carried_tick); this tick's unmatched ones replace them. */
static void match_note_offs(const char *file, uint64_t tick, EventList *f_offs, EventList *d_offs,
                            EventList *carried, uint64_t *carried_tick) {
    if (carried->count) {
        if (*carried_tick + 1 == tick) {
            for (size_t i = carried->count; i-- > 0;) {
                if (take_event(d_offs, carried->items[i])) {
                    carried->items[i] = carried->items[--carried->count];
                }
            }
        }

        if (carried->count) {
            FidelityEvent e = carried->items[0];
            fprintf(stderr, "FAIL %s tick=%llu port=%u ch=%u note-off key %u missing\n", file,
                    (unsigned long long)*carried_tick, e.port, e.status & MIDI_CHANNEL_MASK, e.a);
            exit(1);
        }
    }

    for (size_t i = f_offs->count; i-- > 0;) {
        if (take_event(d_offs, f_offs->items[i])) {
            f_offs->items[i] = f_offs->items[--f_offs->count];
        }
    }

    if (d_offs->count) {
        FidelityEvent e = d_offs->items[0];
        fprintf(stderr, "FAIL %s tick=%llu port=%u ch=%u note-off key %u extra\n", file,
                (unsigned long long)tick, e.port, e.status & MIDI_CHANNEL_MASK, e.a);
        exit(1);
    }

    carried->count = 0;

    for (size_t i = 0; i < f_offs->count; ++i) {
        push_event(carried, tick, f_offs->items[i].status, f_offs->items[i].a, 0,
                   f_offs->items[i].port);
    }

    *carried_tick = tick;
}

/* Step 5: walk both event lists tick by tick and verify notes, mode messages, and state. */
static void compare_event_lists(const char *file, const EventList *file_events,
                                const EventList *duck_events) {
    compare_tempos(file, file_events, duck_events);
    StateMap file_map = {0};
    StateMap duck_map = {0};
    size_t fi = 0;
    size_t di = 0;
    EventList carried = {0};
    uint64_t carried_tick = 0;

    while (fi < file_events->count || di < duck_events->count) {
        uint64_t tick = UINT64_MAX;

        if (fi < file_events->count && file_events->items[fi].tick < tick) {
            tick = file_events->items[fi].tick;
        }

        if (di < duck_events->count && duck_events->items[di].tick < tick) {
            tick = duck_events->items[di].tick;
        }

        EventList f_notes = {0}, d_notes = {0}, f_offs = {0}, d_offs = {0};
        EventList f_mode = {0}, d_mode = {0};
        EventList note_on_channels = {0};

        while (fi < file_events->count && file_events->items[fi].tick == tick) {
            FidelityEvent ev = file_events->items[fi++];
            uint32_t kind = ev.status & MIDI_STATUS_MASK;

            if (ev.status == MIDI_META) {
                continue;
            } else if (kind == MIDI_NOTE_ON || kind == MIDI_NOTE_OFF) {
                if (kind == MIDI_NOTE_OFF) {
                    push_event(&f_offs, tick, ev.status, ev.a, 0, ev.port);
                    continue;
                }

                get_state(&file_map, (int)ev.port, (int)(ev.status & MIDI_CHANNEL_MASK))
                    ->has_notes = true;
                push_event(&note_on_channels, tick, ev.status & MIDI_CHANNEL_MASK, 0, 0, ev.port);
                push_event(&f_notes, tick, ev.status, ev.a, ev.b, ev.port);
            } else if (kind == MIDI_CONTROL && ev.a >= 120) {
                push_event(&f_mode, tick, ev.status, ev.a, ev.b, ev.port);
            } else {
                apply_channel_event(&file_map, ev);
            }
        }

        while (di < duck_events->count && duck_events->items[di].tick == tick) {
            FidelityEvent ev = duck_events->items[di++];
            uint32_t kind = ev.status & MIDI_STATUS_MASK;

            if (ev.status == MIDI_META) {
                continue;
            } else if (kind == MIDI_NOTE_ON || kind == MIDI_NOTE_OFF) {
                if (kind == MIDI_NOTE_OFF) {
                    push_event(&d_offs, tick, ev.status, ev.a, 0, ev.port);
                    continue;
                }

                get_state(&duck_map, (int)ev.port, (int)(ev.status & MIDI_CHANNEL_MASK))
                    ->has_notes = true;
                push_event(&d_notes, tick, ev.status, ev.a, ev.b, ev.port);
            } else if (kind == MIDI_CONTROL && ev.a >= 120) {
                push_event(&d_mode, tick, ev.status, ev.a, ev.b, ev.port);
            } else {
                apply_channel_event(&duck_map, ev);
            }
        }

        compare_multisets(file, "note-ons", tick, &f_notes, &d_notes);
        match_note_offs(file, tick, &f_offs, &d_offs, &carried, &carried_tick);
        compare_multisets(file, "mode", tick, &f_mode, &d_mode);

        for (size_t i = 0; i < note_on_channels.count; ++i) {
            int port = (int)note_on_channels.items[i].port;
            int ch = (int)note_on_channels.items[i].status;
            const ChannelState *fa = get_state(&file_map, port, ch);
            const ChannelState *da = get_state(&duck_map, port, ch);
            check_channel_states_equal(file, tick, port, ch, fa, da);
        }

        free_events(&f_notes);
        free_events(&d_notes);
        free_events(&f_offs);
        free_events(&d_offs);
        free_events(&f_mode);
        free_events(&d_mode);
        free_events(&note_on_channels);
    }

    for (size_t i = 0; i < file_map.count; ++i) {
        const ChannelState *fa = &file_map.items[i];

        if (!fa->has_notes) {
            continue;
        }

        const ChannelState *da = get_state(&duck_map, fa->port, fa->channel);
        check_channel_states_equal(file, UINT64_MAX, fa->port, fa->channel, fa, da);
    }

    for (size_t i = 0; i < duck_map.count; ++i) {
        const ChannelState *da = &duck_map.items[i];

        if (!da->has_notes) {
            continue;
        }

        const ChannelState *fa = get_state(&file_map, da->port, da->channel);
        check_channel_states_equal(file, UINT64_MAX, da->port, da->channel, fa, da);
    }

    CHECK(!carried.count);
    free_events(&carried);
    free_state_map(&file_map);
    free_state_map(&duck_map);
}

static void verify_midi_bytes(const char *label, const uint8_t *data, size_t size,
                              bool can_skip_project) {
    if (can_skip_project && bytes_contain(data, size, "DUCKMIDI")) {
        printf("SKIP %s: duckmidi project\n", label);
        return;
    }

    EventList file_events = {0};

    if (!parse_smf(label, data, size, &file_events)) {
        free_events(&file_events);
        return;
    }

    DmProject p;
    dm_project_init(&p);
    char error[256] = {0};
    bool imported = dm_import(&p, data, size, -1, 0, error, sizeof(error));

    if (!imported && can_skip_project && strstr(error, "duckmidi editor information") != NULL) {
        printf("SKIP %s: duckmidi project\n", label);
        dm_project_free(&p);
        free_events(&file_events);
        return;
    }

    if (!imported) {
        fprintf(stderr, "FAIL %s: dm_import failed: %s\n", label, error);
    }

    CHECK(imported);
    DmExportEvent *tl = NULL;
    size_t tl_count = 0;
    uint64_t duration = 0;
    CHECK(dm_export_timeline(&p, &tl, &tl_count, &duration, error, sizeof(error)));

    EventList duck_events = {0};
    collect_duck_events(&p, tl, tl_count, &duck_events);
    compare_event_lists(label, &file_events, &duck_events);
    free(tl);
    free_events(&duck_events);

    /* Export-and-re-import roundtrip check. */
    DmBytes re_mid = {0};
    CHECK(dm_midi(&p, &re_mid, error, sizeof(error)));
    DmProject p2;
    dm_project_init(&p2);
    CHECK(dm_import(&p2, re_mid.data, re_mid.size, -1, 0, error, sizeof(error)));
    DmExportEvent *tl2 = NULL;
    size_t tl2_count = 0;
    uint64_t duration2 = 0;
    CHECK(dm_export_timeline(&p2, &tl2, &tl2_count, &duration2, error, sizeof(error)));

    EventList duck_events2 = {0};
    collect_duck_events(&p2, tl2, tl2_count, &duck_events2);
    compare_event_lists(label, &file_events, &duck_events2);

    free(tl2);
    free_events(&duck_events2);
    dm_project_free(&p2);
    dm_bytes_free(&re_mid);
    dm_project_free(&p);
    free_events(&file_events);
    printf("PASS: %s\n", label);
}

/* Step 6: sample MIDI files. */
static void test_sample(const char *name);

static void test_samples(void) {
    static const char *const samples[] = {
        "D_46_D15",
        "astley",
        "beat-it",
        "carwash",
        "d_betwee",
        "dream_express",
        "gm",
        "gm_clean",
        "imp_night",
        "knock",
        "large-complex",
        "lover-format0",
        "lover-format1",
        "move",
        "plastic",
        "stalks",
        "stickerbrush",
        "stillill",
        "youkai",
    };

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        test_sample(samples[i]);
    }
}

/* One sample file by name (without samples/ and .mid). */
static void test_sample(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "samples/%s.mid", name);
    DmBytes bytes = {0};
    char error[256] = {0};

    if (!dm_read(path, &bytes, error, sizeof(error))) {
        printf("SKIP %s: missing\n", path);
        return;
    }

    verify_midi_bytes(path, bytes.data, bytes.size, true);
    dm_bytes_free(&bytes);
}

/* Step 7: three built files always run. */
static void test_built_fixtures(void) {
    static const uint8_t nrpn_fixture[] = {
        'M',  'T',  'h', 'd', 0,    0,    0,    6,  0,    0,    0,    1,    0x01,
        0xe0, 'M',  'T', 'r', 'k',  0,    0,    0,  30,   0x00, 0xb0, 99,   1,
        0x00, 0xb0, 98,  2,   0x00, 0xb0, 6,    40, 0x00, 0xb0, 38,   50,   0x83,
        0x60, 0x90, 60,  100, 0x83, 0x60, 0x80, 60, 0,    0x00, 0xff, 0x2f, 0x00,
    };

    static const uint8_t reset_twice_fixture[] = {
        'M',  'T',  'h',  'd',  0,    0,    0,    6,    0,    0,    0,    1,    0x01,
        0xe0, 'M',  'T',  'r',  'k',  0,    0,    0,    28,   0x00, 0x90, 60,   100,
        0x83, 0x60, 0xb0, 121,  0,    0x83, 0x60, 0xb0, 1,    64,   0x83, 0x60, 0xb0,
        121,  0,    0x83, 0x60, 0x80, 60,   0,    0x00, 0xff, 0x2f, 0x00,
    };

    static const uint8_t gs_reset_volume_fixture[] = {
        'M',  'T',  'h',  'd',  0,    0,    0,    6,    0,    0,    0,    1,    0x01, 0xe0,
        'M',  'T',  'r',  'k',  0,    0,    0,    31,   0x00, 0xf0, 0x0a, 0x41, 0x10, 0x42,
        0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7, 0x00, 0xb0, 7,    55,   0x83, 0x60, 0x90,
        60,   100,  0x83, 0x60, 0x80, 60,   0,    0x00, 0xff, 0x2f, 0x00,
    };

    verify_midi_bytes("built:nrpn", nrpn_fixture, sizeof(nrpn_fixture), false);
    verify_midi_bytes("built:reset_twice", reset_twice_fixture, sizeof(reset_twice_fixture), false);
    verify_midi_bytes("built:gs_reset_volume", gs_reset_volume_fixture,
                      sizeof(gs_reset_volume_fixture), false);
}

/* With arguments, checks only the named sample files (for example: import_fidelity_test knock). */
int main(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            test_sample(argv[i]);
        }
    } else {
        test_built_fixtures();
        test_samples();
    }

    printf("PASS: %d checks (import fidelity)\n", checks);
    return 0;
}
