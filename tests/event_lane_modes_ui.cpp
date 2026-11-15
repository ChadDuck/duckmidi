/* Drives the event selector and mode dropdown of the pattern's event lane and of the Event
   Editor, and draws LFO placements and the LFO editor for review by eye. */
#include "test_ui.h"

static UiRect event_lane() {
    float x = FLT_MAX, y = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;

    for (const auto &v : find_window("/Piano roll_")->DrawList->VtxBuffer) {
        if (v.col == 0xff121212u) {
            x = ImMin(x, v.pos.x);
            y = ImMin(y, v.pos.y);
            right = ImMax(right, v.pos.x);
            bottom = ImMax(bottom, v.pos.y);
        }
    }

    return {x, y, right - x, bottom - y};
}

static void select_event(Editor *e, Check check, const char *window, const char *event) {
    auto *w = find_window(window);
    check(w != nullptr, "event panel renders");
    ImGui::ActivateItemByID(w->GetID("##Event type"));
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
    check(popups.Size && popups.back().Window, "the event selector opens");
    ImGui::ActivateItemByID(popups.back().Window->GetID(event));
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
}

static void switch_event_modes(Editor *e, Check check, const char *window, int resolution) {
    select_event(e, check, window, DM_TEXT_PITCH_BEND);
    check(e->automation_cc == DM_EVENT_BEND && !ed_graph_resolution(e) && !e->graph_switch,
          "selecting pitch bend opens Point without a conversion warning");
    select_event(e, check, window, DM_TEXT_EXPRESSION_CC11);
    check(e->automation_cc == MIDI_CC_EXPRESSION && !ed_graph_resolution(e) && !e->graph_switch,
          "selecting another event keeps its Point view");
    select_event(e, check, window, DM_TEXT_MODULATION_CC1);
    check(e->automation_cc == MIDI_CC_MODULATION && ed_graph_resolution(e) == resolution &&
              !e->graph_switch,
          "returning to modulation restores its Graph view and resolution");
}

/* Each event keeps its own Point or Graph view; Volume and Pan lanes offer Graph but not LFO. */
static void pattern_lane_modes(Editor *e, Check check) {
    auto &p = e->project.patterns[0];
    e->roll = true;
    e->roll_auto_hidden = false;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_NOTES;
    e->automation_open = true;
    e->automation_cc = MIDI_CC_MODULATION;
    e->automation_raw = false;
    e->automation_height = 180;
    e->project.automation_count = 0;
    p.graph_lanes[MIDI_CC_MODULATION] = 16;
    frame(e, 0, 0, false);
    frame(e, 0, 0, false);
    UiRect lane = event_lane();
    ed_graph_paint(e, lane, &e->roll_view, 80 * e->roll_view.zoom, {lane.x + 2, lane.y + 10},
                   {lane.x + 600, lane.y + 10}, true);
    e->cursor_beat[1] = 1.1;
    frame(e, 0, 0, false);
    capture(e, "agents/reports/event-lanes-graph-playhead.bmp");
    switch_event_modes(e, check, "/Piano roll_", 16);
    select_event(e, check, "/Piano roll_", DM_TEXT_VOLUME_CC7);
    check(e->automation_cc == MIDI_CC_VOLUME, "the note pattern's event dropdown offers Volume");
    auto *window = find_window("/Piano roll_");
    ImGui::ActivateItemByID(window->GetID("##Event mode"));
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
    check(popups.Size && popups.back().Window, "the Volume lane offers a mode dropdown");
    ImGui::ActivateItemByID(popups.back().Window->GetID(DM_TEXT_LFO));
    frame(e, -100, -100, false);
    check(popups.Size && !p.mode, "the pattern Volume mode dropdown excludes LFO");
    ImGui::ActivateItemByID(popups.back().Window->GetID(DM_TEXT_GRAPH));
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    check(ed_graph_resolution(e) == 16, "the pattern Volume mode dropdown activates Graph");

    select_event(e, check, "/Piano roll_", DM_TEXT_PAN_CC10);
    check(e->automation_cc == MIDI_CC_PAN && !ed_graph_resolution(e),
          "the note pattern's event dropdown offers an independent Pan lane");
    select_event(e, check, "/Piano roll_", DM_TEXT_VOLUME_CC7);
    check(ed_graph_resolution(e) == 16, "the Volume lane remembers Graph across lane changes");
    p.graph_lanes[MIDI_CC_VOLUME] = 0;
    e->automation_cc = MIDI_CC_MODULATION;
    p.graph_lanes[MIDI_CC_MODULATION] = 0;
    e->automation_open = false;
}

/* An LFO placement draws the wave at the playlist's beat scale, so resizing the placement only
   reveals or hides more of it. Captures are checked by eye. */
static int lfo_pattern(Editor *e) {
    int id = dm_pattern_add(&e->project, "LFO");
    DmPattern *p = &e->project.patterns[id];
    p->kind = 1;
    p->event = 1;
    p->mode = 1;
    p->length = 4;
    p->speed = 1;
    p->amount = .8;
    p->center = 64;
    p->skew = .4;
    p->pulse = .5;
    p->tension = 0;
    return id;
}

static void lfo_previews(Editor *e, Check check, int id) {
    e->roll = false;
    e->song_event = false;
    e->project.clip_count = 0;
    e->project.lane_count[0] = 1;
    e->channel_expanded[0] = true;
    e->playlist_view = {1, 0, 0, 0};
    check(
        dm_clip_add(&e->project, {.beat = 0, .pattern = id, .channel = 0, .length = 4, .lane = 1}),
        "LFO placement fixture");
    frame(e, 0, 0, false);
    capture(e, "agents/reports/event-lanes-lfo-placement-4.bmp");
    e->project.clips[0].length = 7;
    frame(e, 0, 0, false);
    capture(e, "agents/reports/event-lanes-lfo-placement-7.bmp");
    e->roll = true;
    e->roll_auto_hidden = false;
    ed_open_pattern(e, id, false);
    frame(e, 0, 0, false);
    frame(e, 0, 0, false);
    capture(e, "agents/reports/event-lanes-lfo-editor.bmp");
    /* A four-bar LFO pattern still shows one bar. */
    e->project.patterns[id].length = 16;
    frame(e, 0, 0, false);
    capture(e, "agents/reports/event-lanes-lfo-editor-four-bars.bmp");
    e->project.patterns[id].length = 4;
    ed_open_pattern(e, 0, false);
    e->roll = false;
    e->project.clip_count = 0;
    e->project.lane_count[0] = 0;
}

extern "C" void test_event_lane_modes_ui(Editor *e, Check check) {
    int id = lfo_pattern(e);
    lfo_previews(e, check, id);
    pattern_lane_modes(e, check);
    DmPattern &p = e->project.patterns[id];
    p.mode = 0;
    p.graph_lanes[MIDI_CC_MODULATION] = 32;
    ed_open_pattern(e, id, false);
    frame(e, -100, -100, false);
    switch_event_modes(e, check, "/Event Editor_", 32);

    /* Point menu "Hold until pattern end" with one undo step each way, Graph lane menu
       "Hold last bar until pattern end", and square-vs-circle marker rendering. */
    ed_new_project(e);
    e->roll = true;
    e->roll_auto_hidden = false;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_NOTES;
    e->automation_open = true;
    e->automation_cc = MIDI_CC_EXPRESSION;
    e->automation_raw = false;
    e->automation_height = 180;
    e->project.patterns[0].length = 4;
    check(dm_automation_add(
              &e->project,
              {.beat = 0, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 64, .curve = 1}),
          "line start");
    check(dm_automation_add(
              &e->project,
              {.beat = 2, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 64, .curve = 1}),
          "line end");
    check(dm_automation_add(
              &e->project,
              {.beat = 2, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 127, .curve = -1}),
          "line restore");
    frame(e, 0, 0, false);
    frame(e, 0, 0, false);
    UiRect lane = event_lane();
    UiPoint end_pt = ed_automation_point(e, e->project.automation[1], lane, &e->roll_view,
                                         80 * e->roll_view.zoom);

    /* Toggle Hold until pattern end ON via point_menu. */
    frame(e, end_pt.x, end_pt.y, false, false, false, true);
    frame(e, end_pt.x, end_pt.y, false, true, false, true);
    frame(e, end_pt.x, end_pt.y, false, false, false, false);
    auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
    check(popups.Size && popups.back().Window, "point menu opens on Shift+right-click");
    ImGui::ActivateItemByID(popups.back().Window->GetID(DM_TEXT_HOLD_UNTIL_PATTERN_END));
    frame(e, -100, -100, false);
    check(e->project.automation_count == 2, "toggling hold on removes restoration node");
    ed_undo(e, false);
    check(e->project.automation_count == 3, "undo restores restoration in one step");
    ed_undo(e, true);
    check(e->project.automation_count == 2, "redo removes restoration again");

    /* Toggle Hold until pattern end OFF via point_menu. */
    frame(e, end_pt.x, end_pt.y, false, false, false, true);
    frame(e, end_pt.x, end_pt.y, false, true, false, true);
    frame(e, end_pt.x, end_pt.y, false, false, false, false);
    check(popups.Size && popups.back().Window, "point menu reopens on held end");
    ImGui::ActivateItemByID(popups.back().Window->GetID(DM_TEXT_HOLD_UNTIL_PATTERN_END));
    frame(e, -100, -100, false);
    check(e->project.automation_count == 3 && e->project.automation[2].curve == -1,
          "toggling hold off adds restoration node");
    ed_undo(e, false);
    check(e->project.automation_count == 2, "undo removes added restoration in one step");
    ed_undo(e, true);

    /* Shape test: add a discrete held point at beat 3 and compare marker corners. */
    check(dm_automation_add(
              &e->project,
              {.beat = 3, .type = MIDI_CONTROL, .a = MIDI_CC_EXPRESSION, .b = 64, .curve = 0}),
          "discrete held point");
    frame(e, -100, -100, false);
    frame(e, -100, -100, false);
    UiPoint circle_pt = ed_automation_point(e, e->project.automation[0], lane, &e->roll_view,
                                            80 * e->roll_view.zoom);
    size_t held = 0;

    while (held < e->project.automation_count &&
           (e->project.automation[held].beat != 3 || e->project.automation[held].curve)) {
        ++held;
    }

    check(held < e->project.automation_count && ed_point_holds(e, held),
          "the discrete point holds");
    UiPoint square_pt = ed_automation_point(e, e->project.automation[held], lane, &e->roll_view,
                                            80 * e->roll_view.zoom);
    SDL_Surface *surface = SDL_RenderReadPixels(e->renderer, nullptr);
    check(surface != nullptr, "read pixels for marker shape check");

    if (surface) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(e->window, &win_w, &win_h);
        float px = win_w > 0 ? (float)surface->w / (float)win_w : 1.0f;
        /* A marker's corner, 4 px up and left of its centre, lies inside a square of half-size
           5 but outside a circle of radius 5: a square's corner has its centre's colour. */
        auto pixel = [&](UiPoint at, float dx, float dy) {
            Uint8 r = 0, g = 0, b = 0, a = 0;
            SDL_ReadSurfacePixel(surface, (int)((at.x + dx) * px), (int)((at.y + dy) * px), &r, &g,
                                 &b, &a);
            return (Uint32)r << 16 | (Uint32)g << 8 | b;
        };
        check(pixel(circle_pt, -4, -4) != pixel(circle_pt, 0, 0),
              "non-held circle leaves marker corner empty");
        check(pixel(square_pt, -4, -4) == pixel(square_pt, 0, 0),
              "held square fills marker corner");
        SDL_DestroySurface(surface);
    }

    /* Graph lane menu toggles graph_hold with undo. */
    e->automation_cc = MIDI_CC_MODULATION;
    e->project.patterns[0].graph_lanes[MIDI_CC_MODULATION] = 16;
    e->project.patterns[0].graph_hold[MIDI_CC_MODULATION] = false;
    frame(e, 0, 0, false);
    lane = event_lane();
    frame(e, lane.x + 40, lane.y + 40, false, false, false, true);
    frame(e, lane.x + 40, lane.y + 40, false, true, false, true);
    frame(e, lane.x + 40, lane.y + 40, false, false, false, false);
    check(popups.Size && popups.back().Window, "Graph lane menu opens on Shift+right-click");
    ImGui::ActivateItemByID(popups.back().Window->GetID(DM_TEXT_HOLD_LAST_BAR));
    frame(e, -100, -100, false);
    check(e->project.patterns[0].graph_hold[MIDI_CC_MODULATION],
          "Graph lane menu toggles graph_hold on");
    ed_undo(e, false);
    check(!e->project.patterns[0].graph_hold[MIDI_CC_MODULATION],
          "undo restores graph_hold off in one step");

    /* Pitch-bend note-name view: dim stretches follow the sounding note (F7). */
    ed_new_project(e);
    e->roll = true;
    e->roll_auto_hidden = false;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_NOTES;
    e->automation_open = true;
    e->automation_cc = DM_EVENT_BEND;
    e->automation_raw = false;
    e->automation_height = 180;
    e->project.patterns[0].length = 12;
    check(dm_clip_add(&e->project, {.beat = 0, .pattern = 0, .channel = 0, .length = 12}),
          "pitch-bend reference clip");
    check(
        dm_note_add(&e->project.patterns[0], {.beat = 0, .length = 2, .key = 69, .velocity = 100}),
        "note 1 (A4)");
    check(
        dm_note_add(&e->project.patterns[0], {.beat = 2, .length = 2, .key = 67, .velocity = 100}),
        "note 2 (G4)");
    check(
        dm_note_add(&e->project.patterns[0], {.beat = 5, .length = 2, .key = 72, .velocity = 100}),
        "note 3 (C5)");
    check(
        dm_note_add(&e->project.patterns[0], {.beat = 6, .length = 1, .key = 62, .velocity = 100}),
        "note 3b (D4, overlaps C5)");
    check(
        dm_note_add(&e->project.patterns[0], {.beat = 8, .length = 2, .key = 60, .velocity = 100}),
        "chord lower note (C4)");
    check(dm_note_add(&e->project.patterns[0],
                      {.beat = 8, .length = 2, .key = 64, .velocity = 100, .bend_id = 7}),
          "chord upper note (E4)");

    auto add_bend = [&](double beat, int value, int curve, int anchor, uint32_t bend_note) {
        return dm_automation_add(&e->project, {.beat = beat,
                                               .type = MIDI_PITCH_BEND,
                                               .a = value & 127,
                                               .b = (value >> 7) & 127,
                                               .pattern = 0,
                                               .curve = curve,
                                               .anchor = anchor,
                                               .bend_note = bend_note});
    };

    check(add_bend(0, MIDI_BEND_CENTER, 1, 69, 0), "line 1 start");
    check(add_bend(1, MIDI_BEND_CENTER + 2048, 1, 69, 0), "line 1 end");
    check(add_bend(1, MIDI_BEND_CENTER, -1, 69, 0), "line 1 restore");
    check(add_bend(5, MIDI_BEND_CENTER, 2, 72, 0), "line 2 start");
    check(add_bend(6, MIDI_BEND_CENTER - 2048, 2, 72, 0), "line 2 end");
    check(add_bend(6, MIDI_BEND_CENTER, -2, 72, 0), "line 2 restore");
    check(add_bend(8, MIDI_BEND_CENTER, 3, 64, 7), "chord line start");
    check(add_bend(9, MIDI_BEND_CENTER + 1024, 3, 64, 0), "chord line end");
    check(add_bend(9, MIDI_BEND_CENTER, -3, 64, 0), "chord line restore");

    EdBendReferences refs = {0};
    check(ed_bend_references(e, &refs), "build bend reference timeline");
    check(ed_bend_reference_at(&refs, 0.5) == 69, "reference on first note");
    check(ed_bend_reference_at(&refs, 1.5) == 69, "reference stays on first note after restore");
    check(ed_bend_reference_at(&refs, 2.0) == 67, "reference changes exactly at second note start");
    check(ed_bend_reference_at(&refs, 3.0) == 67, "reference on second note");
    check(ed_bend_reference_at(&refs, 4.5) == 67, "gap keeps previous note key");
    check(ed_bend_reference_at(&refs, 5.0) == 72, "reference changes at third note start");
    check(ed_bend_reference_at(&refs, 6.0) == 62,
          "chord with no attached bend uses lowest note even when higher note started earlier");
    check(ed_bend_reference_at(&refs, 7.5) == 62, "gap after overlap keeps lowest note key");
    check(ed_bend_reference_at(&refs, 8.5) == 64, "chord with attached bend uses upper note");
    check(ed_bend_reference_at(&refs, 10.5) == 64, "gap after chord keeps chord reference");
    ed_bend_references_free(&refs);

    e->project.automation[6].bend_note = 0;
    check(ed_bend_references(e, &refs), "rebuild bend reference timeline without attachment");
    check(ed_bend_reference_at(&refs, 8.5) == 60, "chord without attached bend uses lowest note");
    ed_bend_references_free(&refs);
    e->project.automation[6].bend_note = 7;

    frame(e, 0, 0, false);
    frame(e, 0, 0, false);

    /* After line 1 returns at beat 1, the dim neutral line runs on A4's row to beat 2, where G4
       starts, then on G4's row to line 2 at beat 5: its corners are dim line ends at those rows. */
    UiRect bend_lane = event_lane();
    float bend_scale = 80 * e->roll_view.zoom;
    UiPoint restore_pt =
        ed_automation_point(e, e->project.automation[2], bend_lane, &e->roll_view, bend_scale);
    uint32_t dim = (ed_point_color(e) & 0xffffffu) | 0x66000000u;
    auto dim_corner = [&](int key, double beat) {
        float row = bend_lane.y + bend_lane.h * .5f - (key - e->automation_center) * ED_LANE_STEP;
        float x = restore_pt.x + (float)(beat - 1) * bend_scale;

        for (const auto &v : find_window("/Piano roll_")->DrawList->VtxBuffer) {
            if (v.col == dim && fabsf(v.pos.y - row) < 2 && fabsf(v.pos.x - x) < 2) {
                return true;
            }
        }

        return false;
    };
    check(dim_corner(69, 1) && dim_corner(69, 2),
          "the dim neutral line sits on A4's row while A4 sounds");
    check(dim_corner(67, 2) && dim_corner(67, 5),
          "the dim neutral line moves to G4's row when G4 starts");
    DmExportEvent *before_events = nullptr, *after_events = nullptr;
    size_t before_count = 0, after_count = 0;
    uint64_t before_dur = 0, after_dur = 0;
    char err[128] = "";
    bool before_ok = dm_export_timeline(&e->project, &before_events, &before_count, &before_dur,
                                        err, sizeof(err));
    e->automation_raw = true;
    frame(e, 0, 0, false);
    frame(e, 0, 0, false);
    bool after_ok =
        dm_export_timeline(&e->project, &after_events, &after_count, &after_dur, err, sizeof(err));
    check(before_ok && after_ok, "export timeline succeeds in both views");
    bool same_timeline =
        before_ok && after_ok && before_dur == after_dur && before_count == after_count;

    for (size_t i = 0; same_timeline && i < before_count; ++i) {
        same_timeline = before_events[i].tick == after_events[i].tick &&
                        before_events[i].status == after_events[i].status &&
                        before_events[i].a == after_events[i].a &&
                        before_events[i].b == after_events[i].b &&
                        before_events[i].port == after_events[i].port &&
                        before_events[i].flags == after_events[i].flags;
    }

    check(same_timeline, "flattened events are identical before and after switching views");
    free(before_events);
    free(after_events);
}
