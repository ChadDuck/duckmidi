extern "C" {
#include "editor/internal.h"
#include "text/en.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cfloat>
#include <cmath>
#include <cstring>

typedef void (*Check)(bool, const char *);

static void frame(Editor *e, float x, float y, bool left, bool right = false, bool alt = false,
                  bool shift = false) {
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, left);
    io.AddMouseButtonEvent(1, right);
    io.AddKeyEvent(ImGuiMod_Alt, alt);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    ui_frame();
    editor_frame(e, 1400, 900);
    SDL_SetRenderDrawColor(e->renderer, 0, 0, 0, 255);
    SDL_RenderClear(e->renderer);
    ui_render(e->renderer);
    SDL_RenderPresent(e->renderer);
}

static void click(Editor *e, float x, float y, bool right = false, bool alt = false) {
    frame(e, x, y, false, false, alt);
    frame(e, x, y, !right, right, alt);
    frame(e, x, y, false, false, alt);
}

static void capture(Editor *e, const char *path) {
    SDL_Surface *surface = SDL_RenderReadPixels(e->renderer, NULL);

    if (surface) {
        SDL_SaveBMP(surface, path);
        SDL_DestroySurface(surface);
    }
}

static ImGuiWindow *find_window(const char *part) {
    for (auto *w : ImGui::GetCurrentContext()->Windows) {
        if (w->Active && std::strstr(w->Name, part)) {
            return w;
        }
    }

    return nullptr;
}

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
    e->tool = 0;
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
    e->context = 2;
    e->edit_focus = FOCUS_NOTES;
    e->tool = 0;
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
    check(e->automation_cc == -2 && !ed_graph_resolution(e) && !e->graph_switch,
          "selecting pitch bend opens Point without a conversion warning");
    select_event(e, check, window, DM_TEXT_EXPRESSION_CC11);
    check(e->automation_cc == MIDI_CC_EXPRESSION && !ed_graph_resolution(e) && !e->graph_switch,
          "selecting another event keeps its Point view");
    select_event(e, check, window, DM_TEXT_MODULATION_CC1);
    check(e->automation_cc == MIDI_CC_MODULATION && ed_graph_resolution(e) == resolution &&
              !e->graph_switch,
          "returning to modulation restores its Graph view and resolution");
}

static void event_points(Editor *e, Check check) {
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
    ed_graph_paint(e, lane, &e->roll_view, 80 * e->roll_view.zoom, {lane.x + 2, lane.y + 10},
                   {lane.x + 600, lane.y + 10}, true);
    e->cursor_beat[1] = 1.1;
    frame(e, 0, 0, false);
    capture(e, "agents/reports/todo-e-2026-11-10/graph-playhead-pattern.bmp");
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

/* Item 3: the Song events window is hidden until its contents are measured, then opens centered
   at its fitted height, without moving afterwards. */
static void song_event_window(Editor *e, Check check) {
    ed_song_event_open_bar(e, 0, true);
    frame(e, 0, 0, false);
    auto *w = ImGui::FindWindowByName(DM_TEXT_SONG_EVENT);
    check(w && w->Active && w->Hidden, "the Song events window is hidden while it measures");
    int hidden = 1;

    while (w->Hidden && hidden < 4) {
        frame(e, 0, 0, false);
        hidden += w->Hidden;
    }

    check(hidden <= 2, "the Song events window shows within two frames");
    ImVec2 pos = w->Pos, size = w->Size;
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    check(!w->Hidden && std::fabs(pos.y + size.y * .5f - center.y) < 1.5f &&
              std::fabs(pos.x + size.x * .5f - center.x) < 1.5f,
          "the Song events window appears centered");
    check(size.y < ImGui::GetMainViewport()->Size.y * .5f,
          "the Add mode Song events window fits its fields");
    frame(e, 0, 0, false);
    frame(e, 0, 0, false);
    check(w->Pos.x == pos.x && w->Pos.y == pos.y && w->Size.x == size.x && w->Size.y == size.y,
          "the Song events window does not move or resize after it appears");
    e->song_event = false;
    frame(e, 0, 0, false);
}

/* Items 5 and 5-1: an LFO placement draws the wave at the playlist's beat scale, so resizing
   the placement only reveals or hides more of it. Captures are checked by eye. */
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
    capture(e, "agents/reports/todo-e-2026-11-10/lfo-placement-4.bmp");
    e->project.clips[0].length = 7;
    frame(e, 0, 0, false);
    capture(e, "agents/reports/todo-e-2026-11-10/lfo-placement-7.bmp");
    e->roll = true;
    e->roll_auto_hidden = false;
    ed_open_pattern(e, id, false);
    frame(e, 0, 0, false);
    frame(e, 0, 0, false);
    capture(e, "agents/reports/todo-e-2026-11-10/lfo-editor.bmp");
    /* Item 6: a four-bar LFO pattern still shows one bar. */
    e->project.patterns[id].length = 16;
    frame(e, 0, 0, false);
    capture(e, "agents/reports/todo-e-2026-11-10/lfo-editor-four-bars.bmp");
    e->project.patterns[id].length = 4;
    ed_open_pattern(e, 0, false);
    e->roll = false;
    e->project.clip_count = 0;
    e->project.lane_count[0] = 0;
}

extern "C" void test_todo_e_ui(Editor *e, Check check) {
    int id = lfo_pattern(e);
    lfo_previews(e, check, id);
    SDL_CreateDirectory("agents/reports/todo-e-2026-11-10");
    song_event_window(e, check);
    playlist(e, check);
    piano_roll(e, check);
    event_points(e, check);
    DmPattern &p = e->project.patterns[id];
    p.mode = 0;
    p.graph_lanes[MIDI_CC_MODULATION] = 32;
    ed_open_pattern(e, id, false);
    frame(e, -100, -100, false);
    switch_event_modes(e, check, "/Event Editor_", 32);
}
