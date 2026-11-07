/* OoT converter and repository patches: catalog defaults, MML output, layers, loops, ranges,
   soundfont XML, and the sequence table, spec and audio memory edits on generated excerpts of
   a zeldaret/oot checkout. */
#include "oot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PPQ 96

static DmeEvent events[256];
static unsigned event_count;

static void add(uint64_t tick, uint32_t status, uint32_t a, uint32_t b) {
    assert(event_count < 256);
    events[event_count] = (DmeEvent){tick, event_count + 1, status, a, b, 0};
    event_count++;
}

static void note(uint64_t tick, uint64_t length, unsigned channel, unsigned key) {
    add(tick, MIDI_NOTE_ON | channel, key, 100);
    add(tick + length, MIDI_NOTE_OFF | channel, key, 0);
}

static int compare_events(const void *left, const void *right) {
    const DmeEvent *a = left, *b = right;

    if (a->tick != b->tick) {
        return a->tick < b->tick ? -1 : 1;
    }

    return a->source_id < b->source_id ? -1 : 1;
}

static void default_request(OotRequest *q, int font) {
    memset(q, 0, sizeof(*q));
    qsort(events, event_count, sizeof(*events), compare_events);
    q->events = events;
    q->event_count = event_count;
    q->ppq = PPQ;
    q->duration = 0;

    for (unsigned i = 0; i < event_count; i++) {
        q->duration = events[i].tick > q->duration ? events[i].tick : q->duration;
    }

    q->font = font;
    q->font_index = 38;
    q->volume = 100;
    q->loop = OOT_LOOP_SONG;
    q->name = "test_song";

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned p = 0; p < 128; p++) {
            q->instruments[ch][p] = oot_default_instrument(font, p);
        }
    }

    for (unsigned k = 0; k < 128; k++) {
        q->drums[k] = oot_default_drum(font, k);
    }
}

static int find_instrument(const char *name) {
    for (unsigned i = 0; i < oot_instrument_count; i++) {
        if (!strcmp(oot_instruments[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static unsigned count(const char *text, const char *needle) {
    unsigned n = 0;

    for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle)) {
        n++;
    }

    return n;
}

static void check_catalog(void) {
    assert(oot_instrument_count > 40 && oot_drum_count >= 10);

    /* Every GM program has a default in the custom soundfont and in each vanilla soundfont. */
    for (unsigned p = 0; p < 128; p++) {
        int value = oot_default_instrument(OOT_CUSTOM_FONT, p);
        assert(oot_instrument_valid(value));

        for (int font = 0; font < 48; font++) {
            if (oot_music_font(font)) {
                int v = oot_default_instrument(font, p);
                assert(oot_synth_wave(v) >= 0 || oot_font_program(font, v) >= 0);
            }
        }
    }

    char label[96];
    oot_instrument_label(oot_default_instrument(OOT_CUSTOM_FONT, 0), label, sizeof(label));
    assert(!strcmp(label, "Piano (Main Orchestra)"));
    oot_instrument_label(oot_default_instrument(OOT_CUSTOM_FONT, 80), label, sizeof(label));
    assert(!strcmp(label, "Square wave (synthesized)"));
    assert(oot_font_program(3, find_instrument("Flute")) == 0);
    assert(!strcmp(oot_font_title(3), "Main Orchestra") && !oot_music_font(0) &&
           !oot_music_font(4));

    /* General MIDI drums use the orchestra kit; vanilla kits keep their own layout. */
    int kick = oot_default_drum(OOT_CUSTOM_FONT, 36);
    oot_drum_label(kick, label, sizeof(label));
    assert(!strcmp(label, "Kick (Orchestra kit)"));
    oot_drum_label(oot_default_drum(OOT_CUSTOM_FONT, 49), label, sizeof(label));
    assert(!strcmp(label, "Crash Cymbal (Orchestra kit)"));
    assert(oot_default_drum(OOT_CUSTOM_FONT, 20) == OOT_SILENT);
    assert(oot_default_drum(3, 36) == kick && oot_default_drum(3, 90) == OOT_SILENT);

    /* Toms move by octaves into the timpani's range; the kick plays at its own keys. */
    int timpani = oot_default_drum(OOT_CUSTOM_FONT, 41);
    assert(oot_drum_key(OOT_CUSTOM_FONT, timpani, 41) == 65);
    assert(oot_drum_key(OOT_CUSTOM_FONT, kick, 36) == 36);

    /* Note names follow the soundfont XML spelling. */
    oot_note_name(60, label, sizeof(label));
    assert(!strcmp(label, "C4"));
    oot_note_name(63, label, sizeof(label));
    assert(!strcmp(label, "EF4"));
    oot_note_name(11, label, sizeof(label));
    assert(!strcmp(label, "BNEG1"));

    /* Rates: a sample plays at its own rate at its root key and doubles an octave up. */
    int flute = find_instrument("Flute");
    const OotRegion *r = &oot_instruments[flute].regions[0];
    double at_root = oot_instrument_rate(flute, (unsigned)r->base);
    assert(at_root > r->rate / 32000.0 - 1e-9 && at_root < r->rate / 32000.0 + 1e-9);
    double octave = oot_instrument_rate(flute, (unsigned)r->base + 12);
    assert(octave > 2 * at_root - 1e-9 && octave < 2 * at_root + 1e-9);
}

static void check_conversion(void) {
    char error[256] = {0};
    OotRequest q;
    OotResult result;

    /* Piano chords, a program change, a tempo change, a pitch bend and drums. */
    event_count = 0;
    add(0, 0xff, 500000, 0);
    add(0, MIDI_PROGRAM | 0, 0, 0);
    add(0, MIDI_CONTROL | 0, MIDI_CC_VOLUME, 90);
    note(0, 96, 0, 60);
    note(0, 96, 0, 64);
    note(96, 48, 0, 67);
    add(192, MIDI_PROGRAM | 0, 73, 0);
    add(192, 0xff, 400000, 0);
    note(192, 96, 0, 72);
    add(240, MIDI_PITCH_BEND | 0, 0, 0x50);
    note(0, 24, MIDI_DRUM_CHANNEL, 36);
    note(48, 24, MIDI_DRUM_CHANNEL, 38);
    note(96, 24, MIDI_DRUM_CHANNEL, 42);
    add(384, MIDI_CONTROL | 0, MIDI_CC_VOLUME, 90);
    default_request(&q, OOT_CUSTOM_FONT);
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    const char *seq = result.sequence.data;
    assert(strstr(seq, "#include \"aseq.h\"\n#include \"Soundfont_test_song.h\""));
    assert(strstr(seq, ".startseq Sequence_test_song") &&
           strstr(seq, ".endseq Sequence_test_song"));
    assert(strstr(seq, "initchan 0x0201") && strstr(seq, "tempo 120") && strstr(seq, "tempo 150"));
    assert(strstr(seq, "SEQ_LOOP:") && strstr(seq, "jump SEQ_LOOP") && !strstr(seq, "freechan"));
    assert(strstr(seq, "ldchan 0, CHAN_0_0") && strstr(seq, "ldchan 9, CHAN_9_0"));
    assert(strstr(seq, "instr FONTANY_INSTR_DRUM") && strstr(seq, "panweight 0"));
    assert(strstr(seq, "instr 0") && strstr(seq, "instr 1") && strstr(seq, "vol 90"));
    assert(strstr(seq, "bend ") || strstr(seq, "bendfine "));
    assert(strstr(seq, "ldlayer 0, LAYER_0_0_0") && strstr(seq, "ldlayer 1, LAYER_0_0_1"));
    /* Middle C is pitch 39 in OoT; the 96-tick note lasts 48 sequence ticks. */
    assert(strstr(seq, "notedv 39, 48, 100") || strstr(seq, "notedvg 39, 48, 100"));
    assert(result.sequence_bytes > 0 && result.sequence_bytes % 16 == 0 &&
           result.sequence_bytes < 1024);
    assert(result.channels == 2 && result.instruments == 2 && result.drums == 3);

    /* The custom soundfont copies the vanilla instruments and places drums at their keys. */
    const char *font = result.soundfont.data;
    assert(strstr(font, "<Soundfont Name=\"Soundfont_test_song\" Index=\"38\""));
    assert(strstr(font, "SampleBank_0.xml\" Indirect=\"1\""));
    assert(strstr(font, "<Instrument ProgramNumber=\"0\" Name=\"INST_0\""));
    assert(strstr(font, "<Instrument ProgramNumber=\"1\" Name=\"INST_1\""));
    assert(strstr(font, "<Drum Name=\"DRUM_36\" Note=\"C2\""));
    assert(strstr(font, "<Drum Name=\"DRUM_42\" Note=\"GF2\""));
    assert(count(font, "<Envelope ") >= 1 && strstr(font, "</Soundfont>"));
    assert(result.soundfont_bytes > 0 && result.soundfont_bytes < OOT_FONT_CACHE);
    assert(!result.range_count);
    oot_result_free(&result);

    /* A vanilla soundfont needs no XML and uses its own program numbers. */
    default_request(&q, 3);
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    assert(strstr(result.sequence.data, "#include \"Soundfont_3.h\"") && !result.soundfont.size);
    assert(strstr(result.sequence.data, "instr 13")); /* Piano is program 13 of font 3. */
    oot_result_free(&result);

    /* Six overlapping notes: four layers on channel 1, two more on a free channel. */
    event_count = 0;

    for (unsigned k = 0; k < 6; k++) {
        note(0, 96, 0, 60 + k);
    }

    default_request(&q, OOT_CUSTOM_FONT);
    q.loop = OOT_LOOP_NONE;
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    seq = result.sequence.data;
    assert(strstr(seq, "ldlayer 3, LAYER_0_0_3") && strstr(seq, "ldlayer 1, LAYER_1_0_1"));
    assert(strstr(seq, "initchan 0x0003") && strstr(seq, "freechan 0x0003") &&
           !strstr(seq, "SEQ_LOOP"));
    oot_result_free(&result);

    /* Loop region: the intro plays once, then the loop section repeats. Notes crossing the loop
       start are split there. */
    event_count = 0;
    note(0, 288, 0, 60);
    note(288, 96, 0, 62);
    note(384, 96, 0, 64);
    default_request(&q, OOT_CUSTOM_FONT);
    q.loop = OOT_LOOP_REGION;
    q.loop_start = 192;
    q.loop_end = 480;
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    seq = result.sequence.data;
    assert(strstr(seq, "ldchan 0, CHAN_0_0") && strstr(seq, "ldchan 0, CHAN_0_1"));
    assert(strstr(seq, "SEQ_LOOP:") && strstr(seq, ".layer LAYER_0_1_0"));
    assert(strstr(seq, "notedv 39, 96, 100") && strstr(seq, "notedv 39, 48, 100"));
    oot_result_free(&result);
    q.loop_start = 480;
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_INVALID);
    oot_result_free(&result);

    /* High notes past ~4x the sample's rate are reported, with a supported substitute. */
    event_count = 0;
    note(0, 96, 0, 124);
    note(96, 96, 0, 126);
    default_request(&q, OOT_CUSTOM_FONT);
    int flute = find_instrument("Flute");
    q.instruments[0][0] = flute;
    assert(oot_convert(&q, NULL, true, &result, error, sizeof(error)) == DME_OK);
    assert(result.range_count == 1 && result.ranges[0].low == 124 && result.ranges[0].high == 126 &&
           result.ranges[0].count == 2 && result.ranges[0].rate > OOT_MAX_RATE);
    assert(!oot_range_supported(&q, &result.ranges[0], flute));
    assert(oot_range_supported(&q, &result.ranges[0], OOT_SYNTH_BASE));
    oot_result_free(&result);

    /* Notes above C6 use layer transposition; the pitch argument stays within 0..63. */
    event_count = 0;
    note(0, 96, 0, 96);
    default_request(&q, OOT_CUSTOM_FONT);
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    assert(strstr(result.sequence.data, "transpose 12") &&
           strstr(result.sequence.data, "notedv 63, 48, 100"));
    oot_result_free(&result);

    /* Repeated bars become one subroutine called from the layer. With short notes the first bar
       sets velocity and gate, so it enters differently and is written inline. */
    event_count = 0;

    for (unsigned bar = 0; bar < 8; bar++) {
        for (unsigned beat = 0; beat < 4; beat++) {
            note(bar * 4 * PPQ + beat * PPQ, PPQ / 2, 0, 60 + beat);
        }
    }

    default_request(&q, OOT_CUSTOM_FONT);
    q.duration = 8 * 4 * PPQ;
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    assert(strstr(result.sequence.data, ".layer LSUB_0") &&
           count(result.sequence.data, "call LSUB_0") >= 6 &&
           strstr(result.sequence.data, "shortdvg"));
    oot_result_free(&result);

    /* Invalid names and empty songs are rejected. */
    q.name = "Bad Name";
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_INVALID);
    oot_result_free(&result);
    event_count = 0;
    default_request(&q, OOT_CUSTOM_FONT);
    q.duration = PPQ;
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_INVALID &&
           strstr(error, "no notes"));
    oot_result_free(&result);
}

static const char *table =
    "/**\n * Sequence Table\n */\n"
    "DEFINE_SEQUENCE    (Sequence_0,         NA_BGM_GENERAL_SFX,      MEDIUM_CART, "
    "CACHE_LOAD_PERMANENT,  SEQ_0_FLAGS) // general_sfx\n"
    "DEFINE_SEQUENCE    (Sequence_29,        NA_BGM_MARKET,           MEDIUM_CART, "
    "CACHE_LOAD_TEMPORARY,  0          ) // market\n"
    "DEFINE_SEQUENCE    (Sequence_34,        NA_BGM_ITEM_GET,         MEDIUM_CART, "
    "CACHE_LOAD_PERSISTENT, SEQ_FLAG_FANFARE) // item_get\n"
    "DEFINE_SEQUENCE_PTR(NA_BGM_GREAT_FAIRY, NA_BGM_FILE_SELECT,      MEDIUM_CART, "
    "CACHE_LOAD_TEMPORARY,  SEQ_FLAG_SKIP_HARP_INTRO) // file_select\n"
    "DEFINE_SEQUENCE    (Sequence_109,       NA_BGM_CUTSCENE_EFFECTS, MEDIUM_CART, "
    "CACHE_LOAD_TEMPORARY,  0          ) // cutscene_effects\n";

static const char *spec =
    "beginseg\n    name \"Audiobank\"\n    address 0\n"
    "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_0.o\"\n"
    "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_1.o\"\n"
    "    include \"$(BUILD_DIR)/assets/audio/soundfonts/Soundfont_2.o\"\n"
    "#if OOT_VERSION >= PAL_1_0\n"
    "    include \"$(BUILD_DIR)/assets/audio/audiobank_padding.o\"\n#endif\nendseg\n\n"
    "beginseg\n    name \"Audioseq\"\n    address 0\n"
    "    include \"$(BUILD_DIR)/assets/audio/sequences/seq_0.prg.o\"\n"
    "    include \"$(BUILD_DIR)/assets/audio/sequences/seq_109.prg.o\"\nendseg\n\n"
    "beginseg\n    name \"Audiotable\"\nendseg\n";

static const char *session =
    "AudioSpec gAudioSpecs[3] = {\n"
    "    { 32000, 1, 24, 4, 0, 0, 2, sReverbSettings[0], 0x300, 0x200, 0x7FFF, 0x7F0, 0xE00, 0, "
    "0x4000, 0x2880, 0, 0, 0 },\n"
    "    { 32000, 1, 28, 3, 0, 0, 2, sReverbSettings[10], 0x300, 0x200, 0x7FFF, 0x7F0, 0xE00, 0, "
    "0x2800, 0x2880, 0, 0, 0 },\n"
    "    { 32000, 1, 24, 4, 0, 0, 2, sReverbSettings[2], 0x300, 0x200, 0x7FFF, 0x7F0, 0xE00, 0, "
    "0x3600, 0x2600, 0, 0, 0 },\n"
    "};\n";

static void check_repository(void) {
    char error[256] = {0}, notice[256];
    OotText out, again;

    /* Replace: only the first argument changes. */
    assert(oot_patch_sequence_table((const uint8_t *)table, strlen(table), "my_song",
                                    "NA_BGM_MARKET", 0x1000, &out, notice, sizeof(notice), error,
                                    sizeof(error)));
    assert(strstr(out.data, "DEFINE_SEQUENCE    (Sequence_my_song,        NA_BGM_MARKET,") &&
           !strstr(out.data, "Sequence_29,") && !*notice && out.size == strlen(table) + 5);
    oot_text_free(&out);

    /* A large song replacing a persistent fanfare moves to the temporary cache. */
    assert(oot_patch_sequence_table((const uint8_t *)table, strlen(table), "my_song",
                                    "NA_BGM_ITEM_GET", 0x1000, &out, notice, sizeof(notice), error,
                                    sizeof(error)));
    assert(strstr(out.data, "(Sequence_my_song,        NA_BGM_ITEM_GET,         MEDIUM_CART, "
                            "CACHE_LOAD_TEMPORARY, SEQ_FLAG_FANFARE)") &&
           strstr(notice, "temporary"));
    oot_text_free(&out);

    /* A new song goes after the last entry; exporting again keeps one line. */
    assert(oot_patch_sequence_table((const uint8_t *)table, strlen(table), "my_song", NULL, 0x100,
                                    &out, notice, sizeof(notice), error, sizeof(error)));
    assert(strstr(out.data, "// cutscene_effects\nDEFINE_SEQUENCE    (Sequence_my_song, "
                            "NA_BGM_MY_SONG, MEDIUM_CART, CACHE_LOAD_TEMPORARY, 0)"));
    assert(oot_patch_sequence_table((const uint8_t *)out.data, out.size, "my_song", NULL, 0x100,
                                    &again, notice, sizeof(notice), error, sizeof(error)));
    assert(count(again.data, "Sequence_my_song") == 1 && again.size == out.size);
    oot_text_free(&out);
    oot_text_free(&again);

    assert(!oot_patch_sequence_table((const uint8_t *)table, strlen(table), "my_song",
                                     "NA_BGM_FILE_SELECT", 0x100, &out, notice, sizeof(notice),
                                     error, sizeof(error)) &&
           strstr(error, "points at another song"));
    oot_text_free(&out);
    assert(!oot_patch_sequence_table((const uint8_t *)"nothing\n", 8, "my_song", NULL, 0x100, &out,
                                     notice, sizeof(notice), error, sizeof(error)));
    oot_text_free(&out);

    /* CRLF tables keep CRLF. */
    const char *crlf = "DEFINE_SEQUENCE    (Sequence_1, NA_BGM_A, MEDIUM_CART, "
                       "CACHE_LOAD_TEMPORARY, 0)\r\n";
    assert(oot_patch_sequence_table((const uint8_t *)crlf, strlen(crlf), "my_song", NULL, 0x100,
                                    &out, notice, sizeof(notice), error, sizeof(error)));
    assert(strstr(out.data, "added by duckmidi\r\n"));
    oot_text_free(&out);

    /* Spec: the soundfont follows the last soundfont, before the PAL padding; the sequence ends
       the Audioseq segment. A second pass changes nothing. */
    assert(oot_next_font_index((const uint8_t *)spec, strlen(spec), "my_song") == 3);
    assert(oot_patch_spec((const uint8_t *)spec, strlen(spec), "my_song", true, &out, error,
                          sizeof(error)));
    assert(strstr(out.data, "Soundfont_2.o\"\n    include \"$(BUILD_DIR)/assets/audio/soundfonts/"
                            "Soundfont_my_song.o\"\n#if OOT_VERSION"));
    assert(strstr(out.data, "seq_109.prg.o\"\n    include \"$(BUILD_DIR)/assets/audio/sequences/"
                            "my_song.o\"\nendseg"));
    assert(oot_next_font_index((const uint8_t *)out.data, out.size, "my_song") == 3);
    assert(oot_patch_spec((const uint8_t *)out.data, out.size, "my_song", true, &again, error,
                          sizeof(error)));
    assert(again.size == out.size && !memcmp(again.data, out.data, out.size));
    oot_text_free(&out);
    oot_text_free(&again);
    assert(oot_patch_spec((const uint8_t *)spec, strlen(spec), "my_song", false, &out, error,
                          sizeof(error)));
    assert(!strstr(out.data, "Soundfont_my_song") && strstr(out.data, "sequences/my_song.o"));
    oot_text_free(&out);

    /* Audio memory: rows below the song's size rise to it; the heap grows by the largest raise. */
    uint32_t growth = 0;
    assert(oot_patch_session_config((const uint8_t *)session, strlen(session), 0x3800, 0, &out,
                                    &growth, error, sizeof(error)));
    assert(growth == 0x1000 && count(out.data, "0x3800, 0x2880") == 1 &&
           count(out.data, "0x4000, 0x2880") == 1 && strstr(out.data, "0x3800, 0x2600"));
    oot_text_free(&out);
    assert(oot_patch_session_config((const uint8_t *)session, strlen(session), 0x3000, 0x2880, &out,
                                    &growth, error, sizeof(error)));
    assert(growth == 0x800 && strstr(out.data, "0x3600, 0x2880"));
    oot_text_free(&out);
    assert(oot_patch_session_config((const uint8_t *)session, strlen(session), 0x1000, 0, &out,
                                    &growth, error, sizeof(error)) &&
           growth == 0 && out.size == strlen(session));
    oot_text_free(&out);

    const char *heap = "ALIGNED(16) u8 gAudioHeap[0x38000];\n";
    const char *buffers = "extern u8 gAudioHeap[0x38000]; // 0x38000 bytes\n";
    assert(oot_patch_audio_heap((const uint8_t *)heap, strlen(heap), 0x1000, &out, error,
                                sizeof(error)));
    assert(!strcmp(out.data, "ALIGNED(16) u8 gAudioHeap[0x39000];\n"));
    oot_text_free(&out);
    assert(oot_patch_audio_heap((const uint8_t *)buffers, strlen(buffers), 0x1000, &out, error,
                                sizeof(error)));
    assert(!strcmp(out.data, "extern u8 gAudioHeap[0x39000]; // 0x39000 bytes\n"));
    oot_text_free(&out);

    char *guide = oot_install_guide("my_song", NULL, true, 38, 0x1000, 0x800);
    assert(guide && strstr(guide, "NA_BGM_MY_SONG") && strstr(guide, "Soundfont_my_song.xml") &&
           strstr(guide, "Index is 38"));
    free(guide);
}

static void write_file(const char *dir, const char *name, const OotText *text, uint64_t bytes) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(text->data, 1, text->size, f) == text->size);
    fclose(f);
    printf("%s: %llu bytes\n", path, (unsigned long long)bytes);
}

/* Writes songs that use every feature, for checking with the decomp's assembler and soundfont
   compiler (tools/validate-oot-export.sh). */
static void write_demos(const char *dir) {
    char error[256] = {0};
    OotRequest q;
    OotResult result;
    event_count = 0;
    add(0, 0xff, 500000, 0);
    add(0, MIDI_CONTROL | 0, MIDI_CC_PAN, 30);
    add(0, MIDI_CONTROL | 0, MIDI_CC_REVERB, 40);
    add(0, MIDI_CONTROL | 1, MIDI_CC_MODULATION, 64);
    add(0, MIDI_PROGRAM | 1, 40, 0);

    for (unsigned bar = 0; bar < 6; bar++) {
        uint64_t t = bar * 4 * PPQ;

        for (unsigned k = 0; k < 6; k++) {
            note(t, 3 * PPQ, 0, 48 + k * 4); /* Six-note chords use a second channel */
        }

        note(t, PPQ, 1, 96 + bar); /* High violin needs transposition */
        note(t + 2 * PPQ, PPQ / 3, 1, 84);

        for (unsigned beat = 0; beat < 4; beat++) {
            note(t + beat * PPQ, PPQ / 4, MIDI_DRUM_CHANNEL, beat % 2 ? 38 : 36);
            note(t + beat * PPQ + PPQ / 2, PPQ / 4, MIDI_DRUM_CHANNEL, 42);
        }
    }

    add(2 * PPQ, MIDI_PITCH_BEND | 1, 0, 0x60);
    add(3 * PPQ, MIDI_PITCH_BEND | 1, 0, 0x40);
    add(8 * PPQ, 0xff, 300000, 0);
    add(10 * PPQ, MIDI_CONTROL | 0, MIDI_CC_EXPRESSION, 80);
    add(24 * PPQ, MIDI_CONTROL | 0, MIDI_CC_VOLUME, 100);
    default_request(&q, OOT_CUSTOM_FONT);
    q.duration = 24 * PPQ;
    q.loop = OOT_LOOP_REGION;
    q.loop_start = 8 * PPQ;
    q.name = "demo_custom";
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    write_file(dir, "demo_custom.seq", &result.sequence, result.sequence_bytes);
    write_file(dir, "Soundfont_demo_custom.xml", &result.soundfont, result.soundfont_bytes);
    oot_result_free(&result);
    default_request(&q, 3);
    q.duration = 24 * PPQ;
    q.name = "demo_vanilla";
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    write_file(dir, "demo_vanilla.seq", &result.sequence, result.sequence_bytes);
    oot_result_free(&result);
}

/* Dense volume ramps that pass OoT's 64 KiB limit are simplified until the song fits; the notes
   stay as written and the warning is a list item without the "audible changes" flag. */
static void check_dense_controllers(void) {
    enum { CHANNELS = 16, STEPS = 1600 };

    size_t capacity = CHANNELS * (STEPS + 2), count = 0;
    DmeEvent *dense = calloc(capacity, sizeof(*dense));
    assert(dense);

    for (unsigned step = 0; step < STEPS; step++) {
        for (unsigned ch = 0; ch < CHANNELS; ch++) {
            /* Distinct per channel, so the streams cannot be shared. */
            unsigned phase = step + ch * 37;
            unsigned value = phase % 254 < 127 ? phase % 127 : 127 - phase % 127;
            dense[count] =
                (DmeEvent){step * 2, count + 1, MIDI_CONTROL | ch, MIDI_CC_VOLUME, value, 0};
            count++;
        }
    }

    for (unsigned ch = 0; ch < CHANNELS; ch++) {
        if (ch == MIDI_DRUM_CHANNEL) {
            continue;
        }

        dense[count] = (DmeEvent){0, count + 1, MIDI_NOTE_ON | ch, 48 + ch, 100, 0};
        count++;
        dense[count] = (DmeEvent){STEPS * 2, count + 1, MIDI_NOTE_OFF | ch, 48 + ch, 0, 0};
        count++;
    }

    qsort(dense, count, sizeof(*dense), compare_events);
    OotRequest q;
    event_count = 0;
    default_request(&q, OOT_CUSTOM_FONT);
    q.events = dense;
    q.event_count = count;
    q.duration = STEPS * 2;
    OotResult result;
    char error[256] = {0};
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    assert(result.sequence_bytes <= 0x10000);
    bool simplified = false;

    for (size_t i = 0; i < result.diagnostic_count; i++) {
        const OotDiagnostic *d = &result.diagnostics[i];
        simplified |= (d->flags & DME_LIST) && !(d->flags & DME_ALTERED) &&
                      strstr(d->text, "curves: simplified");
    }

    unsigned notes = 0;

    for (const char *line = result.sequence.data; (line = strstr(line, "\n")) != NULL; ++line) {
        const char *word = line + 1;

        while (*word == ' ') {
            ++word;
        }

        notes += !strncmp(word, "note", 4) || !strncmp(word, "shortd", 6) ||
                 !strncmp(word, "shortvg", 7);
    }

    assert(simplified && notes == CHANNELS - 1);
    oot_result_free(&result);
    free(dense);
    puts("PASS dense controllers fit by simplifying curves only");
}

/* Channels with the same volume automation store it once: the later stream jumps into the
   earlier one's identical tail. */
static void check_shared_streams(void) {
    event_count = 0;

    for (unsigned step = 0; step < 100; step++) {
        add(step * 4, MIDI_CONTROL | 0, MIDI_CC_VOLUME, 20 + step);
        add(step * 4, MIDI_CONTROL | 1, MIDI_CC_VOLUME, 20 + step);
    }

    note(0, 400, 0, 60);
    note(0, 400, 1, 64);
    OotRequest q;
    default_request(&q, OOT_CUSTOM_FONT);
    OotResult result;
    char error[256] = {0};
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    assert(strstr(result.sequence.data, "jump CTRL_") && count(result.sequence.data, "vol ") < 150);
    oot_result_free(&result);
    puts("PASS identical controller streams are shared");
}

/* Repeated bars with a tick of timing jitter and slightly different velocities share nothing
   and pass 64 KiB without any controllers. The last resort rounds the notes until bars repeat,
   and reports it as an audible change. */
static void check_note_reduction(void) {
    enum { CHANNELS = 15, BARS = 300, NOTES = 8 };

    size_t count = 0;
    DmeEvent *jittered = calloc(CHANNELS * BARS * NOTES * 2, sizeof(*jittered));
    assert(jittered);
    unsigned seed = 1;

    for (unsigned ch = 0, out = 0; out < CHANNELS; ch++) {
        if (ch == MIDI_DRUM_CHANNEL) {
            continue;
        }

        for (unsigned bar = 0; bar < BARS; bar++) {
            for (unsigned i = 0; i < NOTES; i++) {
                seed = seed * 1103515245u + 12345u;
                uint64_t start = (bar * NOTES + i) * (PPQ / 2) + (seed >> 16) % 3;
                unsigned velocity = 96 + (seed >> 20) % 4;
                jittered[count] =
                    (DmeEvent){start, count + 1, MIDI_NOTE_ON | ch, 60 + i, velocity, 0};
                count++;
                jittered[count] =
                    (DmeEvent){start + PPQ / 4, count + 1, MIDI_NOTE_OFF | ch, 60 + i, 0, 0};
                count++;
            }
        }

        out++;
    }

    qsort(jittered, count, sizeof(*jittered), compare_events);
    OotRequest q;
    event_count = 0;
    default_request(&q, OOT_CUSTOM_FONT);
    q.events = jittered;
    q.event_count = count;
    q.duration = (uint64_t)BARS * NOTES * (PPQ / 2) + PPQ;
    OotResult result;
    char error[256] = {0};
    assert(oot_convert(&q, NULL, false, &result, error, sizeof(error)) == DME_OK);
    assert(result.sequence_bytes <= 0x10000);
    bool altered = false, curves = false;

    for (size_t i = 0; i < result.diagnostic_count; i++) {
        altered |= (result.diagnostics[i].flags & DME_ALTERED) != 0;
        curves |= strstr(result.diagnostics[i].text, "curves") != NULL;
    }

    assert(altered && !curves);
    oot_result_free(&result);
    free(jittered);
    puts("PASS unshareable notes fit by last-resort note reduction");
}

int main(int argc, char **argv) {
    check_catalog();
    check_conversion();
    check_dense_controllers();
    check_shared_streams();
    check_note_reduction();
    check_repository();

    if (argc > 1) {
        write_demos(argv[1]);
    }

    puts("oot_export_test: ok");
    return 0;
}
