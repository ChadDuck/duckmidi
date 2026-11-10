#ifndef DM_EDITOR_MIDI_BINDINGS_H
#define DM_EDITOR_MIDI_BINDINGS_H

#include "midi_input/midi_input.h"

typedef enum {
    ED_MIDI_VOLUME,
    ED_MIDI_PAN,
    ED_MIDI_MUTE,
    ED_MIDI_SOLO,
    ED_MIDI_PLAY,
    ED_MIDI_STOP,
    ED_MIDI_RECORD,
    ED_MIDI_METRONOME,
    ED_MIDI_QUANTIZE,
    ED_MIDI_UNDO,
    ED_MIDI_REDO,
    ED_MIDI_CHANNEL_PREVIOUS,
    ED_MIDI_CHANNEL_NEXT,
    ED_MIDI_TARGETS
} EdMidiTargetKind;

typedef struct {
    EdMidiTargetKind kind;
    uint8_t channel, port;
} EdMidiTarget;

typedef struct {
    uint8_t previous, expected;
    bool seen, picked;
} EdMidiPickup;

typedef struct {
    DmMidiBindingSource source;
    EdMidiTarget target;
    bool toggle;
    EdMidiPickup input[DM_MIDI_INPUTS];
} EdMidiBinding;

typedef struct {
    EdMidiTarget target;
    uint64_t started;
    bool any_device;
} EdMidiLearn;

#endif
