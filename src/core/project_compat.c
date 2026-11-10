/* Conversion of projects saved by older schema revisions.

   project.c reads older documents with the fields added since then left at their defaults. This
   file converts the remaining differences in meaning, one revision at a time, so a schema-9
   project passes through the schema-10 conversion before the schema-11 one. Schemas older than
   DM_PROJECT_SCHEMA_OLDEST are rejected when loading. */
#include "core/duck.h"
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
   edit points, and earlier projects play incoming MIDI notes as themselves. */
static void upgrade_14_to_15(DmProject *p) {
    for (size_t i = 0; i < p->pattern_count; ++i) {
        p->patterns[i].graph = 0;
    }

    memset(p->percussion_midi, 0, sizeof(p->percussion_midi));
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
}
