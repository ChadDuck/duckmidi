/* The tempo map (beats, seconds, meters) and the playback schedule built from the timeline. */
#include "core/midi_internal.h"

uint32_t dm_tempo_microseconds(const uint8_t *message, size_t size) {
    if (size != 6 || message[0] != MIDI_META || message[1] != MIDI_META_TEMPO || message[2] != 3) {
        return 0;
    }

    return ((uint32_t)message[3] << 16) | ((uint32_t)message[4] << 8) | message[5];
}

/* Seconds per beat a tempo change after the song start sets, or 0. */
static double tempo_event(const DmEvent *e) {
    return e->beat > 0 ? dm_tempo_microseconds(e->message.data, e->message.size) / 1000000.0 : 0;
}

DmMeter dm_meter(const DmProject *p, double beat) {
    DmMeter meter = {dm_bar(p), 0, 0, INFINITY};

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        const uint8_t *b = e->message.data;

        if (e->message.size != 7 || b[0] != MIDI_META || b[1] != MIDI_META_TIME || e->beat <= 0) {
            continue;
        }

        if (e->beat > beat) {
            meter.next_change = e->beat;
            break;
        }

        meter.completed += ceil((e->beat - meter.origin) / meter.bar - 1e-9);
        meter.origin = e->beat;
        meter.bar = b[3] * 4.0 / (1 << b[4]);
    }

    return meter;
}

double dm_beat_seconds(const DmProject *p, double beat) {
    double last = 0, seconds = 0, rate = 60 / p->bpm;

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        double next = tempo_event(e);

        if (!next || e->beat > beat) {
            continue;
        }

        seconds += (e->beat - last) * rate;
        last = e->beat;
        rate = next;
    }

    return seconds + (beat - last) * rate;
}

static double seconds_beat(const DmProject *p, double seconds, double *bpm) {
    double last = 0, elapsed = 0, rate = 60 / p->bpm;

    for (size_t i = 0; i < p->event_count; ++i) {
        const DmEvent *e = &p->events[i];
        double next = tempo_event(e);

        if (!next) {
            continue;
        }

        double span = (e->beat - last) * rate;

        if (elapsed + span > seconds) {
            break;
        }

        elapsed += span;
        last = e->beat;
        rate = next;
    }

    if (bpm) {
        *bpm = 60 / rate;
    }

    return last + (seconds - elapsed) / rate;
}

double dm_seconds_beat(const DmProject *p, double seconds) {
    return seconds_beat(p, seconds, NULL);
}

double dm_playback_bpm(const DmProject *p, double seconds) {
    double bpm;
    seconds_beat(p, seconds, &bpm);
    return bpm;
}

static bool schedule_append(DmSchedule *s, const Raw *raw, double seconds) {
    if (!dm_reserve((void **)&s->items, &s->capacity, s->count + 1, sizeof(DmScheduled))) {
        return false;
    }

    DmScheduled event = {0};
    event.seconds = seconds;
    event.status = raw->bytes.data[0];
    event.port = (uint8_t)raw->port;
    event.a = raw->bytes.data[1];
    event.b = raw->bytes.size > 2 ? raw->bytes.data[2] : 0;

    if (event.status >= MIDI_SYSTEM) {
        Reader input = {raw->bytes.data + 1, raw->bytes.data + raw->bytes.size, true};
        uint32_t length = dm_midi_read_vlq(&input);

        if (!input.ok || !dm_bytes(&event.sysex, input.p, length)) {
            return false;
        }
    }

    s->items[s->count++] = event;
    return true;
}

static bool schedule_events(const DmProject *p, const Midi *m, DmSchedule *s) {
    double previous = 0, seconds = 0, rate = 60 / p->bpm;

    for (size_t i = 0; i < m->count; ++i) {
        const Raw *raw = &m->items[i];
        const uint8_t *message = raw->bytes.data;
        double beat = (double)raw->tick / DM_PPQ;
        seconds += (beat - previous) * rate;
        previous = beat;

        if (message[0] == MIDI_META) {
            uint32_t tempo = dm_tempo_microseconds(message, raw->bytes.size);

            if (tempo && beat > 0) {
                rate = tempo / 1000000.0;
            }

            continue;
        }

        if (!schedule_append(s, raw, seconds)) {
            return false;
        }
    }

    return true;
}

static bool schedule_beats(const DmProject *p, DmSchedule *s) {
    double length = dm_duration(p);
    s->duration = dm_beat_seconds(p, length);
    s->beat_count = (size_t)ceil(length);

    if (s->beat_count > 10000000) {
        return false;
    }

    s->beats = malloc(s->beat_count * sizeof(double));

    if (!s->beats) {
        return false;
    }

    double previous = 0, seconds = 0, rate = 60 / p->bpm;
    size_t event = 0;

    for (size_t beat = 0; beat < s->beat_count; ++beat) {
        while (event < p->event_count && p->events[event].beat <= (double)beat) {
            const DmEvent *e = &p->events[event++];
            double next = tempo_event(e);

            if (!next) {
                continue;
            }

            seconds += (e->beat - previous) * rate;
            previous = e->beat;
            rate = next;
        }

        s->beats[beat] = seconds + ((double)beat - previous) * rate;
    }

    s->next_beat = dm_beat_seconds(p, (double)s->beat_count);

    for (; event < p->event_count; ++event) {
        double next = tempo_event(&p->events[event]);

        if (next) {
            rate = next;
        }
    }

    s->final_rate = rate;
    return true;
}

bool dm_schedule(const DmProject *p, DmSchedule *s) {
    Midi m = {0};
    bool ok = dm_midi_collect(p, &m, true);

    if (ok) {
        qsort(m.items, m.count, sizeof(Raw), dm_midi_export_compare);
        ok = schedule_events(p, &m, s);
    }

    if (ok) {
        ok = schedule_beats(p, s);
    }

    dm_midi_free(&m);

    if (!ok) {
        dm_schedule_free(s);
    }

    return ok;
}

void dm_schedule_free(DmSchedule *s) {
    for (size_t i = 0; i < s->count; ++i) {
        dm_bytes_free(&s->items[i].sysex);
    }

    free(s->items);
    free(s->beats);
    memset(s, 0, sizeof(*s));
}
