#include "editor_internal.h"
static int checks;
static void check(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL workspace_navigation: %s\n", text);
        exit(1);
    }
}
void test_workspace_navigation_ui(Editor *e, void (*check)(bool, const char *));
static void configuration(Editor *e) {
    char error[256];
    const char *path = "agents/reports/workspace_navigation-config.ini";
    snprintf(e->settings_path, DM_PATH, "%s", path);
    ed_defaults(e);
    e->bindings[ACT_PLAY_MODE].key = SDL_SCANCODE_F9;
    e->accent = 0xff123456;
    snprintf(e->preferred_font, DM_PATH, "C:\\fonts\\a \"quoted\" \\font.sf2");
    snprintf(e->midi_folder, DM_PATH, "C:\\MIDI\\");
    snprintf(e->font_folder, DM_PATH, "D:\\Fonts\\");
    snprintf(e->output_name, sizeof(e->output_name), "My MIDI output");
    e->monitor = false;
    check(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names), "save INI configuration");
    e->accent = 0;
    e->preferred_font[0] = e->midi_folder[0] = e->font_folder[0] = e->output_name[0] = 0;
    ed_defaults(e);
    check(dm_preferences_load(e, ed_shortcut_schema()), "load generated INI");
    check(e->accent == 0xff123456 && strstr(e->preferred_font, "\"quoted\"") &&
              !strcmp(e->midi_folder, "C:\\MIDI\\") && !strcmp(e->font_folder, "D:\\Fonts\\"),
          "color and independently escaped folder/font strings round trip");
    check(!e->monitor && !strcmp(e->output_name, "My MIDI output"),
          "output choice and monitoring persist");
    check(e->bindings[ACT_PLAY_MODE].key == SDL_SCANCODE_F9, "custom binding restored");
    ed_defaults(e);
    check(dm_preferences_load(e, ed_shortcut_schema() + 1),
          "other preferences survive shortcut schema change");
    check(e->bindings[ACT_PLAY_MODE].key == SDL_SCANCODE_GRAVE && e->accent == 0xff123456,
          "changed defaults invalidate all saved shortcuts without discarding other settings");
    const char *bad[] = {"[application",
                         "[application]\nschema=1",
                         "[application]\nschema=3\naccent=bad",
                         "[application]\nschema=3\nsoundfont=%00",
                         "[application]\nschema=3\nsoundfont=%xx",
                         "[application]\nschema=3\nmonitor_soundfont=3"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        check(dm_write(path, bad[i], strlen(bad[i]), error, sizeof(error)),
              "write isolated invalid configuration");
        check(!dm_preferences_load(e, ed_shortcut_schema()) && e->accent == 0xff123456,
              "invalid INI/schema rejected without partial application");
    }
    const char *valid = "[application]\nschema=3\nsoundfont=\"é🎹.sf2\"\n[future]\nvalues=unknown";
    check(dm_write(path, valid, strlen(valid), error, sizeof(error)) &&
              dm_preferences_load(e, ed_shortcut_schema()),
          "Unicode paths and unknown INI sections");
    check(!strcmp(e->preferred_font, "\xc3\xa9\xf0\x9f\x8e\xb9.sf2"), "Unicode stored as UTF-8");
    ed_defaults(e);
    check(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names),
          "leave a valid isolated configuration example");
}
static void startup(SDL_Window *window, SDL_Renderer *renderer) {
    const char *base = "agents/reports/workspace_navigation-startup/";
    SDL_CreateDirectory(base);
    SDL_RemovePath("agents/reports/workspace_navigation-startup/duckmidi-settings.ini");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, window, renderer, NULL, base, false);
    SDL_PathInfo info;
    check(SDL_GetPathInfo(e->settings_path, &info),
          "first launch generates configuration beside app");
    snprintf(e->preferred_font, DM_PATH, "agents/reports/generated-sine.sf2");
    snprintf(e->output_name, sizeof(e->output_name), "Unavailable test-only MIDI output");
    e->monitor = false;
    editor_free(e);
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio != NULL, "audio for preference restoration");
    editor_init(e, window, renderer, audio, base, false);
    check(!strcmp(e->preferred_font, "agents/reports/generated-sine.sf2") &&
              !strcmp(e->project.soundfont, e->preferred_font),
          "next launch restores selected SoundFont");
    check(e->device < 0 && strstr(e->status, "unavailable"),
          "missing saved output falls back with a clear message");
    Uint64 start = SDL_GetTicks();
    while (e->font_request && SDL_GetTicks() - start < 10000) {
        editor_background(e);
        SDL_Delay(5);
    }
    check(!e->font_request && !strcmp(e->status, "SoundFont ready"),
          "saved SoundFont actually loads on next launch");
    editor_free(e);
    dm_audio_destroy(audio);
    free(e);
}
static void navigation(Editor *e) {
    editor_smoke(e);
    check(ed_piano_key(SDL_SCANCODE_Z) == 48 && ed_piano_key(SDL_SCANCODE_Q) == 60,
          "keyboard row starting octaves retained");
    check(ed_piano_key(SDL_SCANCODE_COMMA) == 60 && ed_piano_key(SDL_SCANCODE_SLASH) == 64,
          "lower row extends to slash");
    check(ed_piano_key(SDL_SCANCODE_LEFTBRACKET) == 77 && ed_piano_key(SDL_SCANCODE_EQUALS) == 78 &&
              ed_piano_key(SDL_SCANCODE_RIGHTBRACKET) == 79,
          "upper row extends with sharps through right bracket");
    check(ed_piano_key(SDL_SCANCODE_RETURN) < 0 && ed_piano_key(SDL_SCANCODE_LSHIFT) < 0,
          "modifier and enter keys are not piano notes");
    e->piano = true;
    ed_shortcut(e, SDL_SCANCODE_GRAVE, SDL_KMOD_NONE, false);
    check(e->pattern_mode, "mode shortcut operates while keyboard piano is enabled");
    e->context = 1;
    e->edit_focus = FOCUS_PLAYLIST;
    ed_play(e);
    check(e->pattern_mode, "playlist focus does not override chosen pattern playback");
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    ed_shortcut(e, SDL_SCANCODE_GRAVE, SDL_KMOD_NONE, false);
    check(!e->pattern_mode, "mode shortcut selects song despite piano-roll focus");
    DmPattern *p = ed_pattern(e);
    p->count = 0;
    p->length = 40;
    dm_note_add(p, (DmNote){20, 1, 80, 100, 0});
    dm_note_add(p, (DmNote){1, 1, 42, 100, 0});
    e->focus_pattern = true;
    ed_focus_first_note(e, p, (UiRect){0, 0, 640, 200});
    check(e->roll_view.scroll_x == 0 && e->roll_view.scroll_y == 1230,
          "pitch range midpoint is vertically centered and near-start time remains left aligned");
    p->notes[1].beat = 30;
    e->focus_pattern = true;
    ed_focus_first_note(e, p, (UiRect){0, 0, 640, 200});
    check(e->roll_view.scroll_x == 1280 && e->roll_view.scroll_y == 1230,
          "large leading gap centers earliest time while preserving pitch range midpoint");
    int list[16];
    for (int c = 0; c < 16; ++c) {
        e->project.channels[c] = (DmChannel){false, false, false, -1, 100, 64};
    }
    e->project.clip_count = 0;
    check(ed_channels(e, list) == 1 && list[0] == 0, "empty arrangement offers one spare channel");
    e->project.channels[0].enabled = true;
    e->project.channels[0].program = 2;
    check(ed_channels(e, list) == 2 && list[0] == 0 && list[1] == 1,
          "enabled channel is followed by one spare");
    for (int c = 0; c < 16; ++c) {
        e->project.channels[c] = (DmChannel){true, false, false, 0, 100, 64};
    }
    check(ed_channels(e, list) == 16,
          "full arrangement does not add an invalid seventeenth channel");
}
static void selection(Editor *e) {
    editor_smoke(e);
    e->dirty = false;
    UiMouse mouse = {0};
    mouse.pos = (UiPoint){220, 100};
    ed_playlist_hit_click(e, mouse, 0, 0);
    mouse.left = true;
    int list[] = {0, 1, 9};
    ed_playlist_drag(e, (UiRect){0, 0, 600, 400}, &e->playlist_view, 64, 40, list, 3, mouse);
    check(!e->dirty && !e->clip_moved, "stationary clip press is selection only");
    mouse.left = false;
    mouse.released = true;
    ed_playlist_drag(e, (UiRect){0, 0, 600, 400}, &e->playlist_view, 64, 40, list, 3, mouse);
    check(!e->dirty && e->drag == 0, "selection release ends without republishing playback");
}
int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w =
        SDL_CreateWindow("workspace_navigation validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", true);
    configuration(e);
    startup(w, r);
    navigation(e);
    selection(e);
    e->accent = 0xff56eeffu;
    ed_message(e, "Ready");
    test_workspace_navigation_ui(e, check);
    printf("PASS workspace_navigation: %d checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
