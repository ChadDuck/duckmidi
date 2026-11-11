#ifndef DUCK_H
#define DUCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../include/duckmidi_midi.h"
#include "../../shared/sample/ids.h"

#define DM_PORT_COMBINED 256 /* Virtual source view; exported channel messages use port 0. */
#define DM_LANES 32
#define DM_PPQ 960
#define DM_PIANO_KEYS 37
#define DM_PROJECT_SCHEMA 18
#define DM_PROJECT_SCHEMA_PREVIOUS 17 /* Still loaded through src/core/project_compat.c. */
#define DM_PROJECT_SCHEMA_OLDEST 9    /* Schema 9 (0.10.1) also loads, converted through 10. */
#define DM_NOTE_PORTS 128
#define DM_PATTERN_COLORS 16
#define DM_DRUM_CHANNEL 9 /* General MIDI channel 10. */
/* Loop points for exporters, stored as marker meta events so they survive MIDI files. */
#define DM_LOOP_START_MARKER "loopStart"
#define DM_LOOP_END_MARKER "loopEnd"
#define DM_PARTS (DM_NOTE_PORTS * 16)
#define DM_PATH 4096
#define DM_PATCH_REPLACEMENTS_MAX 4096

/* This build's version: the one set for the build, or its UTC build time (src/version.c). */
extern const char dm_app_version[];
#define DM_BPM_MIN (60000000.0 / 16777215.0)
#define DM_BPM_MAX 60000000.0

typedef struct {
    void *file;
    char path[DM_PATH], temporary[DM_PATH + 96];
    bool failed;
} DmOutput;

typedef struct {
    unsigned char *data;
    size_t size, capacity;
} DmBytes;

typedef struct {
    double beat, length;
    uint8_t key, velocity, selected, port;
    uint32_t bend_id; /* Pattern-local identity assigned when a bend is attached. */
} DmNote;

enum { DM_GRAPH_POINT = 1, DM_GRAPH_PITCH_LANE = 128, DM_GRAPH_LANES };

typedef struct {
    char name[64];
    char base_name[64];
    uint32_t duplicate_index;
    uint32_t uid;
    double length;
    DmNote *notes;
    size_t count, capacity;
    int kind, mode, event; /* kind: 0 notes, 1 automation; mode: 0 point, 1 LFO. */
    double speed, tension, skew, pulse, amount;
    double center, pitch_center;
    int reference;
    int control_scope; /* 0 ordinary; 1 detached note controls; 2 unscoped source controls. */
    int port;
    bool inverted;
    uint8_t color; /* 0 uses the application color; 1..DM_PATTERN_COLORS select the palette. */
    /* Indexed by controller or DM_GRAPH_PITCH_LANE. 0 is the default Point view,
       DM_GRAPH_POINT is explicitly Point, and 8..256 is a Graph note denominator. */
    uint16_t graph_lanes[DM_GRAPH_LANES];
} DmPattern;

typedef struct {
    double beat;
    int pattern, channel;
    uint8_t selected;
    double length; /* 0 uses the pattern duration; automation placements may override it. */
    int lane;      /* 0 note row, 1..DM_LANES automation subrow. */
    double offset; /* Pattern beat shown at the placement start; slices share one pattern. */
} DmClip;

enum { DM_BEND_ROOT_NONE, DM_BEND_ROOT_PITCH, DM_BEND_ROOT_MIDI };

typedef struct {
    double beat;
    int type, channel, a, b;
    int pattern;       /* -1: song event; otherwise local to a reusable pattern. */
    int curve, anchor; /* 0: discrete source; positive: line nodes; negative: restoration. */
    double bend;       /* Midpoint offset in MIDI value units for the outgoing segment. */
    bool ramp;         /* Discrete-source outgoing segment was explicitly converted to a curve. */
    uint8_t port;
    bool hold; /* Line node: the outgoing segment holds this value until the next point. */
    bool fixed_anchor; /* A discrete pitch point keeps the pitch reference it was drawn against. */
    uint32_t bend_note; /* Only the root point of a note-attached curve names its note. */
    uint8_t bend_root; /* Authoring units of a pitch bend's root or standalone point. */
} DmAutomation;

typedef struct {
    int range, cents, msb, lsb;
} DmPitchState;

DmPitchState dm_pitch_state(const DmAutomation *items, size_t count, int pattern, double beat,
                            unsigned port);
double dm_pitch_range(const DmAutomation *items, size_t count, DmAutomation point);

/* Graph-capable event types and their stored lanes. Pitch bend uses event type -2. */
int dm_graph_lane(int event);
int dm_automation_graph_lane(DmAutomation point);
int dm_graph_resolution(const DmPattern *pattern, int event);
bool dm_pattern_has_graph(const DmPattern *pattern);

typedef struct {
    bool enabled, mute, solo;
    int program, volume, pan;
    /* A percussion part: programs select drum kits and keys select drums. New projects use
       channel 10; imported files may mark others (GS rhythm parts, XG/GM2 drum banks). */
    bool drums;
} DmChannel;

typedef struct {
    int port, channel, volume, pan;
    bool mute, solo;
    int program; /* Program plus one for this port only; 0 follows the channel. */
} DmPortMix;

typedef struct {
    uint16_t bank;
    uint8_t port, channel, program;
    bool drums;
} DmPatchKey;

typedef struct {
    DmPatchKey key;       /* Port 0 is the channel default. */
    uint32_t generation;  /* Runtime sample cache token; zero explicitly selects SoundFont. */
    char locator[DM_PATH], set_id[DM_SAMPLE_ID_CAP], preset_id[DM_SAMPLE_ID_CAP];
} DmPatchReplacement;

typedef struct {
    double beat;
    int track, port;
    DmBytes message;
} DmEvent;

typedef struct {
    double position;
    int track, index;
    uint8_t program_plus_one;
} DmProgramHistory;

typedef struct {
    int port, sequence;
    DmBytes state;
} DmView;

typedef struct {
    char name[128], soundfont[DM_PATH];
    double bpm;
    int numerator, denominator, port, sequence, source_format;
    DmChannel channels[16];
    DmPortMix *port_mix;
    size_t port_mix_count, port_mix_capacity;
    DmPatchReplacement *patch_replacements;
    size_t patch_replacement_count, patch_replacement_capacity;
    bool percussion_override;
    uint8_t percussion_keys[128]; /* 0 unassigned; otherwise PC piano-key index + 1. */
    uint8_t percussion_midi[128]; /* 0 unassigned; otherwise incoming MIDI note + 1. */
    int lane_count[16];
    char lane_names[16][DM_LANES][64];
    DmPattern *patterns;
    size_t pattern_count, pattern_capacity;
    uint32_t next_pattern_uid;
    DmClip *clips;
    size_t clip_count, clip_capacity;
    DmAutomation *automation;
    size_t automation_count, automation_capacity;
    DmEvent *events;
    size_t event_count, event_capacity;
    DmBytes original;
    int ports[256], port_count, sequence_count;
    DmView *views;
    size_t view_count, view_capacity;
} DmProject;

typedef struct {
    double seconds;
    uint8_t status, a, b;
    DmBytes sysex;
    double note_end; /* Prepared audio note lifetime; not serialized. */
    uint16_t note_sample;
    uint8_t note_program;
    uint8_t port;
} DmScheduled;

typedef struct {
    DmScheduled *items;
    size_t count, capacity;
    double duration;
    double *beats;
    size_t beat_count;
    double next_beat, final_rate;
} DmSchedule;

typedef struct {
    double bar, origin, completed, next_change;
} DmMeter;

bool dm_reserve(void **p, size_t *capacity, size_t needed, size_t width);
bool dm_bytes(DmBytes *b, const void *data, size_t size);
void dm_bytes_free(DmBytes *b);
void dm_project_init(DmProject *p);
void dm_project_free(DmProject *p);
/* Deep copy into an initialized destination; leave it unchanged on failure. */
bool dm_project_copy(DmProject *destination, const DmProject *source);
DmPortMix dm_port_mix(const DmProject *project, int port, int channel);
bool dm_port_mix_set(DmProject *project, DmPortMix mix);
bool dm_patch_replacement_valid(const DmPatchReplacement *replacement);
const DmPatchReplacement *dm_patch_replacement_exact(const DmProject *project, DmPatchKey key);
const DmPatchReplacement *dm_patch_replacement(const DmProject *project, DmPatchKey key);
bool dm_patch_replacement_set(DmProject *project, const DmPatchReplacement *replacement);
void dm_patch_replacement_clear(DmProject *project, DmPatchKey key);
/* Ports with notes, automation or mixer settings on the channel; port 0 is not implied. */
void dm_channel_ports(const DmProject *project, int channel, bool used[DM_NOTE_PORTS]);
int dm_pattern_add(DmProject *p, const char *name);
int dm_pattern_copy(DmProject *p, int source);
void dm_pattern_remove(DmProject *p, int pattern);
/* Moves pattern "from" in front of the pattern at "before" (pattern_count moves it to the end),
   updating placements and event points. Returns its new index. */
int dm_pattern_move(DmProject *p, int from, int before);
/* An index after dm_pattern_move moved the pattern at "from" to "to". */
int dm_pattern_moved_index(int index, int from, int to);
bool dm_note_add(DmPattern *p, DmNote note);
bool dm_clip_add(DmProject *p, DmClip clip);
double dm_clip_length(const DmProject *p, DmClip clip);
double dm_lfo(const DmPattern *pattern, double beat);
double dm_lfo_center_snap(double value, bool pitch, bool free);
double dm_lfo_speed_snap(double speed, double bar);
int dm_lfo_range(const DmPattern *pattern);
int dm_lfo_value(const DmPattern *pattern, double beat);
bool dm_automation_later(DmAutomation first, DmAutomation second);
bool dm_automation_add(DmProject *p, DmAutomation item);
double dm_curve_value(DmAutomation first, DmAutomation last, double t);
bool dm_automation_curves(DmAutomation a);
bool dm_event_add(DmProject *p, double beat, int track, int port, const DmBytes *message);
int dm_project_program_repeat(DmProgramHistory history[DM_PORT_COMBINED][16], const DmProject *p,
                              int index, bool automation);
bool dm_collapse_program_repeats(DmProject *p, int index, bool automation);
bool dm_message_valid(const DmBytes *message);
bool dm_conductor(const DmBytes *message);
double dm_duration(const DmProject *p);
double dm_bar(const DmProject *p);
void dm_patterns_fit(DmProject *project);
void dm_pattern_name(DmProject *project, int pattern, const char *name);
void dm_pattern_fit(const DmProject *p, DmPattern *pattern);
DmMeter dm_meter(const DmProject *p, double beat);
void dm_quantize(DmPattern *p, double grid);
bool dm_import(DmProject *p, const unsigned char *data, size_t size, int port, int sequence,
               char *error, size_t cap);
bool dm_midi(const DmProject *p, DmBytes *out, char *error, size_t cap);
bool dm_midi_all(const DmProject *p, DmBytes *out, char *error, size_t cap);
/* Imports every source port into one arrangement; source port k uses note port k. */
bool dm_import_ports(DmProject *p, const unsigned char *data, size_t size, const int *ports,
                     int count, int sequence, char *error, size_t cap);

/* Resolved active-arrangement events, owned by the caller (free with free()).
   0xff denotes tempo (a = microseconds/quarter), 0xf0 unsupported SysEx.
   Other statuses and a/b are MIDI channel messages in stable playback order. */
typedef struct {
    uint64_t tick, source_id;
    uint32_t status, a, b, port;
    uint32_t flags;
} DmExportEvent;

/* A mixer pan inserted at its centre default. Channels in dm_export_authored_pan had that
   centre pan authored, so it is not a default there. */
enum { DM_EXPORT_DEFAULT_CONTROL = 1u };

bool dm_export_timeline(const DmProject *p, DmExportEvent **events, size_t *count,
                        uint64_t *duration, char *error, size_t cap);
/* Mask of MIDI channels whose opening pan was authored, including pan folded into the mixer
   on import. Reads the retained source MIDI, so call it once per export. */
unsigned dm_export_authored_pan(const DmProject *p);

typedef enum {
    DM_METADATA_KEEP_ALL,
    DM_METADATA_DUCKMIDI_ONLY,
    DM_METADATA_STRIP_ALL
} DmMetadataMode;

/* File export policy for sequencer-specific (FF 7F) editor information. */
bool dm_midi_export(const DmProject *p, DmBytes *out, bool all_views, DmMetadataMode metadata,
                    char *error, size_t cap);
bool dm_pack(const DmProject *p, DmBytes *out);
bool dm_unpack(DmProject *p, const unsigned char *data, size_t size, char *error, size_t cap);
/* LZ77 compression of project bodies; see src/compress.c. */
bool dm_compress(const uint8_t *data, size_t size, DmBytes *out);
bool dm_decompress(const uint8_t *data, size_t size, size_t limit, DmBytes *out);
/* Converts a project loaded from an older schema (DM_PROJECT_SCHEMA_OLDEST and later) to the
   current one; see src/core/project_compat.c. */
void dm_project_upgrade(DmProject *p, uint32_t schema);
/* An unused channel's settings; only channel 10 starts as a percussion part. */
DmChannel dm_channel_default(int channel);
bool dm_channel_drums(const DmProject *p, int channel);
bool dm_view_switch(DmProject *p, int port, int sequence, char *error, size_t cap);
bool dm_read(const char *path, DmBytes *out, char *error, size_t cap);
bool dm_write(const char *path, const void *data, size_t size, char *error, size_t cap);

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t size;
} DmArchiveEntry;

bool dm_zip(const DmArchiveEntry *entries, size_t count, DmBytes *out, char *error, size_t cap);
bool dm_output_begin(DmOutput *out, const char *path, char *error, size_t cap);
bool dm_output_write(DmOutput *out, const void *data, size_t size, char *error, size_t cap);
bool dm_output_finish(DmOutput *out, bool commit, char *error, size_t cap);
bool dm_schedule(const DmProject *p, DmSchedule *s);
void dm_schedule_free(DmSchedule *s);
double dm_beat_seconds(const DmProject *p, double beat);
double dm_seconds_beat(const DmProject *p, double seconds);
double dm_playback_bpm(const DmProject *p, double seconds);
const char *dm_drum_name(int key);
/* GS drum kit name for a percussion program, or NULL when the program is not a kit. */
const char *dm_drum_kit_name(int program);
bool dm_midi_rhythm_part(const DmBytes *message, int *channel, bool *drums);
/* 1 for a loop start marker, 2 for a loop end marker, 0 otherwise. */
int dm_loop_marker(const DmBytes *message);
/* Beats of the first loop start and end markers, -1 when absent. Returns whether either exists. */
bool dm_loop_points(const DmProject *p, double *start, double *end);
const char *dm_program_name(int program);
/* Sets a channel's starting instrument, including program changes sent before its first note.
   Without apply, only reports whether anything would change. */
bool dm_set_initial_program(DmProject *p, int channel, int program, bool apply);
#endif
