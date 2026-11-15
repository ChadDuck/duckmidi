#include "devtools/devtools.h"

#ifdef DM_DEVTOOLS
#include <math.h>

static const struct {
    const char *name;
} sections[DM_PROFILE_COUNT] = {
    [DM_PROFILE_FRAME] = {"frame"},
    [DM_PROFILE_INPUT] = {"input"},
    [DM_PROFILE_UI_BUILD] = {"ui_build"},
    [DM_PROFILE_GESTURES] = {"gestures"},
    [DM_PROFILE_PLAYLIST_LAYOUT] = {"playlist_layout"},
    [DM_PROFILE_PLAYLIST_ROWS] = {"playlist_rows"},
    [DM_PROFILE_DRAW] = {"draw"},
    [DM_PROFILE_CHECKPOINT] = {"checkpoint"},
    [DM_PROFILE_HISTORY_PACK] = {"history_pack"},
    [DM_PROFILE_CHANGED] = {"changed"},
    [DM_PROFILE_VALIDATE] = {"validate"},
    [DM_PROFILE_PUBLISH] = {"publish"},
    [DM_PROFILE_SCHEDULE] = {"schedule"},
    [DM_PROFILE_PUBLISH_SWAP] = {"publish_swap"},
};

enum { PROFILE_FRAMES = 1024 };

typedef struct {
    uint64_t timestamp;
    double ms[DM_PROFILE_COUNT];
    int calls[DM_PROFILE_COUNT];
} ProfileFrame;

static ProfileFrame frames[PROFILE_FRAMES], pending;
static unsigned next_frame, frame_count;
static unsigned depth[DM_PROFILE_COUNT];

uint64_t dm_profile_begin(int section) {
    ++depth[section];
    return SDL_GetPerformanceCounter();
}

void dm_profile_end(int section, uint64_t start) {
    SDL_assert(depth[section] > 0);

    if (--depth[section]) {
        return;
    }

    uint64_t now = SDL_GetPerformanceCounter();
    pending.timestamp = now;
    pending.ms[section] += (double)(now - start) * 1000 / SDL_GetPerformanceFrequency();
    ++pending.calls[section];
}

void dm_profile_frame_end(void) {
    pending.timestamp = SDL_GetPerformanceCounter();
    frames[next_frame] = pending;
    next_frame = (next_frame + 1) % PROFILE_FRAMES;

    if (frame_count < PROFILE_FRAMES) {
        ++frame_count;
    }

    pending = (ProfileFrame){0};
}

const char *dm_profile_name(int section) {
    return section >= 0 && section < DM_PROFILE_COUNT ? sections[section].name : NULL;
}

static bool in_window(const ProfileFrame *frame, uint64_t now, double ticks) {
    return frame->timestamp && (double)(now - frame->timestamp) <= ticks;
}

int dm_profile_frames(double window_seconds) {
    uint64_t now = SDL_GetPerformanceCounter();
    double ticks = window_seconds * SDL_GetPerformanceFrequency();
    int count = in_window(&pending, now, ticks);

    for (unsigned i = 0; i < frame_count; ++i) {
        count += in_window(&frames[i], now, ticks);
    }

    return count;
}

bool dm_profile_stats(int section, double window_seconds, double *average_ms, double *worst_ms,
                      int *calls) {
    *average_ms = *worst_ms = 0;
    *calls = 0;

    if (section < 0 || section >= DM_PROFILE_COUNT || !isfinite(window_seconds) ||
        window_seconds <= 0) {
        return false;
    }

    uint64_t now = SDL_GetPerformanceCounter();
    double ticks = window_seconds * SDL_GetPerformanceFrequency();
    int count = 0;

    for (unsigned i = 0; i <= frame_count; ++i) {
        const ProfileFrame *frame = i == frame_count ? &pending : &frames[i];

        if (in_window(frame, now, ticks)) {
            *average_ms += frame->ms[section];
            *worst_ms = fmax(*worst_ms, frame->ms[section]);
            *calls += frame->calls[section];
            ++count;
        }
    }

    if (count) {
        *average_ms /= count;
    }

    return count != 0;
}
#endif
