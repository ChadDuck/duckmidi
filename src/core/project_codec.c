#include "core/project_internal.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

/* Schema 10 stores numbers compactly. Integers are variable-length (7 bits per byte). A real
   that is an exact multiple of 1/DM_REAL_STEPS, as beats on any common MIDI resolution are, is
   stored as that small integer; any other real keeps its full 8 bytes. */

bool dm_put_fixed32(DmBytes *b, uint32_t n) {
    uint8_t v[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    return dm_bytes(b, v, 4);
}

bool dm_put_varint(DmBytes *b, uint64_t n) {
    uint8_t v[10];
    size_t size = 0;

    do {
        v[size++] = (uint8_t)((n & 127) | (n > 127 ? 128 : 0));
        n >>= 7;
    } while (n);

    return dm_bytes(b, v, size);
}

bool dm_put_real(DmBytes *b, double n) {
    if (isfinite(n) && fabs(n) < 1e12) {
        int64_t steps = llround(n * DM_REAL_STEPS);

        if ((double)steps / DM_REAL_STEPS == n) {
            uint64_t zigzag =
                steps < 0 ? ((uint64_t)(-(steps + 1)) << 1) | 1 : (uint64_t)steps << 1;
            return dm_put_varint(b, zigzag << 1);
        }
    }

    uint64_t bits;
    memcpy(&bits, &n, 8);
    return dm_put_varint(b, 1) && dm_put_fixed32(b, (uint32_t)bits) &&
           dm_put_fixed32(b, (uint32_t)(bits >> 32));
}

/* Times in time-ordered lists are stored as the step difference from the previous time, which
   is usually one or two bytes. Times that are not exact steps are stored in full. */
bool dm_exact_steps(double n, int64_t *steps) {
    if (!isfinite(n) || fabs(n) >= 1e12) {
        return false;
    }

    *steps = llround(n * DM_REAL_STEPS);
    return (double)*steps / DM_REAL_STEPS == n;
}

bool dm_put_time_after(DmBytes *b, double n, double previous) {
    int64_t now, before;

    if (dm_exact_steps(n, &now) && dm_exact_steps(previous, &before)) {
        int64_t delta = now - before;
        uint64_t zigzag = delta < 0 ? ((uint64_t)(-(delta + 1)) << 1) | 1 : (uint64_t)delta << 1;
        return dm_put_varint(b, zigzag << 1);
    }

    uint64_t bits;
    memcpy(&bits, &n, 8);
    return dm_put_varint(b, 1) && dm_put_fixed32(b, (uint32_t)bits) &&
           dm_put_fixed32(b, (uint32_t)(bits >> 32));
}

bool dm_put_block(DmBytes *b, const void *data, size_t size) {
    return size <= UINT32_MAX && dm_put_varint(b, (uint32_t)size) && dm_bytes(b, data, size);
}

bool dm_put_string(DmBytes *b, const char *s) {
    return dm_put_block(b, s, strlen(s));
}

uint32_t dm_get_fixed32(Input *in) {
    if (in->left < 4) {
        in->ok = false;
        return 0;
    }

    const uint8_t *p = in->p;
    uint32_t n = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    in->p += 4;
    in->left -= 4;
    return n;
}

uint64_t dm_get_varint(Input *in) {
    uint64_t n = 0;

    for (int shift = 0; shift < 64; shift += 7) {
        if (!in->left) {
            break;
        }

        uint8_t byte = *in->p++;
        --in->left;
        n |= (uint64_t)(byte & 127) << shift;

        if (!(byte & 128)) {
            return n;
        }
    }

    in->ok = false;
    return 0;
}

double dm_get_real10(Input *in) {
    uint64_t tag = dm_get_varint(in);

    if (!(tag & 1)) {
        uint64_t zigzag = tag >> 1;
        int64_t steps = zigzag & 1 ? -(int64_t)(zigzag >> 1) - 1 : (int64_t)(zigzag >> 1);
        return (double)steps / DM_REAL_STEPS;
    }

    if (tag != 1) {
        in->ok = false;
        return 0;
    }

    uint64_t bits = dm_get_fixed32(in);
    bits |= (uint64_t)dm_get_fixed32(in) << 32;
    double n;
    memcpy(&n, &bits, 8);

    if (!isfinite(n)) {
        in->ok = false;
    }

    return n;
}

double dm_get_time_after10(Input *in, double previous) {
    uint64_t tag = dm_get_varint(in);
    int64_t before;

    if (!(tag & 1)) {
        uint64_t zigzag = tag >> 1;
        int64_t delta = zigzag & 1 ? -(int64_t)(zigzag >> 1) - 1 : (int64_t)(zigzag >> 1);

        if (!dm_exact_steps(previous, &before)) {
            in->ok = false;
            return 0;
        }

        return (double)(before + delta) / DM_REAL_STEPS;
    }

    if (tag != 1) {
        in->ok = false;
        return 0;
    }

    uint64_t bits = dm_get_fixed32(in);
    bits |= (uint64_t)dm_get_fixed32(in) << 32;
    double n;
    memcpy(&n, &bits, 8);

    if (!isfinite(n)) {
        in->ok = false;
    }

    return n;
}

/* Standard CRC-32, one table lookup per byte. Undo snapshots checksum the whole project on every
   edit, so this matters for large songs. */
uint32_t dm_crc32(const uint8_t *p, size_t size) {
    static uint32_t table[256];
    static atomic_int state;

    if (atomic_load_explicit(&state, memory_order_acquire) != 2) {
        int expected = 0;

        if (atomic_compare_exchange_strong_explicit(&state, &expected, 1, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            for (uint32_t n = 0; n < 256; ++n) {
                uint32_t c = n;

                for (int b = 0; b < 8; ++b) {
                    c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
                }

                table[n] = c;
            }

            atomic_store_explicit(&state, 2, memory_order_release);
        } else {
            while (atomic_load_explicit(&state, memory_order_acquire) != 2) {
            }
        }
    }

    uint32_t crc = ~0u;

    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }

    return ~crc;
}
