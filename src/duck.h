#ifndef DUCK_H
#define DUCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DM_PORT_COMBINED 256 /* Virtual source view; exported channel messages use port 0. */
#define DM_LANES 32
#define DM_PPQ 960
#define DM_PIANO_KEYS 37
#define DM_PROJECT_SCHEMA 9
#define DM_NOTE_PORTS 128
#define DM_PARTS (DM_NOTE_PORTS * 16)
#define DM_PATH 4096
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
} DmNote;
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
} DmPattern;
typedef struct {
    double beat;
    int pattern, channel;
    uint8_t selected;
    double length; /* 0 uses the pattern duration; automation placements may override it. */
    int lane;      /* 0 note row, 1..DM_LANES automation subrow. */
} DmClip;
typedef struct {
    double beat;
    int type, channel, a, b;
    int pattern;       /* -1: song event; otherwise local to a reusable pattern. */
    int curve, anchor; /* 0: discrete source; positive: line nodes; negative: restoration. */
    double bend;       /* Midpoint offset in MIDI value units for the outgoing segment. */
    bool ramp;         /* Discrete-source outgoing segment was explicitly converted to a curve. */
    uint8_t port;
} DmAutomation;
typedef struct {
    bool enabled, mute, solo;
    int program, volume, pan;
} DmChannel;
typedef struct {
    int port, channel, volume, pan;
    bool mute, solo;
} DmPortMix;
typedef struct {
    double beat;
    int track, port;
    DmBytes message;
} DmEvent;
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
    bool percussion_override;
    uint8_t percussion_keys[128]; /* 0 unassigned; otherwise PC piano-key index + 1. */
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
bool dm_project_copy(DmProject *destination,const DmProject *source);
DmPortMix dm_port_mix(const DmProject *project, int port, int channel);
bool dm_port_mix_set(DmProject *project, DmPortMix mix);
void dm_channel_ports(const DmProject *project, int channel, bool used[DM_NOTE_PORTS]);
int dm_pattern_add(DmProject *p, const char *name);
int dm_pattern_copy(DmProject *p, int source);
void dm_pattern_remove(DmProject *p, int pattern);
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
bool dm_event_add(DmProject *p, double beat, int track, int port, const DmBytes *message);
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

/* Resolved active-arrangement events, owned by the caller (free with free()).
   0xff denotes tempo (a = microseconds/quarter), 0xf0 unsupported SysEx.
   Other statuses and a/b are MIDI channel messages in stable playback order. */
typedef struct {
    uint64_t tick, source_id;
    uint32_t status, a, b, port;
} DmExportEvent;
bool dm_export_timeline(const DmProject *p, DmExportEvent **events, size_t *count,
                        uint64_t *duration, char *error, size_t cap);
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
bool dm_view_switch(DmProject *p, int port, int sequence, char *error, size_t cap);
bool dm_read(const char *path, DmBytes *out, char *error, size_t cap);
bool dm_write(const char *path, const void *data, size_t size, char *error, size_t cap);
typedef struct { const char *name; const unsigned char *data; size_t size; } DmArchiveEntry;
bool dm_zip(const DmArchiveEntry *entries, size_t count, DmBytes *out, char *error, size_t cap);
bool dm_output_begin(DmOutput *out, const char *path, char *error, size_t cap);
bool dm_output_write(DmOutput *out, const void *data, size_t size, char *error, size_t cap);
bool dm_output_finish(DmOutput *out, bool commit, char *error, size_t cap);
bool dm_schedule(const DmProject *p, DmSchedule *s);
void dm_schedule_free(DmSchedule *s);
double dm_beat_seconds(const DmProject *p, double beat);
double dm_seconds_beat(const DmProject *p, double seconds);
const char *dm_drum_name(int key);
const char *dm_program_name(int program);
#endif
