#include "midi_input/midi_input.h"
#include "midi_input/core.h"
#include "rtmidi_c.h"
#include "text/en.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct DmMidiInput *owner;
    unsigned index;
    void *handle;
    DmMidiInputStatus status;
    DmMidiHeld held;
    DmMidiClockValue clock_cache; /* Callback only, seeded before opening the port. */
    DmMidiRing rings[2], releases[2];
    DmMidiInputEvent events[2][DM_MIDI_RING], release_events[2][DM_MIDI_RELEASE_RING];
    atomic_ullong last_ns;
    atomic_bool sensing;
    atomic_uint pedals[2]; /* Sustain and sostenuto source channels admitted to monitoring. */
    atomic_ullong received, filtered, echoes, invalid, timing_fallbacks;
} InputSlot;

struct DmMidiInput {
    DmMidiInputBackend backend;
    InputSlot slots[DM_MIDI_INPUTS];
    DmMidiClock clock;
    DmMidiEcho echo;
    SDL_Thread *worker;
    SDL_Mutex *lock;
    SDL_Semaphore *wake;
    atomic_bool quit, refresh, thru;
    atomic_uint open, ready[2], options, learning;
    DmMidiBindingSource *sources; /* Protected by lock. */
    size_t source_count;
    atomic_uint bound[DM_MIDI_INPUTS][MIDI_CHANNELS][8]; /* Four words each for CC and notes. */
    atomic_bool overflow[2];
    char ports[DM_MIDI_PORTS][DM_MIDI_NAME];
    int port_count;
};

typedef struct {
    RtMidiInPtr device;
    DmMidiReceive receive;
    void *receiver;
} RtInput;

/* Worker-only state: one enumeration client for the input's lifetime (CoreMIDI and ALSA
   clients are costly to create every poll) and scratch names for opening. */
typedef struct {
    RtMidiInPtr list;
    char names[DM_MIDI_PORTS][DM_MIDI_NAME];
} RtBackend;

static bool rt_ok(RtMidiPtr device, char *error, size_t cap) {
    if (device && device->ok) {
        return true;
    }

    snprintf(error, cap, "%s", device && device->msg ? device->msg : DM_TEXT_MIDI_INPUT_FAILED);
    return false;
}

/* Stable port names: RtMidi's position suffixes removed, repeated names numbered. Port i of
   the device has names[i]. */
static int rt_names(RtMidiInPtr device, char names[][DM_MIDI_NAME], int capacity, char *error,
                    size_t cap) {
    unsigned count = rtmidi_get_port_count(device);

    if (!rt_ok(device, error, cap)) {
        return -1;
    }

    int result = (int)(count < (unsigned)capacity ? count : (unsigned)capacity);

    for (int i = 0; i < result; ++i) {
        int length = DM_MIDI_NAME;
        int read = rtmidi_get_port_name(device, (unsigned)i, names[i], &length);

        if (!rt_ok(device, error, cap) || read < 0 || length > DM_MIDI_NAME) {
            return -1;
        }
    }

    enum RtMidiApi api = rtmidi_in_get_current_api(device);
    dm_midi_port_names(names, result,
                       api == RTMIDI_API_WINDOWS_MM   ? DM_MIDI_NAMES_INDEXED
                       : api == RTMIDI_API_LINUX_ALSA ? DM_MIDI_NAMES_ALSA
                                                      : DM_MIDI_NAMES_PLAIN);
    return result;
}

static int rt_list(void *context, char names[][DM_MIDI_NAME], int capacity, char *error,
                   size_t cap) {
    RtBackend *backend = context;

    if (!backend->list) {
        backend->list = rtmidi_in_create(RTMIDI_API_UNSPECIFIED, "duckmidi input list", 1);

        if (!rt_ok(backend->list, error, cap)) {
            if (backend->list) {
                rtmidi_in_free(backend->list);
            }

            backend->list = NULL;
            return -1;
        }
    }

    return rt_names(backend->list, names, capacity, error, cap);
}

static void rt_destroy(void *context) {
    RtBackend *backend = context;

    if (backend->list) {
        rtmidi_in_free(backend->list);
    }

    free(backend);
}

static void rt_receive(double delta, const unsigned char *bytes, size_t size, void *context) {
    (void)delta;
    RtInput *port = context;
    port->receive(port->receiver, bytes, size, SDL_GetTicksNS());
}

static void rt_close(void *context, void *handle) {
    (void)context;
    RtInput *port = handle;
    /* RtMidi's destructor closes the native port and joins its delivery thread. The callback
       context stays alive until then (cancel_callback alone is not a synchronization barrier). */
    rtmidi_in_free(port->device);
    free(port);
}

static void *rt_open(void *context, const char *name, DmMidiReceive receive, void *receiver,
                     char *error, size_t cap) {
    RtBackend *backend = context;
    RtInput *port = calloc(1, sizeof(*port));

    if (!port) {
        snprintf(error, cap, "%s", DM_TEXT_MIDI_INPUT_MEMORY);
        return NULL;
    }

    port->receive = receive;
    port->receiver = receiver;
    port->device = rtmidi_in_create(RTMIDI_API_UNSPECIFIED, "duckmidi input", 1);

    if (!rt_ok(port->device, error, cap)) {
        goto fail;
    }

    /* Index the names through this handle, so the index matches the port it opens. */
    int count = rt_names(port->device, backend->names, DM_MIDI_PORTS, error, cap);

    if (count < 0) {
        goto fail;
    }

    for (int i = 0; i < count; ++i) {
        if (strcmp(backend->names[i], name)) {
            continue;
        }

        rtmidi_in_ignore_types(port->device, true, true, false);

        if (!rt_ok(port->device, error, cap)) {
            goto fail;
        }

        rtmidi_in_set_callback(port->device, rt_receive, port);

        if (!rt_ok(port->device, error, cap)) {
            goto fail;
        }

        rtmidi_open_port(port->device, (unsigned)i, "duckmidi input");

        if (rt_ok(port->device, error, cap)) {
            return port;
        }

        goto fail;
    }

    snprintf(error, cap, "%s", DM_TEXT_MIDI_INPUT_MISSING);
fail:
    if (port->device) {
        rt_close(NULL, port);
    } else {
        free(port);
    }

    return NULL;
}

/* Consumer 0 is audio monitoring and 1 the editor. Bound controls reach only the editor. */
static void enqueue(InputSlot *slot, DmMidiInputEvent event, bool record, bool worker) {
    unsigned first = event.flags & (DM_MIDI_EVENT_BOUND | DM_MIDI_EVENT_LEARN) ? 1 : 0;

    for (unsigned consumer = first; consumer < 2; ++consumer) {
        if (consumer == 1 && !record) {
            continue;
        }

        if (!dm_midi_ring_put(worker ? &slot->releases[consumer] : &slot->rings[consumer], event)) {
            atomic_store_explicit(&slot->owner->overflow[consumer], true, memory_order_release);
        }
        atomic_store_explicit(&slot->owner->ready[consumer], 1, memory_order_release);
    }
}

static void release_notes(InputSlot *slot, int channel, DmMidiInputEvent event, bool worker) {
    event.flags |= DM_MIDI_EVENT_RELEASE;
    event.b = 64;

    for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
        if (channel >= 0 && c != (unsigned)channel) {
            continue;
        }

        event.status = MIDI_NOTE_OFF | c;

        for (unsigned word = 0; word < 4; ++word) {
            uint32_t bits = dm_midi_held_release(&slot->held, c, word);

            for (unsigned bit = 0; bit < 32; ++bit) {
                if (bits & (1u << bit)) {
                    event.a = word * 32 + bit;
                    enqueue(slot, event, true, worker);
                }
            }
        }
    }
}

static void bind_slot(DmMidiInput *input, unsigned slot, const char *name) {
    unsigned words[MIDI_CHANNELS][8] = {0};

    for (size_t i = 0; i < input->source_count; ++i) {
        const DmMidiBindingSource *source = &input->sources[i];

        if (!*source->device || !strcmp(source->device, name)) {
            unsigned word = source->number / 32 + (source->kind == DM_MIDI_SOURCE_NOTE ? 4 : 0);
            words[source->channel][word] |= 1u << (source->number % 32);
        }
    }

    for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
        for (unsigned w = 0; w < 8; ++w) {
            atomic_store_explicit(&input->bound[slot][c][w], words[c][w], memory_order_relaxed);
        }
    }
}

static bool source_learn(const DmMidiInput *input, DmMidiInputEvent event) {
    unsigned mode = atomic_load_explicit(&input->learning, memory_order_relaxed);
    unsigned type = event.status & MIDI_STATUS_MASK;
    return mode != DM_MIDI_LEARN_OFF &&
           ((type == MIDI_CONTROL && dm_midi_source_bindable(DM_MIDI_SOURCE_CC, event.a)) ||
            (mode == DM_MIDI_LEARN_BUTTON && type == MIDI_NOTE_ON));
}

static bool source_bound(const DmMidiInput *input, DmMidiInputEvent event) {
    unsigned type = event.status & MIDI_STATUS_MASK;

    if (type != MIDI_CONTROL && type != MIDI_NOTE_ON && type != MIDI_NOTE_OFF) {
        return false;
    }

    unsigned word = event.a / 32 + (type == MIDI_CONTROL ? 0 : 4);
    unsigned channel = event.status & MIDI_CHANNEL_MASK;
    unsigned bits =
        atomic_load_explicit(&input->bound[event.device][channel][word], memory_order_relaxed);
    return (bits & (1u << (event.a % 32))) != 0;
}

static int pedal_index(DmMidiInputEvent event) {
    if ((event.status & MIDI_STATUS_MASK) == MIDI_CONTROL) {
        if (event.a == MIDI_CC_SUSTAIN) {
            return 0;
        }

        if (event.a == MIDI_CC_SOSTENUTO) {
            return 1;
        }
    }

    return -1;
}

/* Whether a message ends a key or pedal that monitoring and recording already received. */
static bool ends_admitted(InputSlot *slot, DmMidiInputEvent event, int pedal) {
    if (pedal >= 0) {
        unsigned bits = atomic_load_explicit(&slot->pedals[pedal], memory_order_relaxed);
        return event.b < 64 && (bits & (1u << (event.status & MIDI_CHANNEL_MASK)));
    }

    return (event.status & MIDI_STATUS_MASK) == MIDI_NOTE_OFF &&
           dm_midi_held_note(&slot->held, event);
}

/* Delivers an unbound message and tracks the keys and pedals it leaves held. */
static void admit(InputSlot *slot, DmMidiInputEvent event, DmMidiVerdict verdict, int pedal) {
    unsigned bit = 1u << (event.status & MIDI_CHANNEL_MASK);

    if (pedal >= 0) {
        if (event.b >= 64) {
            atomic_fetch_or_explicit(&slot->pedals[pedal], bit, memory_order_relaxed);
        } else {
            atomic_fetch_and_explicit(&slot->pedals[pedal], ~bit, memory_order_relaxed);
        }
    } else if (verdict == DM_MIDI_MONITOR) {
        atomic_fetch_and_explicit(&slot->pedals[0], ~bit, memory_order_relaxed);
        atomic_fetch_and_explicit(&slot->pedals[1], ~bit, memory_order_relaxed);
    }

    dm_midi_held_event(&slot->held, event);
    enqueue(slot, event, verdict == DM_MIDI_RECORD, false);
}

enum { OPTION_FIXED_SHIFT = 2, OPTION_INVERT_SHIFT = 9, OPTION_CHANNEL_SHIFT = 10 };

static unsigned pack_options(const DmMidiInputOptions *options) {
    return (unsigned)options->velocity | (unsigned)options->fixed_velocity << OPTION_FIXED_SHIFT |
           (unsigned)options->sustain_invert << OPTION_INVERT_SHIFT |
           (unsigned)options->channel << OPTION_CHANNEL_SHIFT;
}

static DmMidiInputOptions unpack_options(unsigned packed) {
    return (DmMidiInputOptions){
        .velocity = packed & 3u,
        .fixed_velocity = (packed >> OPTION_FIXED_SHIFT) & MIDI_DATA_MASK,
        .sustain_invert = (packed >> OPTION_INVERT_SHIFT) & 1u,
        .channel = packed >> OPTION_CHANNEL_SHIFT,
    };
}

static void receive(void *context, const uint8_t *bytes, size_t size, uint64_t ns) {
    InputSlot *slot = context;
    DmMidiInput *input = slot->owner;
    atomic_store_explicit(&slot->last_ns, ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&slot->received, 1, memory_order_relaxed);
    DmMidiInputEvent event = {.ticks_ns = ns, .device = slot->index};
    DmMidiVerdict verdict = dm_midi_normalize(bytes, size, &event);

    if (verdict == DM_MIDI_INVALID) {
        atomic_fetch_add_explicit(&slot->invalid, 1, memory_order_relaxed);
    }

    if (verdict == DM_MIDI_SENSING) {
        atomic_store(&slot->sensing, true);
    }

    if (verdict == DM_MIDI_FILTERED || verdict == DM_MIDI_INVALID || verdict == DM_MIDI_SENSING) {
        atomic_fetch_add_explicit(&slot->filtered, 1, memory_order_relaxed);
        return;
    }

    if (dm_midi_echo_match(&input->echo, bytes[0], bytes[1], size == 3 ? bytes[2] : 0, ns)) {
        atomic_fetch_add_explicit(&slot->echoes, 1, memory_order_relaxed);
        return;
    }

    DmMidiInputOptions options =
        unpack_options(atomic_load_explicit(&input->options, memory_order_relaxed));
    bool passes = dm_midi_shape(&event, &options);
    int pedal = pedal_index(event);
    bool ends = ends_admitted(slot, event, pedal);

    if (!passes) {
        atomic_fetch_add_explicit(&slot->filtered, 1, memory_order_relaxed);

        /* After a filter change, releases still end what the previous filter admitted. */
        if (!ends && verdict != DM_MIDI_RELEASE) {
            return;
        }

        event.flags |= DM_MIDI_EVENT_RELEASE;
    }

    DmMidiClockValue clock;

    if (dm_midi_clock_read(&input->clock, &clock)) {
        slot->clock_cache = clock;
    } else {
        atomic_fetch_add_explicit(&slot->timing_fallbacks, 1, memory_order_relaxed);
    }

    dm_midi_clock_stamp(slot->clock_cache, &event);

    bool learn = passes && verdict == DM_MIDI_RECORD && source_learn(input, event);

    if (verdict == DM_MIDI_RELEASE) {
        release_notes(slot, event.status & MIDI_CHANNEL_MASK, event, false);
    } else if (learn || (passes && verdict == DM_MIDI_RECORD && source_bound(input, event))) {
        /* A key or pedal held before it was bound still ends on its original route. */
        if (ends) {
            DmMidiInputEvent release = event;
            release.flags |= DM_MIDI_EVENT_RELEASE;
            admit(slot, release, verdict, pedal);
        }

        event.flags |= learn ? DM_MIDI_EVENT_LEARN : DM_MIDI_EVENT_BOUND;
        enqueue(slot, event, true, false);
    } else {
        admit(slot, event, verdict, pedal);
    }
}

static void slot_state(DmMidiInput *input, InputSlot *slot, DmMidiInputState state,
                       const char *message) {
    SDL_LockMutex(input->lock);
    slot->status.state = state;
    snprintf(slot->status.message, sizeof(slot->status.message), "%s", message);
    SDL_UnlockMutex(input->lock);
}

/* The worker may wait out a writer mid-update; unlike a driver callback it can yield. */
static DmMidiClockValue worker_clock(DmMidiInput *input) {
    DmMidiClockValue value = {0};

    for (int attempt = 0; attempt < 100 && !dm_midi_clock_read(&input->clock, &value); ++attempt) {
        SDL_Delay(0);
    }

    return value;
}

static void worker_release(InputSlot *slot, uint64_t ns) {
    DmMidiInputEvent event = {.ticks_ns = ns, .device = slot->index};
    DmMidiInput *input = slot->owner;
    dm_midi_clock_stamp(worker_clock(input), &event);
    release_notes(slot, -1, event, true);
    event.ticks_ns = SDL_GetTicksNS();
    event.flags = DM_MIDI_EVENT_RELEASE | DM_MIDI_EVENT_DEVICE_RELEASE;
    dm_midi_clock_stamp(worker_clock(input), &event);
    event.status = MIDI_CONTROL;
    event.a = MIDI_CC_NOTES_OFF;
    event.b = 0;
    enqueue(slot, event, true, true);
    event.flags &= (uint8_t)~DM_MIDI_EVENT_DEVICE_RELEASE;
    event.flags |= DM_MIDI_EVENT_RELEASE;
    /* A disconnected pedal must not leave sustained voices or a bent next audition. */
    for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
        event.status = MIDI_CONTROL | c;
        event.a = MIDI_CC_RESET;
        event.b = 0;
        enqueue(slot, event, false, true);
        event.status = MIDI_PITCH_BEND | c;
        event.a = 0;
        event.b = MIDI_BEND_CENTER >> 7;
        enqueue(slot, event, false, true);
    }
    atomic_store_explicit(&slot->pedals[0], 0, memory_order_relaxed);
    atomic_store_explicit(&slot->pedals[1], 0, memory_order_relaxed);
    atomic_store(&slot->sensing, false);
}

static void slot_close(DmMidiInput *input, InputSlot *slot) {
    input->backend.close(input->backend.context, slot->handle);
    slot->handle = NULL;
    atomic_fetch_sub(&input->open, 1);
    worker_release(slot, SDL_GetTicksNS());
}

static int input_worker(void *context) {
    DmMidiInput *input = context;
    uint64_t next_list = 0;
    char names[DM_MIDI_PORTS][DM_MIDI_NAME];
    int count = 0;

    while (!atomic_load(&input->quit)) {
        DmMidiInputStatus wanted[DM_MIDI_INPUTS];
        dm_midi_input_status(input, wanted);
        bool any = false;

        for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
            any |= wanted[i].requested;
        }

        uint64_t now = SDL_GetTicksNS();
        bool refresh = atomic_exchange(&input->refresh, false);
        bool listed = refresh || (any && now >= next_list);
        char error[256] = {0};

        if (listed) {
            count = input->backend.list(input->backend.context, names, DM_MIDI_PORTS, error,
                                        sizeof(error));
            SDL_LockMutex(input->lock);
            input->port_count = count > 0 ? count : 0;
            memcpy(input->ports, names, input->port_count * sizeof(names[0]));
            SDL_UnlockMutex(input->lock);
            next_list = SDL_GetTicksNS() + 1000000000;
        }

        for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
            InputSlot *slot = &input->slots[i];
            bool found = false;

            for (int p = 0; p < count; ++p) {
                found |= !strcmp(names[p], wanted[i].name);
            }

            if (slot->handle && (!wanted[i].requested || (listed && count >= 0 && !found))) {
                slot_close(input, slot);
            }

            if (!wanted[i].requested) {
                slot_state(input, slot, DM_MIDI_CLOSED, "");
                continue;
            }

            if (listed && !slot->handle) {
                if (count < 0 || !found) {
                    slot_state(input, slot, count < 0 ? DM_MIDI_FAILED : DM_MIDI_MISSING,
                               count < 0 ? error : DM_TEXT_MIDI_INPUT_MISSING);
                } else {
                    slot_state(input, slot, DM_MIDI_OPENING, "");
                    slot->clock_cache = worker_clock(input);
                    slot->handle = input->backend.open(input->backend.context, wanted[i].name,
                                                       receive, slot, error, sizeof(error));

                    if (slot->handle) {
                        atomic_fetch_add(&input->open, 1);
                        atomic_store(&input->ready[0], 1);
                        atomic_store(&input->ready[1], 1);
                    }

                    slot_state(input, slot, slot->handle ? DM_MIDI_OPEN : DM_MIDI_FAILED, error);
                }
            }

            uint64_t last = atomic_load(&slot->last_ns);

            if (slot->handle && atomic_load(&slot->sensing) && now > last &&
                now - last > (uint64_t)DM_MIDI_SENSING_MS * 1000000) {
                worker_release(slot, now);
            }
        }

        SDL_WaitSemaphoreTimeout(input->wake, 100);
    }

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        if (input->slots[i].handle) {
            slot_close(input, &input->slots[i]);
        }
    }

    if (input->backend.destroy) {
        input->backend.destroy(input->backend.context);
    }

    return 0;
}

DmMidiInput *dm_midi_input_create(const DmMidiInputBackend *backend) {
    if (backend && (!backend->list || !backend->open || !backend->close)) {
        return NULL;
    }

    DmMidiInput *input = calloc(1, sizeof(*input));
    RtBackend *rt = backend ? NULL : calloc(1, sizeof(*rt));

    if (!input || (!backend && !rt)) {
        free(input);
        free(rt);
        return NULL;
    }

    input->backend =
        backend ? *backend : (DmMidiInputBackend){rt, rt_list, rt_open, rt_close, rt_destroy};
    input->lock = SDL_CreateMutex();
    input->wake = SDL_CreateSemaphore(0);
    atomic_init(&input->thru, true);
    atomic_init(&input->quit, false);
    atomic_init(&input->refresh, false);
    atomic_init(&input->open, 0);
    atomic_init(&input->options, 0);
    atomic_init(&input->learning, DM_MIDI_LEARN_OFF);
    dm_midi_clock_init(&input->clock);
    dm_midi_echo_init(&input->echo);

    for (unsigned c = 0; c < 2; ++c) {
        atomic_init(&input->ready[c], 0);
        atomic_init(&input->overflow[c], false);
    }

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        InputSlot *slot = &input->slots[i];
        slot->owner = input;
        slot->index = i;
        atomic_init(&slot->last_ns, 0);
        atomic_init(&slot->sensing, false);
        atomic_init(&slot->pedals[0], 0);
        atomic_init(&slot->pedals[1], 0);
        atomic_init(&slot->received, 0);
        atomic_init(&slot->filtered, 0);
        atomic_init(&slot->echoes, 0);
        atomic_init(&slot->invalid, 0);
        atomic_init(&slot->timing_fallbacks, 0);
        dm_midi_held_init(&slot->held);

        for (unsigned c = 0; c < MIDI_CHANNELS; ++c) {
            for (unsigned w = 0; w < 8; ++w) {
                atomic_init(&input->bound[i][c][w], 0);
            }
        }

        for (unsigned c = 0; c < 2; ++c) {
            dm_midi_ring_init(&slot->rings[c], slot->events[c], DM_MIDI_RING);
            dm_midi_ring_init(&slot->releases[c], slot->release_events[c], DM_MIDI_RELEASE_RING);
        }
    }

    if (input->lock && input->wake) {
        input->worker = SDL_CreateThread(input_worker, DM_TEXT_MIDI_INPUT, input);
    }

    if (!input->worker) {
        SDL_DestroyMutex(input->lock);
        SDL_DestroySemaphore(input->wake);
        free(rt); /* A caller's backend stays with the caller on failure. */
        free(input);
        return NULL;
    }

    return input;
}

void dm_midi_input_destroy(DmMidiInput *input) {
    if (!input) {
        return;
    }

    atomic_store(&input->quit, true);
    SDL_SignalSemaphore(input->wake);
    SDL_WaitThread(input->worker, NULL);
    free(input->sources);
    SDL_DestroySemaphore(input->wake);
    SDL_DestroyMutex(input->lock);
    free(input);
}

void dm_midi_input_refresh(DmMidiInput *input) {
    atomic_store(&input->refresh, true);
    SDL_SignalSemaphore(input->wake);
}

int dm_midi_input_ports(DmMidiInput *input, char names[][DM_MIDI_NAME], int capacity) {
    SDL_LockMutex(input->lock);
    int count = input->port_count < capacity ? input->port_count : capacity;

    if (count > 0 && names) {
        memcpy(names, input->ports, count * sizeof(*names));
    }

    SDL_UnlockMutex(input->lock);
    return count;
}

bool dm_midi_input_open(DmMidiInput *input, const char *name) {
    if (!name || !*name || strlen(name) >= DM_MIDI_NAME) {
        return false;
    }

    SDL_LockMutex(input->lock);
    InputSlot *slot = NULL;

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        if (!strcmp(input->slots[i].status.name, name)) {
            slot = &input->slots[i];
            break;
        }

        InputSlot *candidate = &input->slots[i];
        bool empty = true;

        for (unsigned c = 0; c < 2; ++c) {
            empty &=
                atomic_load(&candidate->rings[c].read) == atomic_load(&candidate->rings[c].write);
            empty &= atomic_load(&candidate->releases[c].read) ==
                     atomic_load(&candidate->releases[c].write);
        }

        if (!slot &&
            (!*candidate->status.name || (!candidate->status.requested &&
                                          candidate->status.state == DM_MIDI_CLOSED && empty))) {
            slot = &input->slots[i];
        }
    }

    if (slot) {
        if (strcmp(slot->status.name, name)) {
            bind_slot(input, slot->index, name);
        }

        snprintf(slot->status.name, sizeof(slot->status.name), "%s", name);
        slot->status.requested = true;
        if (slot->status.state != DM_MIDI_OPEN) {
            slot->status.state = DM_MIDI_OPENING;
        }
    }

    SDL_UnlockMutex(input->lock);

    if (slot) {
        dm_midi_input_refresh(input);
    }

    return slot != NULL;
}

void dm_midi_input_close(DmMidiInput *input, const char *name) {
    if (!name) {
        return;
    }

    SDL_LockMutex(input->lock);

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        if (!strcmp(input->slots[i].status.name, name)) {
            input->slots[i].status.requested = false;
        }
    }

    SDL_UnlockMutex(input->lock);
    SDL_SignalSemaphore(input->wake);
}

void dm_midi_input_status(DmMidiInput *input, DmMidiInputStatus status[DM_MIDI_INPUTS]) {
    SDL_LockMutex(input->lock);

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        status[i] = input->slots[i].status;
    }

    SDL_UnlockMutex(input->lock);
}

void dm_midi_input_stats(DmMidiInput *input, DmMidiInputStats *stats) {
    *stats = (DmMidiInputStats){0};

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        InputSlot *slot = &input->slots[i];
        stats->received += atomic_load(&slot->received);
        stats->filtered += atomic_load(&slot->filtered);
        stats->echoes += atomic_load(&slot->echoes);
        stats->dropped += atomic_load(&slot->invalid);
        stats->timing_fallbacks += atomic_load(&slot->timing_fallbacks);

        for (unsigned c = 0; c < 2; ++c) {
            stats->dropped +=
                atomic_load(&slot->rings[c].dropped) + atomic_load(&slot->releases[c].dropped);
        }
    }
}

void dm_midi_input_set_options(DmMidiInput *input, const DmMidiInputOptions *options) {
    if (!input || !options || (unsigned)options->velocity > DM_MIDI_VELOCITY_HEAVY ||
        options->fixed_velocity > MIDI_DATA_MAX || options->channel > MIDI_CHANNELS) {
        return;
    }

    /* One atomic snapshot keeps callbacks from observing a partially changed configuration. */
    atomic_store_explicit(&input->options, pack_options(options), memory_order_relaxed);
}

bool dm_midi_source_bindable(DmMidiInputSource kind, unsigned number) {
    if (number > MIDI_DATA_MAX) {
        return false;
    }

    return kind == DM_MIDI_SOURCE_NOTE ||
           (kind == DM_MIDI_SOURCE_CC && number < MIDI_CC_SOUND_OFF && number != MIDI_CC_BANK &&
            number != MIDI_CC_BANK_LSB && number != MIDI_CC_DATA && number != MIDI_CC_DATA_LSB &&
            (number < MIDI_CC_INCREMENT || number > MIDI_CC_RPN_MSB));
}

bool dm_midi_input_bind_sources(DmMidiInput *input, const DmMidiBindingSource *sources,
                                size_t count) {
    if (!input || (count && !sources) || count > SIZE_MAX / sizeof(*sources)) {
        return false;
    }

    DmMidiBindingSource *copy = count ? malloc(count * sizeof(*copy)) : NULL;

    if (count && !copy) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        if (!memchr(sources[i].device, 0, sizeof(sources[i].device)) ||
            sources[i].channel >= MIDI_CHANNELS ||
            !dm_midi_source_bindable(sources[i].kind, sources[i].number)) {
            free(copy);
            return false;
        }

        copy[i] = sources[i];
    }

    SDL_LockMutex(input->lock);
    free(input->sources);
    input->sources = copy;
    input->source_count = count;

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        bind_slot(input, i, input->slots[i].status.name);
    }

    SDL_UnlockMutex(input->lock);
    return true;
}

void dm_midi_input_learn(DmMidiInput *input, DmMidiLearnMode mode) {
    atomic_store_explicit(&input->learning, mode, memory_order_relaxed);
}

void dm_midi_input_set_thru(DmMidiInput *input, bool enabled) {
    atomic_store(&input->thru, enabled);
}

bool dm_midi_input_thru(DmMidiInput *input) {
    return atomic_load(&input->thru);
}

DmMidiClock *dm_midi_input_clock(DmMidiInput *input) {
    return &input->clock;
}

DmMidiEcho *dm_midi_input_echo(DmMidiInput *input) {
    return &input->echo;
}

bool dm_midi_input_is_open(DmMidiInput *input) {
    return atomic_load(&input->open) != 0;
}

bool dm_midi_input_begin(DmMidiInput *input, unsigned consumer) {
    if (!atomic_load_explicit(&input->ready[consumer], memory_order_acquire)) {
        return false;
    }

    atomic_exchange(&input->ready[consumer], 0);
    return true;
}

bool dm_midi_input_pop(DmMidiInput *input, unsigned consumer, DmMidiInputEvent *event) {
    DmMidiRing *best = NULL;
    DmMidiInputEvent candidate;

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        DmMidiRing *rings[] = {&input->slots[i].rings[consumer],
                               &input->slots[i].releases[consumer]};

        for (unsigned r = 0; r < 2; ++r) {
            if (dm_midi_ring_peek(rings[r], &candidate) &&
                (!best || candidate.ticks_ns < event->ticks_ns)) {
                best = rings[r];
                *event = candidate;
            }
        }
    }

    return best && dm_midi_ring_get(best, event);
}

void dm_midi_input_end(DmMidiInput *input, unsigned consumer) {
    bool pending = dm_midi_input_is_open(input);

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        DmMidiInputEvent event;
        pending |= dm_midi_ring_peek(&input->slots[i].rings[consumer], &event) ||
                   dm_midi_ring_peek(&input->slots[i].releases[consumer], &event);
    }

    if (pending) {
        atomic_store_explicit(&input->ready[consumer], 1, memory_order_release);
    }
}

bool dm_midi_input_overflow(DmMidiInput *input, unsigned consumer) {
    return atomic_exchange_explicit(&input->overflow[consumer], false, memory_order_acquire);
}
