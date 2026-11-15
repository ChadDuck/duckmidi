#include "devtools/devtools.h"

#ifdef DM_DEVTOOLS
#include "editor/editor.h"
#include <stdio.h>
#include <string.h>

static DmGestureRecord records[GESTURE_COUNT * 2];
static int record_count;

static void format_mods(unsigned mods, char *out, size_t cap) {
    out[0] = 0;
    const char *sep = "";

    if (mods & SDL_KMOD_CTRL) {
        snprintf(out, cap, "Ctrl");
        sep = "+";
    }

    if (mods & SDL_KMOD_SHIFT) {
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%sShift", sep);
        sep = "+";
    }

    if (mods & SDL_KMOD_ALT) {
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%sAlt", sep);
    }
}

void dm_gesture_inspector_begin(void) {
    record_count = 0;
}

void dm_gesture_inspector_record(int gesture, int helper, bool result, unsigned req_mods,
                                 unsigned held_mods, bool pressed) {
    for (int i = 0; i < record_count; ++i) {
        if (records[i].gesture == gesture && records[i].helper == helper &&
            records[i].result == result) {
            ++records[i].count;
            return;
        }
    }

    if (record_count >= GESTURE_COUNT * 2) {
        return;
    }

    DmGestureRecord rec = {.gesture = gesture, .helper = helper, .result = result, .count = 1};

    if (!result) {
        char mods_buf[32];

        if (helper == DM_GESTURE_HELD) {
            format_mods(req_mods & ~held_mods, mods_buf, sizeof(mods_buf));
            snprintf(rec.reason, sizeof(rec.reason), "modifiers not held: %s", mods_buf);
        } else if (!pressed) {
            snprintf(rec.reason, sizeof(rec.reason), "button not pressed");
        } else if ((held_mods & req_mods) != req_mods) {
            format_mods(req_mods & ~held_mods, mods_buf, sizeof(mods_buf));
            snprintf(rec.reason, sizeof(rec.reason), "modifiers not held: %s", mods_buf);
        } else {
            snprintf(rec.reason, sizeof(rec.reason), "extra modifiers held");
        }
    }

    records[record_count++] = rec;
}

int dm_gesture_inspector_count(void) {
    return record_count;
}

bool dm_gesture_inspector_entry(int index, DmGestureRecord *out) {
    if (index < 0 || index >= record_count || !out) {
        return false;
    }

    *out = records[index];
    return true;
}

bool dm_gesture_inspector_queried(int gesture) {
    for (int i = 0; i < record_count; ++i) {
        if (records[i].gesture == gesture) {
            return true;
        }
    }

    return false;
}

bool dm_gesture_inspector_won(int gesture) {
    for (int i = 0; i < record_count; ++i) {
        if (records[i].gesture == gesture && records[i].result) {
            return true;
        }
    }

    return false;
}

const char *dm_gesture_inspector_lost_reason(int gesture) {
    for (int i = 0; i < record_count; ++i) {
        if (records[i].gesture == gesture && !records[i].result) {
            return records[i].reason;
        }
    }

    return NULL;
}
#endif
