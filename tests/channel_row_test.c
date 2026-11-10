#include "editor/internal.h"
static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL channel rows: %s\n", text);
        exit(1);
    }
}

void test_channel_row_ui(Editor *e, void (*check)(bool, const char *));

static void removal(Editor *e) {
    ed_new_project(e);
    e->project.channels[2] = (DmChannel){true, true, false, 25, 80, 64, true};
    e->project.lane_count[2] = 1;
    e->channel_expanded[2] = true;
    snprintf(e->project.lane_names[2][0], sizeof(e->project.lane_names[2][0]), "Modulation");
    int lane = dm_pattern_add(&e->project, "Automation");
    check(lane >= 0, "automation pattern fixture");
    e->project.patterns[lane].kind = 1;
    check(ed_place_pattern(e, 0, 2, 0) >= 0 && ed_place_pattern(e, lane, 2 + 16, 1) >= 0 &&
              ed_place_pattern(e, 0, 0, 0) >= 0,
          "note and automation placements share a channel");
    DmAutomation point = {
        .channel = 2, .type = MIDI_CONTROL, .a = MIDI_CC_MODULATION, .b = 60, .pattern = -1};
    check(dm_automation_add(&e->project, point), "channel automation fixture");
    point.pattern = lane;
    check(dm_automation_add(&e->project, point), "pattern automation fixture");
    uint8_t cc[] = {MIDI_CONTROL | 2, MIDI_CC_MODULATION, 70};
    DmBytes message = {cc, sizeof(cc), sizeof(cc)};
    check(dm_event_add(&e->project, 1, 0, 0, &message), "channel MIDI event fixture");
    check(
        dm_port_mix_set(&e->project, (DmPortMix){.port = 2, .channel = 2, .volume = 90, .pan = 64}),
        "channel port mixer fixture");
    int undo = e->undo_count;
    check(ed_remove_channel(e, 2), "remove a populated channel");
    check(!e->project.channels[2].enabled && !e->project.channels[2].mute &&
              !e->project.lane_count[2] && !e->project.lane_names[2][0][0] &&
              !e->channel_expanded[2] && !e->project.event_count && !e->project.port_mix_count,
          "removal clears channel settings, lanes, events and port mixes");
    check(e->project.pattern_count == 2 && e->project.clip_count == 1 &&
              e->project.clips[0].channel == 0 && e->project.automation_count == 1 &&
              e->project.automation[0].pattern == lane,
          "removal keeps other rows and the source patterns and their automation");
    check(e->selected_clip == 0 && e->undo_count == undo + 1,
          "removal preserves surviving selection and creates one undo entry");
    ed_undo(e, false);
    check(e->project.channels[2].enabled && e->project.channels[2].drums &&
              e->project.clip_count == 3 && e->project.event_count == 1,
          "undo restores the removed row and its content");
    ed_undo(e, true);
    check(!e->project.channels[2].enabled && e->project.clip_count == 1,
          "redo removes the channel again");
}

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("channel rows", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", "", true);
    int list[16];
    check(ed_channels(e, list) == 2 && list[0] == 0 && list[1] == MIDI_DRUM_CHANNEL,
          "new projects show only channels 1 and 10");
    ed_new_project(e);
    test_channel_row_ui(e, check);
    removal(e);
    ed_new_project(e);
    check(ed_place_pattern(e, 0, 1, 0) < 0 && !e->project.channels[1].enabled,
          "placing a pattern cannot activate an unused channel");
    ed_open_instrument(e, -1, false);
    ed_apply_instrument(e);
    check(ed_place_pattern(e, 0, 1, 0) >= 0, "place a pattern on a newly created channel");
    ed_focus_edit(e, FOCUS_PLAYLIST);
    ed_clipboard(e, 0);
    check(e->clipboard.size && ed_remove_channel(e, 1), "copy a placement and remove its channel");
    int undo = e->undo_count;
    ed_paste(e);
    check(!e->project.channels[1].enabled && !e->project.clip_count && e->undo_count == undo &&
              strstr(e->status, "+ New"),
          "pasting requires an existing destination and cannot recreate a channel");

    for (int c = 0; c < 16; ++c) {
        e->project.channels[c].enabled = true;
    }

    ed_open_instrument(e, -1, false);
    check(ed_channels(e, list) == 16 && e->instrument_channel < 0,
          "a full playlist cannot open the new channel dialog");
    printf("PASS channel rows: %d checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
