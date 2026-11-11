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

static void free_retired(DmAudio *a);
static void free_port_synths(Synth **ports);
static void keyboard_silence(DmAudio *a);
static void keyboard_reset_controllers(DmAudio *a);

static void input_clock(DmAudio *a, bool changed) {
    if (changed) {
        ++a->input_transport;
    }

    DmMidiInput *input = atomic_load_explicit(&a->input, memory_order_acquire);

    if (input) {
        dm_midi_clock_write(dm_midi_input_clock(input),
                            (DmMidiClockValue){SDL_GetTicksNS(), a->position, a->input_latency,
                                               a->input_transport, a->playing, a->input_loop});
    }
}

static void synth_free(Synth *s) {
    if (s->synth) {
        delete_fluid_synth(s->synth);
    }

    if (s->settings) {
        delete_fluid_settings(s->settings);
    }

    memset(s, 0, sizeof(*s));
}

static bool synth_create(Synth *s, const char *font) {
    s->settings = new_fluid_settings();

    if (!s->settings) {
        return false;
    }

    fluid_settings_setstr(s->settings, "synth.portamento-time", "auto");
    fluid_settings_setnum(s->settings, "synth.sample-rate", RATE);
    fluid_settings_setnum(s->settings, "synth.gain", 0.35);
    fluid_settings_setint(s->settings, "synth.audio-channels", 16);
    fluid_settings_setint(s->settings, "synth.audio-groups", 16);
    fluid_settings_setint(s->settings, "synth.effects-groups", 16);
    fluid_settings_setint(s->settings, "synth.threadsafe-api", 0);
    fluid_settings_setint(s->settings, "synth.polyphony", 512);
    s->synth = new_fluid_synth(s->settings);

    if (!s->synth || (font && *font && fluid_synth_sfload(s->synth, font, 1) < 0)) {
        synth_free(s);
        return false;
    }

    return true;
}

static bool queue_put(CommandQueue *q, Command c) {
    int w = SDL_GetAtomicInt(&q->write), next = (w + 1) % QUEUE;

    if (next == SDL_GetAtomicInt(&q->read)) {
        SDL_SetAtomicInt(&q->panic, 1);
        return false;
    }

    q->items[w] = c;
    q->system[w] = NULL;
    SDL_SetAtomicInt(&q->write, next);
    return true;
}

static void queue_system(CommandQueue *q, const DmScheduled *event) {
    int w = SDL_GetAtomicInt(&q->write), next = (w + 1) % QUEUE;

    if (next == SDL_GetAtomicInt(&q->read)) {
        SDL_SetAtomicInt(&q->panic, 1);
        return;
    }

    q->items[w] = (Command){event->status, 0, 0};
    q->system[w] = event;
    SDL_SetAtomicInt(&q->write, next);
}

static bool queue_get(CommandQueue *q, Command *c, const DmScheduled **system) {
    int r = SDL_GetAtomicInt(&q->read);

    if (r == SDL_GetAtomicInt(&q->write)) {
        return false;
    }

    *c = q->items[r];

    if (system) {
        *system = q->system[r];
    }

    SDL_SetAtomicInt(&q->read, (r + 1) % QUEUE);
    return true;
}

static void send_synth(fluid_synth_t *s, uint8_t status, uint8_t a, uint8_t b) {
    int c = status & MIDI_CHANNEL_MASK;

    switch (status & MIDI_STATUS_MASK) {
    case MIDI_NOTE_OFF:
        fluid_synth_noteoff(s, c, a);
        break;
    case MIDI_NOTE_ON:
        if (b) {
            fluid_synth_noteon(s, c, a, b);
        } else {
            fluid_synth_noteoff(s, c, a);
        }
        break;
    case MIDI_KEY_PRESSURE:
        fluid_synth_key_pressure(s, c, a, b);
        break;
    case MIDI_CONTROL:
        fluid_synth_cc(s, c, a, b);
        break;
    case MIDI_PROGRAM:
        fluid_synth_program_change(s, c, a);
        break;
    case MIDI_CHANNEL_PRESSURE:
        fluid_synth_channel_pressure(s, c, a);
        break;
    case MIDI_PITCH_BEND:
        fluid_synth_pitch_bend(s, c, a | (b << 7));
        break;
    }
}

static void send_route(Synth *s, uint8_t status, uint8_t a, uint8_t b) {
    if ((status & MIDI_STATUS_MASK) == MIDI_CONTROL) {
        dm_sample_pedal_cc(&s->sample_pedals, s->synth, status & MIDI_CHANNEL_MASK, a, b);
    } else {
        send_synth(s->synth, status, a, b);
    }
}

static Synth *live_port(DmAudio *a, unsigned port) {
    return port ? a->port_live[port] : &a->live;
}

static void reset_live_ports(DmAudio *a) {
    fluid_synth_system_reset(a->live.synth);
    memset(&a->live.sample_pedals, 0, sizeof(a->live.sample_pedals));

    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        if (a->port_live[p]) {
            fluid_synth_system_reset(a->port_live[p]->synth);
            memset(&a->port_live[p]->sample_pedals, 0, sizeof(a->port_live[p]->sample_pedals));
        }
    }
}

static void copy_part_rows(void *destination, const void *source, size_t row, const bool *used) {
    for (unsigned p = 0; p < DM_NOTE_PORTS; p++) {
        if (used[p]) {
            memcpy((uint8_t *)destination + p * 16 * row, (const uint8_t *)source + p * 16 * row,
                   16 * row);
        }
    }
}

static void copy_chase(ChaseState *dst, const ChaseState *src, const bool *used) {
#define PART_COPY(field) copy_part_rows(dst->field, src->field, sizeof(dst->field[0]), used)
    PART_COPY(cc);
    PART_COPY(program);
    PART_COPY(bend);
    PART_COPY(notes);
    PART_COPY(pressure);
    PART_COPY(poly);
    PART_COPY(held);
    PART_COPY(sostenuto);
    PART_COPY(sample);
#undef PART_COPY
}

static void instrument_preview_control(DmAudio *a, int part, uint8_t status, uint8_t first,
                                       uint8_t second) {
    SampleAudition *p = &a->sample_audition;
    int type = status & MIDI_STATUS_MASK;

    if (a->monitor && p->keyboard && p->song_part == part && status < MIDI_SYSTEM &&
        type != MIDI_PROGRAM && type != MIDI_NOTE_ON && type != MIDI_NOTE_OFF &&
        !(type == MIDI_CONTROL && (first == MIDI_CC_BANK || first == MIDI_CC_BANK_LSB))) {
        send_route(&p->voice, (uint8_t)(type | 1), first, second);
    }
}

static void send_live(DmAudio *a, int part, uint8_t status, uint8_t first, uint8_t second) {
    if ((status & MIDI_STATUS_MASK) == MIDI_PROGRAM) {
        a->live_program[part] = first;
    }

    if (a->monitor) {
        instrument_preview_control(a, part, status, first, second);

        Synth *s = live_port(a, (unsigned)part / 16);

        if (s) {
            send_route(s, status, first, second);
        }
    }
}

static void send_event(Synth *s, const DmScheduled *e) {
    if (e->status < MIDI_SYSTEM) {
        send_route(s, e->status, e->a, e->b);
        return;
    }

    if (e->status == MIDI_SYSEX && e->sysex.size) {
        size_t n = e->sysex.size;

        if (e->sysex.data[n - 1] == MIDI_SYSEX_END) {
            --n;
        }

        fluid_synth_sysex(s->synth, (const char *)e->sysex.data, (int)n, NULL, NULL, NULL, 0);
        if (n >= 4 && e->sysex.data[0] == 0x7e && e->sysex.data[2] == 9 &&
            (e->sysex.data[3] == 1 || e->sysex.data[3] == 3)) {
            memset(&s->sample_pedals, 0, sizeof(s->sample_pedals));
        }
    }
}

static uint32_t sample_start(DmAudio *a, Synth *s, uint32_t preset, int channel, int key,
                             int velocity) {
    if (!preset || a->sample_bank < 0) {
        fluid_synth_noteon(s->synth, channel, key, velocity);
        return 0;
    }

    /* FluidSynth's ordinary note-on IDs occupy a separate range in this session. */
    a->sample_serial = a->sample_serial % (UINT32_MAX / 4 - 1) + 1;
    unsigned id = UINT32_C(0x80000000) | a->sample_serial;
    DmSampleStart result = dm_live_sample_bank_start(a->sample_banks[a->sample_bank], s->synth,
                                                     preset, id, channel, key, velocity);

    if (result == DM_SAMPLE_STOCK) {
        fluid_synth_noteon(s->synth, channel, key, velocity);
        return 0;
    }

    s->sample_banks |= 1u << a->sample_bank;

    if (result == DM_SAMPLE_RELEASE) {
        dm_sample_pedal_on(&s->sample_pedals, id, (unsigned)channel);
    }

    return result == DM_SAMPLE_ONE_SHOT ? SAMPLE_ONE_SHOT : id;
}

static void sample_stop(Synth *s, uint32_t id, int channel, int key) {
    if (!id) {
        fluid_synth_noteoff(s->synth, channel, key);
    } else if (id != SAMPLE_ONE_SHOT) {
        dm_sample_pedal_off(&s->sample_pedals, s->synth, id);
    }
}

static void song_note_on(DmAudio *a, int part, int key, int velocity, uint16_t preset) {
    Synth *s = live_port(a, (unsigned)part / 16);

    if (a->monitor && s) {
        int c = part & MIDI_CHANNEL_MASK;
        uint32_t previous = a->note_sample_id[part][key];

        if (previous) {
            sample_stop(s, previous, c, key);
        }

        SampleAudition *p = &a->sample_audition;

        if (p->keyboard && p->song_part == part) {
            if (p->song_held[key]) {
                sample_stop(&p->voice, p->song_keys[key], 1, key);
            }

            DmSampleStart result = p->bank
                                       ? dm_sample_synth_start(p->bank, p->voice.synth, p->preset,
                                                               key + 256, 1, key, velocity)
                                       : DM_SAMPLE_STOCK;
            p->song_keys[key] = result == DM_SAMPLE_ONE_SHOT  ? SAMPLE_ONE_SHOT
                                : result == DM_SAMPLE_RELEASE ? (unsigned)key + 256
                                                              : 0;

            if (result == DM_SAMPLE_STOCK) {
                fluid_synth_noteon(p->voice.synth, 1, key, velocity);
            } else if (result == DM_SAMPLE_RELEASE) {
                dm_sample_pedal_on(&p->voice.sample_pedals, key + 256, 1);
            }

            p->song_held[key] = true;
            a->note_sample_id[part][key] = 0;
        } else {
            a->note_sample_id[part][key] = sample_start(a, s, preset, c, key, velocity);
        }
    }
}

static void song_note_off(DmAudio *a, int part, int key) {
    Synth *s = live_port(a, (unsigned)part / 16);

    if (a->monitor && s) {
        SampleAudition *p = &a->sample_audition;

        if (p->keyboard && p->song_part == part && p->song_held[key]) {
            sample_stop(&p->voice, p->song_keys[key], 1, key);
            p->song_held[key] = false;
        } else {
            sample_stop(s, a->note_sample_id[part][key], part & MIDI_CHANNEL_MASK, key);
        }
    }

    a->note_sample_id[part][key] = 0;
}

static void release_push(DmAudio *a, NoteRelease item) {
    if (a->release_count >= a->release_capacity) {
        return;
    }

    size_t i = a->release_count++;

    while (i && a->releases[(i - 1) / 2].seconds > item.seconds) {
        a->releases[i] = a->releases[(i - 1) / 2];
        i = (i - 1) / 2;
    }

    a->releases[i] = item;
    a->release_end = fmax(a->release_end, item.seconds);
}

static NoteRelease release_pop(DmAudio *a) {
    NoteRelease first = a->releases[0], last = a->releases[--a->release_count];
    size_t i = 0;

    while (i * 2 + 1 < a->release_count) {
        size_t child = i * 2 + 1;

        if (child + 1 < a->release_count &&
            a->releases[child + 1].seconds < a->releases[child].seconds) {
            ++child;
        }

        if (last.seconds <= a->releases[child].seconds) {
            break;
        }

        a->releases[i] = a->releases[child];
        i = child;
    }

    if (a->release_count) {
        a->releases[i] = last;
    } else {
        a->release_end = 0;
    }

    return first;
}

static void release_due(DmAudio *a) {
    while (a->release_count && a->releases[0].seconds <= a->position + .5 / RATE) {
        NoteRelease item = release_pop(a);

        if (a->note_token[item.channel][item.key] == item.token) {
            song_note_off(a, item.channel, item.key);
            queue_put(&a->midi,
                      (Command){MIDI_NOTE_OFF | (item.channel & MIDI_CHANNEL_MASK), item.key, 0});
        }
    }
}

static double playback_end(DmAudio *a) {
    return fmax(a->schedule.duration, a->release_end);
}

static void silence(DmAudio *a) {
    a->release_count = 0;
    a->release_end = 0;

    if (a->sample_audition.keyboard) {
        fluid_synth_all_sounds_off(a->sample_audition.voice.synth, 1);
        memset(a->sample_audition.song_held, 0, sizeof(a->sample_audition.song_held));
    }

    memset(a->live_program, 255, sizeof(a->live_program));
    if (a->live.sample_banks) {
        memset(a->note_sample_id[0], 0, 16 * sizeof(a->note_sample_id[0]));
    }

    for (int c = 0; c < 16; ++c) {
        fluid_synth_all_sounds_off(a->live.synth, c);

        for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
            if (a->port_live[p]) {
                fluid_synth_all_sounds_off(a->port_live[p]->synth, c);
            }
        }

        queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_SOUND_OFF, 0});
    }

    for (unsigned port = 1; port < DM_NOTE_PORTS; ++port) {
        if (a->port_live[port] && a->port_live[port]->sample_banks) {
            memset(a->note_sample_id[port * 16], 0, 16 * sizeof(a->note_sample_id[0]));
        }
    }
}

static bool parameter_control(int cc) {
    switch (cc) {
    case MIDI_CC_BANK:
    case MIDI_CC_BANK_LSB:
    case MIDI_CC_DATA:
    case MIDI_CC_DATA_LSB:
    case MIDI_CC_INCREMENT:
    case MIDI_CC_DECREMENT:
    case MIDI_CC_NRPN_LSB:
    case MIDI_CC_NRPN_MSB:
    case MIDI_CC_RPN_LSB:
    case MIDI_CC_RPN_MSB:
        return true;
    default:
        return cc >= 120;
    }
}

static void replay_event(DmAudio *a, const DmScheduled *event) {
    instrument_preview_control(a, event->port * 16 + (event->status & MIDI_CHANNEL_MASK),
                               event->status, event->a, event->b);
    if ((event->status & MIDI_STATUS_MASK) == MIDI_PROGRAM) {
        a->live_program[event->port * 16 + (event->status & MIDI_CHANNEL_MASK)] = event->a;
    }

    if (a->monitor) {
        Synth *s = live_port(a, event->port);

        if (s) {
            send_event(s, event);
        }
    }

    if (event->status < MIDI_SYSTEM) {
        queue_put(&a->midi, (Command){event->status, event->a, event->b});
    } else {
        queue_system(&a->midi, event);
    }
}

static void initial_state(DmAudio *a) {
    for (size_t i = 0; i < a->schedule.count && a->schedule.items[i].seconds == 0; ++i) {
        const DmScheduled *e = &a->schedule.items[i];
        int type = e->status & MIDI_STATUS_MASK;

        if (type != MIDI_NOTE_OFF && type != MIDI_NOTE_ON && type != MIDI_KEY_PRESSURE) {
            replay_event(a, e);
        }
    }
}

static void chase_parameters(DmAudio *a) {
    for (size_t i = 0; i < a->cursor; ++i) {
        const DmScheduled *e = &a->schedule.items[i];

        if (e->seconds == 0) {
            continue;
        }

        int type = e->status & MIDI_STATUS_MASK;

        if (e->status >= MIDI_SYSTEM || type == MIDI_PROGRAM ||
            (type == MIDI_CONTROL && parameter_control(e->a))) {
            replay_event(a, e);
        }
    }
}

static void chase_voice_on(DmAudio *a, int c, int key) {
    Command on = {MIDI_NOTE_ON | (c & MIDI_CHANNEL_MASK), key, a->state.notes[c][key]};
    song_note_on(a, c, key, on.b, a->state.sample[c][key]);

    if (a->state.held[c][key]) {
        a->note_token[c][key] = ++a->token_serial;
        release_push(a, (NoteRelease){a->seek_end[c][key], c, key, a->token_serial});
    }

    queue_put(&a->midi, on);

    if (a->state.poly[c][key] != 255) {
        Command at = {MIDI_KEY_PRESSURE | (c & MIDI_CHANNEL_MASK), key, a->state.poly[c][key]};
        send_live(a, c, at.status, at.a, at.b);
        queue_put(&a->midi, at);
    }
}

static void chase_sostenuto(DmAudio *a, int c, int value) {
    send_live(a, c, MIDI_CONTROL | (c & MIDI_CHANNEL_MASK), MIDI_CC_SOSTENUTO, value);
    queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_SOSTENUTO, value});
}

static void chase_voices(DmAudio *a, int c) {
    /* Capture only the keys that were held at the original pedal-down edge. */
    chase_sostenuto(a, c, 0);

    if (a->playing) {
        for (int key = 0; key < 128; ++key) {
            if (a->state.notes[c][key] && a->state.sostenuto[c][key]) {
                chase_voice_on(a, c, key);
            }
        }
    }

    chase_sostenuto(a, c, a->state.cc[c][66] == 255 ? 0 : a->state.cc[c][66]);

    if (!a->playing) {
        return;
    }

    for (int key = 0; key < 128; ++key) {
        if (a->state.notes[c][key]) {
            if (!a->state.sostenuto[c][key]) {
                chase_voice_on(a, c, key);
            }

            if (!a->state.held[c][key]) {
                Command off = {MIDI_NOTE_OFF | (c & MIDI_CHANNEL_MASK), key, 0};
                song_note_off(a, c, key);
                queue_put(&a->midi, off);
            }
        }
    }
}

static void chase(DmAudio *a) {
    if (!a->chase) {
        return;
    }

    chase_parameters(a);

    for (int c = 0; c < DM_PARTS; ++c) {
        if (!a->used_ports[c / 16]) {
            continue;
        }

        for (int cc = 0; cc < 120; ++cc) {
            if (a->state.cc[c][cc] != 255 && !parameter_control(cc) && cc != 66) {
                Command cmd = {MIDI_CONTROL | (c & MIDI_CHANNEL_MASK), (uint8_t)cc,
                               a->state.cc[c][cc]};
                send_live(a, c, cmd.status, cmd.a, cmd.b);
                queue_put(&a->midi, cmd);
            }
        }

        Command bend = {MIDI_PITCH_BEND | (c & MIDI_CHANNEL_MASK), a->state.bend[c][0],
                        a->state.bend[c][1]};
        send_live(a, c, bend.status, bend.a, bend.b);
        queue_put(&a->midi, bend);

        if (a->state.pressure[c] != 255) {
            Command pressure = {MIDI_CHANNEL_PRESSURE | (c & MIDI_CHANNEL_MASK),
                                a->state.pressure[c], 0};
            send_live(a, c, pressure.status, pressure.a, 0);
            queue_put(&a->midi, pressure);
        }

        chase_voices(a, c);
    }

    a->chase = false;
}

static void render(Synth *s, float *interleaved, int frames, SDL_AtomicInt *peaks) {
    float dry[32][BLOCK], fx[64][BLOCK];
    float *out[32], *effects[64];
    memset(dry, 0, sizeof(dry));
    memset(fx, 0, sizeof(fx));

    for (int i = 0; i < 32; ++i) {
        out[i] = dry[i];
    }

    for (int i = 0; i < 64; ++i) {
        effects[i] = fx[i];
    }

    fluid_synth_process(s->synth, frames, 64, effects, 32, out);

    for (int i = 0; i < frames; ++i) {
        float left = 0, right = 0;

        for (int c = 0; c < 16; ++c) {
            float l = dry[c * 2][i] + fx[c * 4][i] + fx[c * 4 + 2][i],
                  r = dry[c * 2 + 1][i] + fx[c * 4 + 1][i] + fx[c * 4 + 3][i];
            left += l;
            right += r;

            if (peaks) {
                int value = (int)(fmaxf(fabsf(l), fabsf(r)) * 10000);

                if (value > SDL_GetAtomicInt(&peaks[c])) {
                    SDL_SetAtomicInt(&peaks[c], value);
                }
            }
        }

        interleaved[i * 2] = fmaxf(-1, fminf(1, left));
        interleaved[i * 2 + 1] = fmaxf(-1, fminf(1, right));
    }
}

static void render_port_mix(Synth *s, float *out, int frames, SDL_AtomicInt *peaks) {
    if (fluid_synth_get_active_voice_count(s->synth)) {
        s->tail = RATE * 2;
    }

    if (s->tail <= 0) {
        return;
    }

    float extra[BLOCK * 2];
    render(s, extra, frames, peaks);

    for (int i = 0; i < frames * 2; i++) {
        out[i] = fmaxf(-1, fminf(1, out[i] + extra[i]));
    }

    s->tail -= frames;
}

static void render_click(DmAudio *a, float *out, int offset, int n) {
    for (int i = 0; i < n; ++i) {
        if (a->click_frames <= 0) {
            break;
        }

        double elapsed = (960 - a->click_frames) / (double)RATE;
        float value = (float)(sin(elapsed * 1800 * 6.283185307) * .12 * a->click_frames / 960);

        if (a->metronome) {
            out[(offset + i) * 2] += value;
            out[(offset + i) * 2 + 1] += value;
        }

        --a->click_frames;
    }
}

static void mix_from_state(DmAudio *a) {
    for (int part = 0; part < DM_PARTS; ++part) {
        a->mix_volume[part] = a->state.cc[part][MIDI_CC_VOLUME];
        a->mix_pan[part] = a->state.cc[part][MIDI_CC_PAN];
    }
}

static void mix_track(DmAudio *a, int part, int status, int controller, int value) {
    if ((status & MIDI_STATUS_MASK) == MIDI_CONTROL && part >= 0 && part < DM_PARTS) {
        if (controller == MIDI_CC_VOLUME) {
            a->mix_volume[part] = (uint8_t)value;
        } else if (controller == MIDI_CC_PAN) {
            a->mix_pan[part] = (uint8_t)value;
        }
    }
}

static bool dispatch_note(DmAudio *a, const DmScheduled *event) {
    int type = event->status & MIDI_STATUS_MASK;

    if (type == MIDI_NOTE_OFF || (type == MIDI_NOTE_ON && !event->b)) {
        return false;
    }

    if (type == MIDI_PROGRAM) {
        a->live_program[event->port * 16 + (event->status & MIDI_CHANNEL_MASK)] = event->a;
    }

    if (type == MIDI_NOTE_ON) {
        int channel = event->port * 16 + (event->status & MIDI_CHANNEL_MASK);

        if (a->live_program[channel] != event->note_program) {
            send_live(a, channel, MIDI_PROGRAM | (channel & MIDI_CHANNEL_MASK), event->note_program,
                      0);
            queue_put(&a->midi, (Command){MIDI_PROGRAM | (channel & MIDI_CHANNEL_MASK),
                                          event->note_program, 0});
        }

        a->note_token[channel][event->a] = ++a->token_serial;
        release_push(a, (NoteRelease){event->note_end, channel, event->a, a->token_serial});
        song_note_on(a, channel, event->a, event->b, event->note_sample);
    }

    return true;
}

static void dispatch_due(DmAudio *a) {
    if (!a->playing) {
        return;
    }

    release_due(a);

    while (a->cursor < a->schedule.count &&
           a->schedule.items[a->cursor].seconds <= a->position + .5 / RATE) {
        DmScheduled *event = &a->schedule.items[a->cursor++];

        if (!dispatch_note(a, event)) {
            continue;
        }

        if (event->status < MIDI_SYSTEM) {
            mix_track(a, event->port * 16 + (event->status & MIDI_CHANNEL_MASK), event->status,
                      event->a, event->b);
            instrument_preview_control(a, event->port * 16 + (event->status & MIDI_CHANNEL_MASK),
                                       event->status, event->a, event->b);
        }

        if (a->monitor && (event->status & MIDI_STATUS_MASK) != MIDI_NOTE_ON) {
            Synth *s = live_port(a, event->port);

            if (s) {
                send_event(s, event);
            }
        }

        if (event->status < MIDI_SYSTEM) {
            queue_put(&a->midi, (Command){event->status, event->a, event->b});
        } else {
            queue_system(&a->midi, event);
        }
    }

    while (a->beat_cursor < a->schedule.beat_count &&
           a->schedule.beats[a->beat_cursor] <= a->position + .5 / RATE) {
        a->click_frames = 960;
        ++a->beat_cursor;
    }

    if (a->recording && a->beat_cursor >= a->schedule.beat_count) {
        while (a->freewheel_click <= a->position + .5 / RATE) {
            a->click_frames = 960;
            a->freewheel_click += a->freewheel_rate;
        }
    }
}

static int next_boundary(DmAudio *a, int limit) {
    if (!a->playing) {
        return limit;
    }

    double next = INFINITY;

    if (a->cursor < a->schedule.count) {
        next = a->schedule.items[a->cursor].seconds;
    }

    if (a->release_count) {
        next = fmin(next, a->releases[0].seconds);
    }

    if (a->beat_cursor < a->schedule.beat_count) {
        next = fmin(next, a->schedule.beats[a->beat_cursor]);
    }

    if (a->loop_enabled && !a->loop_wait) {
        next = fmin(next, a->loop_end);
    }

    if (a->recording) {
        next = fmin(next, a->freewheel_click);
    } else if (a->schedule.duration > 0) {
        next = fmin(next, playback_end(a));
    }

    double frames = ceil((next - a->position) * RATE);

    if (frames > 0 && frames < limit) {
        return (int)frames;
    }

    return limit;
}

static void loop_end(DmAudio *a) {
    dispatch_due(a);
    silence(a);
    reset_live_ports(a);
    // Reset external controllers as well as internal synth state at the wrap boundary.
    for (int c = 0; c < 16; ++c) {
        queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_RESET, 0});
    }

    a->position = 0;
    ++a->input_loop;
    input_clock(a, true);
    a->cursor = a->beat_cursor = 0;
    a->click_frames = 0;
    a->chase = false;

    if (a->schedule.duration < 1.0 / RATE) {
        a->playing = false;
    }
}

static void wrap_playback(DmAudio *a) {
    if (a->loop_wait && a->position < a->loop_end) {
        a->loop_wait = false;
    }

    if (a->playing && a->loop_enabled && !a->loop_wait && a->position >= a->loop_end - .25 / RATE) {
        silence(a);
        reset_live_ports(a);

        for (int c = 0; c < 16; ++c) {
            queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_RESET, 0});
        }

        a->position = a->loop_start;
        ++a->input_loop;
        input_clock(a, true);
        a->cursor = a->loop_cursor;
        a->beat_cursor = a->loop_beat_cursor;
        copy_chase(&a->state, &a->loop_state, a->used_ports);
        mix_from_state(a);
        copy_part_rows(a->seek_end, a->loop_note_end, sizeof(a->seek_end[0]), a->used_ports);
        a->chase = a->loop_start > 0;
        a->click_frames = 0;
        initial_state(a);
        chase(a);
        return;
    }

    if (a->playing && (!a->loop_enabled || a->loop_wait) && !a->recording &&
        a->position >= playback_end(a) - .25 / RATE) {
        loop_end(a);
    }
}

static void keyboard_configure(DmAudio *a);

static void keyboard_command(DmAudio *a, Command cmd, int song_part, bool audition_notes) {
    SampleAudition *preview = &a->sample_audition;
    int message_type = cmd.status & MIDI_STATUS_MASK;
    bool controller = message_type != MIDI_NOTE_ON && message_type != MIDI_NOTE_OFF;

    if (preview->keyboard && preview->keyboard_route == (cmd.status & MIDI_CHANNEL_MASK) &&
        (audition_notes || controller)) {
        int type = cmd.status & MIDI_STATUS_MASK;

        if (type == MIDI_NOTE_ON && cmd.b) {
            if (preview->bank) {
                if (preview->keys[cmd.a]) {
                    sample_stop(&preview->voice, preview->keys[cmd.a], 0, cmd.a);
                }

                DmSampleStart start =
                    dm_sample_synth_start(preview->bank, preview->voice.synth, preview->preset,
                                          cmd.a + 2, 0, cmd.a, cmd.b);
                preview->keys[cmd.a] = start == DM_SAMPLE_ONE_SHOT ? SAMPLE_ONE_SHOT : cmd.a + 2;

                if (start == DM_SAMPLE_RELEASE) {
                    dm_sample_pedal_on(&preview->voice.sample_pedals, cmd.a + 2, 0);
                }
            } else {
                send_synth(preview->voice.synth, MIDI_NOTE_ON, cmd.a, cmd.b);
            }
        } else if (type == MIDI_NOTE_OFF || (type == MIDI_NOTE_ON && !cmd.b)) {
            if (!preview->bank || preview->keys[cmd.a]) {
                sample_stop(&preview->voice, preview->keys[cmd.a], 0, cmd.a);
            }
            preview->keys[cmd.a] = 0;
        } else if (type != MIDI_PROGRAM) {
            send_route(&preview->voice, (uint8_t)type, cmd.a, cmd.b);
        }

        if (audition_notes) {
            return;
        }
    }

    if (a->monitor) {
        a->keyboard_tail = RATE * 2;
        int type = cmd.status & MIDI_STATUS_MASK, channel = cmd.status & MIDI_CHANNEL_MASK;
        DmChannel *applied = &a->keyboard_applied[channel];
        DmSamplePatchState *patch = &a->keyboard_patch[channel];
        dm_sample_patch_state_event(patch, cmd.status, cmd.a, cmd.b);

        if (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF) {
            uint32_t *id = &a->keyboard_sample_id[channel][cmd.a];

            if (type == MIDI_NOTE_ON && cmd.b) {
                int part = a->keyboard_parts[channel];
                DmPatchKey route = {patch->selected, part / 16, part & MIDI_CHANNEL_MASK,
                                    patch->program, patch->drums};
                uint32_t preset =
                    a->sample_bank < 0
                        ? 0
                        : dm_live_sample_bank_patch(a->sample_banks[a->sample_bank], route);

                if (*id) {
                    sample_stop(&a->keyboard, *id, channel, cmd.a);
                }

                *id = sample_start(a, &a->keyboard, preset, channel, cmd.a, cmd.b);
            } else {
                sample_stop(&a->keyboard, *id, channel, cmd.a);
                *id = 0;
            }
        } else {
            send_route(&a->keyboard, cmd.status, cmd.a, cmd.b);
        }

        if (type == MIDI_PROGRAM) {
            applied->program = cmd.a;
        } else if (audition_notes && type == MIDI_CONTROL && cmd.a == MIDI_CC_VOLUME) {
            applied->volume = cmd.b;
        } else if (audition_notes && type == MIDI_CONTROL && cmd.a == MIDI_CC_PAN) {
            applied->pan = cmd.b;
        }

        if (song_part >= 0 && type != MIDI_NOTE_OFF && type != MIDI_NOTE_ON) {
            mix_track(a, song_part, cmd.status, cmd.a, cmd.b);
            send_live(a, song_part, cmd.status, cmd.a, cmd.b);
        }
    }
}

static void preview_command(DmAudio *a, Command cmd) {
    /* Preserve the computer piano's existing controller destinations. */
    int part =
        (cmd.status & MIDI_STATUS_MASK) == MIDI_KEY_PRESSURE ? -1 : cmd.status & MIDI_CHANNEL_MASK;
    keyboard_command(a, cmd, part, true);
}

static int input_controller_targets(DmAudio *a, unsigned route) {
    return atomic_load_explicit(&a->input_policy, memory_order_relaxed) == DM_INPUT_CONTROLLERS_SONG
               ? a->keyboard_parts[route]
               : -1;
}

static bool input_thru_command(DmAudio *a, Command cmd) {
    unsigned channel = cmd.status & MIDI_CHANNEL_MASK;
    unsigned type = cmd.status & MIDI_STATUS_MASK;

    if (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF) {
        bool *held = &a->input_thru_held[channel][cmd.a];

        if (type == MIDI_NOTE_OFF && !*held) {
            return true;
        }

        *held = type == MIDI_NOTE_ON;
    } else if (type == MIDI_CONTROL) {
        uint16_t bit = (uint16_t)(1u << channel);

        if (cmd.a == MIDI_CC_SUSTAIN || cmd.a == MIDI_CC_RESET) {
            a->input_thru_sustain &= ~bit;
            a->input_thru_sustain |= cmd.a == MIDI_CC_SUSTAIN && cmd.b >= 64 ? bit : 0;
        }

        if (cmd.a == MIDI_CC_SOSTENUTO || cmd.a == MIDI_CC_RESET) {
            a->input_thru_sostenuto &= ~bit;
            a->input_thru_sostenuto |= cmd.a == MIDI_CC_SOSTENUTO && cmd.b >= 64 ? bit : 0;
        }
    }

    return queue_put(&a->midi, cmd);
}

/* End external MIDI input voices when thru is disabled or the input is detached. */
static void input_thru_stop(DmAudio *a) {
    for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
        if (a->input_thru_sustain & (1u << c)) {
            queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_SUSTAIN, 0});
        }

        if (a->input_thru_sostenuto & (1u << c)) {
            queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_SOSTENUTO, 0});
        }

        for (unsigned key = 0; key < MIDI_KEYS; ++key) {
            if (a->input_thru_held[c][key] && !a->piano_held[c][key]) {
                queue_put(&a->midi, (Command){MIDI_NOTE_OFF | c, key, 64});
            }
        }
    }

    memset(a->input_thru_held, 0, sizeof(a->input_thru_held));
    a->input_thru_sustain = a->input_thru_sostenuto = 0;
}

static void input_note_off(DmAudio *a, unsigned route, unsigned key, bool thru) {
    if (a->piano_held[route][key]) {
        a->input_thru_held[route][key] = false;
        return;
    }

    Command cmd = {MIDI_NOTE_OFF | route, key, 64};
    keyboard_command(a, cmd, -1, false);

    if (thru) {
        input_thru_command(a, cmd);
    }
}

/* Ends one held input key; the voice stops when no other input or piano key holds it. */
static void input_release(DmAudio *a, InputHeld *held, bool thru) {
    unsigned route = held->route - 1, key = held->key;
    held->route = 0;

    if (!--a->input_notes[route][key]) {
        input_note_off(a, route, key, thru);
    }
}

static void input_silence(DmAudio *a, DmMidiInput *input) {
    bool thru = input && dm_midi_input_thru(input);

    for (unsigned route = 0; route < MIDI_CHANNELS; ++route) {
        for (unsigned key = 0; key < MIDI_KEYS; ++key) {
            if (a->input_notes[route][key]) {
                input_note_off(a, route, key, thru);
            }
        }
    }

    memset(a->input_held, 0, sizeof(a->input_held));
    memset(a->input_notes, 0, sizeof(a->input_notes));
}

static void input_commands(DmAudio *a) {
    DmMidiInput *input = atomic_load_explicit(&a->input, memory_order_acquire);

    if (!input || !dm_midi_input_begin(input, 0)) {
        return;
    }

    input_clock(a, false);
    unsigned routing = atomic_load_explicit(&a->input_route, memory_order_relaxed);
    unsigned route = routing & MIDI_CHANNEL_MASK;
    DmMidiInputEvent event;
    bool thru = dm_midi_input_thru(input);

    if (dm_midi_input_overflow(input, 0)) {
        input_silence(a, input);
        keyboard_silence(a);
        a->input_discard_ns = SDL_GetTicksNS();
    }

    if (!(routing & 16)) {
        input_silence(a, input);
    }

    for (unsigned n = 0; n < DM_MIDI_MONITOR_DRAIN && dm_midi_input_pop(input, 0, &event); ++n) {
        if (event.ticks_ns <= a->input_discard_ns) {
            continue;
        }

        int type = event.status & MIDI_STATUS_MASK;
        InputHeld *held = &a->input_held[event.device][event.status & MIDI_CHANNEL_MASK][event.a];
        unsigned target = route, key = event.a;

        if (event.flags & DM_MIDI_EVENT_DEVICE_RELEASE) {
            for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
                for (unsigned k = 0; k < MIDI_KEYS; ++k) {
                    if (a->input_held[event.device][c][k].route) {
                        input_release(a, &a->input_held[event.device][c][k], thru);
                    }
                }
            }

            continue;
        }

        if ((event.flags & DM_MIDI_EVENT_RELEASE) &&
            ((type == MIDI_CONTROL && event.a == MIDI_CC_RESET) || type == MIDI_PITCH_BEND)) {
            for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
                if (a->input_routes[event.device] & (1u << c)) {
                    Command reset = {type | c, event.a, event.b};
                    keyboard_command(a, reset, input_controller_targets(a, c), false);

                    if (thru) {
                        input_thru_command(a, reset);
                    }
                }
            }

            if (type == MIDI_PITCH_BEND) {
                a->input_routes[event.device] = 0;
            }

            continue;
        }

        if (type == MIDI_NOTE_OFF) {
            if (!held->route) {
                continue;
            }

            target = held->route - 1;
            key = held->key;
            held->route = 0;

            if (--a->input_notes[target][key]) {
                continue;
            }

            if (a->piano_held[target][key]) {
                a->input_thru_held[target][key] = false;
                continue;
            }
        } else {
            if (!(routing & 16)) {
                continue;
            }

            if (type == MIDI_KEY_PRESSURE) {
                key = held->route ? held->key : a->input_map[event.a];
            }

            if (type == MIDI_NOTE_ON) {
                if (!(a->preview_mask & (1u << route)) && a->keyboard_override[route] < 0) {
                    continue;
                }

                if (held->route) {
                    input_release(a, held, thru);
                }

                key = a->input_map[event.a];
                *held = (InputHeld){(uint8_t)(route + 1), (uint8_t)key};
                ++a->input_notes[route][key];

                if (a->input_notes[route][key] > 1 || a->piano_held[route][key]) {
                    if (thru && a->piano_held[route][key]) {
                        a->input_thru_held[route][key] = true;
                    }

                    continue;
                }
            }
        }

        Command cmd = {(event.status & MIDI_STATUS_MASK) | target, key, event.b};
        a->input_routes[event.device] |= (uint16_t)(1u << target);
        keyboard_command(a, cmd, input_controller_targets(a, target), false);

        if (thru && !input_thru_command(a, cmd)) {
            atomic_fetch_add_explicit(&a->input_dropped, 1, memory_order_relaxed);
        }

        atomic_fetch_add_explicit(&a->input_played, 1, memory_order_relaxed);
    }

    dm_midi_input_end(input, 0);
}

static void preview_commands(DmAudio *a) {
    Command cmd;

    while (queue_get(&a->preview, &cmd, NULL)) {
        if (cmd.live_only) {
            int part = cmd.port * 16 + (cmd.status & MIDI_CHANNEL_MASK);

            /* Mixer volume and pan hold until the song changes them, and the mixer shows them. */
            mix_track(a, part, cmd.status, cmd.a, cmd.b);

            send_live(a, part, cmd.status, cmd.a, cmd.b);
            queue_put(&a->midi, cmd);
            continue;
        }

        if (cmd.status == 0xf1) {
            int channel = cmd.a;
            a->keyboard_override[channel] = cmd.b == 255 ? -1 : cmd.b;
            a->keyboard_override_drums[channel] = cmd.port != 0;
            a->keyboard_config = true;
            keyboard_configure(a);
            continue;
        }

        if ((cmd.status & MIDI_STATUS_MASK) == MIDI_NOTE_ON && cmd.b &&
            !(a->preview_mask & (1u << (cmd.status & MIDI_CHANNEL_MASK))) &&
            a->keyboard_override[cmd.status & MIDI_CHANNEL_MASK] < 0) {
            continue;
        }

        int type = cmd.status & MIDI_STATUS_MASK, channel = cmd.status & MIDI_CHANNEL_MASK;
        bool main_keyboard =
            !a->sample_audition.keyboard || a->sample_audition.keyboard_route != channel;

        if (main_keyboard && (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF)) {
            a->piano_held[channel][cmd.a] = type == MIDI_NOTE_ON && cmd.b;

            if (a->input_notes[channel][cmd.a]) {
                if (!a->input_thru_held[channel][cmd.a]) {
                    queue_put(&a->midi, cmd);
                }

                continue;
            }
        }

        preview_command(a, cmd);
        queue_put(&a->midi, cmd);
    }
}

static void keyboard_configure(DmAudio *a) {
    if (!a->keyboard_config) {
        return;
    }

    for (int c = 0; c < 16; ++c) {
        DmChannel next = a->keyboard_channels[c], *old = &a->keyboard_applied[c];
        bool override = a->keyboard_override[c] >= 0;
        int part = a->keyboard_parts[c];
        int program = override ? a->keyboard_override[c] : a->keyboard_programs[part];
        next.drums = override ? a->keyboard_override_drums[c] : next.drums;

        if (!override && !(a->preview_mask & (1u << c))) {
            next.volume = 0;
        }

        bool type_changed = old->program < 0 || old->drums != next.drums;

        if (type_changed) {
            fluid_synth_set_channel_type(a->keyboard.synth, c,
                                         next.drums ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
            fluid_synth_bank_select(a->keyboard.synth, c, next.drums ? 128 : 0);
            a->keyboard_patch[c] = (DmSamplePatchState){.drums = next.drums};
        }

        if (type_changed || old->program != program) {
            send_synth(a->keyboard.synth, MIDI_PROGRAM | c, program, 0);
            dm_sample_patch_state_event(&a->keyboard_patch[c], MIDI_PROGRAM, program, 0);
        }

        if (old->volume != next.volume) {
            send_synth(a->keyboard.synth, MIDI_CONTROL | c, MIDI_CC_VOLUME, next.volume);
        }

        if (old->pan != next.pan) {
            send_synth(a->keyboard.synth, MIDI_CONTROL | c, MIDI_CC_PAN, next.pan);
        }

        *old = next;
        old->program = program;
    }

    a->keyboard_config = false;
}

static void keyboard_reset_controllers(DmAudio *a) {
    for (int c = 0; c < 16; ++c) {
        send_route(&a->keyboard, MIDI_CONTROL | c, MIDI_CC_RESET, 0);
        send_route(&a->keyboard, MIDI_PITCH_BEND | c, 0, MIDI_BEND_CENTER >> 7);
    }
}

static void keyboard_silence(DmAudio *a) {
    keyboard_reset_controllers(a);

    for (int c = 0; c < 16; ++c) {
        fluid_synth_all_sounds_off(a->keyboard.synth, c);
    }

    a->keyboard_tail = 0;
    input_thru_stop(a);
    memset(&a->keyboard.sample_pedals, 0, sizeof(a->keyboard.sample_pedals));
    memset(a->keyboard_sample_id, 0, sizeof(a->keyboard_sample_id));
    memset(a->piano_held, 0, sizeof(a->piano_held));
    memset(a->input_held, 0, sizeof(a->input_held));
    memset(a->input_notes, 0, sizeof(a->input_notes));
}

static void keyboard_render(DmAudio *a, float *out, int frames) {
    if (fluid_synth_get_active_voice_count(a->keyboard.synth)) {
        a->keyboard_tail = RATE * 2;
    }

    if (a->keyboard_tail <= 0) {
        return;
    }

    float keys[BLOCK * 2];
    SDL_AtomicInt peaks[16] = {0};
    render(&a->keyboard, keys, frames, peaks);

    for (int c = 0; c < 16; c++) {
        int peak = SDL_GetAtomicInt(&peaks[c]), part = a->keyboard_parts[c];

        if (peak > SDL_GetAtomicInt(&a->peaks[part])) {
            SDL_SetAtomicInt(&a->peaks[part], peak);
        }
    }

    for (int i = 0; i < frames * 2; ++i) {
        out[i] = fmaxf(-1, fminf(1, out[i] + keys[i]));
    }

    a->keyboard_tail -= frames;
}

static void feedback_stop(DmAudio *a) {
    if (a->feedback_key >= 0) {
        sample_stop(&a->feedback, a->feedback_sample_id,
                    a->feedback_program == 128 ? MIDI_DRUM_CHANNEL : 0, a->feedback_key);
        a->feedback_key = -1;
        a->feedback_sample_id = 0;
    }
}

static void feedback_commands(DmAudio *a) {
    Command cmd;

    if (SDL_SetAtomicInt(&a->feedback_queue.panic, 0)) {
        feedback_stop(a);
    }

    while (queue_get(&a->feedback_queue, &cmd, NULL)) {
        if (cmd.status == 255 || !a->monitor) {
            feedback_stop(a);
            continue;
        }

        int channel = cmd.status == 128 ? MIDI_DRUM_CHANNEL : 0;
        bool attack = a->feedback_key != cmd.a || a->feedback_program != cmd.status ||
                      a->feedback_part != cmd.feedback_part;

        if (attack) {
            feedback_stop(a);
            a->feedback_program = cmd.status;
            a->feedback_key = cmd.a;
            a->feedback_part = cmd.feedback_part;
            fluid_synth_program_change(a->feedback.synth, channel,
                                       cmd.status == 128 ? 0 : cmd.status);
        }

        fluid_synth_pitch_wheel_sens(a->feedback.synth, channel, 2);
        fluid_synth_pitch_bend(a->feedback.synth, channel,
                               MIDI_BEND_CENTER + (int)cmd.b * MIDI_BEND_CENTER / 256);

        if (attack) {
            uint32_t preset = 0;

            if (a->sample_bank >= 0 && cmd.feedback_part) {
                int part = cmd.feedback_part - 1;
                DmPatchKey route = {0, part / 16, part & MIDI_CHANNEL_MASK,
                                    cmd.status == 128 ? 0 : cmd.status, cmd.status == 128};
                preset = dm_live_sample_bank_patch(a->sample_banks[a->sample_bank], route);
            }

            a->feedback_sample_id = sample_start(a, &a->feedback, preset, channel, cmd.a, 100);
        }

        a->feedback_fraction = cmd.b;
        a->feedback_tail = RATE * 2;
    }
}

static void feedback_render(DmAudio *a, float *out, int frames) {
    if (fluid_synth_get_active_voice_count(a->feedback.synth)) {
        a->feedback_tail = RATE * 2;
    }

    if (a->feedback_tail > 0) {
        float sound[BLOCK * 2];
        render(&a->feedback, sound, frames, a->peaks);

        for (int i = 0; i < frames * 2; ++i) {
            out[i] = fmaxf(-1, fminf(1, out[i] + sound[i]));
        }

        a->feedback_tail -= frames;
    }
}

static void drain_muted_samples(Synth *s, int frames) {
    if (s && s->sample_banks && fluid_synth_get_active_voice_count(s->synth)) {
        float discard[BLOCK * 2];
        render(s, discard, frames, NULL);
    }
}

static void sample_audition_start(SampleAudition *preview) {
    dm_sample_synth_start(preview->bank, preview->voice.synth, preview->preset, 1, 0, preview->key,
                          100);
}

static void sample_audition_render(SampleAudition *preview, float *out, int frames) {
    if (!preview->active) {
        return;
    }

    float sound[BLOCK * 2];
    render(&preview->voice, sound, frames, NULL);

    for (int i = 0; i < frames * 2; ++i) {
        out[i] = fmaxf(-1, fminf(1, out[i] + sound[i]));
    }

    if (preview->release_in > 0) {
        preview->release_in -= frames;

        if (preview->release_in <= 0) {
            if (preview->bank) {
                fluid_synth_stop(preview->voice.synth, 1);
            } else {
                fluid_synth_noteoff(preview->voice.synth, 0, preview->key);
            }
        }
    }

    if (!preview->keyboard && !fluid_synth_get_active_voice_count(preview->voice.synth)) {
        if (preview->repeating) {
            sample_audition_start(preview);
        } else {
            preview->active = false;
        }
    }
}

static void run_block(DmAudio *a, float *out, int frames) {
    if (a->reset) {
        silence(a);
        reset_live_ports(a);

        if (!a->playing || a->position > 0) {
            initial_state(a);
        }

        a->reset = false;
        a->click_frames = 0;
    }

    chase(a);

    if (a->input_reset) {
        keyboard_reset_controllers(a);
        a->input_reset = false;
    }

    if (SDL_SetAtomicInt(&a->preview.panic, 0)) {
        silence(a);
        keyboard_silence(a);
    }

    if (a->keyboard_panic) {
        keyboard_silence(a);
        feedback_stop(a);

        for (int c = 0; c < 16; ++c) {
            fluid_synth_all_sounds_off(a->feedback.synth, c);
        }

        a->keyboard_panic = false;
    }

    keyboard_configure(a);
    preview_commands(a);
    input_commands(a);
    feedback_commands(a);
    keyboard_configure(a);
    int offset = 0;

    while (offset < frames) {
        wrap_playback(a);
        dispatch_due(a);
        int n = next_boundary(a, frames - offset);

        if (a->monitor) {
            render(&a->live, out + offset * 2, n, a->peaks);

            for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
                if (a->port_live[p]) {
                    render_port_mix(a->port_live[p], out + offset * 2, n, &a->peaks[p * 16]);
                }
            }

            keyboard_render(a, out + offset * 2, n);
            feedback_render(a, out + offset * 2, n);
        } else {
            memset(out + offset * 2, 0, (size_t)n * 2 * sizeof(float));
            drain_muted_samples(&a->live, n);
            drain_muted_samples(&a->keyboard, n);
            drain_muted_samples(&a->feedback, n);

            for (unsigned port = 1; port < DM_NOTE_PORTS; ++port) {
                drain_muted_samples(a->port_live[port], n);
            }
        }

        sample_audition_render(&a->sample_audition, out + offset * 2, n);
        render_click(a, out, offset, n);

        if (a->playing) {
            a->position += (double)n / RATE;
        }

        offset += n;
    }

    wrap_playback(a);

    if (a->audition_frames > 0) {
        a->audition_frames -= frames;

        if (a->audition_frames <= 0) {
            dm_audio_preview(a, a->audition_channel, a->audition_key, 0, false);
        }
    }

    SDL_SetAtomicInt(&a->playing_flag, a->playing);
    SDL_SetAtomicInt(&a->position_ms, (int)fmin(a->position * 1000, INT32_MAX));

    if (a->playing && SDL_GetAtomicInt(&a->analyzer_enabled)) {
        for (int i = 0; i < frames; ++i) {
            int at = a->analyzer_cursor * 2;
            a->analyzer_samples[at] = out[i * 2];
            a->analyzer_samples[at + 1] = out[i * 2 + 1];
            a->analyzer_cursor = (a->analyzer_cursor + 1) % DM_SPECTRUM_FRAMES;
        }

        a->analyzer_count = (int)fmin(DM_SPECTRUM_FRAMES, a->analyzer_count + frames);
    }
}

static void SDLCALL callback(void *userdata, SDL_AudioStream *stream, int needed, int total) {
    (void)total;
    DmAudio *a = userdata;
    int frames = (needed + 7) / 8;

    while (frames > 0) {
        float buffer[BLOCK * 2];
        int n = frames > BLOCK ? BLOCK : frames;
        run_block(a, buffer, n);
        SDL_PutAudioStreamData(stream, buffer, n * 8);
        frames -= n;
    }
}

static bool midi_send_system(DmAudio *a, const DmScheduled *event) {
    DmBytes bytes = {0};
    bool ok = true;

    if (event->status == MIDI_SYSEX) {
        uint8_t start = MIDI_SYSEX;
        ok = dm_bytes(&bytes, &start, 1);
    }

    ok = ok && dm_bytes(&bytes, event->sysex.data, event->sysex.size);

    if (ok) {
        ok = rtmidi_out_send_message(a->device, bytes.data, (int)bytes.size) >= 0;
    }

    dm_bytes_free(&bytes);
    return ok;
}

static int midi_worker(void *userdata) {
    DmAudio *a = userdata;

    while (!SDL_GetAtomicInt(&a->quit)) {
        SDL_LockMutex(a->device_lock);
        Command cmd;
        const DmScheduled *system = NULL;

        if (SDL_SetAtomicInt(&a->midi.panic, 0) && a->device_index >= 0) {
            for (int c = 0; c < 16; ++c) {
                uint8_t b[] = {MIDI_CONTROL | c, MIDI_CC_SOUND_OFF, 0};
                rtmidi_out_send_message(a->device, b, 3);
            }
        }

        while (queue_get(&a->midi, &cmd, &system)) {
            if (a->device_index < 0) {
                continue;
            }

            if (system) {
                if (!midi_send_system(a, system)) {
                    SDL_SetAtomicInt(&a->midi_failed, 1);
                }

                continue;
            }

            uint8_t b[] = {cmd.status, cmd.a, cmd.b};
            int n = (cmd.status & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3;

            DmMidiInput *input = atomic_load_explicit(&a->input, memory_order_acquire);

            if (input && dm_midi_input_is_open(input)) {
                dm_midi_echo_write(dm_midi_input_echo(input), cmd.status, cmd.a, n == 3 ? cmd.b : 0,
                                   SDL_GetTicksNS());
            }

            if (rtmidi_out_send_message(a->device, b, n) < 0) {
                SDL_SetAtomicInt(&a->midi_failed, 1);
            }
        }

        free_retired(a);
        SDL_UnlockMutex(a->device_lock);
        SDL_Delay(1);
    }

    return 0;
}

void dm_audio_attach_midi_input(DmAudio *a, DmMidiInput *input) {
    if (!a) {
        return;
    }

    double latency = dm_audio_buffer_ms(a) / 1000;
    SDL_LockMutex(a->device_lock);

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    input_thru_stop(a);
    input_silence(a, NULL);
    atomic_store_explicit(&a->input, input, memory_order_release);

    if (a->input_latency_auto) {
        a->input_latency = latency;
    }

    a->input_discard_ns = 0;
    input_clock(a, true);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    SDL_UnlockMutex(a->device_lock);
}

void dm_audio_input_route(DmAudio *a, int channel, bool enabled) {
    if (a) {
        atomic_store_explicit(&a->input_route, (channel & MIDI_CHANNEL_MASK) | (enabled ? 16 : 0),
                              memory_order_relaxed);
    }
}

void dm_audio_input_controllers(DmAudio *a, DmInputControllerPolicy policy) {
    if (a && (policy == DM_INPUT_CONTROLLERS_KEYBOARD || policy == DM_INPUT_CONTROLLERS_SONG)) {
        atomic_store_explicit(&a->input_policy, policy, memory_order_relaxed);
    }
}

void dm_audio_input_thru(DmAudio *a, DmMidiInput *input, bool enabled) {
    if (dm_midi_input_thru(input) == enabled) {
        return;
    }

    if (a && a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    if (a && !enabled) {
        input_thru_stop(a);
    }

    dm_midi_input_set_thru(input, enabled);

    if (a && a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_input_note_map(DmAudio *a, const uint8_t map[MIDI_KEYS]) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    for (unsigned key = 0; key < MIDI_KEYS; ++key) {
        a->input_map[key] = map ? map[key] & MIDI_DATA_MASK : (uint8_t)key;
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_input_offset(DmAudio *a, double seconds) {
    if (!a || isinf(seconds)) {
        return;
    }

    bool automatic = isnan(seconds);
    double latency = automatic ? dm_audio_buffer_ms(a) / 1000 : seconds;

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->input_latency_auto = automatic;
    a->input_latency = latency;
    input_clock(a, false);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

DmAudio *dm_audio_create(char *error, size_t cap) {
    DmAudio *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }

    atomic_init(&a->input, NULL);
    atomic_init(&a->input_route, 0);
    atomic_init(&a->input_policy, DM_INPUT_CONTROLLERS_KEYBOARD);
    atomic_init(&a->input_played, 0);
    atomic_init(&a->input_dropped, 0);
    a->input_latency_auto = true;

    for (unsigned key = 0; key < MIDI_KEYS; ++key) {
        a->input_map[key] = (uint8_t)key;
    }

    memset(a->mix_volume, 255, sizeof(a->mix_volume));
    memset(a->mix_pan, 255, sizeof(a->mix_pan));

    a->device_index = -1;
    a->sample_bank = -1;
    a->preview_mask = UINT16_MAX;
    a->monitor = true;
    a->click_bpm = 120;

    if (!synth_create(&a->live, NULL) || !synth_create(&a->keyboard, NULL) ||
        !synth_create(&a->feedback, NULL)) {
        snprintf(error, cap, DM_TEXT_CANNOT_INITIALIZE_FLUIDSYNTH);
        dm_audio_destroy(a);
        return NULL;
    }

    a->feedback_key = -1;

    for (int c = 0; c < 16; ++c) {
        a->keyboard_channels[c].program = 0;
        a->keyboard_channels[c].volume = 100;
        a->keyboard_channels[c].pan = 64;
        a->keyboard_channels[c].drums = c == MIDI_DRUM_CHANNEL;
        a->keyboard_override[c] = -1;
        a->keyboard_parts[c] = c;
        a->keyboard_applied[c].program = -1;
        a->keyboard_applied[c].volume = -1;
        a->keyboard_applied[c].pan = -1;
    }

    a->device_lock = SDL_CreateMutex();
    a->device = rtmidi_out_create_default();

    if (a->device && !a->device->ok) {
        rtmidi_out_free(a->device);
        a->device = NULL;
    }

    if (!a->device_lock) {
        snprintf(error, cap, DM_TEXT_CANNOT_INITIALIZE_MIDI);
        dm_audio_destroy(a);
        return NULL;
    }

    if (a->device) {
        a->midi_thread = SDL_CreateThread(midi_worker, DM_TEXT_MIDI_OUTPUT, a);
    }

    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, a);

    if (!a->stream) {
        snprintf(error, cap, DM_TEXT_AUDIO_UNAVAILABLE_USE, SDL_GetError());
        return a;
    }

    SDL_ResumeAudioStreamDevice(a->stream);
    return a;
}

void dm_audio_destroy(DmAudio *a) {
    if (!a) {
        return;
    }

    SDL_SetAtomicInt(&a->cancel, 1);

    if (a->worker) {
        SDL_WaitThread(a->worker, NULL);
    }

    if (a->stream) {
        SDL_DestroyAudioStream(a->stream);
    }

    SDL_SetAtomicInt(&a->quit, 1);

    if (a->midi_thread) {
        SDL_WaitThread(a->midi_thread, NULL);
    }

    if (a->device) {
        if (a->device_index >= 0) {
            for (int c = 0; c < 16; ++c) {
                uint8_t b[] = {MIDI_CONTROL | c, MIDI_CC_SOUND_OFF, 0};
                rtmidi_out_send_message(a->device, b, 3);
            }
        }

        rtmidi_out_free(a->device);
    }

    if (a->device_lock) {
        SDL_DestroyMutex(a->device_lock);
    }

    synth_free(&a->live);
    synth_free(&a->keyboard);
    synth_free(&a->feedback);
    synth_free(&a->prepared);
    synth_free(&a->prepared_keyboard);
    synth_free(&a->prepared_feedback);
    synth_free(&a->sample_audition.voice);
    dm_sample_synth_free(a->sample_audition.bank);
    free_port_synths(a->port_live);
    free_port_synths(a->prepared_ports);

    for (unsigned i = 0; i < LIVE_SAMPLE_BANKS; ++i) {
        dm_live_sample_bank_free(a->sample_banks[i]);
    }

    dm_schedule_free(&a->schedule);
    dm_schedule_free(&a->render_schedule);
    dm_bytes_free(&a->render_project);
    free_retired(a);
    free(a->releases);
    free(a);
}

static void chase_note_off(DmAudio *a, int channel, int key) {
    a->state.held[channel][key] = 0;

    if (a->state.cc[channel][64] < 64 && !a->state.sostenuto[channel][key]) {
        a->state.notes[channel][key] = 0;
    }
}

static void chase_pedal_release(DmAudio *a, int channel) {
    for (int key = 0; key < 128; ++key) {
        if (!a->state.held[channel][key] && a->state.cc[channel][64] < 64 &&
            !a->state.sostenuto[channel][key]) {
            a->state.notes[channel][key] = 0;
        }
    }
}

static void reset_chase_channel(DmAudio *a, int c) {
    memset(a->state.cc[c], 255, 128);
    a->state.cc[c][64] = 0;
    a->state.cc[c][66] = 0;
    memset(a->state.notes[c], 0, 128);
    memset(a->state.held[c], 0, 128);
    memset(a->state.sostenuto[c], 0, 128);
    memset(a->state.poly[c], 255, 128);
    a->state.pressure[c] = 255;
    a->state.bend[c][0] = 0;
    a->state.bend[c][1] = 64;
}

static void reset_chase_controllers(DmAudio *a, int c) {
    const int keep[] = {0, 32, 7, 10};
    uint8_t values[4];

    for (int i = 0; i < 4; ++i) {
        values[i] = a->state.cc[c][keep[i]];
    }

    memset(a->state.cc[c], 255, 128);

    for (int i = 0; i < 4; ++i) {
        a->state.cc[c][keep[i]] = values[i];
    }

    a->state.cc[c][64] = 0;
    a->state.cc[c][66] = 0;
    memset(a->state.sostenuto[c], 0, 128);
    a->state.bend[c][0] = 0;
    a->state.bend[c][1] = 64;
    a->state.pressure[c] = 0;
    memset(a->state.poly[c], 0, 128);
    chase_pedal_release(a, c);
}

static void cache_controller(DmAudio *a, int c, int cc, int value) {
    int previous = a->state.cc[c][cc];
    a->state.cc[c][cc] = value;

    if (cc == 66) {
        if (value >= 64 && previous < 64) {
            memcpy(a->state.sostenuto[c], a->state.held[c], 128);
        } else if (value < 64) {
            memset(a->state.sostenuto[c], 0, 128);
            chase_pedal_release(a, c);
        }
    }

    if (cc == 64 && value < 64) {
        chase_pedal_release(a, c);
    }

    if (cc == 121) {
        reset_chase_controllers(a, c);
    }

    if (cc == 120) {
        memset(a->state.notes[c], 0, 128);
        memset(a->state.held[c], 0, 128);
        memset(a->state.sostenuto[c], 0, 128);
    }

    if (cc >= 123) {
        for (int key = 0; key < 128; ++key) {
            chase_note_off(a, c, key);
        }
    }
}

static bool gm_reset(const DmScheduled *event) {
    if (event->status != MIDI_SYSEX || event->sysex.size < 4) {
        return false;
    }

    const uint8_t *b = event->sysex.data;
    return b[0] == 0x7e && b[2] == 9 && (b[3] == 1 || b[3] == 3);
}

static void cache_event(DmAudio *a, const DmScheduled *event) {
    if (gm_reset(event)) {
        for (int c = event->port * 16; c < event->port * 16 + 16; ++c) {
            reset_chase_channel(a, c);
        }

        return;
    }

    int c = event->port * 16 + (event->status & MIDI_CHANNEL_MASK);

    switch (event->status & MIDI_STATUS_MASK) {
    case MIDI_KEY_PRESSURE:
        a->state.poly[c][event->a] = event->b;
        break;
    case MIDI_CHANNEL_PRESSURE:
        a->state.pressure[c] = event->a;
        break;
    case MIDI_CONTROL:
        cache_controller(a, c, event->a, event->b);
        break;
    case MIDI_PITCH_BEND:
        a->state.bend[c][0] = event->a;
        a->state.bend[c][1] = event->b;
        break;
    case MIDI_NOTE_ON:
        if (event->b) {
            a->state.notes[c][event->a] = event->b;
            a->state.sample[c][event->a] = event->note_sample;
            a->state.held[c][event->a] = 1;
            a->seek_end[c][event->a] = event->note_end;
        } else {
            chase_note_off(a, c, event->a);
        }
        break;
    case MIDI_NOTE_OFF:
        chase_note_off(a, c, event->a);
        break;
    }
}

static void prepare_chase(DmAudio *a, const DmSchedule *s, double seconds) {
    a->beat_cursor = 0;

    while (a->beat_cursor < s->beat_count && s->beats[a->beat_cursor] < seconds) {
        ++a->beat_cursor;
    }

    memset(a->used_ports, 0, sizeof(a->used_ports));
    a->used_ports[0] = true;

    for (size_t i = 0; i < s->count; i++) {
        a->used_ports[s->items[i].port] = true;
    }

    for (int c = 0; c < DM_PARTS; ++c) {
        if (a->used_ports[c / 16]) {
            reset_chase_channel(a, c);
        }
    }

    a->cursor = 0;

    while (a->cursor < s->count && s->items[a->cursor].seconds < seconds) {
        cache_event(a, &s->items[a->cursor++]);
    }

    a->chase = seconds > 0;
}

void dm_audio_loop(DmAudio *a, bool enabled, double start, double end) {
    if (!a) {
        return;
    }

    enabled =
        enabled && isfinite(start) && isfinite(end) && start >= 0 && end - start >= 1.0 / RATE;
    DmAudio *scratch = enabled ? calloc(1, sizeof(*scratch)) : NULL;

    if (enabled && !scratch) {
        return;
    }

    /* GUI-thread preparation; the callback only copies bounded state at each wrap. */
    if (enabled) {
        prepare_chase(scratch, &a->schedule, start);
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->loop_enabled = enabled;
    a->loop_start = start;
    a->loop_end = end;
    a->loop_wait = enabled && a->position >= end;

    if (enabled) {
        copy_chase(&a->loop_state, &scratch->state, a->used_ports);
        copy_part_rows(a->loop_note_end, scratch->seek_end, sizeof(a->loop_note_end[0]),
                       a->used_ports);
        a->loop_cursor = scratch->cursor;
        a->loop_beat_cursor = scratch->beat_cursor;
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    free(scratch);
}

static uint16_t project_preview_mask(const DmProject *p) {
    bool solo = false;

    for (int c = 0; c < 16; ++c) {
        solo |= p->channels[c].enabled && p->channels[c].solo;
    }

    uint16_t mask = 0;

    for (int c = 0; c < 16; ++c) {
        if (p->channels[c].enabled && !p->channels[c].mute && (!solo || p->channels[c].solo)) {
            mask |= (uint16_t)(1u << c);
        }
    }

    return mask;
}

static bool prepare_note_ends(DmSchedule *schedule, const DmProject *project,
                              const DmPatternPreview *preview, const DmLiveSampleBank *bank) {
    size_t (*head)[128] = malloc(DM_PARTS * sizeof(*head)),
           (*tail)[128] = malloc(DM_PARTS * sizeof(*tail));
    uint8_t programs[DM_PARTS] = {0};
    DmSamplePatchState patches[DM_PARTS];

    if (bank) {
        dm_sample_patch_state_init(project, patches);
    }

    if (!head || !tail) {
        free(head);
        free(tail);
        return false;
    }

    memset(head, 255, DM_PARTS * sizeof(*head));
    memset(tail, 255, DM_PARTS * sizeof(*tail));
    size_t *links = malloc((schedule->count + 1) * sizeof(*links));

    if (!links) {
        free(head);
        free(tail);
        return false;
    }

    for (size_t i = 0; i < schedule->count; ++i) {
        DmScheduled *event = &schedule->items[i];
        int type = event->status & MIDI_STATUS_MASK,
            c = event->port * 16 + (event->status & MIDI_CHANNEL_MASK), key = event->a;
        int source_channel = preview && preview->program >= 0 ? preview->channel
                                                             : c & MIDI_CHANNEL_MASK;
        int source_part = event->port * 16 + source_channel;

        if (type == MIDI_PROGRAM) {
            programs[c] = event->a;
        }

        if (bank && event->status < MIDI_SYSTEM) {
            dm_sample_patch_state_event(&patches[source_part], event->status, event->a, event->b);
        } else if (bank && gm_reset(event)) {
            for (int ch = 0; ch < 16; ++ch) {
                DmSamplePatchState *patch = &patches[event->port * 16 + ch];
                *patch = (DmSamplePatchState){.drums = ch == MIDI_DRUM_CHANNEL};
            }
        }

        if (type == MIDI_NOTE_ON && event->b) {
            event->note_program = programs[c];
            event->note_end = schedule->duration;
            links[i] = SIZE_MAX;

            if (bank) {
                DmSamplePatchState patch = patches[source_part];
                bool drums = preview && preview->program >= 0 && !preview->follow
                                 ? preview->program == 128
                                 : patch.drums;
                DmPatchKey route = {patch.selected, event->port, source_channel, patch.program,
                                    drums};
                event->note_sample = (uint16_t)dm_live_sample_bank_patch(bank, route);
            }

            if (head[c][key] == SIZE_MAX) {
                head[c][key] = i;
            } else {
                links[tail[c][key]] = i;
            }

            tail[c][key] = i;
        } else if ((type == MIDI_NOTE_OFF || (type == MIDI_NOTE_ON && !event->b)) &&
                   head[c][key] != SIZE_MAX) {
            size_t on = head[c][key];
            schedule->items[on].note_end = event->seconds;
            head[c][key] = links[on];
        }
    }

    free(links);
    free(head);
    free(tail);
    return true;
}

static void free_retired(DmAudio *a) {
    while (a->retired) {
        RetiredSchedule *old = a->retired;
        a->retired = old->next;
        dm_schedule_free(&old->schedule);
        free(old);
    }
}

static size_t schedule_after(DmSchedule *schedule, double seconds) {
    size_t lo = 0, hi = schedule->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (schedule->items[mid].seconds <= seconds) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo;
}

static NoteRelease *prepare_release_storage(DmAudio *a, size_t count, size_t *capacity) {
    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    *capacity = count + a->schedule.count + a->release_count + 2048;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    return malloc(*capacity * sizeof(NoteRelease));
}

static void edit_cursor(DmAudio *scratch, DmSchedule *next, double seconds) {
    scratch->cursor = schedule_after(next, seconds);
    size_t lo = 0, hi = next->beat_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (next->beats[mid] < seconds) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    scratch->beat_cursor = lo;
}

static bool install_release_storage(DmAudio *a, DmAudio *scratch, DmSchedule *next,
                                    NoteRelease *releases, bool editing, double *seconds,
                                    bool *playing) {
    bool preserve = editing && a->playing;

    if (preserve) {
        *seconds = a->position;
        *playing = a->playing;
        edit_cursor(scratch, next, *seconds);

        if (a->release_count) {
            memcpy(releases, a->releases, a->release_count * sizeof(*releases));
        }
    } else {
        a->release_count = 0;
        a->release_end = 0;
    }

    return preserve;
}

static void free_port_synths(Synth **ports) {
    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        if (ports[p]) {
            synth_free(ports[p]);
            free(ports[p]);
            ports[p] = NULL;
        }
    }
}

static bool prepare_ports(DmAudio *a, const bool *used, Synth **fresh, char *error, size_t cap) {
    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        if (used[p] && !a->port_live[p]) {
            if (SDL_GetAtomicInt(&a->job_busy) && a->job_type == 1) {
                snprintf(error, cap, DM_TEXT_WAIT_SOUNDFONT_LOADING_BEFORE);
                free_port_synths(fresh);
                return false;
            }

            fresh[p] = calloc(1, sizeof(Synth));

            if (!fresh[p] || !synth_create(fresh[p], a->font_path)) {
                snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_INDEPENDENT_PLAYBACK, p);
                free_port_synths(fresh);
                return false;
            }
        }
    }

    return true;
}

static unsigned sample_bank_users(Synth *s) {
    if (s && s->sample_banks && !fluid_synth_get_active_voice_count(s->synth)) {
        s->sample_banks = 0;
    }

    return s ? s->sample_banks : 0;
}

static void collect_sample_banks(DmAudio *a) {
    unsigned retired = 0;

    for (int i = 0; i < LIVE_SAMPLE_BANKS; ++i) {
        if (i != a->sample_bank && a->sample_banks[i]) {
            retired |= 1u << i;
        }
    }

    if (!retired) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    /* FluidSynth 2.5.4 counts overflow voices as active until they drop their samples. */
    unsigned used = sample_bank_users(&a->live) | sample_bank_users(&a->keyboard) |
                    sample_bank_users(&a->feedback);

    for (unsigned port = 1; port < DM_NOTE_PORTS; ++port) {
        used |= sample_bank_users(a->port_live[port]);
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    for (int i = 0; i < LIVE_SAMPLE_BANKS; ++i) {
        if ((retired & ~used) & (1u << i)) {
            dm_live_sample_bank_free(a->sample_banks[i]);
            a->sample_banks[i] = NULL;
        }
    }
}

static void DME_CALL live_sample_warning(void *user, uint32_t severity, uint32_t flags,
                                        const char *message) {
    (void)flags;
    char *warning = user;

    if (severity == DME_WARNING && !*warning) {
        snprintf(warning, sizeof(((DmAudio *)0)->sample_warning), "%s", message);
    }
}

static bool publish_schedule(DmAudio *a, const DmProject *p, const DmPatternPreview *preview,
                             DmSchedule next, double seconds, bool playing, bool editing,
                             char *error, size_t cap) {
    collect_sample_banks(a);
    int slot = a->sample_bank;
    DmLiveSampleBank *bank = slot < 0 ? NULL : a->sample_banks[slot], *fresh_bank = NULL;
    char warning[sizeof(a->sample_warning)] = {0};

    if (a->sample_bank_dirty || !dm_live_sample_bank_matches(bank, p)) {
        slot = -1;
        bank = NULL;

        if (p->patch_replacement_count) {
            for (int i = 0; i < LIVE_SAMPLE_BANKS; ++i) {
                if (!a->sample_banks[i]) {
                    slot = i;
                    break;
                }
            }

            if (slot < 0) {
                snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_BUSY);
                dm_schedule_free(&next);
                return false;
            }

            DmeHost host = {.user = warning, .diagnostic = live_sample_warning};
            bank = fresh_bank = dm_live_sample_bank_create(p, &host);

            if (!bank) {
                snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_PLAYBACK);
                dm_schedule_free(&next);
                return false;
            }
        }
    }

    /* Prepare seek state outside the audio lock. Swap only bounded state in the critical section.
     */
    DmAudio *scratch = calloc(1, sizeof(*scratch));
    DmAudio *loop = NULL;
    RetiredSchedule *retired = calloc(1, sizeof(*retired));

    if (!scratch || !retired || !prepare_note_ends(&next, p, preview, bank)) {
        free(scratch);
        free(retired);
        dm_live_sample_bank_free(fresh_bank);
        dm_schedule_free(&next);
        return false;
    }

    prepare_chase(scratch, &next, seconds);

    if (a->loop_enabled) {
        loop = calloc(1, sizeof(*loop));

        if (!loop) {
            free(scratch);
            free(retired);
            dm_live_sample_bank_free(fresh_bank);
            dm_schedule_free(&next);
            return false;
        }

        prepare_chase(loop, &next, a->loop_start);
    }

    size_t capacity;
    NoteRelease *releases = prepare_release_storage(a, next.count, &capacity);

    if (!releases) {
        free(scratch);
        free(retired);
        free(loop);
        dm_live_sample_bank_free(fresh_bank);
        dm_schedule_free(&next);
        return false;
    }

    Synth *fresh[DM_NOTE_PORTS] = {0};

    if (!prepare_ports(a, scratch->used_ports, fresh, error, cap)) {
        free(releases);
        free(scratch);
        free(retired);
        free(loop);
        dm_live_sample_bank_free(fresh_bank);
        dm_schedule_free(&next);
        return false;
    }

    SDL_LockMutex(a->device_lock);

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    bool preserve =
        install_release_storage(a, scratch, &next, releases, editing, &seconds, &playing);
    bool was_playing = a->playing;
    NoteRelease *old_releases = a->releases;
    a->releases = releases;
    a->release_capacity = capacity;
    DmSchedule old = a->schedule;
    a->schedule = next;

    if (slot != a->sample_bank) {
        if (fresh_bank) {
            a->sample_banks[slot] = fresh_bank;
        }

        a->sample_bank = slot;
        memcpy(a->sample_warning, warning, sizeof(warning));
    }

    a->sample_bank_dirty = false;

    a->freewheel_rate = next.final_rate;
    a->freewheel_click = next.next_beat;

    if (seconds > a->freewheel_click) {
        a->freewheel_click +=
            ceil((seconds - a->freewheel_click) / a->freewheel_rate) * a->freewheel_rate;
    }

    a->cursor = scratch->cursor;
    a->beat_cursor = scratch->beat_cursor;

    if (!preserve) {
        SDL_SetAtomicInt(&a->midi.read, SDL_GetAtomicInt(&a->midi.write));
    }

    memcpy(a->used_ports, scratch->used_ports, sizeof(a->used_ports));

    if (loop) {
        copy_chase(&a->loop_state, &loop->state, a->used_ports);
        copy_part_rows(a->loop_note_end, loop->seek_end, sizeof(a->loop_note_end[0]),
                       a->used_ports);
        a->loop_cursor = loop->cursor;
        a->loop_beat_cursor = loop->beat_cursor;
    }

    for (unsigned port = 1; port < DM_NOTE_PORTS; port++) {
        if (fresh[port]) {
            a->port_live[port] = fresh[port];
        }
    }

    copy_part_rows(a->seek_end, scratch->seek_end, sizeof(a->seek_end[0]), a->used_ports);
    copy_chase(&a->state, &scratch->state, a->used_ports);
    mix_from_state(a);
    a->preview_mask = project_preview_mask(p);
    memcpy(a->keyboard_channels, p->channels, sizeof(a->keyboard_channels));

    for (int part = 0; part < DM_PARTS; ++part) {
        int channel = part & MIDI_CHANNEL_MASK;
        int program = p->channels[channel].program;
        a->keyboard_programs[part] = program < 0 ? 0 : (uint8_t)program;
    }

    for (size_t i = 0; i < p->port_mix_count; ++i) {
        DmPortMix mix = p->port_mix[i];

        if (mix.program) {
            a->keyboard_programs[mix.port * 16 + mix.channel] = mix.program - 1;
        }
    }

    a->keyboard_config = true;
    a->chase = preserve ? a->chase : playing && scratch->chase;
    a->position = seconds;
    a->playing = playing;
    a->reset = a->reset || !preserve;
    /* Running seeks and song wraps preserve the piano's held voices and pedal state. */
    a->input_reset |= !editing && (!playing || !was_playing);
    input_clock(a, true);
    SDL_SetAtomicInt(&a->position_ms, (int)(seconds * 1000));
    SDL_SetAtomicInt(&a->playing_flag, playing);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    if (!preserve || SDL_GetAtomicInt(&a->midi.read) == SDL_GetAtomicInt(&a->midi.write)) {
        free_retired(a);
    }

    retired->schedule = old;
    retired->next = a->retired;
    a->retired = retired;
    SDL_UnlockMutex(a->device_lock);
    bool colored = false;

    for (unsigned port = 1; port < DM_NOTE_PORTS; port++) {
        colored |= a->used_ports[port];
    }

    if (colored && a->device_index >= 0 && !a->port_warning_sent) {
        SDL_SetAtomicInt(&a->port_warning, 1);
        a->port_warning_sent = true;
    }

    if (!colored) {
        a->port_warning_sent = false;
    }

    free(old_releases);
    free(scratch);
    free(loop);
    collect_sample_banks(a);
    return true;
}

bool dm_audio_publish(DmAudio *a, const DmProject *p, double seconds, bool playing, char *error,
                      size_t cap) {
    if (!a) {
        return false;
    }

    DmSchedule next = {0};

    if (!dm_schedule(p, &next)) {
        snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_PLAYBACK);
        return false;
    }

    return publish_schedule(a, p, NULL, next, seconds, playing, false, error, cap);
}

void dm_audio_reload_samples(DmAudio *a) {
    if (a) {
        a->sample_bank_dirty = true;
    }
}

static void preview_channels(DmProject *view, const DmPatternPreview *v) {
    for (int c = 0; c < 16; ++c) {
        view->channels[c].mute = false;
        view->channels[c].solo = false;

        if (v->program >= 0) {
            view->channels[c].enabled = false;
        }
    }

    view->channels[v->channel].enabled = true;

    if (v->program >= 0 && !v->follow) {
        view->channels[v->channel].program = v->program == 128 ? 0 : v->program;
    }
}

/* One remembered value per channel message type and controller. */
enum { PREVIEW_CHASE_KEYS = 8 * 128 };

typedef struct {
    bool set;
    double beat;
    const DmEvent *event;
    const DmAutomation *automation;
} PreviewChase;

static PreviewChase *preview_chase_slot(PreviewChase *chase, int type, int a) {
    bool keyed = type == MIDI_CONTROL || type == MIDI_KEY_PRESSURE;
    return &chase[((type >> 4) & 7) * 128 + (keyed ? a & 127 : 0)];
}

static void preview_remember(PreviewChase *slot, double beat, const DmEvent *event,
                             const DmAutomation *automation) {
    if (!slot->set || beat >= slot->beat) {
        *slot = (PreviewChase){true, beat, event, automation};
    }
}

/* Song beat 0 values replaced by the channel settings during playback. */
static bool preview_channel_setting(const uint8_t *m, double beat) {
    int type = m[0] & MIDI_STATUS_MASK;
    return beat == 0 && (type == MIDI_PROGRAM ||
                         (type == MIDI_CONTROL && (m[1] == MIDI_CC_VOLUME || m[1] == MIDI_CC_PAN)));
}

static void preview_chase_raw(PreviewChase *chase, const DmEvent *event) {
    const uint8_t *m = event->message.data;
    int type = m[0] & MIDI_STATUS_MASK;

    if (type != MIDI_NOTE_ON && type != MIDI_NOTE_OFF && !preview_channel_setting(m, event->beat)) {
        preview_remember(preview_chase_slot(chase, type, event->message.size > 1 ? m[1] : 0),
                         event->beat, event, NULL);
    }
}

/* Applies the channel state at the region start: programs, volume and pan become the
   channel settings; other messages are sent at the pattern start. */
static void preview_apply_chase(DmProject *view, const PreviewChase *chase,
                                const DmPatternPreview *v, size_t *automation) {
    DmChannel *channel = &view->channels[v->channel];

    for (int i = 0; i < PREVIEW_CHASE_KEYS; ++i) {
        if (!chase[i].set) {
            continue;
        }

        const DmEvent *event = chase[i].event;
        const DmAutomation *a = chase[i].automation;
        int type = event ? event->message.data[0] & MIDI_STATUS_MASK : a->type;
        int first = event ? (event->message.size > 1 ? event->message.data[1] : 0) : a->a;
        int second = event ? (event->message.size > 2 ? event->message.data[2] : 0) : a->b;

        if (type == MIDI_PROGRAM) {
            if (v->follow) {
                channel->program = first;
            }
        } else if (type == MIDI_CONTROL && first == MIDI_CC_VOLUME) {
            channel->volume = second;
        } else if (type == MIDI_CONTROL && first == MIDI_CC_PAN) {
            channel->pan = second;
        } else if (event) {
            DmEvent moved = *event;
            moved.beat = 0;
            view->events[view->event_count++] = moved;
        } else {
            DmAutomation moved = *a;
            moved.beat = 0;
            moved.curve = 0;
            view->automation[(*automation)++] = moved;
        }
    }
}

/* Plays the channel's song events from the pattern's placement region, starting from the state
   in effect at that point. Tempo and other system events keep their song timing. */
static bool preview_region(DmProject *view, const DmProject *p, const DmPatternPreview *v) {
    PreviewChase *chase = calloc(PREVIEW_CHASE_KEYS, sizeof(*chase));
    view->events = calloc(p->event_count + 1, sizeof(*view->events));
    view->automation = calloc(p->automation_count + 1, sizeof(*view->automation));

    if (!chase || !view->events || !view->automation) {
        free(chase);
        return false;
    }

    double origin = v->origin, end = v->origin + v->length;
    view->event_count = 0;

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *event = &p->events[i];

        if (!event->message.size) {
            continue;
        }

        unsigned status = event->message.data[0];

        if (status >= MIDI_SYSTEM) {
            if (event->beat <= v->length) {
                view->events[view->event_count++] = *event;
            }
        } else if ((int)(status & MIDI_CHANNEL_MASK) != v->channel || event->beat > end) {
            continue;
        } else if (event->beat > origin) {
            DmEvent moved = *event;
            moved.beat -= origin;
            view->events[view->event_count++] = moved;
        } else {
            preview_chase_raw(chase, event);
        }
    }

    size_t automation = 0;

    for (size_t i = 0; i < p->automation_count; ++i) {
        const DmAutomation *a = &p->automation[i];

        if (a->pattern >= 0) {
            view->automation[automation++] = *a;
        } else if (a->channel != v->channel || a->beat > end) {
            continue;
        } else if (a->beat > origin) {
            DmAutomation moved = *a;
            moved.beat -= origin;
            view->automation[automation++] = moved;
        } else {
            preview_remember(preview_chase_slot(chase, a->type, a->a), a->beat, NULL, a);
        }
    }

    preview_apply_chase(view, chase, v, &automation);
    view->automation_count = automation;
    free(chase);
    return true;
}

static void preview_schedule_program(DmSchedule *next, const DmPatternPreview *v) {
    if (v->program < 0) {
        return;
    }

    int route = v->program == 128 ? MIDI_DRUM_CHANNEL : 0;

    for (size_t i = 0; i < next->count; ++i) {
        DmScheduled *event = &next->items[i];

        if (event->status >= MIDI_STATUS && event->status < MIDI_SYSTEM) {
            int type = event->status & MIDI_STATUS_MASK;
            event->status = type | route;

            if (v->follow) {
                continue;
            }

            if (type == MIDI_PROGRAM) {
                event->a = v->program == 128 ? 0 : v->program;
            } else if (type == MIDI_CONTROL &&
                       (event->a == MIDI_CC_BANK || event->a == MIDI_CC_BANK_LSB)) {
                event->b = 0;
            }
        }
    }
}

bool dm_pattern_schedule(const DmProject *p, const DmPatternPreview *v, DmSchedule *next) {
    if (!v || v->pattern < 0 || (size_t)v->pattern >= p->pattern_count || v->channel < 0 ||
        v->channel > 15 || v->program < -1 || v->program > 128 || !isfinite(v->length) ||
        v->length <= 0 || v->length > 10000000 || !isfinite(v->origin) || v->origin < 0) {
        return false;
    }

    DmProject view = *p;
    preview_channels(&view, v);
    DmClip clip = {0, v->pattern, v->channel, 0, v->length, 0};
    view.clips = &clip;
    view.clip_count = 1;
    *next = (DmSchedule){0};
    bool ok = preview_region(&view, p, v) && dm_schedule(&view, next);
    free(view.events);
    free(view.automation);

    if (ok) {
        preview_schedule_program(next, v);
        next->duration = dm_beat_seconds(p, v->length);
    }

    return ok;
}

static bool publish_pattern(DmAudio *a, const DmProject *p, const DmPatternPreview *v,
                            double seconds, bool playing, bool editing, char *error, size_t cap) {
    DmSchedule next;

    if (!a || !v) {
        return false;
    }

    if (!dm_pattern_schedule(p, v, &next)) {
        snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_PATTERN);
        return false;
    }

    return publish_schedule(a, p, v, next, seconds, playing, editing, error, cap);
}

bool dm_audio_publish_pattern(DmAudio *a, const DmProject *p, const DmPatternPreview *pattern,
                              double seconds, bool playing, char *error, size_t cap) {
    return publish_pattern(a, p, pattern, seconds, playing, false, error, cap);
}

bool dm_audio_update(DmAudio *a, const DmProject *p, const DmPatternPreview *pattern, char *error,
                     size_t cap) {
    if (!a) {
        return false;
    }

    double seconds = dm_audio_position(a);
    bool playing = dm_audio_playing(a);

    if (pattern) {
        return publish_pattern(a, p, pattern, seconds, playing, true, error, cap);
    }

    DmSchedule next = {0};

    if (!dm_schedule(p, &next)) {
        return false;
    }

    return publish_schedule(a, p, NULL, next, seconds, playing, true, error, cap);
}

void dm_audio_transport(DmAudio *a, bool playing) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->playing = playing;
    input_clock(a, true);
    a->input_reset = true;

    if (!playing) {
        a->reset = true;
    }

    SDL_SetAtomicInt(&a->playing_flag, playing);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

double dm_audio_position(DmAudio *a) {
    return a ? SDL_GetAtomicInt(&a->position_ms) / 1000.0 : 0;
}

void dm_audio_panic(DmAudio *a) {
    if (!a) {
        return;
    }

    dm_audio_sample_audition_stop(a);

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->playing = a->recording = a->reset = a->chase = false;
    input_clock(a, true);
    input_thru_stop(a);
    input_silence(a, NULL);
    SDL_SetAtomicInt(&a->playing_flag, 0);
    a->release_count = 0;
    a->release_end = 0;

    if (a->sample_audition.keyboard) {
        fluid_synth_all_sounds_off(a->sample_audition.voice.synth, 1);
        memset(a->sample_audition.song_held, 0, sizeof(a->sample_audition.song_held));
    }

    a->keyboard_tail = a->feedback_tail = a->audition_frames = a->click_frames = 0;
    reset_live_ports(a);
    a->feedback_key = -1;
    a->feedback_sample_id = 0;
    memset(a->note_sample_id, 0, sizeof(a->note_sample_id));
    memset(a->keyboard_sample_id, 0, sizeof(a->keyboard_sample_id));
    memset(a->note_token, 0, sizeof(a->note_token));
    memset(a->state.notes, 0, sizeof(a->state.notes));
    memset(a->state.held, 0, sizeof(a->state.held));
    memset(a->state.sostenuto, 0, sizeof(a->state.sostenuto));
    SDL_SetAtomicInt(&a->preview.read, SDL_GetAtomicInt(&a->preview.write));
    SDL_SetAtomicInt(&a->preview.panic, 0);
    SDL_SetAtomicInt(&a->feedback_queue.read, SDL_GetAtomicInt(&a->feedback_queue.write));
    SDL_SetAtomicInt(&a->feedback_queue.panic, 0);
    // The worker sends these after discarding pending note-ons, without device I/O on the GUI
    // thread.
    SDL_LockMutex(a->device_lock);
    SDL_SetAtomicInt(&a->midi.read, SDL_GetAtomicInt(&a->midi.write));
    SDL_SetAtomicInt(&a->midi.panic, 0);
    const int controls[] = {64, 66, 69, 123, 120, 121};

    for (int c = 0; c < 16; ++c) {
        for (size_t i = 0; i < sizeof(controls) / sizeof(*controls); ++i) {
            int cc = controls[i];
            fluid_synth_cc(a->live.synth, c, cc, 0);
            fluid_synth_cc(a->keyboard.synth, c, cc, 0);
            fluid_synth_cc(a->feedback.synth, c, cc, 0);
            queue_put(&a->midi, (Command){MIDI_CONTROL | c, cc, 0});
        }

        fluid_synth_pitch_bend(a->live.synth, c, MIDI_BEND_CENTER);
        fluid_synth_pitch_bend(a->keyboard.synth, c, MIDI_BEND_CENTER);
        fluid_synth_pitch_bend(a->feedback.synth, c, MIDI_BEND_CENTER);
        queue_put(&a->midi, (Command){MIDI_PITCH_BEND | c, 0, 64});
        a->keyboard_applied[c].volume = a->keyboard_applied[c].pan = -1;
        SDL_SetAtomicInt(&a->peaks[c], 0);
    }

    a->keyboard_config = true;
    keyboard_configure(a);
    SDL_UnlockMutex(a->device_lock);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

bool dm_audio_playing(DmAudio *a) {
    return a && SDL_GetAtomicInt(&a->playing_flag) != 0;
}

void dm_audio_edit_preview(DmAudio *a, int program, float pitch, bool on) {
    dm_audio_edit_preview_part(a, -1, program, pitch, on);
}

void dm_audio_edit_preview_part(DmAudio *a, int part, int program, float pitch, bool on) {
    if (!a) {
        return;
    }

    if (!on) {
        queue_put(&a->feedback_queue, (Command){255, 0, 0});
    } else if (program >= 0 && program <= 128 && isfinite(pitch) && pitch >= 0 && pitch <= 127) {
        int key = (int)floorf(pitch);
        int fraction = (int)roundf((pitch - key) * 128);
        queue_put(&a->feedback_queue,
                  (Command){.status = program, .a = key, .b = fraction,
                            .feedback_part = part >= 0 && part < DM_PARTS ? part + 1 : 0});
    }
}

void dm_audio_preview(DmAudio *a, int channel, int key, int velocity, bool on) {
    if (a && key >= 0 && key <= 127) {
        queue_put(&a->preview,
                  (Command){(on ? MIDI_NOTE_ON : MIDI_NOTE_OFF) | (channel & MIDI_CHANNEL_MASK),
                            key, velocity});
    }
}

void dm_audio_audition(DmAudio *a, int channel, int key, int velocity, double duration) {
    if (!a || key < 0 || key > 127 || !isfinite(duration) || duration <= 0 || duration > 10) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    if (a->audition_frames > 0) {
        dm_audio_preview(a, a->audition_channel, a->audition_key, 0, false);
    }

    dm_audio_preview(a, channel, key, velocity, true);
    a->audition_frames = (int)(duration * RATE);
    a->audition_channel = channel;
    a->audition_key = key;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

bool dm_audio_sample_audition_edit(DmAudio *a, const DmeSampleZoneParams *params) {
    if (!a || !a->stream) {
        return false;
    }

    SDL_LockAudioStream(a->stream);
    SampleAudition *p = &a->sample_audition;
    bool ok = p->active && !p->keyboard &&
              dm_sample_synth_edit(p->bank, p->voice.synth, p->preset, NULL, params);
    if (ok) {
        bool looping = params->loop_mode != DME_SAMPLE_NO_LOOP;
        p->repeating = p->sustained && !looping;

        if (p->looping != looping) {
            p->release_in = !p->sustained && looping ? RATE : 0;
            p->looping = looping;
        }
    }

    SDL_UnlockAudioStream(a->stream);

    for (bool pending = ok; pending;) {
        SDL_LockAudioStream(a->stream);
        pending = dm_sample_synth_rescale(a->sample_audition.bank, GAIN_SLICE_FRAMES);
        SDL_UnlockAudioStream(a->stream);
    }

    return ok;
}

void dm_audio_sample_edit(DmAudio *a, const char *locator, const char *set_id,
                          const char *preset_id, uint32_t region, const char *asset,
                          const DmeSampleZoneParams *params) {
    if (!a || !a->stream) {
        return;
    }

    SDL_LockAudioStream(a->stream);

    for (int bank = 0; bank < LIVE_SAMPLE_BANKS; ++bank) {
        DmLiveSampleBank *b = a->sample_banks[bank];
        dm_live_sample_bank_edit(b, a->live.synth, locator, set_id, preset_id, region, asset,
                                 params);
        dm_live_sample_bank_edit(b, a->keyboard.synth, locator, set_id, preset_id, region, asset,
                                 params);
        dm_live_sample_bank_edit(b, a->feedback.synth, locator, set_id, preset_id, region, asset,
                                 params);

        for (unsigned port = 1; port < DM_NOTE_PORTS; ++port) {
            if (a->port_live[port]) {
                dm_live_sample_bank_edit(b, a->port_live[port]->synth, locator, set_id, preset_id,
                                         region, asset, params);
            }
        }
    }

    SDL_UnlockAudioStream(a->stream);

    for (bool pending = true; pending;) {
        pending = false;
        SDL_LockAudioStream(a->stream);

        for (int bank = 0; bank < LIVE_SAMPLE_BANKS; ++bank) {
            pending |= dm_live_sample_bank_rescale(a->sample_banks[bank], GAIN_SLICE_FRAMES);
        }

        SDL_UnlockAudioStream(a->stream);
    }
}

bool dm_audio_sample_audition_loop(DmAudio *a, uint64_t start, uint64_t end) {
    if (!a || !a->stream) {
        return false;
    }

    SDL_LockAudioStream(a->stream);
    SampleAudition *preview = &a->sample_audition;
    bool ok = preview->active && !preview->keyboard &&
              dm_sample_synth_loop(preview->bank, preview->voice.synth, start, end);
    SDL_UnlockAudioStream(a->stream);
    return ok;
}

static void audition_swap(DmAudio *a, SampleAudition next) {
    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    SampleAudition old = a->sample_audition;
    a->sample_audition = next;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    synth_free(&old.voice);
    dm_sample_synth_free(old.bank);
}

void dm_audio_sample_audition_stop(DmAudio *a) {
    if (a) {
        audition_swap(a, (SampleAudition){0});
    }
}

bool dm_audio_instrument_preview(DmAudio *a, const DmPatchReplacement *r, int route, char *error,
                                 size_t cap) {
    if (!a || !a->stream || !r || route < 0 || route >= 16 || r->key.port >= DM_NOTE_PORTS) {
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_AUDITION);
        return false;
    }

    SampleAudition next = {.active = true,
                           .keyboard = true,
                           .keyboard_route = route,
                           .song_part = r->key.port * 16 + route,
                           .key = r->key.drums ? 36 : 60,
                           .release_in = RATE / 3};
    DmSampleExportSnapshot *snapshot = NULL;
    DmeContext context = {0};
    uint32_t preset = 0;
    DmProject project = {.patch_replacements = (DmPatchReplacement *)r,
                         .patch_replacement_count = 1};
    bool prepared =
        !r->generation || dm_sample_playback_snapshot(&project, &context, NULL, &preset, &snapshot);

    if (prepared && preset) {
        next.preset = preset;
        const DmeSamplePreset *view = &context.sample_presets[preset - 1];
        const DmeSampleZoneParams *zone = &context.sample_zones[view->first_zone].params;
        next.key = r->key.drums ? (int)zone->key_lo : (int)zone->root;
        next.key = (int)fmin(zone->key_hi, fmax(zone->key_lo, next.key));
        next.bank = dm_sample_synth_create(&context, snapshot, NULL);
        prepared = next.bank != NULL;
    }

    dm_sample_export_free(snapshot);

    if (!prepared || !synth_create(&next.voice, next.bank ? NULL : a->font_path)) {
        dm_sample_synth_free(next.bank);
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_AUDITION);
        return false;
    }

    if (!next.bank) {
        fluid_synth_set_channel_type(next.voice.synth, 0,
                                     r->key.drums ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        fluid_synth_bank_select(next.voice.synth, 0,
                                r->key.drums && !r->key.bank ? 128 : r->key.bank);
        fluid_synth_program_change(next.voice.synth, 0, r->key.program);
        fluid_synth_noteon(next.voice.synth, 0, next.key, 100);
    } else {
        sample_audition_start(&next);
    }

    if (!next.bank) {
        fluid_synth_set_channel_type(next.voice.synth, 1,
                                     r->key.drums ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        fluid_synth_bank_select(next.voice.synth, 1,
                                r->key.drums && !r->key.bank ? 128 : r->key.bank);
        fluid_synth_program_change(next.voice.synth, 1, r->key.program);
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    int part = next.song_part;
    fluid_synth_cc(next.voice.synth, 1, MIDI_CC_VOLUME,
                   a->mix_volume[part] < 128 ? a->mix_volume[part] : 100);
    fluid_synth_cc(next.voice.synth, 1, MIDI_CC_PAN,
                   a->mix_pan[part] < 128 ? a->mix_pan[part] : 64);
    static const unsigned controls[] = {MIDI_CC_MODULATION, MIDI_CC_EXPRESSION, MIDI_CC_SUSTAIN,
                                        MIDI_CC_SOSTENUTO};

    for (unsigned i = 0; i < SDL_arraysize(controls); ++i) {
        unsigned value = a->state.cc[part][controls[i]];

        if (value < 128) {
            send_route(&next.voice, MIDI_CONTROL | 1, (uint8_t)controls[i], (uint8_t)value);
        }
    }

    if (a->state.bend[part][0] < 128 && a->state.bend[part][1] < 128) {
        fluid_synth_pitch_bend(next.voice.synth, 1,
                               a->state.bend[part][0] | a->state.bend[part][1] << 7);
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    audition_swap(a, next);
    return true;
}

static bool audition_begin(DmAudio *a, DmSampleSynth *bank, unsigned preset, int key,
                            bool sustained, bool looping, char *error, size_t cap) {
    SampleAudition next = {.bank = bank,
                           .preset = preset,
                           .key = key,
                           .active = true,
                           .release_in = !sustained && looping ? RATE : 0,
                           .repeating = sustained && !looping,
                           .sustained = sustained,
                           .looping = looping};

    if (!a || !a->stream || !bank || !synth_create(&next.voice, NULL)) {
        dm_sample_synth_free(bank);
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_AUDITION);
        return false;
    }

    sample_audition_start(&next);
    audition_swap(a, next);
    return true;
}

bool dm_audio_sample_audition(DmAudio *a, const DmeContext *context, const DmeHost *host, int key,
                              bool sustained, bool looping, char *error, size_t cap) {
    DmSampleSynth *bank = dm_sample_synth_create(context, NULL, host);
    return audition_begin(a, bank, 1, key, sustained, looping, error, cap);
}

void dm_audio_program(DmAudio *a, int channel, int program) {
    if (a) {
        queue_put(&a->preview, (Command){MIDI_PROGRAM | (channel & MIDI_CHANNEL_MASK),
                                         program & MIDI_DATA_MASK, 0});
    }
}

void dm_audio_keyboard_program(DmAudio *a, int channel, int program, bool drums) {
    if (a && channel >= 0 && channel < 16 && program >= -1 && program <= 127) {
        /* Keyboard override commands use the port byte to carry the drum-bank choice. */
        queue_put(&a->preview, (Command){.status = 0xf1,
                                         .a = channel,
                                         .b = program < 0 ? 255 : program,
                                         .port = drums});
    }
}

void dm_audio_control(DmAudio *a, int channel, int controller, int value) {
    if (a) {
        queue_put(&a->preview, (Command){MIDI_CONTROL | (channel & MIDI_CHANNEL_MASK),
                                         controller & MIDI_DATA_MASK, value & MIDI_DATA_MASK});
    }
}

void dm_audio_keyboard_mix(DmAudio *a, int route, int part, int volume, int pan) {
    if (!a || route < 0 || route > 15 || part < 0 || part >= DM_PARTS) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->keyboard_parts[route] = part;
    a->keyboard_channels[route].volume = volume;
    a->keyboard_channels[route].pan = pan;
    a->preview_mask |= (uint16_t)(1u << route);
    a->keyboard_config = true;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_part_control(DmAudio *a, int port, int channel, int controller, int value) {
    if (a && port >= 0 && port < DM_NOTE_PORTS && channel >= 0 && channel < 16) {
        queue_put(&a->preview, (Command){.status = (uint8_t)(MIDI_CONTROL | channel),
                                         .a = (uint8_t)controller,
                                         .b = (uint8_t)value,
                                         .port = (uint8_t)port,
                                         .live_only = true});
    }
}

bool dm_audio_mix_state(DmAudio *a, uint8_t volume[DM_PARTS], uint8_t pan[DM_PARTS]) {
    if (!a || !SDL_GetAtomicInt(&a->playing_flag)) {
        return false;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    for (int part = 0; part < DM_PARTS; ++part) {
        volume[part] = a->mix_volume[part];
        pan[part] = a->mix_pan[part];
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    return true;
}

float dm_audio_peak(DmAudio *a, int c) {
    return a && c >= 0 && c < DM_PARTS ? SDL_SetAtomicInt(&a->peaks[c], 0) / 10000.0f : 0;
}

void dm_audio_analyzer_enable(DmAudio *a, bool enabled) {
    if (!a || SDL_GetAtomicInt(&a->analyzer_enabled) == enabled) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->analyzer_cursor = a->analyzer_count = 0;
    SDL_SetAtomicInt(&a->analyzer_enabled, enabled);

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

bool dm_audio_analyzer_read(DmAudio *a, float *stereo) {
    if (!a) {
        return false;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    bool ready = SDL_GetAtomicInt(&a->analyzer_enabled) && a->analyzer_count == DM_SPECTRUM_FRAMES;

    if (ready) {
        int tail = DM_SPECTRUM_FRAMES - a->analyzer_cursor;
        memcpy(stereo, a->analyzer_samples + a->analyzer_cursor * 2,
               (size_t)tail * 2 * sizeof(float));
        memcpy(stereo + tail * 2, a->analyzer_samples,
               (size_t)a->analyzer_cursor * 2 * sizeof(float));
    }

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }

    return ready;
}

const char *dm_audio_driver(DmAudio *a) {
    return a && a->stream ? SDL_GetCurrentAudioDriver() : DM_TEXT_UNAVAILABLE;
}

int dm_audio_buffer_frames(DmAudio *a) {
    int frames = 0;

    if (a && a->stream) {
        SDL_AudioSpec spec;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->stream), &spec, &frames);
    }

    return frames;
}

double dm_audio_buffer_ms(DmAudio *a) {
    int frames = 0;
    SDL_AudioSpec spec = {0};

    if (a && a->stream) {
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->stream), &spec, &frames);
    }

    return spec.freq > 0 ? frames * 1000.0 / spec.freq : 0;
}

bool dm_audio_retry(DmAudio *a, char *error, size_t cap) {
    if (!a) {
        return false;
    }

    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        snprintf(error, cap, DM_TEXT_AUDIO_INITIALIZATION_FAILED, SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    SDL_AudioStream *next =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, a);

    if (!next) {
        snprintf(error, cap, DM_TEXT_AUDIO_RETRY_FAILED, SDL_GetError());
        return false;
    }

    if (a->stream) {
        SDL_DestroyAudioStream(a->stream);
    }

    a->stream = next;

    if (a->input_latency_auto) {
        dm_audio_input_offset(a, NAN);
    }

    SDL_ResumeAudioStreamDevice(next);
    return true;
}

void dm_audio_cancel(DmAudio *a) {
    if (a) {
        SDL_SetAtomicInt(&a->cancel, 1);
    }
}

void dm_audio_metronome(DmAudio *a, bool enabled, double bpm) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->metronome = enabled;
    a->click_bpm = bpm;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_monitor(DmAudio *a, bool enabled) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->monitor = enabled;
    a->reset = true;
    a->keyboard_panic = true;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

void dm_audio_recording(DmAudio *a, bool enabled) {
    if (!a) {
        return;
    }

    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }

    a->recording = enabled;

    if (a->stream) {
        SDL_UnlockAudioStream(a->stream);
    }
}

int dm_audio_device_count(DmAudio *a) {
    if (!a || !a->device) {
        return 0;
    }

    SDL_LockMutex(a->device_lock);
    int n = (int)rtmidi_get_port_count(a->device);
    SDL_UnlockMutex(a->device_lock);
    return n;
}

void dm_audio_device_name(DmAudio *a, int i, char *name, size_t cap) {
    if (!a) {
        return;
    }

    SDL_LockMutex(a->device_lock);
    int n = (int)cap;
    rtmidi_get_port_name(a->device, (unsigned)i, name, &n);
    SDL_UnlockMutex(a->device_lock);
    name[cap - 1] = 0;
}

bool dm_audio_device(DmAudio *a, int i, char *error, size_t cap) {
    if (!a || !a->device) {
        return false;
    }

    SDL_LockMutex(a->device_lock);

    if (a->device_index >= 0) {
        for (int c = 0; c < 16; ++c) {
            uint8_t message[] = {(uint8_t)(MIDI_CONTROL | c), MIDI_CC_SOUND_OFF, 0};
            rtmidi_out_send_message(a->device, message, 3);
        }
    }

    rtmidi_close_port(a->device);
    a->device_index = -1;

    if (i >= 0) {
        rtmidi_open_port(a->device, (unsigned)i, DM_TEXT_DUCKMIDI_OUTPUT);
    }

    bool ok = i < 0 || a->device->ok;

    if (ok) {
        a->device_index = i;
    } else {
        snprintf(error, cap, DM_TEXT_MIDI_DEVICE, a->device->msg);
    }

    SDL_UnlockMutex(a->device_lock);
    return ok;
}

static int32_t DME_CALL render_canceled(void *user) {
    DmAudio *a = user;
    return SDL_GetAtomicInt(&a->cancel);
}

static void DME_CALL render_sample_warning(void *user, uint32_t severity, uint32_t flags,
                                          const char *message) {
    (void)flags;
    (void)message;
    DmAudio *a = user;
    a->render_missing_samples += severity == DME_WARNING;
}

static bool prepare_render_samples(DmAudio *a, DmeContext *context,
                                   DmSampleExportSnapshot **snapshot, DmSampleSynth **synth) {
    if (!a->render_project.size) {
        return true;
    }

    DmProject project;
    dm_project_init(&project);
    size_t count = a->render_schedule.count;
    DmeEvent *events = count ? calloc(count, sizeof(*events)) : NULL;
    bool ok = (!count || events) &&
              dm_unpack(&project, a->render_project.data, a->render_project.size,
                        a->job_message, sizeof(a->job_message));

    if (ok) {
        for (size_t i = 0; i < count; ++i) {
            const DmScheduled *event = &a->render_schedule.items[i];
            events[i] = (DmeEvent){.source_id = i + 1, .status = event->status, .a = event->a,
                                   .b = event->b, .port = event->port};
        }

        *context = (DmeContext){.events = events, .event_count = count};
        DmeHost host = {.user = a, .cancelled = render_canceled, .diagnostic = render_sample_warning};
        ok = dm_sample_export_snapshot(&project, context, &host, snapshot);

        if (ok && context->sample_preset_count) {
            *synth = dm_sample_synth_create(context, *snapshot, &host);
            ok = *synth != NULL;
        }
    }

    context->events = NULL;
    free(events);
    dm_project_free(&project);
    dm_bytes_free(&a->render_project);

    if (!ok && !*a->job_message) {
        snprintf(a->job_message, sizeof(a->job_message), "%s", DM_TEXT_SAMPLE_AUDIO_PREPARE);
    }

    return ok;
}

static bool audio_render(DmAudio *a) {
    Synth s = {0};
    Synth *ports[DM_NOTE_PORTS] = {0};

    if (!synth_create(&s, a->font_path)) {
        snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_CANNOT_LOAD_SOUNDFONT_AUDIO);
        return false;
    }

    uint64_t frames = (uint64_t)ceil((a->render_schedule.duration + 2) * RATE);

    if (a->render_options.codec == DM_AUDIO_WAV &&
        frames > (UINT32_MAX - 256) / (2 * (a->render_options.bits / 8))) {
        synth_free(&s);
        snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_WAV_EXCEEDS_RIFF_SIZE);
        return false;
    }

    bool ok = true;

    for (size_t i = 0; i < a->render_schedule.count; i++) {
        unsigned port = a->render_schedule.items[i].port;

        if (!port || ports[port]) {
            continue;
        }

        ports[port] = calloc(1, sizeof(Synth));

        if (!ports[port] || !synth_create(ports[port], a->font_path)) {
            snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_CANNOT_PREPARE_NOTE_PORTS);
            ok = false;
            break;
        }

        if (SDL_GetAtomicInt(&a->cancel)) {
            ok = false;
            break;
        }
    }

    DmOutput output = {0};
    DmAudioEncoder *encoder = NULL;
    DmeContext sample_context = {0};
    DmSampleExportSnapshot *snapshot = NULL;
    DmSampleSynth *sample_synth = NULL;
    ok = ok && prepare_render_samples(a, &sample_context, &snapshot, &sample_synth);

    if (ok) {
        ok = dm_output_begin(&output, a->job_path, a->job_message, sizeof(a->job_message));
    }

    if (ok) {
        encoder = dm_audio_encoder_begin(&output, &a->render_options, a->job_message,
                                         sizeof(a->job_message));
        ok = encoder != NULL;
    }

    size_t cursor = 0;
    uint64_t pos = 0;

    while (ok && pos < frames) {
        if (SDL_GetAtomicInt(&a->cancel)) {
            ok = false;
            break;
        }

        while (cursor < a->render_schedule.count &&
               a->render_schedule.items[cursor].seconds * RATE <= pos + .5) {
            size_t index = cursor++;
            const DmScheduled *event = &a->render_schedule.items[index];
            Synth *route = event->port ? ports[event->port] : &s;

            if (!sample_synth ||
                !dm_sample_synth_note(sample_synth, route->synth, &route->sample_pedals, event,
                                     sample_context.effective_patches[index])) {
                send_event(route, event);
            }
        }

        int n = (int)fmin(BLOCK, (double)(frames - pos));

        if (cursor < a->render_schedule.count) {
            int until = (int)ceil(a->render_schedule.items[cursor].seconds * RATE - pos);

            if (until > 0 && until < n) {
                n = until;
            }
        }

        float sound[BLOCK * 2];
        render(&s, sound, n, NULL);

        for (unsigned port = 1; port < DM_NOTE_PORTS; port++) {
            if (ports[port]) {
                render_port_mix(ports[port], sound, n, NULL);
            }
        }

        ok = dm_audio_encoder_write(encoder, sound, n);
        pos += (unsigned)n;
        SDL_SetAtomicInt(&a->progress, (int)(pos * 100 / frames));
    }

    if (SDL_GetAtomicInt(&a->cancel)) {
        snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_AUDIO_EXPORT_CANCELED);
        ok = false;
    }

    ok = dm_audio_encoder_finish(encoder, ok) && ok;

    if (output.file) {
        ok = dm_output_finish(&output, ok, a->job_message, sizeof(a->job_message));
    }

    free_port_synths(ports);
    synth_free(&s);
    dm_sample_synth_free(sample_synth);
    dm_sample_export_free(snapshot);
    return ok;
}

static int job_worker(void *userdata) {
    DmAudio *a = userdata;

    if (a->job_type == 1) {
        a->job_success = synth_create(&a->prepared, a->job_path) &&
                         synth_create(&a->prepared_keyboard, a->job_path) &&
                         synth_create(&a->prepared_feedback, a->job_path);

        for (unsigned p = 1; p < DM_NOTE_PORTS && a->job_success; p++) {
            if (a->job_ports[p]) {
                if (SDL_GetAtomicInt(&a->cancel)) {
                    a->job_success = false;
                    break;
                }

                a->prepared_ports[p] = calloc(1, sizeof(Synth));
                a->job_success =
                    a->prepared_ports[p] && synth_create(a->prepared_ports[p], a->job_path);
            }
        }

        if (SDL_GetAtomicInt(&a->cancel)) {
            a->job_success = false;
        }

        if (!a->job_success) {
            synth_free(&a->prepared);
            synth_free(&a->prepared_keyboard);
            synth_free(&a->prepared_feedback);
            free_port_synths(a->prepared_ports);
        }

        snprintf(a->job_message, sizeof(a->job_message), "%s",
                 a->job_success ? DM_TEXT_SOUNDFONT_READY : DM_TEXT_UNABLE_LOAD_SOUNDFONT);
    } else {
        a->job_success = audio_render(a);

        if (a->job_success && a->render_missing_samples) {
            snprintf(a->job_message, sizeof(a->job_message), DM_TEXT_SAMPLE_AUDIO_MISSING,
                     dm_audio_codec_name(a->render_options.codec), a->render_missing_samples);
        } else if (a->job_success) {
            snprintf(a->job_message, sizeof(a->job_message), "%s export complete",
                     dm_audio_codec_name(a->render_options.codec));
        }
    }

    SDL_SetAtomicInt(&a->job_done, 1);
    return 0;
}

static bool job_start(DmAudio *a, const char *path, int type, char *error, size_t cap) {
    if (!a || SDL_GetAtomicInt(&a->job_busy)) {
        snprintf(error, cap, DM_TEXT_BACKGROUND_OPERATION_ALREADY);
        return false;
    }

    snprintf(a->job_path, sizeof(a->job_path), "%s", path);
    a->job_type = type;

    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        a->job_ports[p] = a->port_live[p] != NULL;
    }

    a->job_success = false;
    a->job_message[0] = 0;
    SDL_SetAtomicInt(&a->cancel, 0);
    SDL_SetAtomicInt(&a->job_done, 0);
    SDL_SetAtomicInt(&a->progress, 0);
    SDL_SetAtomicInt(&a->job_busy, 1);
    a->worker =
        SDL_CreateThread(job_worker, type == 1 ? DM_TEXT_LOAD_SOUNDFONT : DM_TEXT_RENDER_AUDIO, a);

    if (!a->worker) {
        SDL_SetAtomicInt(&a->job_busy, 0);
        snprintf(error, cap, DM_TEXT_CANNOT_START_BACKGROUND);
        return false;
    }

    return true;
}

bool dm_audio_soundfont(DmAudio *a, const char *path, char *error, size_t cap) {
    return job_start(a, path, 1, error, cap);
}

bool dm_audio_busy(DmAudio *a) {
    return a && SDL_GetAtomicInt(&a->job_busy) != 0;
}

bool dm_audio_poll(DmAudio *a, char *message, size_t cap, bool *success, int *kind) {
    if (!a) {
        return false;
    }

    collect_sample_banks(a);

    if (*a->sample_warning) {
        if (kind) {
            *kind = 0;
        }

        *success = true;
        snprintf(message, cap, "%s", a->sample_warning);
        *a->sample_warning = 0;
        return true;
    }

    if (SDL_SetAtomicInt(&a->port_warning, 0)) {
        if (kind) {
            *kind = 0;
        }

        *success = true;
        snprintf(message, cap, DM_TEXT_INDEPENDENT_NOTE_PORTS_SHARE);
        return true;
    }

    if (SDL_SetAtomicInt(&a->midi_failed, 0)) {
        if (kind) {
            *kind = 0;
        }

        snprintf(message, cap, DM_TEXT_EXTERNAL_MIDI_DEVICE);
        *success = false;
        return true;
    }

    if (!SDL_GetAtomicInt(&a->job_done)) {
        return false;
    }

    SDL_WaitThread(a->worker, NULL);

    if (kind) {
        *kind = a->job_type;
    }

    a->worker = NULL;

    if (a->job_type == 1 && a->job_success) {
        if (a->stream) {
            SDL_LockAudioStream(a->stream);
        }

        Synth old = a->live;
        Synth old_keyboard = a->keyboard;
        Synth old_feedback = a->feedback;
        Synth *old_ports[DM_NOTE_PORTS] = {0};

        for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
            old_ports[p] = a->port_live[p];
            a->port_live[p] = a->prepared_ports[p];
            a->prepared_ports[p] = NULL;
        }

        a->live = a->prepared;
        a->keyboard = a->prepared_keyboard;
        a->feedback = a->prepared_feedback;
        a->feedback_key = -1;
        a->feedback_sample_id = 0;
        memset(a->keyboard_sample_id, 0, sizeof(a->keyboard_sample_id));
        a->feedback_tail = 0;
        memset(&a->prepared, 0, sizeof(a->prepared));
        memset(&a->prepared_keyboard, 0, sizeof(a->prepared_keyboard));
        memset(&a->prepared_feedback, 0, sizeof(a->prepared_feedback));

        for (int c = 0; c < 16; ++c) {
            a->keyboard_applied[c].program = -1;
            a->keyboard_applied[c].volume = -1;
            a->keyboard_applied[c].pan = -1;
        }

        a->keyboard_config = true;
        a->keyboard_tail = 0;
        a->reset = true;

        if (a->stream) {
            SDL_UnlockAudioStream(a->stream);
        }

        synth_free(&old);
        synth_free(&old_keyboard);
        synth_free(&old_feedback);
        free_port_synths(old_ports);
        snprintf(a->font_path, sizeof(a->font_path), "%s", a->job_path);
    }

    dm_schedule_free(&a->render_schedule);
    dm_bytes_free(&a->render_project);
    snprintf(message, cap, "%s", a->job_message);
    *success = a->job_success;
    SDL_SetAtomicInt(&a->job_done, 0);
    SDL_SetAtomicInt(&a->progress, 0);
    SDL_SetAtomicInt(&a->job_busy, 0);
    return true;
}

bool dm_audio_export(DmAudio *a, const DmProject *p, const char *path,
                     const DmAudioOptions *options, char *error, size_t cap) {
    DmAudioOptions chosen = options ? *options : dm_audio_options_default();

    if (!dm_audio_options_valid(&chosen)) {
        snprintf(error, cap, DM_TEXT_INVALID_AUDIO_EXPORT_SETTINGS);
        return false;
    }

    if (!a || !*p->soundfont) {
        snprintf(error, cap, DM_TEXT_CHOOSE_SOUNDFONT_IN_PREFERENCES);
        return false;
    }

    if (dm_audio_busy(a)) {
        snprintf(error, cap, DM_TEXT_BACKGROUND_OPERATION_ALREADY);
        return false;
    }

    if (!dm_schedule(p, &a->render_schedule)) {
        return false;
    }

    if (p->patch_replacement_count && !dm_pack(p, &a->render_project)) {
        dm_schedule_free(&a->render_schedule);
        snprintf(error, cap, "%s", DM_TEXT_SAMPLE_AUDIO_PREPARE);
        return false;
    }

    a->render_missing_samples = 0;
    a->render_options = chosen;
    snprintf(a->font_path, sizeof(a->font_path), "%s", p->soundfont);

    if (job_start(a, path, 2, error, cap)) {
        return true;
    }

    dm_schedule_free(&a->render_schedule);
    dm_bytes_free(&a->render_project);
    return false;
}

bool dm_audio_exporting(DmAudio *a) {
    return a && a->job_type == 2 && SDL_GetAtomicInt(&a->job_busy) != 0;
}

int dm_audio_export_progress(DmAudio *a) {
    return a ? SDL_GetAtomicInt(&a->progress) : 0;
}
