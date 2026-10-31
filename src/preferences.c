#include "editor_internal.h"
#include "text/en.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Editor next;
    Binding keys[ACT_COUNT];
    bool seen[ACT_COUNT];
    bool percussion_seen[128];
    uint32_t version, fingerprint;
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

            *binding = (Binding){key, mods};
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
    const char *name =
        binding.key == SDL_SCANCODE_KP_PLUS ? "Keypad Add" : SDL_GetScancodeName(binding.key);

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
        if (b[i].key <= 0 || b[i].key >= SDL_SCANCODE_COUNT || (b[i].mods & ~allowed)) {
            return false;
        }

        for (int k = 0; k < i; ++k) {
            if (b[i].key == b[k].key && b[i].mods == b[k].mods) {
                return false;
            }
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

    if (!strcmp(key, "accent")) {
        uint32_t color;

        if (!number(s, &color)) {
            return false;
        }

        e->accent = color | 0xff000000u;
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

    return true;
}

static bool shortcut(Ini *ini, const char *key, char *s) {
    if (!strcmp(key, "schema")) {
        return number(s, &ini->fingerprint);
    }

    int index = -1;

    for (int i = 0; i < ACT_COUNT; i++) {
        char label[160];
        setting_label(ed_action_names[i], label, sizeof(label));

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

static bool percussion(Ini *ini, const char *key, const char *s) {
    unsigned note = 128;

    for (unsigned n = 0; n < 128; n++) {
        char label[160];
        percussion_label(n, label, sizeof(label));

        if (!strcmp(key, label)) {
            note = n;
            break;
        }
    }

    if (note >= 128 || ini->percussion_seen[note]) {
        return false;
    }

    ini->percussion_seen[note] = true;
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

    if (!strcmp(section, "shortcuts")) {
        return shortcut(ini, key, entry);
    }

    if (!strcmp(section, "percussion")) {
        return percussion(ini, key, entry);
    }

    if (!strcmp(section, "recent")) {
        return recent(ini, key, entry);
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

bool dm_preferences_load(Editor *e, uint32_t shortcut_schema) {
    DmBytes b = {0};
    char error[256];

    if (!dm_read(e->settings_path, &b, error, sizeof(error))) {
        snprintf(e->status, sizeof(e->status), DM_TEXT_CANNOT_READ_INI_CONFIGURATION, error);
        return false;
    }

    if (b.size > 262144 || memchr(b.data, 0, b.size) || !dm_bytes(&b, "", 1)) {
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
    bool ok = parse(ini, (char *)b.data);
    ok = ok && ini->version == 3;
    bool used[DM_PIANO_KEYS + 1] = {0};

    for (unsigned n = 0; n < 128 && ok; n++) {
        unsigned key = ini->next.percussion_global[n];

        if (key && used[key]) {
            ok = false;
        }

        used[key] = key != 0;
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

        (void)shortcut_schema;

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

        *e = ini->next;
    }

    if (!ok) {
        snprintf(e->status, sizeof(e->status), DM_TEXT_INVALID_INI_CONFIGURATION_USE);
    }

    free(ini);
    dm_bytes_free(&b);
    return ok;
}

static bool text(DmBytes *b, const char *s) {
    return dm_bytes(b, s, strlen(s));
}

static bool text_field(DmBytes *b, const char *key, const char *s) {
    if (!text(b, key) || !text(b, "=\"")) {
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

    return text(b, "\"\n");
}

static bool fields(DmBytes *b, Editor *e) {
    return text_field(b, "soundfont", e->preferred_font) &&
           text_field(b, "midi_output", e->output_name) &&
           text_field(b, "midi_folder", e->midi_folder) &&
           text_field(b, "soundfont_folder", e->font_folder) &&
           text_field(b, "export_folder", e->export_folder);
}

bool dm_preferences_save(Editor *e, uint32_t shortcut_schema, const char *const *names) {
    DmBytes b = {0};
    char head[256];
    snprintf(head, sizeof(head), DM_TEXT_INI_APPLICATION_SECTION, e->accent, e->monitor ? 1 : 0,
             e->ui_size, e->os_cursor ? 1 : 0);
    bool ok = text(&b, head) && fields(&b, e);
    snprintf(head, sizeof(head), DM_TEXT_SHORTCUTS_NSCHEMA, shortcut_schema);
    ok = ok && text(&b, head);

    for (int i = 0; i < ACT_COUNT && ok; ++i) {
        char binding[160], label[160];
        setting_label(names[i], label, sizeof(label));
        ok = dm_binding_format(e->bindings[i], binding, sizeof(binding)) &&
             text_field(&b, label, binding);
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
