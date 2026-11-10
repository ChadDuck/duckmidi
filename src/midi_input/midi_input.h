#ifndef DM_MIDI_INPUT_H
#define DM_MIDI_INPUT_H
/* MIDI input devices: an asynchronous worker owns every driver call. Safe to include from C++;
   the lock-free internals stay in midi_input_core.h. */
#include "midi_input_types.h"

#define DM_MIDI_PORTS 128

typedef struct DmMidiInput DmMidiInput;
typedef struct DmMidiClock DmMidiClock;
typedef struct DmMidiEcho DmMidiEcho;
typedef void (*DmMidiReceive)(void *context, const uint8_t *bytes, size_t size, uint64_t ns);

/* Called only on the input worker. Callbacks for one handle must be serialized; separate
   handles may deliver concurrently. close must finish all callbacks before returning.
   list returns the number copied, or -1 with an error. Names must identify ports uniquely and
   stay the same while a port exists. */
typedef struct {
    void *context;
    int (*list)(void *context, char names[][DM_MIDI_NAME], int capacity, char *error, size_t cap);
    void *(*open)(void *context, const char *name, DmMidiReceive receive, void *receiver,
                  char *error, size_t cap);
    void (*close)(void *context, void *handle);
    void (*destroy)(void *context);
} DmMidiInputBackend;

typedef enum {
    DM_MIDI_CLOSED,
    DM_MIDI_OPENING,
    DM_MIDI_OPEN,
    DM_MIDI_MISSING,
    DM_MIDI_FAILED
} DmMidiInputState;

typedef struct {
    char name[DM_MIDI_NAME], message[256];
    DmMidiInputState state;
    bool requested;
} DmMidiInputStatus;

typedef struct {
    uint64_t received, filtered, dropped, echoes, timing_fallbacks;
} DmMidiInputStats;

#ifdef __cplusplus
extern "C" {
#endif
/* The backend is transferred on success; NULL selects RtMidi. No enumeration at creation. */
DmMidiInput *dm_midi_input_create(const DmMidiInputBackend *backend);
/* Detach audio first. Destruction waits for driver open/close and callback completion. */
void dm_midi_input_destroy(DmMidiInput *input);
void dm_midi_input_refresh(DmMidiInput *input);
int dm_midi_input_ports(DmMidiInput *input, char names[][DM_MIDI_NAME], int capacity);
bool dm_midi_input_open(DmMidiInput *input, const char *name);
void dm_midi_input_close(DmMidiInput *input, const char *name);
void dm_midi_input_status(DmMidiInput *input, DmMidiInputStatus status[DM_MIDI_INPUTS]);
void dm_midi_input_stats(DmMidiInput *input, DmMidiInputStats *stats);
void dm_midi_input_set_thru(DmMidiInput *input, bool enabled);
bool dm_midi_input_thru(DmMidiInput *input);
DmMidiClock *dm_midi_input_clock(DmMidiInput *input);
DmMidiEcho *dm_midi_input_echo(DmMidiInput *input);
bool dm_midi_input_is_open(DmMidiInput *input);

/* Consumer 0: audio, 1: editor. Each consumer calls begin, bounded pop, end.
   begin is one atomic load when unused; end rearms pending work, including releases. */
bool dm_midi_input_begin(DmMidiInput *input, unsigned consumer);
bool dm_midi_input_pop(DmMidiInput *input, unsigned consumer, DmMidiInputEvent *event);
void dm_midi_input_end(DmMidiInput *input, unsigned consumer);
bool dm_midi_input_overflow(DmMidiInput *input, unsigned consumer);

#ifdef __cplusplus
}
#endif

#endif
