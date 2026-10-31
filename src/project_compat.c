/* Conversion of projects saved by older schema revisions.

   project.c reads older documents with the fields added since then left at their defaults. This
   file converts the remaining differences in meaning, one revision at a time, so a schema-9
   project passes through the schema-10 conversion before the schema-11 one. Schemas older than
   DM_PROJECT_SCHEMA_OLDEST are rejected when loading. */
#include "duck.h"

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

void dm_project_upgrade(DmProject *p, uint32_t schema) {
    if (schema <= 9) {
        upgrade_9_to_10(p);
    }

    if (schema <= 10) {
        upgrade_10_to_11(p);
    }
}
