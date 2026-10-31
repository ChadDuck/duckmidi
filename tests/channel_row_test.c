#include "editor_internal.h"
static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL channel rows: %s\n", text);
        exit(1);
    }
}

static void label_check(Editor *e, int channel, const char *expected) {
    int list[] = {channel};
    char text[128];
    ed_playlist_label_text(e, (UiRect){0, 0, 600, 400}, 240, 64, list, 1, 0, text, sizeof(text));
    check(strstr(text, expected) != NULL, "channel label matches its current assignment");
}

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("channel rows", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", "", true);
    int startup[16];
    check(ed_channels(e, startup) == 2 && startup[0] == 0 && startup[1] == 9,
          "actual editor initialization shows only Channel 1 and percussion");
    editor_smoke(e);
    e->project.clip_count = 0;

    for (int c = 0; c < 16; ++c) {
        e->project.channels[c] = (DmChannel){false, false, false, -1, 100, 64};
    }

    int list[16 * (DM_LANES + 1)];
    check(ed_channels(e, list) == 1 && list[0] == 0, "empty playlist offers first channel");
    e->project.channels[0].enabled = e->project.channels[9].enabled = true;
    e->project.channels[9].program = 0;
    check(ed_channels(e, list) == 2 && list[0] == 0 && list[1] == 9,
          "startup reuses unassigned channel 1 instead of showing a duplicate option");
    check(ed_available_channel(e) == 0, "Mixer also reuses the existing unassigned channel");
    e->project.channels[0].program = 0;
    check(ed_channels(e, list) == 3 && list[0] == 0 && list[1] == 1 && list[2] == 9,
          "next spare sits above percussion for channels 1 through 9");
    e->project.channels[1].program = 36;
    e->channel_expanded[1] = true;
    e->project.lane_count[1] = 2;
    check(ed_playlist_channels(e, list) == 3, "spare never exposes leftover automation subrows");
    ui_frame();
    ui_root(0, 900);
    label_check(e, 1, "+ Choose instrument");
    UiMouse mouse = {.clicked = true};
    ed_playlist_label_action(e, 1, 0, mouse);
    check(e->instrument_channel == 1 && !e->project.channels[1].enabled,
          "spare click opens instrument picker without activating channel");
    ed_choose_program(e, 1, 36);
    check(e->project.channels[1].enabled && e->project.channels[1].program == 36,
          "instrument selection activates spare");
    label_check(e, 1, dm_program_name(36));
    check(ed_channels(e, list) == 4 && list[2] == 2, "assignment offers the next free channel");
    e->project.channels[1].mute = e->project.channels[1].solo = true;
    check(ed_remove_empty_channel(e, 1), "assigned empty channel can be removed");
    check(!e->project.channels[1].enabled && e->project.channels[1].program == -1 &&
              !e->project.channels[1].mute && !e->project.channels[1].solo,
          "removal clears stale instrument, mute and solo");
    check(!e->channel_selected && !e->channel_expanded[1], "removal clears active row state");
    label_check(e, 1, "+ Choose instrument");
    check(ed_channels(e, list) == 3 && list[1] == 1, "removed slot returns as the spare");
    check(!ed_remove_empty_channel(e, 1), "repeated removal is a no-op");
    ed_undo(e, false);
    check(e->project.channels[1].enabled && e->project.channels[1].program == 36,
          "undo restores instrument assignment");
    ed_undo(e, true);
    check(!e->project.channels[1].enabled && e->project.channels[1].program == -1,
          "redo restores clean spare state");

    for (int c = 0; c < 16; ++c) {
        e->project.channels[c].enabled = true;
        e->project.channels[c].program = 0;
    }

    check(ed_channels(e, list) == 16, "full playlist has only 16 MIDI rows plus Events");

    for (int c = 0; c < 16; ++c) {
        check(ed_remove_empty_channel(e, c), "each channel can be removed when empty");
        check(ed_channels(e, list) == 16 - c && list[0] == 0,
              "only one spare remains as channels are removed");
        label_check(e, c, "+ Choose instrument");
    }

    for (int c = 0; c < 10; ++c) {
        e->project.channels[c] = (DmChannel){true, false, false, 0, 100, 64};
    }

    check(ed_channels(e, list) == 11 && list[9] == 9 && list[10] == 10,
          "spare moves below percussion once channels 1 through 9 are assigned");
    e->project.channels[3].enabled = false;
    check(ed_channels(e, list) == 10 && list[3] == 3 && list[9] == 9,
          "removed lower slot restores spare above percussion without an upper spare");

    for (int c = 0; c < 16; ++c) {
        e->project.channels[c].enabled = false;
    }

    ed_choose_program(e, 9, 0);
    label_check(e, 9, "Percussion");
    check(ed_remove_empty_channel(e, 9), "percussion can be removed when empty");
    label_check(e, 9, "+ Choose instrument");
    bool clip =
        dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 3, .beat = 0, .length = 4});
    check(clip && ed_channel_has_clips(e, 3), "placement marks channel occupied");
    check(!ed_remove_empty_channel(e, 3) && e->project.channels[3].enabled,
          "channel containing a placement cannot be removed");
    ui_end();
    ui_render(r);
    printf("PASS channel rows: %d checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
