#include "audio.h"
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
typedef struct {
    uint8_t status, a, b;
    uint8_t port;
    bool live_only;
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
} Synth;
typedef struct {
    uint8_t cc[DM_PARTS][128], program[DM_PARTS], bend[DM_PARTS][2], notes[DM_PARTS][128];
    uint8_t pressure[DM_PARTS], poly[DM_PARTS][128];
    uint8_t held[DM_PARTS][128], sostenuto[DM_PARTS][128];
} ChaseState;
typedef struct {
    double seconds;
    uint16_t channel;
    uint8_t key;
    uint64_t token;
} NoteRelease;
typedef struct RetiredSchedule {
    DmSchedule schedule;
    struct RetiredSchedule *next;
} RetiredSchedule;
struct DmAudio {
    SDL_AudioStream *stream;
    Synth live, keyboard, feedback;
    Synth *port_live[DM_NOTE_PORTS],*prepared_ports[DM_NOTE_PORTS];
    bool used_ports[DM_NOTE_PORTS],job_ports[DM_NOTE_PORTS];
    DmChannel keyboard_channels[16], keyboard_applied[16];
    int keyboard_override[16];
    bool keyboard_config, keyboard_panic;
    int keyboard_tail;
    DmSchedule schedule;
    NoteRelease *releases;
    size_t release_count, release_capacity;
    double release_end, seek_end[DM_PARTS][128], loop_note_end[DM_PARTS][128];
    uint64_t note_token[DM_PARTS][128], token_serial;
    int live_program[DM_PARTS];
    RetiredSchedule *retired;
    size_t cursor, beat_cursor;
    double position;
    bool playing, reset, metronome, monitor, recording;
    double click_bpm;
    int click_frames;
    double click_phase;
    double freewheel_click, freewheel_rate;
    SDL_AtomicInt position_ms, playing_flag, peaks[DM_PARTS], quit, job_done, job_busy, midi_failed,port_warning;
    bool port_warning_sent;
    SDL_AtomicInt analyzer_enabled;
    float analyzer_samples[DM_SPECTRUM_FRAMES * 2];
    int analyzer_cursor, analyzer_count;
    CommandQueue preview, midi, feedback_queue;
    int feedback_key, feedback_program, feedback_fraction, feedback_tail;
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
    SDL_AtomicInt progress, cancel;
    ChaseState state, loop_state;
    double loop_start, loop_end;
    bool loop_enabled, loop_wait;
    size_t loop_cursor, loop_beat_cursor;
    uint16_t preview_mask;
    bool chase;
};
static void free_retired(DmAudio *a);
static void free_port_synths(Synth **ports);
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
    int c = status & 15;
    switch (status & 0xf0) {
    case 0x80:
        fluid_synth_noteoff(s, c, a);
        break;
    case 0x90:
        if (b) {
            fluid_synth_noteon(s, c, a, b);
        } else {
            fluid_synth_noteoff(s, c, a);
        }
        break;
    case 0xa0:
        fluid_synth_key_pressure(s, c, a, b);
        break;
    case 0xb0:
        fluid_synth_cc(s, c, a, b);
        break;
    case 0xc0:
        fluid_synth_program_change(s, c, a);
        break;
    case 0xd0:
        fluid_synth_channel_pressure(s, c, a);
        break;
    case 0xe0:
        fluid_synth_pitch_bend(s, c, a | (b << 7));
        break;
    }
}
static Synth *live_port(DmAudio *a,unsigned port){return port? a->port_live[port]:&a->live;}
static void reset_live_ports(DmAudio *a) {
    fluid_synth_system_reset(a->live.synth);
    for(unsigned p=1;p<DM_NOTE_PORTS;p++)if(a->port_live[p])fluid_synth_system_reset(a->port_live[p]->synth);
}
static void copy_part_rows(void *destination,const void *source,size_t row,const bool *used) {
    for(unsigned p=0;p<DM_NOTE_PORTS;p++)if(used[p])memcpy((uint8_t *)destination+p*16*row,(const uint8_t *)source+p*16*row,16*row);
}
static void copy_chase(ChaseState *dst,const ChaseState *src,const bool *used) {
#define PART_COPY(field) copy_part_rows(dst->field,src->field,sizeof(dst->field[0]),used)
    PART_COPY(cc);PART_COPY(program);PART_COPY(bend);PART_COPY(notes);PART_COPY(pressure);PART_COPY(poly);PART_COPY(held);PART_COPY(sostenuto);
#undef PART_COPY
}
static void send_live(DmAudio *a,int part, uint8_t status, uint8_t first, uint8_t second) {
    if ((status & 0xf0) == 0xc0) {
        a->live_program[part] = first;
    }
    if (a->monitor) {
        Synth *s=live_port(a,(unsigned)part/16);if(s)send_synth(s->synth, status, first, second);
    }
}
static void send_event(Synth *s, const DmScheduled *e) {
    if (e->status < 0xf0) {
        send_synth(s->synth, e->status, e->a, e->b);
        return;
    }
    if (e->status == 0xf0 && e->sysex.size) {
        size_t n = e->sysex.size;
        if (e->sysex.data[n - 1] == 0xf7) {
            --n;
        }
        fluid_synth_sysex(s->synth, (const char *)e->sysex.data, (int)n, NULL, NULL, NULL, 0);
    }
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
            send_live(a,item.channel, 0x80 | (item.channel&15), item.key, 0);
            queue_put(&a->midi, (Command){0x80 | (item.channel&15), item.key, 0});
        }
    }
}
static double playback_end(DmAudio *a) {
    return fmax(a->schedule.duration, a->release_end);
}
static void silence(DmAudio *a) {
    a->release_count = 0;
    a->release_end = 0;
    memset(a->live_program, 255, sizeof(a->live_program));
    for (int c = 0; c < 16; ++c) {
        fluid_synth_all_sounds_off(a->live.synth, c);
        for(unsigned p=1;p<DM_NOTE_PORTS;p++)if(a->port_live[p])fluid_synth_all_sounds_off(a->port_live[p]->synth,c);
        queue_put(&a->midi, (Command){0xb0 | c, 120, 0});
    }
}
static bool parameter_control(int cc) {
    switch (cc) {
    case 0:
    case 32:
    case 6:
    case 38:
    case 96:
    case 97:
    case 98:
    case 99:
    case 100:
    case 101:
        return true;
    default:
        return cc >= 120;
    }
}
static void replay_event(DmAudio *a, const DmScheduled *event) {
    if ((event->status & 0xf0) == 0xc0) {
        a->live_program[event->port*16+(event->status & 15)] = event->a;
    }
    if (a->monitor) {
        Synth *s=live_port(a,event->port);if(s)send_event(s,event);
    }
    if (event->status < 0xf0) {
        queue_put(&a->midi, (Command){event->status, event->a, event->b});
    } else {
        queue_system(&a->midi, event);
    }
}
static void initial_state(DmAudio *a) {
    for (size_t i = 0; i < a->schedule.count && a->schedule.items[i].seconds == 0; ++i) {
        const DmScheduled *e = &a->schedule.items[i];
        int type = e->status & 0xf0;
        if (type != 0x80 && type != 0x90 && type != 0xa0) {
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
        int type = e->status & 0xf0;
        if (e->status >= 0xf0 || type == 0xc0 || (type == 0xb0 && parameter_control(e->a))) {
            replay_event(a, e);
        }
    }
}
static void chase_voice_on(DmAudio *a, int c, int key) {
    Command on = {0x90 | (c&15), key, a->state.notes[c][key]};
    send_live(a,c, on.status, on.a, on.b);
    if (a->state.held[c][key]) {
        a->note_token[c][key] = ++a->token_serial;
        release_push(a, (NoteRelease){a->seek_end[c][key], c, key, a->token_serial});
    }
    queue_put(&a->midi, on);
    if (a->state.poly[c][key] != 255) {
        Command at = {0xa0 | (c&15), key, a->state.poly[c][key]};
        send_live(a,c, at.status, at.a, at.b);
        queue_put(&a->midi, at);
    }
}
static void chase_sostenuto(DmAudio *a, int c, int value) {
    send_live(a,c, 0xb0 | (c&15), 66, value);
    queue_put(&a->midi, (Command){0xb0 | c, 66, value});
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
                Command off = {0x80 | (c&15), key, 0};
                send_live(a,c, off.status, off.a, 0);
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
        if(!a->used_ports[c/16])continue;
        for (int cc = 0; cc < 120; ++cc) {
            if (a->state.cc[c][cc] != 255 && !parameter_control(cc) && cc != 66) {
                Command cmd = {0xb0 | (c&15), (uint8_t)cc, a->state.cc[c][cc]};
                send_live(a,c, cmd.status, cmd.a, cmd.b);
                queue_put(&a->midi, cmd);
            }
        }
        Command bend = {0xe0 | (c&15), a->state.bend[c][0], a->state.bend[c][1]};
        send_live(a,c, bend.status, bend.a, bend.b);
        queue_put(&a->midi, bend);
        if (a->state.pressure[c] != 255) {
            Command pressure = {0xd0 | (c&15), a->state.pressure[c], 0};
            send_live(a,c, pressure.status, pressure.a, 0);
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
static void render_port_mix(Synth *s,float *out,int frames,SDL_AtomicInt *peaks) {
    if(fluid_synth_get_active_voice_count(s->synth))s->tail=RATE*2;
    if(s->tail<=0)return;
    float extra[BLOCK*2];render(s,extra,frames,peaks);
    for(int i=0;i<frames*2;i++)out[i]=fmaxf(-1,fminf(1,out[i]+extra[i]));
    s->tail-=frames;
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
static bool dispatch_note(DmAudio *a, const DmScheduled *event) {
    int type = event->status & 0xf0;
    if (type == 0x80 || (type == 0x90 && !event->b)) {
        return false;
    }
    if (type == 0xc0) {
        a->live_program[event->port*16+(event->status & 15)] = event->a;
    }
    if (type == 0x90) {
        int channel = event->port*16+(event->status & 15);
        if (a->live_program[channel] != event->note_program) {
            send_live(a,channel, 0xc0 | (channel&15), event->note_program, 0);
            queue_put(&a->midi, (Command){0xc0 | (channel&15), event->note_program, 0});
        }
        a->note_token[channel][event->a] = ++a->token_serial;
        release_push(a, (NoteRelease){event->note_end, channel, event->a, a->token_serial});
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
        if (a->monitor) {
            Synth *s=live_port(a,event->port);if(s)send_event(s,event);
        }
        if (event->status < 0xf0) {
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
        queue_put(&a->midi, (Command){0xb0 | c, 121, 0});
    }
    a->position = 0;
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
            queue_put(&a->midi, (Command){0xb0 | c, 121, 0});
        }
        a->position = a->loop_start;
        a->cursor = a->loop_cursor;
        a->beat_cursor = a->loop_beat_cursor;
        copy_chase(&a->state,&a->loop_state,a->used_ports);
        copy_part_rows(a->seek_end,a->loop_note_end,sizeof(a->seek_end[0]),a->used_ports);
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
static void preview_command(DmAudio *a, Command cmd) {
    if (a->monitor) {
        send_synth(a->keyboard.synth, cmd.status, cmd.a, cmd.b);
        a->keyboard_tail = RATE * 2;
        int type = cmd.status & 0xf0;
        DmChannel *applied = &a->keyboard_applied[cmd.status & 15];
        if (type == 0xc0) {
            applied->program = cmd.a;
        } else if (type == 0xb0 && cmd.a == 7) {
            applied->volume = cmd.b;
        } else if (type == 0xb0 && cmd.a == 10) {
            applied->pan = cmd.b;
        }
        if (type != 0x80 && type != 0x90 && type != 0xa0) {
            send_live(a,cmd.status&15, cmd.status, cmd.a, cmd.b);
        }
    }
}
static void preview_commands(DmAudio *a) {
    Command cmd;
    while (queue_get(&a->preview, &cmd, NULL)) {
        if(cmd.live_only) {
            send_live(a,cmd.port*16+(cmd.status&15),cmd.status,cmd.a,cmd.b);
            queue_put(&a->midi,cmd);continue;
        }
        if (cmd.status == 0xf1) {
            int channel = cmd.a;
            a->keyboard_override[channel] = cmd.b == 255 ? -1 : cmd.b;
            a->keyboard_config = true;
            keyboard_configure(a);
            continue;
        }
        if ((cmd.status & 0xf0) == 0x90 && cmd.b &&
            !(a->preview_mask & (1u << (cmd.status & 15))) &&
            a->keyboard_override[cmd.status & 15] < 0) {
            continue;
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
        int program = override ? a->keyboard_override[c] : next.program < 0 ? 0 : next.program;
        if (!override && !(a->preview_mask & (1u << c))) {
            next.volume = 0;
        }
        if (old->program != program) {
            send_synth(a->keyboard.synth, 0xc0 | c, program, 0);
        }
        if (old->volume != next.volume) {
            send_synth(a->keyboard.synth, 0xb0 | c, 7, next.volume);
        }
        if (old->pan != next.pan) {
            send_synth(a->keyboard.synth, 0xb0 | c, 10, next.pan);
        }
        *old = next;
        old->program = program;
    }
    a->keyboard_config = false;
}
static void keyboard_silence(DmAudio *a) {
    for (int c = 0; c < 16; ++c) {
        fluid_synth_all_sounds_off(a->keyboard.synth, c);
    }
    a->keyboard_tail = 0;
}
static void keyboard_render(DmAudio *a, float *out, int frames) {
    if (fluid_synth_get_active_voice_count(a->keyboard.synth)) {
        a->keyboard_tail = RATE * 2;
    }
    if (a->keyboard_tail <= 0) {
        return;
    }
    float keys[BLOCK * 2];
    SDL_AtomicInt peaks[16]={0};
    render(&a->keyboard, keys, frames, peaks);
    for(int c=0;c<16;c++) {
        int peak=SDL_GetAtomicInt(&peaks[c]),part=a->keyboard_parts[c];
        if(peak>SDL_GetAtomicInt(&a->peaks[part]))SDL_SetAtomicInt(&a->peaks[part],peak);
    }
    for (int i = 0; i < frames * 2; ++i) {
        out[i] = fmaxf(-1, fminf(1, out[i] + keys[i]));
    }
    a->keyboard_tail -= frames;
}
static void feedback_stop(DmAudio *a) {
    if (a->feedback_key >= 0) {
        fluid_synth_noteoff(a->feedback.synth, a->feedback_program == 128 ? 9 : 0, a->feedback_key);
        a->feedback_key = -1;
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
        int channel = cmd.status == 128 ? 9 : 0;
        bool attack = a->feedback_key != cmd.a || a->feedback_program != cmd.status;
        if (attack) {
            feedback_stop(a);
            a->feedback_program = cmd.status;
            a->feedback_key = cmd.a;
            fluid_synth_program_change(a->feedback.synth, channel,
                                       cmd.status == 128 ? 0 : cmd.status);
        }
        fluid_synth_pitch_wheel_sens(a->feedback.synth, channel, 2);
        fluid_synth_pitch_bend(a->feedback.synth, channel, 8192 + (int)cmd.b * 8192 / 256);
        if (attack) {
            fluid_synth_noteon(a->feedback.synth, channel, cmd.a, 100);
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
    feedback_commands(a);
    keyboard_configure(a);
    int offset = 0;
    while (offset < frames) {
        wrap_playback(a);
        dispatch_due(a);
        int n = next_boundary(a, frames - offset);
        if (a->monitor) {
            render(&a->live, out + offset * 2, n, a->peaks);
            for(unsigned p=1;p<DM_NOTE_PORTS;p++)if(a->port_live[p])render_port_mix(a->port_live[p],out+offset*2,n,&a->peaks[p*16]);
            keyboard_render(a, out + offset * 2, n);
            feedback_render(a, out + offset * 2, n);
        } else {
            memset(out + offset * 2, 0, (size_t)n * 2 * sizeof(float));
        }
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
    if (event->status == 0xf0) {
        uint8_t start = 0xf0;
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
                uint8_t b[] = {0xb0 | c, 120, 0};
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
            int n = (cmd.status & 0xe0) == 0xc0 ? 2 : 3;
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
DmAudio *dm_audio_create(char *error, size_t cap) {
    DmAudio *a = calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    a->device_index = -1;
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
        snprintf(error, cap, DM_TEXT_CANNOT_INITIALIZE_MIDI_OUTPUT);
        dm_audio_destroy(a);
        return NULL;
    }
    if (a->device) {
        a->midi_thread = SDL_CreateThread(midi_worker, DM_TEXT_MIDI_OUTPUT, a);
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, a);
    if (!a->stream) {
        snprintf(error, cap, DM_TEXT_AUDIO_UNAVAILABLE_S_USE_PREFERENCES_TO_RETRY, SDL_GetError());
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
                uint8_t b[] = {0xb0 | c, 120, 0};
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
    free_port_synths(a->port_live);free_port_synths(a->prepared_ports);
    dm_schedule_free(&a->schedule);
    dm_schedule_free(&a->render_schedule);
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
    if (event->status != 0xf0 || event->sysex.size < 4) {
        return false;
    }
    const uint8_t *b = event->sysex.data;
    return b[0] == 0x7e && b[2] == 9 && (b[3] == 1 || b[3] == 3);
}
static void cache_event(DmAudio *a, const DmScheduled *event) {
    if (gm_reset(event)) {
        for (int c = event->port*16; c < event->port*16+16; ++c) {
            reset_chase_channel(a, c);
        }
        return;
    }
    int c = event->port*16+(event->status & 15);
    switch (event->status & 0xf0) {
    case 0xa0:
        a->state.poly[c][event->a] = event->b;
        break;
    case 0xd0:
        a->state.pressure[c] = event->a;
        break;
    case 0xb0:
        cache_controller(a, c, event->a, event->b);
        break;
    case 0xe0:
        a->state.bend[c][0] = event->a;
        a->state.bend[c][1] = event->b;
        break;
    case 0x90:
        if (event->b) {
            a->state.notes[c][event->a] = event->b;
            a->state.held[c][event->a] = 1;
            a->seek_end[c][event->a] = event->note_end;
        } else {
            chase_note_off(a, c, event->a);
        }
        break;
    case 0x80:
        chase_note_off(a, c, event->a);
        break;
    }
}
static void prepare_chase(DmAudio *a, const DmSchedule *s, double seconds) {
    a->beat_cursor = 0;
    while (a->beat_cursor < s->beat_count && s->beats[a->beat_cursor] < seconds) {
        ++a->beat_cursor;
    }
    memset(a->used_ports,0,sizeof(a->used_ports));a->used_ports[0]=true;
    for(size_t i=0;i<s->count;i++)a->used_ports[s->items[i].port]=true;
    for (int c = 0; c < DM_PARTS; ++c)if(a->used_ports[c/16])reset_chase_channel(a,c);
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
        copy_chase(&a->loop_state,&scratch->state,a->used_ports);
        copy_part_rows(a->loop_note_end,scratch->seek_end,sizeof(a->loop_note_end[0]),a->used_ports);
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
static bool prepare_note_ends(DmSchedule *schedule) {
    size_t (*head)[128]=malloc(DM_PARTS*sizeof(*head)),(*tail)[128]=malloc(DM_PARTS*sizeof(*tail));
    uint8_t programs[DM_PARTS] = {0};
    if(!head||!tail){free(head);free(tail);return false;}
    memset(head, 255, DM_PARTS*sizeof(*head));
    memset(tail, 255, DM_PARTS*sizeof(*tail));
    size_t *links = malloc((schedule->count + 1) * sizeof(*links));
    if (!links) {
        free(head);free(tail);
        return false;
    }
    for (size_t i = 0; i < schedule->count; ++i) {
        DmScheduled *event = &schedule->items[i];
        int type = event->status & 0xf0, c = event->port*16+(event->status & 15), key = event->a;
        if (type == 0xc0) {
            programs[c] = event->a;
        }
        if (type == 0x90 && event->b) {
            event->note_program = programs[c];
            event->note_end = schedule->duration;
            links[i] = SIZE_MAX;
            if (head[c][key] == SIZE_MAX) {
                head[c][key] = i;
            } else {
                links[tail[c][key]] = i;
            }
            tail[c][key] = i;
        } else if ((type == 0x80 || (type == 0x90 && !event->b)) && head[c][key] != SIZE_MAX) {
            size_t on = head[c][key];
            schedule->items[on].note_end = event->seconds;
            head[c][key] = links[on];
        }
    }
    free(links);
    free(head);free(tail);
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
    for(unsigned p=1;p<DM_NOTE_PORTS;p++)if(ports[p]){synth_free(ports[p]);free(ports[p]);ports[p]=NULL;}
}
static bool prepare_ports(DmAudio *a,const bool *used,Synth **fresh,char *error,size_t cap) {
    for(unsigned p=1;p<DM_NOTE_PORTS;p++)if(used[p]&&!a->port_live[p]) {
        if(SDL_GetAtomicInt(&a->job_busy)&&a->job_type==1){snprintf(error,cap,"Wait for SoundFont loading before activating a new note port.");free_port_synths(fresh);return false;}
        fresh[p]=calloc(1,sizeof(Synth));
        if(!fresh[p]||!synth_create(fresh[p],a->font_path)){snprintf(error,cap,"Cannot prepare independent playback for note port %u.",p);free_port_synths(fresh);return false;}
    }
    return true;
}
static bool publish_schedule(DmAudio *a, const DmProject *p, DmSchedule next, double seconds,
                             bool playing, bool editing, char *error, size_t cap) {
    /* Prepare seek state outside the audio lock. Swap only bounded state in the critical section.
     */
    DmAudio *scratch = calloc(1, sizeof(*scratch));
    RetiredSchedule *retired = calloc(1, sizeof(*retired));
    if (!scratch || !retired || !prepare_note_ends(&next)) {
        free(scratch);
        free(retired);
        dm_schedule_free(&next);
        return false;
    }
    prepare_chase(scratch, &next, seconds);
    size_t capacity;
    NoteRelease *releases = prepare_release_storage(a, next.count, &capacity);
    if (!releases) {
        free(scratch);
        free(retired);
        dm_schedule_free(&next);
        return false;
    }
    Synth *fresh[DM_NOTE_PORTS]={0};
    if(!prepare_ports(a,scratch->used_ports,fresh,error,cap)){free(releases);free(scratch);free(retired);dm_schedule_free(&next);return false;}
    SDL_LockMutex(a->device_lock);
    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }
    bool preserve =
        install_release_storage(a, scratch, &next, releases, editing, &seconds, &playing);
    NoteRelease *old_releases = a->releases;
    a->releases = releases;
    a->release_capacity = capacity;
    DmSchedule old = a->schedule;
    a->schedule = next;
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
    memcpy(a->used_ports,scratch->used_ports,sizeof(a->used_ports));
    for(unsigned port=1;port<DM_NOTE_PORTS;port++)if(fresh[port])a->port_live[port]=fresh[port];
    copy_part_rows(a->seek_end,scratch->seek_end,sizeof(a->seek_end[0]),a->used_ports);
    copy_chase(&a->state,&scratch->state,a->used_ports);
    a->preview_mask = project_preview_mask(p);
    memcpy(a->keyboard_channels, p->channels, sizeof(a->keyboard_channels));
    a->keyboard_config = true;
    a->chase = preserve ? a->chase : playing && scratch->chase;
    a->position = seconds;
    a->playing = playing;
    a->reset = a->reset || !preserve;
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
    bool colored=false;for(unsigned port=1;port<DM_NOTE_PORTS;port++)colored|=a->used_ports[port];
    if(colored&&a->device_index>=0&&!a->port_warning_sent){SDL_SetAtomicInt(&a->port_warning,1);a->port_warning_sent=true;}
    if(!colored)a->port_warning_sent=false;
    free(old_releases);
    free(scratch);
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
    return publish_schedule(a, p, next, seconds, playing, false, error, cap);
}
static bool preview_event(const DmEvent *event, int channel, double length) {
    if (event->beat > length || !event->message.size) {
        return false;
    }
    unsigned status = event->message.data[0];
    return status >= 0xf0 || (int)(status & 15) == channel;
}
static void preview_channels(DmProject *view, int channel, int program) {
    for (int c = 0; c < 16; ++c) {
        view->channels[c].mute = false;
        view->channels[c].solo = false;
        if (program >= 0) {
            view->channels[c].enabled = false;
        }
    }
    view->channels[channel].enabled = true;
    if (program >= 0) {
        view->channels[channel].program = program == 128 ? 0 : program;
    }
}
static void preview_schedule_program(DmSchedule *next, int program) {
    if (program < 0) {
        return;
    }
    int route = program == 128 ? 9 : 0;
    for (size_t i = 0; i < next->count; ++i) {
        DmScheduled *event = &next->items[i];
        if (event->status >= 0x80 && event->status < 0xf0) {
            int type = event->status & 0xf0;
            event->status = type | route;
            if (type == 0xc0) {
                event->a = program == 128 ? 0 : program;
            } else if (type == 0xb0 && (event->a == 0 || event->a == 32)) {
                event->b = 0;
            }
        }
    }
}
static bool publish_pattern(DmAudio *a, const DmProject *p, int pattern, int channel, int program,
                            double length, double seconds, bool playing, bool editing, char *error,
                            size_t cap) {
    if (!a || pattern < 0 || (size_t)pattern >= p->pattern_count || channel < 0 || channel > 15 ||
        program < -1 || program > 128 || !isfinite(length) || length <= 0 || length > 10000000) {
        return false;
    }
    DmProject view = *p;
    preview_channels(&view, channel, program);
    DmClip clip = {0, pattern, channel, 0, length, 0};
    view.clips = &clip;
    view.clip_count = 1;
    view.events = calloc(p->event_count + 1, sizeof(*view.events));
    if (!view.events) {
        return false;
    }
    view.event_count = 0;
    for (size_t i = 0; i < p->event_count; ++i) {
        if (preview_event(&p->events[i], channel, length)) {
            view.events[view.event_count++] = p->events[i];
        }
    }
    DmSchedule next = {0};
    bool ok = dm_schedule(&view, &next);
    free(view.events);
    if (!ok) {
        snprintf(error, cap, DM_TEXT_CANNOT_PREPARE_PATTERN_PLAYBACK);
        return false;
    }
    preview_schedule_program(&next, program);
    next.duration = dm_beat_seconds(p, length);
    return publish_schedule(a, p, next, seconds, playing, editing, error, cap);
}
bool dm_audio_publish_pattern(DmAudio *a, const DmProject *p, int pattern, int channel, int program,
                              double length, double seconds, bool playing, char *error,
                              size_t cap) {
    return publish_pattern(a, p, pattern, channel, program, length, seconds, playing, false, error,
                           cap);
}
bool dm_audio_update(DmAudio *a, const DmProject *p, int pattern, int channel, int program,
                     double length, char *error, size_t cap) {
    if (!a) {
        return false;
    }
    double seconds = dm_audio_position(a);
    bool playing = dm_audio_playing(a);
    if (pattern >= 0) {
        return publish_pattern(a, p, pattern, channel, program, length, seconds, playing, true,
                               error, cap);
    }
    DmSchedule next = {0};
    if (!dm_schedule(p, &next)) {
        return false;
    }
    return publish_schedule(a, p, next, seconds, playing, true, error, cap);
}
void dm_audio_transport(DmAudio *a, bool playing) {
    if (!a) {
        return;
    }
    if (a->stream) {
        SDL_LockAudioStream(a->stream);
    }
    a->playing = playing;
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
    if (!a) return;
    if (a->stream) SDL_LockAudioStream(a->stream);
    a->playing = a->recording = a->reset = a->chase = false;
    SDL_SetAtomicInt(&a->playing_flag, 0);
    a->release_count = 0; a->release_end = 0;
    a->keyboard_tail = a->feedback_tail = a->audition_frames = a->click_frames = 0;
    reset_live_ports(a);
    a->feedback_key = -1;
    memset(a->note_token, 0, sizeof(a->note_token));
    memset(a->state.notes, 0, sizeof(a->state.notes));
    memset(a->state.held, 0, sizeof(a->state.held));
    memset(a->state.sostenuto, 0, sizeof(a->state.sostenuto));
    SDL_SetAtomicInt(&a->preview.read, SDL_GetAtomicInt(&a->preview.write));
    SDL_SetAtomicInt(&a->preview.panic, 0);
    SDL_SetAtomicInt(&a->feedback_queue.read, SDL_GetAtomicInt(&a->feedback_queue.write));
    SDL_SetAtomicInt(&a->feedback_queue.panic, 0);
    // The worker sends these after discarding pending note-ons, without device I/O on the GUI thread.
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
            queue_put(&a->midi, (Command){0xb0 | c, cc, 0});
        }
        fluid_synth_pitch_bend(a->live.synth, c, 8192);
        fluid_synth_pitch_bend(a->keyboard.synth, c, 8192);
        fluid_synth_pitch_bend(a->feedback.synth, c, 8192);
        queue_put(&a->midi, (Command){0xe0 | c, 0, 64});
        a->keyboard_applied[c].volume = a->keyboard_applied[c].pan = -1;
        SDL_SetAtomicInt(&a->peaks[c], 0);
    }
    a->keyboard_config = true;
    keyboard_configure(a);
    SDL_UnlockMutex(a->device_lock);
    if (a->stream) SDL_UnlockAudioStream(a->stream);
}
bool dm_audio_playing(DmAudio *a) {
    return a && SDL_GetAtomicInt(&a->playing_flag) != 0;
}
void dm_audio_edit_preview(DmAudio *a, int program, float pitch, bool on) {
    if (!a) {
        return;
    }
    if (!on) {
        queue_put(&a->feedback_queue, (Command){255, 0, 0});
    } else if (program >= 0 && program <= 128 && isfinite(pitch) && pitch >= 0 && pitch <= 127) {
        int key = (int)floorf(pitch);
        int fraction = (int)roundf((pitch - key) * 128);
        queue_put(&a->feedback_queue, (Command){program, key, fraction});
    }
}
void dm_audio_preview(DmAudio *a, int channel, int key, int velocity, bool on) {
    if (a && key >= 0 && key <= 127) {
        queue_put(&a->preview, (Command){(on ? 0x90 : 0x80) | (channel & 15), key, velocity});
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
void dm_audio_program(DmAudio *a, int channel, int program) {
    if (a) {
        queue_put(&a->preview, (Command){0xc0 | (channel & 15), program & 127, 0});
    }
}
void dm_audio_keyboard_program(DmAudio *a, int channel, int program) {
    if (a && channel >= 0 && channel < 16 && program >= -1 && program <= 127) {
        queue_put(&a->preview, (Command){0xf1, channel, program < 0 ? 255 : program});
    }
}
void dm_audio_control(DmAudio *a, int channel, int controller, int value) {
    if (a) {
        queue_put(&a->preview, (Command){0xb0 | (channel & 15), controller & 127, value & 127});
    }
}
void dm_audio_keyboard_mix(DmAudio *a,int route,int part,int volume,int pan) {
    if(!a||route<0||route>15||part<0||part>=DM_PARTS)return;
    if(a->stream)SDL_LockAudioStream(a->stream);
    a->keyboard_parts[route]=part;a->keyboard_channels[route].volume=volume;a->keyboard_channels[route].pan=pan;
    a->preview_mask|=(uint16_t)(1u<<route);a->keyboard_config=true;
    if(a->stream)SDL_UnlockAudioStream(a->stream);
}
void dm_audio_part_control(DmAudio *a, int port, int channel, int controller, int value) {
    if(a&&port>=0&&port<DM_NOTE_PORTS&&channel>=0&&channel<16)
        queue_put(&a->preview,(Command){.status=(uint8_t)(0xb0|channel),.a=(uint8_t)controller,.b=(uint8_t)value,.port=(uint8_t)port,.live_only=true});
}
float dm_audio_peak(DmAudio *a, int c) {
    return a&&c>=0&&c<DM_PARTS ? SDL_SetAtomicInt(&a->peaks[c], 0) / 10000.0f : 0;
}
void dm_audio_analyzer_enable(DmAudio *a, bool enabled) {
    if (!a || SDL_GetAtomicInt(&a->analyzer_enabled) == enabled) return;
    if (a->stream) SDL_LockAudioStream(a->stream);
    a->analyzer_cursor = a->analyzer_count = 0;
    SDL_SetAtomicInt(&a->analyzer_enabled, enabled);
    if (a->stream) SDL_UnlockAudioStream(a->stream);
}
bool dm_audio_analyzer_read(DmAudio *a, float *stereo) {
    if (!a) return false;
    if (a->stream) SDL_LockAudioStream(a->stream);
    bool ready = SDL_GetAtomicInt(&a->analyzer_enabled) && a->analyzer_count == DM_SPECTRUM_FRAMES;
    if (ready) {
        int tail = DM_SPECTRUM_FRAMES - a->analyzer_cursor;
        memcpy(stereo, a->analyzer_samples + a->analyzer_cursor * 2, (size_t)tail * 2 * sizeof(float));
        memcpy(stereo + tail * 2, a->analyzer_samples, (size_t)a->analyzer_cursor * 2 * sizeof(float));
    }
    if (a->stream) SDL_UnlockAudioStream(a->stream);
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
        snprintf(error, cap, DM_TEXT_AUDIO_INITIALIZATION_FAILED_S, SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    SDL_AudioStream *next =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, a);
    if (!next) {
        snprintf(error, cap, DM_TEXT_AUDIO_RETRY_FAILED_S, SDL_GetError());
        return false;
    }
    if (a->stream) {
        SDL_DestroyAudioStream(a->stream);
    }
    a->stream = next;
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
            uint8_t message[] = {(uint8_t)(0xb0 | c), 120, 0};
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
        snprintf(error, cap, DM_TEXT_MIDI_DEVICE_S, a->device->msg);
    }
    SDL_UnlockMutex(a->device_lock);
    return ok;
}

static bool audio_render(DmAudio *a) {
    Synth s={0};Synth *ports[DM_NOTE_PORTS]={0};
    if(!synth_create(&s,a->font_path)){snprintf(a->job_message,sizeof(a->job_message),"Cannot load SoundFont for audio export.");return false;}
    uint64_t frames=(uint64_t)ceil((a->render_schedule.duration+2)*RATE);
    if(a->render_options.codec==DM_AUDIO_WAV&&frames>(UINT32_MAX-256)/(2*(a->render_options.bits/8))) {
        synth_free(&s);snprintf(a->job_message,sizeof(a->job_message),"WAV exceeds the RIFF size limit.");return false;
    }
    bool ok=true;
    for(size_t i=0;i<a->render_schedule.count;i++) {
        unsigned port=a->render_schedule.items[i].port;if(!port||ports[port])continue;
        ports[port]=calloc(1,sizeof(Synth));
        if(!ports[port]||!synth_create(ports[port],a->font_path)){snprintf(a->job_message,sizeof(a->job_message),"Cannot prepare note ports for audio export.");ok=false;break;}
        if(SDL_GetAtomicInt(&a->cancel)){ok=false;break;}
    }
    DmOutput output={0};DmAudioEncoder *encoder=NULL;
    if(ok)ok=dm_output_begin(&output,a->job_path,a->job_message,sizeof(a->job_message));
    if(ok){encoder=dm_audio_encoder_begin(&output,&a->render_options,a->job_message,sizeof(a->job_message));ok=encoder!=NULL;}
    size_t cursor=0;uint64_t pos=0;
    while(ok&&pos<frames) {
        if(SDL_GetAtomicInt(&a->cancel)){ok=false;break;}
        while(cursor<a->render_schedule.count&&a->render_schedule.items[cursor].seconds*RATE<=pos+.5) {
            const DmScheduled *event=&a->render_schedule.items[cursor++];send_event(event->port?ports[event->port]:&s,event);
        }
        int n=(int)fmin(BLOCK,(double)(frames-pos));
        if(cursor<a->render_schedule.count){int until=(int)ceil(a->render_schedule.items[cursor].seconds*RATE-pos);if(until>0&&until<n)n=until;}
        float sound[BLOCK*2];render(&s,sound,n,NULL);
        for(unsigned port=1;port<DM_NOTE_PORTS;port++)if(ports[port])render_port_mix(ports[port],sound,n,NULL);
        ok=dm_audio_encoder_write(encoder,sound,n);pos+=(unsigned)n;
        SDL_SetAtomicInt(&a->progress,(int)(pos*100/frames));
    }
    if(SDL_GetAtomicInt(&a->cancel)){snprintf(a->job_message,sizeof(a->job_message),"Audio export canceled.");ok=false;}
    ok=dm_audio_encoder_finish(encoder,ok)&&ok;
    if(output.file)ok=dm_output_finish(&output,ok,a->job_message,sizeof(a->job_message));
    free_port_synths(ports);synth_free(&s);return ok;
}static int job_worker(void *userdata) {
    DmAudio *a = userdata;
    if (a->job_type == 1) {
        a->job_success = synth_create(&a->prepared, a->job_path) &&
                         synth_create(&a->prepared_keyboard, a->job_path) &&
                         synth_create(&a->prepared_feedback, a->job_path);
        for(unsigned p=1;p<DM_NOTE_PORTS&&a->job_success;p++)if(a->job_ports[p]) {
            if(SDL_GetAtomicInt(&a->cancel)){a->job_success=false;break;}
            a->prepared_ports[p]=calloc(1,sizeof(Synth));
            a->job_success=a->prepared_ports[p]&&synth_create(a->prepared_ports[p],a->job_path);
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
                 a->job_success ? DM_TEXT_SOUNDFONT_READY
                                : DM_TEXT_UNABLE_TO_LOAD_SOUNDFONT_PREVIOUS_SYNTH_PRESERVED);
    } else {
        a->job_success = audio_render(a);
        if (a->job_success) {
            snprintf(a->job_message, sizeof(a->job_message), "%s export complete", dm_audio_codec_name(a->render_options.codec));
        }
    }
    SDL_SetAtomicInt(&a->job_done, 1);
    return 0;
}
static bool job_start(DmAudio *a, const char *path, int type, char *error, size_t cap) {
    if (!a || SDL_GetAtomicInt(&a->job_busy)) {
        snprintf(error, cap, DM_TEXT_A_BACKGROUND_OPERATION_IS_ALREADY_RUNNING);
        return false;
    }
    snprintf(a->job_path, sizeof(a->job_path), "%s", path);
    a->job_type = type;
    for(unsigned p=1;p<DM_NOTE_PORTS;p++)a->job_ports[p]=a->port_live[p]!=NULL;
    a->job_success = false;
    a->job_message[0] = 0;
    SDL_SetAtomicInt(&a->cancel, 0);
    SDL_SetAtomicInt(&a->job_done, 0);
    SDL_SetAtomicInt(&a->progress, 0);
    SDL_SetAtomicInt(&a->job_busy, 1);
    a->worker =
        SDL_CreateThread(job_worker, type == 1 ? DM_TEXT_LOAD_SOUNDFONT : "Render audio", a);
    if (!a->worker) {
        SDL_SetAtomicInt(&a->job_busy, 0);
        snprintf(error, cap, DM_TEXT_CANNOT_START_BACKGROUND_WORKER);
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
    if(SDL_SetAtomicInt(&a->port_warning,0)) {
        if(kind)*kind=0;
        *success=true;
        snprintf(message,cap,"Independent note ports share one external MIDI connection and may interfere there. Internal playback and MIDI export retain separate ports.");return true;
    }
    if (SDL_SetAtomicInt(&a->midi_failed, 0)) {
        if (kind) {
            *kind = 0;
        }
        snprintf(message, cap, DM_TEXT_EXTERNAL_MIDI_DEVICE_FAILED_RECONNECT_IT_IN_PREFERENCES);
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
        Synth *old_ports[DM_NOTE_PORTS]={0};
        for(unsigned p=1;p<DM_NOTE_PORTS;p++){old_ports[p]=a->port_live[p];a->port_live[p]=a->prepared_ports[p];a->prepared_ports[p]=NULL;}
        a->live = a->prepared;
        a->keyboard = a->prepared_keyboard;
        a->feedback = a->prepared_feedback;
        a->feedback_key = -1;
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
    snprintf(message, cap, "%s", a->job_message);
    *success = a->job_success;
    SDL_SetAtomicInt(&a->job_done, 0);
    SDL_SetAtomicInt(&a->progress, 0);
    SDL_SetAtomicInt(&a->job_busy, 0);
    return true;
}
bool dm_audio_export(DmAudio *a, const DmProject *p, const char *path, const DmAudioOptions *options, char *error, size_t cap) {
    DmAudioOptions chosen=options?*options:dm_audio_options_default();
    if(!dm_audio_options_valid(&chosen)){snprintf(error,cap,"Invalid audio export settings.");return false;}
    if (!a || !*p->soundfont) {
        snprintf(error, cap, "Choose a SoundFont in Preferences before exporting audio.");
        return false;
    }
    if (dm_audio_busy(a)) {
        snprintf(error, cap, DM_TEXT_A_BACKGROUND_OPERATION_IS_ALREADY_RUNNING);
        return false;
    }
    if (!dm_schedule(p, &a->render_schedule)) {
        return false;
    }
    a->render_options=chosen;
    snprintf(a->font_path, sizeof(a->font_path), "%s", p->soundfont);
    if (job_start(a, path, 2, error, cap)) {
        return true;
    }
    dm_schedule_free(&a->render_schedule);
    return false;
}

bool dm_audio_exporting(DmAudio *a){return a&&a->job_type==2&&SDL_GetAtomicInt(&a->job_busy)!=0;}
int dm_audio_export_progress(DmAudio *a){return a?SDL_GetAtomicInt(&a->progress):0;}
