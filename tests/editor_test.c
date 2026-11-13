#include "editor/internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <windows.h>
#define TEST_NAME "editor"
#include "test_common.h"
static int checks;
void test_editor_mouse(Editor *e, void (*check)(bool, const char *));
void test_editor_updates(Editor *e, void (*check)(bool, const char *));

static void press(Editor *e, SDL_Scancode key, SDL_Keymod mods) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.down = true;
    event.key.scancode = key;
    event.key.mod = mods;
    editor_event(e, &event);
}

static void frame(Editor *e) {
    ui_frame();
    editor_frame(e, 1400, 900);
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_RenderPresent(e->renderer);
}

static void notes(Editor *e) {
    e->edit_focus = FOCUS_NOTES;
    e->selected_pattern = 0;

    for (size_t i = 0; i < e->project.patterns[0].count; ++i) {
        e->project.patterns[0].notes[i].selected = 0;
    }

    e->project.patterns[0].notes[0].selected = 1;
    int pitch = e->project.patterns[0].notes[0].key;
    press(e, SDL_SCANCODE_UP, SDL_KMOD_SHIFT);
    check(e->project.patterns[0].notes[0].key == pitch + 1, "semitone transpose");
    press(e, SDL_SCANCODE_UP, SDL_KMOD_CTRL);
    check(e->project.patterns[0].notes[0].key == pitch + 13, "octave transpose");
    size_t count = e->project.patterns[0].count;
    press(e, SDL_SCANCODE_DELETE, 0);
    check(e->project.patterns[0].count == count - 1, "delete note");
    press(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    check(e->project.patterns[0].count == count, "undo note deletion");
    press(e, SDL_SCANCODE_Y, SDL_KMOD_CTRL);
    check(e->project.patterns[0].count == count - 1, "redo note deletion");
    press(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    e->project.patterns[0].notes[0].selected = 1;
    press(e, SDL_SCANCODE_C, SDL_KMOD_CTRL);
    press(e, SDL_SCANCODE_V, SDL_KMOD_CTRL);
    check(e->project.patterns[0].count == count + 1, "copy and paste notes");
}

static void clips(Editor *e) {
    e->edit_focus = FOCUS_PLAYLIST;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        e->project.clips[i].selected = 0;
    }

    e->project.clips[0].selected = 1;
    size_t count = e->project.clip_count;
    press(e, SDL_SCANCODE_C, SDL_KMOD_CTRL);
    press(e, SDL_SCANCODE_V, SDL_KMOD_CTRL);
    check(e->project.clip_count == count + 1, "copy and paste clips");
    press(e, SDL_SCANCODE_DELETE, 0);
    check(e->project.clip_count == count, "delete pasted clip");
    press(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    check(e->project.clip_count == count + 1, "undo clip deletion");
}

static int pattern_with_uid(const DmProject *p, uint32_t uid) {
    for (size_t i = 0; i < p->pattern_count; ++i) {
        if (p->patterns[i].uid == uid) {
            return (int)i;
        }
    }

    return -1;
}

static void note_pattern_shortcuts(Editor *e) {
    DmProject *p = &e->project;
    size_t count = p->pattern_count;
    uint32_t ids[3];

    ed_choose_pattern_card(e, 0, false);
    press(e, SDL_SCANCODE_KP_PLUS, 0);
    check(e->selected_pattern == 1 && p->pattern_count == count,
          "plus selects the next edited note pattern");
    press(e, SDL_SCANCODE_KP_MINUS, 0);
    check(e->selected_pattern == 0 && p->pattern_count == count,
          "minus selects the previous edited note pattern");
    press(e, SDL_SCANCODE_KP_PLUS, 0);

    for (int i = 0; i < 3; ++i) {
        press(e, SDL_SCANCODE_KP_PLUS, 0);
        const DmPattern *pattern = ed_pattern(e);
        char name[64];
        snprintf(name, sizeof(name), "Pattern %d", i + 1);
        check(p->pattern_count == count + i + 1 && !e->automation_browser && pattern &&
                  !pattern->kind && !strcmp(pattern->name, name),
              "keypad plus creates consecutive empty note patterns");
        ids[i] = pattern->uid;
    }

    int second = pattern_with_uid(p, ids[1]);
    check(dm_note_add(&p->patterns[second], (DmNote){.length = .5, .key = 60, .velocity = 100}),
          "edit note pattern 2");
    ed_commit(e);
    press(e, SDL_SCANCODE_KP_MINUS, 0);
    check(p->pattern_count == count + 2 && pattern_with_uid(p, ids[2]) < 0,
          "minus removes the newest empty note pattern");
    press(e, SDL_SCANCODE_KP_MINUS, 0);
    check(p->pattern_count == count + 1 && pattern_with_uid(p, ids[0]) < 0 && ed_pattern(e) &&
              ed_pattern(e)->uid == ids[1] && ed_pattern(e)->count == 1,
          "minus skips the note pattern with content and removes the older empty one");
    press(e, SDL_SCANCODE_KP_PLUS, 0);
    check(ed_pattern(e) && !strcmp(ed_pattern(e)->name, "Pattern 1") &&
              e->selected_pattern < pattern_with_uid(p, ids[1]),
          "a new note pattern reuses number 1 ahead of the surviving pattern 2");
    press(e, SDL_SCANCODE_KP_MINUS, 0);
}

static void automation_shortcuts(Editor *e) {
    DmProject *p = &e->project;
    size_t count = p->pattern_count;
    uint32_t ids[10];

    for (int i = 0; i < 10; ++i) {
        press(e, SDL_SCANCODE_KP_PLUS, i % 2 ? 0 : SDL_KMOD_SHIFT);
        DmPattern *pattern = ed_pattern(e);
        char name[64];
        snprintf(name, sizeof(name), "Automation %d", i + 1);
        check(p->pattern_count == count + i + 1 && e->automation_browser && pattern &&
                  pattern->kind && !strcmp(pattern->name, name),
              "each keypad plus creates and selects another numbered automation pattern");
        ids[i] = pattern->uid;
    }

    ed_choose_pattern_card(e, pattern_with_uid(p, ids[7]), false);
    ed_checkpoint(e);
    check(dm_automation_add(p, (DmAutomation){.beat = 1,
                                              .type = MIDI_PITCH_BEND,
                                              .b = 90,
                                              .pattern = e->selected_pattern}),
          "edit automation pattern 8");
    ed_commit(e);
    ed_choose_pattern_card(e, pattern_with_uid(p, ids[9]), false);

    for (int i = 9; i >= 0; --i) {
        if (i == 7) {
            continue;
        }

        size_t before = p->pattern_count;
        press(e, SDL_SCANCODE_KP_MINUS, i % 2 ? 0 : SDL_KMOD_SHIFT);
        check(p->pattern_count == before - 1 && pattern_with_uid(p, ids[i]) < 0,
              "each keypad minus removes one empty automation pattern in creation order");
        check(pattern_with_uid(p, ids[7]) >= 0, "keypad minus preserves pattern 8");
    }

    press(e, SDL_SCANCODE_KP_MINUS, 0);
    check(p->pattern_count == count + 1 && ed_pattern(e)->uid == ids[7],
          "keypad minus leaves the last populated pattern selected");

    press(e, SDL_SCANCODE_KP_PLUS, 0);
    int first = e->selected_pattern;
    check(!strcmp(ed_pattern(e)->name, "Automation 1") && first < pattern_with_uid(p, ids[7]),
          "numbering restarts at 1 and inserts the card before pattern 8");
    press(e, SDL_SCANCODE_KP_PLUS, SDL_KMOD_SHIFT);
    int second = e->selected_pattern;
    int eighth = pattern_with_uid(p, ids[7]);
    check(!strcmp(ed_pattern(e)->name, "Automation 2") && first < second && second < eighth &&
              !strcmp(p->patterns[eighth].name, "Automation 8"),
          "another new card fills the next gap without renaming the survivor");
    bool point_preserved = false;

    for (size_t i = 0; i < p->automation_count; ++i) {
        DmAutomation point = p->automation[i];
        point_preserved |= point.pattern == eighth && point.type == MIDI_PITCH_BEND &&
                           point.beat == 1 && point.b == 90;
    }

    check(point_preserved, "inserting before pattern 8 preserves its point");
    p->patterns[first].mode = 1;
    p->lane_count[0] = 1;
    snprintf(p->lane_names[0][0], sizeof(p->lane_names[0][0]), "Pitch bend");

    check(dm_clip_add(p, (DmClip){.pattern = second, .channel = 0, .lane = 1}),
          "place an empty automation pattern on the playlist");
    ed_commit(e);
    press(e, SDL_SCANCODE_KP_PLUS, SDL_KMOD_SHIFT);
    check(e->selected_pattern == eighth && p->pattern_count == count + 3,
          "plus selects the next edited automation pattern");
    press(e, SDL_SCANCODE_KP_MINUS, SDL_KMOD_SHIFT);
    check(e->selected_pattern == second && p->pattern_count == count + 3,
          "minus selects the previous edited automation pattern when none are empty");
    press(e, SDL_SCANCODE_KP_PLUS, 0);
    press(e, SDL_SCANCODE_KP_PLUS, 0);
    uint32_t newest = ed_pattern(e)->uid;
    press(e, SDL_SCANCODE_KP_MINUS, 0);
    check(pattern_with_uid(p, newest) < 0, "minus removes a new card inserted before pattern 8");
    press(e, SDL_SCANCODE_KP_MINUS, SDL_KMOD_SHIFT);
    check(p->pattern_count == count + 3 && p->patterns[first].mode &&
              p->clips[p->clip_count - 1].pattern == second,
          "minus preserves LFOs and placed automation patterns");
    press(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
    check(pattern_with_uid(p, newest) >= 0, "undo restores the removed empty card");
    press(e, SDL_SCANCODE_Y, SDL_KMOD_CTRL);
    check(pattern_with_uid(p, newest) < 0, "redo removes the empty card again");
}

static void test_shortcuts(Editor *e) {
    press(e, SDL_SCANCODE_F1, 0);
    check(e->piano, "keyboard piano enabled");
    e->tool = TOOL_DELETE;
    press(e, SDL_SCANCODE_1, 0);
    check(e->tool == TOOL_DELETE, "piano suppresses tool keys");
    press(e, SDL_SCANCODE_K, 0);
    check(!e->shortcuts, "piano suppresses letter shortcuts");
    press(e, SDL_SCANCODE_F1, 0);
    check(!e->piano, "keyboard piano disabled");
    press(e, SDL_SCANCODE_K, 0);
    check(e->shortcuts, "open shortcut window");
    frame(e);
    e->about = true;
    frame(e);
    press(e, SDL_SCANCODE_ESCAPE, 0);
    frame(e);
    check(!e->about && e->shortcuts, "Escape closes topmost window");
    frame(e);
    press(e, SDL_SCANCODE_ESCAPE, 0);
    frame(e);
    check(!e->shortcuts && e->running, "Escape closes floating window without quitting");
    note_pattern_shortcuts(e);
    automation_shortcuts(e);

    /* Start below the undo limit so the controller tests can count new steps. */
    ed_clear_history(e->undo, &e->undo_count);

    for (int mode = 0; mode < 2; ++mode) {
        ed_choose_pattern_card(e, (int)e->project.pattern_count - 1, false);
        press(e, SDL_SCANCODE_KP_PLUS, SDL_KMOD_SHIFT);
        frame(e);
        DmPattern *automation = ed_pattern(e);
        check(automation && automation->kind, "automation pattern for controller undo");
        automation->mode = mode;
        e->edit_focus = FOCUS_EVENTS;
        int before = automation->event, history = e->undo_count;
        e->automation_cc = before == 1 ? 7 : 1;
        int chosen = e->automation_cc;
        frame(e);
        check(ed_pattern(e)->event == chosen && e->undo_count == history + 1,
              "switching an automation pattern's controller is an undo step");
        press(e, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
        frame(e);
        check(ed_pattern(e)->event == before && e->automation_cc == before && e->redo_count,
              "undo restores the automation pattern's controller");
        press(e, SDL_SCANCODE_Y, SDL_KMOD_CTRL);
        frame(e);
        check(ed_pattern(e)->event == chosen && e->automation_cc == chosen,
              "redo switches the automation pattern's controller again");
        press(e, SDL_SCANCODE_KP_MINUS, 0);
        frame(e);
    }
}

/* The pattern editor's instrument follows program changes at the current position. */
static void placed_instrument(Editor *e) {
    int index = -1;

    for (size_t i = 0; i < e->project.clip_count && index < 0; ++i) {
        DmClip c = e->project.clips[i];

        if (!c.lane && !e->project.patterns[c.pattern].kind &&
            !dm_channel_drums(&e->project, c.channel)) {
            index = (int)i;
        }
    }

    check(index >= 0, "melodic placement for instrument display");
    DmClip clip = e->project.clips[index];
    ed_open_pattern(e, clip.pattern, false);
    ed_select_placement_route(e, index);
    double change = clip.beat + clip.offset + 1;
    check(dm_automation_add(&e->project, (DmAutomation){change, MIDI_PROGRAM, clip.channel, 113, 0,
                                                        -1, 0, 0, 0, false, 0, false}),
          "add song program change inside the placement");
    e->pattern_mode = false;
    e->cursor_beat[0] = change - .5;
    check(ed_placed_program(e) != 113, "instrument before the program change");
    e->cursor_beat[0] = change;
    check(ed_placed_program(e) == 113, "instrument follows the song position's program change");
    e->pattern_mode = true;
    e->cursor_beat[1] = change - ed_pattern_region_origin(e);
    check(ed_placed_program(e) == 113, "instrument follows the program change in pattern playback");
    e->pattern_mode = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (a.pattern < 0 && a.type == MIDI_PROGRAM && a.a == 113 && a.beat == change) {
            memmove(e->project.automation + i, e->project.automation + i + 1,
                    (e->project.automation_count - i - 1) * sizeof(DmAutomation));
            --e->project.automation_count;
            break;
        }
    }
}

static void save_load(Editor *e) {
    snprintf(e->project.name, sizeof(e->project.name), "Editor validation");
    snprintf(e->path, sizeof(e->path), "agents/reports/editor-validation.duck");
    e->dirty = true;
    press(e, SDL_SCANCODE_S, SDL_KMOD_CTRL);
    frame(e);
    uint64_t save_start = SDL_GetTicks();

    while (ed_save_busy(e) && SDL_GetTicks() - save_start < 10000) {
        ed_save_poll(e);
        SDL_Delay(1);
    }

    check(!e->dirty, "Ctrl+S saves project");
    snprintf(e->project.name, sizeof(e->project.name), "Changed in isolated test");
    SDL_Event drop;
    SDL_zero(drop);
    drop.type = SDL_EVENT_DROP_FILE;
    drop.drop.data = "agents/reports/editor-validation.duck";
    editor_event(e, &drop);
    check(!strcmp(e->project.name, "Editor validation"), "drop loads saved project");
    check(!e->config, "project load bypasses import configuration");
}

static void grid_shades(Editor *e) {
    ed_new_project(e);
    uint32_t bar = ed_grid_color(e, 0), beat = ed_grid_color(e, 1), quarter = ed_grid_color(e, .25);
    check((bar & 0xff) > (beat & 0xff) && (beat & 0xff) > (quarter & 0xff),
          "beat lines sit between bar and quarter-beat lines");
    check(ed_grid_color(e, 4) == bar, "every bar uses the bar shade");
}

static void save_fixture(Editor *e) {
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
    e->drag = DRAG_NONE;
    e->tool = TOOL_SELECT;
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

/* A save writes the snapshot taken when it started; later edits keep the project unsaved. */
static void worker_saves(Editor *e) {
    save_fixture(e);

    for (int i = 1; i < 20000; i++) {
        assert(dm_note_add(ed_pattern(e), (DmNote){i * .25, .125, (uint8_t)(48 + i % 24), 90, 0}));
    }

    ed_changed(e);
    const char *path = "agents/reports/editor-save-snapshot.duck";
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

static void wait_autosave(Editor *e) {
    uint64_t start = SDL_GetTicks();

    while (ed_autosave_busy(e) && SDL_GetTicks() - start < 10000) {
        ed_save_poll(e);
        SDL_Delay(1);
    }

    check(!ed_autosave_busy(e), "autosave worker finishes");
}

static void autosaves(Editor *e) {
    save_fixture(e);
    SDL_Time time;
    check(SDL_GetCurrentTime(&time), "autosave fixture timestamp");
    char *cwd = SDL_GetCurrentDirectory();
    char settings[DM_PATH], folder[DM_PATH], marker[DM_PATH], original[DM_PATH];
    snprintf(settings, sizeof(settings), "%sagents/reports/autosave-%" PRId64, cwd, (int64_t)time);
    SDL_free(cwd);
    snprintf(folder, sizeof(folder), "%s/projects/auto", settings);
    snprintf(marker, sizeof(marker), "%s/session.lock", folder);
    snprintf(original, sizeof(original), "%s/original.duck", settings);
    snprintf(e->path, sizeof(e->path), "%s", original);
    ed_changed(e);
    char recents[sizeof(e->recent)];
    memcpy(recents, e->recent, sizeof(recents));
    ed_autosave_init(e, settings);
    check(e->autosave != NULL, "autosave initializes in isolated settings");
    SDL_PathInfo info;
    check(SDL_GetPathInfo(marker, &info), "session marker exists");
    ed_autosave_step(e, SDL_GetTicks());
    check(!ed_autosave_busy(e), "autosave waits until due");
    uint64_t due = SDL_GetTicks() + ED_AUTOSAVE_INTERVAL_MS;
    e->active = false;
    ed_autosave_step(e, due);
    check(!ed_autosave_busy(e), "closed project is not autosaved");
    e->active = true;
    e->dirty = false;
    ed_autosave_step(e, due += ED_AUTOSAVE_INTERVAL_MS);
    check(!ed_autosave_busy(e), "clean project is not autosaved");
    e->dirty = true;
    ed_autosave_step(e, due += ED_AUTOSAVE_INTERVAL_MS);
    check(ed_autosave_busy(e) && !ed_save_busy(e), "autosave uses its own job slot");
    wait_autosave(e);
    check(e->dirty && !strcmp(e->path, original),
          "autosave preserves dirty state and original path");
    check(!memcmp(recents, e->recent, sizeof(recents)), "autosave preserves recent files");
    int count = 0;
    char **files = SDL_GlobDirectory(folder, "*.duck", 0, &count);
    check(files && count == 1, "autosave lands in projects/auto");
    char latest[DM_PATH], prefix[256];
    snprintf(latest, sizeof(latest), "%s/%s", folder, files[0]);
    size_t prefix_size = strlen(files[0]) - 25;
    check(prefix_size < sizeof(prefix), "autosave has a timestamped name");
    memcpy(prefix, files[0], prefix_size);
    prefix[prefix_size] = 0;
    SDL_free(files);
    DmBytes data = {0};
    char error[512];
    check(dm_read(latest, &data, error, sizeof(error)), "autosave can be read");
    DmProject copy;
    dm_project_init(&copy);
    check(dm_unpack(&copy, data.data, data.size, error, sizeof(error)) &&
              copy.patterns[0].notes[0].velocity == 100,
          "autosave contains the project snapshot");
    dm_project_free(&copy);

    for (int i = 1; i <= 7; ++i) {
        char path[DM_PATH];
        snprintf(path, sizeof(path), "%s/%s_2000-01-%02d_00-00-00.duck", folder, prefix, i);
        check(dm_write(path, data.data, data.size, error, sizeof(error)), "seed old autosave");
    }

    dm_bytes_free(&data);
    ed_autosave_step(e, due += ED_AUTOSAVE_INTERVAL_MS);
    check(!ed_autosave_busy(e), "unchanged revision is not autosaved again");
    e->project.patterns[0].notes[0].velocity = 47;
    ed_changed(e);
    ed_autosave_step(e, due += ED_AUTOSAVE_INTERVAL_MS);
    check(ed_autosave_busy(e), "new revision starts an autosave");
    check(ed_save_to(e, original), "manual save can start while autosave runs");
    wait_autosave(e);
    wait_save(e);
    check(ed_save_to(e, original), "start independent manual save");
    ed_changed(e);
    ed_autosave_step(e, due += ED_AUTOSAVE_INTERVAL_MS);
    check(ed_save_busy(e) && ed_autosave_busy(e), "autosave can start while manual save runs");
    wait_autosave(e);
    wait_save(e);
    files = SDL_GlobDirectory(folder, "*.duck", 0, &count);
    check(files && count == ED_AUTOSAVE_KEEP, "autosave pruning keeps the newest five");

    for (int i = 0; i < count; ++i) {
        char path[DM_PATH];
        snprintf(path, sizeof(path), "%s/%s", folder, files[i]);

        if (strcmp(path, latest) > 0) {
            snprintf(latest, sizeof(latest), "%s", path);
        }
    }

    SDL_free(files);
    memcpy(recents, e->recent, sizeof(recents));
    char oldest[DM_PATH];
    snprintf(oldest, sizeof(oldest), "%s/%s_2000-01-01_00-00-00.duck", folder, prefix);
    check(!SDL_GetPathInfo(oldest, &info), "autosave pruning removes oldest snapshots");
    ed_autosave_free(e);
    check(!SDL_GetPathInfo(marker, &info), "clean exit removes its marker");

    char marker_text[DM_PATH * 2 + 128];
    int n = snprintf(marker_text, sizeof(marker_text), "4294967295\n1\n%s\n%s\n", latest, original);
    check(dm_write(marker, marker_text, (size_t)n, error, sizeof(error)),
          "seed dead-session marker");
    char logs[DM_PATH], crash[DM_PATH];
    snprintf(logs, sizeof(logs), "%s/logs", settings);
    check(SDL_CreateDirectory(logs), "create isolated crash logs");
    snprintf(crash, sizeof(crash), "%s/crash-2026-11-12_00-00-00.txt", logs);
    check(dm_write(crash, "generated crash fixture\n", 24, error, sizeof(error)),
          "seed crash report");
    ed_autosave_init(e, settings);
    const char *question = ed_autosave_question(e);
    check(question && strstr(question, "closed unexpectedly") &&
              strstr(question, "original.duck") && strstr(question, crash),
          "dead session offers recovery and includes the crash report path");
    e->project.patterns[0].notes[0].velocity = 12;
    check(ed_autosave_restore(e), "restore stale session autosave");
    check(e->dirty && !strcmp(e->path, original) && e->project.patterns[0].notes[0].velocity == 47,
          "restore loads snapshot, restores original path and marks project dirty");
    check(!memcmp(recents, e->recent, sizeof(recents)),
          "recovery does not add autosave to recents");
    ed_autosave_free(e);

    n = snprintf(marker_text, sizeof(marker_text), "%lu\n1\n%s\n%s\n",
                 (unsigned long)GetCurrentProcessId(), latest, original);
    check(dm_write(marker, marker_text, (size_t)n, error, sizeof(error)),
          "seed live-session marker");
    ed_autosave_init(e, settings);
    check(!ed_autosave_question(e), "live session does not offer recovery");
    ed_autosave_free(e);
    check(dm_read(marker, &data, error, sizeof(error)) && data.size == (size_t)n &&
              !memcmp(data.data, marker_text, (size_t)n),
          "second instance leaves the live session marker intact");
    dm_bytes_free(&data);
    check(SDL_RemovePath(marker), "remove generated live-session fixture");

    n = snprintf(marker_text, sizeof(marker_text), "4294967295\n1\n%s\n\n", latest);
    check(dm_write(marker, marker_text, (size_t)n, error, sizeof(error)), "seed untitled recovery");
    ed_autosave_init(e, settings);
    check(ed_autosave_restore(e) && !*e->path && e->dirty,
          "untitled recovery keeps an empty save path and is dirty");
    ed_autosave_free(e);

    char failed_settings[DM_PATH], failed_folder[DM_PATH], failed_marker[DM_PATH];
    snprintf(failed_settings, sizeof(failed_settings), "%s/failure", settings);
    snprintf(failed_folder, sizeof(failed_folder), "%s/projects/auto", failed_settings);
    snprintf(failed_marker, sizeof(failed_marker), "%s/session.lock", failed_folder);
    ed_autosave_init(e, failed_settings);
    check(SDL_RemovePath(failed_marker) && SDL_RemovePath(failed_folder),
          "isolate autosave destination failure");
    check(dm_write(failed_folder, "blocked", 7, error, sizeof(error)),
          "replace isolated autosave destination with a file");
    ed_autosave_step(e, SDL_GetTicks() + ED_AUTOSAVE_INTERVAL_MS);
    wait_autosave(e);
    check(strstr(e->status, "Autosave failed") && e->dirty && !*e->path,
          "autosave failure reports once and preserves the unsaved project");
    uint64_t status_time = e->status_time;
    ed_save_poll(e);
    check(e->status_time == status_time, "finished autosave failure is not reported again");
    ed_autosave_free(e);
}

int main(void) {
    Editor *e = test_editor_open("duckmidi validation", (TestDesktop){0});
    editor_smoke(e);
    frame(e);
    test_editor_mouse(e, check);
    notes(e);
    clips(e);
    test_shortcuts(e);
    placed_instrument(e);
    save_load(e);
    test_editor_updates(e, check);
    grid_shades(e);
    worker_saves(e);
    autosaves(e);
    editor_smoke(e);
    frame(e);
    printf("PASS editor: %d checks (editing, clipboard, undo/redo, shortcut context, Escape, "
           "save/drop)\n",
           checks);
    test_editor_close(e);
    return 0;
}
