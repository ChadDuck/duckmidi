/* Catalog lookups: names, General MIDI defaults, families for substitutes, and sample ranges. */
#include "oot.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Soundfont titles after the community instrument lists (by the songs that use them). */
static const char *const font_titles[] = {
    [3] = "Main Orchestra",
    [5] = "Market",
    [6] = "Title Screen",
    [8] = "Kakariko Village",
    [9] = "Fairy Fountain",
    [10] = "Fire Temple",
    [11] = "Dodongo's Cavern",
    [12] = "Forest Temple",
    [13] = "Lon Lon Ranch",
    [15] = "Kokiri Forest",
    [17] = "Horse Race",
    [18] = "Warp Songs",
    [19] = "Legend of Hyrule",
    [20] = "Minigames",
    [21] = "Zora's Domain",
    [22] = "Shop",
    [23] = "Ice Cavern",
    [24] = "Shadow Temple",
    [25] = "Water Temple",
    [26] = "Unused Piano",
    [27] = "Gerudo Valley",
    [28] = "Lakeside Laboratory",
    [29] = "Kotake and Koume",
    [30] = "Ganon's Castle Organ",
    [31] = "Inside Ganon's Castle",
    [32] = "Ganondorf Battle",
    [33] = "Ending 1",
    [34] = "Ending 2",
    [35] = "Fanfares",
    [36] = "Owl",
};

enum {
    CLASS_KEYS,
    CLASS_MALLET,
    CLASS_ORGAN,
    CLASS_GUITAR,
    CLASS_BASS,
    CLASS_STRINGS,
    CLASS_VOICE,
    CLASS_BRASS,
    CLASS_REED,
    CLASS_PIPE,
    CLASS_PAD,
    CLASS_PERCUSSION,
    CLASS_EFFECT,
    CLASS_SYNTH
};

typedef struct {
    const char *name;
    int class_id;
} ClassName;

static const ClassName classes[] = {
    {"Piano", CLASS_KEYS},
    {"Harpsichord", CLASS_KEYS},
    {"Harp", CLASS_KEYS},
    {"Koto", CLASS_KEYS},
    {"Glockenspiel", CLASS_MALLET},
    {"Xylophone", CLASS_MALLET},
    {"Bell", CLASS_MALLET},
    {"Steel Drum", CLASS_MALLET},
    {"Organ", CLASS_ORGAN},
    {"Accordion", CLASS_ORGAN},
    {"Harmonica", CLASS_ORGAN},
    {"Guitar", CLASS_GUITAR},
    {"Electric Guitar", CLASS_GUITAR},
    {"Electric Guitar (high)", CLASS_GUITAR},
    {"Banjo", CLASS_GUITAR},
    {"Bass Guitar", CLASS_BASS},
    {"Strings", CLASS_STRINGS},
    {"Violin", CLASS_STRINGS},
    {"Pizzicato Strings", CLASS_STRINGS},
    {"Choir", CLASS_VOICE},
    {"Choir (low)", CLASS_VOICE},
    {"Choir (high)", CLASS_VOICE},
    {"Malon Voice", CLASS_VOICE},
    {"Chant", CLASS_VOICE},
    {"Chant 2", CLASS_VOICE},
    {"Voice Pad", CLASS_PAD},
    {"Trumpet", CLASS_BRASS},
    {"Trumpet (low)", CLASS_BRASS},
    {"Trumpet (high)", CLASS_BRASS},
    {"Trombone", CLASS_BRASS},
    {"Tuba", CLASS_BRASS},
    {"French Horn", CLASS_BRASS},
    {"Oboe", CLASS_REED},
    {"Clarinet", CLASS_REED},
    {"Bassoon", CLASS_REED},
    {"Flute", CLASS_PIPE},
    {"Ocarina", CLASS_PIPE},
    {"Forest Pad", CLASS_PAD},
    {"Bell Pad", CLASS_PAD},
    {"Hand Drum", CLASS_PERCUSSION},
    {"Hand Drum 2", CLASS_PERCUSSION},
    {"Clap", CLASS_PERCUSSION},
    {"Cymbal", CLASS_PERCUSSION},
    {"Tambourine", CLASS_PERCUSSION},
    {"Wood Chime", CLASS_PERCUSSION},
    {"Wind Chime", CLASS_PERCUSSION},
};

typedef struct {
    const char *name;
    int wave;
} Synth;

static const Synth synths[OOT_SYNTH_COUNT] = {
    {"Sawtooth wave", 128}, {"Triangle wave", 129},     {"Sine wave", 130},
    {"Square wave", 131},   {"Eighth-pulse wave", 134},
};

/* Preferred catalog name for each General MIDI program. */
static const char *const gm_names[128] = {
    "Piano",
    "Piano",
    "Piano",
    "Piano",
    "Piano",
    "Piano",
    "Harpsichord",
    "Harpsichord",
    "Glockenspiel",
    "Glockenspiel",
    "Bell",
    "Bell",
    "Xylophone",
    "Xylophone",
    "Bell",
    "Koto",
    "Organ",
    "Organ",
    "Organ",
    "Organ",
    "Organ",
    "Accordion",
    "Harmonica",
    "Accordion",
    "Guitar",
    "Guitar",
    "Guitar",
    "Electric Guitar",
    "Guitar",
    "Electric Guitar",
    "Electric Guitar",
    "Electric Guitar (high)",
    "Bass Guitar",
    "Bass Guitar",
    "Bass Guitar",
    "Bass Guitar",
    "Bass Guitar",
    "Bass Guitar",
    "Bass Guitar",
    "Bass Guitar",
    "Violin",
    "Strings",
    "Strings",
    "Strings",
    "Strings",
    "Pizzicato Strings",
    "Harp",
    "Pizzicato Strings",
    "Strings",
    "Strings",
    "Strings",
    "Strings",
    "Choir",
    "Voice Pad",
    "Voice Pad",
    "Strings",
    "Trumpet",
    "Trombone",
    "Tuba",
    "Trumpet",
    "French Horn",
    "Trumpet",
    "French Horn",
    "French Horn",
    "Clarinet",
    "Clarinet",
    "Clarinet",
    "Bassoon",
    "Oboe",
    "Oboe",
    "Bassoon",
    "Clarinet",
    "Flute",
    "Flute",
    "Flute",
    "Ocarina",
    "Ocarina",
    "Flute",
    "Ocarina",
    "Ocarina",
    "Square wave",
    "Sawtooth wave",
    "Ocarina",
    "Flute",
    "Electric Guitar",
    "Voice Pad",
    "Sawtooth wave",
    "Sawtooth wave",
    "Bell Pad",
    "Forest Pad",
    "Voice Pad",
    "Choir",
    "Strings",
    "Bell Pad",
    "Voice Pad",
    "Forest Pad",
    "Wind Chime",
    "Forest Pad",
    "Bell",
    "Voice Pad",
    "Bell Pad",
    "Dark Ambience",
    "Voice Pad",
    "Bell Pad",
    "Koto",
    "Banjo",
    "Koto",
    "Koto",
    "Xylophone",
    "Accordion",
    "Violin",
    "Oboe",
    "Bell",
    "Wood Chime",
    "Steel Drum",
    "Wood Chime",
    "Hand Drum",
    "Hand Drum",
    "Hand Drum",
    "Cymbal",
    "Guitar",
    "Wind",
    "Wind",
    "Ocarina",
    "Bell",
    "Wind",
    "Clap",
    "Clap",
};

enum { DRUM_KICK, DRUM_SNARE, DRUM_TOM, DRUM_CYMBAL, DRUM_HAND, DRUM_OTHER };

void oot_text_append(OotText *text, const char *data, size_t size) {
    if (text->failed) {
        return;
    }

    if (text->size + size + 1 > text->capacity) {
        size_t capacity = text->capacity ? text->capacity : 4096;

        while (capacity < text->size + size + 1) {
            capacity *= 2;
        }

        char *grown = realloc(text->data, capacity);

        if (!grown) {
            text->failed = true;
            return;
        }

        text->data = grown;
        text->capacity = capacity;
    }

    memcpy(text->data + text->size, data, size);
    text->size += size;
    text->data[text->size] = 0;
}

void oot_text_add(OotText *text, const char *format, ...) {
    char small[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(small, sizeof(small), format, args);
    va_end(args);

    if (length < 0) {
        text->failed = true;
        return;
    }

    if ((size_t)length < sizeof(small)) {
        oot_text_append(text, small, (size_t)length);
        return;
    }

    char *large = malloc((size_t)length + 1);

    if (!large) {
        text->failed = true;
        return;
    }

    va_start(args, format);
    vsnprintf(large, (size_t)length + 1, format, args);
    va_end(args);
    oot_text_append(text, large, (size_t)length);
    free(large);
}

void oot_text_free(OotText *text) {
    free(text->data);
    *text = (OotText){0};
}

void oot_note_name(int key, char *out, size_t capacity) {
    static const char *const notes[] = {"C",  "DF", "D",  "EF", "E",  "F",
                                        "GF", "G",  "AF", "A",  "BF", "B"};
    int octave = key / 12 - 1;

    if (octave < 0) {
        snprintf(out, capacity, "%sNEG%d", notes[key % 12], -octave);
    } else {
        snprintf(out, capacity, "%s%d", notes[key % 12], octave);
    }
}

bool oot_instrument_valid(int value) {
    return (value >= 0 && (unsigned)value < oot_instrument_count) ||
           (value >= OOT_SYNTH_BASE && value < OOT_SYNTH_BASE + OOT_SYNTH_COUNT);
}

bool oot_drum_valid(int value) {
    return value == OOT_SILENT || (value >= 0 && (unsigned)value < oot_drum_count);
}

int oot_synth_wave(int value) {
    return value >= OOT_SYNTH_BASE && value < OOT_SYNTH_BASE + OOT_SYNTH_COUNT
               ? synths[value - OOT_SYNTH_BASE].wave
               : -1;
}

const char *oot_font_title(int font) {
    return font >= 0 && font < (int)(sizeof(font_titles) / sizeof(*font_titles)) ? font_titles[font]
                                                                                 : NULL;
}

bool oot_music_font(int font) {
    for (unsigned i = 0; i < oot_font_entry_count; i++) {
        if (oot_font_entries[i].font == font) {
            return oot_font_title(font) != NULL;
        }
    }

    return false;
}

void oot_instrument_label(int value, char *out, size_t capacity) {
    if (value >= OOT_SYNTH_BASE && value < OOT_SYNTH_BASE + OOT_SYNTH_COUNT) {
        snprintf(out, capacity, "%s (synthesized)", synths[value - OOT_SYNTH_BASE].name);
        return;
    }

    if (value < 0 || (unsigned)value >= oot_instrument_count) {
        snprintf(out, capacity, "None");
        return;
    }

    const OotInstrument *in = &oot_instruments[value];
    const char *title = oot_font_title(in->font);
    bool twin = false;

    for (unsigned i = 0; i < oot_instrument_count; i++) {
        twin |= (int)i != value && oot_instruments[i].font == in->font &&
                !strcmp(oot_instruments[i].name, in->name);
    }

    if (twin) {
        snprintf(out, capacity, "%s (%s, #%u)", in->name, title ? title : "OoT", in->program);
    } else {
        snprintf(out, capacity, "%s (%s)", in->name, title ? title : "OoT");
    }
}

void oot_drum_label(int value, char *out, size_t capacity) {
    if (value < 0 || (unsigned)value >= oot_drum_count) {
        snprintf(out, capacity, "None (silent)");
        return;
    }

    const OotDrum *d = &oot_drums[value];

    if (!strncmp(d->sound, "sound ", 6)) {
        snprintf(out, capacity, "%s, %s", d->kit, d->sound);
    } else {
        snprintf(out, capacity, "%s (%s)", d->sound, d->kit);
    }
}

int oot_font_program(int font, int value) {
    for (unsigned i = 0; i < oot_font_entry_count; i++) {
        if (oot_font_entries[i].font == font && oot_font_entries[i].index == value) {
            return oot_font_entries[i].program;
        }
    }

    return -1;
}

int oot_font_drum_hi(int font, int drum) {
    for (unsigned i = 0; i < oot_font_drum_count; i++) {
        if (oot_font_drums[i].font == font && oot_font_drums[i].index == drum) {
            return oot_font_drums[i].program;
        }
    }

    return -1;
}

static int name_class(const char *name) {
    for (size_t i = 0; i < sizeof(classes) / sizeof(*classes); i++) {
        if (!strcmp(classes[i].name, name)) {
            return classes[i].class_id;
        }
    }

    return CLASS_EFFECT;
}

int oot_instrument_class(int value) {
    if (value >= OOT_SYNTH_BASE && value < OOT_SYNTH_BASE + OOT_SYNTH_COUNT) {
        return CLASS_SYNTH;
    }

    return value >= 0 && (unsigned)value < oot_instrument_count
               ? name_class(oot_instruments[value].name)
               : CLASS_EFFECT;
}

static bool available(int font, int value) {
    return oot_synth_wave(value) >= 0 || font == OOT_CUSTOM_FONT ||
           oot_font_program(font, value) >= 0;
}

static int find_named(int font, const char *name) {
    for (int i = 0; i < OOT_SYNTH_COUNT; i++) {
        if (!strcmp(synths[i].name, name)) {
            return OOT_SYNTH_BASE + i;
        }
    }

    for (unsigned i = 0; i < oot_instrument_count; i++) {
        if (!strcmp(oot_instruments[i].name, name) && available(font, (int)i)) {
            return (int)i;
        }
    }

    return -1;
}

int oot_default_instrument(int font, unsigned gm_program) {
    const char *name = gm_names[gm_program & 127];
    int found = find_named(font, name);

    if (found >= 0) {
        return found;
    }

    /* A vanilla font without that instrument: take one of the same family, else its first. */
    int wanted = name_class(name), first = -1;

    for (unsigned i = 0; i < oot_font_entry_count; i++) {
        const OotFontEntry *e = &oot_font_entries[i];

        if (e->font != font) {
            continue;
        }

        if (oot_instrument_class(e->index) == wanted) {
            return e->index;
        }

        if (first < 0) {
            first = e->index;
        }
    }

    return first >= 0 ? first : find_named(OOT_CUSTOM_FONT, "Piano");
}

int oot_gm_drum_class(unsigned key) {
    if (key == 35 || key == 36) {
        return DRUM_KICK;
    }

    if (key >= 37 && key <= 40) {
        return DRUM_SNARE;
    }

    if (key == 41 || key == 43 || key == 45 || key == 47 || key == 48 || key == 50) {
        return DRUM_TOM;
    }

    if (key == 42 || key == 44 || key == 46 || key == 49 || key == 51 || key == 52 || key == 53 ||
        key == 55 || key == 57 || key == 59) {
        return DRUM_CYMBAL;
    }

    if (key >= 60 && key <= 68) {
        return DRUM_HAND;
    }

    return DRUM_OTHER;
}

int oot_drum_class(int drum) {
    if (drum < 0 || (unsigned)drum >= oot_drum_count) {
        return DRUM_OTHER;
    }

    const char *sound = oot_drums[drum].sound;

    if (!strcmp(sound, "Kick")) {
        return DRUM_KICK;
    }

    if (!strncmp(sound, "Snare", 5)) {
        return DRUM_SNARE;
    }

    if (!strcmp(sound, "Timpani")) {
        return DRUM_TOM;
    }

    if (!strcmp(sound, "Crash Cymbal")) {
        return DRUM_CYMBAL;
    }

    return !strncmp(oot_drums[drum].kit, "Conga", 5) ? DRUM_HAND : DRUM_OTHER;
}

static int find_drum(const char *kit, const char *sound) {
    for (unsigned i = 0; i < oot_drum_count; i++) {
        if (!strcmp(oot_drums[i].kit, kit) && !strcmp(oot_drums[i].sound, sound)) {
            return (int)i;
        }
    }

    return OOT_SILENT;
}

static int custom_drum(unsigned key) {
    static const char *const orchestra = "Orchestra kit", *const conga = "Conga and bongo kit";

    switch (oot_gm_drum_class(key)) {
    case DRUM_KICK:
        return find_drum(orchestra, "Kick");
    case DRUM_SNARE:
        return find_drum(orchestra, key <= 38 ? "Snare" : "Snare 2");
    case DRUM_TOM:
        return find_drum(orchestra, "Timpani");
    case DRUM_CYMBAL:
        return find_drum(orchestra, "Crash Cymbal");
    case DRUM_HAND:
        return find_drum(conga, key <= 61 ? "sound 3" : "sound 2");
    default:
        break;
    }

    if (key == 54) {
        return find_drum("Tambourine kit", "sound 3");
    }

    if (key >= 69 && key <= 81) {
        return find_drum(conga, "sound 4");
    }

    return OOT_SILENT;
}

int oot_default_drum(int font, unsigned key) {
    if (font == OOT_CUSTOM_FONT) {
        return key >= 35 && key <= 81 ? custom_drum(key) : OOT_SILENT;
    }

    /* A vanilla kit keeps its own layout: the key plays whatever the font maps there. */
    for (unsigned i = 0; i < oot_font_drum_count; i++) {
        const OotFontEntry *e = &oot_font_drums[i];

        if (e->font == font && key >= oot_drums[e->index].lo && key <= (unsigned)e->program) {
            return e->index;
        }
    }

    return OOT_SILENT;
}

static int compare_choices(const void *left, const void *right) {
    return strcmp(((const DmeChoice *)left)->label, ((const DmeChoice *)right)->label);
}

unsigned oot_instrument_choices(int font, DmeChoice *choices, char (*labels)[96], unsigned max) {
    unsigned count = 0;

    for (int value = 0; value < OOT_SYNTH_BASE + OOT_SYNTH_COUNT && count < max; value++) {
        if (!oot_instrument_valid(value) || !available(font, value)) {
            continue;
        }

        oot_instrument_label(value, labels[count], 96);
        choices[count] = (DmeChoice){value, labels[count]};
        count++;
    }

    qsort(choices, count, sizeof(*choices), compare_choices);
    return count;
}

unsigned oot_drum_choices(int font, DmeChoice *choices, char (*labels)[96], unsigned max) {
    unsigned count = 0;

    for (unsigned d = 0; d < oot_drum_count && count < max; d++) {
        if (font != OOT_CUSTOM_FONT && oot_font_drum_hi(font, (int)d) < 0) {
            continue;
        }

        oot_drum_label((int)d, labels[count], 96);
        choices[count] = (DmeChoice){(int)d, labels[count]};
        count++;
    }

    qsort(choices, count, sizeof(*choices), compare_choices);

    if (count < max) {
        snprintf(labels[count], 96, "None (silent)");
        choices[count] = (DmeChoice){OOT_SILENT, labels[count]};
        count++;
    }

    return count;
}

double oot_instrument_rate(int value, unsigned key) {
    if (value < 0 || (unsigned)value >= oot_instrument_count) {
        return 1.0; /* Synthetic waves and silence */
    }

    const OotInstrument *in = &oot_instruments[value];

    for (unsigned r = 0; r < in->region_count; r++) {
        const OotRegion *region = &in->regions[r];

        if (key >= region->lo && key <= region->hi) {
            return region->rate / 32000.0 * pow(2.0, ((double)key - region->base) / 12.0);
        }
    }

    return 1.0;
}

unsigned oot_drum_key(int font, int drum, unsigned key) {
    if (drum < 0 || (unsigned)drum >= oot_drum_count) {
        return key;
    }

    int lo = oot_drums[drum].lo;
    int hi = font == OOT_CUSTOM_FONT ? oot_drums[drum].hi : oot_font_drum_hi(font, drum);

    if (hi < lo) {
        hi = lo;
    }

    int k = (int)key;

    /* Move by octaves into the sound's range so relative pitches survive, then clamp. */
    while (k < lo && k + 12 <= hi) {
        k += 12;
    }

    while (k > hi && k - 12 >= lo) {
        k -= 12;
    }

    return (unsigned)(k < lo ? lo : k > hi ? hi : k);
}

double oot_drum_rate(int font, int drum, unsigned key) {
    if (drum < 0 || (unsigned)drum >= oot_drum_count) {
        return 1.0;
    }

    const OotDrum *d = &oot_drums[drum];
    int played = (int)oot_drum_key(font, drum, key);
    int base = d->base - (played - d->lo);
    return d->rate / 32000.0 * pow(2.0, (60.0 - base) / 12.0);
}
