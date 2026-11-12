/* The project model: channels, ports, patterns, notes, placements, automation and LFO shapes. */
#include "core/duck.h"
#include "core/midi_internal.h"
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
        p->note_rows[c] = 1;
        p->channels[c] = dm_channel_default(c);
        p->channels[c].enabled = c == 0 || c == MIDI_DRUM_CHANNEL;
        p->channels[c].program = c == MIDI_DRUM_CHANNEL ? 0 : -1;
    }
}

DmChannel dm_channel_default(int channel) {
    return (DmChannel){false, false, false, -1, 100, 64, channel == MIDI_DRUM_CHANNEL};
}

bool dm_channel_drums(const DmProject *p, int channel) {
    return channel >= 0 && channel < 16 && p->channels[channel].drums;
}

static int swapped_channel(int channel, int a, int b) {
    return channel == a ? b : channel == b ? a : channel;
}

/* GS rhythm-part setup belongs to the part it addresses, including imported setup messages.
   Keep its checksum valid when the part moves across the special channel-10 block. */
static void swap_rhythm_part(DmBytes *message, int a, int b) {
    int channel;
    bool drums;

    if (!dm_midi_rhythm_part(message, &channel, &drums)) {
        return;
    }

    int next = swapped_channel(channel, a, b);

    if (next == channel) {
        return;
    }

    Reader input = {message->data + 1, message->data + message->size, true};
    uint32_t size = dm_midi_read_vlq(&input);
    uint8_t *body = message->data + (input.p - message->data);
    uint32_t checksum = size - (body[size - 1] == MIDI_SYSEX_END ? 2 : 1);

    if (checksum < 8) {
        return;
    }

    uint8_t block = (uint8_t)(next == MIDI_DRUM_CHANNEL  ? 0
                              : next < MIDI_DRUM_CHANNEL ? next + 1
                                                         : next);
    uint8_t address = 0x10 | block;
    body[checksum] = (uint8_t)((body[checksum] + body[5] - address) & MIDI_DATA_MASK);
    body[5] = address;
}

bool dm_channel_swap(DmProject *p, int a, int b) {
    if (!p || a < 0 || a >= MIDI_CHANNELS || b < 0 || b >= MIDI_CHANNELS || a == b) {
        return false;
    }

    DmChannel channel = p->channels[a];
    p->channels[a] = p->channels[b];
    p->channels[b] = channel;
    int lanes = p->lane_count[a], rows = p->note_rows[a];
    p->lane_count[a] = p->lane_count[b];
    p->lane_count[b] = lanes;
    p->note_rows[a] = p->note_rows[b];
    p->note_rows[b] = rows;
    char names[DM_LANES][64];
    memcpy(names, p->lane_names[a], sizeof(names));
    memcpy(p->lane_names[a], p->lane_names[b], sizeof(names));
    memcpy(p->lane_names[b], names, sizeof(names));

    for (size_t i = 0; i < p->clip_count; ++i) {
        p->clips[i].channel = swapped_channel(p->clips[i].channel, a, b);
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        p->automation[i].channel = swapped_channel(p->automation[i].channel, a, b);
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        DmBytes *message = &p->events[i].message;

        if (message->size && message->data[0] >= MIDI_NOTE_OFF && message->data[0] < MIDI_SYSTEM) {
            int c = swapped_channel(message->data[0] & MIDI_CHANNEL_MASK, a, b);
            message->data[0] = (uint8_t)((message->data[0] & MIDI_STATUS_MASK) | c);
        } else {
            swap_rhythm_part(message, a, b);
        }
    }

    for (size_t i = 0; i < p->port_mix_count; ++i) {
        p->port_mix[i].channel = swapped_channel(p->port_mix[i].channel, a, b);
    }

    for (size_t i = 0; i < p->patch_replacement_count; ++i) {
        DmPatchKey *key = &p->patch_replacements[i].key;
        key->channel = (uint8_t)swapped_channel(key->channel, a, b);
    }

    return true;
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

bool dm_sample_id_valid(const char *text) {
    for (size_t i = 0; i < DM_SAMPLE_ID_CAP; ++i) {
        char c = text[i];

        if (!c) {
            return i > 0;
        }

        bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (!word && (i == 0 || (c != '-' && c != '_'))) {
            return false;
        }
    }

    return false;
}

static bool patch_key_equal(DmPatchKey a, DmPatchKey b) {
    return a.bank == b.bank && a.port == b.port && a.channel == b.channel &&
           a.program == b.program && a.drums == b.drums;
}

bool dm_patch_replacement_valid(const DmPatchReplacement *r) {
    if (r->key.bank > 16383 || r->key.port >= DM_NOTE_PORTS || r->key.channel >= 16 ||
        r->key.program > 127) {
        return false;
    }

    if (!r->generation) {
        return r->key.port && !r->locator[0] && !r->set_id[0] && !r->preset_id[0];
    }

    return r->locator[0] && memchr(r->locator, 0, sizeof(r->locator)) &&
           dm_sample_id_valid(r->set_id) && dm_sample_id_valid(r->preset_id);
}

const DmPatchReplacement *dm_patch_replacement_exact(const DmProject *p, DmPatchKey key) {
    for (size_t i = 0; i < p->patch_replacement_count; ++i) {
        if (patch_key_equal(p->patch_replacements[i].key, key)) {
            return &p->patch_replacements[i];
        }
    }

    return NULL;
}

const DmPatchReplacement *dm_patch_replacement(const DmProject *p, DmPatchKey key) {
    const DmPatchReplacement *found = dm_patch_replacement_exact(p, key);

    if (!found && key.port) {
        key.port = 0;
        found = dm_patch_replacement_exact(p, key);
    }

    return found;
}

bool dm_patch_replacement_set(DmProject *p, const DmPatchReplacement *r) {
    if (!dm_patch_replacement_valid(r)) {
        return false;
    }

    const DmPatchReplacement *found = dm_patch_replacement_exact(p, r->key);

    if (found) {
        p->patch_replacements[found - p->patch_replacements] = *r;
        return true;
    }

    size_t count = p->patch_replacement_count;

    if (count >= DM_PATCH_REPLACEMENTS_MAX ||
        !dm_reserve((void **)&p->patch_replacements, &p->patch_replacement_capacity, count + 1,
                    sizeof(*p->patch_replacements))) {
        return false;
    }

    p->patch_replacements[p->patch_replacement_count++] = *r;
    return true;
}

void dm_patch_replacement_clear(DmProject *p, DmPatchKey key) {
    const DmPatchReplacement *found = dm_patch_replacement_exact(p, key);

    if (found) {
        size_t index = (size_t)(found - p->patch_replacements);
        --p->patch_replacement_count;
        memmove(p->patch_replacements + index, p->patch_replacements + index + 1,
                (p->patch_replacement_count - index) * sizeof(*p->patch_replacements));
    }
}

void dm_channel_ports(const DmProject *p, int channel, bool used[DM_NOTE_PORTS]) {
    memset(used, 0, DM_NOTE_PORTS * sizeof(*used));

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
    free(p->patch_replacements);
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
    next.patch_replacements = NULL;
    next.patch_replacement_count = next.patch_replacement_capacity = 0;

    if (!dm_reserve((void **)&next.port_mix, &next.port_mix_capacity, source->port_mix_count,
                    sizeof(DmPortMix))) {
        goto fail;
    }

    if (source->port_mix_count) {
        memcpy(next.port_mix, source->port_mix, source->port_mix_count * sizeof(DmPortMix));
    }

    next.port_mix_count = source->port_mix_count;

    if (source->patch_replacement_count) {
        if (!dm_reserve((void **)&next.patch_replacements, &next.patch_replacement_capacity,
                        source->patch_replacement_count, sizeof(*next.patch_replacements))) {
            goto fail;
        }

        memcpy(next.patch_replacements, source->patch_replacements,
               source->patch_replacement_count * sizeof(*next.patch_replacements));
        next.patch_replacement_count = source->patch_replacement_count;
    }

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
    pat->event = DM_EVENT_BEND;
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
    memcpy(to->graph_lanes, from->graph_lanes, sizeof(to->graph_lanes));
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

static bool automation_series(DmAutomation a, DmAutomation b) {
    return a.pattern == b.pattern && a.port == b.port && a.type == b.type &&
           ((a.type != MIDI_CONTROL && a.type != MIDI_KEY_PRESSURE) || a.a == b.a);
}

static const DmAutomation *automation_curve_next(const DmProject *p, size_t index) {
    DmAutomation a = p->automation[index];

    for (size_t i = index + 1; i < p->automation_count; ++i) {
        if (automation_series(a, p->automation[i]) && a.curve == p->automation[i].curve) {
            return &p->automation[i];
        }
    }

    return NULL;
}

static DmAutomation automation_curve_node(DmAutomation a, DmAutomation b, double beat) {
    int value = (int)llround(dm_curve_value(a, b, (beat - a.beat) / (b.beat - a.beat)));
    a.beat = beat;

    if (a.type == MIDI_PITCH_BEND) {
        a.a = value & MIDI_DATA_MASK;
        a.b = value >> 7;
    } else if (a.type == MIDI_PROGRAM || a.type == MIDI_CHANNEL_PRESSURE) {
        a.a = value;
    } else {
        a.b = value;
    }

    return a;
}

/* Preserve authored nodes and identities; only the two cut boundaries need new nodes.
   Restricting a quadratic to a fraction of its span scales its midpoint bend by that fraction
   squared, just as splitting a curve node does in the editor. */
static bool pattern_trim_automation(const DmProject *p, DmClip clip, DmBytes *points) {
    double end = clip.offset + dm_clip_length(p, clip);

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation original = p->automation[i];

        if (original.pattern != clip.pattern || original.beat > end) {
            continue;
        }

        const DmAutomation *next = automation_curve_next(p, i);
        bool segment = dm_automation_curves(original) && next && next->beat > original.beat;
        DmAutomation a = original;

        if (a.beat < clip.offset) {
            bool last = true;

            for (size_t j = i + 1; last && j < p->automation_count; ++j) {
                last = !automation_series(original, p->automation[j]) ||
                       p->automation[j].beat > clip.offset;
            }

            if (!last) {
                continue;
            }

            if (segment && next->beat > clip.offset) {
                a = automation_curve_node(original, *next, clip.offset);
            } else {
                a.beat = clip.offset;
                a.bend = 0;
                a.ramp = false;
                /* An attached held bend still needs its line identity. */
                a.curve = a.bend_note ? a.curve : 0;

                if (!a.curve && a.bend_root) {
                    a.fixed_anchor = true;
                }
            }
        }

        if (segment && next->beat > a.beat) {
            double fraction = (fmin(next->beat, end) - a.beat) / (next->beat - original.beat);
            a.bend = original.bend * fraction * fraction;
        }

        a.beat -= clip.offset;

        if (!dm_bytes(points, &a, sizeof(a))) {
            return false;
        }

        if (segment && original.beat < end && next->beat > end) {
            a = automation_curve_node(original, *next, end);
            a.beat -= clip.offset;
            a.bend = 0;
            a.bend_note = 0;
            a.bend_root = DM_BEND_ROOT_NONE;

            if (!dm_bytes(points, &a, sizeof(a))) {
                return false;
            }
        }
    }

    /* A cut can remove a bend's root while keeping later nodes of its curve. Move the link to
       the earliest retained node; note bend ids remain pattern-local and do not need changing. */
    DmAutomation *items = (DmAutomation *)points->data;
    size_t count = points->size / sizeof(*items);

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation root = p->automation[i];
        size_t first = count;

        if (root.pattern != clip.pattern || !root.bend_note) {
            continue;
        }

        for (size_t j = 0; j < count; ++j) {
            if (automation_series(root, items[j]) && items[j].curve == root.curve) {
                items[j].bend_note = 0;

                if (first == count || items[j].beat < items[first].beat) {
                    first = j;
                }
            }
        }

        if (first < count) {
            items[first].bend_note = root.bend_note;
        }
    }

    return true;
}

int dm_clip_make_unique(DmProject *p, size_t index) {
    if (index >= p->clip_count) {
        return -1;
    }

    DmClip clip = p->clips[index];
    double length = dm_clip_length(p, clip);
    const DmPattern *source = &p->patterns[clip.pattern];
    bool sliced = !(source->kind && source->mode) && (clip.offset > 0 || length < source->length);
    DmBytes points = {0};

    if (sliced && !pattern_trim_automation(p, clip, &points)) {
        dm_bytes_free(&points);
        return -1;
    }
    int id = dm_pattern_copy(p, clip.pattern);

    if (id < 0 || !sliced) {
        dm_bytes_free(&points);

        if (id >= 0) {
            p->clips[index].pattern = id;
        }

        return id;
    }

    DmPattern *pattern = &p->patterns[id];
    size_t write = 0;

    for (size_t i = 0; i < pattern->count; ++i) {
        DmNote note = pattern->notes[i];
        double start = fmax(note.beat, clip.offset);
        double end = fmin(note.beat + note.length, clip.offset + length);

        if (end > start) {
            note.beat = start - clip.offset;
            note.length = end - start;
            pattern->notes[write++] = note;
        }
    }

    pattern->count = write;
    pattern->length = 1;
    write = 0;

    for (size_t i = 0; i < p->automation_count; ++i) {
        if (p->automation[i].pattern != id) {
            p->automation[write++] = p->automation[i];
        }
    }

    p->automation_count = write;
    DmAutomation *items = (DmAutomation *)points.data;
    bool ok = true;

    for (size_t i = 0; ok && i < points.size / sizeof(*items); ++i) {
        DmAutomation a = items[i];
        a.pattern = id;
        bool attached = false;

        for (size_t j = 0; a.bend_note && j < pattern->count; ++j) {
            attached |=
                pattern->notes[j].bend_id == a.bend_note && pattern->notes[j].port == a.port;
        }

        if (!attached) {
            a.bend_note = 0;
        }

        ok = dm_automation_add(p, a);
    }

    dm_bytes_free(&points);

    if (!ok) {
        dm_pattern_remove(p, id);
        return -1;
    }

    dm_pattern_fit(p, pattern);
    p->clips[index].pattern = id;
    p->clips[index].offset = p->clips[index].length = 0;
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

    if (p->event == DM_EVENT_BEND) {
        double pitch = fmax(0, fmin(127, p->pitch_center + 2 * wave));
        value = MIDI_BEND_CENTER + (pitch - p->reference) * MIDI_BEND_CENTER / dm_lfo_range(p);
    }

    return (int)llround(
        fmax(0, fmin(p->event == DM_EVENT_BEND ? MIDI_BEND_MAX : MIDI_DATA_MAX, value)));
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

    if (c.row < 0 || (c.lane ? c.row != 0 : c.row >= p->note_rows[c.channel])) {
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

int dm_graph_lane(int event) {
    switch (event) {
    case DM_EVENT_BEND:
        return DM_GRAPH_PITCH_LANE;
    case MIDI_CC_PORTAMENTO_TIME:
    case MIDI_CC_DATA:
    case MIDI_CC_DATA_LSB:
    case MIDI_CC_SUSTAIN:
    case MIDI_CC_SOSTENUTO:
    case MIDI_CC_RPN_LSB:
    case MIDI_CC_RPN_MSB:
        return -1;
    default:
        return event >= 0 && event < DM_GRAPH_PITCH_LANE ? event : -1;
    }
}

int dm_automation_graph_lane(DmAutomation point) {
    return point.type == MIDI_PITCH_BEND ? DM_GRAPH_PITCH_LANE
           : point.type == MIDI_CONTROL  ? dm_graph_lane(point.a)
                                         : -1;
}

int dm_graph_resolution(const DmPattern *pattern, int event) {
    int lane = dm_graph_lane(event);
    int setting = pattern && !pattern->mode && lane >= 0 ? pattern->graph_lanes[lane] : 0;
    return setting > DM_GRAPH_POINT ? setting : 0;
}

bool dm_pattern_has_graph(const DmPattern *pattern) {
    if (pattern->mode) {
        return false;
    }

    for (int lane = 0; lane < DM_GRAPH_LANES; ++lane) {
        if (pattern->graph_lanes[lane] > DM_GRAPH_POINT) {
            return true;
        }
    }

    return false;
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

/* The bend range at beat from a list of automation points in project order: the whole project's,
   or a subset holding at least the pattern's RPN and data entry controllers. */
DmPitchState dm_pitch_state(const DmAutomation *items, size_t count, int pattern_id, double beat,
                            unsigned port) {
    DmPitchState state = {2, 0, 127, 127};

    for (size_t i = 0; i < count; ++i) {
        DmAutomation a = items[i];

        if (a.beat > beat) {
            continue;
        }

        if (a.pattern != pattern_id || a.type != MIDI_CONTROL || a.port != port) {
            continue;
        }

        if (a.a == MIDI_CC_RPN_MSB) {
            state.msb = a.b;
        } else if (a.a == MIDI_CC_RPN_LSB) {
            state.lsb = a.b;
        } else if (state.msb == 0 && state.lsb == 0) {
            if (a.a == MIDI_CC_DATA) {
                state.range = a.b;
            }

            if (a.a == MIDI_CC_DATA_LSB) {
                state.cents = a.b;
            }
        }
    }

    return state;
}

double dm_pitch_range(const DmAutomation *items, size_t count, DmAutomation point) {
    DmPitchState state = dm_pitch_state(items, count, point.pattern, point.beat, point.port);

    for (size_t i = 0; point.curve > 0 && i < count; ++i) {
        DmAutomation a = items[i];

        if (a.pattern != point.pattern || a.port != point.port || a.curve != point.curve ||
            a.type != MIDI_CONTROL) {
            continue;
        }

        if (a.a == MIDI_CC_DATA) {
            state.range = a.b;
        } else if (a.a == MIDI_CC_DATA_LSB) {
            state.cents = a.b;
        }
    }

    return fmax(.01, state.range + state.cents * .01);
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
