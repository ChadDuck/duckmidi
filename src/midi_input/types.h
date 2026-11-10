#ifndef DM_MIDI_INPUT_TYPES_H
#define DM_MIDI_INPUT_TYPES_H
/* Plain MIDI input types shared by the device worker, the audio thread and the editor. The
   lock-free storage behind them stays in midi_input/core.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DM_MIDI_INPUTS 8
#define DM_MIDI_NAME 256
#define DM_MIDI_RING 16384 /* Events per ring; a power of two. */
#define DM_MIDI_RELEASE_RING 4096
#define DM_MIDI_MONITOR_DRAIN 1024 /* Events per audio block. */
#define DM_MIDI_RECORD_DRAIN 8192  /* Events per editor frame. */
#define DM_MIDI_COMMIT_MS 100
#define DM_MIDI_ECHO_MS 10
#define DM_MIDI_SENSING_MS 300

typedef enum {
    DM_MIDI_VELOCITY_MEDIUM,
    DM_MIDI_VELOCITY_LIGHT,
    DM_MIDI_VELOCITY_HEAVY
} DmMidiVelocityCurve;

/* Zero initialization selects the unmodified input behavior. */
typedef struct {
    DmMidiVelocityCurve velocity;
    uint8_t fixed_velocity; /* 0 follows the curve. */
    bool sustain_invert;
    uint8_t channel; /* 0 = all channels, 1–16 = source channel. */
} DmMidiInputOptions;

typedef enum { DM_MIDI_SOURCE_CC, DM_MIDI_SOURCE_NOTE } DmMidiInputSource;

typedef struct {
    char device[DM_MIDI_NAME]; /* Empty matches any device. */
    DmMidiInputSource kind;
    uint8_t channel, number; /* Zero-based source channel. */
} DmMidiBindingSource;

typedef enum { DM_MIDI_LEARN_OFF, DM_MIDI_LEARN_CC, DM_MIDI_LEARN_BUTTON } DmMidiLearnMode;

enum {
    DM_MIDI_EVENT_PLAYING = 1u,        /* The transport was playing when the event arrived. */
    DM_MIDI_EVENT_RELEASE = 2u,        /* Synthesized: all notes off, sensing loss or unplugging. */
    DM_MIDI_EVENT_DEVICE_RELEASE = 4u, /* Release every note the consumer holds for the device. */
    DM_MIDI_EVENT_BOUND = 8u,          /* Delivered only to the editor. */
    DM_MIDI_EVENT_LEARN = 16u          /* Candidate reserved for a MIDI learn session. */
};

typedef struct {
    uint64_t ticks_ns;   /* SDL_GetTicksNS() at receipt. */
    double song_seconds; /* Heard song position at receipt. */
    uint32_t transport;  /* Transport generation at receipt. */
    uint32_t loop;       /* Loop pass at receipt. */
    uint8_t device, status, a, b, flags;
} DmMidiInputEvent;

#endif
