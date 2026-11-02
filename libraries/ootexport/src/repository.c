/* Patches for a zeldaret/oot checkout: sequence table, spec, audio cache sizes and heap. Each
   patch returns the whole new file and leaves the input untouched. */
#include "oot.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Songs that can be replaced, from include/tables/sequence_table.h. The Hyrule Field parts
   (driven by field_logic), the sound-effect and ambience programs and the file-select pointer are
   left out. */
const OotSong oot_songs[] = {
    {"NA_BGM_DUNGEON", "Dungeon"},
    {"NA_BGM_KAKARIKO_ADULT", "Kakariko Village (adult)"},
    {"NA_BGM_ENEMY", "Battle"},
    {"NA_BGM_BOSS", "Boss battle"},
    {"NA_BGM_INSIDE_DEKU_TREE", "Inside the Deku Tree"},
    {"NA_BGM_MARKET", "Market"},
    {"NA_BGM_TITLE", "Title theme"},
    {"NA_BGM_LINK_HOUSE", "Link's house"},
    {"NA_BGM_GAME_OVER", "Game over (fanfare)"},
    {"NA_BGM_BOSS_CLEAR", "Boss defeated"},
    {"NA_BGM_ITEM_GET", "Item get (fanfare)"},
    {"NA_BGM_OPENING_GANON", "Ganondorf appears"},
    {"NA_BGM_HEART_GET", "Heart container get (fanfare)"},
    {"NA_BGM_JABU_JABU", "Jabu-Jabu's Belly"},
    {"NA_BGM_KAKARIKO_KID", "Kakariko Village (child)"},
    {"NA_BGM_GREAT_FAIRY", "Great Fairy's Fountain"},
    {"NA_BGM_ZELDA_THEME", "Zelda's theme"},
    {"NA_BGM_FIRE_TEMPLE", "Fire Temple"},
    {"NA_BGM_OPEN_TRE_BOX", "Treasure chest (fanfare)"},
    {"NA_BGM_FOREST_TEMPLE", "Forest Temple"},
    {"NA_BGM_COURTYARD", "Hyrule Castle courtyard"},
    {"NA_BGM_GANON_TOWER", "Ganon's Tower"},
    {"NA_BGM_LONLON", "Lon Lon Ranch"},
    {"NA_BGM_GORON_CITY", "Goron City"},
    {"NA_BGM_SPIRITUAL_STONE", "Spiritual stone get (fanfare)"},
    {"NA_BGM_MINI_BOSS", "Mini-boss battle"},
    {"NA_BGM_SMALL_ITEM_GET", "Small item get (fanfare)"},
    {"NA_BGM_TEMPLE_OF_TIME", "Temple of Time"},
    {"NA_BGM_EVENT_CLEAR", "Puzzle solved (fanfare)"},
    {"NA_BGM_KOKIRI", "Kokiri Forest"},
    {"NA_BGM_SARIA_THEME", "Lost Woods"},
    {"NA_BGM_SPIRIT_TEMPLE", "Spirit Temple"},
    {"NA_BGM_HORSE", "Horse race"},
    {"NA_BGM_HORSE_GOAL", "Horse race goal"},
    {"NA_BGM_INGO", "Ingo"},
    {"NA_BGM_MEDALLION_GET", "Medallion get (fanfare)"},
    {"NA_BGM_NAVI_OPENING", "Navi's opening"},
    {"NA_BGM_DEKU_TREE_CS", "Deku Tree cutscene"},
    {"NA_BGM_WINDMILL", "Windmill hut"},
    {"NA_BGM_HYRULE_CS", "Hyrule cutscene"},
    {"NA_BGM_MINI_GAME", "Minigame"},
    {"NA_BGM_SHEIK", "Sheik"},
    {"NA_BGM_ZORA_DOMAIN", "Zora's Domain"},
    {"NA_BGM_ADULT_LINK", "Adult Link"},
    {"NA_BGM_MASTER_SWORD", "Master Sword"},
    {"NA_BGM_INTRO_GANON", "Ganondorf intro"},
    {"NA_BGM_SHOP", "Shop"},
    {"NA_BGM_CHAMBER_OF_SAGES", "Chamber of the Sages"},
    {"NA_BGM_ICE_CAVERN", "Ice Cavern"},
    {"NA_BGM_SHADOW_TEMPLE", "Shadow Temple"},
    {"NA_BGM_WATER_TEMPLE", "Water Temple"},
    {"NA_BGM_SEAL_OF_SAGES", "Seal of the Sages"},
    {"NA_BGM_GERUDO_VALLEY", "Gerudo Valley"},
    {"NA_BGM_POTION_SHOP", "Potion shop"},
    {"NA_BGM_KOTAKE_KOUME", "Kotake and Koume"},
    {"NA_BGM_ESCAPE", "Castle escape"},
    {"NA_BGM_UNDERGROUND", "Underground"},
    {"NA_BGM_GANONDORF_BOSS", "Ganondorf battle"},
    {"NA_BGM_GANON_BOSS", "Ganon battle"},
    {"NA_BGM_OCARINA_OF_TIME", "Ocarina of Time"},
    {"NA_BGM_STAFF_1", "Credits 1"},
    {"NA_BGM_STAFF_2", "Credits 2"},
    {"NA_BGM_STAFF_3", "Credits 3"},
    {"NA_BGM_STAFF_4", "Credits 4"},
    {"NA_BGM_FIRE_BOSS", "Volvagia battle"},
    {"NA_BGM_TIMED_MINI_GAME", "Timed minigame"},
};

const unsigned oot_song_count = sizeof(oot_songs) / sizeof(*oot_songs);

#define PERSISTENT_SEQ_CACHE 0x7F0u /* Shared by every persistent song loaded in a scene. */

static const char *line_end(const uint8_t *data, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        if (data[i] == '\n') {
            return i && data[i - 1] == '\r' ? "\r\n" : "\n";
        }
    }

    return "\n";
}

/* Iterates lines: the start and length exclude the line break; returns false at the end. */
static bool next_line(const uint8_t *data, uint64_t size, uint64_t *at, uint64_t *start,
                      uint64_t *length, uint64_t *next) {
    if (*at >= size) {
        return false;
    }

    *start = *at;
    uint64_t i = *at;

    while (i < size && data[i] != '\n') {
        i++;
    }

    *length = i - *start;

    if (*length && data[*start + *length - 1] == '\r') {
        (*length)--;
    }

    *next = i < size ? i + 1 : size;
    *at = *next;
    return true;
}

static bool line_has(const uint8_t *line, uint64_t length, const char *text) {
    size_t n = strlen(text);

    for (uint64_t i = 0; i + n <= length; i++) {
        if (!memcmp(line + i, text, n)) {
            return true;
        }
    }

    return false;
}

/* Arguments of a DEFINE_SEQUENCE(...) line, trimmed. Returns the count, at most 5. */
static unsigned table_arguments(const uint8_t *line, uint64_t length, char args[5][96],
                                uint64_t spans[5][2]) {
    uint64_t i = 0;

    while (i < length && line[i] != '(') {
        i++;
    }

    unsigned count = 0;
    i++;

    while (i < length && count < 5) {
        while (i < length && line[i] == ' ') {
            i++;
        }

        uint64_t begin = i;

        while (i < length && line[i] != ',' && line[i] != ')') {
            i++;
        }

        uint64_t end = i;

        while (end > begin && line[end - 1] == ' ') {
            end--;
        }

        size_t n = (size_t)(end - begin) < 95 ? (size_t)(end - begin) : 95;
        memcpy(args[count], line + begin, n);
        args[count][n] = 0;
        spans[count][0] = begin;
        spans[count][1] = end;
        count++;

        if (i >= length || line[i] == ')') {
            break;
        }

        i++;
    }

    return count;
}

static bool is_definition(const uint8_t *line, uint64_t length, bool *pointer) {
    uint64_t i = 0;

    while (i < length && isspace(line[i])) {
        i++;
    }

    static const char key[] = "DEFINE_SEQUENCE";

    if (length - i < sizeof(key) - 1 || memcmp(line + i, key, sizeof(key) - 1)) {
        return false;
    }

    *pointer = length - i >= sizeof(key) + 3 && !memcmp(line + i + sizeof(key) - 1, "_PTR", 4);
    return true;
}

bool oot_patch_sequence_table(const uint8_t *data, uint64_t size, const char *name,
                              const char *song, uint64_t sequence_bytes, OotText *out, char *notice,
                              size_t notice_capacity, char *error, size_t capacity) {
    char symbol[96], id[96];
    snprintf(symbol, sizeof(symbol), "Sequence_%s", name);

    if (song) {
        snprintf(id, sizeof(id), "%s", song);
    } else {
        snprintf(id, sizeof(id), "NA_BGM_%s", name);

        for (char *c = id; *c; c++) {
            *c = (char)toupper((unsigned char)*c);
        }
    }

    if (notice && notice_capacity) {
        *notice = 0;
    }

    uint64_t at = 0, start, length, next, last_definition = 0;
    unsigned entries = 0;
    bool found = false;
    *out = (OotText){0};

    while (next_line(data, size, &at, &start, &length, &next)) {
        const uint8_t *line = data + start;
        bool pointer;

        if (!is_definition(line, length, &pointer)) {
            continue;
        }

        entries++;
        last_definition = next;
        char args[5][96];
        uint64_t spans[5][2];

        if (table_arguments(line, length, args, spans) != 5) {
            snprintf(error, capacity, "Unexpected line in %s.", OOT_SEQUENCE_TABLE);
            return false;
        }

        if (strcmp(args[1], id)) {
            continue;
        }

        if (pointer) {
            snprintf(error, capacity, "%s points at another song; choose a different song.", id);
            return false;
        }

        found = true;
    }

    if (!entries) {
        snprintf(error, capacity, "%s has no DEFINE_SEQUENCE lines.", OOT_SEQUENCE_TABLE);
        return false;
    }

    if (song && !found) {
        snprintf(error, capacity, "%s is not in %s.", song, OOT_SEQUENCE_TABLE);
        return false;
    }

    if (!found && entries >= 0x7F) {
        snprintf(error, capacity, "The sequence table is full; OoT allows 127 songs.");
        return false;
    }

    const char *eol = line_end(data, size);

    if (!found) {
        /* A new song goes after the last entry and takes the next ID. */
        oot_text_append(out, (const char *)data, (size_t)last_definition);

        if (last_definition && data[last_definition - 1] != '\n') {
            oot_text_add(out, "%s", eol);
        }

        oot_text_add(out,
                     "DEFINE_SEQUENCE    (%s, %s, MEDIUM_CART, CACHE_LOAD_TEMPORARY, 0) // "
                     "added by duckmidi%s",
                     symbol, id, eol);
        oot_text_append(out, (const char *)data + last_definition,
                        (size_t)(size - last_definition));
        return !out->failed;
    }

    at = 0;

    while (next_line(data, size, &at, &start, &length, &next)) {
        const uint8_t *line = data + start;
        bool pointer;
        char args[5][96];
        uint64_t spans[5][2];

        if (!is_definition(line, length, &pointer) ||
            table_arguments(line, length, args, spans) != 5 || strcmp(args[1], id)) {
            oot_text_append(out, (const char *)line, (size_t)(next - start));
            continue;
        }

        /* Same line with the new sequence name; a large song also leaves the small persistent
           cache. */
        oot_text_append(out, (const char *)line, (size_t)spans[0][0]);
        oot_text_add(out, "%s", symbol);
        oot_text_append(out, (const char *)line + spans[0][1], (size_t)(spans[3][0] - spans[0][1]));

        if (!strcmp(args[3], "CACHE_LOAD_PERSISTENT") && sequence_bytes > PERSISTENT_SEQ_CACHE) {
            oot_text_add(out, "CACHE_LOAD_TEMPORARY");

            if (notice && notice_capacity) {
                snprintf(notice, notice_capacity,
                         "%s used the %u-byte persistent cache; the %llu-byte song loads into the "
                         "temporary cache instead.",
                         id, PERSISTENT_SEQ_CACHE, (unsigned long long)sequence_bytes);
            }
        } else {
            oot_text_append(out, (const char *)line + spans[3][0],
                            (size_t)(spans[3][1] - spans[3][0]));
        }

        oot_text_append(out, (const char *)line + spans[3][1],
                        (size_t)(next - start - spans[3][1]));
    }

    return !out->failed;
}

/* Finds a spec segment by name: the line after `name "<segment>"` through `endseg`. */
static bool find_segment(const uint8_t *data, uint64_t size, const char *segment, uint64_t *begin,
                         uint64_t *end) {
    char key[64];
    snprintf(key, sizeof(key), "name \"%s\"", segment);
    uint64_t at = 0, start, length, next;
    bool inside = false;

    while (next_line(data, size, &at, &start, &length, &next)) {
        if (!inside && line_has(data + start, length, key)) {
            inside = true;
            *begin = next;
        } else if (inside && line_has(data + start, length, "endseg")) {
            *end = start;
            return true;
        }
    }

    return false;
}

static bool segment_has(const uint8_t *data, uint64_t begin, uint64_t end, const char *text) {
    return line_has(data + begin, end - begin, text);
}

int oot_next_font_index(const uint8_t *data, uint64_t size, const char *name) {
    uint64_t begin, end;

    if (!find_segment(data, size, "Audiobank", &begin, &end)) {
        return -1;
    }

    char own[128];
    snprintf(own, sizeof(own), "soundfonts/Soundfont_%s.o\"", name);
    int count = 0;
    uint64_t at = begin, start, length, next;

    while (at < end && next_line(data, end, &at, &start, &length, &next)) {
        if (line_has(data + start, length, "assets/audio/soundfonts/") &&
            !line_has(data + start, length, own)) {
            count++;
        }
    }

    return count;
}

bool oot_patch_spec(const uint8_t *data, uint64_t size, const char *name, bool font, OotText *out,
                    char *error, size_t capacity) {
    uint64_t seq_begin, seq_end, bank_begin, bank_end;
    *out = (OotText){0};

    if (!find_segment(data, size, "Audioseq", &seq_begin, &seq_end) ||
        !find_segment(data, size, "Audiobank", &bank_begin, &bank_end)) {
        snprintf(error, capacity, "%s has no Audioseq or Audiobank segment.", OOT_SPEC);
        return false;
    }

    const char *eol = line_end(data, size);
    char seq_line[256], font_line[256];
    snprintf(seq_line, sizeof(seq_line),
             "    include \"$(BUILD_DIR)/assets/audio/sequences/%s.o\"%s", name, eol);
    snprintf(font_line, sizeof(font_line),
             "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_%s.o\"%s", name, eol);
    char seq_key[160], font_key[160];
    snprintf(seq_key, sizeof(seq_key), "assets/audio/sequences/%s.o\"", name);
    snprintf(font_key, sizeof(font_key), "assets/audio/soundfonts/Soundfont_%s.o\"", name);
    bool add_seq = !segment_has(data, seq_begin, seq_end, seq_key);
    bool add_font = font && !segment_has(data, bank_begin, bank_end, font_key);

    /* The soundfont goes after the last soundfont include, before any padding object. */
    uint64_t font_at = bank_begin, at = bank_begin, start, length, next;

    while (at < bank_end && next_line(data, bank_end, &at, &start, &length, &next)) {
        if (line_has(data + start, length, "assets/audio/soundfonts/")) {
            font_at = next;
        }
    }

    uint64_t cursor = 0;

    if (add_font && font_at < seq_end) {
        oot_text_append(out, (const char *)data, (size_t)font_at);
        oot_text_add(out, "%s", font_line);
        cursor = font_at;
        add_font = false;
    }

    if (add_seq) {
        oot_text_append(out, (const char *)data + cursor, (size_t)(seq_end - cursor));
        oot_text_add(out, "%s", seq_line);
        cursor = seq_end;
    }

    if (add_font) {
        oot_text_append(out, (const char *)data + cursor, (size_t)(font_at - cursor));
        oot_text_add(out, "%s", font_line);
        cursor = font_at;
    }

    oot_text_append(out, (const char *)data + cursor, (size_t)(size - cursor));
    return !out->failed;
}

/* Values of fields 14 and 15 (temporary sequence and font caches) of a gAudioSpecs row. */
bool oot_patch_session_config(const uint8_t *data, uint64_t size, uint32_t sequence_cache,
                              uint32_t font_cache, OotText *out, uint32_t *growth, char *error,
                              size_t capacity) {
    *out = (OotText){0};
    *growth = 0;
    const char *key = "gAudioSpecs[";
    uint64_t at = 0;

    while (at + strlen(key) <= size && memcmp(data + at, key, strlen(key))) {
        at++;
    }

    while (at < size && data[at] != '{') {
        at++;
    }

    if (at >= size) {
        snprintf(error, capacity, "%s has no gAudioSpecs table.", OOT_SESSION_CONFIG);
        return false;
    }

    uint64_t cursor = 0;
    unsigned rows = 0;
    at++;

    while (at < size) {
        while (at < size && data[at] != '{' && data[at] != '}') {
            at++;
        }

        if (at >= size || data[at] == '}') {
            break;
        }

        /* One row: split the top-level fields between { and }. */
        uint64_t fields[24][2];
        unsigned count = 0, depth = 0;
        uint64_t field_start = at + 1, i = at + 1;

        for (; i < size; i++) {
            char c = (char)data[i];

            if (c == '[' || c == '(') {
                depth++;
            } else if (c == ']' || c == ')') {
                depth--;
            } else if ((c == ',' || c == '}') && !depth) {
                if (count < 24) {
                    fields[count][0] = field_start;
                    fields[count][1] = i;
                    count++;
                }

                field_start = i + 1;

                if (c == '}') {
                    break;
                }
            }
        }

        if (count < 16) {
            snprintf(error, capacity, "Unexpected gAudioSpecs row in %s.", OOT_SESSION_CONFIG);
            oot_text_free(out);
            return false;
        }

        uint32_t wanted[2] = {sequence_cache, font_cache}, row_growth = 0;

        for (unsigned f = 0; f < 2; f++) {
            uint64_t begin = fields[14 + f][0], end = fields[14 + f][1];

            while (begin < end && isspace(data[begin])) {
                begin++;
            }

            while (end > begin && isspace(data[end - 1])) {
                end--;
            }

            char text[32];
            size_t n = (size_t)(end - begin) < 31 ? (size_t)(end - begin) : 31;
            memcpy(text, data + begin, n);
            text[n] = 0;
            char *stop;
            unsigned long value = strtoul(text, &stop, 0);

            if (stop == text || *stop) {
                snprintf(error, capacity, "Unexpected cache size \"%s\" in %s.", text,
                         OOT_SESSION_CONFIG);
                oot_text_free(out);
                return false;
            }

            if (value >= wanted[f]) {
                continue;
            }

            oot_text_append(out, (const char *)data + cursor, (size_t)(begin - cursor));
            oot_text_add(out, "0x%X", wanted[f]);
            cursor = end;
            row_growth += wanted[f] - (uint32_t)value;
        }

        *growth = row_growth > *growth ? row_growth : *growth;
        rows++;
        at = i + 1;
    }

    if (!rows) {
        snprintf(error, capacity, "%s has no gAudioSpecs rows.", OOT_SESSION_CONFIG);
        oot_text_free(out);
        return false;
    }

    oot_text_append(out, (const char *)data + cursor, (size_t)(size - cursor));
    return !out->failed;
}

bool oot_patch_audio_heap(const uint8_t *data, uint64_t size, uint32_t growth, OotText *out,
                          char *error, size_t capacity) {
    *out = (OotText){0};
    const char *key = "gAudioHeap[";
    uint64_t at = 0;

    while (at + strlen(key) <= size && memcmp(data + at, key, strlen(key))) {
        at++;
    }

    if (at + strlen(key) > size) {
        snprintf(error, capacity, "No gAudioHeap declaration found.");
        return false;
    }

    uint64_t begin = at + strlen(key), end = begin;

    while (end < size && data[end] != ']') {
        end++;
    }

    char text[32], old[48];
    size_t n = (size_t)(end - begin) < 31 ? (size_t)(end - begin) : 31;
    memcpy(text, data + begin, n);
    text[n] = 0;
    char *stop;
    unsigned long value = strtoul(text, &stop, 0);

    if (stop == text || *stop) {
        snprintf(error, capacity, "Unexpected gAudioHeap size \"%s\".", text);
        return false;
    }

    snprintf(old, sizeof(old), "%s bytes", text);
    oot_text_append(out, (const char *)data, (size_t)begin);
    oot_text_add(out, "0x%lX", value + growth);
    uint64_t cursor = end;

    /* Keep a "// 0x38000 bytes" comment in step with the size. */
    for (uint64_t i = end; i + strlen(old) <= size; i++) {
        if (data[i] == '\n') {
            break;
        }

        if (!memcmp(data + i, old, strlen(old))) {
            oot_text_append(out, (const char *)data + cursor, (size_t)(i - cursor));
            oot_text_add(out, "0x%lX bytes", value + growth);
            cursor = i + strlen(old);
            break;
        }
    }

    oot_text_append(out, (const char *)data + cursor, (size_t)(size - cursor));
    return !out->failed;
}

char *oot_install_guide(const char *name, const char *song, bool font, int font_index,
                        uint64_t sequence_bytes, uint64_t font_bytes) {
    OotText t = {0};
    char id[96];
    snprintf(id, sizeof(id), "NA_BGM_%s", name);

    for (char *c = id; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }

    oot_text_add(&t, "Installing %s in a zeldaret/oot checkout\n\n", name);
    oot_text_add(&t, "duckmidi's Export to Repository does these steps for you. To do them by "
                     "hand:\n\n");
    oot_text_add(&t, "1. Copy %s.seq to assets/audio/sequences/.\n", name);

    if (font) {
        oot_text_add(&t,
                     "2. Copy Soundfont_%s.xml to assets/audio/soundfonts/. Its Index is %d; it "
                     "must be the next free soundfont index (38 on a stock checkout).\n",
                     name, font_index);
    } else {
        oot_text_add(&t, "2. The song uses a vanilla soundfont; no soundfont file is needed.\n");
    }

    if (song) {
        oot_text_add(&t,
                     "3. In include/tables/sequence_table.h, change the first argument of the %s "
                     "line to Sequence_%s.\n",
                     song, name);
    } else {
        oot_text_add(&t,
                     "3. Add this line at the end of include/tables/sequence_table.h:\n"
                     "   DEFINE_SEQUENCE    (Sequence_%s, %s, MEDIUM_CART, CACHE_LOAD_TEMPORARY, "
                     "0)\n   Then play %s from a scene's sound settings or from code.\n",
                     name, id, id);
    }

    oot_text_add(&t,
                 "4. In spec/spec, add to the Audioseq segment:\n"
                 "   include \"$(BUILD_DIR)/assets/audio/sequences/%s.o\"\n",
                 name);

    if (font) {
        oot_text_add(&t,
                     "   and after the last soundfont in the Audiobank segment:\n"
                     "   include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_%s.o\"\n",
                     name);
    }

    oot_text_add(&t, "5. Run make.\n\nMemory\n\n");
    oot_text_add(&t,
                 "The sequence needs %llu bytes. Songs load into the temporary sequence cache "
                 "in gAudioSpecs (src/audio/game/session_config.c): 0x%X bytes in most scenes "
                 "and 0x%X in the smallest. A song that does not fit does not play.\n",
                 (unsigned long long)sequence_bytes, OOT_SEQ_CACHE_COMMON, OOT_SEQ_CACHE_MIN);

    if (font) {
        oot_text_add(&t,
                     "The soundfont needs about %llu bytes of the temporary font cache (0x%X "
                     "bytes in the smallest spec).\n",
                     (unsigned long long)font_bytes, OOT_FONT_CACHE);
    }

    oot_text_add(&t,
                 "To make room, raise temporarySeqCacheSize (and temporaryFontCacheSize) in every "
                 "gAudioSpecs row and grow gAudioHeap in src/buffers/audio_heap.c and "
                 "include/buffers.h by the largest increase, so the other audio pools keep "
                 "their size.\n");

    if (t.failed) {
        oot_text_free(&t);
        return NULL;
    }

    return t.data;
}
