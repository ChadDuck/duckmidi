#include "devtools/devtools.h"

#ifdef DM_DEVTOOLS
#include "core/duck.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char settings_dir[DM_PATH];
static DmInputLog active_log;
static bool active_recording;
/* Keys pressed since recording started; a release of any other key (such as the F10 that started
   the recording) is not recorded, so replay never sends a release without its press. */
static bool keys_down[SDL_SCANCODE_COUNT];

/* About a day at 60 frames per second: replay waits frame by frame for each event. */
enum { FRAME_MAX = 60 * 60 * 60 * 24 };

void dm_input_log_set_folder(const char *settings_folder) {
    if (!settings_folder) {
        settings_dir[0] = 0;
        return;
    }

    snprintf(settings_dir, sizeof(settings_dir), "%s", settings_folder);
}

bool dm_input_log_recording(void) {
    return active_recording;
}

void dm_input_log_free(DmInputLog *log) {
    if (!log) {
        return;
    }

    free(log->events);
    memset(log, 0, sizeof(*log));
}

void dm_input_log_start(int width, int height) {
    dm_input_log_free(&active_log);
    memset(keys_down, 0, sizeof(keys_down));
    active_log.width = width > 0 ? width : 1400;
    active_log.height = height > 0 ? height : 900;
    active_recording = true;
}

static bool append_event(DmInputLog *log, const DmInputEvent *ev) {
    if (log->count >= DM_INPUT_LOG_MAX) {
        return false;
    }

    if (log->count == log->capacity) {
        size_t next = log->capacity ? log->capacity * 2 : 1024;

        if (next > DM_INPUT_LOG_MAX) {
            next = DM_INPUT_LOG_MAX;
        }

        DmInputEvent *grown = realloc(log->events, next * sizeof(*grown));

        if (!grown) {
            return false;
        }

        log->events = grown;
        log->capacity = next;
    }

    log->events[log->count++] = *ev;
    return true;
}

static bool build_log_path(char *path, size_t cap) {
    if (!path || !cap) {
        return false;
    }

    path[0] = 0;
    char logs[DM_PATH];
    size_t len = strlen(settings_dir);
    const char *sep =
        len && settings_dir[len - 1] != '/' && settings_dir[len - 1] != '\\' ? "/" : "";

    if (snprintf(logs, sizeof(logs), "%s%slogs", settings_dir, sep) >= (int)sizeof(logs) - 40 ||
        !SDL_CreateDirectory(logs)) {
        return false;
    }

    SDL_Time now = 0;
    SDL_DateTime dt = {0};

    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &dt, true)) {
        return snprintf(path, cap, "%s/input-recording.txt", logs) < (int)cap;
    }

    return snprintf(path, cap, "%s/input-%04d%02d%02d-%02d%02d%02d.txt", logs, dt.year, dt.month,
                    dt.day, dt.hour, dt.minute, dt.second) < (int)cap;
}

bool dm_input_log_record(const SDL_Event *event, uint64_t frame, bool *full_stopped, char *path,
                         size_t path_cap) {
    if (full_stopped) {
        *full_stopped = false;
    }

    if (!active_recording || !event) {
        return false;
    }

    DmInputEvent ev = {.frame = frame};

    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
        ev.kind = DM_INPUT_MOTION;
        ev.motion.x = event->motion.x;
        ev.motion.y = event->motion.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        ev.kind =
            event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? DM_INPUT_BUTTON_DOWN : DM_INPUT_BUTTON_UP;
        ev.button.button = event->button.button;
        ev.button.x = event->button.x;
        ev.button.y = event->button.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        ev.kind = DM_INPUT_WHEEL;
        ev.wheel.x = event->wheel.x;
        ev.wheel.y = event->wheel.y;
        ev.wheel.mouse_x = event->wheel.mouse_x;
        ev.wheel.mouse_y = event->wheel.mouse_y;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        ev.kind = event->type == SDL_EVENT_KEY_DOWN ? DM_INPUT_KEY_DOWN : DM_INPUT_KEY_UP;
        ev.key.scancode = event->key.scancode;
        ev.key.mod = event->key.mod;
        ev.key.repeat = event->key.repeat;

        if (ev.key.scancode >= 0 && ev.key.scancode < SDL_SCANCODE_COUNT) {
            if (ev.kind == DM_INPUT_KEY_UP && !keys_down[ev.key.scancode]) {
                return false;
            }

            keys_down[ev.key.scancode] = ev.kind == DM_INPUT_KEY_DOWN;
        }
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (!event->text.text) {
            return false;
        }

        ev.kind = DM_INPUT_TEXT;
        snprintf(ev.text.text, sizeof(ev.text.text), "%s", event->text.text);
        break;
    default:
        return false;
    }

    bool ok = append_event(&active_log, &ev);

    if (!ok || active_log.count >= DM_INPUT_LOG_MAX) {
        if (full_stopped) {
            *full_stopped = true;
        }

        dm_input_log_stop(path, path_cap);
        return ok;
    }

    return true;
}

static void write_escaped(FILE *file, const char *text) {
    for (const char *p = text; *p; ++p) {
        switch (*p) {
        case '\\':
            fputs("\\\\", file);
            break;
        case '\n':
            fputs("\\n", file);
            break;
        case '\r':
            fputs("\\r", file);
            break;
        case '\t':
            fputs("\\t", file);
            break;
        default:
            fputc((unsigned char)*p, file);
            break;
        }
    }
}

bool dm_input_log_save(const DmInputLog *log, const char *path) {
    if (!log || !path || !*path) {
        return false;
    }

    FILE *file = fopen(path, "w");

    if (!file) {
        return false;
    }

    fprintf(file, "duckmidi-input 1 %d %d\n", log->width, log->height);

    for (size_t i = 0; i < log->count; ++i) {
        const DmInputEvent *ev = &log->events[i];

        switch (ev->kind) {
        case DM_INPUT_MOTION:
            fprintf(file, "%" PRIu64 " motion %.6g %.6g\n", ev->frame, ev->motion.x, ev->motion.y);
            break;
        case DM_INPUT_BUTTON_DOWN:
            fprintf(file, "%" PRIu64 " button_down %u %.6g %.6g\n", ev->frame,
                    (unsigned)ev->button.button, ev->button.x, ev->button.y);
            break;
        case DM_INPUT_BUTTON_UP:
            fprintf(file, "%" PRIu64 " button_up %u %.6g %.6g\n", ev->frame,
                    (unsigned)ev->button.button, ev->button.x, ev->button.y);
            break;
        case DM_INPUT_WHEEL:
            fprintf(file, "%" PRIu64 " wheel %.6g %.6g %.6g %.6g\n", ev->frame, ev->wheel.x,
                    ev->wheel.y, ev->wheel.mouse_x, ev->wheel.mouse_y);
            break;
        case DM_INPUT_KEY_DOWN:
            fprintf(file, "%" PRIu64 " key_down %d %u %d\n", ev->frame, (int)ev->key.scancode,
                    (unsigned)ev->key.mod, ev->key.repeat ? 1 : 0);
            break;
        case DM_INPUT_KEY_UP:
            fprintf(file, "%" PRIu64 " key_up %d %u %d\n", ev->frame, (int)ev->key.scancode,
                    (unsigned)ev->key.mod, ev->key.repeat ? 1 : 0);
            break;
        case DM_INPUT_TEXT:
            fprintf(file, "%" PRIu64 " text ", ev->frame);
            write_escaped(file, ev->text.text);
            fputc('\n', file);
            break;
        }
    }

    bool ok = ferror(file) == 0;
    fclose(file);
    return ok;
}

bool dm_input_log_stop_to(const char *path) {
    active_recording = false;
    bool ok = dm_input_log_save(&active_log, path);
    dm_input_log_free(&active_log);
    return ok;
}

bool dm_input_log_stop(char *path, size_t path_cap) {
    active_recording = false;
    char generated[DM_PATH];
    char *target = path && path_cap ? path : generated;
    size_t cap = path && path_cap ? path_cap : sizeof(generated);
    bool ok = build_log_path(target, cap) && dm_input_log_save(&active_log, target);
    dm_input_log_free(&active_log);
    return ok;
}

static void unescape_text(const char *src, char *dst, size_t cap) {
    size_t out = 0;

    for (size_t i = 0; src[i] && out + 1 < cap; ++i) {
        if (src[i] == '\\' && src[i + 1]) {
            ++i;

            switch (src[i]) {
            case 'n':
                dst[out++] = '\n';
                break;
            case 'r':
                dst[out++] = '\r';
                break;
            case 't':
                dst[out++] = '\t';
                break;
            case '\\':
                dst[out++] = '\\';
                break;
            default:
                dst[out++] = src[i];
                break;
            }
        } else {
            dst[out++] = src[i];
        }
    }

    dst[out] = 0;
}

bool dm_input_log_load(DmInputLog *log, const char *path) {
    if (!log || !path || !*path) {
        return false;
    }

    memset(log, 0, sizeof(*log));
    FILE *file = fopen(path, "r");

    if (!file) {
        return false;
    }

    char line[512];

    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return false;
    }

    int version = 0, width = 0, height = 0;

    if (sscanf(line, "duckmidi-input %d %d %d", &version, &width, &height) != 3 || version != 1 ||
        width <= 0 || height <= 0) {
        fclose(file);
        return false;
    }

    log->width = width;
    log->height = height;

    while (fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);

        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = 0;
        }

        if (!len) {
            continue;
        }

        DmInputEvent ev = {0};
        char kind[32] = {0};
        int offset = 0;

        if (sscanf(line, "%" SCNu64 " %31s%n", &ev.frame, kind, &offset) != 2) {
            dm_input_log_free(log);
            fclose(file);
            return false;
        }

        /* Replay waits for each frame in turn, so frames must not go backwards or lie absurdly
           far ahead. */
        if (ev.frame > FRAME_MAX || (log->count && ev.frame < log->events[log->count - 1].frame)) {
            dm_input_log_free(log);
            fclose(file);
            return false;
        }

        const char *rest = line + offset + (line[offset] == ' ' ? 1 : 0);

        if (!strcmp(kind, "motion")) {
            if (sscanf(rest, "%f %f", &ev.motion.x, &ev.motion.y) != 2) {
                dm_input_log_free(log);
                fclose(file);
                return false;
            }

            ev.kind = DM_INPUT_MOTION;
        } else if (!strcmp(kind, "button_down") || !strcmp(kind, "button_up")) {
            unsigned button = 0;

            if (sscanf(rest, "%u %f %f", &button, &ev.button.x, &ev.button.y) != 3) {
                dm_input_log_free(log);
                fclose(file);
                return false;
            }

            ev.kind = !strcmp(kind, "button_down") ? DM_INPUT_BUTTON_DOWN : DM_INPUT_BUTTON_UP;
            ev.button.button = (uint8_t)button;
        } else if (!strcmp(kind, "wheel")) {
            if (sscanf(rest, "%f %f %f %f", &ev.wheel.x, &ev.wheel.y, &ev.wheel.mouse_x,
                       &ev.wheel.mouse_y) != 4) {
                dm_input_log_free(log);
                fclose(file);
                return false;
            }

            ev.kind = DM_INPUT_WHEEL;
        } else if (!strcmp(kind, "key_down") || !strcmp(kind, "key_up")) {
            int scancode = 0, repeat = 0;
            unsigned mod = 0;

            if (sscanf(rest, "%d %u %d", &scancode, &mod, &repeat) != 3) {
                dm_input_log_free(log);
                fclose(file);
                return false;
            }

            ev.kind = !strcmp(kind, "key_down") ? DM_INPUT_KEY_DOWN : DM_INPUT_KEY_UP;
            ev.key.scancode = (SDL_Scancode)scancode;
            ev.key.mod = (SDL_Keymod)mod;
            ev.key.repeat = repeat != 0;
        } else if (!strcmp(kind, "text")) {
            ev.kind = DM_INPUT_TEXT;
            unescape_text(rest, ev.text.text, sizeof(ev.text.text));
        } else {
            dm_input_log_free(log);
            fclose(file);
            return false;
        }

        if (!append_event(log, &ev)) {
            dm_input_log_free(log);
            fclose(file);
            return false;
        }
    }

    fclose(file);
    return true;
}

bool dm_input_log_to_sdl(const DmInputEvent *ev, SDL_WindowID window_id, SDL_Event *out) {
    if (!ev || !out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    switch (ev->kind) {
    case DM_INPUT_MOTION:
        out->type = SDL_EVENT_MOUSE_MOTION;
        out->motion.windowID = window_id;
        out->motion.x = ev->motion.x;
        out->motion.y = ev->motion.y;
        break;
    case DM_INPUT_BUTTON_DOWN:
    case DM_INPUT_BUTTON_UP:
        out->type = ev->kind == DM_INPUT_BUTTON_DOWN ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                                     : SDL_EVENT_MOUSE_BUTTON_UP;
        out->button.windowID = window_id;
        out->button.button = ev->button.button;
        out->button.down = ev->kind == DM_INPUT_BUTTON_DOWN;
        out->button.clicks = 1;
        out->button.x = ev->button.x;
        out->button.y = ev->button.y;
        break;
    case DM_INPUT_WHEEL:
        out->type = SDL_EVENT_MOUSE_WHEEL;
        out->wheel.windowID = window_id;
        out->wheel.x = ev->wheel.x;
        out->wheel.y = ev->wheel.y;
        out->wheel.mouse_x = ev->wheel.mouse_x;
        out->wheel.mouse_y = ev->wheel.mouse_y;
        out->wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        break;
    case DM_INPUT_KEY_DOWN:
    case DM_INPUT_KEY_UP:
        out->type = ev->kind == DM_INPUT_KEY_DOWN ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        out->key.windowID = window_id;
        out->key.scancode = ev->key.scancode;
        out->key.mod = ev->key.mod;
        out->key.repeat = ev->key.repeat;
        out->key.down = ev->kind == DM_INPUT_KEY_DOWN;
        out->key.key = SDL_GetKeyFromScancode(ev->key.scancode, ev->key.mod, false);
        break;
    case DM_INPUT_TEXT:
        out->type = SDL_EVENT_TEXT_INPUT;
        out->text.windowID = window_id;
        out->text.text = ev->text.text;
        break;
    default:
        return false;
    }

    return true;
}
#endif