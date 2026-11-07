#include "native_sequence_decode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *bytes;
    size_t size;
    const char *text;
    NativeDecodedSequence *sequence;
    uint64_t tick;
    unsigned channel, layer, pan, weight, transpose;
    unsigned short_gate; /* OoT short notes: the layer's gate. */
} Decoder;

static unsigned byte(Decoder *d, size_t *at) {
    assert(*at < d->size);
    return d->bytes[(*at)++];
}

static unsigned variable(Decoder *d, size_t *at) {
    unsigned value = byte(d, at);
    return value & 128 ? (value & 127) * 256 + byte(d, at) : value;
}

static unsigned address(Decoder *d, size_t *at) {
    unsigned hi = byte(d, at);
    return hi * 256 + byte(d, at);
}

static void note(Decoder *d, unsigned key, unsigned duration, unsigned gate) {
    NativeDecodedSequence *s = d->sequence;
    assert(s->count < sizeof(s->notes) / sizeof(*s->notes));
    s->notes[s->count++] = (NativeDecodedNote){d->tick,
                                               d->tick + duration - duration * gate / 256,
                                               d->channel,
                                               d->layer,
                                               key + d->transpose,
                                               d->pan,
                                               d->weight};
    d->tick += duration;
}

static void control(Decoder *d, unsigned op, unsigned value) {
    NativeDecodedSequence *s = d->sequence;
    assert(s->control_count < sizeof(s->controls) / sizeof(*s->controls));
    s->controls[s->control_count++] = (NativeDecodedControl){d->tick, d->channel, op, value};
}

static void m64_layer(Decoder *d, size_t at, unsigned depth) {
    assert(depth < 32);

    for (unsigned steps = 0; steps < 100000; steps++) {
        unsigned op = byte(d, &at);

        if (op < 128) {
            unsigned duration = variable(d, &at);
            byte(d, &at);
            unsigned gate = op < 64 ? byte(d, &at) : 0;
            note(d, op & 63, duration, gate);
        } else if (op == 0xc0) {
            d->tick += variable(d, &at);
        } else if (op == 0xca) {
            d->pan = byte(d, &at);
        } else if (op == 0xc2) {
            d->transpose = byte(d, &at);
        } else if (op == 0xc6) {
            byte(d, &at);
        } else if (op == 0xfc) {
            unsigned target = address(d, &at);
            d->sequence->calls++;
            m64_layer(d, target, depth + 1);
        } else if (op == 0xfb) {
            at = address(d, &at);
        } else if (op == 0xff) {
            return;
        } else {
            assert(op == 0xc5 || op == 0xc8);
        }
    }

    assert(!"unterminated M64 layer");
}

static void m64_channel(Decoder *d, size_t at) {
    unsigned layers[4] = {0};
    d->weight = 128;
    d->tick = 0;

    for (unsigned steps = 0; steps < 100000; steps++) {
        unsigned op = byte(d, &at);

        if (op >= 0x90 && op <= 0x93) {
            layers[op - 0x90] = address(d, &at);
        } else if (op == 0xdc) {
            d->weight = byte(d, &at);
        } else if (op == 0xde) {
            address(d, &at);
        } else if (op == 0xfd) {
            d->tick += variable(d, &at);
        } else if (op == 0xd4 || op == 0xd7 || op == 0xd8) {
            control(d, op, byte(d, &at));
        } else if (op == 0xfb || op == 0xf3 || op == 0xff) {
            break;
        } else if (op != 0xc4) {
            assert(op == 0xc1 || op == 0xdb || op == 0xdf || op == 0xe0 || op == 0xdd ||
                   op == 0xd2);
            byte(d, &at);
        }
    }

    for (unsigned i = 0; i < 4; i++) {
        if (layers[i]) {
            d->layer = i;
            d->tick = d->transpose = 0;
            d->pan = 64;
            m64_layer(d, layers[i], 0);
        }
    }
}

void native_decode_m64(const uint8_t *bytes, size_t size, NativeDecodedSequence *sequence) {
    *sequence = (NativeDecodedSequence){0};
    Decoder d = {.bytes = bytes, .size = size, .sequence = sequence};
    size_t at = 0;

    for (unsigned steps = 0; steps < 100000; steps++) {
        unsigned op = byte(&d, &at);

        if (op >= 0x90 && op <= 0x9f) {
            unsigned target = address(&d, &at);
            d.channel = op - 0x90;
            m64_channel(&d, target);
        } else if (op == 0xd7) {
            address(&d, &at);
        } else if (op == 0xfd) {
            variable(&d, &at);
        } else if (op == 0xff || op == 0xfb) {
            return;
        } else {
            assert(op == 0xd3 || op == 0xd5 || op == 0xd0 || op == 0xdf || op == 0xdb ||
                   op == 0xdd);
            byte(&d, &at);
        }
    }

    assert(!"unterminated M64 sequence");
}

static void oot_layer(Decoder *d, const char *name, unsigned depth) {
    assert(depth < 32);
    char label[96];
    snprintf(label, sizeof(label), ".layer %s\n", name);
    const char *line = strstr(d->text, label);
    assert(line);
    line += strlen(label);

    while (*line) {
        unsigned key, duration, velocity, gate, value, sub;

        if (sscanf(line, " notedvg %u, %u, %u, %u", &key, &duration, &velocity, &gate) == 4) {
            note(d, key + 21, duration, gate);
        } else if (sscanf(line, " notedv %u, %u, %u", &key, &duration, &velocity) == 3) {
            note(d, key + 21, duration, 0);
        } else if (sscanf(line, " shortdvg %u, %u", &key, &duration) == 2) {
            note(d, key + 21, duration, d->short_gate);
        } else if (sscanf(line, " shortgate %u", &value) == 1) {
            d->short_gate = value;
        } else if (sscanf(line, " shortvel %u", &value) == 1) {
            /* Velocity does not affect the decoded timing. */
        } else if (sscanf(line, " ldelay %u", &duration) == 1) {
            d->tick += duration;
        } else if (sscanf(line, " notepan %u", &value) == 1) {
            d->pan = value;
        } else if (sscanf(line, " transpose %u", &value) == 1) {
            d->transpose = value;
        } else if (sscanf(line, " call LSUB_%u", &sub) == 1) {
            char target[48];
            snprintf(target, sizeof(target), "LSUB_%u", sub);
            d->sequence->calls++;
            oot_layer(d, target, depth + 1);
        } else {
            char command[32];
            assert(sscanf(line, " %31s", command) == 1);

            if (!strcmp(command, "end")) {
                return;
            }

            assert(!strcmp(command, "instr"));
        }

        line = strchr(line, '\n');
        assert(line);
        line++;
    }

    assert(!"unterminated OoT layer");
}

void native_decode_oot(const char *text, NativeDecodedSequence *sequence) {
    *sequence = (NativeDecodedSequence){0};
    Decoder d = {.text = text, .sequence = sequence};
    const char *at = text;

    for (const char *channel = text; channel && (channel = strstr(channel, "\n.channel CHAN_"));) {
        unsigned section;
        assert(sscanf(channel, " .channel CHAN_%u_%u", &d.channel, &section) == 2);
        channel = strchr(channel + 1, '\n');
        d.tick = 0;

        while (section == 0 && channel && channel[1] && channel[1] != '.') {
            unsigned value;

            if (sscanf(channel, " delay %u", &value) == 1) {
                d.tick += value;
            } else if (sscanf(channel, " reverb %u", &value) == 1) {
                control(&d, 0xd4, value);
            } else if (sscanf(channel, " vibfreq %u", &value) == 1) {
                control(&d, 0xd7, value);
            } else if (sscanf(channel, " vibdepth %u", &value) == 1) {
                control(&d, 0xd8, value);
            }

            channel = strchr(channel + 1, '\n');
        }
    }

    while ((at = strstr(at, "\n.layer LAYER_")) != NULL) {
        unsigned section;
        char name[64], channel_label[64];
        assert(sscanf(at, " .layer %63s", name) == 1);
        assert(sscanf(name, "LAYER_%u_%u_%u", &d.channel, &section, &d.layer) == 3);
        assert(section == 0);
        snprintf(channel_label, sizeof(channel_label), ".channel CHAN_%u_0\n", d.channel);
        const char *channel = strstr(text, channel_label);
        assert(channel);
        const char *end = strstr(channel, "\n.");
        const char *weight = strstr(channel, "panweight ");
        d.weight = 128;

        if (weight && (!end || weight < end)) {
            assert(sscanf(weight, "panweight %u", &d.weight) == 1);
        }

        d.tick = d.transpose = 0;
        d.pan = 64;
        oot_layer(&d, name, 0);
        at++;
    }
}
