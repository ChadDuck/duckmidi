/* sm64coopdx target of the M64 library: songs play sm64coopdx's extended bank 0x2A, so every
   mapping resolves to an instrument of that bank and no bank is generated. */
#include "m64export/m64export.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static M64Request request(const M64Event *events, uint64_t count, uint64_t duration) {
    M64Request q = {0};
    q.events = events;
    q.event_count = count;
    q.duration = duration;
    q.ppq = 48;
    q.target = M64_TARGET_SM64COOPDX;
    q.voice_limit = 16;
    q.sequence_name = "03_level_grass";
    q.bank_name = "unused";
    return q;
}

static bool has_code(const M64Api *api, const M64Result *r, uint32_t code) {
    for (uint64_t i = 0; i < api->diagnostic_count(r); i++) {
        M64DiagnosticView d;
        api->diagnostic_at(r, i, &d);

        if (d.code == code) {
            return true;
        }
    }

    return false;
}

static bool contains(const M64ArtifactView *a, const uint8_t *bytes, size_t size) {
    for (uint64_t i = 0; i + size <= a->size; i++) {
        if (!memcmp(a->data + i, bytes, size)) {
            return true;
        }
    }

    return false;
}

static char *manifest(const M64Api *api, const M64Result *r) {
    M64ArtifactView a;
    assert(api->artifact_at(r, M64_MANIFEST, &a) == M64_OK);
    char *text = calloc((size_t)a.size + 1, 1);
    memcpy(text, a.data, (size_t)a.size);
    return text;
}

int main(void) {
    const M64Api *api = m64_get_api_3();
    M64Result *r = NULL;

    /* Every GM program, one after another on channel 1: each resolves to a bank instrument a
       layer can select, since the channel changes instruments. */
    M64Event programs[128 * 3];

    for (unsigned p = 0; p < 128; p++) {
        programs[p * 3] = (M64Event){p * 48, p * 3 + 1, 0xC0, p, 0};
        programs[p * 3 + 1] = (M64Event){p * 48, p * 3 + 2, 0x90, 60, 100};
        programs[p * 3 + 2] = (M64Event){p * 48 + 24, p * 3 + 3, 0x80, 60, 0};
    }

    M64Request q = request(programs, 128 * 3, 128 * 48);
    assert(api->compile(&q, NULL, &r) == M64_OK && api->artifact_count(r) == 4);
    assert(!has_code(api, r, 70) && !has_code(api, r, 57));
    M64ArtifactView bank, entry, sequence;
    assert(api->artifact_at(r, M64_BANK_JSON, &bank) == M64_OK && !bank.size);
    assert(api->artifact_at(r, M64_SEQUENCE_JSON, &entry) == M64_OK && !entry.size);
    assert(api->artifact_at(r, M64_SEQUENCE, &sequence) == M64_OK && sequence.size);
    char *text = manifest(api, r);
    assert(strstr(text, "\"target\": \"sm64coopdx\"") && strstr(text, "\"bank_id\": 42") &&
           strstr(text, "\"bank_instrument\": ") &&
           !strstr(text, "minimum_temporary_sequence_pool_bytes"));
    free(text);
    /* Grand piano is only in instruments 193 and 201, which a layer cannot select. */
    assert(has_code(api, r, 71));
    api->result_free(r);

    /* One instrument on the channel: grand piano is the channel's instrument (chan_setinstr). */
    M64Event piano[] = {{0, 1, 0x90, 60, 100}, {48, 2, 0x80, 60, 0}};
    q = request(piano, 2, 48);
    assert(api->compile(&q, NULL, &r) == M64_OK && !has_code(api, r, 71));
    assert(api->artifact_at(r, M64_SEQUENCE, &sequence) == M64_OK);
    const uint8_t setinstr[] = {0xC1, 193};
    assert(contains(&sequence, setinstr, 2));
    text = manifest(api, r);
    assert(strstr(text, "\"gm_program\": 0") && strstr(text, "\"bank_instrument\": 193"));
    free(text);
    api->result_free(r);

    /* Stock instruments are tuned like stock sequences: MIDI C4 is note 39, so the layer plays
       note 0x40 + 39 when the next note follows directly. */
    M64Event scale[] = {
        {0, 1, 0x90, 60, 100}, {48, 2, 0x80, 60, 0}, {48, 3, 0x90, 62, 100}, {96, 4, 0x80, 62, 0}};
    q = request(scale, 4, 96);
    assert(api->compile(&q, NULL, &r) == M64_OK);
    assert(api->artifact_at(r, M64_SEQUENCE, &sequence) == M64_OK);
    const uint8_t c4[] = {0x40 + 39, 48}, d4[] = {0x40 + 41, 48};
    assert(contains(&sequence, c4, 2) && contains(&sequence, d4, 2));
    api->result_free(r);

    /* An unpitched sample plays only in its own note range: GM 113 is cabasa, which instrument
       99 (decaying, like the mapping) plays above note 60, so C4 moves up to note 61 instead of
       playing a triangle. */
    M64Event cabasa[] = {{0, 1, 0xC0, 113, 0},
                         {0, 2, 0x90, 60, 100},
                         {48, 3, 0x80, 60, 0},
                         {48, 4, 0x90, 60, 100},
                         {96, 5, 0x80, 60, 0}};
    q = request(cabasa, 5, 96);
    assert(api->compile(&q, NULL, &r) == M64_OK);
    assert(api->artifact_at(r, M64_SEQUENCE, &sequence) == M64_OK);
    const uint8_t cabasa_note[] = {0x40 + 61, 48}, cabasa_instrument[] = {0xC1, 99};
    assert(contains(&sequence, cabasa_note, 2) && contains(&sequence, cabasa_instrument, 2));
    api->result_free(r);

    /* Every drum note maps to the bank's percussion or to a melodic instrument with the sample. */
    M64Event drums[128 * 2];

    for (unsigned k = 0; k < 128; k++) {
        drums[k * 2] = (M64Event){k * 48, k * 2 + 1, 0x99, k, 100};
        drums[k * 2 + 1] = (M64Event){k * 48 + 24, k * 2 + 2, 0x89, k, 0};
    }

    q = request(drums, 128 * 2, 128 * 48);
    assert(api->compile(&q, NULL, &r) == M64_OK && !has_code(api, r, 72));
    text = manifest(api, r);
    /* Kick 36 is percussion entry 15; the closed hi-hat plays instrument 5's low sample. */
    assert(strstr(text, "\"gm_note\": 36, \"slot\": ") &&
           strstr(text, "\"bank_instrument\": 127, \"note\": 15}"));
    assert(strstr(text, "\"bank_instrument\": 5, \"note\": 23}"));
    free(text);
    api->result_free(r);

    /* The other targets keep generating a bank; HackerSM64 only changes the manifest. */
    q = request(piano, 2, 48);
    q.target = M64_TARGET_HACKERSM64;
    assert(api->compile(&q, NULL, &r) == M64_OK);
    assert(api->artifact_at(r, M64_BANK_JSON, &bank) == M64_OK && bank.size);
    text = manifest(api, r);
    assert(strstr(text, "\"target\": \"HackerSM64\"") &&
           strstr(text, "minimum_temporary_sequence_pool_bytes"));
    free(text);
    api->result_free(r);
    q.target = 4;
    assert(api->compile(&q, NULL, &r) == M64_INVALID);
    api->result_free(r);
    puts("PASS M64 sm64coopdx target");
    return 0;
}
