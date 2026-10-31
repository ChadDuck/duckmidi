/* SM64 US sequence memory: plans, the install guide, and source patches.

   A sequence is loaded into the audio heap's temporary sequence pool. The stock US presets give
   that pool 0x3F00 to 0x7400 bytes. A larger pool must come from a larger audio heap, which takes
   RAM below the engine segment. The stock US build (n64decomp/sm64, refresh 16) leaves 0x114A0
   bytes there: growing the heap by 0x9100 linked with 0x83A0 bytes to spare. */
#include "m64_plugin.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STOCK_HEAP 0x31150u
#define STOCK_BUFFER 0x31200u
#define SPARE_RAM 0x114A0u
#define SMALLEST_POOL 0x3F00u
#define LOW_RATE_POOL 0x7400u
#define LOW_RATE_MACRO "DUCKMIDI_US_LOW_RATE_SEQ_MEM"
#define MARKER "duckmidi"

/* Temporary sequence pools of the eight US presets in gAudioSessionPresets. */
static const uint32_t us_pools[8] = {0x4400, 0x4400, 0x4400, 0x4400,
                                     0x4400, 0x3F00, 0x4400, 0x4000};

/* Fingerprints of the stock files (FNV-1a over the bytes, ignoring carriage returns). */
#define STOCK_DATA_C 0x71bbf55e6b0b6a6fULL
#define STOCK_DATA_H 0xaea46ef89a8e4521ULL
#define STOCK_BUFFERS_C 0xd971a5e4629c493cULL

uint64_t m64_fingerprint(const uint8_t *data, uint64_t size) {
    uint64_t hash = 1469598103934665603ULL;

    for (uint64_t i = 0; i < size; ++i) {
        if (data[i] != '\r') {
            hash = (hash ^ data[i]) * 1099511628211ULL;
        }
    }

    return hash;
}

bool m64_stock_memory_file(const char *path, const uint8_t *data, uint64_t size) {
    uint64_t hash = m64_fingerprint(data, size);

    return (!strcmp(path, M64_DATA_C) && hash == STOCK_DATA_C) ||
           (!strcmp(path, M64_DATA_H) && hash == STOCK_DATA_H) ||
           (!strcmp(path, M64_BUFFERS_C) && hash == STOCK_BUFFERS_C);
}

M64MemoryPlan m64_memory_plan(uint64_t required, bool maximum) {
    M64MemoryPlan plan = {0};
    uint64_t pool = maximum ? SMALLEST_POOL + SPARE_RAM : (required + 0xFF) & ~(uint64_t)0xFF;
    plan.required = required;
    plan.fits = required <= SMALLEST_POOL + SPARE_RAM;
    plan.needed = maximum || required > SMALLEST_POOL;

    if (!plan.needed || !plan.fits) {
        return plan;
    }

    plan.pool = (uint32_t)pool;
    plan.growth = (uint32_t)(pool - SMALLEST_POOL);
    plan.heap = STOCK_HEAP + plan.growth;
    plan.buffer = STOCK_BUFFER + plan.growth;
    plan.low_rate = pool > LOW_RATE_POOL ? (uint32_t)pool : LOW_RATE_POOL;
    return plan;
}

static void append(M64Text *t, const char *format, ...) {
    if (t->failed) {
        return;
    }

    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (length < 0 || t->size + (size_t)length + 1 > t->capacity) {
        size_t capacity = t->capacity ? t->capacity : 1024;

        while (length >= 0 && t->size + (size_t)length + 1 > capacity) {
            capacity *= 2;
        }

        char *grown = length < 0 ? NULL : realloc(t->data, capacity);

        if (!grown) {
            t->failed = true;
            va_end(args);
            return;
        }

        t->data = grown;
        t->capacity = capacity;
    }

    vsnprintf(t->data + t->size, t->capacity - t->size, format, args);
    t->size += (size_t)length;
    va_end(args);
}

static void append_bytes(M64Text *t, const char *data, size_t size) {
    append(t, "%.*s", (int)size, data);
}

void m64_text_free(M64Text *t) {
    free(t->data);
    *t = (M64Text){0};
}

static uint32_t new_pool(uint32_t stock, const M64MemoryPlan *plan) {
    return plan->pool > stock ? plan->pool : stock;
}

/* Writes the source edits for one memory plan as numbered instructions. */
static void guide_steps(M64Text *t, const M64MemoryPlan *plan) {
    append(t,
           "1. src/audio/data.c - the audio session presets (gAudioSessionPresets).\n"
           "   The eight US rows sit between \"#else\" and \"#endif\" in that table. Change each\n"
           "   row's ninth value, the memory for temporary sequences, so the song fits:\n\n");

    for (int i = 0; i < 8; ++i) {
        uint32_t value = new_pool(us_pools[i], plan);
        append(t, "     row %d: 0x%04X -> 0x%04X%s\n", i + 1, us_pools[i], value,
               value == us_pools[i] ? " (unchanged)" : "");
    }

    if (plan->low_rate > LOW_RATE_POOL) {
        append(t,
               "\n   The nine rows after \"#endif\" are shared with the JP version. Replace their\n"
               "   ninth value 0x7400 with %s, and add this above\n"
               "   \"struct AudioSessionSettings gAudioSessionPresets[18] = {\" so only US\n"
               "   builds change:\n\n"
               "     #ifdef VERSION_US\n     #define %s 0x%04X\n     #else\n"
               "     #define %s 0x7400\n     #endif\n",
               LOW_RATE_MACRO, LOW_RATE_MACRO, plan->low_rate, LOW_RATE_MACRO);
    }

    append(t,
           "\n2. src/audio/data.h - the audio heap size. The heap holds the sequence pool, so it\n"
           "   grows by the same amount. Replace\n\n"
           "     #define AUDIO_HEAP_SIZE 0x31150\n\n   with\n\n"
           "     #ifdef VERSION_US\n     #define AUDIO_HEAP_SIZE 0x%05X\n     #else\n"
           "     #define AUDIO_HEAP_SIZE 0x31150\n     #endif\n",
           plan->heap);
    append(
        t,
        "\n3. src/buffers/buffers.c - the buffer that backs the audio heap. Above the last\n"
        "   \"#else\" before \"ALIGNED16 u8 gAudioHeap[DOUBLE_SIZE_ON_64_BIT(0x31200)];\", add\n\n"
        "     #elif defined(VERSION_US)\n"
        "     ALIGNED16 u8 gAudioHeap[DOUBLE_SIZE_ON_64_BIT(0x%05X)];\n\n",
        plan->buffer);
    append(t, "4. Build for US: make VERSION=us COMPARE=0\n"
              "   The linker reports an overflow if other changes already use the free RAM.\n");
}

char *m64_memory_guide(uint64_t required, const char *sequence, bool maximum) {
    M64Text t = {0};
    M64MemoryPlan plan = m64_memory_plan(required, maximum);
    append(&t, "SM64 sequence memory for %s.m64\n\n", sequence);
    append(&t,
           "This song needs 0x%llX bytes of temporary sequence memory. Some stock US audio\n"
           "presets provide only 0x3F00; a song larger than its preset's pool overwrites\n"
           "instrument data and loses instruments.\n\n",
           (unsigned long long)required);

    if (!plan.fits) {
        append(&t,
               "The stock US memory layout can provide at most 0x%X bytes. Shorten or simplify\n"
               "the song, or move other data out of main RAM in your ROM.\n",
               SMALLEST_POOL + SPARE_RAM);
    } else {
        append(&t, "%s\n\n",
               maximum ? "Emulator Only: every US preset gets the largest pool the stock layout\n"
                         "can hold, using all of the free RAM. Later songs up to that size fit\n"
                         "without new edits, but other mods may no longer have room in main RAM."
                       : "Console-Compatible: the smallest change for this song. It uses part of\n"
                         "the RAM the stock build leaves free, so the ROM still runs on console.");
        guide_steps(&t, &plan);
    }

    append(&t,
           "\nHackerSM64 checkouts use one setting instead of the steps above:\n\n"
           "  src/audio/data.h - in the EXPAND_AUDIO_HEAP branch your config uses, make sure\n"
           "  TEMPORARY_SEQ_MEM is at least 0x%llX. The audio heap grows with it.\n",
           (unsigned long long)((required + 0xFF) & ~(uint64_t)0xFF));
    return t.failed ? (m64_text_free(&t), NULL) : t.data;
}

/* Line-based patching. Lines keep their own endings, so CRLF checkouts stay CRLF. */
typedef struct {
    const char *text;
    size_t length;
} Line;

static bool next_line(const char **cursor, const char *end, Line *line) {
    if (*cursor >= end) {
        return false;
    }

    const char *newline = memchr(*cursor, '\n', (size_t)(end - *cursor));
    line->text = *cursor;
    line->length = newline ? (size_t)(newline - *cursor) + 1 : (size_t)(end - *cursor);
    *cursor += line->length;
    return true;
}

static const char *ending(Line line) {
    if (line.length >= 2 && line.text[line.length - 2] == '\r') {
        return "\r\n";
    }

    return line.length && line.text[line.length - 1] == '\n' ? "\n" : "";
}

static bool starts(Line line, const char *prefix) {
    size_t i = 0;

    while (i < line.length && (line.text[i] == ' ' || line.text[i] == '\t')) {
        ++i;
    }

    size_t n = strlen(prefix);
    return line.length - i >= n && !memcmp(line.text + i, prefix, n);
}

static bool contains(Line line, const char *needle) {
    size_t n = strlen(needle);

    for (size_t i = 0; i + n <= line.length; ++i) {
        if (!memcmp(line.text + i, needle, n)) {
            return true;
        }
    }

    return false;
}

/* The table ends with an all-zero row, { 0, 0, ... }, that is not a preset. */
static bool zero_row(Line line) {
    for (size_t i = 0; i < line.length; ++i) {
        char c = line.text[i];

        if ((c >= '1' && c <= '9') || c == 'x' || c == 'X' || c == '_') {
            return false;
        }
    }

    return true;
}

/* Replaces the ninth value of a preset row, keeping its spacing. */
static bool replace_pool(M64Text *out, Line line, const char *value) {
    size_t commas = 0, start = 0, stop = 0;

    for (size_t i = 0; i < line.length; ++i) {
        if (line.text[i] == ',' && ++commas == 8) {
            start = i + 1;
        } else if (line.text[i] == ',' && commas == 9) {
            stop = i;
            break;
        }
    }

    if (!start || !stop) {
        return false;
    }

    while (start < stop && line.text[start] == ' ') {
        ++start;
    }

    append_bytes(out, line.text, start);
    append(out, "%s", value);
    append_bytes(out, line.text + stop, line.length - stop);
    return true;
}

bool m64_patch_data_c(const uint8_t *data, uint64_t size, const M64MemoryPlan *plan, M64Text *out,
                      char *error, size_t capacity) {
    const char *cursor = (const char *)data, *end = cursor + size;
    bool has_macro = false;
    Line line;

    while (next_line(&cursor, end, &line)) {
        has_macro |= starts(line, "#define " LOW_RATE_MACRO);
    }

    bool use_macro = plan->low_rate > LOW_RATE_POOL || has_macro;
    int section = 0, us_rows = 0, shared_rows = 0;
    bool in_us_macro = false;
    cursor = (const char *)data;

    /* section: 0 before the table, 1 table start, 2 JP rows, 3 US rows, 4 shared rows, 5 after. */
    while (next_line(&cursor, end, &line)) {
        const char *eol = ending(line);

        if (section == 0 && contains(line, "gAudioSessionPresets[18] = {")) {
            if (use_macro && !has_macro) {
                append(out,
                       "/* " MARKER ": temporary sequence memory of the US low-rate presets. */%s",
                       eol);
                append(out,
                       "#ifdef VERSION_US%s#define %s 0x%04X%s#else%s#define %s 0x7400%s#endif%s",
                       eol, LOW_RATE_MACRO, plan->low_rate, eol, eol, LOW_RATE_MACRO, eol, eol);
            }

            section = 1;
        } else if (starts(line, "#ifdef VERSION_US") && !section) {
            in_us_macro = true;
        } else if (starts(line, "#define " LOW_RATE_MACRO)) {
            append(out, "#define %s 0x%04X%s", LOW_RATE_MACRO,
                   in_us_macro ? plan->low_rate : 0x7400, eol);
            in_us_macro = false;
            continue;
        } else if (section == 1 && starts(line, "#ifdef VERSION_JP")) {
            section = 2;
        } else if (section == 2 && starts(line, "#else")) {
            section = 3;
        } else if (section == 3 && starts(line, "#endif")) {
            section = 4;
        } else if (section == 4 && starts(line, "};")) {
            section = 5;
        } else if (section == 3 && starts(line, "{")) {
            char value[16];
            snprintf(value, sizeof(value), "0x%04X",
                     us_rows < 8 ? new_pool(us_pools[us_rows], plan) : 0);

            if (us_rows >= 8 || !replace_pool(out, line, value)) {
                snprintf(error, capacity, "src/audio/data.c has an unexpected US preset table.");
                return false;
            }

            ++us_rows;
            continue;
        } else if (section == 4 && starts(line, "{") && !zero_row(line)) {
            if (use_macro && !replace_pool(out, line, LOW_RATE_MACRO)) {
                snprintf(error, capacity,
                         "src/audio/data.c has an unexpected low-rate preset row.");
                return false;
            }

            ++shared_rows;

            if (use_macro) {
                continue;
            }
        }

        append_bytes(out, line.text, line.length);
    }

    if (us_rows != 8 || shared_rows != 9 || out->failed) {
        snprintf(error, capacity,
                 out->failed
                     ? "Not enough memory to patch src/audio/data.c."
                     : "src/audio/data.c does not contain the expected gAudioSessionPresets "
                       "table. Apply sequence-memory.txt by hand.");
        return false;
    }

    return true;
}

bool m64_patch_data_h(const uint8_t *data, uint64_t size, const M64MemoryPlan *plan, M64Text *out,
                      char *error, size_t capacity) {
    const char *cursor = (const char *)data, *end = cursor + size;
    bool patched = false, previous_marker = false;
    Line line;

    while (next_line(&cursor, end, &line)) {
        const char *eol = ending(line);

        if (previous_marker && starts(line, "#define AUDIO_HEAP_SIZE")) {
            append(out, "#define AUDIO_HEAP_SIZE 0x%05X%s", plan->heap, eol);
            patched = true;
            previous_marker = false;
            continue;
        }

        previous_marker = starts(line, "#ifdef VERSION_US") && contains(line, MARKER);

        if (!patched && starts(line, "#define AUDIO_HEAP_SIZE 0x31150")) {
            append(out, "#ifdef VERSION_US /* " MARKER ": larger temporary sequence pools */%s",
                   eol);
            append(out, "#define AUDIO_HEAP_SIZE 0x%05X%s#else%s", plan->heap, eol, eol);
            append_bytes(out, line.text, line.length);
            append(out, "#endif%s", eol);
            patched = true;
            continue;
        }

        append_bytes(out, line.text, line.length);
    }

    if (!patched || out->failed) {
        snprintf(error, capacity,
                 "src/audio/data.h does not define the stock US AUDIO_HEAP_SIZE. Apply "
                 "sequence-memory.txt by hand.");
        return false;
    }

    return true;
}

bool m64_patch_buffers_c(const uint8_t *data, uint64_t size, const M64MemoryPlan *plan,
                         M64Text *out, char *error, size_t capacity) {
    const char *cursor = (const char *)data, *end = cursor + size;
    bool patched = false, previous_marker = false;
    Line line, pending = {0};

    while (next_line(&cursor, end, &line)) {
        const char *eol = ending(line);

        if (previous_marker && starts(line, "ALIGNED16 u8 gAudioHeap[")) {
            append(out, "ALIGNED16 u8 gAudioHeap[DOUBLE_SIZE_ON_64_BIT(0x%05X)];%s", plan->buffer,
                   eol);
            patched = true;
            previous_marker = false;
            continue;
        }

        previous_marker = starts(line, "#elif defined(VERSION_US)") && contains(line, MARKER);

        /* Hold an "#else" until the next line shows whether it guards the stock heap. */
        if (pending.text) {
            if (!patched && contains(line, "gAudioHeap[DOUBLE_SIZE_ON_64_BIT(0x31200)];")) {
                const char *held = ending(pending);
                append(out,
                       "#elif defined(VERSION_US) /* " MARKER
                       ": larger temporary sequence pools */%s",
                       held);
                append(out, "ALIGNED16 u8 gAudioHeap[DOUBLE_SIZE_ON_64_BIT(0x%05X)];%s",
                       plan->buffer, held);
                patched = true;
            }

            append_bytes(out, pending.text, pending.length);
            pending = (Line){0};
        }

        if (starts(line, "#else")) {
            pending = line;
            continue;
        }

        append_bytes(out, line.text, line.length);
    }

    if (pending.text) {
        append_bytes(out, pending.text, pending.length);
    }

    if (!patched || out->failed) {
        snprintf(error, capacity,
                 "src/buffers/buffers.c does not declare the stock gAudioHeap. Apply "
                 "sequence-memory.txt by hand.");
        return false;
    }

    return true;
}

bool m64_patch_sequences(const uint8_t *data, uint64_t size, const char *sequence, const char *bank,
                         M64Text *out, char *error, size_t capacity) {
    const char *cursor = (const char *)data, *end = cursor + size;
    char key[128];
    snprintf(key, sizeof(key), "\"%s\":", sequence);
    bool patched = false;
    Line line;

    while (next_line(&cursor, end, &line)) {
        if (!patched && starts(line, key)) {
            size_t indent = 0;

            while (indent < line.length &&
                   (line.text[indent] == ' ' || line.text[indent] == '\t')) {
                ++indent;
            }

            bool comma = false;

            for (size_t i = line.length; i > 0; --i) {
                char c = line.text[i - 1];

                if (c == ',') {
                    comma = true;
                    break;
                }

                if (c == ']') {
                    break;
                }
            }

            /* HackerSM64 also writes entries as {"ifdef": [...], "banks": [...]}; only the
               bank list changes there. */
            const char *banks = NULL;

            for (size_t i = 0; i + 8 <= line.length; ++i) {
                if (!memcmp(line.text + i, "\"banks\":", 8)) {
                    banks = line.text + i;
                    break;
                }
            }

            const char *open =
                banks ? memchr(banks, '[', line.length - (size_t)(banks - line.text)) : NULL;
            const char *close =
                open ? memchr(open, ']', line.length - (size_t)(open - line.text)) : NULL;

            if (banks && (!open || !close)) {
                break;
            }

            if (banks) {
                append_bytes(out, line.text, (size_t)(open - line.text));
                append(out, "[\"%s\"]", bank);
                append_bytes(out, close + 1, line.length - (size_t)(close + 1 - line.text));
            } else {
                append_bytes(out, line.text, indent);
                append(out, "%s [\"%s\"]%s%s", key, bank, comma ? "," : "", ending(line));
            }

            patched = true;
            continue;
        }

        append_bytes(out, line.text, line.length);
    }

    if (!patched || out->failed) {
        snprintf(error, capacity,
                 "sound/sequences.json has no \"%s\" entry. Export to Repository replaces an "
                 "existing song; choose one of the listed sequence names.",
                 sequence);
        return false;
    }

    return true;
}

/* HackerSM64 sizes the audio heap from TEMPORARY_SEQ_MEM and the other pools in
   src/audio/data.h, with one value per EXPAND_AUDIO_HEAP branch. The active branch is raised to
   the song's need; a larger value is left alone. */
bool m64_hacker_expanded(const uint8_t *config, uint64_t size) {
    const char *cursor = (const char *)config, *end = cursor + size;
    Line line;

    while (next_line(&cursor, end, &line)) {
        if (starts(line, "#define EXPAND_AUDIO_HEAP")) {
            return true;
        }
    }

    return false;
}

bool m64_patch_hacker_data_h(const uint8_t *data, uint64_t size, uint64_t required, bool expanded,
                             M64Text *out, bool *changed, char *error, size_t capacity) {
    const char *cursor = (const char *)data, *end = cursor + size;
    uint64_t pool = (required + 0xFF) & ~(uint64_t)0xFF;
    int branch = 0; /* 0 outside, 1 expanded branch, 2 default branch */
    bool found = false;
    Line line;
    *changed = false;

    while (next_line(&cursor, end, &line)) {
        if (starts(line, "#ifdef EXPAND_AUDIO_HEAP")) {
            branch = 1;
        } else if (branch == 1 && starts(line, "#else")) {
            branch = 2;
        } else if (branch && starts(line, "#endif")) {
            branch = 0;
        } else if (branch == (expanded ? 1 : 2) && starts(line, "#define TEMPORARY_SEQ_MEM")) {
            char text[96] = {0};
            memcpy(text, line.text,
                   line.length < sizeof(text) - 1 ? line.length : sizeof(text) - 1);
            const char *value = strstr(text, "TEMPORARY_SEQ_MEM");
            unsigned long current = value ? strtoul(value + 17, NULL, 0) : 0;
            found = current > 0;

            if (found && current < pool) {
                append(out, "#define TEMPORARY_SEQ_MEM 0x%llX /* " MARKER " */%s",
                       (unsigned long long)pool, ending(line));
                *changed = true;
                continue;
            }
        }

        append_bytes(out, line.text, line.length);
    }

    if (!found || out->failed) {
        snprintf(error, capacity,
                 out->failed ? "Not enough memory to patch src/audio/data.h."
                             : "src/audio/data.h does not define TEMPORARY_SEQ_MEM for this "
                               "HackerSM64 configuration. Set it by hand to at least 0x%llX.",
                 (unsigned long long)pool);
        return false;
    }

    return true;
}
