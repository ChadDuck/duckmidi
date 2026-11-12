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
}
