/* Colored note ports in M64 export.

   M64 has one controller state per channel. A port that shares a channel with another moves to
   an unused channel when one is free, which keeps it exact. Drum ports stay on the drum channel,
   and ports beyond the free channels merge into their own channel. Merged ports interfere when
   they send different values for the same control, such as pitch bend or a controller; the
   export then keeps the latest value and the window warns about it. Other percussion channels
   (the context's percussion mask) merge into the drum channel, and a melodic channel 10 moves to
   a free channel. */
#include "m64_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KIND_BEND = 128, KIND_PRESSURE, KIND_PROGRAM, KIND_KEY_PRESSURE, KINDS };

static int kind_of(const DmeEvent *e) {
    unsigned type = e->status & MIDI_STATUS_MASK;

    if (type == MIDI_CONTROL) {
        return (int)(e->a & MIDI_DATA_MASK);
    }

    if (type == MIDI_PITCH_BEND) {
        return KIND_BEND;
    }

    if (type == MIDI_CHANNEL_PRESSURE) {
        return KIND_PRESSURE;
    }

    if (type == MIDI_PROGRAM) {
        return KIND_PROGRAM;
    }

    if (type == MIDI_KEY_PRESSURE) {
        return KIND_KEY_PRESSURE;
    }

    return -1;
}

static void kind_name(int kind, char *out, size_t capacity) {
    if (kind == KIND_BEND) {
        snprintf(out, capacity, "pitch bend");
    } else if (kind == KIND_PRESSURE) {
        snprintf(out, capacity, "channel pressure");
    } else if (kind == KIND_PROGRAM) {
        snprintf(out, capacity, "instrument");
    } else if (kind == KIND_KEY_PRESSURE) {
        snprintf(out, capacity, "key pressure");
    } else {
        snprintf(out, capacity, "controller %d", kind);
    }
}

static bool channel_message(const DmeEvent *e) {
    return e->status >= MIDI_STATUS && e->status < MIDI_SYSTEM && e->port < M64_PORTS;
}

static void add_notice(M64PortLayout *layout, const char *format, unsigned a, unsigned b,
                       unsigned c) {
    char *notice = layout->notice + strlen(layout->notice);
    size_t left = sizeof(layout->notice) - strlen(layout->notice);

    if (*layout->notice && left > 1) {
        *notice++ = ' ';
        left--;
    }

    snprintf(notice, left, format, a, b, c);
}

bool m64_port_layout(const DmeEvent *events, uint64_t count, uint32_t percussion,
                     M64PortLayout *layout) {
    memset(layout, 0, sizeof(*layout));
    bool present[MIDI_CHANNELS][M64_PORTS] = {{false}}, channel_used[MIDI_CHANNELS] = {false};

    for (uint64_t i = 0; i < count; ++i) {
        if (channel_message(&events[i])) {
            unsigned ch = events[i].status & MIDI_CHANNEL_MASK;
            present[ch][events[i].port] = true;
            channel_used[ch] = true;
        }
    }

    /* The drum channel plays only percussion: a melodic channel 10 needs a free channel. */
    channel_used[MIDI_DRUM_CHANNEL] = true;

    /* The lowest port keeps its channel; the others move to free channels in order. Other
       percussion parts play on the drum channel. */
    unsigned next_free = 0;
    int groups = 0, group_of[MIDI_CHANNELS][M64_PORTS];

    for (unsigned ch = 0; ch < MIDI_CHANNELS; ++ch) {
        bool first = true, drums = (percussion >> ch) & 1;

        for (unsigned port = 0; port < M64_PORTS; ++port) {
            group_of[ch][port] = -1;
            layout->out[ch][port] = (uint8_t)ch;

            if (!present[ch][port]) {
                continue;
            }

            /* Other percussion parts join the drum channel quietly; their controllers are
               not compared with channel 10's. */
            if (drums && ch != MIDI_DRUM_CHANNEL) {
                layout->out[ch][port] = MIDI_DRUM_CHANNEL;
                continue;
            }

            bool melodic_drum_channel = ch == MIDI_DRUM_CHANNEL && !drums;

            if (first && !melodic_drum_channel) {
                first = false;
                continue;
            }

            first = false;

            while (next_free < MIDI_CHANNELS && channel_used[next_free]) {
                ++next_free;
            }

            if ((ch != MIDI_DRUM_CHANNEL || melodic_drum_channel) && next_free < MIDI_CHANNELS) {
                layout->out[ch][port] = (uint8_t)next_free;
                channel_used[next_free] = true;

                if (layout->moved_count < 4) {
                    add_notice(layout, "Port %u of channel %u plays on channel %u.", port, ch + 1,
                               next_free + 1);
                }

                ++layout->moved_count;
                continue;
            }

            layout->merged = true;
        }
    }

    if (!layout->merged) {
        return true;
    }

    /* Merged ports: fingerprint each port's events per kind, then compare ports that share a
       channel. Identical streams do not interfere, and drum kit choices are ignored. */
    for (unsigned ch = 0; ch < MIDI_CHANNELS; ++ch) {
        for (unsigned port = 0; port < M64_PORTS; ++port) {
            unsigned out = layout->out[ch][port];

            if (present[ch][port] && out == ch) {
                group_of[ch][port] = groups++;
            }
        }
    }

    uint64_t *hash = calloc((size_t)groups * KINDS, sizeof(*hash));

    if (!hash) {
        return false;
    }

    for (uint64_t i = 0; i < count; ++i) {
        const DmeEvent *e = &events[i];
        int kind = channel_message(e) ? kind_of(e) : -1;
        int group = kind >= 0 ? group_of[e->status & MIDI_CHANNEL_MASK][e->port] : -1;

        if (group >= 0) {
            uint64_t *h = &hash[(size_t)group * KINDS + (size_t)kind];
            uint64_t value = e->tick * 1000003u + e->a * 131u + e->b + 1;
            *h = (*h ^ value) * 1099511628211ULL + 1;
        }
    }

    for (unsigned out = 0; out < MIDI_CHANNELS && layout->conflict_count < 3; ++out) {
        for (int kind = 0; kind < KINDS && layout->conflict_count < 3; ++kind) {
            if (out == MIDI_DRUM_CHANNEL && kind == KIND_PROGRAM) {
                continue;
            }

            int first = -1;
            unsigned first_ch = 0, first_port = 0;

            for (unsigned ch = 0; ch < MIDI_CHANNELS; ++ch) {
                for (unsigned port = 0; port < M64_PORTS; ++port) {
                    int group = group_of[ch][port];
                    uint64_t h = group >= 0 && layout->out[ch][port] == out
                                     ? hash[(size_t)group * KINDS + (size_t)kind]
                                     : 0;

                    if (!h) {
                        continue;
                    }

                    if (first < 0) {
                        first = group;
                        first_ch = ch;
                        first_port = port;
                        continue;
                    }

                    if (h != hash[(size_t)first * KINDS + (size_t)kind]) {
                        char name[40], where[96];
                        kind_name(kind, name, sizeof(name));

                        if (ch == first_ch) {
                            snprintf(where, sizeof(where), "Channel %u: ports %u and %u", ch + 1,
                                     first_port, port);
                        } else {
                            snprintf(where, sizeof(where), "Channels %u and %u", first_ch + 1,
                                     ch + 1);
                        }

                        char *notice = layout->notice + strlen(layout->notice);
                        snprintf(notice, sizeof(layout->notice) - strlen(layout->notice),
                                 "%s%s both change %s; they are merged and the latest value wins.",
                                 *layout->notice ? " " : "", where, name);
                        size_t merged = strlen(layout->merges);
                        snprintf(layout->merges + merged, sizeof(layout->merges) - merged,
                                 "%s: %s\n", where, name);
                        layout->interference = true;
                        ++layout->conflict_count;
                        ch = MIDI_CHANNELS;
                        break;
                    }
                }
            }
        }
    }

    free(hash);
    return true;
}
