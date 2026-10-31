#ifndef M64_INTERNAL_H
#define M64_INTERNAL_H
#include "duckmidi_midi.h"
#include "m64export/m64export.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

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
    size_t range_count, range_capacity;
};

typedef struct {
    uint64_t start, end, source;
    uint32_t channel, output, key, velocity, program, layer;
    uint32_t section; /* 0 plays once; 1 is the repeated loop region. */
    int next;
    bool released;
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

typedef struct {
    uint32_t volume, expression, pan, reverb, program, pedal;
    uint32_t rpn_msb, rpn_lsb, range, cents;
    int32_t bend;
    uint32_t modulation, encoded_bend;
    int active[128], tail[128];
} Channel;

typedef struct {
    const M64Request *request;
    const M64Hooks *hooks;
    M64Result *result;
    Note *notes;
    size_t count, capacity;
    Channel channels[16];
    int slots[4096], drums[128];
    uint32_t programs[127], percussion[128], program_count, drum_count;
    int sources[16];
    uint64_t duration;
    uint32_t quality_grid;
    uint32_t phrase_notes;
    double tempo_scale;
    bool warned[256];
    Register registers[127];
    bool region;                   /* Request loop 2: intro plus repeated region. */
    uint64_t loop_start, loop_end; /* Region bounds in 48-tick units. */
    uint32_t emit_section;         /* Section being written by sequence() and emit_layers(). */
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
extern const StockRegister stock_registers[];
extern const size_t stock_register_count;
bool select_registers(Compile *c);
bool timeline(Compile *c);
bool sequence(Compile *c);
bool emit_layers(Compile *c, Buffer *b, size_t fixes[16][4]);
bool bank_json(Compile *c);
#endif
