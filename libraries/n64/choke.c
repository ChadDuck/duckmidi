#include "sample.h"
#include <stdlib.h>

typedef struct {
    uint64_t key;
    size_t head;
    uint64_t cut_tick;
    bool cut;
} ChokeGroup;

static ChokeGroup *group(ChokeGroup *groups, size_t mask, uint16_t choice, uint16_t channel,
                         uint32_t number) {
    uint64_t key = (uint64_t)choice << 48 | (uint64_t)channel << 32 | number;
    uint64_t hash = key ^ (key >> 33);
    hash *= UINT64_C(0xff51afd7ed558ccd);
    size_t slot = (size_t)(hash ^ (hash >> 33)) & mask;

    while (groups[slot].key && groups[slot].key != key) {
        slot = (slot + 1) & mask;
    }

    groups[slot].key = key;
    return &groups[slot];
}

bool es_native_has_choke(const EsNativeSamples *samples) {
    for (size_t i = 0; samples && i < samples->count; i++) {
        if (samples->regions[i].params.cut_group) {
            return true;
        }
    }

    return false;
}

bool es_native_choke(const EsNativeSamples *samples, const EsNativeChokeNote *notes, size_t count,
                     const DmeHost *host) {
    if (!count || !es_native_has_choke(samples)) {
        return true;
    }

    size_t slots = 32;

    /* Each region contributes at most a Cut and Cut by key on each of 16 source channels. */
    while (slots < samples->count * 64) {
        slots *= 2;
    }

    ChokeGroup *groups = calloc(slots, sizeof(*groups));
    size_t *next = calloc(count, sizeof(*next));
    bool ok = groups && next;

    for (size_t i = 0; ok && i < count; i++) {
        if (!(i % 1024) && host && host->cancelled && host->cancelled(host->user)) {
            ok = false;
            break;
        }

        const EsNativeChokeNote *note = &notes[i];
        const EsNativeRegion *region = &samples->regions[note->region];
        const DmeSampleZoneParams *params = &region->params;

        if (params->cut_group) {
            ChokeGroup *cut =
                group(groups, slots - 1, region->preset_scope, note->channel, params->cut_group);
            /* Simultaneous attacks cannot choke one another; scan prior voices once per tick. */
            size_t *link = !cut->cut || cut->cut_tick != note->start ? &cut->head : NULL;
            cut->cut_tick = note->start;
            cut->cut = true;

            while (link && *link) {
                size_t index = *link - 1;
                const EsNativeChokeNote *earlier = &notes[index];

                if (!(index % 1024) && host && host->cancelled && host->cancelled(host->user)) {
                    ok = false;
                    break;
                }

                if (earlier->start < note->start && *earlier->end > note->start) {
                    *earlier->end = note->start;
                }

                if (*earlier->end <= note->start) {
                    *link = next[index];
                } else {
                    link = &next[index];
                }
            }
        }

        if (!ok) {
            break;
        }

        if (params->cut_by && *note->end > note->start) {
            ChokeGroup *held =
                group(groups, slots - 1, region->preset_scope, note->channel, params->cut_by);
            next[i] = held->head;
            held->head = i + 1;
        }
    }

    free(next);
    free(groups);
    return ok;
}
