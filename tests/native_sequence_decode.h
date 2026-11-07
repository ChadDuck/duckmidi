#ifndef TEST_NATIVE_SEQUENCE_DECODE_H
#define TEST_NATIVE_SEQUENCE_DECODE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t start, end;
    unsigned channel, layer, key, pan, weight;
} NativeDecodedNote;

/* Channel effect commands, by M64 opcode: 0xd4 reverb, 0xd7 vibrato rate, 0xd8 depth. */
typedef struct {
    uint64_t tick;
    unsigned channel, op, value;
} NativeDecodedControl;

typedef struct {
    NativeDecodedNote notes[4096];
    size_t count;
    unsigned calls;
    NativeDecodedControl controls[1024];
    size_t control_count;
} NativeDecodedSequence;

/* Interprets the emitted converter subset, including layer subroutine state and note gates. */
void native_decode_m64(const uint8_t *, size_t, NativeDecodedSequence *);
void native_decode_oot(const char *, NativeDecodedSequence *);

#endif
