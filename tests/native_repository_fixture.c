#include "native_repository_fixture.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void save(const char *root, const char *path, const void *data, size_t size) {
    char full[4096], parent[4096];
    assert(SDL_snprintf(full, sizeof(full), "%s/%s", root, path) < (int)sizeof(full));
    SDL_snprintf(parent, sizeof(parent), "%s", full);
    char *slash = strrchr(parent, '/');
    assert(slash);
    *slash = 0;
    assert(SDL_CreateDirectory(parent) && SDL_SaveFile(full, data, size));
}

static void text(const char *root, const char *path, const char *data) {
    save(root, path, data, strlen(data));
}

static void sm64(const char *root) {
    text(root, "Makefile", "VERSION ?= us\n");
    size_t size;
    char *catalog = SDL_LoadFile("libraries/m64export/src/catalog.c", &size);
    assert(catalog);
    const uint8_t header[] = {'F',  'O',  'R',  'M', 0, 0,  0, 174, 'A', 'I', 'F', 'F', 'C', 'O',
                              'M',  'M',  0,    0,   0, 18, 0, 1,   0,   0,   0,   64,  0,   16,
                              0x40, 0x0d, 0xfa, 0,   0, 0,  0, 0,   0,   0,   'S', 'S', 'N', 'D',
                              0,    0,    0,    136, 0, 0,  0, 0,   0,   0,   0,   0};
    uint8_t audio[182] = {0};
    memcpy(audio, header, sizeof(header));

    for (char *p = catalog; (p = strchr(p, '"')) != NULL;) {
        const char *begin = ++p;
        char *end = strchr(p, '"');
        assert(end);
        size_t length = (size_t)(end - begin);
        char sample[128];

        if (length >= 2 && length < sizeof(sample) && strchr("0123456789ABCDEF", begin[0]) &&
            strchr("0123456789ABCDEF", begin[1])) {
            memcpy(sample, begin, length);
            sample[length] = 0;
            bool valid = true;

            for (size_t i = 0; i < length; i++) {
                valid &= strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_#-",
                                sample[i]) != NULL;
            }

            if (valid) {
                char path[256];
                SDL_snprintf(path, sizeof(path), "sound/samples/instruments/%s.aiff", sample);
                save(root, path, audio, sizeof(audio));
            }
        }

        p = end + 1;
    }

    SDL_free(catalog);
    const char *bank = "{/* fixture comment */\n\"sample_bank\":\"instruments\","
                       "\"envelopes\":{\"env\":[[1,32700],[1,32700],\"hang\"]},"
                       "\"instruments\":{\"piano\":{\"release_rate\":128,\"envelope\":\"env\","
                       "\"sound\":\"44_grand_piano_F#5\"}},\"instrument_list\":[\"piano\"]}\n";
    const char *banks[] = {"11", "22", "0E"};

    for (unsigned i = 0; i < 3; i++) {
        char path[256];
        SDL_snprintf(path, sizeof(path), "sound/sound_banks/%s.json", banks[i]);
        text(root, path, bank);
    }

    const char *songs[] = {"02_menu_title_screen", "03_custom_worker", "04_level_inside_castle"};
    const uint8_t sequence[] = {0xff};

    for (unsigned i = 0; i < 3; i++) {
        char path[256];
        SDL_snprintf(path, sizeof(path), "sound/sequences/us/%s.m64", songs[i]);
        save(root, path, sequence, sizeof(sequence));
    }

    text(root, "tools/assemble_sound.py",
         "# isolated native source fixture\nVALID_INSTRUMENT_NAMES = []\n");
}

static void oot(const char *root) {
    text(root, "Makefile", "VERSION ?= ntsc-1.0\n");
    text(root, "tools/audio/soundfont_compiler.c", "/* isolated native tool boundary */\n");
    uint8_t audio[172] = {'R', 'I', 'F', 'F', 164, 0, 0,   0,   'W', 'A', 'V', 'E', 'f', 'm', 't',
                          ' ', 16,  0,   0,   0,   1, 0,   1,   0,   0,   125, 0,   0,   0,   250,
                          0,   0,   2,   0,   16,  0, 'd', 'a', 't', 'a', 128, 0,   0,   0};
    save(root, "assets/audio/samples/SampleBank_0/fixture.wav", audio, sizeof(audio));
    char *bank = malloc(180000);
    assert(bank);
    size_t at = (size_t)SDL_snprintf(
        bank, 180000,
        "<SampleBank Name=\"SampleBank_0\" Index=\"0\" "
        "Medium=\"MEDIUM_CART\" CachePolicy=\"CACHE_LOAD_EITHER_NOSYNC\"><Pointer Index=\"1\"/>\n");

    for (unsigned i = 0; i < 1024; i++) {
        int n =
            SDL_snprintf(bank + at, 180000 - at,
                         "<Sample Name=\"SAMPLE_0_%u\" "
                         "Path=\"$(BUILD_DIR)/assets/audio/samples/SampleBank_0/fixture.aifc\"/>\n",
                         i);
        assert(n > 0 && (size_t)n < 180000 - at);
        at += (size_t)n;
    }

    assert(at + 14 < 180000);
    memcpy(bank + at, "</SampleBank>\n", 14);
    at += 14;
    save(root, "extracted/ntsc-1.0/assets/audio/samplebanks/SampleBank_0.xml", bank, at);
    free(bank);
    const char *names[] = {"0", "old_a"};

    for (unsigned i = 0; i < 2; i++) {
        char path[256], font[2048];
        SDL_snprintf(path, sizeof(path), "assets/audio/soundfonts/Soundfont_%s.xml", names[i]);
        SDL_snprintf(
            font, sizeof(font),
            "<!-- preserve initialized custom files -->\n"
            "<Soundfont Name=\"Soundfont_%s\" Index=\"%u\" Medium=\"MEDIUM_CART\" "
            "CachePolicy=\"CACHE_LOAD_TEMPORARY\" "
            "SampleBank=\"$(BUILD_DIR)/assets/audio/samplebanks/SampleBank_0.xml\">"
            "<Envelopes><Envelope Name=\"Env\" Release=\"245\"><Point Delay=\"1\" Arg=\"32700\"/>"
            "</Envelope></Envelopes><Samples><Sample Name=\"SAMPLE_0_0\"/></Samples>"
            "<Instruments><Instrument Name=\"Piano\" ProgramNumber=\"0\" Envelope=\"Env\" "
            "Sample=\"SAMPLE_0_0\"/></Instruments></Soundfont>\n",
            names[i], i);
        text(root, path, font);
    }
}

void native_repository_fixture(const char *checkout, bool is_oot) {
    if (is_oot) {
        oot(checkout);
    } else {
        sm64(checkout);
    }
}
