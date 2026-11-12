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
   A regular Events line used 127 for both input modes, so leave those ambiguous sections literal. */
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
}
