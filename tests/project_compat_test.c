/* Loads a generated schema-9 project and checks its conversion to the current schema. The writer
   below follows the schema-9 layout field by field; it exists only to produce this fixture. */
#include "duck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL project compat: %s\n", text);
        exit(1);
    }
}

static void u32(DmBytes *b, uint32_t n) {
    uint8_t v[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    check(dm_bytes(b, v, 4), "fixture memory");
}

static void real(DmBytes *b, double n) {
    uint64_t bits;
    memcpy(&bits, &n, 8);
    u32(b, (uint32_t)bits);
    u32(b, (uint32_t)(bits >> 32));
}

static void text(DmBytes *b, const char *s) {
    u32(b, (uint32_t)strlen(s));
    check(dm_bytes(b, s, strlen(s)), "fixture memory");
}

static uint32_t crc32(const uint8_t *p, size_t size) {
    uint32_t crc = ~0u;

    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];

        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }

    return ~crc;
}

/* One pattern with one note on port 2, one placement on channel 1, a port-2 mixer entry and a
   program change event that schema 9 stored with source port 3. */
static DmBytes schema_9_project(void) {
    DmBytes body = {0}, file = {0};
    text(&body, "Old song");
    text(&body, "");
    real(&body, 120);
    u32(&body, 4);
    u32(&body, 4);
    u32(&body, 3); /* Source port. */
    u32(&body, 0); /* Sequence. */
    u32(&body, 1); /* Source format. */
    u32(&body, 1); /* Sequence count. */
    u32(&body, 1); /* Port count. */
    u32(&body, 3); /* Ports. */
    u32(&body, 0); /* Percussion override. */

    for (int i = 0; i < 128; ++i) {
        u32(&body, 0);
    }

    for (int c = 0; c < 16; ++c) {
        u32(&body, c == 0);
        u32(&body, 0);
        u32(&body, 0);
        u32(&body, c == 0 ? 5 : UINT32_MAX);
        u32(&body, 100);
        u32(&body, 64);
    }

    for (int c = 0; c < 16; ++c) {
        u32(&body, 0);
    }

    u32(&body, 0); /* Original MIDI block. */
    u32(&body, 1); /* Next pattern uid. */
    u32(&body, 1); /* Pattern count. */
    u32(&body, 1);
    text(&body, "Pattern 1");
    text(&body, "Pattern 1");
    u32(&body, 1);
    real(&body, 4);
    u32(&body, 1); /* Note count. */
    u32(&body, 0); /* Kind, mode and event. */
    u32(&body, 0);
    u32(&body, (uint32_t)-2);
    real(&body, 1);
    real(&body, 0);
    real(&body, 0);
    real(&body, .5);
    real(&body, .5);
    real(&body, 64);
    real(&body, 60);
    u32(&body, 60);
    u32(&body, 0);
    u32(&body, 0); /* Control scope. */
    u32(&body, 0); /* Pattern port; schema 10 adds the color after it. */
    real(&body, 0);
    real(&body, 1);
    u32(&body, 60);
    u32(&body, 100);
    u32(&body, 0);
    u32(&body, 2); /* Note port. */
    u32(&body, 1); /* Placement; schema 10 adds the offset after the lane. */
    real(&body, 0);
    u32(&body, 0);
    u32(&body, 0);
    u32(&body, 0);
    real(&body, 0);
    u32(&body, 0);
    u32(&body, 0); /* Automation count. */
    u32(&body, 1); /* Event: program change with source port 3. */
    real(&body, 0);
    u32(&body, 1);
    u32(&body, 3);
    u32(&body, 2);
    uint8_t program[] = {0xc0, 5};
    check(dm_bytes(&body, program, 2), "fixture memory");
    u32(&body, 0); /* Views. */
    u32(&body, 1); /* Port mixer; schema 10 adds a program after solo. */
    u32(&body, 2);
    u32(&body, 0);
    u32(&body, 90);
    u32(&body, 30);
    u32(&body, 0);
    u32(&body, 0);

    check(dm_bytes(&file, "DUCKMIDI", 8), "fixture memory");
    u32(&file, 9);
    u32(&file, (uint32_t)body.size);
    u32(&file, crc32(body.data, body.size));
    check(dm_bytes(&file, body.data, body.size), "fixture memory");
    dm_bytes_free(&body);
    return file;
}

int main(void) {
    DmProject p, again;
    dm_project_init(&p);
    dm_project_init(&again);
    char error[256] = {0};
    DmBytes old = schema_9_project(), current = {0};

    check(dm_unpack(&p, old.data, old.size, error, sizeof(error)), error);
    check(!strcmp(p.name, "Old song") && p.pattern_count == 1 && p.clip_count == 1 &&
              p.patterns[0].notes[0].port == 2 && p.channels[0].program == 5,
          "schema-9 content loads");
    check(p.patterns[0].color == 0 && p.clips[0].offset == 0 &&
              dm_port_mix(&p, 2, 0).volume == 90 && dm_port_mix(&p, 2, 0).program == 0,
          "fields added in schema 10 take their defaults");
    check(p.event_count == 1 && p.events[0].port == 0,
          "schema-9 events move to the default note port they played on");

    check(dm_pack(&p, &current) && current.data[8] == DM_PROJECT_SCHEMA,
          "converted projects save as the current schema");
    check(dm_unpack(&again, current.data, current.size, error, sizeof(error)) &&
              again.patterns[0].notes[0].port == 2 && again.events[0].port == 0,
          "saved conversion reloads");

    old.data[8] = 8;
    check(!dm_unpack(&again, old.data, old.size, error, sizeof(error)) && strstr(error, "schema 8"),
          "older schemas are rejected with a clear message");

    printf("PASS project compat: %d checks\n", checks);
    dm_bytes_free(&old);
    dm_bytes_free(&current);
    dm_project_free(&p);
    dm_project_free(&again);
    return 0;
}
