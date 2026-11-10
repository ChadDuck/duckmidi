#include "midi_input/core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "MIDI input requires lock-free 64-bit atomics");

DmMidiVerdict dm_midi_normalize(const uint8_t *bytes, size_t size, DmMidiInputEvent *event) {
    if (!bytes || !size || bytes[0] < MIDI_STATUS) {
        return DM_MIDI_INVALID;
    }

    if (bytes[0] >= MIDI_SYSTEM) {
        return bytes[0] == MIDI_ACTIVE_SENSING && size == 1 ? DM_MIDI_SENSING : DM_MIDI_FILTERED;
    }

    int type = bytes[0] & MIDI_STATUS_MASK;
    size_t length = (bytes[0] & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3;

    if (size != length || bytes[1] > MIDI_DATA_MAX || (length == 3 && bytes[2] > MIDI_DATA_MAX)) {
        return DM_MIDI_INVALID;
    }

    event->status = bytes[0];
    event->a = bytes[1];
    event->b = length == 3 ? bytes[2] : 0;

    if (type == MIDI_NOTE_ON && !event->b) {
        event->status = MIDI_NOTE_OFF | (bytes[0] & MIDI_CHANNEL_MASK);
        event->b = 64;
    }

    if (type == MIDI_PROGRAM) {
        return DM_MIDI_FILTERED;
    }

    if (type == MIDI_CONTROL) {
        if (event->a == MIDI_CC_BANK || event->a == MIDI_CC_BANK_LSB) {
            return DM_MIDI_FILTERED;
        }

        if (event->a == MIDI_CC_SOUND_OFF || event->a == MIDI_CC_NOTES_OFF) {
            return DM_MIDI_RELEASE;
        }

        if (event->a == MIDI_CC_RESET) {
            return DM_MIDI_MONITOR;
        }

        if (event->a >= MIDI_CC_SOUND_OFF) {
            return DM_MIDI_FILTERED;
        }
    }

    return DM_MIDI_RECORD;
}

bool dm_midi_shape(DmMidiInputEvent *event, const DmMidiInputOptions *options) {
    unsigned type = event->status & MIDI_STATUS_MASK;

    if (type == MIDI_NOTE_ON) {
        if (options->fixed_velocity) {
            event->b = options->fixed_velocity;
        } else if (options->velocity != DM_MIDI_VELOCITY_MEDIUM) {
            double gamma = options->velocity == DM_MIDI_VELOCITY_LIGHT ? 0.6 : 1.6;
            long shaped = lround(MIDI_DATA_MAX * pow(event->b / (double)MIDI_DATA_MAX, gamma));
            event->b = shaped < 1 ? 1 : (uint8_t)shaped;
        }
    } else if (type == MIDI_CONTROL && event->a == MIDI_CC_SUSTAIN && options->sustain_invert) {
        event->b = MIDI_DATA_MAX - event->b;
    }

    return !options->channel || (event->status & MIDI_CHANNEL_MASK) + 1u == options->channel;
}

/* Length of name without a trailing " <digits>" (indexed) or " <digits>:<digits>" (ALSA). */
static size_t undecorated(const char *name, DmMidiNameStyle style) {
    size_t n = strlen(name), end = n;
    int groups = style == DM_MIDI_NAMES_ALSA ? 2 : style == DM_MIDI_NAMES_INDEXED ? 1 : 0;

    for (int group = 0; group < groups; ++group) {
        size_t digits = end;

        while (digits && name[digits - 1] >= '0' && name[digits - 1] <= '9') {
            --digits;
        }

        if (digits == end || !digits) {
            return n;
        }

        char separator = group + 1 < groups ? ':' : ' ';

        if (name[digits - 1] != separator) {
            return n;
        }

        end = digits - 1;
    }

    return end ? end : n;
}

/* Whether numbered is plain itself or plain followed by " #<digits>". */
static bool same_port(const char *numbered, const char *plain) {
    size_t length = strlen(plain);

    if (strncmp(numbered, plain, length)) {
        return false;
    }

    const char *rest = numbered + length;

    if (!*rest) {
        return true;
    }

    if (strncmp(rest, " #", 2) || !rest[2]) {
        return false;
    }

    for (rest += 2; *rest; ++rest) {
        if (*rest < '0' || *rest > '9') {
            return false;
        }
    }

    return true;
}

void dm_midi_port_names(char names[][DM_MIDI_NAME], int count, DmMidiNameStyle style) {
    for (int i = 0; i < count; ++i) {
        names[i][undecorated(names[i], style)] = 0;
    }

    for (int i = 0; i < count; ++i) {
        unsigned repeat = 1;

        for (int j = 0; j < i; ++j) {
            repeat += same_port(names[j], names[i]); /* Earlier names may already be numbered. */
        }

        if (repeat > 1) {
            size_t length = strlen(names[i]);
            size_t room = DM_MIDI_NAME - 1 - 12;
            snprintf(names[i] + (length < room ? length : room),
                     DM_MIDI_NAME - (length < room ? length : room), " #%u", repeat);
        }
    }
}

void dm_midi_ring_init(DmMidiRing *ring, DmMidiInputEvent *storage, unsigned capacity) {
    ring->events = storage;
    ring->capacity = capacity;
    atomic_init(&ring->read, 0);
    atomic_init(&ring->write, 0);
    atomic_init(&ring->dropped, 0);
}

bool dm_midi_ring_put(DmMidiRing *ring, DmMidiInputEvent event) {
    unsigned w = atomic_load_explicit(&ring->write, memory_order_relaxed);

    if (w - atomic_load_explicit(&ring->read, memory_order_acquire) == ring->capacity) {
        atomic_fetch_add_explicit(&ring->dropped, 1, memory_order_relaxed);
        return false;
    }

    ring->events[w & (ring->capacity - 1)] = event;
    atomic_store_explicit(&ring->write, w + 1, memory_order_release);
    return true;
}

bool dm_midi_ring_peek(DmMidiRing *ring, DmMidiInputEvent *event) {
    unsigned r = atomic_load_explicit(&ring->read, memory_order_relaxed);

    if (r == atomic_load_explicit(&ring->write, memory_order_acquire)) {
        return false;
    }

    *event = ring->events[r & (ring->capacity - 1)];
    return true;
}

bool dm_midi_ring_get(DmMidiRing *ring, DmMidiInputEvent *event) {
    if (!dm_midi_ring_peek(ring, event)) {
        return false;
    }

    atomic_fetch_add_explicit(&ring->read, 1, memory_order_release);
    return true;
}

void dm_midi_held_init(DmMidiHeld *held) {
    for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
        for (unsigned w = 0; w < 4; ++w) {
            atomic_init(&held->bits[c][w], 0);
        }
    }
}

void dm_midi_held_event(DmMidiHeld *held, DmMidiInputEvent event) {
    unsigned type = event.status & MIDI_STATUS_MASK;
    atomic_uint *word = &held->bits[event.status & MIDI_CHANNEL_MASK][event.a / 32];
    unsigned bit = 1u << (event.a % 32);

    if (type == MIDI_NOTE_ON && event.b) {
        atomic_fetch_or_explicit(word, bit, memory_order_relaxed);
    } else if (type == MIDI_NOTE_OFF || type == MIDI_NOTE_ON) {
        atomic_fetch_and_explicit(word, ~bit, memory_order_relaxed);
    }
}

bool dm_midi_held_note(const DmMidiHeld *held, DmMidiInputEvent event) {
    const atomic_uint *word = &held->bits[event.status & MIDI_CHANNEL_MASK][event.a / 32];
    return (atomic_load_explicit(word, memory_order_relaxed) & (1u << (event.a % 32))) != 0;
}

uint32_t dm_midi_held_release(DmMidiHeld *held, unsigned channel, unsigned word) {
    return atomic_exchange_explicit(&held->bits[channel][word], 0, memory_order_relaxed);
}

static uint64_t double_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double bits_double(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void dm_midi_clock_init(DmMidiClock *clock) {
    atomic_init(&clock->sequence, 0);
    atomic_init(&clock->transport, 0);
    atomic_init(&clock->playing, 0);
    atomic_init(&clock->loop, 0);
    atomic_init(&clock->ticks_ns, 0);
    atomic_init(&clock->position, 0);
    atomic_init(&clock->latency, 0);
}

void dm_midi_clock_write(DmMidiClock *clock, DmMidiClockValue value) {
    atomic_fetch_add(&clock->sequence, 1);
    atomic_store(&clock->ticks_ns, value.ticks_ns);
    atomic_store(&clock->position, double_bits(value.position));
    atomic_store(&clock->latency, double_bits(value.latency));
    atomic_store(&clock->transport, value.transport);
    atomic_store(&clock->playing, value.playing);
    atomic_store(&clock->loop, value.loop);
    atomic_fetch_add(&clock->sequence, 1);
}

bool dm_midi_clock_read(const DmMidiClock *clock, DmMidiClockValue *value) {
    /* A suspended writer must never stall a driver callback. */
    for (int attempt = 0; attempt < 8; ++attempt) {
        unsigned seq = atomic_load(&clock->sequence);

        if (seq & 1u) {
            continue;
        }

        DmMidiClockValue next = {atomic_load(&clock->ticks_ns),
                                 bits_double(atomic_load(&clock->position)),
                                 bits_double(atomic_load(&clock->latency)),
                                 atomic_load(&clock->transport),
                                 atomic_load(&clock->playing),
                                 atomic_load(&clock->loop)};

        if (seq == atomic_load(&clock->sequence)) {
            *value = next;
            return true;
        }
    }

    return false;
}

void dm_midi_clock_stamp(DmMidiClockValue value, DmMidiInputEvent *event) {
    event->transport = value.transport;
    event->loop = value.loop;
    event->song_seconds = value.position;

    if (value.playing) {
        event->flags |= DM_MIDI_EVENT_PLAYING;
        event->song_seconds +=
            (double)((int64_t)(event->ticks_ns - value.ticks_ns)) / 1e9 - value.latency;
    }
}

void dm_midi_echo_init(DmMidiEcho *echo) {
    echo->cursor = 0;

    for (unsigned i = 0; i < 256; ++i) {
        atomic_init(&echo->entries[i], 0);
    }
}

/* Microsecond timestamp modulo 2^40 and three bytes fit in a single atomic word. */
void dm_midi_echo_write(DmMidiEcho *echo, uint8_t status, uint8_t a, uint8_t b, uint64_t ns) {
    uint64_t entry = ((ns / 1000) << 24) | ((uint64_t)status << 16) | ((uint64_t)a << 8) | b;
    atomic_store_explicit(&echo->entries[echo->cursor++ % 256], entry, memory_order_relaxed);
}

bool dm_midi_echo_match(const DmMidiEcho *echo, uint8_t status, uint8_t a, uint8_t b, uint64_t ns) {
    uint32_t bytes = ((uint32_t)status << 16) | ((uint32_t)a << 8) | b;

    for (unsigned i = 0; i < 256; ++i) {
        uint64_t entry = atomic_load_explicit(&echo->entries[i], memory_order_relaxed);

        if ((entry & 0xffffff) == bytes &&
            (((ns / 1000) - (entry >> 24)) & UINT64_C(0xffffffffff)) < DM_MIDI_ECHO_MS * 1000) {
            return true;
        }
    }

    return false;
}
