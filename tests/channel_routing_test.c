#include "editor_internal.h"

extern void recording_feedback_audio_pause(DmAudio *);
extern void recording_feedback_audio_render(DmAudio *, int);
extern int recording_feedback_audio_preset(DmAudio *, int, bool);
extern unsigned recording_feedback_audio_voice(DmAudio *, int, int);
extern void test_channel_routing_ui(Editor *, void (*)(bool, const char *));
static int checks;
static void check(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL channel routing: %s\n", text);
        exit(1);
    }
}
static void fixture(Editor *e) {
    ed_release_piano(e);
    ed_new_project(e);
    e->project.clip_count = e->project.automation_count = 0;
    e->selected_pattern = 0;
    e->selected_channel = e->pattern_channel = 0;
    e->selected_clip = e->playlist_context_clip = -1;
    e->preview_override = false;
    e->preview_source_channel = -1;
    e->context = 1;
    e->edit_focus = FOCUS_PLAYLIST;
    e->drag = 0;
    e->clip_moved = false;
    ed_pattern(e)->count = 0;
    dm_note_add(ed_pattern(e), (DmNote){0, 1, 60, 100, 0});
    dm_note_add(ed_pattern(e), (DmNote){2, 1, 64, 100, 0});
    dm_note_add(ed_pattern(e), (DmNote){6, 1, 67, 100, 0});
}
static void programs(Editor *e) {
    fixture(e);
    for (int c = 0; c < 16; ++c) {
        e->project.channels[c].enabled = true;
        e->project.channels[c].program = c == 9 ? 0 : c;
        dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = c});
    }
    check(ed_transport_publish(e, 0, true), "start all-channel song");
    recording_feedback_audio_render(e->audio, 256);
    int before[16];
    for (int c = 0; c < 16; ++c) {
        before[c] = recording_feedback_audio_preset(e->audio, c, false);
    }
    double position = dm_audio_position(e->audio);
    ed_choose_program(e, 3, 40);
    recording_feedback_audio_render(e->audio, 256);
    check(dm_audio_playing(e->audio) && dm_audio_position(e->audio) >= position,
          "instrument choice preserves uninterrupted transport");
    check(recording_feedback_audio_preset(e->audio, 3, false) == 40,
          "playing synth updates immediately");
    for (int c = 0; c < 16; ++c) {
        check(e->project.channels[c].program == (c == 3   ? 40
                                                 : c == 9 ? 0
                                                          : c),
              "instrument choice edits only its target assignment");
        if (c != 3) {
            check(recording_feedback_audio_preset(e->audio, c, false) == before[c],
                  "unrelated synth channels keep their instruments");
        }
    }
    recording_feedback_audio_render(e->audio, 60000);
    check(recording_feedback_audio_preset(e->audio, 3, false) == 40,
          "later scheduled notes retain the chosen instrument without restart");
    e->instrument_channel = 3;
    SDL_KeyboardEvent key = {.scancode = SDL_SCANCODE_Z};
    check(ed_instrument_note(e, &key), "audition instrument-dialog key");
    recording_feedback_audio_render(e->audio, 256);
    check(e->held_channels[SDL_SCANCODE_Z] == 3 &&
              recording_feedback_audio_voice(e->audio, 3, e->held[SDL_SCANCODE_Z]) != UINT32_MAX,
          "instrument audition uses the selected channel, including external routing");
    check(recording_feedback_audio_voice(e->audio, 0, e->held[SDL_SCANCODE_Z]) == UINT32_MAX &&
              recording_feedback_audio_preset(e->audio, 0, false) == before[0],
          "audition does not touch unrelated channel one");
    ed_release_piano(e);
    e->instrument_channel = -1;
    dm_audio_transport(e->audio, false);
}
static void placements(Editor *e) {
    fixture(e);
    e->project.channels[0].program = 0;
    e->project.channels[2] = (DmChannel){true, false, false, 33, 100, 64};
    int index = ed_place_pattern(e, 0, 0, 0);
    check(index == 0, "click placement returns new index");
    ed_select_placement_route(e, index);
    ed_move_clip(e, &e->project.clips[index], 2, 1);
    check(e->project.clips[index].channel == 2 && e->pattern_channel == 2 &&
              e->selected_channel == 2 && e->preview_source_channel == 2 &&
              e->preview_program == 33,
          "moving selected clip rebinds editor, channel selection, and preview");
    int automation = dm_pattern_add(&e->project, "Automation");
    e->project.patterns[automation].kind = 1;
    e->project.patterns[automation].event = 11;
    check(ed_place_pattern(e, automation, 0, 0) == 1 && e->project.clips[1].lane == 1 &&
              e->channel_expanded[0],
          "automation click on parent creates visible subrow");
    e->clip_moved = false;
    ed_move_clip(e, &e->project.clips[1], 2, 2);
    check(e->project.clips[1].channel == 2 && e->project.clips[1].lane == 1 &&
              e->project.lane_count[2] == 1 && e->channel_expanded[2],
          "automation drag onto parent uses the same placement rule");
    int selected = e->selected_clip, undo_count = e->undo_count;
    check(ed_place_pattern(e, 0, 2 + 16, 0) == -1 && e->project.clip_count == 2 &&
              e->selected_clip == selected && e->undo_count == undo_count,
          "rejected note placement leaves selection and history alone");
    check(ed_place_pattern(e, 0, -1, 0) == -1 && ed_place_pattern(e, 0, 16 * 33, 0) == -1 &&
              ed_place_pattern(e, 0, 0, -1) == -1,
          "reject invalid placement destinations");
    e->selected_pattern = automation;
    e->selected_clip = 1;
    e->project.clips[1].selected = true;
    e->project.clips[0].selected = false;
    e->project.clips[1].lane = 3;
    e->project.lane_count[2] = 3;
    e->project.channels[4] = (DmChannel){true, false, false, 16, 100, 64};
    ed_transpose(e, -1);
    check(e->project.clips[1].channel == 4 && e->project.lane_count[4] == 3 &&
              e->channel_expanded[4] && e->pattern_channel == 4,
          "keyboard channel move creates destination automation rows and rebinds preview");
    e->playlist_context_clip = 1;
    ed_remove_placement(e, 0);
    check(e->selected_clip == 0 && e->playlist_context_clip == 0,
          "deleting earlier placement preserves correct selection references");
    ed_remove_placement(e, 0);
    check(e->selected_clip == -1 && e->playlist_context_clip == -1,
          "deleting selected placement clears references");
    fixture(e);
    int trimmed = ed_place_pattern(e, 0, 2, 10);
    e->project.clips[trimmed].length = 3;
    ed_split_clip(e, (size_t)trimmed, 11);
    check(e->project.clip_count == 2 && e->project.clips[0].channel == 2 &&
              e->project.clips[1].channel == 2 && e->project.clips[0].length == 1 &&
              e->project.clips[1].length == 2 && e->project.clips[1].beat == 11,
          "slicing trimmed note placement preserves channel and total extent");
    check(e->project.patterns[e->project.clips[1].pattern].count == 1,
          "slicing does not reveal notes beyond trimmed placement");
}
static void imports(void) {
    DmProject source, loaded;
    dm_project_init(&source);
    dm_project_init(&loaded);
    for (int c = 0; c < 16; ++c) {
        source.channels[c] = (DmChannel){true, false, false, c, 100, 64};
    }
    int id = dm_pattern_add(&source, "Used channel");
    dm_note_add(&source.patterns[id], (DmNote){2, 1, 60, 100, 0});
    dm_clip_add(&source, (DmClip){.pattern = id, .channel = 3});
    for (int c = 0; c < 16; ++c) {
        uint8_t bytes[] = {(uint8_t)(0xc0 | c), 40};
        DmBytes b = {bytes, 2, 2};
        dm_event_add(&source, 1, c, 0, &b);
    }
    uint8_t expression[] = {0xb5, 11, 70};
    DmBytes b = {expression, 3, 3};
    dm_event_add(&source, .5, 5, 0, &b);
    uint8_t marker[] = {0xff, 6, 4, 'T', 'e', 's', 't'};
    b = (DmBytes){marker, sizeof(marker), sizeof(marker)};
    dm_event_add(&source, 1, 0, 0, &b);
    DmBytes midi = {0};
    char error[512];
    check(dm_midi_export(&source, &midi, false, DM_METADATA_STRIP_ALL, error, sizeof(error)),
          "generate MIDI with unused program assignments");
    check(dm_import(&loaded, midi.data, midi.size, 0, 0, error, sizeof(error)),
          "import generated MIDI");
    for (int c = 0; c < 16; ++c) {
        check(loaded.channels[c].enabled == (c == 3 || c == 5),
              "only note-bearing and expressive-controller channels remain visible");
    }
    bool marker_found = false, later_program = false;
    for (size_t i = 0; i < loaded.event_count; ++i) {
        DmEvent event = loaded.events[i];
        uint8_t *m = event.message.data;
        if ((m[0] & 0xf0) == 0xc0) {
            check((m[0] & 15) == 3 || (m[0] & 15) == 5, "unused program events are discarded");
            later_program |= m[0] == 0xc3 && event.beat == 1;
        }
        marker_found |= m[0] == 0xff && m[1] == 6;
    }
    check(later_program, "program changes before a channel's first note are retained");
    check(marker_found, "unused-program cleanup preserves markers");
    check(loaded.pattern_count == 1 && loaded.patterns[0].count == 1 && loaded.bpm == source.bpm,
          "import cleanup preserves notes and timing");
    dm_bytes_free(&midi);
    dm_project_free(&source);
    dm_project_free(&loaded);
}
int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Channel routing", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "renderer");
    char error[512];
    DmAudio *audio = dm_audio_create(error, sizeof(error));
    check(audio && dm_audio_soundfont(audio, "agents/reports/generated-programs.sf2", error,
                                      sizeof(error)),
          "load generated multi-program SoundFont");
    uint64_t start = SDL_GetTicks();
    while (dm_audio_busy(audio)) {
        bool success = false;
        if (dm_audio_poll(audio, error, sizeof(error), &success, NULL)) {
            check(success, error);
        }
        if (SDL_GetTicks() - start > 30000) {
            check(false, "font load timeout");
        }
        SDL_Delay(5);
    }
    recording_feedback_audio_pause(audio);
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, audio, "", true);
    ui_frame();
    ui_root(0, 900);
    programs(e);
    placements(e);
    imports();
    ui_end();
    ui_render(r);
    fixture(e);
    test_channel_routing_ui(e, check);
    printf("PASS channel routing: %d checks\n", checks);
    editor_free(e);
    free(e);
    dm_audio_destroy(audio);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
