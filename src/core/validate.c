/* dm_project_validate: the rules that tie the project's arrays together (agents' model contract,
   "Invariants"), which the loader enforces on every document. dm_project_check_edit adds the
   rules the editor keeps but older files may break harmlessly (an unpaired restoration point,
   which playback ignores); DEV builds and the desktop tests run it after every edit. Rules about
   a single field stay with the code that reads or adds it. */
#include "core/duck.h"
#include "text/en.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static bool invalid(char *error, size_t cap, const char *format, size_t index) {
    if (error && cap) {
        snprintf(error, cap, format, index);
    }

    return false;
}

static int uid_compare(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static bool pattern_uids_valid(const DmProject *p, char *error, size_t cap) {
    if (p->pattern_count < 2) {
        return true;
    }

    if (p->pattern_count > SIZE_MAX / sizeof(uint32_t)) {
        return invalid(error, cap, DM_TEXT_PROJECT_VALIDATE_ALLOCATION, 0);
    }

    uint32_t *ids = malloc(p->pattern_count * sizeof(uint32_t));

    if (!ids) {
        return invalid(error, cap, DM_TEXT_PROJECT_VALIDATE_ALLOCATION, 0);
    }

    for (size_t i = 0; i < p->pattern_count; ++i) {
        ids[i] = p->patterns[i].uid;
    }

    qsort(ids, p->pattern_count, sizeof(uint32_t), uid_compare);

    for (size_t i = 1; i < p->pattern_count; ++i) {
        if (ids[i - 1] == ids[i]) {
            uint32_t duplicate = ids[i];
            size_t index = 0;

            while (p->patterns[index].uid != duplicate) {
                ++index;
            }

            free(ids);
            return invalid(error, cap, DM_TEXT_PROJECT_DUPLICATE_UID, index);
        }
    }

    free(ids);
    return true;
}

typedef struct {
    int pattern, port, curve;
} LineKey;

static int line_compare(const void *a, const void *b) {
    const LineKey *x = a, *y = b;

    if (x->pattern != y->pattern) {
        return x->pattern < y->pattern ? -1 : 1;
    }

    if (x->port != y->port) {
        return x->port < y->port ? -1 : 1;
    }

    return x->curve < y->curve ? -1 : x->curve > y->curve;
}

static bool restorations_valid(const DmProject *p, size_t lines, char *error, size_t cap) {
    if (lines > SIZE_MAX / sizeof(LineKey)) {
        return invalid(error, cap, DM_TEXT_PROJECT_VALIDATE_ALLOCATION, 0);
    }

    LineKey *keys = lines ? malloc(lines * sizeof(LineKey)) : NULL;

    if (lines && !keys) {
        return invalid(error, cap, DM_TEXT_PROJECT_VALIDATE_ALLOCATION, 0);
    }

    size_t count = 0;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];

        if (a.curve > 0) {
            keys[count++] = (LineKey){a.pattern, a.port, a.curve};
        }
    }

    if (count) {
        qsort(keys, count, sizeof(*keys), line_compare);
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];

        if (a.curve < 0) {
            LineKey key = {a.pattern, a.port, -a.curve};

            if (!count || !bsearch(&key, keys, count, sizeof(*keys), line_compare)) {
                free(keys);
                return invalid(error, cap, DM_TEXT_PROJECT_RESTORATION, i);
            }
        }
    }

    free(keys);
    return true;
}

bool dm_project_validate(const DmProject *p, char *error, size_t cap) {
    for (size_t c = 0; c < 16; ++c) {
        if (p->note_rows[c] < 1 || p->note_rows[c] > DM_NOTE_ROWS || p->lane_count[c] < 0 ||
            p->lane_count[c] > DM_LANES) {
            return invalid(error, cap, DM_TEXT_PROJECT_INVALID_CHANNEL, c);
        }
    }

    for (size_t i = 0; i < p->pattern_count; ++i) {
        const DmPattern *pattern = &p->patterns[i];

        if (!pattern->uid || pattern->uid > p->next_pattern_uid || pattern->kind < 0 ||
            pattern->kind > 1 || (pattern->kind == 1 && pattern->count)) {
            return invalid(error, cap, DM_TEXT_PROJECT_INVALID_PATTERN, i);
        }
    }

    if (!pattern_uids_valid(p, error, cap)) {
        return false;
    }

    for (size_t i = 0; i < p->clip_count; ++i) {
        if (!dm_clip_valid(p, p->clips[i])) {
            return invalid(error, cap, DM_TEXT_PROJECT_INVALID_CLIP, i);
        }
    }

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation a = p->automation[i];

        if (a.pattern < 0 || !dm_automation_valid(p, a)) {
            return invalid(error, cap, DM_TEXT_PROJECT_INVALID_AUTOMATION, i);
        }

        if (i && dm_automation_later(p->automation[i - 1], a)) {
            return invalid(error, cap, DM_TEXT_PROJECT_AUTOMATION_ORDER, i);
        }

        if (p->patterns[a.pattern].kind == 1 && a.beat > p->patterns[a.pattern].length) {
            return invalid(error, cap, DM_TEXT_PROJECT_AUTOMATION_LENGTH, i);
        }
    }

    for (size_t i = 0; i < p->event_count; ++i) {
        if (!isfinite(p->events[i].beat) || (i && p->events[i].beat < p->events[i - 1].beat)) {
            return invalid(error, cap, DM_TEXT_PROJECT_INVALID_EVENT, i);
        }
    }

    return true;
}

bool dm_project_check_edit(const DmProject *p, char *error, size_t cap) {
    if (!dm_project_validate(p, error, cap)) {
        return false;
    }

    size_t lines = 0;
    bool restoration = false;

    for (size_t i = 0; i < p->automation_count; ++i) {
        lines += p->automation[i].curve > 0;
        restoration |= p->automation[i].curve < 0;
    }

    return !restoration || restorations_valid(p, lines, error, cap);
}
