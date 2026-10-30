#include "editor_internal.h"
extern void test_pattern_workflow_ui(Editor *, void (*)(bool, const char *));
static int checks;
static void check(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL pattern_workflow: %s\n", text);
        exit(1);
    }
}
extern void recording_feedback_audio_pause(DmAudio *);
extern void recording_feedback_audio_render(DmAudio *, int);
extern int recording_feedback_audio_preset(DmAudio *, int, bool);
static void fixture(Editor *e) {
    ed_release_piano(e);
    editor_smoke(e);
    e->project.clip_count = e->project.automation_count = 0;
    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        e->project.patterns[i].count = 0;
        e->project.patterns[i].length = 1;
    }
    e->selected_pattern = 0;
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->project.patterns[0].kind = 0;
    e->tool = 0;
    e->pattern_mode = false;
    e->record = e->recording_take = e->take_snapshot = e->take_started = false;
    e->automation_node = -1;
    e->automation_all = false;
    e->event_selection.size = e->event_drag.size = 0;
    e->automation_cc = 11;
    e->automation_raw = false;
    e->overwrite = true;
    e->preview_program = 0;
    e->preview_override = true;
    e->selected_clip = -1;
    e->drag = 0;
    e->edit_moved = false;
}
static void sizing_names(Editor *e) {
    fixture(e);
    DmPattern *p = ed_pattern(e);
    check(p->length == 1, "empty pattern is one beat");
    dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0});
    dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 1, .length = .5});
    dm_note_add(p, (DmNote){0, 1, 60, 100, 0});
    ed_changed(e);
    check(p->length == 1, "exact beat boundary does not add another beat");
    dm_note_add(p, (DmNote){3.5, 1, 64, 100, 0});
    ed_changed(e);
    check(p->length == 5 && dm_clip_length(&e->project, e->project.clips[0]) == 5 &&
              dm_clip_length(&e->project, e->project.clips[1]) == .5,
          "auto placement grows, explicit placement stays fixed");
    p->count = 1;
    ed_changed(e);
    check(p->length == 1, "removal shrinks auto size");
    p->count = 0;
    ed_changed(e);
    check(p->length == 1, "cleared pattern retains one-beat minimum");
    dm_automation_add(&e->project, (DmAutomation){20, 0xe0, 0, 0, 64, 0, 0, 0, 0, false});
    ed_changed(e);
    check(p->length == 1 && e->project.automation_count == 1,
          "trailing controller data stays stored without extending automatic note placements");
    int base = dm_pattern_add(&e->project, "Pattern 2");
    int a = dm_pattern_copy(&e->project, base), b = dm_pattern_copy(&e->project, base),
        c = dm_pattern_copy(&e->project, a);
    char expected[64];
    snprintf(expected, 64, "%s-2", e->project.patterns[base].name);
    check(!strcmp(e->project.patterns[a].name, expected), "first duplicate gets suffix two");
    snprintf(expected, 64, "%s-3", e->project.patterns[base].name);
    check(!strcmp(e->project.patterns[b].name, expected), "next duplicate gets suffix three");
    snprintf(expected, 64, "%s-4", e->project.patterns[base].name);
    check(!strcmp(e->project.patterns[c].name, expected),
          "duplicate of duplicate increments its original name family");
    check(e->project.patterns[a].uid != e->project.patterns[base].uid,
          "pattern copies have independent identities");
    DmBytes bytes = {0};
    DmProject loaded;
    dm_project_init(&loaded);
    char error[256];
    check(dm_pack(&e->project, &bytes) &&
              dm_unpack(&loaded, bytes.data, bytes.size, error, sizeof(error)),
          "schema six roundtrip");
    check(loaded.patterns[a].uid == e->project.patterns[a].uid, "stable identity survives saving");
    dm_bytes_free(&bytes);
    uint32_t saved = e->project.patterns[a].uid;
    e->project.patterns[a].uid = e->project.patterns[base].uid;
    check(dm_pack(&e->project, &bytes) &&
              !dm_unpack(&loaded, bytes.data, bytes.size, error, sizeof(error)),
          "duplicate pattern identities are rejected");
    e->project.patterns[a].uid = saved;
    dm_bytes_free(&bytes);
    check(dm_pack(&e->project, &bytes), "pack current identity fixture");
    bytes.data[8] = 5;
    check(!dm_unpack(&loaded, bytes.data, bytes.size, error, sizeof(error)),
          "previous project schema rejected");
    dm_bytes_free(&bytes);
    dm_project_free(&loaded);
}
static void brush_velocity(Editor *e) {
    fixture(e);
    DmPattern *p = ed_pattern(e);
    View v = {1, 0, 1340, 0};
    UiRect grid = {100, 100, 500, 300};
    UiMouse m = {.clicked = true, .left = true, .shift = true, .pos = {105, 110}};
    int history = e->undo_count;
    ed_brush_begin(e, p, grid, &v, 80, 20, m);
    m.clicked = false;
    m.pos = (UiPoint){205, 70};
    ed_brush_drag(e, p, grid, &v, 80, 20, m);
    check(p->count >= 6 && e->undo_count == history + 1,
          "brush fills crossed cells in one undo step");
    for (size_t i = 0; i < p->count; ++i) {
        check(p->notes[i].length == .25 &&
                  fabs(p->notes[i].beat * 4 - round(p->notes[i].beat * 4)) < 1e-8,
              "brush notes are quarter-beat quantized");
    }
    m.left = false;
    m.released = true;
    ed_brush_drag(e, p, grid, &v, 80, 20, m);
    check(!e->drag, "brush release commits stroke");
    p->count = 0;
    m = (UiMouse){.clicked = true, .left = true, .shift = true, .ctrl = true, .pos = {105, 110}};
    ed_brush_begin(e, p, grid, &v, 80, 20, m);
    m.clicked = false;
    m.pos = (UiPoint){205, 10};
    ed_brush_drag(e, p, grid, &v, 80, 20, m);
    for (size_t i = 0; i < p->count; ++i) {
        check(p->notes[i].key == 60, "Ctrl brush keeps starting pitch");
    }
    p->count = 0;
    dm_note_add(p, (DmNote){0, 1, 60, 50, 1});
    dm_note_add(p, (DmNote){1, 1, 64, 75, 1});
    m = (UiMouse){.alt = true, .wheel = -1, .pos = {120, 110}};
    check(ed_note_velocity_wheel(e, p, (UiRect){36, 72, 564, 328}, &v, 64, 20, 80, m, true),
          "Alt-wheel consumed over note");
    check(p->notes[0].velocity == 45 && p->notes[1].velocity == 70,
          "selected velocities change relatively by five");
    check(ed_velocity_delta(p, 0, 100, p->notes) == 57 && p->notes[0].velocity == 102 &&
              p->notes[1].velocity == 127,
          "velocity group clamps delta without collapsing differences");
    e->automation_cc = -3;
    e->edit_focus = FOCUS_EVENTS;
    p->notes[0].selected = p->notes[1].selected = 0;
    ed_select_all(e);
    check(p->notes[0].selected && p->notes[1].selected, "velocity Ctrl+A selects every note value");
}
static void linked_copy(Editor *e) {
    fixture(e);
    e->context = 1;
    e->edit_focus = FOCUS_PLAYLIST;
    uint32_t uid = e->project.patterns[1].uid;
    dm_clip_add(&e->project,
                (DmClip){.beat = 4, .pattern = 1, .channel = 2, .length = 2.5, .selected = 1});
    ed_clipboard(e, 0);
    dm_pattern_remove(&e->project, 0);
    e->selected_pattern = 0;
    dm_pattern_name(&e->project, 0, "Renamed linked source");
    e->cursor_beat[0] = 8;
    ed_paste(e);
    check(e->project.pattern_count == 1 && e->project.patterns[0].uid == uid,
          "paste links source after rename and index renumber");
    check(e->project.clip_count == 2 && e->project.clips[1].pattern == 0 &&
              e->project.clips[1].length == 2.5 && e->project.clips[1].beat == 8,
          "paste preserves placement length and destination beat");
    size_t clips = e->project.clip_count;
    ed_duplicate_clips(e);
    check(e->project.clip_count == clips + 1 && e->project.pattern_count == 1,
          "Ctrl+D duplicates linked placement without cloning notes");
    check(e->project.clips[clips].length == 2.5 && e->project.clips[clips].beat == 11,
          "duplicate keeps independent length and advances by occupied span");
    e->project.clips[clips].length = .75;
    check(e->project.clips[1].length == 2.5, "copied lengths are independent");
    ed_checkpoint(e);
    int created = dm_pattern_add(&e->project, "Undo candidate");
    uint32_t created_uid = e->project.patterns[created].uid;
    ed_commit(e);
    ed_undo(e, false);
    int replacement = dm_pattern_add(&e->project, "Replacement");
    check(e->project.patterns[replacement].uid > created_uid,
          "undo followed by new creation cannot reuse clipboard identities");
    ++e->project_session;
    size_t patterns = e->project.pattern_count;
    ed_paste(e);
    check(e->project.pattern_count == patterns + 1 &&
              e->project.clips[e->project.clip_count - 1].pattern == (int)patterns,
          "paste into another project session clones content instead of linking equal ids");
}
static void recording(Editor *e) {
    fixture(e);
    e->record = true;
    e->recording_take = true;
    dm_clip_add(&e->project, (DmClip){.beat = 120, .pattern = 0, .channel = 0});
    e->selected_clip = 0;
    check(ed_recording_origin(e, 120.25) == 120 && ed_recording_origin(e, 124) == 120,
          "recording origin follows placement even beyond its current size");
    char error[256];
    dm_audio_publish(e->audio, &e->project, 61, false, error, sizeof(error));
    e->record_start[60] = 120.25;
    e->record_origin[60] = 120;
    ed_record_note(e, 60);
    check(ed_pattern(e)->count == 1 && fabs(ed_pattern(e)->notes[0].beat - .25) < 1e-8 &&
              fabs(ed_pattern(e)->notes[0].length - 1.75) < .01,
          "late-song recording stores pattern-local beat and played duration");
    check(ed_pattern(e)->length == 2, "recording extends only local content span");
    e->pattern_mode = true;
    check(ed_recording_origin(e, 120) == 0, "Pattern-mode recording already uses local beats");
}
static void group_points(Editor *e) {
    fixture(e);
    e->edit_focus = FOCUS_EVENTS;
    dm_automation_add(&e->project, (DmAutomation){1, 0xb0, 0, 11, 30, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){3, 0xb0, 0, 11, 60, 0, 0, 0, 0, false});
    dm_bytes(&e->event_selection, e->project.automation, 2 * sizeof(DmAutomation));
    UiRect lane = {100, 100, 600, 200};
    View v = {1, 0, 0, 0};
    UiMouse m = {.clicked = true, .left = true, .pos = {340, 180}};
    ed_automation_lane_click(e, lane, &v, 80, m, 1);
    int history = e->undo_count;
    m.clicked = false;
    m.pos = (UiPoint){380, 160};
    ed_automation_node_drag(e, lane, &v, 80, m);
    check(fabs(e->project.automation[0].beat - 1.5) < 1e-8 &&
              fabs(e->project.automation[1].beat - 3.5) < 1e-8,
          "group time delta is anchored to clicked selected point");
    check(e->project.automation[1].b - e->project.automation[0].b == 30,
          "group values retain their relative difference");
    m.left = false;
    m.released = true;
    ed_automation_node_drag(e, lane, &v, 80, m);
    check(e->event_selection.size == 2 * sizeof(DmAutomation) && e->undo_count == history + 1,
          "group drag keeps selection and one undo step");
    ed_undo(e, false);
    check(e->project.automation[0].beat == 1 && e->project.automation[1].beat == 3,
          "undo restores entire event group");
    dm_bytes_free(&e->event_selection);
    dm_bytes(&e->event_selection, e->project.automation, 2 * sizeof(DmAutomation));
    m = (UiMouse){.clicked = true, .left = true, .pos = {340, 180}};
    ed_automation_lane_click(e, lane, &v, 80, m, 1);
    m.clicked = false;
    m.alt = true;
    m.pos = (UiPoint){353, 180};
    ed_automation_node_drag(e, lane, &v, 80, m);
    check(fabs(e->project.automation[0].beat - 1.1625) < 1e-8 &&
              fabs(e->project.automation[1].beat - 3.1625) < 1e-8,
          "Alt group drag bypasses quarter-beat snapping");
    m.pos.x = -100;
    ed_automation_node_drag(e, lane, &v, 80, m);
    check(e->project.automation[0].beat == 0 && e->project.automation[1].beat == 2,
          "group time clamping preserves spacing at song start");
    m.left = false;
    m.released = true;
    ed_automation_node_drag(e, lane, &v, 80, m);
    fixture(e);
    e->project.patterns[0].kind = 1;
    e->automation_cc = -2;
    e->edit_focus = FOCUS_EVENTS;
    dm_automation_add(&e->project, (DmAutomation){1, 0xe0, 0, 0, 64, 0, 0, 0, 0, false});
    dm_automation_add(&e->project, (DmAutomation){3, 0xe0, 0, 0, 96, 0, 0, 0, 0, false});
    float first = ed_automation_pitch(e, e->project.automation[0]);
    float second = ed_automation_pitch(e, e->project.automation[1]);
    dm_bytes(&e->event_selection, e->project.automation, 2 * sizeof(DmAutomation));
    m = (UiMouse){.clicked = true, .left = true, .pos = {340, 180}};
    ed_automation_lane_click(e, lane, &v, 80, m, 1);
    m.clicked = false;
    m.pos.y -= 40;
    ed_automation_node_drag(e, lane, &v, 80, m);
    EventDragItem *items = (EventDragItem *)e->event_drag.data;
    check(fabs(ed_automation_pitch(e, e->project.automation[items[0].index]) - first - 2) < .02 &&
              fabs(ed_automation_pitch(e, e->project.automation[items[1].index]) - second - 2) <
                  .02,
          "standalone musical pitch group preserves semitone spacing");
    m.pos.x = 180;
    ed_automation_node_drag(e, lane, &v, 80, m);
    items = (EventDragItem *)e->event_drag.data;
    check(
        e->project.automation[items[0].index].beat == 0 &&
            e->project.automation[items[1].index].beat == 2 &&
            fabs(ed_automation_pitch(e, e->project.automation[items[0].index]) - first - 2) < .02 &&
            fabs(ed_automation_pitch(e, e->project.automation[items[1].index]) - second - 2) < .02,
        "group pitch remains correct after crossing inserted bend-range messages");
    m.left = false;
    m.released = true;
    ed_automation_node_drag(e, lane, &v, 80, m);
}
int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), "SDL");
    SDL_Window *w = SDL_CreateWindow("pattern_workflow", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio && dm_audio_soundfont(audio, "agents/reports/generated-programs.sf2", error,
                                      sizeof(error)),
          "fixture font");
    while (dm_audio_busy(audio)) {
        bool success = false;
        if (dm_audio_poll(audio, error, sizeof(error), &success, NULL)) {
            check(success, "font loaded");
        }
        SDL_Delay(5);
    }
    recording_feedback_audio_pause(audio);
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, audio, "", true);
    ui_frame();
    ui_root(0, 900);
    sizing_names(e);
    brush_velocity(e);
    linked_copy(e);
    recording(e);
    group_points(e);
    ui_end();
    ui_render(r);
    fixture(e);
    test_pattern_workflow_ui(e, check);
    printf("PASS pattern_workflow: %d checks\n", checks);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
