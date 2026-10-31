/* Conversion of projects saved by the previous schema revision.

   project.c reads schema-9 documents with the fields added in schema 10 left at their defaults:
   pattern color 0 (application color), placement offset 0 and port program 0 (follow the
   channel). This file converts the remaining differences in meaning. Only the previous revision is
   converted; older schemas are rejected when loading. */
#include "duck.h"

/* Schema 9 stored the source port number on song events, and only the default note port played
   them. Schema 10 events name the note port they play on, so every schema-9 event moves to the
   default port and keeps sounding the same. */
static void upgrade_event_ports(DmProject *p) {
    for (size_t i = 0; i < p->event_count; ++i) {
        p->events[i].port = 0;
    }
}

void dm_project_upgrade_previous(DmProject *p) {
    upgrade_event_ports(p);
}
