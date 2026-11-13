/* The audio callback: command queues, synth messages, state chasing, event dispatch, looping and
   block rendering, and the MIDI output worker. */
#include "audio/internal.h"

void dm_audio_input_clock(DmAudio *a, bool changed) {
    if (changed) {
        ++a->input_transport;
    }

    DmMidiClockValue value = {SDL_GetTicksNS(),   a->position, a->input_latency,
                              a->input_transport, a->playing,  a->input_loop};
    dm_midi_clock_write(&a->clock, value);
    DmMidiInput *input = atomic_load_explicit(&a->input, memory_order_acquire);

    if (input) {
        dm_midi_clock_write(dm_midi_input_clock(input), value);
    }
}

void dm_audio_synth_free(Synth *s) {
    if (s->synth) {
        delete_fluid_synth(s->synth);
    }

    if (s->settings) {
        delete_fluid_settings(s->settings);
    }

    memset(s, 0, sizeof(*s));
}

bool dm_audio_synth_create(Synth *s, const char *font) {
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
        dm_audio_synth_free(s);
        return false;
    }

    return true;
}

bool dm_audio_queue_put(CommandQueue *q, Command c) {
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

bool dm_audio_queue_get(CommandQueue *q, Command *c, const DmScheduled **system) {
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

void dm_audio_send_synth(fluid_synth_t *s, uint8_t status, uint8_t a, uint8_t b) {
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

void dm_audio_send_route(Synth *s, uint8_t status, uint8_t a, uint8_t b) {
    if ((status & MIDI_STATUS_MASK) == MIDI_CONTROL) {
        dm_sample_pedal_cc(&s->sample_pedals, s->synth, status & MIDI_CHANNEL_MASK, a, b);
    } else {
        dm_audio_send_synth(s->synth, status, a, b);
    }
}

static Synth *live_port(DmAudio *a, unsigned port) {
    return port ? a->port_live[port] : &a->live;
}

void dm_audio_reset_live_ports(DmAudio *a) {
    fluid_synth_system_reset(a->live.synth);
    memset(&a->live.sample_pedals, 0, sizeof(a->live.sample_pedals));

    for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
        if (a->port_live[p]) {
            fluid_synth_system_reset(a->port_live[p]->synth);
            memset(&a->port_live[p]->sample_pedals, 0, sizeof(a->port_live[p]->sample_pedals));
        }
    }
}

void dm_audio_copy_part_rows(void *destination, const void *source, size_t row, const bool *used) {
    for (unsigned p = 0; p < DM_NOTE_PORTS; p++) {
        if (used[p]) {
            memcpy((uint8_t *)destination + p * 16 * row, (const uint8_t *)source + p * 16 * row,
                   16 * row);
        }
    }
}

void dm_audio_copy_chase(ChaseState *dst, const ChaseState *src, const bool *used) {
#define PART_COPY(field)                                                                           \
    dm_audio_copy_part_rows(dst->field, src->field, sizeof(dst->field[0]), used)
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
        dm_audio_send_route(&p->voice, (uint8_t)(type | 1), first, second);
    }
}

void dm_audio_send_live(DmAudio *a, int part, uint8_t status, uint8_t first, uint8_t second) {
    if ((status & MIDI_STATUS_MASK) == MIDI_PROGRAM) {
        a->live_program[part] = first;
    }

    if (a->monitor) {
        instrument_preview_control(a, part, status, first, second);

        Synth *s = live_port(a, (unsigned)part / 16);

        if (s) {
            dm_audio_send_route(s, status, first, second);
        }
    }
}

void dm_audio_send_event(Synth *s, const DmScheduled *e) {
    if (e->status < MIDI_SYSTEM) {
        dm_audio_send_route(s, e->status, e->a, e->b);
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

uint32_t dm_audio_sample_start(DmAudio *a, Synth *s, uint32_t preset, int channel, int key,
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

void dm_audio_sample_stop(Synth *s, uint32_t id, int channel, int key) {
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
            dm_audio_sample_stop(s, previous, c, key);
        }

        SampleAudition *p = &a->sample_audition;

        if (p->keyboard && p->song_part == part) {
            if (p->song_held[key]) {
                dm_audio_sample_stop(&p->voice, p->song_keys[key], 1, key);
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
            a->note_sample_id[part][key] = dm_audio_sample_start(a, s, preset, c, key, velocity);
        }
    }
}

static void song_note_off(DmAudio *a, int part, int key) {
    Synth *s = live_port(a, (unsigned)part / 16);

    if (a->monitor && s) {
        SampleAudition *p = &a->sample_audition;

        if (p->keyboard && p->song_part == part && p->song_held[key]) {
            dm_audio_sample_stop(&p->voice, p->song_keys[key], 1, key);
            p->song_held[key] = false;
        } else {
            dm_audio_sample_stop(s, a->note_sample_id[part][key], part & MIDI_CHANNEL_MASK, key);
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
            dm_audio_queue_put(
                &a->midi,
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

        dm_audio_queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_SOUND_OFF, 0});
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
        return cc >= MIDI_CC_SOUND_OFF;
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
            dm_audio_send_event(s, event);
        }
    }

    if (event->status < MIDI_SYSTEM) {
        dm_audio_queue_put(&a->midi, (Command){event->status, event->a, event->b});
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

    dm_audio_queue_put(&a->midi, on);

    if (a->state.poly[c][key] != 255) {
        Command at = {MIDI_KEY_PRESSURE | (c & MIDI_CHANNEL_MASK), key, a->state.poly[c][key]};
        dm_audio_send_live(a, c, at.status, at.a, at.b);
        dm_audio_queue_put(&a->midi, at);
    }
}

static void chase_sostenuto(DmAudio *a, int c, int value) {
    dm_audio_send_live(a, c, MIDI_CONTROL | (c & MIDI_CHANNEL_MASK), MIDI_CC_SOSTENUTO, value);
    dm_audio_queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_SOSTENUTO, value});
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

    chase_sostenuto(
        a, c, a->state.cc[c][MIDI_CC_SOSTENUTO] == 255 ? 0 : a->state.cc[c][MIDI_CC_SOSTENUTO]);

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
                dm_audio_queue_put(&a->midi, off);
            }
        }
    }
}

void dm_audio_chase(DmAudio *a) {
    if (!a->chase) {
        return;
    }

    chase_parameters(a);

    for (int c = 0; c < DM_PARTS; ++c) {
        if (!a->used_ports[c / 16]) {
            continue;
        }

        for (int cc = 0; cc < MIDI_CC_SOUND_OFF; ++cc) {
            if (a->state.cc[c][cc] != 255 && !parameter_control(cc) && cc != MIDI_CC_SOSTENUTO) {
                Command cmd = {MIDI_CONTROL | (c & MIDI_CHANNEL_MASK), (uint8_t)cc,
                               a->state.cc[c][cc]};
                dm_audio_send_live(a, c, cmd.status, cmd.a, cmd.b);
                dm_audio_queue_put(&a->midi, cmd);
            }
        }

        Command bend = {MIDI_PITCH_BEND | (c & MIDI_CHANNEL_MASK), a->state.bend[c][0],
                        a->state.bend[c][1]};
        dm_audio_send_live(a, c, bend.status, bend.a, bend.b);
        dm_audio_queue_put(&a->midi, bend);

        if (a->state.pressure[c] != 255) {
            Command pressure = {MIDI_CHANNEL_PRESSURE | (c & MIDI_CHANNEL_MASK),
                                a->state.pressure[c], 0};
            dm_audio_send_live(a, c, pressure.status, pressure.a, 0);
            dm_audio_queue_put(&a->midi, pressure);
        }

        chase_voices(a, c);
    }

    a->chase = false;
}

void dm_audio_render(Synth *s, float *interleaved, int frames, SDL_AtomicInt *peaks) {
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

void dm_audio_render_port_mix(Synth *s, float *out, int frames, SDL_AtomicInt *peaks) {
    if (fluid_synth_get_active_voice_count(s->synth)) {
        s->tail = RATE * 2;
    }

    if (s->tail <= 0) {
        return;
    }

    float extra[BLOCK * 2];
    dm_audio_render(s, extra, frames, peaks);

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

void dm_audio_mix_from_state(DmAudio *a) {
    for (int part = 0; part < DM_PARTS; ++part) {
        a->mix_volume[part] = a->state.cc[part][MIDI_CC_VOLUME];
        a->mix_pan[part] = a->state.cc[part][MIDI_CC_PAN];
    }
}

void dm_audio_mix_track(DmAudio *a, int part, int status, int controller, int value) {
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
            dm_audio_send_live(a, channel, MIDI_PROGRAM | (channel & MIDI_CHANNEL_MASK),
                               event->note_program, 0);
            dm_audio_queue_put(&a->midi, (Command){MIDI_PROGRAM | (channel & MIDI_CHANNEL_MASK),
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
            dm_audio_mix_track(a, event->port * 16 + (event->status & MIDI_CHANNEL_MASK),
                               event->status, event->a, event->b);
            instrument_preview_control(a, event->port * 16 + (event->status & MIDI_CHANNEL_MASK),
                                       event->status, event->a, event->b);
        }

        if (a->monitor && (event->status & MIDI_STATUS_MASK) != MIDI_NOTE_ON) {
            Synth *s = live_port(a, event->port);

            if (s) {
                dm_audio_send_event(s, event);
            }
        }

        if (event->status < MIDI_SYSTEM) {
            dm_audio_queue_put(&a->midi, (Command){event->status, event->a, event->b});
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

void dm_audio_loop_end(DmAudio *a) {
    dispatch_due(a);
    silence(a);
    dm_audio_reset_live_ports(a);
    // Reset external controllers as well as internal synth state at the wrap boundary.
    for (int c = 0; c < 16; ++c) {
        dm_audio_queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_RESET, 0});
    }

    a->position = 0;
    ++a->input_loop;
    dm_audio_input_clock(a, true);
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
        dm_audio_reset_live_ports(a);

        for (int c = 0; c < 16; ++c) {
            dm_audio_queue_put(&a->midi, (Command){MIDI_CONTROL | c, MIDI_CC_RESET, 0});
        }

        a->position = a->loop_start;
        ++a->input_loop;
        dm_audio_input_clock(a, true);
        a->cursor = a->loop_cursor;
        a->beat_cursor = a->loop_beat_cursor;
        dm_audio_copy_chase(&a->state, &a->loop_state, a->used_ports);
        dm_audio_mix_from_state(a);
        dm_audio_copy_part_rows(a->seek_end, a->loop_note_end, sizeof(a->seek_end[0]),
                                a->used_ports);
        a->chase = a->loop_start > 0;
        a->click_frames = 0;
        initial_state(a);
        dm_audio_chase(a);
        return;
    }

    if (a->playing && (!a->loop_enabled || a->loop_wait) && !a->recording &&
        a->position >= playback_end(a) - .25 / RATE) {
        dm_audio_loop_end(a);
    }
}

static void drain_muted_samples(Synth *s, int frames) {
    if (s && s->sample_banks && fluid_synth_get_active_voice_count(s->synth)) {
        float discard[BLOCK * 2];
        dm_audio_render(s, discard, frames, NULL);
    }
}

void dm_audio_sample_audition_start(SampleAudition *preview) {
    dm_sample_synth_start(preview->bank, preview->voice.synth, preview->preset, 1, 0, preview->key,
                          100);
}

static void sample_audition_render(SampleAudition *preview, float *out, int frames) {
    if (!preview->active) {
        return;
    }

    float sound[BLOCK * 2];
    dm_audio_render(&preview->voice, sound, frames, NULL);

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
            dm_audio_sample_audition_start(preview);
        } else {
            preview->active = false;
        }
    }
}

static void run_block(DmAudio *a, float *out, int frames) {
    if (a->reset) {
        silence(a);
        dm_audio_reset_live_ports(a);

        if (!a->playing || a->position > 0) {
            initial_state(a);
        }

        a->reset = false;
        a->click_frames = 0;
    }

    dm_audio_chase(a);

    if (a->input_reset) {
        dm_audio_keyboard_reset_controllers(a);
        a->input_reset = false;
    }

    if (SDL_SetAtomicInt(&a->preview.panic, 0)) {
        silence(a);
        dm_audio_keyboard_silence(a);
    }

    if (a->keyboard_panic) {
        dm_audio_keyboard_silence(a);
        dm_audio_feedback_stop(a);

        for (int c = 0; c < 16; ++c) {
            fluid_synth_all_sounds_off(a->feedback.synth, c);
        }

        a->keyboard_panic = false;
    }

    dm_audio_keyboard_configure(a);
    dm_audio_preview_commands(a);
    dm_audio_input_clock(a, false); /* Each block restarts the clock from its position. */
    dm_audio_input_commands(a);
    dm_audio_feedback_commands(a);
    dm_audio_keyboard_configure(a);
    int offset = 0;

    while (offset < frames) {
        wrap_playback(a);
        dispatch_due(a);
        int n = next_boundary(a, frames - offset);

        if (a->monitor) {
            dm_audio_render(&a->live, out + offset * 2, n, a->peaks);

            for (unsigned p = 1; p < DM_NOTE_PORTS; p++) {
                if (a->port_live[p]) {
                    dm_audio_render_port_mix(a->port_live[p], out + offset * 2, n,
                                             &a->peaks[p * 16]);
                }
            }

            dm_audio_keyboard_render(a, out + offset * 2, n);
            dm_audio_feedback_render(a, out + offset * 2, n);
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

void SDLCALL dm_audio_callback(void *userdata, SDL_AudioStream *stream, int needed, int total) {
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

int dm_audio_midi_worker(void *userdata) {
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

        while (dm_audio_queue_get(&a->midi, &cmd, &system)) {
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

        dm_audio_free_retired(a);
        SDL_UnlockMutex(a->device_lock);
        SDL_Delay(1);
    }

    return 0;
}
