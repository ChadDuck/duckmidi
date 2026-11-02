/* sm64coopdx mod files. A mod folder holds main.lua and the sequence in sound/; main.lua calls
   smlua_audio_utils_replace_sequence(sequence ID, bank, volume, name), which plays
   sound/<name>.m64 with one existing bank. Songs use the extended bank 0x2A (see
   ../src/coopdx.c). A stock sequence name replaces that song; another name gets a new sequence
   ID from smlua_audio_utils_allocate_sequence. */
#include "m64_plugin.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COOPDX_BANK 0x2A
#define COOPDX_VOLUME 75 /* Default volume of the stock level songs. */

void m64_coopdx_folder(const char *sequence, char *out, size_t capacity) {
    snprintf(out, capacity, "duckmidi_%s", sequence);
}

char *m64_coopdx_main_lua(const char *sequence) {
    size_t capacity = 2048;
    char *text = malloc(capacity);
    int stock = m64_stock_sequence(sequence);

    if (!text) {
        return NULL;
    }

    if (stock >= 0) {
        snprintf(text, capacity,
                 "-- name: duckmidi %s\n"
                 "-- description: Replaces %s with music exported by duckmidi.\n\n"
                 "smlua_audio_utils_replace_sequence(0x%02X, 0x%02X, %d, \"%s\")\n",
                 sequence, sequence, stock, COOPDX_BANK, COOPDX_VOLUME, sequence);
        return text;
    }

    char id[128] = "SEQ_";
    size_t length = 4;

    for (const char *s = sequence; *s && length + 1 < sizeof(id); s++) {
        id[length++] = isalnum((unsigned char)*s) ? (char)toupper((unsigned char)*s) : '_';
    }

    id[length] = 0;
    snprintf(text, capacity,
             "-- name: duckmidi %s\n"
             "-- description: Adds %s, music exported by duckmidi.\n\n"
             "-- A new sequence ID. Play the song from this mod with\n"
             "-- play_music(0, %s, 0) or set_background_music(0, %s, 0).\n"
             "%s = smlua_audio_utils_allocate_sequence()\n"
             "smlua_audio_utils_replace_sequence(%s, 0x%02X, %d, \"%s\")\n",
             sequence, sequence, id, id, id, id, COOPDX_BANK, COOPDX_VOLUME, sequence);
    return text;
}
