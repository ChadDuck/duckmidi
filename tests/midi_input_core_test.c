#include "midi_input_core.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DmMidiClock clock_state;
static atomic_bool finished;

static int writer(void *unused) {
    (void)unused;

    for (unsigned i = 1; i <= 100000; ++i) {
        dm_midi_clock_write(&clock_state, (DmMidiClockValue){i, i * 2.0, i * 3.0, i, true, i});
    }

    atomic_store(&finished, true);
    return 0;
}

int main(void) {
    DmMidiInputEvent event = {0};

    for (int status = MIDI_NOTE_OFF; status < MIDI_SYSTEM; ++status) {
        uint8_t bytes[] = {(uint8_t)status, 60, 70};
        size_t n = (status & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3;
        DmMidiVerdict expected =
            (status & MIDI_STATUS_MASK) == MIDI_PROGRAM ? DM_MIDI_FILTERED : DM_MIDI_RECORD;
        assert(dm_midi_normalize(bytes, n, &event) == expected);
        assert(dm_midi_normalize(bytes, n - 1, &event) == DM_MIDI_INVALID);
        bytes[1] = 128;
        assert(dm_midi_normalize(bytes, n, &event) == DM_MIDI_INVALID);
    }

    for (int cc = 0; cc <= 127; ++cc) {
        uint8_t bytes[] = {MIDI_CONTROL, (uint8_t)cc, 12};
        DmMidiVerdict expected = DM_MIDI_RECORD;

        if (cc == MIDI_CC_BANK || cc == MIDI_CC_BANK_LSB || cc >= MIDI_CC_SOUND_OFF) {
            expected = DM_MIDI_FILTERED;
        }

        if (cc == MIDI_CC_RESET) {
            expected = DM_MIDI_MONITOR;
        }

        if (cc == MIDI_CC_SOUND_OFF || cc == MIDI_CC_NOTES_OFF) {
            expected = DM_MIDI_RELEASE;
        }

        assert(dm_midi_normalize(bytes, 3, &event) == expected);
    }

    uint8_t zero[] = {MIDI_NOTE_ON | 3, 60, 0};
    assert(dm_midi_normalize(zero, 3, &event) == DM_MIDI_RECORD);
    assert(event.status == (MIDI_NOTE_OFF | 3) && event.b == 64);

    for (int status = MIDI_SYSTEM; status <= 255; ++status) {
        uint8_t byte = (uint8_t)status;
        assert(dm_midi_normalize(&byte, 1, &event) ==
               (status == MIDI_ACTIVE_SENSING ? DM_MIDI_SENSING : DM_MIDI_FILTERED));
    }

    uint8_t stray[] = {1, 2};
    assert(dm_midi_normalize(stray, 2, &event) == DM_MIDI_INVALID);
    assert(dm_midi_normalize(NULL, 0, &event) == DM_MIDI_INVALID);
    DmMidiRing ring;
    DmMidiInputEvent storage[4];
    dm_midi_ring_init(&ring, storage, 4);

    for (int pass = 0; pass < 20; ++pass) {
        for (int i = 0; i < 4; ++i) {
            assert(dm_midi_ring_put(&ring, (DmMidiInputEvent){.a = (uint8_t)i}));
        }

        assert(!dm_midi_ring_put(&ring, event));

        for (int i = 0; i < 4; ++i) {
            assert(dm_midi_ring_get(&ring, &event) && event.a == i);
        }

        assert(!dm_midi_ring_get(&ring, &event));
    }

    assert(atomic_load(&ring.dropped) == 20);
    DmMidiHeld held = {0};
    dm_midi_held_event(&held, (DmMidiInputEvent){.status = MIDI_NOTE_ON | 5, .a = 127, .b = 1});
    dm_midi_held_event(&held, (DmMidiInputEvent){.status = MIDI_NOTE_ON | 5, .a = 60, .b = 1});
    dm_midi_held_event(&held, (DmMidiInputEvent){.status = MIDI_NOTE_OFF | 5, .a = 60});
    assert(dm_midi_held_release(&held, 5, 1) == 0);
    assert(dm_midi_held_release(&held, 5, 3) == (1u << 31));
    assert(dm_midi_held_release(&held, 5, 3) == 0);
    DmMidiEcho echo = {0};
    dm_midi_echo_write(&echo, MIDI_NOTE_ON | 2, 60, 100, 1000000000);
    assert(dm_midi_echo_match(&echo, MIDI_NOTE_ON | 2, 60, 100, 1009999999));
    assert(!dm_midi_echo_match(&echo, MIDI_NOTE_ON | 2, 60, 100, 1010000000));
    assert(!dm_midi_echo_match(&echo, MIDI_NOTE_ON, 60, 100, 1000000000));
    SDL_Thread *thread = SDL_CreateThread(writer, "clock test", NULL);
    assert(thread);

    while (!atomic_load(&finished)) {
        DmMidiClockValue value;

        if (dm_midi_clock_read(&clock_state, &value)) {
            assert(value.position == value.ticks_ns * 2.0);
            assert(value.latency == value.ticks_ns * 3.0);
            assert(value.transport == value.ticks_ns);
            assert(value.loop == value.ticks_ns);
        }
    }

    SDL_WaitThread(thread, NULL);
    dm_midi_clock_write(&clock_state, (DmMidiClockValue){1000000000, 10, .01, 42, true});
    event = (DmMidiInputEvent){.ticks_ns = 1020000000};
    DmMidiClockValue stamp;
    assert(dm_midi_clock_read(&clock_state, &stamp));
    dm_midi_clock_stamp(stamp, &event);
    assert(event.song_seconds > 10.009999 && event.song_seconds < 10.010001);
    assert(event.transport == 42 && (event.flags & DM_MIDI_EVENT_PLAYING));

    /* Saved names survive reordering: RtMidi's position suffixes go, repeats are numbered. */
    char names[5][DM_MIDI_NAME] = {"Launchkey 49 0", "loopMIDI Port 1", "Launchkey 49 2",
                                   "Port 7 3", "Launchkey 49 #2 4"};
    dm_midi_port_names(names, 5, DM_MIDI_NAMES_INDEXED);
    assert(!strcmp(names[0], "Launchkey 49") && !strcmp(names[1], "loopMIDI Port"));
    assert(!strcmp(names[2], "Launchkey 49 #2") && !strcmp(names[3], "Port 7"));
    assert(!strcmp(names[4], "Launchkey 49 #2 #2"));
    char alsa[3][DM_MIDI_NAME] = {"MPK mini:MPK mini MIDI 1 24:0", "Through:Port-0 14:0", "42"};
    dm_midi_port_names(alsa, 3, DM_MIDI_NAMES_ALSA);
    assert(!strcmp(alsa[0], "MPK mini:MPK mini MIDI 1") && !strcmp(alsa[1], "Through:Port-0"));
    assert(!strcmp(alsa[2], "42"));
    char plain[2][DM_MIDI_NAME] = {"IAC Bus 1", "IAC Bus 1"};
    dm_midi_port_names(plain, 2, DM_MIDI_NAMES_PLAIN);
    assert(!strcmp(plain[0], "IAC Bus 1") && !strcmp(plain[1], "IAC Bus 1 #2"));
    puts("MIDI core: normalization, ring, held notes, echo and concurrent clock passed");
    return 0;
}
