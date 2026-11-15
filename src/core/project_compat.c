/* Conversion of projects saved by older schema revisions.

   project.c reads older documents with the fields added since then left at their defaults. This
   file converts the remaining differences in meaning, one revision at a time, so a schema-9
   project passes through the schema-10 conversion before the schema-11 one. Schemas older than
   DM_PROJECT_SCHEMA_OLDEST are rejected when loading. */
#include "core/duck.h"
#include <math.h>
#include <string.h>

/* Schema 9 stored the source port number on song events, and only the default note port played
   them. Schema 10 events name the note port they play on, so every schema-9 event moves to the
   default port and keeps sounding the same. Schema 10 also added the pattern color, placement
   offset and port program, whose defaults (application color, offset 0, follow the channel)
   keep schema-9 projects unchanged. */
static void upgrade_9_to_10(DmProject *p) {
    for (size_t i = 0; i < p->event_count; ++i) {
        p->events[i].port = 0;
    }
}

/* Schema 11 adds the event point hold flag. Schema-10 line nodes always curve toward the next
   node, which is hold off, so the default needs no change. */
static void upgrade_10_to_11(DmProject *p) {
    for (size_t i = 0; i < p->automation_count; ++i) {
        p->automation[i].hold = false;
    }
}

/* Schema 12 marks percussion channels. Earlier projects played drums on channel 10 only. */
static void upgrade_11_to_12(DmProject *p) {
    for (int c = 0; c < 16; ++c) {
        p->channels[c].drums = c == MIDI_DRUM_CHANNEL;
    }
}

/* Schema 13 adds explicit patch replacements. Existing routes keep the selected SoundFont. */
static void upgrade_12_to_13(DmProject *p) {
    p->patch_replacement_count = 0;
}

/* Schema 14 references the current sample state instead of a stored revision. */
static void upgrade_13_to_14(DmProject *p) {
    for (size_t i = 0; i < p->patch_replacement_count; ++i) {
        p->patch_replacements[i].generation = p->patch_replacements[i].generation != 0;
    }
}

/* Schema 15 adds the pattern graph resolution and the percussion MIDI mapping. Earlier patterns
   default to Point, and earlier projects play incoming MIDI notes as themselves. */
static void upgrade_14_to_15(DmProject *p) {
    memset(p->percussion_midi, 0, sizeof(p->percussion_midi));
}

/* Schema 15 used the same resolution for every eligible event type. The reader puts that
   resolution in the pitch bend slot until it can be copied to each eligible lane. */
static void upgrade_15_to_16(DmProject *p) {
    for (size_t i = 0; i < p->pattern_count; ++i) {
        DmPattern *pattern = &p->patterns[i];
        uint16_t graph = pattern->graph_lanes[DM_GRAPH_PITCH_LANE];

        if (!graph) {
            continue;
        }

        for (int cc = 0; cc < DM_GRAPH_PITCH_LANE; ++cc) {
            if (dm_graph_lane(cc) >= 0) {
                pattern->graph_lanes[cc] = graph;
            }
        }
    }
}

/* The old piano-roll tool drew a centered point on the note with a range of at least 12.
   A normal Events line used range 127. Restore clear matches and leave the rest independent. */
static void upgrade_16_to_17(DmProject *p) {
    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation *root = &p->automation[i];

        if (root->type != MIDI_PITCH_BEND || root->curve <= 0 || root->pattern < 0 ||
            root->a + (root->b << 7) != MIDI_BEND_CENTER || p->patterns[root->pattern].kind) {
            continue;
        }

        DmPattern *pattern = &p->patterns[root->pattern];
        size_t best = pattern->count;
        double distance = HUGE_VAL;

        for (size_t n = 0; n < pattern->count; ++n) {
            DmNote note = pattern->notes[n];
            double delta = fabs(note.beat - root->beat);

            if (note.port == root->port && note.key == root->anchor && delta < distance &&
                root->beat >= note.beat - .25 && root->beat <= note.beat + note.length + .25) {
                best = n;
                distance = delta;
            }
        }

        if (best == pattern->count) {
            continue;
        }

        bool has_root = false, piano_range = false;

        for (size_t j = 0; j < p->automation_count; ++j) {
            DmAutomation point = p->automation[j];

            if (point.pattern == root->pattern && point.port == root->port &&
                point.curve == root->curve) {
                has_root |= point.bend_note != 0;
                piano_range |= point.type == MIDI_CONTROL && point.a == MIDI_CC_DATA &&
                               point.b >= 12 && point.b < 127;
            }
        }

        if (piano_range && !has_root) {
            pattern->notes[best].bend_id = (uint32_t)best + 1;
            root->bend_note = pattern->notes[best].bend_id;
        }
    }
}

/* Schema 17 retained the original note identity on attached roots, but did not record input
   units on detached sections. The old piano-roll tool used ranges 12–126 for ordinary bends.
   A regular Events line used 127 for both input modes, so leave those ambiguous sections literal.
 */
static void upgrade_17_to_18(DmProject *p) {
    for (size_t i = 0; i < p->automation_count; ++i) {
        if (p->automation[i].bend_note) {
            p->automation[i].bend_root = DM_BEND_ROOT_PITCH;
        }
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation setup = p->automation[i];

        if (setup.pattern < 0 || setup.curve <= 0 || setup.type != MIDI_CONTROL ||
            setup.a != MIDI_CC_DATA || setup.b < 12 || setup.b >= 127) {
            continue;
        }

        DmAutomation *first = NULL;

        for (size_t j = i + 1; j < p->automation_count; ++j) {
            DmAutomation *point = &p->automation[j];

            if (point->pattern != setup.pattern || point->port != setup.port ||
                point->curve != setup.curve || point->type != MIDI_PITCH_BEND) {
                continue;
            }

            if (point->bend_root) {
                first = NULL;
                break;
            }

            if (!first) {
                first = point;
            }
        }

        if (first && first->fixed_anchor) {
            first->bend_root = DM_BEND_ROOT_PITCH;
        }
    }
}

/* Schema 19 adds parallel note rows. Every older placement belongs to the first row. */
static void upgrade_18_to_19(DmProject *p) {
    for (int c = 0; c < 16; ++c) {
        p->note_rows[c] = 1;
    }

    for (size_t i = 0; i < p->clip_count; ++i) {
        p->clips[i].row = 0;
    }
}

/* Schema 20 changes the body's layout to tagged records (project_fields.c), not the meaning of
   anything stored, so there is nothing to convert. */
static void upgrade_19_to_20(DmProject *p) {
    (void)p;
}

/* Schema 21 stores song-level channel messages only as raw events. Each automation point with
   pattern -1 becomes the channel message it played:
   - program, CC7 or CC10 at beat 0 sets the channel mixer (note port 0) or that port's mixer
     entry: playback emits the mixer's values at tick 0 and skips raw beat-0 volume and pan (and
     plays a raw beat-0 program as the mixer's), while it played the point after the mixer, so
     its value is the one that was heard. A program keeps its event as well (played as the
     mixer program in its place, after a beat-0 bank select), and so does any point on another
     note port, so the port still counts as used and receives its channel setup. Beat-0 volume
     and pan on a disabled channel are no longer sent: the mixer only plays enabled channels;
   - anything else becomes an event at the same beat and note port, after the events already at
     that beat, as playback collected raw events before song-level points.
   Fields song-level playback never read (curve, ramp, hold, bend, anchor) are dropped. A point
   that cannot be converted (no memory) stays, and the load then fails validation. */
static bool song_point_message(DmAutomation a, DmBytes *message) {
    uint8_t bytes[] = {(uint8_t)(a.type | a.channel), (uint8_t)a.a, (uint8_t)a.b};
    size_t size = a.type == MIDI_PROGRAM || a.type == MIDI_CHANNEL_PRESSURE ? 2 : 3;
    return dm_bytes(message, bytes, size);
}

static void song_point_mixer(DmProject *p, DmAutomation a) {
    bool program = a.type == MIDI_PROGRAM;
    bool volume = a.type == MIDI_CONTROL && a.a == MIDI_CC_VOLUME;

    if (!a.port) {
        DmChannel *channel = &p->channels[a.channel];
        *(program  ? &channel->program
          : volume ? &channel->volume
                   : &channel->pan) = program ? a.a : a.b;
        return;
    }

    DmPortMix mix = dm_port_mix(p, a.port, a.channel);
    *(program ? &mix.program : volume ? &mix.volume : &mix.pan) = program ? a.a + 1 : a.b;
    dm_port_mix_set(p, mix);
}

static void upgrade_20_to_21(DmProject *p) {
    size_t kept = 0;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];

        if (a.pattern >= 0) {
            p->automation[kept++] = a;
            continue;
        }

        bool setup = a.beat == 0 &&
                     (a.type == MIDI_PROGRAM ||
                      (a.type == MIDI_CONTROL && (a.a == MIDI_CC_VOLUME || a.a == MIDI_CC_PAN)));
        DmBytes message = {0};
        bool converted = true;

        if (setup) {
            song_point_mixer(p, a);
        }

        /* A program keeps its event too: playback plays it as the mixer program in its place
           among the raw events, after a beat-0 bank select that precedes it. */
        if (!setup || a.port || a.type == MIDI_PROGRAM) {
            converted =
                song_point_message(a, &message) && dm_event_add(p, a.beat, 0, a.port, &message);
            dm_bytes_free(&message);
        }

        if (!converted) {
            p->automation[kept++] = a;
        }
    }

    p->automation_count = kept;
}

void dm_project_upgrade(DmProject *p, uint32_t schema) {
    if (schema <= 9) {
        upgrade_9_to_10(p);
    }

    if (schema <= 10) {
        upgrade_10_to_11(p);
    }

    if (schema <= 11) {
        upgrade_11_to_12(p);
    }

    if (schema <= 12) {
        upgrade_12_to_13(p);
    }

    if (schema <= 13) {
        upgrade_13_to_14(p);
    }

    if (schema <= 14) {
        upgrade_14_to_15(p);
    }

    if (schema <= 15) {
        upgrade_15_to_16(p);
    }

    if (schema <= 16) {
        upgrade_16_to_17(p);
    }

    if (schema <= 17) {
        upgrade_17_to_18(p);
    }

    if (schema <= 18) {
        upgrade_18_to_19(p);
    }

    if (schema <= 19) {
        upgrade_19_to_20(p);
    }

    if (schema <= 20) {
        upgrade_20_to_21(p);
    }

    /* Schemas 22 and 23 rewrite nothing. Schema 22 turned a pattern's control scope 2 into 1, and
       schema 23 drops the scope: the patterns Generate Patterns separated a note pattern's
       controls into rank like every other automation pattern (newest placed point, then higher
       row). The readers take the retired field from older files and discard it
       (project_fields.c PATTERN_CONTROL_SCOPE, project_legacy.c). */
}
