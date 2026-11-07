#include "internal.h"

typedef struct {
    Buffer bytes;
    uint64_t hash;
    size_t uses, address;
    int transpose, program;
} Phrase;

typedef struct {
    size_t *items, count, capacity;
} Layer;

typedef struct {
    unsigned channel, layer;
    size_t size;
} LayerOrder;

static uint64_t hash_bytes(const Buffer *b) {
    uint64_t h = 1469598103934665603ULL;

    for (size_t i = 0; i < b->size; i++) {
        h = (h ^ b->data[i]) * 1099511628211ULL;
    }

    return h;
}

/* With a loop region, the intro and the repeated region are written as separate layer scripts;
   each starts its notes from the beginning of its own section. */
static bool in_section(const Compile *c, const Note *n) {
    return !c->region || n->section == c->emit_section;
}

static uint64_t section_start(const Compile *c, const Note *n) {
    uint64_t base = c->region && c->emit_section ? c->loop_start : 0;
    return n->start > base ? n->start - base : 0;
}

bool emit_layers(Compile *c, Buffer *b, size_t fixes[16][4]) {
    Layer layers[16][4] = {0};
    Phrase *phrases = NULL;
    size_t count = 0, capacity = 0;
    size_t table_size = 32;

    while (table_size < c->count * 2 + 32) {
        table_size *= 2;
    }

    size_t *table = calloc(table_size, sizeof(*table));
    bool ok = table != NULL;

    for (unsigned ch = 0; ch < 16 && ok; ch++) {
        for (unsigned l = 0; l < 4 && ok; l++) {
            if (fixes[ch][l]) {
                uint64_t now = 0;
                unsigned chunk = 0;
                /* sm64coopdx layers start with their channel's instrument. */
                int start_program = c->coopdx ? c->layer_instrument[ch] : -1;
                int transpose = 0, program = start_program, entry_transpose = 0,
                    entry_program = start_program;
                Buffer phrase = {0};
                unsigned pan = 64;

                for (size_t i = 0; i <= c->count && ok; i++) {
                    const Note *n = i < c->count ? &c->notes[i] : NULL;

                    if (n && (n->output != ch || n->layer != l || n->end <= n->start ||
                              !in_section(c, n))) {
                        continue;
                    }

                    int instrument = -1;
                    unsigned pitch = 0;

                    if (n && c->coopdx) {
                        coopdx_note(c, n, &instrument, &pitch);

                        if (instrument < 0) {
                            continue;
                        }
                    } else if (n) {
                        pitch = n->channel == MIDI_DRUM_CHANNEL
                                    ? (unsigned)c->drums[n->drum]
                                    : (n->key >= 117 ? n->key - 12 : n->key);
                    }

                    uint64_t start = n ? section_start(c, n) : 0;

                    if (n) {
                        if (!(i % 1024) && cancelled(c, 2, 50)) {
                            ok = false;
                            break;
                        }

                        delay(&phrase, 0xc0, start - now);
                        bool preset_pan =
                            c->preset_pan[n->channel] && n->channel != MIDI_DRUM_CHANNEL;
                        unsigned wanted_pan =
                            n->custom ? es_native_pan(&c->custom->samples->regions[n->region]) : 64;

                        /* Every phrase establishes pan, so shared calls have no hidden pan state.
                         */
                        if (preset_pan && (!chunk || wanted_pan != pan)) {
                            put(&phrase, 0xca);
                            put(&phrase, wanted_pan);
                            pan = wanted_pan;
                        }

                        int shift = (int)(pitch & ~63u);

                        if (shift != transpose) {
                            put(&phrase, 0xc2);
                        }

                        /* Phrase identity includes its incoming musical state. */
                        if (shift != transpose) {
                            put(&phrase, (unsigned)shift);
                            transpose = shift;
                        }

                        if (c->coopdx && program != instrument) {
                            program = instrument;
                            put(&phrase, 0xc6);
                            put(&phrase, (unsigned)program);
                        } else if (!c->coopdx && n->channel != MIDI_DRUM_CHANNEL &&
                                   program != c->slots[n->program]) {
                            program = c->slots[n->program];
                            put(&phrase, 0xc6);
                            put(&phrase, (unsigned)program);
                        }

                        uint64_t duration = n->end - n->start;
                        unsigned gate = 0;

                        for (size_t next = i + 1; next < c->count; next++) {
                            const Note *other = &c->notes[next];

                            if (other->output != ch || other->layer != l ||
                                other->end <= other->start || !in_section(c, other)) {
                                continue;
                            }

                            if (c->coopdx) {
                                int other_instrument;
                                unsigned other_pitch;
                                coopdx_note(c, other, &other_instrument, &other_pitch);

                                if (other_instrument < 0) {
                                    continue;
                                }
                            }

                            uint64_t interval = other->start - n->start,
                                     gap = other->start - n->end;

                            if (interval <= 32767 && gap && gap < interval) {
                                uint64_t candidate = (gap * 256 + interval - 1) / interval;

                                if (candidate < 256 && candidate * interval / 256 == gap) {
                                    duration = interval;
                                    gate = (unsigned)candidate;
                                }
                            }
                            break;
                        }

                        uint64_t advance = duration;

                        while (duration) {
                            uint64_t part = duration > 32767 ? 32767 : duration;
                            put(&phrase, (gate ? 0 : 0x40) + (pitch & 63));
                            variable(&phrase, part);
                            put(&phrase, n->velocity);

                            if (gate) {
                                put(&phrase, gate);
                            }

                            duration -= part;
                        }

                        now = start + advance;
                        chunk++;
                    }

                    if (chunk && (chunk == (c->phrase_notes ? c->phrase_notes : 32) || !n)) {
                        if (phrase.failed) {
                            ok = false;
                            break;
                        }

                        uint64_t hash = hash_bytes(&phrase) ^
                                        ((uint64_t)(entry_program + 1) << 32) ^
                                        (unsigned)entry_transpose;
                        size_t slot = (size_t)hash & (table_size - 1);

                        while (table[slot]) {
                            Phrase *p = &phrases[table[slot] - 1];

                            if (p->hash == hash && p->transpose == entry_transpose &&
                                p->program == entry_program && p->bytes.size == phrase.size &&
                                !memcmp(p->bytes.data, phrase.data, phrase.size)) {
                                break;
                            }

                            slot = (slot + 1) & (table_size - 1);
                        }

                        size_t id;

                        if (table[slot]) {
                            id = table[slot] - 1;
                            free(phrase.data);
                        } else {
                            if (!reserve((void **)&phrases, &capacity, count + 1,
                                         sizeof(*phrases))) {
                                ok = false;
                                break;
                            }

                            id = count++;
                            phrases[id] =
                                (Phrase){phrase, hash, 0, 0, entry_transpose, entry_program};
                            table[slot] = id + 1;
                        }

                        phrase = (Buffer){0};
                        phrases[id].uses++;
                        Layer *layer = &layers[ch][l];

                        if (!reserve((void **)&layer->items, &layer->capacity, layer->count + 1,
                                     sizeof(size_t))) {
                            ok = false;
                            break;
                        }

                        layer->items[layer->count++] = id;
                        chunk = 0;
                        entry_transpose = transpose;
                        entry_program = program;
                    }
                }

                free(phrase.data);
            }
        }
    }

    LayerOrder order[64];
    size_t order_count = 0;

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned l = 0; l < 4; l++) {
            if (fixes[ch][l]) {
                size_t size = 7;
                Layer *layer = &layers[ch][l];

                for (size_t i = 0; i < layer->count; i++) {
                    Phrase *p = &phrases[layer->items[i]];
                    size += c->phrase_notes && p->uses > 1 && p->bytes.size > 7 ? 3 : p->bytes.size;
                }

                size_t pos = order_count++;
                /* Put the longest sequential script last, so its tail need not be
                   addressed. Script layout alone must not force musical reduction. */
                while (pos && order[pos - 1].size > size) {
                    order[pos] = order[pos - 1];
                    pos--;
                }

                order[pos] = (LayerOrder){ch, l, size};
            }
        }
    }

    /* Shared phrases come before the layer scripts: every call and layer entry must stay below
       64 KiB, but the longest layer, written last, may run past it. */
    for (size_t i = 0; i < count && ok; i++) {
        if (c->phrase_notes && phrases[i].uses > 1 && phrases[i].bytes.size > 7) {
            phrases[i].address = b->size;
            bytes(b, phrases[i].bytes.data, phrases[i].bytes.size);
            put(b, 0xff);
        }
    }

    for (size_t item = 0; item < order_count && ok; item++) {
        unsigned ch = order[item].channel, l = order[item].layer;
        address(b, fixes[ch][l], b->size);
        put(b, 0xc5);
        put(b, 0xc8);
        put(b, 0xca);
        put(b, 64);
        put(b, 0xc2);
        put(b, 0);
        Layer *layer = &layers[ch][l];

        for (size_t i = 0; i < layer->count; i++) {
            size_t id = layer->items[i];
            Phrase *p = &phrases[id];

            if (c->phrase_notes && p->uses > 1 && p->bytes.size > 7) {
                put(b, 0xfc);
                size_t fix = b->size;
                put(b, 0);
                put(b, 0);
                address(b, fix, p->address);
            } else {
                bytes(b, p->bytes.data, p->bytes.size);
            }
        }

        put(b, 0xff);
    }

    for (unsigned ch = 0; ch < 16; ch++) {
        for (unsigned l = 0; l < 4; l++) {
            free(layers[ch][l].items);
        }
    }

    for (size_t i = 0; i < count; i++) {
        free(phrases[i].bytes.data);
    }

    free(phrases);
    free(table);

    if (!ok && c->result->status == M64_OK) {
        c->result->status = M64_NOMEM;
    }

    return ok;
}
