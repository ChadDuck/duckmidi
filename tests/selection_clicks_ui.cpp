/* Drives selection with the mouse in the playlist, the piano roll and the event lanes: a plain
   click on a selected item keeps only that item, a drag still moves the whole selection and a
   right click clears it. */
#include "test_ui.h"

static int selected_clips(Editor *e) {
    int count = 0;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        count += e->project.clips[i].selected != 0;
    }

    return count;
}

static int selected_notes(DmPattern *p) {
    int count = 0;

    for (size_t i = 0; i < p->count; ++i) {
        count += p->notes[i].selected != 0;
    }

    return count;
}

static void playlist(Editor *e, Check check) {
    e->roll = false;
    e->label_width = 240;
    e->playlist_view = {1, 0, 0, 0};
    e->project.clip_count = 0;
    e->tool = TOOL_SELECT;
    dm_clip_add(&e->project, {.beat = 0, .pattern = 0, .channel = 0, .length = 4});
    dm_clip_add(&e->project, {.beat = 8, .pattern = 0, .channel = 0, .length = 4});
    frame(e, 0, 0, false);
    auto *w = find_window("/Playlist_");
    check(w != nullptr, "playlist renders");
    float gx = w->WorkRect.Min.x + e->label_width;
    float top = w->WorkRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 28;
    float y = top + 64 + 32;
    e->project.clips[0].selected = e->project.clips[1].selected = 1;
    click(e, gx + 40, y);
    check(e->project.clips[0].selected && selected_clips(e) == 1 && e->project.clips[0].beat == 0,
          "a plain click on a selected placement selects only that placement");
    e->project.clips[0].selected = e->project.clips[1].selected = 1;
    frame(e, gx + 40, y, false);
    frame(e, gx + 40, y, true);
    frame(e, gx + 80, y, true);
    frame(e, gx + 80, y, false);
    check(selected_clips(e) == 2 && e->project.clips[1].beat > 8,
          "dragging a selected placement still moves the whole selection");
    click(e, gx + 250, y, true);
    check(selected_clips(e) == 0 && e->project.clip_count == 2,
          "a right click on the playlist grid clears the placement selection");
}

static void piano_roll(Editor *e, Check check) {
    e->roll = true;
    e->roll_auto_hidden = false;
    e->automation_open = false;
    auto &p = e->project.patterns[0];
    p.count = 0;
    p.length = 4;
    e->project.automation_count = 0;
    e->selected_pattern = 0;
    e->edit_focus = FOCUS_NOTES;
    e->tool = TOOL_SELECT;
    e->roll_view = {1, 0, (127 - 70) * 20, 0};
    dm_note_add(&p, {0, .5, 64, 100, 1});
    dm_note_add(&p, {1, .5, 67, 100, 1});
    frame(e, 0, 0, false);
    auto *w = find_window("/Piano roll_");
    check(w != nullptr, "piano roll renders");
    float gx = w->Pos.x + w->WindowPadding.x + 64;
    float gy = w->Pos.y + w->WindowPadding.y + 32 + ImGui::GetStyle().ItemSpacing.y + 28;
    /* Opening the pattern may scroll it to its first note. */
    float x = gx + 20 - e->roll_view.scroll_x,
          y = gy + (127 - 64) * 20 - e->roll_view.scroll_y + 10;
    p.notes[0].selected = p.notes[1].selected = 1;
    click(e, x, y);
    check(p.notes[0].selected && selected_notes(&p) == 1 && p.count == 2,
          "a plain click on a selected note selects only that note");
    p.notes[0].selected = p.notes[1].selected = 1;
    click(e, x + 3 * 80, y, true);
    check(selected_notes(&p) == 0 && p.count == 2,
          "a right click in the piano roll clears the note selection");
}

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

static void event_point_clicks(Editor *e, Check check) {
    auto &p = e->project.patterns[0];
    p.graph_lanes[MIDI_CC_MODULATION] = 0;
    e->automation_open = true;
    e->automation_cc = 1;
    e->automation_raw = false;
    e->automation_height = 180;
    e->project.automation_count = 0;
    frame(e, 0, 0, false);
    UiRect lane = event_lane();
    float ax = lane.x + 40, bx = lane.x + 200, y = lane.y + lane.h * .5f;
    click(e, ax, y, false, true);
    e->automation_node = -1;
    click(e, bx, y, false, true);
    check(e->project.automation_count == 2, "clicks place two event points");
    e->automation_all = true;
    e->automation_node = -1;
    click(e, ax, y);
    check(!e->automation_all && !e->event_selection.size && e->automation_node >= 0 &&
              e->project.automation[e->automation_node].beat < 1,
          "a plain click on a selected event point selects only that point");
    e->automation_all = true;
    click(e, lane.x + 120, y, true);
    check(!e->automation_all && e->automation_node < 0 && e->project.automation_count == 2,
          "a right click in the event lane clears the point selection");
    p.graph_lanes[MIDI_CC_MODULATION] = 16;
    e->automation_all = true;
    frame(e, 0, 0, false);
    click(e, lane.x + 120, y, true);
    check(!e->automation_all, "a right click in a Graph lane clears the bar selection");
    p.graph_lanes[MIDI_CC_MODULATION] = 0;
    e->automation_open = false;
}

extern "C" void test_selection_clicks_ui(Editor *e, Check check) {
    playlist(e, check);
    piano_roll(e, check);
    event_point_clicks(e, check);
}
