#ifndef DM_MIDI_INPUT_TYPES_H
#define DM_MIDI_INPUT_TYPES_H
/* Plain MIDI input types shared by the device worker, the audio thread and the editor. The
   lock-free storage behind them stays in midi_input_core.h. */
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

enum {
    DM_MIDI_EVENT_PLAYING = 1u,       /* The transport was playing when the event arrived. */
    DM_MIDI_EVENT_RELEASE = 2u,       /* Synthesized: all notes off, sensing loss or unplugging. */
    DM_MIDI_EVENT_DEVICE_RELEASE = 4u /* Release every note the consumer holds for the device. */
};

typedef struct {
    uint64_t ticks_ns;   /* SDL_GetTicksNS() at receipt. */
    double song_seconds; /* Heard song position at receipt. */
    uint32_t transport;  /* Transport generation at receipt. */
    uint32_t loop;       /* Loop pass at receipt. */
    uint8_t device, status, a, b, flags;
} DmMidiInputEvent;

#endif
