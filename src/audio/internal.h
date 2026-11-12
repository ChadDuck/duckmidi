#ifndef DM_AUDIO_INTERNAL_H
#define DM_AUDIO_INTERNAL_H
/* Shared by the audio engine's files (audio.c, realtime.c, live.c, publish.c, audition.c,
   jobs.c): the engine state and the helpers that cross them. The public API is audio/audio.h. */
#include "audio/audio.h"
#include "audio/samples.h"
#include "midi_input/midi_input.h"
#include "midi_input/core.h"
#include "rtmidi_c.h"
#include "text/en.h"
#include <SDL3/SDL.h>
#include <fluidsynth.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000
#define BLOCK 64
#define QUEUE 8192
#define LIVE_SAMPLE_BANKS 4

/* Frames of sample gain rewritten per audio lock, so a long sample never delays a buffer. */
#define GAIN_SLICE_FRAMES 65536
#define SAMPLE_ONE_SHOT UINT32_MAX

typedef struct {
    uint8_t status, a, b;
    uint8_t port;
    bool live_only;
    uint16_t input_part; /* One-based destination of a translated MIDI event. */
    uint8_t input_device;
    uint16_t feedback_part; /* One-based project part for isolated edit feedback. */
} Command;

typedef struct {
    Command items[QUEUE];
    const DmScheduled *system[QUEUE];
    SDL_AtomicInt read, write, panic;
} CommandQueue;

typedef struct {
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    int tail;
    unsigned sample_banks;
    DmSamplePedals sample_pedals;
} Synth;

typedef struct {
    Synth voice;
    DmSampleSynth *bank;
    unsigned preset;
    int key, release_in;
    int keyboard_route;
    uint32_t keys[128], song_keys[128];
    bool song_held[128];
    int song_part;
    bool repeating, active, keyboard, sustained, looping;
} SampleAudition;

typedef struct {
    uint8_t cc[DM_PARTS][128], program[DM_PARTS], bend[DM_PARTS][2], notes[DM_PARTS][128];
    uint8_t pressure[DM_PARTS], poly[DM_PARTS][128];
    uint8_t held[DM_PARTS][128], sostenuto[DM_PARTS][128];
    uint16_t sample[DM_PARTS][128];
} ChaseState;

typedef struct {
    double seconds;
    uint16_t channel;
    uint8_t key;
    uint64_t token;
} NoteRelease;

/* A sounding MIDI input key, kept so its Note Off reaches the same voice after a route or
   percussion map change. */
typedef struct {
    uint8_t route; /* Destination channel + 1; 0 when the incoming key is not sounding. */
    uint8_t key;   /* Sounding key chosen at Note On. */
} InputHeld;

typedef struct RetiredSchedule {
    DmSchedule schedule;
    struct RetiredSchedule *next;
} RetiredSchedule;

struct DmAudio {
    _Atomic(DmMidiInput *) input;
    atomic_uint input_route, input_policy;
    atomic_ullong input_played, input_dropped;
    double input_latency;
    uint32_t input_transport, input_loop;
    bool input_reset, input_latency_auto;
    uint64_t input_discard_ns;
    InputHeld input_held[DM_MIDI_INPUTS][MIDI_CHANNELS][MIDI_KEYS];
    uint8_t input_map[MIDI_KEYS]; /* Incoming key to sounding key; see dm_audio_input_note_map. */
    uint16_t input_notes[MIDI_CHANNELS][MIDI_KEYS];
    bool piano_held[MIDI_CHANNELS][MIDI_KEYS];
    uint16_t input_routes[DM_MIDI_INPUTS];
    uint32_t input_event_parts[DM_MIDI_INPUTS][DM_PARTS / 32];
    uint16_t input_event_thru[DM_MIDI_INPUTS];
    bool input_thru_held[MIDI_CHANNELS][MIDI_KEYS];
    uint16_t input_thru_sustain, input_thru_sostenuto;
    SDL_AudioStream *stream;
    Synth live, keyboard, feedback;
    SampleAudition sample_audition;
    Synth *port_live[DM_NOTE_PORTS], *prepared_ports[DM_NOTE_PORTS];
    bool used_ports[DM_NOTE_PORTS], job_ports[DM_NOTE_PORTS];
    DmChannel keyboard_channels[16], keyboard_applied[16];
    DmSamplePatchState keyboard_patch[16];
    uint8_t keyboard_programs[DM_PARTS];
    uint32_t keyboard_sample_id[16][128];
    int keyboard_override[16];
    bool keyboard_override_drums[16];
    bool keyboard_config, keyboard_panic;
    int keyboard_tail;
    DmSchedule schedule;
    NoteRelease *releases;
    size_t release_count, release_capacity;
    double release_end, seek_end[DM_PARTS][128], loop_note_end[DM_PARTS][128];
    uint64_t note_token[DM_PARTS][128], token_serial;
    uint32_t note_sample_id[DM_PARTS][128], sample_serial;
    DmLiveSampleBank *sample_banks[LIVE_SAMPLE_BANKS];
    int sample_bank;
    bool sample_bank_dirty;
    char sample_warning[256];
    int live_program[DM_PARTS];
    RetiredSchedule *retired;
    size_t cursor, beat_cursor;
    double position;
    bool playing, reset, metronome, monitor, recording;
    double click_bpm;
    int click_frames;
    double click_phase;
    double freewheel_click, freewheel_rate;
    SDL_AtomicInt position_ms, playing_flag, peaks[DM_PARTS], quit, job_done, job_busy, midi_failed,
        port_warning;
    bool port_warning_sent;
    SDL_AtomicInt analyzer_enabled;
    float analyzer_samples[DM_SPECTRUM_FRAMES * 2];
    int analyzer_cursor, analyzer_count;
    CommandQueue preview, midi, feedback_queue;
    int feedback_key, feedback_program, feedback_fraction, feedback_tail;
    uint16_t feedback_part;
    uint32_t feedback_sample_id;
    SDL_Thread *worker, *midi_thread;
    RtMidiOutPtr device;
    SDL_Mutex *device_lock;
    int device_index;
    int audition_frames, audition_channel, audition_key;
    char job_path[DM_PATH], font_path[DM_PATH], job_message[512];
    bool job_success;
    int keyboard_parts[16];
    int job_type;
    DmAudioOptions render_options;
    Synth prepared, prepared_keyboard, prepared_feedback;
    DmSchedule render_schedule;
    DmBytes render_project;
    unsigned render_missing_samples;
    SDL_AtomicInt progress, cancel;
    ChaseState state, loop_state;
    /* Volume and pan each part plays with now, shown by the mixer (255: not set). */
    uint8_t mix_volume[DM_PARTS], mix_pan[DM_PARTS];
    double loop_start, loop_end;
    bool loop_enabled, loop_wait;
    size_t loop_cursor, loop_beat_cursor;
    uint16_t preview_mask;
    bool chase;
};

void dm_audio_input_clock(DmAudio *a, bool changed);
void dm_audio_synth_free(Synth *s);
bool dm_audio_synth_create(Synth *s, const char *font);
bool dm_audio_queue_put(CommandQueue *q, Command c);
bool dm_audio_queue_get(CommandQueue *q, Command *c, const DmScheduled **system);
void dm_audio_send_synth(fluid_synth_t *s, uint8_t status, uint8_t a, uint8_t b);
void dm_audio_send_route(Synth *s, uint8_t status, uint8_t a, uint8_t b);
void dm_audio_reset_live_ports(DmAudio *a);
void dm_audio_copy_part_rows(void *destination, const void *source, size_t row, const bool *used);
void dm_audio_copy_chase(ChaseState *dst, const ChaseState *src, const bool *used);
void dm_audio_send_live(DmAudio *a, int part, uint8_t status, uint8_t first, uint8_t second);
void dm_audio_send_event(Synth *s, const DmScheduled *e);
uint32_t dm_audio_sample_start(DmAudio *a, Synth *s, uint32_t preset, int channel, int key,
                               int velocity);
void dm_audio_sample_stop(Synth *s, uint32_t id, int channel, int key);
void dm_audio_chase(DmAudio *a);
void dm_audio_render(Synth *s, float *interleaved, int frames, SDL_AtomicInt *peaks);
void dm_audio_render_port_mix(Synth *s, float *out, int frames, SDL_AtomicInt *peaks);
void dm_audio_mix_from_state(DmAudio *a);
void dm_audio_mix_track(DmAudio *a, int part, int status, int controller, int value);
void dm_audio_loop_end(DmAudio *a);
void dm_audio_input_thru_stop(DmAudio *a);
void dm_audio_input_event_stop(DmAudio *a);
void dm_audio_input_silence(DmAudio *a, DmMidiInput *input);
void dm_audio_input_commands(DmAudio *a);
void dm_audio_preview_commands(DmAudio *a);
void dm_audio_keyboard_configure(DmAudio *a);
void dm_audio_keyboard_reset_controllers(DmAudio *a);
void dm_audio_keyboard_silence(DmAudio *a);
void dm_audio_keyboard_render(DmAudio *a, float *out, int frames);
void dm_audio_feedback_stop(DmAudio *a);
void dm_audio_feedback_commands(DmAudio *a);
void dm_audio_feedback_render(DmAudio *a, float *out, int frames);
void dm_audio_sample_audition_start(SampleAudition *preview);
void SDLCALL dm_audio_callback(void *userdata, SDL_AudioStream *stream, int needed, int total);
int dm_audio_midi_worker(void *userdata);
void dm_audio_free_retired(DmAudio *a);
void dm_audio_free_port_synths(Synth **ports);
void dm_audio_collect_sample_banks(DmAudio *a);

#endif
