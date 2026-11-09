#ifndef DM_MIDI_INPUT_CORE_H
#define DM_MIDI_INPUT_CORE_H
/* Pure MIDI input logic: normalization, lock-free rings, held notes, the song clock and the
   echo guard. C only; C++ code reaches MIDI input through midi_input.h. */
#include "../include/duckmidi_midi.h"
#include "midi_input_types.h"
#include <stdatomic.h>

typedef enum {
    DM_MIDI_FILTERED,
    DM_MIDI_INVALID,
    DM_MIDI_RECORD,
    DM_MIDI_MONITOR,
    DM_MIDI_RELEASE,
    DM_MIDI_SENSING
} DmMidiVerdict;

/* One producer and one consumer. Storage and capacity are fixed before publication. */
typedef struct {
    DmMidiInputEvent *events;
    unsigned capacity;
    atomic_uint read, write;
    atomic_ullong dropped;
} DmMidiRing;

typedef struct {
    atomic_uint bits[MIDI_CHANNELS][4];
} DmMidiHeld;

typedef struct {
    uint64_t ticks_ns;
    double position, latency;
    uint32_t transport;
    bool playing;
    uint32_t loop;
} DmMidiClockValue;

/* Seqlock with atomic payloads, valid under the C memory model. One serialized writer. */
typedef struct DmMidiClock {
    atomic_uint sequence, transport, playing, loop;
    atomic_ullong ticks_ns, position, latency;
} DmMidiClock;

typedef struct DmMidiEcho {
    atomic_ullong entries[256];
    unsigned cursor; /* Output worker only. */
} DmMidiEcho;

/* How a backend decorates port names, so saved names survive reordering and replugging. */
typedef enum {
    DM_MIDI_NAMES_PLAIN,
    DM_MIDI_NAMES_INDEXED, /* WinMM: " <index>" appended by RtMidi. */
    DM_MIDI_NAMES_ALSA     /* ALSA: " <client>:<port>" appended by RtMidi. */
} DmMidiNameStyle;

DmMidiVerdict dm_midi_normalize(const uint8_t *bytes, size_t size, DmMidiInputEvent *event);
/* Removes the backend's position suffix, then numbers repeated names " #2", " #3" in order. */
void dm_midi_port_names(char names[][DM_MIDI_NAME], int count, DmMidiNameStyle style);
void dm_midi_ring_init(DmMidiRing *ring, DmMidiInputEvent *storage, unsigned capacity);
bool dm_midi_ring_put(DmMidiRing *ring, DmMidiInputEvent event);
bool dm_midi_ring_peek(DmMidiRing *ring, DmMidiInputEvent *event);
bool dm_midi_ring_get(DmMidiRing *ring, DmMidiInputEvent *event);
void dm_midi_held_init(DmMidiHeld *held);
void dm_midi_held_event(DmMidiHeld *held, DmMidiInputEvent event);
uint32_t dm_midi_held_release(DmMidiHeld *held, unsigned channel, unsigned word);
void dm_midi_clock_init(DmMidiClock *clock);
void dm_midi_clock_write(DmMidiClock *clock, DmMidiClockValue value);
/* Fails only while the writer is mid-update; callers keep their last good value. */
bool dm_midi_clock_read(const DmMidiClock *clock, DmMidiClockValue *value);
void dm_midi_clock_stamp(DmMidiClockValue value, DmMidiInputEvent *event);
void dm_midi_echo_init(DmMidiEcho *echo);
void dm_midi_echo_write(DmMidiEcho *echo, uint8_t status, uint8_t a, uint8_t b, uint64_t ns);
bool dm_midi_echo_match(const DmMidiEcho *echo, uint8_t status, uint8_t a, uint8_t b, uint64_t ns);

#endif
