/* Generates libraries/ootexport/src/catalog_data.c from a zeldaret/oot audio extraction.

   Build and run from the repository root (MSYS2 UCRT64 GCC on Windows, or any C17 compiler):

     gcc -std=c17 -O2 -o build/development/oot_catalog tools/oot_catalog.c
     build/development/oot_catalog <oot>/extracted/ntsc-1.0/assets/audio \
         libraries/ootexport/src/catalog_data.c

   It reads the music soundfont XMLs that use sample bank 0, the sample bank XML and each WAV
   header (sample rate and root key). Instrument names come from the community lists below; the
   decomp leaves instruments and samples unnamed. The output holds only names, tuning data and the
   XML attributes needed to rebuild an instrument; it contains no audio. */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Font and program names, after LuigiXHero's OoT instrument list (slot = program + 1) and
   TheSoundDefense's randomizer music guide. Fonts appear in the order their instruments are
   preferred when two fonts define the same sound. */
typedef struct {
    int font, program;
    const char *name;
} Name;

static const Name names[] = {
    {3, 0, "Flute"},
    {3, 1, "Oboe"},
    {3, 2, "Clarinet"},
    {3, 3, "Bassoon"},
    {3, 4, "French Horn"},
    {3, 5, "Trumpet"},
    {3, 6, "Trombone"},
    {3, 7, "Tuba"},
    {3, 8, "Glockenspiel"},
    {3, 10, "Strings"},
    {3, 11, "Strings"},
    {3, 12, "Pizzicato Strings"},
    {3, 13, "Piano"},
    {3, 14, "Harp"},
    {3, 15, "Xylophone"},
    {15, 0, "Flute"},
    {15, 1, "Oboe"},
    {15, 2, "Clarinet"},
    {15, 3, "Bassoon"},
    {15, 4, "French Horn"},
    {15, 8, "Glockenspiel"},
    {15, 10, "Strings"},
    {15, 11, "Strings"},
    {15, 12, "Pizzicato Strings"},
    {15, 13, "Harpsichord"},
    {15, 14, "Harp"},
    {15, 15, "Xylophone"},
    {32, 0, "Flute"},
    {32, 1, "Choir"},
    {32, 2, "Choir (low)"},
    {32, 4, "French Horn"},
    {32, 5, "Trumpet"},
    {32, 6, "Trombone"},
    {32, 7, "Tuba"},
    {32, 10, "Strings"},
    {32, 11, "Strings"},
    {32, 13, "Piano"},
    {32, 15, "Xylophone"},
    {33, 0, "Malon Voice"},
    {33, 1, "Malon Voice"},
    {33, 2, "Clarinet"},
    {33, 4, "French Horn"},
    {33, 5, "Oboe"},
    {33, 6, "Harp"},
    {33, 7, "Violin"},
    {33, 8, "Glockenspiel"},
    {33, 10, "Strings"},
    {33, 11, "Strings"},
    {33, 13, "Bell"},
    {33, 14, "Harp"},
    {33, 15, "Choir"},
    {34, 0, "Koto"},
    {34, 1, "Ocarina"},
    {34, 2, "Bassoon"},
    {34, 3, "Oboe"},
    {34, 4, "Choir"},
    {34, 5, "Tambourine"},
    {34, 6, "Harp"},
    {34, 7, "Glockenspiel"},
    {34, 8, "Malon Voice"},
    {34, 10, "Strings"},
    {34, 11, "Strings"},
    {34, 12, "Pizzicato Strings"},
    {34, 13, "French Horn"},
    {34, 14, "Choir"},
    {34, 15, "Sound Effect"},
    {35, 5, "Trumpet"},
    {35, 6, "Trombone"},
    {35, 7, "Tuba"},
    {35, 8, "Glockenspiel"},
    {35, 10, "Strings"},
    {35, 11, "Strings"},
    {35, 14, "Harp"},
    {36, 1, "Oboe"},
    {36, 3, "Bassoon"},
    {36, 10, "Strings"},
    {36, 11, "Strings"},
    {36, 12, "Pizzicato Strings"},
    {36, 14, "Harp"},
    {9, 0, "Harp"},
    {9, 1, "Harp"},
    {9, 2, "Harp"},
    {9, 3, "Harp"},
    {9, 4, "Strings"},
    {9, 5, "Ocarina"},
    {9, 6, "Choir"},
    {9, 7, "Choir"},
    {9, 8, "Glockenspiel"},
    {9, 10, "Strings"},
    {9, 11, "Strings"},
    {9, 12, "Pizzicato Strings"},
    {18, 0, "Harp"},
    {18, 1, "Harp"},
    {18, 4, "Strings"},
    {18, 5, "Ocarina"},
    {18, 8, "Glockenspiel"},
    {18, 10, "Strings"},
    {18, 11, "Strings"},
    {18, 12, "Pizzicato Strings"},
    {5, 0, "Koto"},
    {5, 1, "Ocarina"},
    {5, 2, "Bassoon"},
    {5, 3, "Oboe"},
    {5, 12, "Pizzicato Strings"},
    {6, 1, "Ocarina"},
    {6, 10, "Strings"},
    {6, 11, "Strings"},
    {6, 13, "Piano"},
    {6, 14, "Piano"},
    {8, 0, "Harmonica"},
    {8, 1, "Guitar"},
    {8, 2, "Guitar"},
    {8, 3, "Ocarina"},
    {8, 4, "Glockenspiel"},
    {8, 5, "Accordion"},
    {8, 6, "Accordion"},
    {13, 0, "Malon Voice"},
    {13, 1, "Malon Voice"},
    {13, 2, "Electric Guitar"},
    {13, 3, "Electric Guitar (high)"},
    {13, 4, "Electric Guitar (high)"},
    {13, 11, "Strings"},
    {13, 12, "Strings"},
    {13, 13, "Violin"},
    {13, 14, "Violin"},
    {13, 15, "Bell"},
    {17, 0, "Banjo"},
    {17, 1, "Banjo"},
    {17, 5, "Bass Guitar"},
    {17, 6, "Harmonica"},
    {17, 7, "Guitar"},
    {17, 13, "Violin"},
    {17, 14, "Violin"},
    {19, 0, "Choir"},
    {19, 1, "Choir"},
    {19, 2, "Harp"},
    {19, 3, "Glockenspiel"},
    {20, 0, "Flute"},
    {20, 1, "Clarinet"},
    {20, 2, "Clarinet"},
    {20, 3, "Accordion"},
    {20, 4, "Glockenspiel"},
    {21, 0, "Steel Drum"},
    {21, 1, "Voice Pad"},
    {21, 2, "Guitar"},
    {22, 0, "Guitar"},
    {22, 1, "Accordion"},
    {22, 2, "Bass Guitar"},
    {22, 3, "Trumpet"},
    {22, 4, "Trumpet (high)"},
    {22, 10, "Hand Drum"},
    {25, 0, "Flute"},
    {25, 1, "Wind Chime"},
    {25, 4, "Bell Pad"},
    {25, 5, "Wood Chime"},
    {25, 6, "Voice Pad"},
    {25, 7, "Koto"},
    {26, 13, "Piano"},
    {26, 14, "Piano"},
    {27, 0, "Trumpet"},
    {27, 1, "Trumpet (low)"},
    {27, 2, "Guitar"},
    {27, 3, "Guitar"},
    {27, 5, "Bass Guitar"},
    {27, 7, "Guitar"},
    {27, 10, "Clap"},
    {27, 11, "Clap"},
    {28, 0, "Koto"},
    {28, 1, "Hand Drum"},
    {28, 3, "Hand Drum 2"},
    {28, 4, "Wind Chime"},
    {29, 0, "Koto"},
    {29, 1, "Hand Drum"},
    {29, 5, "Flute"},
    {29, 7, "Flute"},
    {29, 10, "Strings"},
    {29, 11, "Strings"},
    {30, 0, "Organ"},
    {30, 1, "Organ"},
    {30, 2, "Organ"},
    {30, 4, "French Horn"},
    {31, 1, "Wind"},
    {31, 2, "Dark Ambience"},
    {31, 3, "Choir"},
    {31, 4, "Piano"},
    {31, 5, "Piano"},
    {31, 7, "Dark Ambience 2"},
    {24, 0, "Hand Drum"},
    {24, 1, "Wind"},
    {24, 2, "Dark Ambience"},
    {24, 3, "Choir"},
    {24, 4, "Choir (high)"},
    {24, 6, "Chant"},
    {24, 7, "Dark Ambience 2"},
    {24, 8, "Harpsichord"},
    {23, 0, "Bell Pad"},
    {23, 1, "Bell Pad"},
    {23, 2, "Wind"},
    {23, 3, "Bell Pad"},
    {10, 0, "Chant"},
    {10, 1, "Chant 2"},
    {10, 2, "Chant 2"},
    {10, 3, "Xylophone"},
    {10, 5, "Wind"},
    {10, 6, "Cymbal"},
    {11, 0, "Wind"},
    {11, 1, "Cave Ambience"},
    {11, 2, "Cave Ambience 2"},
    {11, 3, "Cave Ambience 3"},
    {12, 0, "Forest Pad"},
    {12, 1, "Voice Pad"},
    {12, 2, "Wood Chime"},
};

static const int font_order[] = {3,  15, 32, 33, 34, 35, 36, 9,  18, 5,  6,  8,  13, 17, 19,
                                 20, 21, 22, 25, 26, 27, 28, 29, 30, 31, 24, 23, 10, 11, 12};

/* Drum kits by their first sample, after the randomizer guide's kit list. Only the orchestra kit's
   sounds are documented; the others are numbered. */
typedef struct {
    int sample;
    const char *kit;
    const char *sound;
} DrumName;

static const DrumName drum_names[] = {
    {367, "Orchestra kit", "Kick"},
    {368, "Orchestra kit", "Snare"},
    {369, "Orchestra kit", "Snare 2"},
    {370, "Orchestra kit", "Crash Cymbal"},
    {371, "Orchestra kit", "Timpani"},
    {378, "Tambourine kit", "sound 1"},
    {379, "Tambourine kit", "sound 2"},
    {380, "Tambourine kit", "sound 3"},
    {396, "Lute kit", "sound 1"},
    {397, "Lute kit", "sound 2"},
    {408, "Gong and wind chime kit", "sound 1"},
    {409, "Gong and wind chime kit", "sound 2"},
    {416, "Conga and bongo kit", "sound 1"},
    {417, "Conga and bongo kit", "sound 2"},
    {418, "Conga and bongo kit", "sound 3"},
    {419, "Conga and bongo kit", "sound 4"},
};

typedef struct {
    double rate;
    int base;
    bool known;
} SampleInfo;

static SampleInfo samples[1024];

typedef struct {
    char name[64];
    int font, program, release, point_count;
    int points[32][2];
    char attributes[512];
    int region_count;
    int lo[3], hi[3], sample[3], base[3];
    double rate[3];
} Instrument;

typedef struct {
    char kit[64], sound[64];
    int font, sample, lo, hi, base, pan, release, point_count;
    int points[32][2];
    double rate;
} Drum;

static Instrument instruments[512];
static int instrument_count;
static Drum drums[128];
static int drum_count;

typedef struct {
    int font, program, instrument;
} FontEntry;

static FontEntry entries[1024];
static int entry_count;

typedef struct {
    int font, drum, hi;
} FontDrum;

static FontDrum font_drums[256];
static int font_drum_count;

static void fail(const char *message, const char *detail) {
    fprintf(stderr, "oot_catalog: %s %s\n", message, detail ? detail : "");
    exit(1);
}

static char *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");

    if (!f) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)length + 1);

    if (!data || fread(data, 1, (size_t)length, f) != (size_t)length) {
        fail("cannot read", path);
    }

    fclose(f);
    data[length] = 0;

    if (size) {
        *size = (size_t)length;
    }

    return data;
}

/* Copies the value of attribute `key` in `line`; returns false when absent. */
static bool attribute(const char *line, const char *key, char *out, size_t capacity) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), " %s=\"", key);
    const char *at = strstr(line, pattern);

    if (!at) {
        return false;
    }

    at += strlen(pattern);
    size_t n = 0;

    while (at[n] && at[n] != '"' && n + 1 < capacity) {
        out[n] = at[n];
        n++;
    }

    out[n] = 0;
    return true;
}

static int int_attribute(const char *line, const char *key, int fallback) {
    char text[64];
    return attribute(line, key, text, sizeof(text)) ? atoi(text) : fallback;
}

/* MIDI key of a soundfont note name such as C4, EF3 or BNEG1 (C4 = 60). */
static int note_number(const char *name) {
    static const int steps[] = {9, 11, 0, 2, 4, 5, 7}; /* A B C D E F G */

    if (!name[0] || name[0] < 'A' || name[0] > 'G') {
        if (isdigit((unsigned char)name[0])) {
            return (atoi(name) + 21) % 128; /* Zelda64 note number */
        }

        fail("bad note name", name);
    }

    int value = steps[name[0] - 'A'];
    const char *rest = name + 1;

    if (rest[0] == 'F' && rest[1]) {
        value--;
        rest++;
    }

    int octave = !strncmp(rest, "NEG", 3) ? -atoi(rest + 3) : atoi(rest);
    return value + (octave + 1) * 12;
}

static int sample_index(const char *name) {
    if (strncmp(name, "SAMPLE_0_", 9)) {
        fail("sample outside sample bank 0:", name);
    }

    int index = atoi(name + 9);

    if (index < 0 || index >= 1024 || !samples[index].known) {
        fail("unknown sample", name);
    }

    return index;
}

static uint32_t le32(const unsigned char *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void read_wav(const char *path, SampleInfo *info) {
    size_t size;
    unsigned char *data = (unsigned char *)read_all(path, &size);

    if (!data || size < 44 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) {
        fail("bad wav", path);
    }

    bool have_rate = false, have_base = false;

    for (size_t at = 12; at + 8 <= size;) {
        uint32_t length = le32(data + at + 4);

        if (!memcmp(data + at, "fmt ", 4) && at + 16 <= size) {
            info->rate = le32(data + at + 12);
            have_rate = true;
        } else if (!memcmp(data + at, "smpl", 4) && at + 24 <= size) {
            info->base = (int)le32(data + at + 20);
            have_base = true;
        }

        at += 8 + length + (length & 1);
    }

    if (!have_rate || !have_base) {
        fail("wav without fmt/smpl chunk:", path);
    }

    info->known = true;
    free(data);
}

static void read_sample_bank(const char *audio) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/samplebanks/SampleBank_0.xml", audio);
    char *xml = read_all(path, NULL);

    if (!xml) {
        fail("cannot open", path);
    }

    for (char *line = strtok(xml, "\n"); line; line = strtok(NULL, "\n")) {
        char name[128], file[512];

        if (!strstr(line, "<Sample ") || !attribute(line, "Name", name, sizeof(name)) ||
            !attribute(line, "Path", file, sizeof(file))) {
            continue;
        }

        const char *leaf = strrchr(file, '/');
        char wav[1024];
        snprintf(wav, sizeof(wav), "%s/samples/SampleBank_0/%s", audio, leaf ? leaf + 1 : file);
        char *ext = strstr(wav, ".aifc");

        if (ext) {
            strcpy(ext, ".wav");
        }

        int index = atoi(name + 9);

        if (strncmp(name, "SAMPLE_0_", 9) || index < 0 || index >= 1024) {
            fail("unexpected sample name", name);
        }

        read_wav(wav, &samples[index]);
    }

    free(xml);
}

static const char *find_name(int font, int program) {
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if (names[i].font == font && names[i].program == program) {
            return names[i].name;
        }
    }

    return NULL;
}

typedef struct {
    char name[32];
    int release, count;
    int points[32][2];
} Envelope;

/* One region of an instrument: the main sample or its low/high split. */
static void add_region(Instrument *in, const char *line, const char *suffix, int lo, int hi) {
    char key[32], value[128];
    snprintf(key, sizeof(key), "Sample%s", suffix);

    if (!attribute(line, key, value, sizeof(value))) {
        return;
    }

    int r = in->region_count++;
    int sample = sample_index(value);
    in->sample[r] = sample;
    in->lo[r] = lo;
    in->hi[r] = hi;
    in->rate[r] = samples[sample].rate;
    in->base[r] = samples[sample].base;
    snprintf(key, sizeof(key), "SampleRate%s", suffix);

    if (attribute(line, key, value, sizeof(value))) {
        in->rate[r] = atof(value);
    }

    snprintf(key, sizeof(key), "BaseNote%s", suffix);

    if (attribute(line, key, value, sizeof(value))) {
        in->base[r] = note_number(value);
    }
}

static bool same_instrument(const Instrument *a, const Instrument *b) {
    return !strcmp(a->attributes, b->attributes) && a->release == b->release &&
           a->point_count == b->point_count &&
           !memcmp(a->points, b->points, sizeof(a->points[0]) * (size_t)a->point_count);
}

static void read_font(const char *audio, int font) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/soundfonts/Soundfont_%d.xml", audio, font);
    char *xml = read_all(path, NULL);

    if (!xml) {
        fail("cannot open", path);
    }

    if (!strstr(xml, "SampleBank_0.xml")) {
        fail("font does not use sample bank 0:", path);
    }

    Envelope envelopes[64];
    int envelope_count = 0;
    Envelope *current = NULL;

    for (char *line = strtok(xml, "\n"); line; line = strtok(NULL, "\n")) {
        if (strstr(line, "<Envelope ")) {
            if (envelope_count == 64) {
                fail("too many envelopes in", path);
            }

            current = &envelopes[envelope_count++];
            memset(current, 0, sizeof(*current));
            attribute(line, "Name", current->name, sizeof(current->name));
            current->release = int_attribute(line, "Release", 0);
        } else if (strstr(line, "</Envelope>")) {
            current = NULL;
        } else if (current && strstr(line, "<Point ")) {
            if (current->count == 32) {
                fail("envelope too long in", path);
            }

            current->points[current->count][0] = int_attribute(line, "Delay", 0);
            current->points[current->count][1] = int_attribute(line, "Arg", 0);
            current->count++;
        } else if (current && strstr(line, "<")) {
            fail("unsupported envelope command in", path);
        } else if (strstr(line, "<Instrument ") || strstr(line, "<Drum ")) {
            char env_name[32];
            attribute(line, "Envelope", env_name, sizeof(env_name));
            const Envelope *env = NULL;

            for (int i = 0; i < envelope_count; i++) {
                if (!strcmp(envelopes[i].name, env_name)) {
                    env = &envelopes[i];
                }
            }

            if (!env) {
                fail("missing envelope in", path);
            }

            if (strstr(line, "<Drum ")) {
                char text[128];
                Drum d = {0};
                d.font = font;
                attribute(line, "Sample", text, sizeof(text));
                d.sample = sample_index(text);

                if (attribute(line, "Note", text, sizeof(text))) {
                    d.lo = d.hi = note_number(text);
                } else {
                    attribute(line, "NoteStart", text, sizeof(text));
                    d.lo = note_number(text);
                    attribute(line, "NoteEnd", text, sizeof(text));
                    d.hi = note_number(text);
                }

                d.base = attribute(line, "BaseNote", text, sizeof(text)) ? note_number(text)
                                                                         : samples[d.sample].base;
                d.rate = attribute(line, "SampleRate", text, sizeof(text)) ? atof(text)
                                                                           : samples[d.sample].rate;
                d.pan = int_attribute(line, "Pan", 64);
                d.release = int_attribute(line, "Release", env->release);
                d.point_count = env->count;
                memcpy(d.points, env->points, sizeof(d.points));
                int found = -1;

                for (int i = 0; i < drum_count; i++) {
                    /* A kit that stops this sound at a lower key is the same sound. */
                    if (drums[i].sample == d.sample && drums[i].lo == d.lo &&
                        drums[i].base == d.base && drums[i].pan == d.pan) {
                        found = i;
                        drums[i].hi = d.hi > drums[i].hi ? d.hi : drums[i].hi;
                    }
                }

                if (found < 0) {
                    snprintf(d.kit, sizeof(d.kit), "Kit of font %d", font);
                    snprintf(d.sound, sizeof(d.sound), "sound %d", d.sample);

                    for (size_t i = 0; i < sizeof(drum_names) / sizeof(*drum_names); i++) {
                        if (drum_names[i].sample == d.sample) {
                            snprintf(d.kit, sizeof(d.kit), "%s", drum_names[i].kit);
                            snprintf(d.sound, sizeof(d.sound), "%s", drum_names[i].sound);
                        }
                    }

                    drums[drum_count] = d;
                    found = drum_count++;
                }

                font_drums[font_drum_count++] = (FontDrum){font, found, d.hi};
                continue;
            }

            Instrument in = {0};
            in.font = font;
            in.program = int_attribute(line, "ProgramNumber", -1);
            in.release = int_attribute(line, "Release", env->release);
            in.point_count = env->count;
            memcpy(in.points, env->points, sizeof(in.points));

            /* Keep the sample attributes verbatim; the generated font repeats them. */
            const char *start = strstr(line, " Sample=");

            if (!start) {
                fail("instrument without sample in", path);
            }

            snprintf(in.attributes, sizeof(in.attributes), "%s", start + 1);
            char *end = strstr(in.attributes, "/>");

            if (end) {
                *end = 0;
            }

            for (size_t n = strlen(in.attributes); n && in.attributes[n - 1] == ' ';) {
                in.attributes[--n] = 0;
            }

            char text[64];
            int range_lo = attribute(line, "RangeLo", text, sizeof(text)) ? note_number(text) : -1;
            int range_hi = attribute(line, "RangeHi", text, sizeof(text)) ? note_number(text) : 128;

            if (range_lo >= 0) {
                add_region(&in, line, "Lo", 0, range_lo);
            }

            add_region(&in, line, "", range_lo + 1, range_hi - 1);

            if (range_hi < 128) {
                add_region(&in, line, "Hi", range_hi, 127);
            }

            const char *name = find_name(font, in.program);
            int found = -1;

            for (int i = 0; i < instrument_count; i++) {
                if (same_instrument(&instruments[i], &in)) {
                    found = i;
                }
            }

            if (found < 0) {
                if (name) {
                    snprintf(in.name, sizeof(in.name), "%s", name);
                } else {
                    /* Name an unlisted instrument after a listed one using its main sample. */
                    int main = in.sample[range_lo >= 0 ? 1 : 0];

                    for (int i = 0; i < instrument_count && !in.name[0]; i++) {
                        for (int r = 0; r < instruments[i].region_count; r++) {
                            if (instruments[i].sample[r] == main &&
                                strncmp(instruments[i].name, "Unlisted", 8)) {
                                snprintf(in.name, sizeof(in.name), "%.63s", instruments[i].name);
                                break;
                            }
                        }
                    }

                    if (!in.name[0]) {
                        snprintf(in.name, sizeof(in.name), "Unlisted sound %d-%d", font,
                                 in.program);
                    }
                }

                if (instrument_count == 512) {
                    fail("too many instruments", NULL);
                }

                instruments[instrument_count] = in;
                found = instrument_count++;
            }

            entries[entry_count++] = (FontEntry){font, in.program, found};
        }
    }

    free(xml);
}

static void print_points(FILE *out, const int (*points)[2], int count) {
    fprintf(out, "{");

    for (int i = 0; i < count; i++) {
        fprintf(out, "%s{%d, %d}", i ? ", " : "", points[i][0], points[i][1]);
    }

    fprintf(out, "}");
}

static void print_escaped(FILE *out, const char *text) {
    fputc('"', out);

    for (; *text; text++) {
        if (*text == '"' || *text == '\\') {
            fputc('\\', out);
        }

        fputc(*text, out);
    }

    fputc('"', out);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: oot_catalog <extracted/VERSION/assets/audio> <catalog_data.c>\n");
        return 2;
    }

    read_sample_bank(argv[1]);

    for (size_t i = 0; i < sizeof(font_order) / sizeof(*font_order); i++) {
        read_font(argv[1], font_order[i]);
    }

    FILE *out = fopen(argv[2], "wb");

    if (!out) {
        fail("cannot write", argv[2]);
    }

    fprintf(out, "/* Generated by tools/oot_catalog.c from a zeldaret/oot audio extraction. Do not "
                 "edit.\n   Instrument names follow community lists; the decomp leaves them "
                 "unnamed. */\n#include \"oot.h\"\n\n");
    fprintf(out, "const OotInstrument oot_instruments[] = {\n");

    for (int i = 0; i < instrument_count; i++) {
        const Instrument *in = &instruments[i];
        fprintf(out, "    {\"%s\", %d, %d, ", in->name, in->font, in->program);
        print_escaped(out, in->attributes);
        fprintf(out, ", %d, %d, ", in->release, in->point_count);
        print_points(out, in->points, in->point_count);
        fprintf(out, ", %d, {", in->region_count);

        for (int r = 0; r < in->region_count; r++) {
            fprintf(out, "%s{%d, %d, %d, %.1f, %d}", r ? ", " : "", in->lo[r], in->hi[r],
                    in->sample[r], in->rate[r], in->base[r]);
        }

        fprintf(out, "}},\n");
    }

    fprintf(out, "};\nconst unsigned oot_instrument_count = %d;\n\n", instrument_count);
    fprintf(out, "const OotDrum oot_drums[] = {\n");

    for (int i = 0; i < drum_count; i++) {
        const Drum *d = &drums[i];
        fprintf(out, "    {\"%s\", \"%s\", %d, %d, %d, %d, %d, %.1f, %d, %d, %d, ", d->kit,
                d->sound, d->font, d->sample, d->lo, d->hi, d->base, d->rate, d->pan, d->release,
                d->point_count);
        print_points(out, d->points, d->point_count);
        fprintf(out, "},\n");
    }

    fprintf(out, "};\nconst unsigned oot_drum_count = %d;\n\n", drum_count);
    fprintf(out, "const OotFontEntry oot_font_entries[] = {\n");

    for (int i = 0; i < entry_count; i++) {
        fprintf(out, "    {%d, %d, %d},\n", entries[i].font, entries[i].program,
                entries[i].instrument);
    }

    fprintf(out, "};\nconst unsigned oot_font_entry_count = %d;\n\n", entry_count);
    fprintf(out, "const OotFontEntry oot_font_drums[] = {\n");

    for (int i = 0; i < font_drum_count; i++) {
        /* For drums the middle field is the last key this font maps to the sound. */
        fprintf(out, "    {%d, %d, %d},\n", font_drums[i].font, font_drums[i].hi,
                font_drums[i].drum);
    }

    fprintf(out, "};\nconst unsigned oot_font_drum_count = %d;\n", font_drum_count);
    fclose(out);
    fprintf(stderr, "oot_catalog: %d instruments, %d drum sounds, %d font entries\n",
            instrument_count, drum_count, entry_count);
    return 0;
}
