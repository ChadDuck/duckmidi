#ifndef M64_INTERNAL_H
#define M64_INTERNAL_H
#include "duckmidi_midi.h"
#include "m64export/m64export.h"
#include "custom.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

/* Vibrato rate byte every channel script sets at its start. */
#define M64_CHANNEL_VIBRATO_RATE 8

/* Stock presets: the 128 GM programs or drum notes, then one plain preset for each sample of
   the stock instruments bank (catalog.json), so that every stock sample can be mapped. */
#define M64_STOCK_SAMPLES 73
#define M64_STOCK_PRESETS (128 + M64_STOCK_SAMPLES)

/* Drum keys: source notes, then custom drum regions from 128, then stock presets from 256. */
#define M64_DRUM_SLOTS (256 + M64_STOCK_PRESETS)

/* Curated alternates per GM program (catalog.json), best first. */
#define M64_ALTERNATES 6

typedef struct {
    uint8_t *data;
    size_t size, capacity;
    bool failed, address_failed;
} Buffer;

typedef struct {
    M64DiagnosticView view;
    char text[256];
} Diagnostic;

struct M64Result {
    Buffer files[M64_ARTIFACT_COUNT];
    Diagnostic *diagnostics;
    size_t count, capacity;
    int32_t status;
    bool compiled;
    M64RangeIssue *ranges;
    uint64_t *range_origins;
    size_t origin_capacity;
    size_t range_count, range_capacity;
};

typedef struct {
    uint64_t start, end, source;
    uint32_t channel, output, key, velocity, program, layer, drum;
    uint64_t group;
    uint32_t original_program;
    uint32_t section; /* 0 plays once; 1 is the repeated loop region. */
    int next;
    bool released;
    bool custom;
    uint16_t region;
    double peak_factor;
} Note;

typedef struct {
    M64Instrument high;
    int normal_hi;
    bool enabled;
} Register;

typedef struct {
    const char *sample;
    double tuning;
    unsigned family;
} StockRegister;

/* sm64coopdx plays mod music with one existing bank, the extended bank 0x2A; see coopdx.c.
   sample is an index in SM64's "instruments" sample bank, or -1. */
typedef struct {
    int sample;
    double tuning;
} CoopdxSound;

typedef struct {
    int present, release_rate, attack, sustained, range_lo, range_hi;
    CoopdxSound lo, normal, hi;
} CoopdxInstrument;

#define COOPDX_BANK 0x2A
#define COOPDX_INSTRUMENTS 204
#define COOPDX_PERCUSSION 127 /* Layer instrument that plays the bank's percussion. */
extern const CoopdxInstrument coopdx_instruments[COOPDX_INSTRUMENTS];
extern const CoopdxSound coopdx_drums[64];

typedef struct {
    uint32_t volume, expression, pan, reverb, program, pedal;
    uint32_t rpn_msb, rpn_lsb, range, cents;
    int32_t bend;
    uint32_t modulation, encoded_bend;
    int active[128], tail[128];
} Channel;

typedef struct {
    const M64Request *request;
    const M64Custom *custom;
    const M64Hooks *hooks;
    M64Result *result;
    Note *notes;
    size_t count, capacity;
    Channel channels[16];
    int *slots, drums[M64_DRUM_SLOTS];
    M64Instrument custom_drums[128];
    unsigned custom_drum_regions[128], custom_drum_keys[128], custom_drum_count;
    uint32_t programs[127], percussion[128], program_count, drum_count;
    int sources[16];
    uint64_t duration;
    uint32_t quality_grid;
    bool *curve_skip; /* Per request event: a volume, pan or bend change left out to fit. */
    uint32_t phrase_notes;
    double tempo_scale;
    bool warned[256];
    bool preset_pan[16];  /* Source channels that use layer/drum pan rather than MIDI pan. */
    EsNativeFxChange *fx; /* Preset reverb/vibrato changes in tick order (48-tick units). */
    size_t fx_count;
    uint8_t fx_depth[16]; /* Largest preset vibrato depth byte per channel, for rate limits. */
    Register registers[127];
    bool region;                   /* Request loop 2: intro plus repeated region. */
    uint64_t loop_start, loop_end; /* Region bounds in 48-tick units. */
    uint32_t emit_section;         /* Section being written by sequence() and emit_layers(). */
    /* sm64coopdx: bank 0x2A instruments per program slot and drum note; see coopdx.c. */
    bool coopdx;
    int fixed[127], fixed_layer[127], drum_instrument[M64_DRUM_SLOTS], drum_pitch[M64_DRUM_SLOTS];
    int channel_instrument[16], layer_instrument[16];
    bool switches[16];
} Compile;

bool reserve(void **p, size_t *cap, size_t count, size_t width);
void put(Buffer *b, unsigned value);
void bytes(Buffer *b, const void *p, size_t size);
void format(Buffer *b, const char *fmt, ...);
void variable(Buffer *b, uint64_t value);
void delay(Buffer *b, unsigned command, uint64_t ticks);
void address(Buffer *b, size_t offset, size_t target);
void diagnose(Compile *c, uint32_t severity, uint32_t code, const M64Event *event, const char *fmt,
              ...);
bool cancelled(Compile *c, uint32_t stage, uint32_t percent);
uint64_t tick48(const Compile *c, uint64_t tick);
uint64_t sequence_load_bytes(const Compile *c);
const M64Instrument *program_definition(const Compile *c, uint32_t program);
const M64Instrument *drum_definition(const Compile *c, uint32_t key);
extern const M64Instrument gm_programs[128], gm_drums[128];
extern const M64Instrument stock_samples[M64_STOCK_SAMPLES], stock_drum_samples[M64_STOCK_SAMPLES];
const M64Instrument *stock_preset(bool drum, uint32_t index);
extern const int16_t gm_alternates[128][M64_ALTERNATES];
extern const StockRegister stock_registers[];
extern const size_t stock_register_count;
bool select_registers(Compile *c);
bool timeline(Compile *c);
bool sequence(Compile *c);
bool emit_layers(Compile *c, Buffer *b, size_t fixes[16][4]);
bool bank_json(Compile *c);
bool coopdx_resolve(Compile *c);
void coopdx_note(const Compile *c, const Note *n, int *instrument, unsigned *pitch);
#endif
