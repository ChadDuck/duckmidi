/* SM64 sample choices for the mapping panel and the replacement review.

   Many GM programs and drum notes share one stock sample. Presets with the same sample and the
   same settings are listed once. Presets that reuse a sample with different settings stay
   separate, and their labels name the difference: envelope for melodic samples, pitch for
   drums. */
#include "m64_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const M64Instrument *preset(bool percussion, int index) {
    const M64Api *api = m64_get_api_3();
    return percussion ? api->default_drum((uint32_t)index) : api->default_program((uint32_t)index);
}

static bool same_settings(const M64Instrument *a, const M64Instrument *b) {
    return a->tuning == b->tuning && a->release_rate == b->release_rate && a->attack == b->attack &&
           a->decay == b->decay && a->sustain == b->sustain;
}

int m64_canonical_preset(bool percussion, int index) {
    if (index < 0 || index > 127) {
        return 0;
    }

    const M64Instrument *target = preset(percussion, index);

    for (int i = 0; i < index; ++i) {
        const M64Instrument *other = preset(percussion, i);

        if (!strcmp(other->sample, target->sample) && same_settings(other, target)) {
            return i;
        }
    }

    return index;
}

static bool has_variants(bool percussion, const M64Instrument *target) {
    for (int i = 0; i < 128; ++i) {
        const M64Instrument *other = preset(percussion, i);

        if (!strcmp(other->sample, target->sample) && !same_settings(other, target)) {
            return true;
        }
    }

    return false;
}

void m64_preset_label(bool percussion, int index, char *out, size_t capacity) {
    const M64Instrument *target = preset(percussion, m64_canonical_preset(percussion, index));

    if (!has_variants(percussion, target)) {
        snprintf(out, capacity, "%s", target->sample);
    } else if (percussion) {
        snprintf(out, capacity, "%s (pitch %.2fx)", target->sample, target->tuning);
    } else {
        const char *release = target->release_rate <= 19   ? "short release"
                              : target->release_rate <= 30 ? "medium release"
                                                           : "long release";
        snprintf(out, capacity, "%s (%s%s, %s)", target->sample,
                 target->attack > 2 ? "soft attack, " : "",
                 target->sustain ? "sustained" : "decaying", release);
    }
}

static int compare_labels(const void *left, const void *right) {
    return strcmp(((const DmeChoice *)left)->label, ((const DmeChoice *)right)->label);
}

unsigned m64_preset_choices(bool percussion, DmeChoice *choices, char (*labels)[220]) {
    unsigned count = 0;

    for (int i = 0; i < 128; ++i) {
        if (m64_canonical_preset(percussion, i) != i) {
            continue;
        }

        m64_preset_label(percussion, i, labels[count], 220);
        choices[count] = (DmeChoice){i, labels[count]};
        ++count;
    }

    /* Sample names begin with their bank index, so sorting by label follows the stock bank. */
    qsort(choices, count, sizeof(*choices), compare_labels);
    return count;
}
