#include "editor_internal.h"
#include "../shared/pattern_generation.h"
#include <assert.h>

static void capture(Editor *e, const char *path) {
    for (int frame = 0; frame < 6; frame++) {
        ui_frame();
        editor_frame(e, 1000, 800);
        ui_render(e->renderer);
    }

    SDL_Surface *image = SDL_RenderReadPixels(e->renderer, NULL);
    assert(image && SDL_SaveBMP(image, path));
    SDL_DestroySurface(image);
}

static void fixture(Editor *e) {
    ed_save_free(e);
    ed_release_piano(e);
    ed_clear_history(e->undo, &e->undo_count);
    ed_clear_history(e->redo, &e->redo_count);
    dm_project_free(&e->project);
    dm_project_init(&e->project);
    ++e->project_session;
    int p = dm_pattern_add(&e->project, "Generated fixture");
    assert(p == 0);
    assert(dm_note_add(&e->project.patterns[0], (DmNote){0, 1, 60, 100, 0}));
    e->active = true;
    e->selected_pattern = e->selected_channel = e->pattern_channel = 0;
    e->drag = 0;
    e->tool = 0;
    e->percussion_window = false;
}

static void wait_save(Editor *e) {
    uint64_t start = SDL_GetTicks();

    while (ed_save_busy(e) && SDL_GetTicks() - start < 10000) {
        ed_save_poll(e);
        SDL_Delay(1);
    }

    assert(!ed_save_busy(e));
}

/* Shortcut files from before the Preferences shortcut: untouched defaults move, customized ones
   stay, and a new action whose default is taken gets a free variant instead of resetting the
   user's shortcuts. Uses generated settings files only. */
static void load_shortcuts(Editor *e, const char *text) {
    ed_defaults(e);
    snprintf(e->settings_path, DM_PATH, "agents/reports/updates-2026-10-31/shortcuts.ini");
    assert(dm_write(e->settings_path, text, strlen(text), e->status, sizeof(e->status)));
    e->status[0] = 0;
    assert(dm_preferences_load(e, ed_shortcut_schema()));
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

static void preferences(Editor *e) {
    ed_defaults(e);
    snprintf(e->settings_path, DM_PATH, "agents/reports/backlog-2026-10-29/settings.ini");
    snprintf(e->midi_folder, DM_PATH, "C:\\MIDI files\\");
    snprintf(e->preferred_font, DM_PATH, "D:\\fonts\\a \"quoted\".sf2");
    e->bindings[ACT_PLAY_MODE] = (Binding){SDL_SCANCODE_F9, SDL_KMOD_SHIFT | SDL_KMOD_ALT};
    e->percussion_global[36] = (uint8_t)(ed_piano_index(SDL_SCANCODE_Z) + 1);
    assert(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names));
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
    assert(dm_preferences_load(e, ed_shortcut_schema()));
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

static void percussion(Editor *e) {
    fixture(e);
    e->piano = false;
    e->piano_octave = 2;
    ed_percussion_open(e);
    e->percussion_note = 38;
    e->percussion_listening = true;
    SDL_Event key = {0};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_F1;
    key.key.key = SDLK_F1;
    editor_event(e, &key);
    assert(e->percussion_listening);
    key.key.scancode = SDL_SCANCODE_ESCAPE;
    key.key.key = SDLK_ESCAPE;
    editor_event(e, &key);
    assert(!e->percussion_listening && e->percussion_window);
    e->percussion_listening = true;
    key.key.scancode = SDL_SCANCODE_Z;
    key.key.key = SDLK_Z;
    editor_event(e, &key);
    assert(!e->percussion_listening && !e->percussion_draft[36] && e->percussion_draft[38]);

    /* Not listening: the key plays the drum it is mapped to in the draft. */
    editor_event(e, &key);
    assert(e->held[SDL_SCANCODE_Z] == 38 && e->held_channels[SDL_SCANCODE_Z] == MIDI_DRUM_CHANNEL);
    key.type = SDL_EVENT_KEY_UP;
    editor_event(e, &key);
    assert(e->held[SDL_SCANCODE_Z] == -1);
    key.type = SDL_EVENT_KEY_DOWN;
    ed_percussion_save(e, true);
    assert(e->project.percussion_override && e->dirty);
    capture(e, "agents/reports/backlog-2026-10-29/percussion-mapping.bmp");
    assert(ed_keyboard_note(e, SDL_SCANCODE_Z, 9) == 38 &&
           ed_keyboard_note(e, SDL_SCANCODE_Z, 0) == 72);
    assert(e->percussion_global[36]);
    DmBytes b = {0};
    assert(dm_pack(&e->project, &b));
    DmProject copy;
    dm_project_init(&copy);
    char error[256];
    assert(dm_unpack(&copy, b.data, b.size, error, sizeof(error)));
    assert(copy.percussion_override && copy.percussion_keys[38]);
    dm_project_free(&copy);
    dm_bytes_free(&b);
    ed_undo(e, false);
    assert(!e->project.percussion_override && ed_keyboard_note(e, SDL_SCANCODE_Z, 9) == 36);
    e->percussion_window = false;
    puts("PASS percussion learning, override precedence, persistence and undo");
}

static void gestures(Editor *e) {
    fixture(e);
    assert(dm_clip_add(&e->project, (DmClip){1, 0, 0, 1, 1, 0}));
    assert(dm_clip_add(&e->project, (DmClip){3, 0, 1, 1, 1, 0}));
    assert(dm_clip_add(&e->project, (DmClip){5, 0, 2, 0, 1, 0}));
    UiMouse m = {0};
    m.clicked = m.left = true;
    m.pos = (UiPoint){100, 60};
    ed_playlist_hit_click(e, m, 0, 1);
    assert(e->project.clips[0].selected && e->project.clips[1].selected);
    int rows[] = {0, 1, 2, 3};
    View v = {1, 0, 0, 0};
    m.clicked = false;
    m.pos = (UiPoint){120, 80};
    ed_playlist_drag(e, (UiRect){0, 0, 800, 500}, &v, 20, 10, rows, 4, m);
    assert(e->project.clips[0].beat == 3 && e->project.clips[1].beat == 5 &&
           e->project.clips[0].channel == 1 && e->project.clips[1].channel == 2);
    assert(e->project.clips[2].beat == 5 && e->project.clips[2].channel == 2 && e->undo_count == 1);
    m.left = false;
    m.released = true;
    ed_playlist_drag(e, (UiRect){0, 0, 800, 500}, &v, 20, 10, rows, 4, m);
    ed_undo(e, false);
    assert(e->project.clips[0].beat == 1 && e->project.clips[1].beat == 3);
    fixture(e);
    DmPattern *p = ed_pattern(e);
    UiRect r = {0, 0, 800, 500}, grid = {0, 28, 800, 472}, box = {100, 100, 100, 20};
    m = (UiMouse){0};
    m.clicked = m.left = m.shift = true;
    m.pos = (UiPoint){150, 110};
    ed_piano_click(e, p, r, grid, &v, 80, 20, m, 0, box);
    assert(e->drag == 3);
    e->drag = 0;
    m.shift = false;
    m.alt = true;
    ed_piano_click(e, p, r, grid, &v, 80, 20, m, 0, box);
    assert(e->drag == 2 && e->drag_edge == 0);
    e->drag = 0;
    m.pos.x = 101;
    ed_piano_click(e, p, r, grid, &v, 80, 20, m, 0, box);
    assert(e->drag == 2 && e->drag_edge == -1);
    e->drag = 0;
    puts("PASS selected playlist group drag and initial Shift/Alt note gestures");
}

static void saves(Editor *e) {
    fixture(e);

    for (int i = 1; i < 20000; i++) {
        assert(dm_note_add(ed_pattern(e), (DmNote){i * .25, .125, (uint8_t)(48 + i % 24), 90, 0}));
    }

    ed_changed(e);
    const char *path = "agents/reports/backlog-2026-10-29/snapshot.duck";
    uint64_t start = SDL_GetPerformanceCounter();
    assert(ed_save_to(e, path) && ed_save_busy(e));
    printf("Save snapshot main-thread preparation: %.3f ms\n",
           (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency());
    ed_pattern(e)->notes[0].velocity = 31;
    ed_changed(e);
    wait_save(e);
    assert(e->dirty);
    DmBytes data = {0};
    char error[256];
    DmProject copy;
    dm_project_init(&copy);
    assert(dm_read(path, &data, error, sizeof(error)) &&
           dm_unpack(&copy, data.data, data.size, error, sizeof(error)));
    assert(copy.patterns[0].count == 20000 && copy.patterns[0].notes[0].velocity == 100);
    dm_project_free(&copy);
    dm_bytes_free(&data);
    assert(ed_save_to(e, path));
    wait_save(e);
    assert(!e->dirty);
    ed_changed(e);
    assert(ed_save_to(e, "AGENTS.md/cannot-save.duck"));
    wait_save(e);
    assert(e->dirty && strstr(e->status, "failed"));
    assert(ed_save_to(e, path));
    ed_save_cancel(e);
    wait_save(e);
    assert(dm_read(path, &data, error, sizeof(error)));
    dm_project_init(&copy);
    assert(dm_unpack(&copy, data.data, data.size, error, sizeof(error)));
    dm_project_free(&copy);
    dm_bytes_free(&data);
    puts("PASS worker save snapshots, newer edits, output failure and cancellation atomicity");
}

static void wait_generation(Editor *e) {
    uint64_t start = SDL_GetTicks();

    while (ed_generate_busy(e) && SDL_GetTicks() - start < 15000) {
        ed_generate_poll(e);
        SDL_Delay(1);
    }

    assert(!ed_generate_busy(e));
}

static int cc_value(const DmProject *p, uint64_t tick, int cc) {
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration;
    char error[256];
    assert(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)));
    int value = -1;

    for (size_t i = 0; i < count; i++) {
        if (events[i].tick <= tick && events[i].status == 0xb0 && events[i].a == (unsigned)cc) {
            value = (int)events[i].b;
        }
    }

    free(events);
    return value;
}

static void generation(Editor *e) {
    fixture(e);
    DmPattern *p = ed_pattern(e);
    p->count = 0;

    for (int repeat = 0; repeat < 3; repeat++) {
        for (int j = 0; j < 4; j++) {
            assert(dm_note_add(p, (DmNote){repeat * 3.125 + j * .625, .25, (uint8_t)(60 + j * 2),
                                           (uint8_t)(j == 1 && repeat == 2 ? 99 : 100), 0}));
        }
    }

    assert(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0}));
    DmAutomation a = {
        .pattern = 0, .type = 0xb0, .channel = 0, .a = 1, .b = 20, .curve = 1, .anchor = 60};
    assert(dm_automation_add(&e->project, a));
    a.beat = 2;
    a.b = 100;
    a.bend = 12;
    assert(dm_automation_add(&e->project, a));
    a.beat = 2.01;
    a.curve = -1;
    a.b = 20;
    assert(dm_automation_add(&e->project, a));
    int lfo = dm_pattern_add(&e->project, "Authored LFO");
    assert(lfo >= 0);
    e->project.patterns[lfo].kind = 1;
    e->project.patterns[lfo].mode = 1;
    e->project.patterns[lfo].event = 11;
    e->project.patterns[lfo].tension = .37;
    e->project.patterns[lfo].length = 9;
    e->project.lane_count[0] = 1;
    snprintf(e->project.lane_names[0][0], 64, "Original lane");
    assert(dm_clip_add(&e->project, (DmClip){.pattern = lfo, .channel = 0, .lane = 1}));
    DmBytes before = {0}, after = {0};
    assert(dm_pack(&e->project, &before));
    ed_generate_start(e);
    wait_generation(e);
    const DmProject *preview = ed_generate_preview(e);
    assert(preview && preview->pattern_count > 1);
    capture(e, "agents/reports/backlog-2026-10-29/generated-preview.bmp");
    assert(dm_pack(&e->project, &after) && before.size == after.size &&
           !memcmp(before.data, after.data, before.size));
    dm_bytes_free(&after);

    for (unsigned t = 0; t < 9000; t += 100) {
        assert(cc_value(&e->project, t, 1) == cc_value(preview, t, 1));
    }

    size_t notes = 0;
    bool different_velocity = false, curves = false;

    for (size_t i = 0; i < preview->clip_count; i++) {
        DmPattern q = preview->patterns[preview->clips[i].pattern];

        if (!q.kind) {
            notes += q.count;

            for (size_t j = 0; j < q.count; j++) {
                different_velocity |= q.notes[j].velocity == 99;
            }
        }
    }

    for (size_t i = 0; i < preview->automation_count; i++) {
        curves |= preview->automation[i].bend == 12;
    }

    assert(notes == 12 && different_velocity && curves);
    bool lfo_retained = false;

    for (size_t i = 0; i < preview->pattern_count; i++) {
        lfo_retained |= preview->patterns[i].mode == 1 && preview->patterns[i].tension == .37;
    }

    assert(lfo_retained && !strcmp(preview->lane_names[0][0], "Original lane"));
    DmBytes generated = {0};
    assert(dm_pack(preview, &generated));
    DmProject check;
    dm_project_init(&check);
    char error[256];
    assert(dm_unpack(&check, generated.data, generated.size, error, sizeof(error)));
    dm_project_free(&check);
    dm_bytes_free(&generated);
    ed_pattern(e)->notes[0].velocity = 29;
    ed_changed(e);
    assert(!ed_generate_apply(e));
    ed_generate_start(e);
    wait_generation(e);
    assert(ed_generate_apply(e) && e->dirty);
    assert(e->undo_count == 1);
    ed_undo(e, false);
    assert(e->project.patterns[0].count == 12 && e->project.patterns[0].notes[0].velocity == 29);
    ed_generate_start(e);
    e->generate_window = false;
    wait_generation(e);
    ed_generate_poll(e);
    assert(!e->pattern_job);
    dm_bytes_free(&before);
    puts(
        "PASS pattern generation preview, repeated phrases, velocity variants, editable curves, stale edits, Apply/undo and cancellation");
}

static int note_order(const void *a, const void *b) {
    const DmExportEvent *x = a, *y = b;

    if (x->tick != y->tick) {
        return x->tick < y->tick ? -1 : 1;
    }

    if (x->status != y->status) {
        return (int)x->status - (int)y->status;
    }

    if (x->a != y->a) {
        return (int)x->a - (int)y->a;
    }

    return (int)x->b - (int)y->b;
}

static void control_update(int state[16][260], DmExportEvent event) {
    unsigned type = event.status & 0xf0, ch = event.status & 15;
    int index = -1, value = (int)event.b;

    if (type == 0xb0) {
        index = (int)event.a;
    } else if (type == 0xc0) {
        index = 128;
        value = (int)event.a;
    } else if (type == 0xe0) {
        index = 129;
        value = (int)(event.a | (event.b << 7));
    } else if (type == 0xd0) {
        index = 130;
        value = (int)event.a;
    } else if (type == 0xa0) {
        index = 131 + (int)event.a;
    }

    if (index >= 0) {
        state[ch][index] = value;
    }
}

static void compare_controls(const char *file, const DmExportEvent *before, size_t bn,
                             const DmExportEvent *after, size_t an) {
    int source[16][260], generated[16][260];

    for (int ch = 0; ch < 16; ch++) {
        for (int c = 0; c < 260; c++) {
            source[ch][c] = generated[ch][c] = -1;
        }
    }

    size_t i = 0, j = 0;

    while (i < bn || j < an) {
        uint64_t tick = i < bn ? before[i].tick : UINT64_MAX;

        if (j < an && after[j].tick < tick) {
            tick = after[j].tick;
        }

        while (i < bn && before[i].tick == tick) {
            control_update(source, before[i++]);
        }

        while (j < an && after[j].tick == tick) {
            control_update(generated, after[j++]);
        }

        for (int ch = 0; ch < 16; ch++) {
            for (int c = 0; c < 260; c++) {
                if (source[ch][c] >= 0 && source[ch][c] != generated[ch][c]) {
                    fprintf(stderr, "Control mismatch %s tick %llu channel %d control %d: %d/%d\n",
                            file, (unsigned long long)tick, ch, c, source[ch][c], generated[ch][c]);
                    assert(false);
                }
            }
        }
    }
}

static void imported_generation(void) {
    const char *files[] = {"samples/imp_night.mid", "samples/e2m1.mus", "samples/D_46_D15.mid",
                           "samples/d_01_51.mus"};

    for (unsigned f = 0; f < 4; f++) {
        DmProject source, result;
        dm_project_init(&source);
        dm_project_init(&result);
        DmBytes data = {0};
        char error[512];
        DmGenerationStats stats;
        assert(dm_read(files[f], &data, error, sizeof(error)) &&
               dm_import(&source, data.data, data.size, 0, 0, error, sizeof(error)));
        assert(dm_generate_patterns(&source, &result, NULL, &stats, error, sizeof(error)));
        DmExportEvent *before = NULL, *after = NULL;
        size_t bn = 0, an = 0;
        uint64_t bd, ad;
        assert(dm_export_timeline(&source, &before, &bn, &bd, error, sizeof(error)) &&
               dm_export_timeline(&result, &after, &an, &ad, error, sizeof(error)));
        compare_controls(files[f], before, bn, after, an);
        qsort(before, bn, sizeof(*before), note_order);
        qsort(after, an, sizeof(*after), note_order);
        size_t i = 0, j = 0, notes = 0;

        for (;;) {
            while (i < bn && (before[i].status & 0xe0) != 0x80) {
                i++;
            }

            while (j < an && (after[j].status & 0xe0) != 0x80) {
                j++;
            }

            if (i == bn || j == an) {
                assert(i == bn && j == an);
                break;
            }

            if (before[i].tick != after[j].tick || before[i].status != after[j].status ||
                before[i].a != after[j].a || before[i].b != after[j].b) {
                fprintf(stderr, "Note mismatch %s index %zu: %llu %x %u %u / %llu %x %u %u\n",
                        files[f], notes, (unsigned long long)before[i].tick, before[i].status,
                        before[i].a, before[i].b, (unsigned long long)after[j].tick,
                        after[j].status, after[j].a, after[j].b);
                assert(false);
            }

            i++;
            j++;
            notes++;
        }

        printf("PASS generation note timeline: %s (%zu events, %zu linked repeats)\n", files[f],
               notes, stats.linked_placements);
        free(before);
        free(after);
        dm_bytes_free(&data);
        dm_project_free(&source);
        dm_project_free(&result);
    }
}

/* The first-launch update question waits for an open file, then fits its contents. No network:
   nothing is applied or checked. */
static void updates(Editor *e) {
    e->active = false;
    e->update_prompted = false;
    ed_update_launch(e);
    capture(e, "agents/reports/updates-2026-10-31/welcome.bmp");
    assert(!e->update_window && e->update_prompt_pending && !e->update);
    fixture(e);
    capture(e, "agents/reports/updates-2026-10-31/updates-window.bmp");
    assert(e->update_window && !e->update_prompt_pending && e->update_choice == 1 && !e->update);
    e->update_window = false;
    e->update_prompted = true;
    puts("PASS update question waits for an open file");
}

int main(void) {
    assert(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *window = SDL_CreateWindow("Backlog checks", 1000, 800, SDL_WINDOW_HIDDEN);
    assert(window);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    assert(renderer && ui_init(window, renderer, NULL));
    Editor *e = calloc(1, sizeof(*e));
    assert(e);
    editor_init(e, window, renderer, NULL, "", "", true);
    preferences(e);
    percussion(e);
    SDL_CreateDirectory("agents/reports/updates-2026-10-31");
    updates(e);
    shortcut_upgrade(e);
    gestures(e);
    saves(e);
    generation(e);
    imported_generation();
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
