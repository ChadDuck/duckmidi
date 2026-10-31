#ifndef DM_TEST_WAVE_FIXTURE_H
#define DM_TEST_WAVE_FIXTURE_H

/* Find PCM data without assuming a particular order of optional RIFF chunks. */
static size_t wave_data_offset(const DmBytes *wave, size_t *size) {
    *size = 0;

    if (wave->size < 12 || memcmp(wave->data, "RIFF", 4) || memcmp(wave->data + 8, "WAVE", 4)) {
        return wave->size;
    }

    for (size_t offset = 12; offset + 8 <= wave->size;) {
        const unsigned char *p = wave->data + offset + 4;
        size_t length = (size_t)p[0] | (size_t)p[1] << 8 | (size_t)p[2] << 16 | (size_t)p[3] << 24;

        if (length > wave->size - offset - 8) {
            break;
        }

        if (!memcmp(wave->data + offset, "data", 4)) {
            *size = length;
            return offset + 8;
        }

        offset += 8 + length + (length & 1);
    }

    return wave->size;
}
#endif
