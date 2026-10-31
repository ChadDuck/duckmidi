/* Small LZ77 compressor for project bodies.

   Stream: the uncompressed size, then repeated [literal count][literals][match length - 4]
   [match distance] until the size is reached; the last literals have no match. Every number is a
   variable-length integer (7 bits per byte). Project data repeats record layouts and values, so
   this usually shrinks it by several times without an external library. */
#include "duck.h"
#include <stdlib.h>
#include <string.h>

enum { HASH_BITS = 16, MIN_MATCH = 4 };

static bool put_varint(DmBytes *out, uint64_t n) {
    uint8_t v[10];
    size_t size = 0;

    do {
        v[size++] = (uint8_t)((n & 127) | (n > 127 ? 128 : 0));
        n >>= 7;
    } while (n);

    return dm_bytes(out, v, size);
}

static bool get_varint(const uint8_t **p, const uint8_t *end, uint64_t *n) {
    *n = 0;

    for (int shift = 0; shift < 64 && *p < end; shift += 7) {
        uint8_t byte = *(*p)++;
        *n |= (uint64_t)(byte & 127) << shift;

        if (!(byte & 128)) {
            return true;
        }
    }

    return false;
}

static uint32_t hash4(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - HASH_BITS);
}

bool dm_compress(const uint8_t *data, size_t size, DmBytes *out) {
    int64_t *table = malloc(sizeof(int64_t) << HASH_BITS);

    if (!table) {
        return false;
    }

    for (size_t i = 0; i < (size_t)1 << HASH_BITS; ++i) {
        table[i] = -1;
    }

    bool ok = put_varint(out, size);
    size_t anchor = 0, i = 0;

    while (ok && i + MIN_MATCH <= size) {
        uint32_t h = hash4(data + i);
        int64_t candidate = table[h];
        table[h] = (int64_t)i;

        if (candidate < 0 || memcmp(data + candidate, data + i, MIN_MATCH)) {
            ++i;
            continue;
        }

        size_t length = MIN_MATCH;

        while (i + length < size && data[candidate + length] == data[i + length]) {
            ++length;
        }

        ok = put_varint(out, i - anchor) && dm_bytes(out, data + anchor, i - anchor) &&
             put_varint(out, length - MIN_MATCH) && put_varint(out, i - (size_t)candidate);

        for (size_t k = i + 1; k < i + length && k + MIN_MATCH <= size; ++k) {
            table[hash4(data + k)] = (int64_t)k;
        }

        i += length;
        anchor = i;
    }

    ok = ok && put_varint(out, size - anchor) && dm_bytes(out, data + anchor, size - anchor);
    free(table);
    return ok;
}

bool dm_decompress(const uint8_t *data, size_t size, size_t limit, DmBytes *out) {
    const uint8_t *p = data, *end = data + size;
    uint64_t total;

    if (!get_varint(&p, end, &total) || total > limit) {
        return false;
    }

    uint8_t *buffer = malloc(total ? total : 1);

    if (!buffer) {
        return false;
    }

    size_t used = 0;

    while (true) {
        uint64_t literals, length, distance;

        if (!get_varint(&p, end, &literals) || literals > total - used ||
            literals > (uint64_t)(end - p)) {
            break;
        }

        memcpy(buffer + used, p, (size_t)literals);
        p += literals;
        used += (size_t)literals;

        if (used == total) {
            if (p != end) {
                break;
            }

            out->data = buffer;
            out->size = out->capacity = total;
            return true;
        }

        if (!get_varint(&p, end, &length) || !get_varint(&p, end, &distance) || !distance ||
            distance > used || length + MIN_MATCH > total - used) {
            break;
        }

        /* Byte by byte: a match may overlap the bytes it is producing. */
        for (uint64_t k = 0; k < length + MIN_MATCH; ++k, ++used) {
            buffer[used] = buffer[used - distance];
        }
    }

    free(buffer);
    return false;
}
