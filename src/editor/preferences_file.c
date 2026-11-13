#include "editor/internal.h"
#include "text/en.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Editor next;
    DmBytes midi_bindings;
    Binding keys[ACT_COUNT];
    bool seen[ACT_COUNT];
    bool percussion_seen[128], percussion_midi_seen[128];
    uint32_t version;
    int count;
} Ini;

static void setting_label(const char *name, char *out, size_t cap) {
    size_t n = 0;
    bool separator = false;

    for (const unsigned char *p = (const unsigned char *)name; *p && n + 1 < cap; p++) {
        if (isalnum(*p)) {
            if (separator && n && n + 1 < cap) {
                out[n++] = '_';
            }

            out[n++] = (char)tolower(*p);
            separator = false;
        } else {
            separator = true;
        }
    }

    out[n] = 0;
}

static void percussion_label(unsigned note, char *out, size_t cap) {
    char label[128];
    setting_label(dm_drum_name((int)note), label, sizeof(label));

    if (note < 35 || note > 81) {
        snprintf(out, cap, "unassigned_%u", note);
    } else {
        snprintf(out, cap, "%s", label);
    }
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) {
        ++s;
    }

    size_t n = strlen(s);

    while (n && isspace((unsigned char)s[n - 1])) {
        s[--n] = 0;
    }

    return s;
}

static bool number(const char *s, uint32_t *out) {
    if (!isdigit((unsigned char)*s)) {
        return false;
    }

    errno = 0;
    char *end;
    unsigned long long n = strtoull(s, &end, 10);

    if (errno || *end || n > UINT32_MAX) {
        return false;
    }

    *out = (uint32_t)n;
    return true;
}

/* Quoted INI strings: backslashes are literal; doubled quotes represent one quote. */
static bool value(char *out, size_t cap, const char *s) {
    size_t length = strlen(s), used = 0;

    if (length < 2 || s[0] != '"' || s[length - 1] != '"') {
        return false;
    }

    for (size_t i = 1; i + 1 < length; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '"') {
            if (i + 2 >= length || s[i + 1] != '"') {
                return false;
            }

            i++;
        }

        if (c < 32 || used + 1 >= cap) {
            return false;
        }

        out[used++] = (char)c;
    }

    out[used] = 0;
    return true;
}

bool dm_binding_parse(const char *text, Binding *binding) {
    if (!text || strlen(text) >= 160) {
        return false;
    }

    char copy[160];
    snprintf(copy, sizeof(copy), "%s", text);
    char *part = copy;
    SDL_Keymod mods = 0;

    for (;;) {
        char *plus = strchr(part, '+');

        if (plus) {
            *plus = 0;
        }

        char *token = trim(part);

        if (!*token) {
            return false;
        }

        if (!plus) {
            uint8_t mouse = !SDL_strcasecmp(token, DM_TEXT_MOUSE_BUTTON_4)   ? SDL_BUTTON_X1
                            : !SDL_strcasecmp(token, DM_TEXT_MOUSE_BUTTON_5) ? SDL_BUTTON_X2
                                                                             : 0;

            if (mouse) {
                *binding = (Binding){.mods = mods, .mouse = mouse};
                return true;
            }

            SDL_Scancode key = !SDL_strcasecmp(token, "Keypad Add")
                                   ? SDL_SCANCODE_KP_PLUS
                                   : SDL_GetScancodeFromName(token);

            if (key <= 0 || key >= SDL_SCANCODE_COUNT || key == SDL_SCANCODE_LCTRL ||
                key == SDL_SCANCODE_RCTRL || key == SDL_SCANCODE_LSHIFT ||
                key == SDL_SCANCODE_RSHIFT || key == SDL_SCANCODE_LALT ||
                key == SDL_SCANCODE_RALT || key == SDL_SCANCODE_LGUI || key == SDL_SCANCODE_RGUI ||
                ed_reserved(key, mods)) {
                return false;
            }

            *binding = (Binding){.key = key, .mods = mods};
            return true;
        }

        SDL_Keymod flag = !SDL_strcasecmp(token, "Ctrl")    ? SDL_KMOD_CTRL
                          : !SDL_strcasecmp(token, "Shift") ? SDL_KMOD_SHIFT
                          : !SDL_strcasecmp(token, "Alt")   ? SDL_KMOD_ALT
                          : !SDL_strcasecmp(token, "Win")   ? SDL_KMOD_GUI
                                                            : 0;

        if (!flag || (mods & flag)) {
            return false;
        }

        mods |= flag;
        part = plus + 1;
    }
}

bool dm_binding_format(Binding binding, char *out, size_t cap) {
    const char *name = binding.mouse == SDL_BUTTON_X1        ? DM_TEXT_MOUSE_BUTTON_4
                       : binding.mouse == SDL_BUTTON_X2      ? DM_TEXT_MOUSE_BUTTON_5
                       : binding.key == SDL_SCANCODE_KP_PLUS ? "Keypad Add"
                                                             : SDL_GetScancodeName(binding.key);

    if (!name || !*name || strchr(name, '+')) {
        return false;
    }

    int n = snprintf(out, cap, "%s%s%s%s%s", binding.mods & SDL_KMOD_CTRL ? "Ctrl + " : "",
                     binding.mods & SDL_KMOD_SHIFT ? "Shift + " : "",
                     binding.mods & SDL_KMOD_ALT ? "Alt + " : "",
                     binding.mods & SDL_KMOD_GUI ? "Win + " : "", name);
    return n >= 0 && (size_t)n < cap;
}

static bool valid_bindings(Binding *b) {
    unsigned allowed = SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI;

    for (int i = 0; i < ACT_COUNT; ++i) {
        bool input = b[i].mouse ? ed_side_button(b[i].mouse) && b[i].key == SDL_SCANCODE_UNKNOWN
                                : b[i].key > 0 && b[i].key < SDL_SCANCODE_COUNT;

        if (!input || (b[i].mods & ~allowed)) {
            return false;
        }

        for (int k = 0; k < i; ++k) {
            if (ed_same_binding(b[i], b[k])) {
                return false;
            }
        }
    }

    return true;
}

/* Comma-separated quoted names; doubled quotes use the same escaping as other INI strings. */
static bool input_names(Editor *e, const char *s) {
    memset(e->input_names, 0, sizeof(e->input_names));
    unsigned count = 0;

    while (*s) {
        while (isspace((unsigned char)*s)) {
            ++s;
        }

        if (!*s) {
            return count == 0;
        }

        if (*s != '"' || count == DM_MIDI_INPUTS) {
            return false;
        }

        const char *start = s++;

        while (*s) {
            if (*s++ == '"') {
                if (*s != '"') {
                    break;
                }

                ++s;
            }
        }

        size_t length = (size_t)(s - start);
        char quoted[DM_MIDI_NAME * 2 + 2];

        if (length >= sizeof(quoted)) {
            return false;
        }

        memcpy(quoted, start, length);
        quoted[length] = 0;

        if (!value(e->input_names[count], DM_MIDI_NAME, quoted) || !*e->input_names[count]) {
            return false;
        }

        for (unsigned i = 0; i < count; ++i) {
            if (!strcmp(e->input_names[i], e->input_names[count])) {
                return false;
            }
        }

        ++count;

        while (isspace((unsigned char)*s)) {
            ++s;
        }

        if (!*s) {
            return true;
        }

        if (*s++ != ',' || !*s) {
            return false;
        }
    }

    return true;
}

static bool application(Ini *ini, const char *key, const char *s) {
    Editor *e = &ini->next;

    if (!strcmp(key, "schema")) {
        return number(s, &ini->version);
    }

    if (!strcmp(key, "ui_size")) {
        uint32_t size;

        if (!number(s, &size) || size > 2) {
            return false;
        }

        e->ui_size = (int)size;
        return true;
    }

    if (!strcmp(key, "accent") || !strcmp(key, "background")) {
        uint32_t color;

        if (!number(s, &color)) {
            return false;
        }

        uint32_t *setting = !strcmp(key, "accent") ? &e->accent : &e->background;
        *setting = color | 0xff000000u;
        return true;
    }

    if (!strcmp(key, "monitor_soundfont")) {
        uint32_t on;

        if (!number(s, &on) || on > 1) {
            return false;
        }

        e->monitor = on != 0;
        return true;
    }

    if (!strcmp(key, "update_check") || !strcmp(key, "update_prompted")) {
        uint32_t on;

        if (!number(s, &on) || on > 1) {
            return false;
        }

        if (key[7] == 'c') {
            e->update_mode = (int)on;
        } else {
            e->update_prompted = on != 0;
        }

        return true;
    }

    if (!strcmp(key, "sample_hash")) {
        char *end;
        errno = 0;
        unsigned long long hash = strtoull(s, &end, 10);

        if (!isdigit((unsigned char)*s) || errno || *end) {
            return false;
        }

        e->sample_hash = hash;
        return true;
    }

    if (!strcmp(key, "repository_history")) {
        uint32_t on;

        if (!number(s, &on) || on > 1) {
            return false;
        }

        e->repository_history = on != 0;
        return true;
    }

    if (!strcmp(key, "modifier_select")) {
        uint32_t on;

        if (!number(s, &on) || on > 1) {
            return false;
        }

        e->modifier_select = on != 0;
        return true;
    }

    if (!strcmp(key, "os_cursor")) {
        uint32_t on;

        if (!number(s, &on) || on > 1) {
            return false;
        }

        e->os_cursor = on != 0;
        return true;
    }

    if (!strcmp(key, "soundfont")) {
        return value(e->preferred_font, DM_PATH, s);
    }

    if (!strcmp(key, "midi_folder")) {
        return value(e->midi_folder, DM_PATH, s);
    }

    if (!strcmp(key, "soundfont_folder")) {
        return value(e->font_folder, DM_PATH, s);
    }

    if (!strcmp(key, "export_folder")) {
        return value(e->export_folder, DM_PATH, s);
    }

    if (!strcmp(key, "midi_output")) {
        return value(e->output_name, sizeof(e->output_name), s);
    }

    if (!strcmp(key, "midi_inputs")) {
        return input_names(e, s);
    }

    return true;
}

static const char *const midi_velocity_names[] = {
    [DM_MIDI_VELOCITY_LIGHT] = "light",
    [DM_MIDI_VELOCITY_MEDIUM] = "medium",
    [DM_MIDI_VELOCITY_HEAVY] = "heavy",
};

static const char *const midi_controller_names[] = {
    [DM_INPUT_CONTROLLERS_KEYBOARD] = "keyboard",
    [DM_INPUT_CONTROLLERS_SONG] = "song",
};

static bool midi_input_preferences(Editor *e, const char *key, const char *s) {
    if (!strcmp(key, "velocity")) {
        for (unsigned i = 0; i < SDL_arraysize(midi_velocity_names); ++i) {
            if (!strcmp(s, midi_velocity_names[i])) {
                e->input_options.velocity = i;
                return true;
            }
        }

        return false;
    }

    if (!strcmp(key, "controllers")) {
        for (unsigned i = 0; i < SDL_arraysize(midi_controller_names); ++i) {
            if (!strcmp(s, midi_controller_names[i])) {
                e->input_controllers = i;
                return true;
            }
        }

        return false;
    }

    if (!strcmp(key, "latency")) {
        if (!strcmp(s, "auto")) {
            e->input_latency_ms = NAN;
            return true;
        }

        char *end;
        errno = 0;
        double ms = strtod(s, &end);

        if (end == s || *end || errno || !isfinite(ms) || fabs(ms) > DM_MIDI_LATENCY_LIMIT_MS) {
            return false;
        }

        e->input_latency_ms = (float)ms;
        return true;
    }

    uint32_t n;

    if (!strcmp(key, "fixed_velocity")) {
        if (!number(s, &n) || n > MIDI_DATA_MAX) {
            return false;
        }

        e->input_options.fixed_velocity = (uint8_t)n;
    } else if (!strcmp(key, "channel")) {
        if (!number(s, &n) || n > MIDI_CHANNELS) {
            return false;
        }

        e->input_options.channel = (uint8_t)n;
    } else {
        bool *value = NULL;

        if (!strcmp(key, "thru")) {
            value = &e->input_thru;
        } else if (!strcmp(key, "sustain_invert")) {
            value = &e->input_options.sustain_invert;
        } else if (!strcmp(key, "pickup")) {
            value = &e->input_pickup;
        }

        if (value) {
            if (!number(s, &n) || n > 1) {
                return false;
            }

            *value = n != 0;
        }
    }

    return true;
}

/* Saved names of binding source kinds, indexed by DmMidiInputSource. */
static const char *const binding_source_names[] = {
    [DM_MIDI_SOURCE_CC] = "cc",
    [DM_MIDI_SOURCE_NOTE] = "note",
    [DM_MIDI_SOURCE_BEND] = "bend",
    [DM_MIDI_SOURCE_PRESSURE] = "pressure",
};

static bool binding_source_kind(const char *name, DmMidiInputSource *kind) {
    for (unsigned i = 0; i < SDL_arraysize(binding_source_names); ++i) {
        if (!strcmp(name, binding_source_names[i])) {
            *kind = (DmMidiInputSource)i;
            return true;
        }
    }

    return false;
}

static bool midi_binding(Ini *ini, const char *key, const char *s) {
    EdMidiBinding binding = {0};

    if (!ed_midi_target_parse(key, &binding.target) || *s != '"') {
        return false;
    }

    const char *end = s + 1;

    while (*end) {
        if (*end == '"') {
            if (end[1] != '"') {
                break;
            }

            ++end;
        }

        ++end;
    }

    char quoted[2 * DM_MIDI_NAME + 2], tail[96];
    size_t length = (size_t)(end - s + 1);

    if (*end != '"' || end[1] != ',' || length >= sizeof(quoted) ||
        strlen(end + 2) >= sizeof(tail)) {
        return false;
    }

    memcpy(quoted, s, length);
    quoted[length] = 0;

    if (!value(binding.source.device, sizeof(binding.source.device), quoted)) {
        return false;
    }

    snprintf(tail, sizeof(tail), "%s", end + 2);
    char *fields[4] = {tail};

    for (unsigned i = 1; i < SDL_arraysize(fields); ++i) {
        char *comma = strchr(fields[i - 1], ',');

        if (!comma) {
            return false;
        }

        *comma = 0;
        fields[i] = comma + 1;
    }

    uint32_t channel, number_value;

    if (!number(trim(fields[0]), &channel) || channel < 1 || channel > MIDI_CHANNELS ||
        !number(trim(fields[2]), &number_value)) {
        return false;
    }

    bool continuous = ed_midi_target_continuous(binding.target);
    const char *kind = trim(fields[1]), *mode = trim(fields[3]);

    if (!binding_source_kind(kind, &binding.source.kind) ||
        (binding.source.kind == DM_MIDI_SOURCE_NOTE && continuous) ||
        ((binding.source.kind == DM_MIDI_SOURCE_BEND ||
          binding.source.kind == DM_MIDI_SOURCE_PRESSURE) &&
         !ed_midi_target_event(binding.target))) {
        return false;
    }

    if (!continuous && !strcmp(mode, "toggle")) {
        binding.toggle = true;
    } else if (strcmp(mode, "momentary")) {
        return false;
    }

    if (!dm_midi_source_bindable(binding.source.kind, number_value)) {
        return false;
    }

    binding.source.channel = (uint8_t)(channel - 1);
    binding.source.number = (uint8_t)number_value;
    return ed_midi_binding_append(&ini->midi_bindings, binding);
}

static bool shortcut(Ini *ini, const char *key, char *s) {
    /* Files written before 0.12.6 carry a shortcut-list number here; it has no meaning. */
    if (!strcmp(key, "schema")) {
        uint32_t ignored;
        return number(s, &ignored);
    }

    int index = -1;

    for (int i = 0; i < ACT_COUNT; i++) {
        char label[160];
        setting_label(ed_actions[i].name, label, sizeof(label));

        if (!strcmp(key, label)) {
            index = i;
            break;
        }
    }

    if (index < 0 || ini->seen[index]) {
        return false;
    }

    char text[160];

    if (!value(text, sizeof(text), s) || !dm_binding_parse(text, &ini->keys[index])) {
        return false;
    }

    ini->seen[index] = true;
    ++ini->count;
    return true;
}

/* The drum note a [percussion] or [percussion_midi] key names, once per section. */
static unsigned percussion_note(const char *key, bool seen[128]) {
    for (unsigned n = 0; n < 128; n++) {
        char label[160];
        percussion_label(n, label, sizeof(label));

        if (!strcmp(key, label)) {
            if (seen[n]) {
                return 128;
            }

            seen[n] = true;
            return n;
        }
    }

    return 128;
}

static bool percussion(Ini *ini, const char *key, const char *s) {
    unsigned note = percussion_note(key, ini->percussion_seen);

    if (note >= 128) {
        return false;
    }

    char name[80];

    if (!value(name, sizeof(name), s)) {
        return false;
    }

    int index = *name ? ed_piano_index(SDL_GetScancodeFromName(name)) : -1;

    if (*name && index < 0) {
        return false;
    }

    ini->next.percussion_global[note] = (uint8_t)(index + 1);
    return true;
}

/* An empty value leaves the drum unassigned; otherwise the incoming MIDI note number. */
static bool percussion_midi(Ini *ini, const char *key, const char *s) {
    unsigned note = percussion_note(key, ini->percussion_midi_seen);
    uint32_t incoming;

    if (note >= 128) {
        return false;
    }

    if (!*s) {
        return true;
    }

    if (!number(s, &incoming) || incoming > MIDI_DATA_MAX) {
        return false;
    }

    ini->next.percussion_midi_global[note] = (uint8_t)(incoming + 1);
    return true;
}

static bool recent(Ini *ini, const char *key, const char *s) {
    uint32_t index;

    if (strncmp(key, "file_", 5)) {
        return true;
    }

    if (!number(key + 5, &index) || index >= 10) {
        return false;
    }

    return value(ini->next.recent[index], DM_PATH, s);
}

/* Mouse gestures the file does not list keep their defaults; unknown names are ignored. */
static bool mouse_gesture(Ini *ini, const char *key, const char *s) {
    for (int i = 0; i < GESTURE_REBINDABLE; ++i) {
        if (!strcmp(key, ed_gestures[i].name)) {
            char text[80];
            return value(text, sizeof(text), s) && ed_gesture_parse(text, &ini->next.gestures[i]);
        }
    }

    return true;
}

static bool line(Ini *ini, char *s, char *section, size_t cap) {
    s = trim(s);

    if (!*s || *s == ';' || *s == '#') {
        return true;
    }

    if (*s == '[') {
        char *end = strchr(s, ']');

        if (!end || end[1] || (size_t)(end - s) >= cap) {
            return false;
        }

        *end = 0;
        snprintf(section, cap, "%s", s + 1);
        return true;
    }

    char *equals = strchr(s, '=');

    if (!equals || !*section) {
        return false;
    }

    *equals++ = 0;
    char *key = trim(s), *entry = trim(equals);

    if (!*key) {
        return false;
    }

    if (!strcmp(section, "application")) {
        return application(ini, key, entry);
    }

    if (!strcmp(section, "midi_input")) {
        return midi_input_preferences(&ini->next, key, entry);
    }

    if (!strcmp(section, "midi_bindings")) {
        return midi_binding(ini, key, entry);
    }

    if (!strcmp(section, "shortcuts")) {
        return shortcut(ini, key, entry);
    }

    if (!strcmp(section, "percussion")) {
        return percussion(ini, key, entry);
    }

    if (!strcmp(section, "percussion_midi")) {
        return percussion_midi(ini, key, entry);
    }

    if (!strcmp(section, "recent")) {
        return recent(ini, key, entry);
    }

    if (!strcmp(section, "mouse")) {
        return mouse_gesture(ini, key, entry);
    }

    return true;
}

static bool parse(Ini *ini, char *data) {
    char section[64] = {0}, *cursor = data;
    bool ok = true;

    while (*cursor && ok) {
        char *end = strchr(cursor, '\n');

        if (end) {
            *end = 0;
        }

        ok = strlen(cursor) < DM_PATH * 3 + 256 && line(ini, cursor, section, sizeof(section));
        cursor = end ? end + 1 : cursor + strlen(cursor);
    }

    return ok;
}

static bool binding_taken(const Binding *b, int except, Binding binding) {
    for (int i = 0; i < ACT_COUNT; ++i) {
        if (i != except && ed_same_binding(b[i], binding)) {
            return true;
        }
    }

    return false;
}

/* Shortcut files written before the Preferences shortcut existed: Project settings leaves its old
   default P for its current default (/). Solo keeps S, which is its default again; the move below
   only applies if the defaults differ. Bindings the user changed are kept. */
static void upgrade_shortcuts(Ini *ini, Binding *merged) {
    Binding old_config = {.key = SDL_SCANCODE_P}, old_solo = {.key = SDL_SCANCODE_S};

    if (ini->seen[ACT_PREFERENCES] || !ini->seen[ACT_CONFIG] ||
        !ed_same_binding(ini->keys[ACT_CONFIG], old_config)) {
        return;
    }

    if (ini->seen[ACT_SOLO] && ed_same_binding(ini->keys[ACT_SOLO], old_solo) &&
        !binding_taken(merged, ACT_SOLO, ini->next.bindings[ACT_SOLO])) {
        merged[ACT_SOLO] = ini->next.bindings[ACT_SOLO];
    }

    if (!binding_taken(merged, ACT_CONFIG, ini->next.bindings[ACT_CONFIG])) {
        merged[ACT_CONFIG] = ini->next.bindings[ACT_CONFIG];
    }
}

/* An action the file does not list takes its default key. When the user already uses that key,
   the action takes the same key with Shift, Ctrl + Shift or Alt + Shift instead, so a new action
   never resets the user's shortcuts. */
static void place_new_actions(Ini *ini, Binding *merged) {
    static const SDL_Keymod extra[] = {SDL_KMOD_SHIFT, SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
                                       SDL_KMOD_ALT | SDL_KMOD_SHIFT};

    for (int i = 0; i < ACT_COUNT; ++i) {
        if (ini->seen[i] || !binding_taken(merged, i, merged[i])) {
            continue;
        }

        for (size_t k = 0; k < sizeof(extra) / sizeof(*extra); ++k) {
            Binding candidate = merged[i];
            candidate.mods |= extra[k];

            if (!binding_taken(merged, i, candidate)) {
                merged[i] = candidate;
                break;
            }
        }
    }
}

bool dm_preferences_load(Editor *e) {
    DmBytes b = {0};
    char error[256];

    if (!dm_read(e->settings_path, &b, error, sizeof(error))) {
        snprintf(e->status, sizeof(e->status), DM_TEXT_CANNOT_READ_INI_CONFIGURATION, error);
        return false;
    }

    const size_t bindings_limit =
        (4 * DM_PARTS + ED_MIDI_TARGETS - ED_MIDI_PLAY) * (2 * DM_MIDI_NAME + 80);

    if (b.size > 262144 + bindings_limit || memchr(b.data, 0, b.size) || !dm_bytes(&b, "", 1)) {
        dm_bytes_free(&b);
        snprintf(e->status, sizeof(e->status), DM_TEXT_INI_CONFIGURATION_TOO_LARGE);
        return false;
    }

    Ini *ini = calloc(1, sizeof(*ini));

    if (!ini) {
        dm_bytes_free(&b);
        snprintf(e->status, sizeof(e->status), DM_TEXT_CANNOT_ALLOCATE_INI_CONFIGURATION);
        return false;
    }

    ini->next = *e;
    memset(ini->next.recent, 0, sizeof(ini->next.recent));
    memset(ini->next.percussion_global, 0, 128);
    memset(ini->next.percussion_midi_global, 0, 128);
    bool ok = parse(ini, (char *)b.data);
    ok = ok && ini->version == 3;
    bool used[DM_PIANO_KEYS + 1] = {0}, heard[MIDI_KEYS + 1] = {0};

    for (unsigned n = 0; n < 128 && ok; n++) {
        unsigned key = ini->next.percussion_global[n];
        unsigned incoming = ini->next.percussion_midi_global[n];

        if ((key && used[key]) || (incoming && heard[incoming])) {
            ok = false;
        }

        used[key] = key != 0;
        heard[incoming] = incoming != 0;
    }

    if (ok) {
        /* Saved shortcuts apply over the current defaults, so actions added since the file was
           written keep their default keys. A clash with a saved key resets all shortcuts. */
        Binding merged[ACT_COUNT];
        memcpy(merged, ini->next.bindings, sizeof(merged));

        for (int i = 0; i < ACT_COUNT; ++i) {
            if (ini->seen[i]) {
                merged[i] = ini->keys[i];
            }
        }

        upgrade_shortcuts(ini, merged);
        place_new_actions(ini, merged);

        if (valid_bindings(merged)) {
            memcpy(ini->next.bindings, merged, sizeof(merged));
        } else {
            snprintf(ini->next.status, sizeof(ini->next.status), DM_TEXT_SHORTCUT_SETTINGS_RESET);
        }

        ini->next.recent_count = 0;

        for (int i = 0; i < 10; ++i) {
            if (*ini->next.recent[i]) {
                memmove(ini->next.recent[ini->next.recent_count++], ini->next.recent[i], DM_PATH);
            }
        }

        ok = ed_midi_bindings_replace(e, &ini->midi_bindings);

        if (ok) {
            ini->next.midi_bindings = e->midi_bindings;
            *e = ini->next;
        }
    }

    if (!ok) {
        snprintf(e->status, sizeof(e->status), DM_TEXT_INVALID_INI_CONFIGURATION_USE);
    }

    dm_bytes_free(&ini->midi_bindings);
    free(ini);
    dm_bytes_free(&b);
    return ok;
}

static bool text(DmBytes *b, const char *s) {
    return dm_bytes(b, s, strlen(s));
}

static bool quoted_text(DmBytes *b, const char *s) {
    if (!text(b, "\"")) {
        return false;
    }

    for (; *s; s++) {
        if ((unsigned char)*s < 32) {
            return false;
        }

        if (*s == '"' && !text(b, "\"")) {
            return false;
        }

        if (!dm_bytes(b, s, 1)) {
            return false;
        }
    }

    return text(b, "\"");
}

static bool text_field(DmBytes *b, const char *key, const char *s) {
    return text(b, key) && text(b, "=") && quoted_text(b, s) && text(b, "\n");
}

static bool input_field(DmBytes *b, Editor *e) {
    if (!text(b, "midi_inputs=")) {
        return false;
    }

    bool first = true;

    for (unsigned i = 0; i < DM_MIDI_INPUTS; ++i) {
        if (!*e->input_names[i]) {
            continue;
        }

        DmBytes name = {0};
        bool ok = text_field(&name, "", e->input_names[i]);

        if (ok) {
            ok = (first || text(b, ",")) && dm_bytes(b, name.data + 1, name.size - 2);
        }

        dm_bytes_free(&name);

        if (!ok) {
            return false;
        }

        first = false;
    }

    return text(b, "\n");
}

static bool fields(DmBytes *b, Editor *e) {
    return text_field(b, "soundfont", e->preferred_font) &&
           text_field(b, "midi_output", e->output_name) && input_field(b, e) &&
           text_field(b, "midi_folder", e->midi_folder) &&
           text_field(b, "soundfont_folder", e->font_folder) &&
           text_field(b, "export_folder", e->export_folder);
}

static bool midi_input_fields(DmBytes *b, const Editor *e) {
    char latency[32] = "auto";

    if (!isnan(e->input_latency_ms)) {
        snprintf(latency, sizeof(latency), "%.9g", e->input_latency_ms);
    }

    char fields[256];
    snprintf(fields, sizeof(fields),
             "\n[midi_input]\nvelocity=%s\nfixed_velocity=%u\nthru=%d\ncontrollers=%s\n"
             "latency=%s\nsustain_invert=%d\nchannel=%u\npickup=%d\n",
             midi_velocity_names[e->input_options.velocity],
             (unsigned)e->input_options.fixed_velocity, e->input_thru,
             midi_controller_names[e->input_controllers], latency, e->input_options.sustain_invert,
             (unsigned)e->input_options.channel, e->input_pickup);
    return text(b, fields);
}

static bool midi_binding_fields(DmBytes *b, const Editor *e) {
    if (!ed_midi_binding_count(e)) {
        return true;
    }

    bool ok = text(b, "\n[midi_bindings]\n");
    const EdMidiBinding *bindings = (const EdMidiBinding *)e->midi_bindings.data;

    for (size_t i = 0; i < ed_midi_binding_count(e) && ok; ++i) {
        const EdMidiBinding *binding = &bindings[i];
        char id[48], tail[48];
        ed_midi_target_id(binding->target, id, sizeof(id));
        snprintf(tail, sizeof(tail), ",%u,%s,%u,%s\n", binding->source.channel + 1u,
                 binding_source_names[binding->source.kind],
                 (unsigned)binding->source.number, binding->toggle ? "toggle" : "momentary");
        ok = text(b, id) && text(b, "=") && quoted_text(b, binding->source.device) && text(b, tail);
    }

    return ok;
}

bool dm_preferences_save(Editor *e) {
    DmBytes b = {0};
    char head[320];
    snprintf(head, sizeof(head), DM_TEXT_INI_APPLICATION_SECTION, e->accent, e->background,
             e->monitor ? 1 : 0, e->ui_size, e->os_cursor ? 1 : 0, e->update_mode,
             e->update_prompted ? 1 : 0, (unsigned long long)e->sample_hash,
             e->repository_history ? 1 : 0, e->modifier_select ? 1 : 0);
    bool ok =
        text(&b, head) && fields(&b, e) && midi_input_fields(&b, e) && midi_binding_fields(&b, e);
    ok = ok && text(&b, DM_TEXT_SHORTCUTS_SECTION);

    for (int i = 0; i < ACT_COUNT && ok; ++i) {
        char binding[160], label[160];
        setting_label(ed_actions[i].name, label, sizeof(label));
        ok = dm_binding_format(e->bindings[i], binding, sizeof(binding)) &&
             text_field(&b, label, binding);
    }

    ok = ok && text(&b, "\n[mouse]\n");

    for (int i = 0; i < GESTURE_REBINDABLE && ok; ++i) {
        char gesture[80];
        ok = ed_gesture_format(e->gestures[i], gesture, sizeof(gesture)) &&
             text_field(&b, ed_gestures[i].name, gesture);
    }

    ok = ok && text(&b, "\n[percussion]\n");

    for (unsigned n = 0; n < 128 && ok; n++) {
        percussion_label(n, head, sizeof(head));
        unsigned key = e->percussion_global[n];

        if (key > DM_PIANO_KEYS) {
            ok = false;
            break;
        }

        ok = text_field(&b, head, key ? SDL_GetScancodeName(ed_piano_scancode((int)key - 1)) : "");
    }

    ok = ok && text(&b, "\n[percussion_midi]\n");

    for (unsigned n = 0; n < 128 && ok; n++) {
        unsigned incoming = e->percussion_midi_global[n];

        if (incoming) {
            char label[160], line[200];
            percussion_label(n, label, sizeof(label));
            snprintf(line, sizeof(line), "%s=%u\n", label, incoming - 1);
            ok = text(&b, line);
        }
    }

    ok = ok && text(&b, DM_TEXT_RECENT_SECTION);

    for (int i = 0; i < e->recent_count && ok; ++i) {
        snprintf(head, sizeof(head), DM_TEXT_RECENT_FILE_KEY, i);
        ok = text_field(&b, head, e->recent[i]);
    }

    char error[256];
    ok = ok && dm_write(e->settings_path, b.data, b.size, error, sizeof(error));
    dm_bytes_free(&b);
    return ok;
}
