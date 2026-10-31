/* Multi-port MIDI import.

   Each source port is imported on its own, then merged into one arrangement. Source port k keeps
   its channels but uses note port k, so its notes, controls and events play through their own
   output and show in that port's color. Notes on a channel that already has a placement join that
   placement's pattern; anything else is added as new patterns and placements. */
#include "duck.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int channel_pattern(const DmProject *p, int channel) {
    for (size_t i = 0; i < p->clip_count; ++i) {
        DmClip clip = p->clips[i];

        if (clip.channel == channel && !clip.lane && !p->patterns[clip.pattern].kind) {
            return (int)i;
        }
    }

    return -1;
}

static int copy_pattern(DmProject *to, const DmPattern *from, uint8_t port) {
    int id = dm_pattern_add(to, from->name);

    if (id < 0) {
        return -1;
    }

    DmPattern *target = &to->patterns[id];
    target->kind = from->kind;
    target->mode = from->mode;
    target->event = from->event;
    target->speed = from->speed;
    target->tension = from->tension;
    target->skew = from->skew;
    target->pulse = from->pulse;
    target->amount = from->amount;
    target->center = from->center;
    target->pitch_center = from->pitch_center;
    target->reference = from->reference;
    target->inverted = from->inverted;
    target->control_scope = from->control_scope;
    target->port = from->kind ? port : 0;

    for (size_t n = 0; n < from->count; ++n) {
        DmNote note = from->notes[n];
        note.port = port;

        if (!dm_note_add(&to->patterns[id], note)) {
            return -1;
        }
    }

    to->patterns[id].length = from->length;
    return id;
}

static bool merge_port(DmProject *to, const DmProject *from, uint8_t port) {
    int *map = malloc((from->pattern_count + 1) * sizeof(*map));

    if (!map) {
        return false;
    }

    bool ok = true, had_channel[16];

    /* Placements enable their channels, so remember which channels the arrangement already had. */
    for (int c = 0; c < 16; ++c) {
        had_channel[c] = to->channels[c].enabled;
    }

    /* Note patterns join the channel's existing pattern; the rest are copied. */
    for (size_t i = 0; i < from->pattern_count && ok; ++i) {
        map[i] = -1;
    }

    for (size_t i = 0; i < from->clip_count && ok; ++i) {
        DmClip clip = from->clips[i];
        const DmPattern *source = &from->patterns[clip.pattern];
        int existing = clip.lane || source->kind ? -1 : channel_pattern(to, clip.channel);

        if (map[clip.pattern] < 0 && existing >= 0) {
            DmClip *target = &to->clips[existing];
            DmPattern *pattern = &to->patterns[target->pattern];
            map[clip.pattern] = target->pattern;

            for (size_t n = 0; n < source->count && ok; ++n) {
                DmNote note = source->notes[n];
                note.port = port;
                ok = dm_note_add(pattern, note);
            }

            pattern->length = fmax(pattern->length, source->length);

            if (target->length > 0 || clip.length > 0) {
                target->length = fmax(dm_clip_length(to, *target), dm_clip_length(from, clip));
            }

            continue;
        }

        if (map[clip.pattern] < 0) {
            map[clip.pattern] = copy_pattern(to, source, port);
            ok = map[clip.pattern] >= 0;
        }

        clip.pattern = map[clip.pattern];
        ok = ok && dm_clip_add(to, clip);
    }

    for (size_t i = 0; i < from->automation_count && ok; ++i) {
        DmAutomation a = from->automation[i];

        if (a.pattern >= 0 && map[a.pattern] < 0) {
            continue;
        }

        a.pattern = a.pattern >= 0 ? map[a.pattern] : -1;
        a.port = port;
        ok = dm_automation_add(to, a);
    }

    /* Tempo, meter and other conductor events are already present from the first port. */
    for (size_t i = 0; i < from->event_count && ok; ++i) {
        const DmEvent *event = &from->events[i];

        if (event->message.size && event->message.data[0] < MIDI_SYSTEM) {
            ok = dm_event_add(to, event->beat, event->track, port, &event->message);
        }
    }

    for (int c = 0; c < 16 && ok; ++c) {
        DmChannel channel = from->channels[c];

        if (!channel.enabled) {
            continue;
        }

        if (!had_channel[c]) {
            to->channels[c] = channel;
        }

        to->lane_count[c] =
            to->lane_count[c] > from->lane_count[c] ? to->lane_count[c] : from->lane_count[c];

        /* A port whose channel opens with another instrument keeps it on that port only. */
        int program = channel.program >= 0 && channel.program != to->channels[c].program
                          ? channel.program + 1
                          : 0;

        ok = dm_port_mix_set(
            to, (DmPortMix){port, c, channel.volume, channel.pan, false, false, program});
    }

    free(map);
    return ok;
}

bool dm_import_ports(DmProject *p, const unsigned char *data, size_t size, const int *ports,
                     int count, int sequence, char *error, size_t cap) {
    DmProject result;
    dm_project_init(&result);

    if (!dm_import(&result, data, size, ports[0], sequence, error, cap)) {
        dm_project_free(&result);
        return false;
    }

    /* Event ports name note ports, which are numbered from the first source port. */
    for (size_t i = 0; i < result.event_count; ++i) {
        result.events[i].port = 0;
    }

    bool ok = true;

    for (int k = 1; k < count && k < DM_NOTE_PORTS && ok; ++k) {
        DmProject port;
        dm_project_init(&port);
        ok = dm_import(&port, data, size, ports[k], sequence, error, cap) &&
             merge_port(&result, &port, (uint8_t)k);
        dm_project_free(&port);
    }

    if (!ok) {
        dm_project_free(&result);
        return false;
    }

    for (size_t i = 0; i < result.pattern_count; ++i) {
        dm_pattern_fit(&result, &result.patterns[i]);
    }

    result.port = DM_PORT_COMBINED;
    dm_project_free(p);
    *p = result;
    return true;
}
