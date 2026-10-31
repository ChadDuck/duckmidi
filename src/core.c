#include "duck.h"
#include "text/en.h"
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

bool dm_reserve(void **p, size_t *capacity, size_t needed, size_t width) {
    if (needed <= *capacity) {
        return true;
    }

    if (!width || needed > SIZE_MAX / width) {
        return false;
    }

    size_t n = *capacity ? *capacity : 16;

    while (n < needed) {
        if (n > SIZE_MAX / 2) {
            n = needed;
            break;
        }

        n *= 2;
    }

    if (n > SIZE_MAX / width) {
        return false;
    }

    void *next = realloc(*p, n * width);

    if (!next) {
        return false;
    }

    *p = next;
    *capacity = n;
    return true;
}

bool dm_bytes(DmBytes *b, const void *data, size_t size) {
    if (size > SIZE_MAX - b->size ||
        !dm_reserve((void **)&b->data, &b->capacity, b->size + size, 1)) {
        return false;
    }

    if (size) {
        memcpy(b->data + b->size, data, size);
    }

    b->size += size;
    return true;
}

void dm_bytes_free(DmBytes *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

bool dm_conductor(const DmBytes *b) {
    if (b->size < 2 || b->data[0] != MIDI_META) {
        return false;
    }

    return b->data[1] == MIDI_META_TEMPO || b->data[1] == MIDI_META_TIME ||
           b->data[1] == MIDI_META_MARKER;
}

void dm_project_init(DmProject *p) {
    memset(p, 0, sizeof(*p));
    strcpy(p->name, DM_TEXT_UNTITLED);
    p->bpm = 120;
    p->numerator = 4;
    p->denominator = 4;
    p->sequence_count = 1;
    p->port_count = 1;

    for (int c = 0; c < 16; ++c) {
        p->channels[c] = (DmChannel){c == 0 || c == MIDI_DRUM_CHANNEL, false, false,
                                     c == MIDI_DRUM_CHANNEL ? 0 : -1,  100,   64};
    }
}

DmPortMix dm_port_mix(const DmProject *p, int port, int channel) {
    DmPortMix mix = {port, channel, 100, 64, false, false};

    if (!port) {
        DmChannel c = p->channels[channel];
        mix.volume = c.volume;
        mix.pan = c.pan;
        mix.mute = c.mute;
        mix.solo = c.solo;
    } else {
        for (size_t i = 0; i < p->port_mix_count; i++) {
            if (p->port_mix[i].port == port && p->port_mix[i].channel == channel) {
                return p->port_mix[i];
            }
        }
    }

    return mix;
}

bool dm_port_mix_set(DmProject *p, DmPortMix mix) {
    if (mix.port < 0 || mix.port >= DM_NOTE_PORTS || mix.channel < 0 || mix.channel > 15 ||
        mix.volume < 0 || mix.volume > 127 || mix.pan < 0 || mix.pan > 127 || mix.program < 0 ||
        mix.program > 128) {
        return false;
    }

    if (!mix.port) {
        DmChannel *c = &p->channels[mix.channel];
        c->volume = mix.volume;
        c->pan = mix.pan;
        c->mute = mix.mute;
        c->solo = mix.solo;
        return true;
    }

    for (size_t i = 0; i < p->port_mix_count; i++) {
        if (p->port_mix[i].port == mix.port && p->port_mix[i].channel == mix.channel) {
            p->port_mix[i] = mix;
            return true;
        }
    }

    if (!dm_reserve((void **)&p->port_mix, &p->port_mix_capacity, p->port_mix_count + 1,
                    sizeof(mix))) {
        return false;
    }

    p->port_mix[p->port_mix_count++] = mix;
    return true;
}

void dm_channel_ports(const DmProject *p, int channel, bool used[DM_NOTE_PORTS]) {
    memset(used, 0, DM_NOTE_PORTS * sizeof(*used));
    used[0] = true;

    for (size_t i = 0; i < p->clip_count; i++) {
        if (p->clips[i].channel == channel) {
            const DmPattern *pat = &p->patterns[p->clips[i].pattern];

            if (pat->kind) {
                used[pat->port] = true;
            }

            for (size_t n = 0; n < pat->count; n++) {
                used[pat->notes[n].port] = true;
            }
        }
    }

    for (size_t i = 0; i < p->automation_count; i++) {
        if (p->automation[i].channel == channel) {
            used[p->automation[i].port] = true;
        }
    }

    for (size_t i = 0; i < p->port_mix_count; i++) {
        if (p->port_mix[i].channel == channel) {
            used[p->port_mix[i].port] = true;
        }
    }
}

void dm_project_free(DmProject *p) {
    for (size_t i = 0; i < p->pattern_count; ++i) {
        free(p->patterns[i].notes);
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        dm_bytes_free(&p->events[i].message);
    }

    free(p->patterns);
    free(p->clips);
    free(p->automation);
    free(p->events);

    for (size_t i = 0; i < p->view_count; ++i) {
        dm_bytes_free(&p->views[i].state);
    }

    free(p->views);
    free(p->port_mix);
    dm_bytes_free(&p->original);
    memset(p, 0, sizeof(*p));
}

bool dm_project_copy(DmProject *destination, const DmProject *source) {
    DmProject next = *source;
    next.patterns = NULL;
    next.pattern_count = next.pattern_capacity = 0;
    next.clips = NULL;
    next.clip_count = next.clip_capacity = 0;
    next.automation = NULL;
    next.automation_count = next.automation_capacity = 0;
    next.events = NULL;
    next.event_count = next.event_capacity = 0;
    next.views = NULL;
    next.view_count = next.view_capacity = 0;
    next.original = (DmBytes){0};
    next.port_mix = NULL;
    next.port_mix_count = next.port_mix_capacity = 0;

    if (!dm_reserve((void **)&next.port_mix, &next.port_mix_capacity, source->port_mix_count,
                    sizeof(DmPortMix))) {
        goto fail;
    }

    if (source->port_mix_count) {
        memcpy(next.port_mix, source->port_mix, source->port_mix_count * sizeof(DmPortMix));
    }

    next.port_mix_count = source->port_mix_count;

    if (!dm_reserve((void **)&next.patterns, &next.pattern_capacity, source->pattern_count,
                    sizeof(DmPattern))) {
        goto fail;
    }

    for (size_t i = 0; i < source->pattern_count; i++) {
        DmPattern p = source->patterns[i];
        p.notes = NULL;
        p.capacity = 0;

        if (!dm_reserve((void **)&p.notes, &p.capacity, p.count, sizeof(DmNote))) {
            goto fail;
        }

        if (p.count) {
            memcpy(p.notes, source->patterns[i].notes, p.count * sizeof(DmNote));
        }

        next.patterns[next.pattern_count++] = p;
    }

    if (!dm_reserve((void **)&next.clips, &next.clip_capacity, source->clip_count,
                    sizeof(DmClip))) {
        goto fail;
    }

    if (source->clip_count) {
        memcpy(next.clips, source->clips, source->clip_count * sizeof(DmClip));
    }

    next.clip_count = source->clip_count;

    if (!dm_reserve((void **)&next.automation, &next.automation_capacity, source->automation_count,
                    sizeof(DmAutomation))) {
        goto fail;
    }

    if (source->automation_count) {
        memcpy(next.automation, source->automation,
               source->automation_count * sizeof(DmAutomation));
    }

    next.automation_count = source->automation_count;

    if (!dm_reserve((void **)&next.events, &next.event_capacity, source->event_count,
                    sizeof(DmEvent))) {
        goto fail;
    }

    for (size_t i = 0; i < source->event_count; i++) {
        DmEvent event = source->events[i];
        event.message = (DmBytes){0};

        if (!dm_bytes(&event.message, source->events[i].message.data,
                      source->events[i].message.size)) {
            goto fail;
        }

        next.events[next.event_count++] = event;
    }

    if (!dm_reserve((void **)&next.views, &next.view_capacity, source->view_count,
                    sizeof(DmView))) {
        goto fail;
    }

    for (size_t i = 0; i < source->view_count; i++) {
        DmView view = source->views[i];
        view.state = (DmBytes){0};

        if (!dm_bytes(&view.state, source->views[i].state.data, source->views[i].state.size)) {
            goto fail;
        }

        next.views[next.view_count++] = view;
    }

    if (!dm_bytes(&next.original, source->original.data, source->original.size)) {
        goto fail;
    }

    dm_project_free(destination);
    *destination = next;
    return true;
fail:
    dm_project_free(&next);
    return false;
}

static bool pattern_name_used(const DmProject *p, int except, const char *name) {
    for (size_t i = 0; i < p->pattern_count; ++i) {
        if ((int)i != except && !strcmp(p->patterns[i].name, name)) {
            return true;
        }
    }

    return false;
}

void dm_pattern_name(DmProject *p, int id, const char *name) {
    char base[64], candidate[64];
    snprintf(base, sizeof(base), "%s", name);
    snprintf(candidate, sizeof(candidate), "%s", base);

    for (unsigned suffix = 2; pattern_name_used(p, id, candidate); ++suffix) {
        char number[16];
        snprintf(number, sizeof(number), "-%u", suffix);
        snprintf(candidate, sizeof(candidate), "%.*s%s", (int)(63 - strlen(number)), base, number);
    }

    snprintf(p->patterns[id].name, 64, "%s", candidate);
    snprintf(p->patterns[id].base_name, 64, "%s", candidate);
    p->patterns[id].duplicate_index = 1;
}

int dm_pattern_add(DmProject *p, const char *name) {
    if (p->next_pattern_uid == UINT32_MAX || p->pattern_count >= 100000 ||
        !dm_reserve((void **)&p->patterns, &p->pattern_capacity, p->pattern_count + 1,
                    sizeof(DmPattern))) {
        return -1;
    }

    DmPattern *pat = &p->patterns[p->pattern_count];
    memset(pat, 0, sizeof(*pat));
    pat->length = 1;
    pat->uid = ++p->next_pattern_uid;
    pat->event = -2;
    pat->speed = 1;
    pat->pulse = .5;
    pat->amount = .5;
    pat->center = 64;
    pat->pitch_center = 60;
    pat->reference = 60;
    char initial[64];

    if (name) {
        snprintf(initial, sizeof(initial), "%s", name);
    } else {
        snprintf(initial, sizeof(initial), DM_TEXT_PATTERN_NUMBER, p->pattern_count + 1);
    }

    dm_pattern_name(p, (int)p->pattern_count, initial);
    return (int)p->pattern_count++;
}

bool dm_note_add(DmPattern *p, DmNote n) {
    if (p->kind) {
        return false;
    }

    if (!isfinite(n.beat) || !isfinite(n.length) || n.beat < 0 || n.length <= 0 || n.key > 127 ||
        !n.velocity || n.velocity > 127 || n.selected > 1 || n.port >= DM_NOTE_PORTS) {
        return false;
    }

    if (!dm_reserve((void **)&p->notes, &p->capacity, p->count + 1, sizeof(DmNote))) {
        return false;
    }

    p->notes[p->count++] = n;
    p->length = fmax(p->length, n.beat + n.length);
    return true;
}

int dm_pattern_copy(DmProject *p, int source) {
    if (source < 0 || (size_t)source >= p->pattern_count) {
        return -1;
    }

    char name[64];
    snprintf(name, sizeof(name), "%s", p->patterns[source].base_name);
    uint32_t suffix = 1;

    for (size_t i = 0; i < p->pattern_count; i++) {
        if (!strcmp(p->patterns[i].base_name, name) && p->patterns[i].duplicate_index > suffix) {
            suffix = p->patterns[i].duplicate_index;
        }
    }

    if (suffix == UINT32_MAX) {
        return -1;
    }

    suffix++;
    int id = dm_pattern_add(p, name);

    if (id < 0) {
        return -1;
    }

    DmPattern *from = &p->patterns[source], *to = &p->patterns[id];
    snprintf(to->base_name, 64, "%s", name);
    to->duplicate_index = suffix;
    char postfix[16];
    snprintf(postfix, sizeof(postfix), "-%u", suffix);
    snprintf(to->name, 64, "%.*s%s", (int)(63 - strlen(postfix)), name, postfix);

    while (pattern_name_used(p, id, to->name)) {
        if (suffix == UINT32_MAX) {
            dm_pattern_remove(p, id);
            return -1;
        }

        suffix++;
        snprintf(postfix, sizeof(postfix), "-%u", suffix);
        snprintf(to->name, 64, "%.*s%s", (int)(63 - strlen(postfix)), name, postfix);
    }

    to->duplicate_index = suffix;

    for (size_t i = 0; i < from->count; ++i) {
        if (!dm_note_add(to, from->notes[i])) {
            dm_pattern_remove(p, id);
            return -1;
        }
    }

    to->length = from->length;
    to->kind = from->kind;
    to->mode = from->mode;
    to->event = from->event;
    to->speed = from->speed;
    to->tension = from->tension;
    to->skew = from->skew;
    to->pulse = from->pulse;
    to->amount = from->amount;
    to->center = from->center;
    to->pitch_center = from->pitch_center;
    to->reference = from->reference;
    to->inverted = from->inverted;
    to->control_scope = from->control_scope;
    to->port = from->port;
    to->color = from->color;
    size_t count = p->automation_count;
    DmAutomation *copy = malloc(count * sizeof(*copy));

    if (count && !copy) {
        dm_pattern_remove(p, id);
        return -1;
    }

    if (count) {
        memcpy(copy, p->automation, count * sizeof(*copy));
    }

    for (size_t i = 0; i < count; ++i) {
        if (copy[i].pattern == source) {
            copy[i].pattern = id;

            if (!dm_automation_add(p, copy[i])) {
                free(copy);
                dm_pattern_remove(p, id);
                return -1;
            }
        }
    }

    free(copy);
    return id;
}

void dm_pattern_remove(DmProject *p, int id) {
    if (id < 0 || (size_t)id >= p->pattern_count) {
        return;
    }

    size_t write = 0;

    for (size_t i = 0; i < p->clip_count; ++i) {
        DmClip c = p->clips[i];

        if (c.pattern == id) {
            continue;
        }

        if (c.pattern > id) {
            --c.pattern;
        }

        p->clips[write++] = c;
    }

    p->clip_count = write;
    write = 0;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];

        if (a.pattern == id) {
            continue;
        }

        if (a.pattern > id) {
            --a.pattern;
        }

        p->automation[write++] = a;
    }

    p->automation_count = write;
    free(p->patterns[id].notes);
    memmove(p->patterns + id, p->patterns + id + 1,
            (p->pattern_count - id - 1) * sizeof(DmPattern));
    --p->pattern_count;
}

int dm_pattern_move(DmProject *p, int from, int before) {
    int count = (int)p->pattern_count;

    if (from < 0 || from >= count || before < 0 || before > count) {
        return from;
    }

    /* Index the pattern takes once it is removed and inserted before the pattern at "before". */
    int to = before > from ? before - 1 : before;

    if (to == from) {
        return from;
    }

    DmPattern moved = p->patterns[from];

    if (to > from) {
        memmove(p->patterns + from, p->patterns + from + 1,
                (size_t)(to - from) * sizeof(DmPattern));
    } else {
        memmove(p->patterns + to + 1, p->patterns + to, (size_t)(from - to) * sizeof(DmPattern));
    }

    p->patterns[to] = moved;

    for (size_t i = 0; i < p->clip_count; ++i) {
        p->clips[i].pattern = dm_pattern_moved_index(p->clips[i].pattern, from, to);
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        p->automation[i].pattern = dm_pattern_moved_index(p->automation[i].pattern, from, to);
    }

    return to;
}

int dm_pattern_moved_index(int index, int from, int to) {
    if (index == from) {
        return to;
    }

    if (from < to && index > from && index <= to) {
        return index - 1;
    }

    if (to < from && index >= to && index < from) {
        return index + 1;
    }

    return index;
}

double dm_clip_length(const DmProject *p, DmClip c) {
    return c.length > 0 ? c.length : fmax(0, p->patterns[c.pattern].length - c.offset);
}

/* A triangle's rise/fall balance becomes sawtooth at skew extremes. Pulse width
   narrows peaks/troughs; tension bends straight sides toward spikes or plateaus. */
double dm_lfo(const DmPattern *p, double beat) {
    double phase = beat * p->speed + (p->skew + 1) * .5;
    phase -= floor(phase);
    double peak = (p->skew + 1) * .5, wave;

    if (peak <= 0) {
        wave = 1 - 2 * phase;
    } else if (peak >= 1) {
        wave = 2 * phase - 1;
    } else {
        wave = phase < peak ? -1 + 2 * phase / peak : 1 - 2 * (phase - peak) / (1 - peak);
    }

    if (p->pulse < .5) {
        wave = (wave - (1 - 2 * p->pulse)) / (2 * p->pulse);
    } else {
        wave = (wave + (2 * p->pulse - 1)) / (2 * (1 - p->pulse));
    }

    wave = fmax(-1, fmin(1, wave));
    double power = pow(64, fabs(p->tension));
    double shaped = p->tension < 0 ? pow(fabs(wave), power) : 1 - pow(1 - fabs(wave), power);
    return copysign(shaped, wave) * p->amount * (p->inverted ? -1 : 1);
}

double dm_lfo_speed_snap(double speed, double bar) {
    const double sync[] = {1.0 / 32, 1.0 / 24, 1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6,
                           1.0 / 4,  1.0 / 3,  .5,       2.0 / 3,  1,       4.0 / 3,
                           2,        8.0 / 3,  4,        16.0 / 3, 8,       16};
    double closest = speed, distance = .04;

    for (size_t i = 0; i < sizeof(sync) / sizeof(*sync); ++i) {
        double d = fabs(speed / sync[i] - 1);

        if (d <= distance) {
            closest = sync[i];
            distance = d;
        }
    }

    for (int i = -4; i <= 3; ++i) {
        double target = 1 / (bar * pow(2, i));

        if (target < .03125 || target > 16) {
            continue;
        }

        double d = fabs(speed / target - 1);

        if (d <= distance) {
            closest = target;
            distance = d;
        }
    }

    return closest;
}

double dm_lfo_center_snap(double value, bool pitch, bool free) {
    if (pitch) {
        if (!free) {
            value = round(value);
        }
    } else {
        value = free ? round(value) : round(value / 10) * 10;
    }

    return fmax(0, fmin(127, value));
}

int dm_lfo_range(const DmPattern *p) {
    return (int)fmin(127, fmax(2, ceil(fabs(p->pitch_center - p->reference) + 2 * p->amount)));
}

int dm_lfo_value(const DmPattern *p, double beat) {
    double wave = dm_lfo(p, beat), value = p->center + 63.5 * wave;

    if (p->event == -2) {
        double pitch = fmax(0, fmin(127, p->pitch_center + 2 * wave));
        value = MIDI_BEND_CENTER + (pitch - p->reference) * MIDI_BEND_CENTER / dm_lfo_range(p);
    }

    return (int)llround(fmax(0, fmin(p->event == -2 ? MIDI_BEND_MAX : MIDI_DATA_MAX, value)));
}

bool dm_clip_add(DmProject *p, DmClip c) {
    if (!isfinite(c.length) || c.length < 0 || c.length > 10000000 || c.lane < 0 ||
        !isfinite(c.offset) || c.offset < 0 || c.offset > 10000000 || c.lane > DM_LANES) {
        return false;
    }

    if (!isfinite(c.beat) || c.beat < 0 || c.channel < 0 || c.channel > 15 || c.pattern < 0 ||
        (size_t)c.pattern >= p->pattern_count || c.selected > 1) {
        return false;
    }

    if (!dm_reserve((void **)&p->clips, &p->clip_capacity, p->clip_count + 1, sizeof(c))) {
        return false;
    }

    p->clips[p->clip_count++] = c;
    p->channels[c.channel].enabled = true;
    return true;
}

static bool automation_message_valid(DmAutomation a) {
    if (!isfinite(a.bend) || fabs(a.bend) > MIDI_BEND_MAX) {
        return false;
    }

    if (a.channel < 0 || a.channel > 15) {
        return false;
    }

    if (a.a < 0 || a.a > 127 || a.b < 0 || a.b > 127) {
        return false;
    }

    return a.type >= MIDI_KEY_PRESSURE && a.type <= MIDI_PITCH_BEND && a.type % 16 == 0;
}

bool dm_automation_later(DmAutomation a, DmAutomation b) {
    if (a.beat != b.beat) {
        return a.beat > b.beat;
    }

    return a.curve < 0 && b.curve >= 0;
}

bool dm_automation_add(DmProject *p, DmAutomation a) {
    if (a.pattern < -1 || (a.pattern >= 0 && (size_t)a.pattern >= p->pattern_count)) {
        return false;
    }

    if (!isfinite(a.beat) || a.beat < 0 || !automation_message_valid(a) || a.anchor < 0 ||
        a.anchor > 127 || a.curve == INT32_MIN || a.port >= DM_NOTE_PORTS) {
        return false;
    }

    if (!dm_reserve((void **)&p->automation, &p->automation_capacity, p->automation_count + 1,
                    sizeof(a))) {
        return false;
    }

    size_t at = p->automation_count;

    while (at > 0 && dm_automation_later(p->automation[at - 1], a)) {
        p->automation[at] = p->automation[at - 1];
        --at;
    }

    p->automation[at] = a;
    ++p->automation_count;

    if (a.pattern >= 0 && p->patterns[a.pattern].kind) {
        DmPattern *pattern = &p->patterns[a.pattern];
        pattern->length = fmax(pattern->length, a.beat);
    }

    return true;
}

/* Whether a point curves toward the next point of its line; otherwise its value holds. */
bool dm_automation_curves(DmAutomation a) {
    return a.type != MIDI_PROGRAM && ((a.curve > 0 && !a.hold) || (!a.curve && a.ramp));
}

/* A point's value: pitch bends use both data bytes; program and channel pressure the first. */
static double automation_value(DmAutomation a) {
    return a.type == MIDI_PITCH_BEND                                   ? a.a + (a.b << 7)
           : a.type == MIDI_PROGRAM || a.type == MIDI_CHANNEL_PRESSURE ? a.a
                                                                       : a.b;
}

double dm_curve_value(DmAutomation first, DmAutomation last, double t) {
    double a = automation_value(first), b = automation_value(last);
    double value = a + (b - a) * t + first.bend * 4 * t * (1 - t);
    return fmax(0, fmin(first.type == MIDI_PITCH_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX, value));
}

double dm_bar(const DmProject *p) {
    return p->numerator * 4.0 / p->denominator;
}

static double note_pattern_end(const DmPattern *pattern) {
    double end = 0;

    for (size_t i = 0; i < pattern->count; ++i) {
        end = fmax(end, pattern->notes[i].beat + pattern->notes[i].length);
    }

    return fmax(1, ceil(end - 1e-9));
}

void dm_pattern_fit(const DmProject *p, DmPattern *pattern) {
    if (!pattern->kind) {
        pattern->length = note_pattern_end(pattern);
        return;
    }

    double end = pattern->length;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];

        if (a.pattern >= 0 && &p->patterns[a.pattern] == pattern) {
            end = fmax(end, ceil(a.beat - 1e-9));
        }
    }

    pattern->length = end;
}

void dm_patterns_fit(DmProject *p) {
    for (size_t i = 0; i < p->pattern_count; ++i) {
        if (!p->patterns[i].kind) {
            p->patterns[i].length = note_pattern_end(&p->patterns[i]);
        }
    }
}

double dm_duration(const DmProject *p) {
    double end = dm_bar(p);

    for (size_t i = 0; i < p->clip_count; ++i) {
        const DmClip *c = &p->clips[i];
        end = fmax(end, c->beat + dm_clip_length(p, *c));
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        if (p->automation[i].pattern < 0) {
            end = fmax(end, p->automation[i].beat);
        }
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        end = fmax(end, p->events[i].beat);
    }

    return end;
}

void dm_quantize(DmPattern *p, double grid) {
    bool selected = false;

    for (size_t i = 0; i < p->count; ++i) {
        selected |= p->notes[i].selected != 0;
    }

    for (size_t i = 0; i < p->count; ++i) {
        if (!selected || p->notes[i].selected) {
            double end = round((p->notes[i].beat + p->notes[i].length) / grid) * grid;
            p->notes[i].beat = round(p->notes[i].beat / grid) * grid;
            p->notes[i].length = fmax(grid, end - p->notes[i].beat);
            p->length = fmax(p->length, p->notes[i].beat + p->notes[i].length);
        }
    }
}
#ifdef _WIN32
static wchar_t *wide_path(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);

    if (!n) {
        return NULL;
    }

    wchar_t *w = malloc((size_t)n * sizeof(*w));

    if (w) {
        MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    }

    return w;
}
#endif
static FILE *file_open(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t *w = wide_path(path);

    if (!w) {
        return NULL;
    }

    FILE *f = _wfopen(w, mode[0] == 'r' ? L"rb" : L"wbx");
    free(w);
    return f;
#else
    return fopen(path, mode);
#endif
}

bool dm_read(const char *path, DmBytes *out, char *error, size_t cap) {
    FILE *f = file_open(path, "rb");

    if (!f) {
        snprintf(error, cap, DM_TEXT_CANNOT_OPEN_FILE, strerror(errno));
        return false;
    }

    bool ok = fseek(f, 0, SEEK_END) == 0;
    long size = ok ? ftell(f) : -1;
    rewind(f);
    ok = size > 0 && size <= 1024 * 1024 * 1024 &&
         dm_reserve((void **)&out->data, &out->capacity, (size_t)size, 1);

    if (ok) {
        out->size = (size_t)size;
        ok = fread(out->data, 1, out->size, f) == out->size;
    }

    fclose(f);

    if (!ok) {
        dm_bytes_free(out);
        snprintf(error, cap, DM_TEXT_FILE_UNREADABLE_EMPTY);
    }

    return ok;
}

bool dm_output_begin(DmOutput *out, const char *path, char *error, size_t cap) {
    static atomic_uint serial = 0;
    memset(out, 0, sizeof(*out));

    if (strlen(path) >= sizeof(out->path)) {
        snprintf(error, cap, DM_TEXT_OUTPUT_PATH_TOO);
        return false;
    }

    snprintf(out->path, sizeof(out->path), "%s", path);
    unsigned sequence = atomic_fetch_add(&serial, 1);
    snprintf(out->temporary, sizeof(out->temporary), DM_TEXT_DUCK_TMP, path,
             (unsigned long long)time(NULL), (unsigned long)clock(), sequence);
    out->file = file_open(out->temporary, "wbx");

    if (!out->file) {
        snprintf(error, cap, DM_TEXT_CANNOT_CREATE_TEMPORARY, strerror(errno));
        return false;
    }

    return true;
}

bool dm_output_write(DmOutput *out, const void *data, size_t size, char *error, size_t cap) {
    if (!out->file || out->failed) {
        return false;
    }

    if (fwrite(data, 1, size, out->file) != size) {
        out->failed = true;
        snprintf(error, cap, DM_TEXT_WRITING_FAILED, strerror(errno));
        return false;
    }

    return true;
}

static bool output_replace(const char *temporary, const char *path) {
#ifdef _WIN32
    wchar_t *from = wide_path(temporary), *to = wide_path(path);
    bool ok =
        from && to && MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(from);
    free(to);
    return ok;
#else
    return rename(temporary, path) == 0;
#endif
}

static void output_remove(const char *path) {
#ifdef _WIN32
    wchar_t *wide = wide_path(path);

    if (wide) {
        DeleteFileW(wide);
        free(wide);
    }
#else
    remove(path);
#endif
}

bool dm_output_finish(DmOutput *out, bool commit, char *error, size_t cap) {
    if (!out->file) {
        return false;
    }

    bool ok = !out->failed;

    if (fflush(out->file)) {
        ok = false;
        snprintf(error, cap, DM_TEXT_CANNOT_FLUSH_OUTPUT, strerror(errno));
    }

    if (fclose(out->file)) {
        ok = false;
        snprintf(error, cap, DM_TEXT_CANNOT_CLOSE_OUTPUT, strerror(errno));
    }

    out->file = NULL;

    if (!commit) {
        ok = false;
    }

    if (ok) {
        ok = output_replace(out->temporary, out->path);

        if (!ok) {
#ifdef _WIN32
            DWORD code = GetLastError();
            char reason[256] = {0};
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                           0, reason, sizeof(reason), NULL);
            reason[strcspn(reason, "\r\n")] = 0;
            snprintf(error, cap, DM_TEXT_CANNOT_REPLACE_DESTINATION_WINDOWS,
                     *reason ? reason : DM_TEXT_FILE_SYSTEM_OPERATION_FAILED, (unsigned long)code);
#else
            snprintf(error, cap, DM_TEXT_CANNOT_REPLACE_DESTINATION_ORIGINAL, strerror(errno));
#endif
        }
    }

    if (!ok) {
        output_remove(out->temporary);

        if (commit && !*error) {
            snprintf(error, cap, DM_TEXT_WRITING_FAILED_ORIGINAL);
        }
    }

    return ok;
}

bool dm_write(const char *path, const void *data, size_t size, char *error, size_t cap) {
    DmOutput out;

    if (!dm_output_begin(&out, path, error, cap)) {
        return false;
    }

    bool ok = dm_output_write(&out, data, size, error, cap);
    return dm_output_finish(&out, ok, error, cap);
}

static bool zip_le(DmBytes *out, uint32_t value, int width) {
    unsigned char b[4];

    for (int i = 0; i < width; i++) {
        b[i] = (unsigned char)value;
        value >>= 8;
    }

    return dm_bytes(out, b, (size_t)width);
}

bool dm_zip(const DmArchiveEntry *entries, size_t count, DmBytes *out, char *error, size_t cap) {
    DmBytes zip = {0};
    uint32_t offsets[64], checksums[64];
    bool ok = entries && count && count <= 64;

    /* Entries carry the local time of the export, in the DOS format ZIP uses. */
    time_t now = time(NULL);
    struct tm local = {0};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    uint32_t dos_time = (uint32_t)(local.tm_hour << 11 | local.tm_min << 5 | local.tm_sec / 2);
    uint32_t dos_date = (uint32_t)((local.tm_year < 80 ? 0 : local.tm_year - 80) << 9 |
                                   (local.tm_mon + 1) << 5 | local.tm_mday);

    for (size_t i = 0; i < count && ok; i++) {
        const DmArchiveEntry *e = &entries[i];
        size_t name = e->name ? strlen(e->name) : 0;
        ok = name && name <= 65535 && e->size <= UINT32_MAX && zip.size <= UINT32_MAX &&
             (!e->size || e->data);

        if (!ok) {
            break;
        }

        offsets[i] = (uint32_t)zip.size;
        uint32_t crc = UINT32_MAX;

        for (size_t j = 0; j < e->size; j++) {
            crc ^= e->data[j];

            for (int bit = 0; bit < 8; bit++) {
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
            }
        }

        checksums[i] = ~crc;
        ok = zip_le(&zip, 0x04034b50, 4) && zip_le(&zip, 20, 2) && zip_le(&zip, 0x800, 2) &&
             zip_le(&zip, 0, 2) && zip_le(&zip, dos_time, 2) && zip_le(&zip, dos_date, 2) &&
             zip_le(&zip, checksums[i], 4) && zip_le(&zip, (uint32_t)e->size, 4) &&
             zip_le(&zip, (uint32_t)e->size, 4) && zip_le(&zip, (uint32_t)name, 2) &&
             zip_le(&zip, 0, 2) && dm_bytes(&zip, e->name, name) &&
             dm_bytes(&zip, e->data, e->size);
    }

    size_t central = zip.size;

    for (size_t i = 0; i < count && ok; i++) {
        const DmArchiveEntry *e = &entries[i];
        size_t name = strlen(e->name);
        ok = zip_le(&zip, 0x02014b50, 4) && zip_le(&zip, 20, 2) && zip_le(&zip, 20, 2) &&
             zip_le(&zip, 0x800, 2) && zip_le(&zip, 0, 2) && zip_le(&zip, dos_time, 2) &&
             zip_le(&zip, dos_date, 2) && zip_le(&zip, checksums[i], 4) &&
             zip_le(&zip, (uint32_t)e->size, 4) && zip_le(&zip, (uint32_t)e->size, 4) &&
             zip_le(&zip, (uint32_t)name, 2) && zip_le(&zip, 0, 2) && zip_le(&zip, 0, 2) &&
             zip_le(&zip, 0, 2) && zip_le(&zip, 0, 2) && zip_le(&zip, 0, 4) &&
             zip_le(&zip, offsets[i], 4) && dm_bytes(&zip, e->name, name);
    }

    if (zip.size > UINT32_MAX) {
        ok = false;
    }

    size_t central_size = zip.size - central;
    ok = ok && zip_le(&zip, 0x06054b50, 4) && zip_le(&zip, 0, 2) && zip_le(&zip, 0, 2) &&
         zip_le(&zip, (uint32_t)count, 2) && zip_le(&zip, (uint32_t)count, 2) &&
         zip_le(&zip, (uint32_t)central_size, 4) && zip_le(&zip, (uint32_t)central, 4) &&
         zip_le(&zip, 0, 2);

    if (!ok) {
        dm_bytes_free(&zip);
        snprintf(error, cap, DM_TEXT_CANNOT_ASSEMBLE_EXPORT_ARCHIVE);
        return false;
    }

    dm_bytes_free(out);
    *out = zip;
    return true;
}

/* Song beat of a channel's first note, or HUGE_VAL when it has none. */
static double first_channel_note(const DmProject *p, int channel) {
    double first = HUGE_VAL;

    for (size_t i = 0; i < p->clip_count; ++i) {
        DmClip clip = p->clips[i];
        const DmPattern *pattern = &p->patterns[clip.pattern];

        if (clip.channel != channel || pattern->kind) {
            continue;
        }

        double end = clip.offset + dm_clip_length(p, clip);

        for (size_t n = 0; n < pattern->count; ++n) {
            DmNote note = pattern->notes[n];

            if (note.beat < end && note.beat + note.length > clip.offset) {
                first = fmin(first, clip.beat + fmax(0, note.beat - clip.offset));
            }
        }
    }

    return first;
}

bool dm_set_initial_program(DmProject *p, int channel, int program, bool apply) {
    if (channel < 0 || channel > 15 || program < 0 || program > 127) {
        return false;
    }

    bool changed = p->channels[channel].program != program || !p->channels[channel].enabled;

    if (apply) {
        p->channels[channel].enabled = true;
        p->channels[channel].program = program;
    }

    /* Imported files often send their opening program change after beat 0. Every program change
       before the channel's first note sets its starting instrument, so it follows this choice. */
    double first = first_channel_note(p, channel);

    for (size_t i = 0; i < p->event_count; ++i) {
        DmBytes *m = &p->events[i].message;

        if (m->size == 2 && m->data[0] == (MIDI_PROGRAM | channel) &&
            p->events[i].beat <= first + 1e-9 && m->data[1] != program) {
            if (apply) {
                m->data[1] = (uint8_t)program;
            }

            changed = true;
        }
    }

    return changed;
}

int dm_loop_marker(const DmBytes *m) {
    if (!m->size || m->size < 3 || m->data[0] != MIDI_META || m->data[1] != MIDI_META_MARKER ||
        m->size != (size_t)m->data[2] + 3) {
        return 0;
    }

    size_t length = m->data[2];
    const char *text = (const char *)m->data + 3;

    if (length == strlen(DM_LOOP_START_MARKER) && !memcmp(text, DM_LOOP_START_MARKER, length)) {
        return 1;
    }

    if (length == strlen(DM_LOOP_END_MARKER) && !memcmp(text, DM_LOOP_END_MARKER, length)) {
        return 2;
    }

    return 0;
}

bool dm_loop_points(const DmProject *p, double *start, double *end) {
    bool any = false;
    *start = *end = -1;

    for (size_t i = 0; i < p->event_count; ++i) {
        int kind = dm_loop_marker(&p->events[i].message);

        if (kind == 1 && *start < 0) {
            *start = p->events[i].beat;
            any = true;
        } else if (kind == 2 && *end < 0) {
            *end = p->events[i].beat;
            any = true;
        }
    }

    return any;
}
