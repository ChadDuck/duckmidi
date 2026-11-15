#ifndef DM_DEVTOOLS_H
#define DM_DEVTOOLS_H

#ifdef DM_DEVTOOLS
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DM_PROFILE_FRAME,
    DM_PROFILE_INPUT,
    DM_PROFILE_UI_BUILD,
    DM_PROFILE_GESTURES,
    DM_PROFILE_PLAYLIST_LAYOUT,
    DM_PROFILE_PLAYLIST_ROWS,
    DM_PROFILE_DRAW,
    DM_PROFILE_CHECKPOINT,
    DM_PROFILE_HISTORY_PACK,
    DM_PROFILE_CHANGED,
    DM_PROFILE_VALIDATE,
    DM_PROFILE_PUBLISH,
    DM_PROFILE_SCHEDULE,
    DM_PROFILE_PUBLISH_SWAP,
    DM_PROFILE_COUNT
} DmProfileSection;

/* GUI thread only, including schedule/publish. Sections are inclusive: gestures nest inside
   ui_build, and edits and publishing can nest inside gestures. Do not add the rows together.
   Same-section nesting records only the outermost interval and counts it as one call. */
uint64_t dm_profile_begin(int section);
void dm_profile_end(int section, uint64_t start);
void dm_profile_frame_end(void);
const char *dm_profile_name(int section);
/* Includes the pending frame when it has samples (also useful to direct editor_frame callers).
   Calls is the total over the window; timings are average/worst per frame, including zeros. */
bool dm_profile_stats(int section, double window_seconds, double *average_ms, double *worst_ms,
                      int *calls);
int dm_profile_frames(double window_seconds);

#define DM_PROFILE_BEGIN(section, name) uint64_t name = dm_profile_begin(DM_PROFILE_##section)
#define DM_PROFILE_END(section, name) dm_profile_end(DM_PROFILE_##section, name)

typedef enum { DM_GESTURE_HELD, DM_GESTURE_PRESSED } DmGestureHelper;

typedef struct {
    int gesture;
    int helper; /* DmGestureHelper */
    bool result;
    int count;
    char reason[64];
} DmGestureRecord;

void dm_gesture_inspector_begin(void);
void dm_gesture_inspector_record(int gesture, int helper, bool result, unsigned req_mods,
                                 unsigned held_mods, bool pressed);
int dm_gesture_inspector_count(void);
bool dm_gesture_inspector_entry(int index, DmGestureRecord *out);
bool dm_gesture_inspector_queried(int gesture);
bool dm_gesture_inspector_won(int gesture);
const char *dm_gesture_inspector_lost_reason(int gesture);

enum { DM_INPUT_LOG_MAX = 200000, DM_INPUT_TEXT_MAX = 128 };

typedef enum {
    DM_INPUT_MOTION,
    DM_INPUT_BUTTON_DOWN,
    DM_INPUT_BUTTON_UP,
    DM_INPUT_WHEEL,
    DM_INPUT_KEY_DOWN,
    DM_INPUT_KEY_UP,
    DM_INPUT_TEXT
} DmInputKind;

typedef struct {
    uint64_t frame;
    DmInputKind kind;

    union {
        struct {
            float x, y;
        } motion;

        struct {
            uint8_t button;
            float x, y;
        } button;

        struct {
            float x, y, mouse_x, mouse_y;
        } wheel;

        struct {
            SDL_Scancode scancode;
            SDL_Keymod mod;
            bool repeat;
        } key;

        struct {
            char text[DM_INPUT_TEXT_MAX];
        } text;
    };
} DmInputEvent;

typedef struct {
    int width, height;
    DmInputEvent *events;
    size_t count, capacity;
} DmInputLog;

void dm_input_log_set_folder(const char *settings_folder);
bool dm_input_log_recording(void);
void dm_input_log_start(int width, int height);
bool dm_input_log_record(const SDL_Event *event, uint64_t frame, bool *full_stopped, char *path,
                         size_t path_cap);
bool dm_input_log_stop(char *path, size_t path_cap);
bool dm_input_log_stop_to(const char *path);
bool dm_input_log_save(const DmInputLog *log, const char *path);
bool dm_input_log_load(DmInputLog *log, const char *path);
void dm_input_log_free(DmInputLog *log);
bool dm_input_log_to_sdl(const DmInputEvent *ev, SDL_WindowID window_id, SDL_Event *out);

#ifdef __cplusplus
}
#endif
#else
#define DM_PROFILE_BEGIN(section, name)
#define DM_PROFILE_END(section, name)
#define dm_profile_frame_end()
#define dm_gesture_inspector_begin()
#define dm_gesture_inspector_record(gesture, helper, result, req_mods, held_mods, pressed)
#define dm_input_log_set_folder(settings_folder)
#define dm_input_log_recording() false
#define dm_input_log_record(event, frame, full_stopped, path, path_cap) false
#define dm_input_log_free(log)
#endif

#endif
