/* The preferences file: named shortcuts and quoted paths, named percussion keys, the OS cursor
   option and shortcut files written by earlier versions. Uses generated settings files only. */
#include "editor/internal.h"
#include <assert.h>
#define TEST_NAME "preferences"
#include "test_common.h"

static bool contains(DmBytes data, const char *text) {
    size_t length = strlen(text);

    for (size_t i = 0; i + length <= data.size; i++) {
        if (!memcmp(data.data + i, text, length)) {
            return true;
        }
    }

    return false;
}

/* Paths are quoted, shortcuts are written by name and parsed back, including odd keys. */
static void readable_values(Editor *e) {
    ed_defaults(e);
    snprintf(e->settings_path, DM_PATH, "agents/reports/preferences-test.ini");
    snprintf(e->midi_folder, DM_PATH, "C:\\MIDI files\\");
    snprintf(e->preferred_font, DM_PATH, "D:\\fonts\\a \"quoted\".sf2");
    e->bindings[ACT_PLAY_MODE] = (Binding){SDL_SCANCODE_F9, SDL_KMOD_SHIFT | SDL_KMOD_ALT};
    e->percussion_global[36] = (uint8_t)(ed_piano_index(SDL_SCANCODE_Z) + 1);
    assert(dm_preferences_save(e));
    DmBytes data = {0};
    char error[256];
    assert(dm_read(e->settings_path, &data, error, sizeof(error)));
    assert(dm_bytes(&data, "", 1));
    assert(strstr((char *)data.data, "midi_folder=\"C:\\MIDI files\\\""));
    assert(strstr((char *)data.data, "Shift + Alt + F9") &&
           !strstr((char *)data.data, "action_0="));
    dm_bytes_free(&data);
    ed_defaults(e);
    memset(e->percussion_global, 0, 128);
    assert(dm_preferences_load(e));
    assert(e->bindings[ACT_PLAY_MODE].key == SDL_SCANCODE_F9 && e->percussion_global[36]);
    Binding b;
    char text[160];
    assert(dm_binding_parse("Shift + Alt + E", &b) && b.key == SDL_SCANCODE_E &&
           b.mods == (SDL_KMOD_SHIFT | SDL_KMOD_ALT));
    assert(dm_binding_parse("Win + F9", &b) && b.mods == SDL_KMOD_GUI &&
           dm_binding_format(b, text, sizeof(text)) && !strcmp(text, "Win + F9"));
    assert(dm_binding_parse("=", &b) && b.key == SDL_SCANCODE_EQUALS);
    assert(dm_binding_parse("-", &b) && b.key == SDL_SCANCODE_MINUS);
    assert(dm_binding_parse("Keypad Add", &b) && b.key == SDL_SCANCODE_KP_PLUS &&
           dm_binding_format(b, text, sizeof(text)) && !strcmp(text, "Keypad Add"));
    assert(!dm_binding_parse("Shift + Shift + E", &b) && !dm_binding_parse("Left Click", &b) &&
           !dm_binding_parse("+", &b));
    puts("PASS readable quoted paths and named keyboard shortcuts");
}

/* Shortcut files from before the Preferences shortcut: untouched defaults move, customized ones
   stay, and a new action whose default is taken gets a free variant instead of resetting the
   user's shortcuts. Uses generated settings files only. */
static void load_shortcuts(Editor *e, const char *text) {
    ed_defaults(e);
    snprintf(e->settings_path, DM_PATH, "agents/reports/preferences-shortcuts.ini");
    assert(dm_write(e->settings_path, text, strlen(text), e->status, sizeof(e->status)));
    e->status[0] = 0;
    assert(dm_preferences_load(e));
}

static bool bound(Editor *e, int action, SDL_Scancode key, SDL_Keymod mods) {
    return e->bindings[action].key == key && e->bindings[action].mods == mods;
}

static void shortcut_upgrade(Editor *e) {
    load_shortcuts(e, "[application]\nschema=3\n[shortcuts]\nproject_settings=\"P\"\n"
                      "solo_selected_channel=\"S\"\n");
    assert(bound(e, ACT_CONFIG, SDL_SCANCODE_SLASH, 0) && bound(e, ACT_SOLO, SDL_SCANCODE_S, 0) &&
           bound(e, ACT_PREFERENCES, SDL_SCANCODE_P, 0) && !*e->status);

    load_shortcuts(e, "[application]\nschema=3\n[shortcuts]\nproject_settings=\"O\"\n"
                      "solo_selected_channel=\"S\"\n");
    assert(bound(e, ACT_CONFIG, SDL_SCANCODE_O, 0) && bound(e, ACT_SOLO, SDL_SCANCODE_S, 0) &&
           bound(e, ACT_PREFERENCES, SDL_SCANCODE_P, 0) && !*e->status);

    load_shortcuts(e, "[application]\nschema=3\n[shortcuts]\nproject_settings=\"P\"\n"
                      "metronome=\"I\"\n");
    assert(bound(e, ACT_METRONOME, SDL_SCANCODE_I, 0) &&
           bound(e, ACT_INSTRUMENT, SDL_SCANCODE_I, SDL_KMOD_SHIFT) &&
           bound(e, ACT_CONFIG, SDL_SCANCODE_SLASH, 0) && !*e->status);

    ed_defaults(e);
    e->status[0] = 0;
    puts("PASS older shortcut files keep custom keys and move untouched defaults");
}

static void cursor_preferences(Editor *e) {
    char saved[DM_PATH];
    snprintf(saved, sizeof(saved), "%s", e->settings_path);
    snprintf(e->settings_path, sizeof(e->settings_path), "agents/reports/preferences-test.ini");
    e->os_cursor = true;
    check(dm_preferences_save(e), "save isolated OS cursor preference");
    e->os_cursor = false;
    check(dm_preferences_load(e) && e->os_cursor, "OS cursor preference roundtrips");
    snprintf(e->settings_path, sizeof(e->settings_path), "%s", saved);
    e->os_cursor = false;
}

/* Actions and drum keys are saved under readable names; F1-F4 are the default toggles. */
static void named_settings(Editor *e) {
    snprintf(e->settings_path, DM_PATH, "agents/reports/preferences-test.ini");
    e->percussion_global[38] = 1;
    assert(dm_preferences_save(e));
    DmBytes b = {0};
    char error[256];
    assert(dm_read(e->settings_path, &b, error, sizeof(error)));
    assert(contains(b, "acoustic_snare=\"Z\"") && contains(b, "color_selected_notes=\"Alt + C\"") &&
           contains(b, "schema=3") && !contains(b, "note_38="));
    dm_bytes_free(&b);
    assert(dm_preferences_load(e) && e->percussion_global[38]);
    assert(e->bindings[ACT_PIANO].key == SDL_SCANCODE_F1 &&
           e->bindings[ACT_METRONOME].key == SDL_SCANCODE_F2 &&
           e->bindings[ACT_WAIT].key == SDL_SCANCODE_F3 &&
           e->bindings[ACT_OVERLAP].key == SDL_SCANCODE_F4);
    puts("PASS underscore action labels, named percussion settings and F1-F4 defaults");
}

int main(void) {
    Editor *e = test_editor_open("Preferences", (TestDesktop){.renderer = "software"});
    readable_values(e);
    shortcut_upgrade(e);
    cursor_preferences(e);
    named_settings(e);
    printf("PASS preferences: %d checks\n", checks);
    test_editor_close(e);
    return 0;
}
