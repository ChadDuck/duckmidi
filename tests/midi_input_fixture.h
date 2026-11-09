#ifndef DM_MIDI_INPUT_FIXTURE_H
#define DM_MIDI_INPUT_FIXTURE_H

#include "midi_input.h"
#include "midi_input_core.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_Mutex *lock;
    SDL_Mutex *port_locks[DM_MIDI_INPUTS];
    atomic_uint available, delay, list_delay, lists, opens, closes;
    atomic_bool fail;
    DmMidiReceive receive[DM_MIDI_INPUTS];
    void *receiver[DM_MIDI_INPUTS];
} MidiFixture;

static int fixture_list(void *context, char names[][DM_MIDI_NAME], int capacity, char *error,
                        size_t cap) {
    (void)error;
    (void)cap;
    MidiFixture *f = context;
    atomic_fetch_add(&f->lists, 1);
    SDL_Delay(atomic_load(&f->list_delay));
    unsigned available = atomic_load(&f->available);
    int count = 0;

    for (unsigned i = 0; i < DM_MIDI_INPUTS && count < capacity; ++i) {
        if (available & (1u << i)) {
            snprintf(names[count++], DM_MIDI_NAME, "Fixture %u", i);
        }
    }

    return count;
}

static void *fixture_open(void *context, const char *name, DmMidiReceive receive, void *receiver,
                          char *error, size_t cap) {
    MidiFixture *f = context;
    atomic_fetch_add(&f->opens, 1);
    SDL_Delay(atomic_load(&f->delay));

    if (atomic_load(&f->fail)) {
        snprintf(error, cap, "Fixture port busy");
        return NULL;
    }

    unsigned i = 99;
    assert(sscanf(name, "Fixture %u", &i) == 1 && i < DM_MIDI_INPUTS);
    SDL_LockMutex(f->port_locks[i]);
    f->receive[i] = receive;
    f->receiver[i] = receiver;
    SDL_UnlockMutex(f->port_locks[i]);
    return (void *)(uintptr_t)(i + 1);
}

static void fixture_close(void *context, void *handle) {
    MidiFixture *f = context;
    unsigned i = (unsigned)(uintptr_t)handle - 1;
    SDL_LockMutex(f->port_locks[i]);
    f->receive[i] = NULL;
    f->receiver[i] = NULL;
    SDL_UnlockMutex(f->port_locks[i]);
    atomic_fetch_add(&f->closes, 1);
}

static DmMidiInput *fixture_create(MidiFixture *f) {
    memset(f, 0, sizeof(*f));
    f->lock = SDL_CreateMutex();
    atomic_init(&f->available, 255);

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        f->port_locks[i] = SDL_CreateMutex();
        assert(f->port_locks[i]);
    }
    DmMidiInputBackend backend = {f, fixture_list, fixture_open, fixture_close, NULL};
    DmMidiInput *input = dm_midi_input_create(&backend);
    assert(input && f->lock);
    return input;
}

static void fixture_emit(MidiFixture *f, unsigned device, uint8_t status, uint8_t a, uint8_t b,
                         uint64_t ns) {
    uint8_t bytes[] = {status, a, b};
    size_t n = status >= MIDI_SYSTEM ? 1 : (status & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3;
    SDL_LockMutex(f->port_locks[device]);
    assert(f->receive[device]);
    f->receive[device](f->receiver[device], bytes, n, ns);
    SDL_UnlockMutex(f->port_locks[device]);
}

static void fixture_free(MidiFixture *f) {
    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        SDL_DestroyMutex(f->port_locks[i]);
    }

    SDL_DestroyMutex(f->lock);
}

static void fixture_wait(DmMidiInput *input, unsigned slot, DmMidiInputState state) {
    uint64_t deadline = SDL_GetTicks() + 10000;

    for (;;) {
        DmMidiInputStatus status[DM_MIDI_INPUTS];
        dm_midi_input_status(input, status);

        if (status[slot].state == state) {
            return;
        }

        assert(SDL_GetTicks() < deadline);
        SDL_Delay(1);
    }
}

static unsigned fixture_drain(DmMidiInput *input, unsigned consumer, unsigned *releases) {
    unsigned count = 0;

    if (dm_midi_input_begin(input, consumer)) {
        DmMidiInputEvent event;

        while (dm_midi_input_pop(input, consumer, &event)) {
            ++count;

            if (releases && (event.flags & DM_MIDI_EVENT_RELEASE)) {
                ++*releases;
            }
        }

        dm_midi_input_end(input, consumer);
    }

    return count;
}

#endif
