#include "editor/internal.h"
#include "midi_input_fixture.h"

static void verify(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL midi input preferences: %s\n", message);
        exit(1);
    }
}

void test_midi_input_preferences_ui(Editor *e, const char *label);

static void input_preferences(Editor *e) {
    DmMidiInputOptions defaults = e->input_options;
    verify(defaults.velocity == DM_MIDI_VELOCITY_MEDIUM && !defaults.fixed_velocity &&
               !defaults.sustain_invert && !defaults.channel && e->input_thru &&
               e->input_controllers == DM_INPUT_CONTROLLERS_KEYBOARD &&
               isnan(e->input_latency_ms) && e->input_pickup,
           "MIDI input defaults");
    MidiFixture f;
    DmMidiInput *input = fixture_create(&f);
    atomic_store(&f.available, 1);
    ed_midi_input_attach(e, input);
    e->smoke = false;
    snprintf(e->settings_path, sizeof(e->settings_path),
             "build/development/midi_input_preferences_test.ini");
    e->preferences = true;
    verify(!atomic_load(&f.lists), "input enumeration waits for Preferences");
    test_midi_input_preferences_ui(e, NULL);
    char names[1][DM_MIDI_NAME];
    uint64_t deadline = SDL_GetTicks() + 5000;

    while (!dm_midi_input_ports(input, names, 1)) {
        verify(SDL_GetTicks() < deadline, "opening MIDI input section refreshes port list");
        SDL_Delay(1);
    }

    verify(!strcmp(names[0], "Fixture 0"), "fake device appears in the list");
    test_midi_input_preferences_ui(e, names[0]);
    fixture_wait(input, 0, DM_MIDI_OPEN);
    verify(!strcmp(e->input_names[0], "Fixture 0"), "device checkbox updates saved input names");
    e->input_names[0][0] = 0;
    verify(dm_preferences_load(e, ed_shortcut_schema()) && !strcmp(e->input_names[0], "Fixture 0"),
           "device checkbox persists the request");
    test_midi_input_preferences_ui(e, "Fixed velocity");
    verify(e->input_options.fixed_velocity == 100, "Fixed velocity checkbox applies the setting");
    fixture_emit(&f, 0, MIDI_NOTE_ON, 60, 12, SDL_GetTicksNS());
    ed_midi_input_frame(e);
    DmMidiInputEvent activity;
    char name[DM_MIDI_NAME];
    verify(ed_midi_input_activity(e, &activity, name) && activity.b == 100 &&
               !strcmp(name, "Fixture 0") && activity.status == MIDI_NOTE_ON,
           "checkbox reaches worker and activity updates without an open project");

    e->input_options = (DmMidiInputOptions){
        .velocity = DM_MIDI_VELOCITY_HEAVY,
        .fixed_velocity = 87,
        .sustain_invert = true,
        .channel = 16,
    };
    e->input_thru = false;
    e->input_controllers = DM_INPUT_CONTROLLERS_SONG;
    e->input_latency_ms = -12.75f;
    e->input_pickup = false;
    ed_preference_save(e);
    e->input_options = defaults;
    e->input_thru = e->input_pickup = true;
    e->input_controllers = DM_INPUT_CONTROLLERS_KEYBOARD;
    e->input_latency_ms = NAN;
    verify(dm_preferences_load(e, ed_shortcut_schema()) &&
               e->input_options.velocity == DM_MIDI_VELOCITY_HEAVY &&
               e->input_options.fixed_velocity == 87 && e->input_options.sustain_invert &&
               e->input_options.channel == 16 && !e->input_thru &&
               e->input_controllers == DM_INPUT_CONTROLLERS_SONG &&
               e->input_latency_ms == -12.75f && !e->input_pickup,
           "all MIDI input settings round-trip");
    ed_midi_input_restore(e);
    verify(!dm_midi_input_thru(input), "loaded thru reaches the worker");
    fixture_emit(&f, 0, MIDI_CONTROL, MIDI_CC_MODULATION, 9, SDL_GetTicksNS());
    ed_midi_input_frame(e);
    verify(ed_midi_input_activity(e, &activity, name) && activity.status == MIDI_NOTE_ON,
           "filtered channel does not appear in activity");
    fixture_emit(&f, 0, MIDI_CONTROL | 15, MIDI_CC_SUSTAIN, 10, SDL_GetTicksNS());
    ed_midi_input_frame(e);
    verify(ed_midi_input_activity(e, &activity, name) && activity.status == (MIDI_CONTROL | 15) &&
               activity.a == MIDI_CC_SUSTAIN && activity.b == 117,
           "restored channel filter and sustain polarity reach the worker");

    atomic_store(&f.available, 0);
    dm_midi_input_refresh(input);
    fixture_wait(input, 0, DM_MIDI_MISSING);
    test_midi_input_preferences_ui(e, "Fixture 0");
    fixture_wait(input, 0, DM_MIDI_CLOSED);
    verify(!e->input_names[0][0], "a missing device stays listed and can be disabled");

    e->input_options = defaults;
    e->input_thru = e->input_pickup = true;
    e->input_controllers = DM_INPUT_CONTROLLERS_KEYBOARD;
    e->input_latency_ms = NAN;
    ed_preference_save(e);
    e->input_latency_ms = 10;
    verify(dm_preferences_load(e, ed_shortcut_schema()) && isnan(e->input_latency_ms),
           "automatic latency round-trips");

    static const char *const bad[] = {
        "velocity=unknown", "fixed_velocity=128", "thru=2",      "controllers=all",
        "latency=nan",      "latency=1001",       "latency=2ms", "sustain_invert=-1",
        "channel=17",       "pickup=true",
    };

    for (unsigned i = 0; i < SDL_arraysize(bad); ++i) {
        char ini[128];
        int n = snprintf(ini, sizeof(ini),
                         "[application]\nschema=3\n[midi_input]\n"
                         "velocity=light\n%s\n",
                         bad[i]);
        verify(SDL_SaveFile(e->settings_path, ini, n) &&
                   !dm_preferences_load(e, ed_shortcut_schema()),
               bad[i]);
        verify(e->input_options.velocity == defaults.velocity && !e->input_options.fixed_velocity &&
                   !e->input_options.sustain_invert && !e->input_options.channel && e->input_thru &&
                   e->input_controllers == DM_INPUT_CONTROLLERS_KEYBOARD &&
                   isnan(e->input_latency_ms) && e->input_pickup,
               "malformed MIDI options leave the defaults intact");
    }

    e->preferences = false;
    e->smoke = true;
    SDL_RemovePath(e->settings_path);
    ed_midi_input_attach(e, NULL);
    fixture_free(&f);
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    verify(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO), "SDL");
    SDL_Window *window =
        SDL_CreateWindow("MIDI input preferences validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    verify(window && renderer && ui_init(window, renderer, NULL), "hidden UI");
    Editor *e = calloc(1, sizeof(*e));
    verify(e != NULL, "editor allocation");
    editor_init(e, window, renderer, NULL, "", "", true);
    input_preferences(e);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    puts("PASS midi input preferences");
    return 0;
}
