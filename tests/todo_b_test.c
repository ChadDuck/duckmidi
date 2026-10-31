#include "editor_internal.h"
#include "../shared/pattern_generation.h"
#include <assert.h>
extern void recording_feedback_audio_pause(DmAudio *);
extern void recording_feedback_audio_render(DmAudio *, int);
extern int recording_feedback_audio_port_cc(DmAudio *, unsigned, int, int);
extern int recording_feedback_audio_port_bend(DmAudio *, unsigned, int);
extern bool recording_feedback_audio_port_voice(DmAudio *, unsigned, int, int);
extern void test_todo_b_ui(Editor *);

static void audio_job(DmAudio *a) {
    uint64_t start = SDL_GetTicks();
    char error[512];

    while (dm_audio_busy(a) && SDL_GetTicks() - start < 15000) {
        bool success;
        int kind;

        if (dm_audio_poll(a, error, sizeof(error), &success, &kind)) {
            if (!success) {
                fprintf(stderr, "Audio job: %s\n", error);
            }

            assert(success);
        }

        SDL_Delay(1);
    }

    assert(!dm_audio_busy(a));
}

static bool contains(DmBytes data, const char *text) {
    size_t length = strlen(text);

    for (size_t i = 0; i + length <= data.size; i++) {
        if (!memcmp(data.data + i, text, length)) {
            return true;
        }
    }

    return false;
}

static void names(void) {
    DmProject p;
    dm_project_init(&p);
    int base = dm_pattern_add(&p, "Bass");
    assert(base >= 0);
    int two = dm_pattern_copy(&p, base), three = dm_pattern_copy(&p, two),
        four = dm_pattern_copy(&p, base);
    assert(!strcmp(p.patterns[two].name, "Bass-2") && !strcmp(p.patterns[three].name, "Bass-3") &&
           !strcmp(p.patterns[four].name, "Bass-4"));
    dm_pattern_name(&p, three, "Lead");
    int lead = dm_pattern_copy(&p, three);
    assert(!strcmp(p.patterns[lead].name, "Lead-2"));
    int independent = dm_pattern_add(&p, "Bass-5");
    assert(independent >= 0);
    int next = dm_pattern_copy(&p, base);
    assert(!strcmp(p.patterns[next].name, "Bass-6"));
    DmBytes bytes = {0};
    DmProject q;
    dm_project_init(&q);
    char error[256];
    assert(dm_pack(&p, &bytes) && dm_unpack(&q, bytes.data, bytes.size, error, sizeof(error)));
    assert(!strcmp(q.patterns[next].base_name, "Bass") && q.patterns[next].duplicate_index == 6);
    dm_project_free(&p);
    dm_project_free(&q);
    dm_bytes_free(&bytes);
    puts(
        "PASS tracked pattern bases, sequential duplicate names, rename reset, collisions and persistence");
}

static void routing(Editor *e) {
    ed_new_project(e);
    DmProject *p = &e->project;
    DmPattern *pat = &p->patterns[0];
    pat->count = 0;
    p->clip_count = 0;
    p->automation_count = 0;
    assert(dm_note_add(pat, (DmNote){0, 4, 60, 100, 0, 0}) &&
           dm_note_add(pat, (DmNote){0, 1, 60, 100, 0, 1}) &&
           dm_note_add(pat, (DmNote){0, 2, 67, 100, 0, 127}));
    assert(dm_clip_add(p, (DmClip){.pattern = 0, .channel = 0}));
    const unsigned ports[] = {0, 1, 127};
    const int values[] = {10, 127, 64};

    for (unsigned i = 0; i < 3; i++) {
        assert(dm_automation_add(p, (DmAutomation){.type = 0xb0,
                                                   .channel = 0,
                                                   .a = 11,
                                                   .b = values[i],
                                                   .pattern = 0,
                                                   .port = (uint8_t)ports[i]}));
        assert(dm_automation_add(p, (DmAutomation){.type = 0xe0,
                                                   .channel = 0,
                                                   .b = i == 1 ? 96 : 64,
                                                   .pattern = 0,
                                                   .port = (uint8_t)ports[i]}));
    }

    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration;
    char error[512];
    assert(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)));
    unsigned starts[DM_NOTE_PORTS] = {0};

    for (size_t i = 0; i < count; i++) {
        if ((events[i].status & 0xf0) == 0x90 && events[i].b) {
            starts[events[i].port]++;
        }
    }

    assert(starts[0] == 1 && starts[1] == 1 && starts[127] == 1);
    free(events);
    DmBytes data = {0};
    assert(dm_midi(p, &data, error, sizeof(error)));
    DmProject imported;
    dm_project_init(&imported);
    assert(dm_import(&imported, data.data, data.size, 1, 0, error, sizeof(error)) &&
           imported.port_count == 3 && imported.patterns[0].count == 1);
    assert(dm_view_switch(&imported, 127, 0, error, sizeof(error)) &&
           imported.patterns[0].notes[0].key == 67);
    dm_project_free(&imported);
    dm_bytes_free(&data);
    assert(dm_pack(p, &data));
    dm_project_init(&imported);
    assert(dm_unpack(&imported, data.data, data.size, error, sizeof(error)) &&
           imported.patterns[0].notes[2].port == 127);
    dm_project_free(&imported);
    dm_bytes_free(&data);
    DmProject generated;
    dm_project_init(&generated);
    DmGenerationStats stats;
    assert(dm_generate_patterns(p, &generated, NULL, &stats, error, sizeof(error)));
    assert(dm_export_timeline(&generated, &events, &count, &duration, error, sizeof(error)));
    memset(starts, 0, sizeof(starts));

    for (size_t i = 0; i < count; i++) {
        if ((events[i].status & 0xf0) == 0x90 && events[i].b) {
            starts[events[i].port]++;
        }
    }

    assert(starts[0] == 1 && starts[1] == 1 && starts[127] == 1);
    free(events);
    dm_project_free(&generated);
    assert(dm_audio_publish(e->audio, p, 0, true, error, sizeof(error)));
    recording_feedback_audio_render(e->audio, 256);

    for (unsigned i = 0; i < 3; i++) {
        assert(recording_feedback_audio_port_cc(e->audio, ports[i], 0, 11) == values[i]);
    }

    assert(recording_feedback_audio_port_bend(e->audio, 0, 0) == 8192 &&
           recording_feedback_audio_port_bend(e->audio, 1, 0) == 12288);
    assert(recording_feedback_audio_port_voice(e->audio, 0, 0, 60) &&
           recording_feedback_audio_port_voice(e->audio, 1, 0, 60));
    recording_feedback_audio_render(e->audio, 24000);
    assert(recording_feedback_audio_port_voice(e->audio, 0, 0, 60) &&
           !recording_feedback_audio_port_voice(e->audio, 1, 0, 60));
    assert(dm_audio_publish(e->audio, p, .25, true, error, sizeof(error)));
    recording_feedback_audio_render(e->audio, 128);

    for (unsigned i = 0; i < 3; i++) {
        assert(recording_feedback_audio_port_cc(e->audio, ports[i], 0, 11) == values[i]);
    }

    dm_audio_loop(e->audio, true, .25, .75);
    recording_feedback_audio_render(e->audio, 30000);
    assert(recording_feedback_audio_port_cc(e->audio, 1, 0, 11) == 127 &&
           recording_feedback_audio_port_cc(e->audio, 0, 0, 11) == 10);
    dm_audio_loop(e->audio, false, 0, 0);
    dm_audio_transport(e->audio, false);
    recording_feedback_audio_render(e->audio, 128);
    assert(dm_audio_soundfont(e->audio, "agents/reports/generated-programs.sf2", error,
                              sizeof(error)));
    audio_job(e->audio);
    assert(dm_audio_publish(e->audio, p, 0, true, error, sizeof(error)));
    recording_feedback_audio_render(e->audio, 128);
    assert(recording_feedback_audio_port_cc(e->audio, 127, 0, 11) == 64);
    dm_audio_transport(e->audio, false);
    snprintf(p->soundfont, DM_PATH, "agents/reports/generated-programs.sf2");
    assert(dm_audio_export(e->audio, p, "agents/reports/todo-2026-10-29-b/ports.wav", NULL, error,
                           sizeof(error)));
    audio_job(e->audio);
    e->selected_pattern = e->pattern_channel = 0;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    p->patterns[0].notes[0].selected = 1;
    assert(ed_color_notes(e, 2) && p->patterns[0].notes[0].port == 2);
    ed_undo(e, false);
    assert(p->patterns[0].notes[0].port == 0);
    e->note_ports[0] = 1;
    assert(ed_curve_node(e, 0, 100, 11, 0, 60).port == 1);
    e->note_ports[0] = 0;
    puts(
        "PASS independent ports 0/1/127: MIDI, project/generation persistence, live CC/bend/voices, note releases, seek/loop, font reload, WAV, color Undo and event scope");
}

static void preferences(Editor *e) {
    snprintf(e->settings_path, DM_PATH, "agents/reports/todo-2026-10-29-b/settings.ini");
    e->percussion_global[38] = 1;
    assert(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names));
    DmBytes b = {0};
    char error[256];
    assert(dm_read(e->settings_path, &b, error, sizeof(error)));
    assert(contains(b, "acoustic_snare=\"Z\"") && contains(b, "color_selected_notes=\"Alt + C\"") &&
           contains(b, "schema=3") && !contains(b, "note_38="));
    dm_bytes_free(&b);
    assert(dm_preferences_load(e, ed_shortcut_schema()) && e->percussion_global[38]);
    assert(e->bindings[ACT_PIANO].key == SDL_SCANCODE_F1 &&
           e->bindings[ACT_METRONOME].key == SDL_SCANCODE_F2 &&
           e->bindings[ACT_WAIT].key == SDL_SCANCODE_F3 &&
           e->bindings[ACT_OVERLAP].key == SDL_SCANCODE_F4);
    puts("PASS underscore action labels, named percussion settings and F1-F4 defaults");
}

static void sample(void) {
    DmBytes data = {0};
    char error[512];
    DmProject p;
    dm_project_init(&p);
    assert(dm_read("assets/funky-beat.duck", &data, error, sizeof(error)));

    if (!dm_unpack(&p, data.data, data.size, error, sizeof(error))) {
        fprintf(stderr, "Sample: %s\n", error);
    }

    assert(p.pattern_count > 0 && p.clip_count > 0);
    dm_project_free(&p);
    dm_bytes_free(&data);
    puts("PASS current bundled sample schema and nonempty arrangement");
}

static void copy_fixture(const char *source, const char *target) {
    DmBytes data = {0};
    char error[256];
    assert(dm_read(source, &data, error, sizeof(error)) &&
           dm_write(target, data.data, data.size, error, sizeof(error)));
    dm_bytes_free(&data);
}

static void first_boot(Editor *e) {
    assert(SDL_CreateDirectory("build/development/bundle-fixture") &&
           SDL_CreateDirectory("build/development/bundle-fixture/assets") &&
           SDL_CreateDirectory("build/development/bundle-fixture/projects"));
    remove("build/development/bundle-fixture/duckmidi-settings.ini");
    copy_fixture("assets/funky-beat.duck",
                 "build/development/bundle-fixture/projects/funky-beat.duck");
    /* The supplied arrangement may retain its author's local SoundFont path.
       Keep this test independent of that installation and the real font. */
    DmBytes sample = {0}, packed = {0};
    DmProject project;
    dm_project_init(&project);
    char error[512];
    assert(dm_read("build/development/bundle-fixture/projects/funky-beat.duck", &sample, error,
                   sizeof(error)));
    assert(dm_unpack(&project, sample.data, sample.size, error, sizeof(error)));
    project.soundfont[0] = 0;
    assert(dm_pack(&project, &packed) &&
           dm_write("build/development/bundle-fixture/projects/funky-beat.duck", packed.data,
                    packed.size, error, sizeof(error)));
    dm_project_free(&project);
    dm_bytes_free(&sample);
    dm_bytes_free(&packed);
    copy_fixture("agents/reports/generated-programs.sf2",
                 "build/development/bundle-fixture/assets/TimGM6mb.sf2");
    Editor *boot = calloc(1, sizeof(*boot));
    assert(boot);
    editor_init(boot, e->window, e->renderer, e->audio, "build/development/bundle-fixture/", false);
    audio_job(e->audio);
    assert(boot->active && boot->project.pattern_count > 0 &&
           strstr(boot->preferred_font, "assets/TimGM6mb.sf2") &&
           strstr(boot->project.soundfont, "bundle-fixture/assets/TimGM6mb.sf2") &&
           strstr(boot->path, "projects/funky-beat.duck"));
    editor_free(boot);
    free(boot);
    boot = calloc(1, sizeof(*boot));
    assert(boot);
    editor_init(boot, e->window, e->renderer, e->audio, "build/development/bundle-fixture/", false);
    audio_job(e->audio);
    assert(!boot->active && strstr(boot->preferred_font, "assets/TimGM6mb.sf2"));
    editor_free(boot);
    free(boot);
    puts(
        "PASS isolated production first boot loads supplied project/default font once; subsequent launch retains preferences without reopening sample");
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO));
    SDL_Window *window = SDL_CreateWindow("Todo B validation", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    assert(window && renderer && ui_init(window, renderer, NULL));
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    assert(audio);
    recording_feedback_audio_pause(audio);
    assert(
        dm_audio_soundfont(audio, "agents/reports/generated-programs.sf2", error, sizeof(error)));
    audio_job(audio);
    Editor *e = calloc(1, sizeof(*e));
    assert(e);
    editor_init(e, window, renderer, audio, "", true);
    names();
    sample();
    routing(e);
    preferences(e);
    test_todo_b_ui(e);
    first_boot(e);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
