#include "core/project_internal.h"
#include <math.h>

/* Events that only take discrete points: no LFO and no graph. */
bool dm_point_only_event(int event) {
    return event == DM_EVENT_PROGRAM || event == DM_EVENT_CHANNEL_PRESSURE ||
           event == DM_EVENT_KEY_PRESSURE || event == MIDI_CC_PORTAMENTO_TIME ||
           event == MIDI_CC_SUSTAIN || event == MIDI_CC_SOSTENUTO;
}

bool dm_graph_resolution_valid(uint32_t graph) {
    return graph == 0 || (graph >= 8 && graph <= DM_GRAPH_MAX && !(graph & (graph - 1)));
}

static bool automation_event_valid(int event) {
    return event == DM_EVENT_PROGRAM || event == DM_EVENT_BEND ||
           event == DM_EVENT_CHANNEL_PRESSURE || event == DM_EVENT_KEY_PRESSURE ||
           (event >= 0 && event <= 127);
}

bool dm_pattern_settings_valid(const DmPattern *p) {
    if (p->kind < 0 || p->kind > 1 || p->mode < 0 || p->mode > 1 ||
        !automation_event_valid(p->event) || (p->mode && dm_point_only_event(p->event))) {
        return false;
    }

    if (!(p->speed >= .03125 && p->speed <= 16) || !(fabs(p->tension) <= 1) ||
        !(fabs(p->skew) <= 1) || !(p->pulse >= .05 && p->pulse <= .95) ||
        !(p->amount >= 0 && p->amount <= 1)) {
        return false;
    }

    if (p->center != floor(p->center) || p->center < 0 || p->center > 127 ||
        !(p->pitch_center >= 0 && p->pitch_center <= 127) || p->reference < 0 ||
        p->reference > 127) {
        return false;
    }

    if (p->port < 0 || p->port >= DM_NOTE_PORTS ||
        p->color > DM_PATTERN_COLORS) {
        return false;
    }

    for (unsigned lane = 0; lane < DM_GRAPH_LANES; ++lane) {
        uint16_t setting = p->graph_lanes[lane];

        if (!setting) {
            continue;
        }

        int event = lane == DM_GRAPH_PITCH_LANE ? DM_EVENT_BEND : (int)lane;

        if (dm_graph_lane(event) < 0 ||
            (setting != DM_GRAPH_POINT && !dm_graph_resolution_valid(setting))) {
            return false;
        }
    }

    return true;
}

bool dm_note_fields_valid(uint32_t key, uint32_t velocity, uint32_t selected, uint32_t port) {
    return key <= 127 && velocity >= 1 && velocity <= 127 && selected <= 1 && port < DM_NOTE_PORTS;
}

bool dm_channel_settings_valid(const DmChannel *c) {
    return c->program >= -1 && c->program <= 127 && c->volume >= 0 && c->volume <= 127 &&
           c->pan >= 0 && c->pan <= 127;
}

bool dm_note_rows_valid(int rows) {
    return rows >= 1 && rows <= DM_NOTE_ROWS;
}

bool dm_lane_count_valid(int count) {
    return count >= 0 && count <= DM_LANES;
}

static bool timing_valid(const DmProject *p) {
    if (!(p->bpm >= DM_BPM_MIN && p->bpm <= DM_BPM_MAX)) {
        return false;
    }

    if (p->numerator < 1 || p->numerator > 255) {
        return false;
    }

    return p->denominator >= 1 && p->denominator <= 128 &&
           (p->denominator & (p->denominator - 1)) == 0;
}

static bool source_valid(const DmProject *p) {
    if (p->port < 0 || p->port > DM_PORT_COMBINED || p->source_format < 0 || p->source_format > 2) {
        return false;
    }

    if (p->sequence_count < 1 || p->sequence_count > 65535 || p->sequence < 0 ||
        p->sequence >= p->sequence_count) {
        return false;
    }

    return p->port_count >= 1 && p->port_count <= 256;
}

bool dm_metadata_valid(const DmProject *p) {
    return timing_valid(p) && source_valid(p);
}

bool dm_percussion_valid(const DmProject *p) {
    bool assigned[DM_PIANO_KEYS + 1] = {0};
    bool heard[MIDI_KEYS + 1] = {0};

    for (unsigned i = 0; i < 128; ++i) {
        uint8_t key = p->percussion_keys[i];

        if (key > DM_PIANO_KEYS || (!p->percussion_override && key) || (key && assigned[key])) {
            return false;
        }

        assigned[key] = key != 0;
        uint8_t note = p->percussion_midi[i];

        if (note > MIDI_KEYS || (!p->percussion_override && note) || (note && heard[note])) {
            return false;
        }

        heard[note] = note != 0;
    }

    return true;
}

bool dm_port_mix_valid(DmPortMix mix) {
    return mix.port >= 1 && mix.port < DM_NOTE_PORTS && mix.channel >= 0 && mix.channel <= 15 &&
           mix.volume >= 0 && mix.volume <= 127 && mix.pan >= 0 && mix.pan <= 127 &&
           mix.program >= 0 && mix.program <= 128;
}

bool dm_automation_fields_valid(DmAutomation a) {
    if (a.port >= DM_NOTE_PORTS) {
        return false;
    }

    if (a.fixed_anchor && a.type != MIDI_PITCH_BEND) {
        return false;
    }

    if (a.bend_note && (a.type != MIDI_PITCH_BEND || a.curve <= 0 || a.pattern < 0)) {
        return false;
    }

    if (a.bend_root > DM_BEND_ROOT_MIDI ||
        (a.bend_root && (a.type != MIDI_PITCH_BEND || a.curve < 0 || a.pattern < 0 ||
                         (!a.curve && !a.fixed_anchor)))) {
        return false;
    }

    return true;
}
