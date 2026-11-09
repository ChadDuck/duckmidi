#include "midi_input_fixture.h"

typedef struct {
    MidiFixture *fixture;
    unsigned device;
} Producer;

static int produce(void *context) {
    Producer *p = context;

    for (unsigned n = 0; n < 3000; ++n) {
        fixture_emit(p->fixture, p->device, MIDI_CONTROL, MIDI_CC_MODULATION, n % 128,
                     SDL_GetTicksNS());
    }

    return 0;
}

static int deliver_until_closed(void *context) {
    MidiFixture *f = context;
    unsigned count = 0;

    for (;;) {
        SDL_LockMutex(f->port_locks[0]);

        if (!f->receive[0]) {
            SDL_UnlockMutex(f->port_locks[0]);
            return 0;
        }

        uint8_t message[] = {MIDI_CONTROL, MIDI_CC_MODULATION, count % 128};
        f->receive[0](f->receiver[0], message, sizeof(message), SDL_GetTicksNS());
        SDL_UnlockMutex(f->port_locks[0]);

        if (++count % 100 == 0) {
            SDL_Delay(1);
        }
    }
}

int main(void) {
    MidiFixture f;
    DmMidiInput *input = fixture_create(&f);
    SDL_Delay(120);
    assert(!atomic_load(&f.lists));
    atomic_store(&f.delay, 2000);
    uint64_t start = SDL_GetTicks();
    assert(dm_midi_input_open(input, "Fixture 0"));
    dm_midi_input_refresh(input);
    DmMidiInputStatus status[DM_MIDI_INPUTS];
    dm_midi_input_status(input, status);
    assert(SDL_GetTicks() - start < 100);
    fixture_wait(input, 0, DM_MIDI_OPEN);
    atomic_store(&f.delay, 0);
    fixture_emit(&f, 0, MIDI_NOTE_ON | 3, 60, 100, SDL_GetTicksNS());
    fixture_emit(&f, 0, MIDI_ACTIVE_SENSING, 0, 0, SDL_GetTicksNS());
    SDL_Delay(450);
    unsigned releases = 0;
    assert(fixture_drain(input, 0, &releases) == 35 && releases == 34);
    releases = 0;
    assert(fixture_drain(input, 1, &releases) == 3 && releases == 2);
    fixture_emit(&f, 0, MIDI_NOTE_ON, 61, 100, SDL_GetTicksNS());
    atomic_store(&f.available, 254);
    dm_midi_input_refresh(input);
    fixture_wait(input, 0, DM_MIDI_MISSING);
    releases = 0;
    assert(fixture_drain(input, 0, &releases) == 35 && releases == 34);
    fixture_drain(input, 1, NULL);
    atomic_store(&f.available, 255);
    dm_midi_input_refresh(input);
    fixture_wait(input, 0, DM_MIDI_OPEN);
    assert(atomic_load(&f.opens) == 2);

    for (unsigned i = 1; i < 4; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "Fixture %u", i);
        assert(dm_midi_input_open(input, name));
        fixture_wait(input, i, DM_MIDI_OPEN);
    }

    Producer producers[4];
    SDL_Thread *threads[4];

    for (unsigned i = 0; i < 4; ++i) {
        producers[i] = (Producer){&f, i};
        threads[i] = SDL_CreateThread(produce, "input producer", &producers[i]);
        assert(threads[i]);
    }

    for (unsigned i = 0; i < 4; ++i) {
        SDL_WaitThread(threads[i], NULL);
    }

    assert(fixture_drain(input, 0, NULL) == 12000);
    assert(fixture_drain(input, 1, NULL) == 12000);
    fixture_emit(&f, 0, MIDI_NOTE_ON, 62, 70, SDL_GetTicksNS());
    fixture_emit(&f, 0, MIDI_CONTROL, MIDI_CC_NOTES_OFF, 0, SDL_GetTicksNS());
    releases = 0;
    assert(fixture_drain(input, 0, &releases) == 2 && releases == 1);
    dm_midi_input_close(input, "Fixture 0");
    fixture_wait(input, 0, DM_MIDI_CLOSED);
    atomic_store(&f.fail, true);
    assert(dm_midi_input_open(input, "Fixture 0"));
    fixture_wait(input, 0, DM_MIDI_FAILED);
    dm_midi_input_status(input, status);
    assert(strstr(status[0].message, "busy"));

    for (unsigned i = 0; i < 4; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "Fixture %u", i);
        dm_midi_input_close(input, name);
        fixture_wait(input, i, DM_MIDI_CLOSED);
    }

    fixture_drain(input, 0, NULL);
    fixture_drain(input, 1, NULL);
    assert(dm_midi_input_open(input, "Missing device"));
    fixture_wait(input, 0, DM_MIDI_MISSING);
    dm_midi_input_status(input, status);
    assert(!strcmp(status[0].name, "Missing device"));
    dm_midi_input_destroy(input);
    fixture_free(&f);

    for (int n = 0; n < 10; ++n) {
        input = fixture_create(&f);
        atomic_store(&f.delay, 10);
        assert(dm_midi_input_open(input, "Fixture 0"));
        SDL_Delay(2);
        dm_midi_input_destroy(input);
        fixture_free(&f);
    }

    for (int n = 0; n < 10; ++n) {
        input = fixture_create(&f);
        assert(dm_midi_input_open(input, "Fixture 0"));
        fixture_wait(input, 0, DM_MIDI_OPEN);
        SDL_Thread *producer = SDL_CreateThread(deliver_until_closed, "live input teardown", &f);
        assert(producer);
        SDL_Delay(2);
        dm_midi_input_destroy(input);
        SDL_WaitThread(producer, NULL);
        fixture_free(&f);
    }

    uint64_t ns = SDL_GetTicksNS();
    input = fixture_create(&f);
    atomic_store(&f.available, 0);
    atomic_store(&f.list_delay, 2000);
    assert(dm_midi_input_open(input, "Fixture 0"));
    dm_midi_input_status(input, status);
    double startup_ms = (SDL_GetTicksNS() - ns) / 1e6;
    assert(startup_ms < 100);
    fixture_wait(input, 0, DM_MIDI_MISSING);
    dm_midi_input_destroy(input);
    fixture_free(&f);
    printf("missing_saved_device_request_ms=%.3f (2 s enumeration)\n", startup_ms);

    puts("MIDI worker: asynchronous open, sensing, loss, reconnect, failure and producers passed");
    return 0;
}
