/* Developer input inspector. Driver callbacks only copy into fixed SPSC rings. */
#include "midi_input.h"
#include "midi_input_core.h"
#include "rtmidi_c.h"
#include <SDL3/SDL.h>
#include <signal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPTURE 4096
#define RAW_BYTES 256

typedef struct {
    uint64_t ns;
    size_t size;
    uint8_t bytes[RAW_BYTES];
} Capture;

typedef struct {
    unsigned count, min, max, last, peak, second_count;
    uint64_t values[2], second, last_ns;
    bool paired, rpn;
} Range;

typedef struct {
    uint8_t manufacturer[3];
    size_t shortest, longest;
    unsigned count;
} SysExSummary;

typedef struct {
    RtMidiInPtr device;
    char name[256];
    Capture ring[CAPTURE];
    atomic_uint read, write, dropped;
    unsigned counts[16][7], poly[128], held[16][128];
    Range cc[128], bend, velocity, keys;
    unsigned pressure[2], filtered, zero_off, sensing, clocks, sysex, pairs, rpn;
    SysExSummary manufacturers[64];
    unsigned manufacturer_count;
    uint64_t first_clock, last_clock;
    uint64_t previous;
} Port;

static volatile sig_atomic_t stopping;

static void stop(int signal_value) {
    (void)signal_value;
    stopping = 1;
}

static void capture(double delta, const unsigned char *bytes, size_t size, void *context) {
    (void)delta;
    Port *port = context;
    unsigned w = atomic_load_explicit(&port->write, memory_order_relaxed);

    if (w - atomic_load_explicit(&port->read, memory_order_acquire) == CAPTURE) {
        atomic_fetch_add(&port->dropped, 1);
        return;
    }

    Capture *item = &port->ring[w % CAPTURE];
    item->ns = SDL_GetTicksNS();
    item->size = size;
    memset(item->bytes, 0, 4);
    memcpy(item->bytes, bytes, size < RAW_BYTES ? size : RAW_BYTES);
    atomic_store_explicit(&port->write, w + 1, memory_order_release);
}

static void range_add(Range *range, unsigned value, uint64_t ns) {
    if (!range->count || value < range->min) {
        range->min = value;
    }

    if (!range->count || value > range->max) {
        range->max = value;
    }

    ++range->count;
    range->last = value;

    if (value < 128) {
        range->values[value / 64] |= UINT64_C(1) << (value % 64);
    }

    if (ns / 1000000000 != range->second) {
        range->second = ns / 1000000000;
        range->second_count = 0;
    }

    if (++range->second_count > range->peak) {
        range->peak = range->second_count;
    }

    range->last_ns = ns;
}

static unsigned distinct(const Range *range) {
    unsigned n = 0;

    for (unsigned i = 0; i < 128; ++i) {
        n += (range->values[i / 64] >> (i % 64)) & 1;
    }

    return n;
}

static void note_name(unsigned key, char *name, size_t cap) {
    const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(name, cap, "%s%d", names[key % 12], (int)key / 12 - 1);
}

static const char *verdict_name(DmMidiVerdict verdict, unsigned type) {
    switch (verdict) {
    case DM_MIDI_RECORD:
        switch (type) {
        case MIDI_NOTE_ON:
        case MIDI_NOTE_OFF:
            return "record: note";
        case MIDI_CONTROL:
            return "record: CC automation";
        case MIDI_PITCH_BEND:
            return "record: pitch bend";
        default:
            return "record: pressure";
        }
    case DM_MIDI_MONITOR:
        return "monitor only: reset controllers";
    case DM_MIDI_RELEASE:
        return "release: all notes off";
    case DM_MIDI_INVALID:
        return "filtered: malformed";
    default:
        return "filtered: system/deferred";
    }
}

static void describe(Port *port, const Capture *item, char *decoded, size_t cap) {
    const uint8_t *b = item->bytes;
    unsigned type = b[0] & MIDI_STATUS_MASK, channel = b[0] & MIDI_CHANNEL_MASK;
    char name[16];
    note_name(b[1] & MIDI_DATA_MASK, name, sizeof(name));

    if (b[0] < MIDI_SYSTEM && b[0] >= MIDI_STATUS && item->size >= 2) {
        ++port->counts[channel][(type - MIDI_NOTE_OFF) / 16];
    }

    switch (type) {
    case MIDI_NOTE_ON:
    case MIDI_NOTE_OFF:
        snprintf(decoded, cap, "%s %s (%u) vel %u", type == MIDI_NOTE_ON ? "Note On" : "Note Off",
                 name, b[1], b[2]);
        port->held[channel][b[1] & MIDI_DATA_MASK] = type == MIDI_NOTE_ON && b[2];
        port->zero_off += type == MIDI_NOTE_ON && !b[2];
        range_add(&port->keys, b[1], item->ns);

        if (type == MIDI_NOTE_ON && b[2]) {
            range_add(&port->velocity, b[2], item->ns);
        }

        break;
    case MIDI_CONTROL:
        snprintf(decoded, cap, "CC %u = %u", b[1], b[2]);
        range_add(&port->cc[b[1] & MIDI_DATA_MASK], b[2], item->ns);

        if (b[1] >= 32 && b[1] < 64 && port->cc[b[1] - 32].last_ns &&
            item->ns - port->cc[b[1] - 32].last_ns < 10000000) {
            ++port->pairs;
            port->cc[b[1]].paired = port->cc[b[1] - 32].paired = true;
        }

        if ((b[1] == MIDI_CC_DATA || b[1] == MIDI_CC_DATA_LSB) &&
            (port->cc[MIDI_CC_RPN_MSB].count || port->cc[MIDI_CC_RPN_LSB].count ||
             port->cc[MIDI_CC_NRPN_MSB].count || port->cc[MIDI_CC_NRPN_LSB].count)) {
            ++port->rpn;
            port->cc[b[1]].rpn = true;
        }

        if (b[1] == MIDI_CC_NOTES_OFF || b[1] == MIDI_CC_SOUND_OFF) {
            memset(port->held[channel], 0, sizeof(port->held[channel]));
        }

        break;
    case MIDI_PITCH_BEND: {
        unsigned bend = b[1] | (b[2] << 7);
        snprintf(decoded, cap, "Bend %+d (%u)", (int)bend - MIDI_BEND_CENTER, bend);
        range_add(&port->bend, bend, item->ns);
        break;
    }
    case MIDI_KEY_PRESSURE:
        snprintf(decoded, cap, "Poly pressure %s = %u", name, b[2]);
        ++port->pressure[1];
        ++port->poly[b[1] & MIDI_DATA_MASK];
        break;
    case MIDI_CHANNEL_PRESSURE:
        snprintf(decoded, cap, "Channel pressure = %u", b[1]);
        ++port->pressure[0];
        break;
    case MIDI_PROGRAM:
        snprintf(decoded, cap, "Program change %u (deferred)", b[1]);
        break;
    default:
        if (b[0] == MIDI_SYSEX) {
            ++port->sysex;
            uint8_t id[] = {b[1], b[1] ? 0 : b[2], b[1] ? 0 : b[3]};
            unsigned i = 0;

            for (; i < port->manufacturer_count; ++i) {
                if (!memcmp(port->manufacturers[i].manufacturer, id, sizeof(id))) {
                    break;
                }
            }

            if (i < 64) {
                SysExSummary *s = &port->manufacturers[i];

                if (i == port->manufacturer_count) {
                    ++port->manufacturer_count;
                    s->shortest = s->longest = item->size;
                    memcpy(s->manufacturer, id, sizeof(id));
                }

                s->shortest = item->size < s->shortest ? item->size : s->shortest;
                s->longest = item->size > s->longest ? item->size : s->longest;
                ++s->count;
            }

            if (b[1]) {
                snprintf(decoded, cap, "SysEx (%zu bytes, manufacturer %02X)", item->size, b[1]);
            } else {
                snprintf(decoded, cap, "SysEx (%zu bytes, manufacturer 00 %02X %02X)", item->size,
                         b[2], b[3]);
            }
        } else if (b[0] == MIDI_CLOCK) {
            if (!port->clocks) {
                port->first_clock = item->ns;
            }

            ++port->clocks;
            port->last_clock = item->ns;
            snprintf(decoded, cap, "Clock");
        } else if (b[0] == MIDI_ACTIVE_SENSING) {
            ++port->sensing;
            snprintf(decoded, cap, "Active sensing");
        } else {
            snprintf(decoded, cap, "System %02X (%zu bytes)", b[0], item->size);
        }

        break;
    }
}

/* Endless encoders in relative mode send small steps around 0 (1..15 up, 113..127 down) or
   around 64 (65..79 up, 49..63 down). Values only on one side are not relative. */
static bool relative(const Range *range) {
    bool zero_up = false, zero_down = false, centre_up = false, centre_down = false;

    for (unsigned v = 0; v < 128; ++v) {
        if (!((range->values[v / 64] >> (v % 64)) & 1)) {
            continue;
        }

        bool near_zero = (v >= 1 && v <= 15) || v >= 113;
        bool near_centre = v >= 49 && v <= 79 && v != 64;

        if (!near_zero && !near_centre) {
            return false;
        }

        zero_up |= v >= 1 && v <= 15;
        zero_down |= v >= 113;
        centre_up |= v >= 65 && v <= 79;
        centre_down |= v >= 49 && v <= 63;
    }

    return (zero_up && zero_down && !centre_up && !centre_down) ||
           (centre_up && centre_down && !zero_up && !zero_down);
}

static void print_range(FILE *out, const char *label, const Range *range) {
    if (range->count) {
        fprintf(out, " %s=%u..%u", label, range->min, range->max);
    } else {
        fprintf(out, " %s=none", label);
    }
}

static void summary(Port *port, unsigned index, FILE *out, double seconds) {
    fprintf(out, "# Device %u: %s; filtered=%u dropped=%u\n", index, port->name, port->filtered,
            atomic_load(&port->dropped));

    for (unsigned c = 0; c < 16; ++c) {
        fprintf(out, "# ch %u: off=%u on=%u poly=%u CC=%u program=%u pressure=%u bend=%u\n", c + 1,
                port->counts[c][0], port->counts[c][1], port->counts[c][2], port->counts[c][3],
                port->counts[c][4], port->counts[c][5], port->counts[c][6]);

        for (unsigned key = 0; key < 128; ++key) {
            if (port->held[c][key]) {
                fprintf(out, "# still held: ch %u key %u\n", c + 1, key);
            }
        }
    }

    fprintf(out, "#");
    print_range(out, "notes", &port->keys);
    print_range(out, "velocity", &port->velocity);
    fprintf(out, " zero-velocity releases=%u\n", port->zero_off);

    for (unsigned cc = 0; cc < 128; ++cc) {
        Range *r = &port->cc[cc];

        if (r->count) {
            unsigned n = distinct(r);
            const char *hint = r->min == 0 && r->max == 127 && n == 2 ? "switch"
                               : relative(r)                          ? "relative?"
                                                                      : "absolute";
            fprintf(out, "# CC %u count=%u range=%u..%u distinct=%u peak/s=%u %s%s%s\n", cc,
                    r->count, r->min, r->max, n, r->peak, hint, r->paired ? " 14-bit pair?" : "",
                    r->rpn ? " RPN/NRPN" : "");
        }
    }

    if (port->bend.count) {
        fprintf(out, "# bend=%+d..%+d (%u..%u) peak/s=%u last-at-centre=%s\n",
                (int)port->bend.min - MIDI_BEND_CENTER, (int)port->bend.max - MIDI_BEND_CENTER,
                port->bend.min, port->bend.max, port->bend.peak,
                port->bend.last == MIDI_BEND_CENTER ? "yes" : "no");
    } else {
        fprintf(out, "# bend=none\n");
    }
    fprintf(out, "# pressure channel=%u poly=%u; 14-bit pair?=%u RPN/NRPN=%u\n", port->pressure[0],
            port->pressure[1], port->pairs, port->rpn);

    for (unsigned key = 0; key < 128; ++key) {
        if (port->poly[key]) {
            fprintf(out, "# poly key %u count=%u\n", key, port->poly[key]);
        }
    }

    double clock_span = (port->last_clock - port->first_clock) / 1e9;
    fprintf(out, "# clocks=%u implied BPM=%.2f active sensing=%u SysEx=%u duration=%.3fs\n",
            port->clocks, clock_span > 0 ? (port->clocks - 1) * 60.0 / (24 * clock_span) : 0,
            port->sensing, port->sysex, seconds);

    for (unsigned i = 0; i < port->manufacturer_count; ++i) {
        SysExSummary *s = &port->manufacturers[i];
        fprintf(out, "# SysEx manufacturer=%02X %02X %02X length=%zu..%zu count=%u\n",
                s->manufacturer[0], s->manufacturer[1], s->manufacturer[2], s->shortest, s->longest,
                s->count);
    }
}

static bool okay(RtMidiPtr device) {
    if (device && device->ok) {
        return true;
    }

    fprintf(stderr, "%s\n", device && device->msg ? device->msg : "Cannot create MIDI input");
    return false;
}

int main(int argc, char **argv) {
    int indices[DM_MIDI_INPUTS], selected = 0;
    const char *names[DM_MIDI_INPUTS], *virtual_name = NULL, *log_path = NULL;
    bool list = false, all = false, raw = false, self_test = false;
    double seconds = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--list")) {
            list = true;
        } else if (!strcmp(argv[i], "--all")) {
            all = true;
        } else if (!strcmp(argv[i], "--raw")) {
            raw = true;
        } else if (!strcmp(argv[i], "--self-test")) {
            self_test = true;
            seconds = .1;
        } else if ((!strcmp(argv[i], "--port") || !strcmp(argv[i], "--name")) && i + 1 < argc &&
                   selected < DM_MIDI_INPUTS) {
            bool number = !strcmp(argv[i], "--port");
            if (number) {
                char *end;
                long index = strtol(argv[++i], &end, 10);

                if (!*argv[i] || *end || index < 0 || index >= 128) {
                    fprintf(stderr, "Invalid port number\n");
                    return 2;
                }

                indices[selected] = (int)index;
                names[selected++] = NULL;
            } else {
                indices[selected] = -1;
                names[selected++] = argv[++i];
            }
        } else if (!strcmp(argv[i], "--virtual") && i + 1 < argc) {
            virtual_name = argv[++i];
        } else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            log_path = argv[++i];
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            char *end;
            seconds = strtod(argv[++i], &end);

            if (!*argv[i] || *end || !isfinite(seconds) || seconds <= 0) {
                fprintf(stderr, "Invalid duration\n");
                return 2;
            }
        } else {
            fprintf(stderr, "Usage: midi_monitor --list | [--port N | --name TEXT]... [--all] "
                            "[--raw] [--log FILE.csv] [--seconds S] [--virtual NAME]\n");
            return 2;
        }
    }

    signal(SIGINT, stop); /* Before opening ports, so Ctrl+C always reaches the summary. */
    RtMidiInPtr enumerator = NULL;
#ifdef _WIN32
    if (virtual_name) {
        fprintf(stderr, "WinMM cannot create virtual ports; use a system loopback input\n");
        return 2;
    }
#endif
    char available[DM_MIDI_PORTS][DM_MIDI_NAME];
    unsigned count = 0;
    int result = 1;
    FILE *log = NULL;
    Port *ports = calloc(DM_MIDI_INPUTS, sizeof(*ports));

    if (!ports) {
        return 1;
    }

    if (!self_test) {
        enumerator = rtmidi_in_create_default();

        if (!okay(enumerator)) {
            goto cleanup;
        }

        enum RtMidiApi api = rtmidi_in_get_current_api(enumerator);

        if (!okay(enumerator)) {
            goto cleanup;
        }

        printf("API: %s\n", rtmidi_api_display_name(api));
        count = rtmidi_get_port_count(enumerator);

        if (!okay(enumerator) || count > DM_MIDI_PORTS) {
            goto cleanup;
        }

        for (unsigned i = 0; i < count; ++i) {
            int length = sizeof(available[i]);
            int read = rtmidi_get_port_name(enumerator, i, available[i], &length);

            if (!okay(enumerator) || read < 0 || length > DM_MIDI_NAME) {
                goto cleanup;
            }
        }

        /* The names duckmidi saves for these devices. */
        dm_midi_port_names(available, (int)count,
                           api == RTMIDI_API_WINDOWS_MM   ? DM_MIDI_NAMES_INDEXED
                           : api == RTMIDI_API_LINUX_ALSA ? DM_MIDI_NAMES_ALSA
                                                          : DM_MIDI_NAMES_PLAIN);

        if (list) {
            for (unsigned i = 0; i < count; ++i) {
                printf("[%u] %s\n", i, available[i]);
            }

            printf("%u input ports\n", count);
            result = 0;
            goto cleanup;
        }
    }

    if (virtual_name || self_test) {
        if (selected) {
            fprintf(stderr, "Virtual/self-test mode uses one input\n");
            goto cleanup;
        }

        selected = 1;
        indices[0] = -1;
        names[0] = NULL;
    }

    if (!selected && count) {
        selected = 1;
        indices[0] = 0;
        names[0] = NULL;
    }

    if (!selected) {
        fprintf(stderr, "No input ports. Use --list.\n");
        goto cleanup;
    }

    if (log_path) {
        log = fopen(log_path, "wb");

        if (!log) {
            perror(log_path);
            goto cleanup;
        }

        fprintf(log, "time_ms,delta_ms,device,raw,channel,decoded,verdict,original_bytes\n");
    }

    for (int i = 0; i < selected; ++i) {
        Port *p = &ports[i];

        if (self_test) {
            snprintf(p->name, sizeof(p->name), "Scripted input");
            continue;
        }

        if (names[i]) {
            indices[i] = -1;

            for (unsigned n = 0; n < count; ++n) {
                if (SDL_strcasestr(available[n], names[i])) {
                    if (indices[i] >= 0) {
                        fprintf(stderr, "Ambiguous name: %s\n", names[i]);
                        goto cleanup;
                    }

                    indices[i] = n;
                }
            }
        }

        if (!virtual_name && (indices[i] < 0 || (unsigned)indices[i] >= count)) {
            fprintf(stderr, "Input port not found\n");
            goto cleanup;
        }

        snprintf(p->name, sizeof(p->name), "%s",
                 virtual_name ? virtual_name : available[indices[i]]);
        p->device = rtmidi_in_create_default();

        if (!okay(p->device)) {
            goto cleanup;
        }

        rtmidi_in_ignore_types(p->device, false, false, false);

        if (!okay(p->device)) {
            goto cleanup;
        }

        rtmidi_in_set_callback(p->device, capture, p);

        if (!okay(p->device)) {
            goto cleanup;
        }

        if (virtual_name) {
            rtmidi_open_virtual_port(p->device, virtual_name);
        } else {
            rtmidi_open_port(p->device, indices[i], "duckmidi monitor");
        }

        if (!okay(p->device)) {
            goto cleanup;
        }
    }

    uint64_t start = SDL_GetTicksNS(), last_line = start;
    unsigned filtered_line = 0;

    if (self_test) {
        const uint8_t messages[][3] = {{MIDI_NOTE_ON, 60, 97},   {MIDI_CONTROL, 21, 71},
                                       {MIDI_PITCH_BEND, 0, 64}, {MIDI_NOTE_ON, 60, 0},
                                       {MIDI_PROGRAM, 5},        {MIDI_CLOCK},
                                       {MIDI_ACTIVE_SENSING},    {MIDI_KEY_PRESSURE, 60, 40}};

        for (unsigned n = 0; n < sizeof(messages) / sizeof(*messages); ++n) {
            size_t length = messages[n][0] >= MIDI_SYSTEM                        ? 1
                            : (messages[n][0] & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2
                                                                                 : 3;
            capture(0, messages[n], length, ports);
        }

        const uint8_t sysex[] = {MIDI_SYSEX, 0, 0x20, 0x29, 1, MIDI_SYSEX_END};
        capture(0, sysex, sizeof(sysex), ports);
    }

    bool receiving = true;

    for (;;) {
        if (receiving && (stopping || (seconds && (SDL_GetTicksNS() - start) / 1e9 >= seconds))) {
            receiving = false;

            for (int i = 0; i < selected; ++i) {
                if (ports[i].device) {
                    rtmidi_in_free(ports[i].device);
                    ports[i].device = NULL;
                }
            }
        }

        bool pending = false;

        for (int i = 0; i < selected; ++i) {
            Port *p = &ports[i];
            unsigned r = atomic_load_explicit(&p->read, memory_order_relaxed);
            unsigned end = atomic_load_explicit(&p->write, memory_order_acquire);

            for (unsigned n = 0; r != end && n < 1024; ++r, ++n) {
                Capture item = p->ring[r % CAPTURE];
                DmMidiInputEvent event = {0};
                DmMidiVerdict verdict = dm_midi_normalize(item.bytes, item.size, &event);
                bool filtered = verdict == DM_MIDI_FILTERED || verdict == DM_MIDI_INVALID ||
                                verdict == DM_MIDI_SENSING;
                p->filtered += filtered;
                filtered_line += filtered;
                char bytes[RAW_BYTES * 3 + 4] = {0}, decoded[256];
                size_t stored = item.size < RAW_BYTES ? item.size : RAW_BYTES;

                for (size_t j = 0; j < stored; ++j) {
                    snprintf(bytes + j * 3, sizeof(bytes) - j * 3, "%02X ", item.bytes[j]);
                }

                if (verdict == DM_MIDI_INVALID) {
                    snprintf(decoded, sizeof(decoded), "Malformed message");
                } else {
                    describe(p, &item, decoded, sizeof(decoded));
                }

                double ms = (int64_t)(item.ns - start) / 1e6;
                double delta = p->previous ? (item.ns - p->previous) / 1e6 : 0;
                p->previous = item.ns;
                const char *verdict_text = verdict_name(verdict, event.status & MIDI_STATUS_MASK);
                if ((item.bytes[0] & MIDI_STATUS_MASK) == MIDI_PROGRAM) {
                    verdict_text = "filtered: program change (deferred)";
                } else if (item.bytes[0] == MIDI_CLOCK) {
                    verdict_text = "filtered: clock";
                } else if (item.bytes[0] == MIDI_ACTIVE_SENSING) {
                    verdict_text = "filtered: active sensing";
                }
                unsigned channel = item.bytes[0] < MIDI_SYSTEM ? (item.bytes[0] & 15) + 1 : 0;

                if (!filtered || all) {
                    printf("%10.3f +%8.3f [%d] %s  %s", ms, delta, i, p->name, bytes);

                    if (!raw) {
                        printf(" ch %u  %s  %s", channel, decoded, verdict_text);
                    }

                    putchar('\n');
                }

                if (log) {
                    fprintf(log, "%.3f,%.3f,%d,\"%s\",%u,\"%s\",\"%s\",%zu\n", ms, delta, i, bytes,
                            channel, decoded, verdict_text, item.size);
                }
            }

            atomic_store_explicit(&p->read, r, memory_order_release);
            pending |= r != atomic_load_explicit(&p->write, memory_order_acquire);
        }

        uint64_t now = SDL_GetTicksNS();

        if (now - last_line >= 1000000000) {
            if (filtered_line && !all) {
                printf("filtered in last second: %u\n", filtered_line);
            }

            filtered_line = 0;
            last_line = now;
        }

        SDL_Delay(1);

        if (!receiving && !pending) {
            break;
        }
    }

    for (int i = 0; i < selected; ++i) {
        if (ports[i].device) {
            rtmidi_in_free(ports[i].device);
            ports[i].device = NULL;
        }

        summary(&ports[i], i, stdout, (SDL_GetTicksNS() - start) / 1e9);

        if (log) {
            summary(&ports[i], i, log, (SDL_GetTicksNS() - start) / 1e9);
        }
    }

    result = self_test && (ports[0].zero_off != 1 || ports[0].cc[21].last != 71 ||
                           ports[0].bend.last != MIDI_BEND_CENTER || ports[0].sysex != 1 ||
                           ports[0].held[0][60] || ports[0].filtered != 4)
                 ? 1
                 : 0;
cleanup:
    for (int i = 0; i < DM_MIDI_INPUTS; ++i) {
        if (ports[i].device) {
            rtmidi_in_free(ports[i].device);
        }
    }

    if (enumerator) {
        rtmidi_in_free(enumerator);
    }

    if (log) {
        fclose(log);
    }

    free(ports);
    return result;
}
