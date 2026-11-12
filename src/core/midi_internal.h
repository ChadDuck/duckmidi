#ifndef DM_CORE_MIDI_INTERNAL_H
#define DM_CORE_MIDI_INTERNAL_H
/* Shared by core/import.c, core/timeline.c, core/midi.c and core/schedule.c: the parsed-event
   list and its byte helpers. Not part of core/duck.h. */
#include "core/duck.h"
#include "text/en.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *p, *end;
    bool ok;
} Reader;

typedef struct {
    uint64_t tick;
    int track, port;
    size_t order;
    int phase;       /* Endpoints, restoration, setup, then notes at a shared tick. */
    bool persistent; /* Detached source points retain explicit restoration values. */
    bool generated_default;
    DmBytes bytes;
} Raw;

typedef struct {
    Raw *items;
    size_t count, capacity;
    int format, tracks, division;
    uint64_t end_tick;
    bool scan_pan;
    int scan_port, scan_sequence;
    uint64_t first_note[16], first_pan[16];
} Midi;

/* Non-commercial/development ID followed by an application namespace. See
   agents/MIDI-METADATA.md for the current, deliberately single-version format. */
static const uint8_t editor_tag[] = {0x7d, 'D', 'U', 'C', 'K', 'M', 'I', 'D', 'I'};

uint32_t dm_midi_read_vlq(Reader *r);
bool dm_midi_byte(DmBytes *b, unsigned v);
bool dm_midi_be(DmBytes *b, uint32_t v, int count);
bool dm_midi_vlq(DmBytes *b, uint32_t v);
void dm_midi_free(Midi *m);
bool dm_midi_raw_add(Midi *m, Raw *raw);
bool dm_midi_parse(Midi *m, const uint8_t *data, size_t size);
uint64_t dm_midi_fingerprint(const Midi *m, bool strip_foreign);
bool dm_midi_metadata_encode(const Midi *m, DmBytes *out, bool strip, const DmBytes *replacement);
bool dm_midi_remove_embedded_source(DmBytes *source, bool strip_foreign);
void dm_midi_remove_embedded_events(DmProject *p, bool strip_foreign);
double dm_midi_tick_beat(const Midi *m, uint64_t tick);
bool dm_midi_add_message(Midi *m, double beat, int track, const uint8_t *data, size_t size);
bool dm_midi_collect_timing(const DmProject *p, Midi *m);
bool dm_midi_collect(const DmProject *p, Midi *m, bool mixer);
int dm_midi_export_compare(const void *a, const void *b);
void dm_midi_sort(Midi *m);

#endif
