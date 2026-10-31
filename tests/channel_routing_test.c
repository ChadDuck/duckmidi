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

/* Note on/off events of the active arrangement, for comparing slices with the unsliced source. */
static size_t note_timeline(const DmProject *p, DmExportEvent *out, size_t capacity) {
    DmExportEvent *events = NULL;
    size_t count = 0, used = 0;
    uint64_t duration = 0;
    char error[256];
    check(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)),
          "slice fixture exports a timeline");

    for (size_t i = 0; i < count; ++i) {
        uint32_t type = events[i].status & 0xf0;

        if ((type == 0x90 || type == 0x80) && used < capacity) {
            out[used] = events[i];
            out[used].source_id = 0;
            out[used].status =
                (events[i].status & 0x0f) | (type == 0x90 && events[i].b ? 0x90 : 0x80);
            ++used;
        }
    }

    free(events);
    return used;
}

static bool same_notes(const DmExportEvent *a, size_t a_count, const DmExportEvent *b,
                       size_t b_count) {
    if (a_count != b_count) {
        return false;
    }

    for (size_t i = 0; i < a_count; ++i) {
        bool found = false;

        for (size_t j = 0; j < b_count && !found; ++j) {
            found = a[i].tick == b[j].tick && a[i].status == b[j].status && a[i].a == b[j].a;
        }

        if (!found) {
            return false;
        }
    }

    return true;
}

static void linked_slices(Editor *e) {
    fixture(e);
    int placed = ed_place_pattern(e, 0, 2, 4);
    e->project.clips[placed].length = 6.5; /* Resized before slicing; hides the last note. */
    DmExportEvent before[32], after[32];
    size_t before_count = note_timeline(&e->project, before, 32);

    /* Slice repeatedly at arbitrary points, including through sustained notes and slices of
       slices. Playback must not change, and every piece must stay linked to pattern 0. */
    const double cuts[] = {5.5, 9, 7.25, 5.75, 8};

    for (size_t c = 0; c < sizeof(cuts) / sizeof(*cuts); ++c) {
        for (size_t i = 0; i < e->project.clip_count; ++i) {
            DmClip clip = e->project.clips[i];

            if (cuts[c] > clip.beat && cuts[c] < clip.beat + dm_clip_length(&e->project, clip)) {
                ed_split_clip(e, i, cuts[c]);
                break;
            }
        }
    }

    size_t after_count = note_timeline(&e->project, after, 32);
    double extent = 0;
    bool linked = e->project.pattern_count == 1;

    for (size_t i = 0; i < e->project.clip_count; ++i) {
        DmClip clip = e->project.clips[i];
        linked &= clip.pattern == 0 && fabs(clip.offset - (clip.beat - 4)) < 1e-9;
        extent += dm_clip_length(&e->project, clip);
    }

    check(e->project.clip_count == 6 && linked && fabs(extent - 6.5) < 1e-9,
          "arbitrary slices remain linked windows covering the resized placement");
    check(same_notes(before, before_count, after, after_count),
          "arbitrary slices play the same notes, shortening notes cut at slice edges");

    /* Resizing a right-hand slice keeps its offset, so it reveals later pattern content. */
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].beat == 9) {
            e->project.clips[i].length = 2;
            check(e->project.clips[i].offset == 5, "resized slice keeps its pattern offset");
        }
    }
}

/* A bend drawn on a note keeps its register and timing when the note moves. */
static void note_curves(Editor *e) {
    fixture(e);
    DmAutomation start = {2, 0xe0, 0, 0, 64, 0, 5, 64, 0, false};
    DmAutomation end = start, restore = start, other = start;
    end.beat = 3;
    end.b = 96;
    restore.curve = -5;
    restore.beat = 3;
    other.curve = 6;
    other.anchor = 60;
    other.beat = .5;
    dm_automation_add(&e->project, start);
    dm_automation_add(&e->project, end);
    dm_automation_add(&e->project, restore);
    dm_automation_add(&e->project, other);
    DmPattern *p = ed_pattern(e);
    DmNote before[3];
    memcpy(before, p->notes, sizeof(before));
    p->notes[1].key = 66;
    p->notes[1].beat = 4;
    ed_follow_note_curves(e, 0, before, 3);
    bool followed = true, untouched = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];

        if (abs(a.curve) == 5) {
            followed &= a.anchor == 66 && (a.beat == 4 || a.beat == 5);
        } else {
            untouched = a.curve == 6 && a.anchor == 60 && a.beat == .5;
        }
    }

    check(followed, "a moved note carries its drawn bend and re-anchors it to the new key");
    check(untouched, "curves on other notes stay in place");
}

/* Any channel can become the drum channel when channel 10 has no placements. */
static void drum_channel(Editor *e) {
    fixture(e);
    ed_remove_empty_channel(e, DM_DRUM_CHANNEL);
    int placed = ed_place_pattern(e, 0, 2, 0);
    check(placed >= 0 && !e->project.channels[DM_DRUM_CHANNEL].enabled, "drum fixture");
    ed_choose_drums(e, 2);
    check(e->project.clips[placed].channel == DM_DRUM_CHANNEL &&
              e->project.channels[DM_DRUM_CHANNEL].enabled &&
              e->project.channels[DM_DRUM_CHANNEL].program == 0 && !e->project.channels[2].enabled,
          "choosing Drum kit moves the channel and its placements to channel 10");
    int other = ed_place_pattern(e, 0, 3, 0);
    ed_choose_drums(e, 3);
    check(e->project.clips[other].channel == 3, "an occupied channel 10 is left untouched");
    check(dm_drum_kit_name(25) && !dm_drum_kit_name(1), "channel 10 programs name drum kits");
}

/* Opening program changes after beat 0 follow the Instrument dialog; later ones are kept. */
static void late_initial_program(Editor *e) {
    fixture(e);
    ed_place_pattern(e, 0, 3, 2);
    uint8_t opening[] = {0xc3, 66}, later[] = {0xc3, 40};
    DmBytes message = {opening, 2, 2};
    dm_event_add(&e->project, 1, 3, 0, &message);
    message = (DmBytes){later, 2, 2};
    dm_event_add(&e->project, 5, 3, 0, &message);
    ed_choose_program(e, 3, 0);
    int opening_program = -1, later_program = -1;

    for (size_t i = 0; i < e->project.event_count; ++i) {
        DmEvent event = e->project.events[i];

        if (event.message.data[0] == 0xc3) {
            *(event.beat < 2 ? &opening_program : &later_program) = event.message.data[1];
        }
    }

    check(opening_program == 0 && later_program == 40 && e->project.channels[3].program == 0,
          "instrument choice replaces program changes before the channel's first note only");
}

/* Deleting a colored port returns its notes to the default port and drops its events. */
static void delete_port(Editor *e) {
    fixture(e);
    ed_place_pattern(e, 0, 0, 0);
    e->pattern_channel = 0;
    ed_pattern(e)->notes[1].port = 2;
    dm_automation_add(&e->project, (DmAutomation){1, 0xb0, 0, 1, 90, 0, 0, 0, 0, false, 2});
    dm_automation_add(&e->project, (DmAutomation){1, 0xb0, 0, 1, 40, 0, 0, 0, 0, false, 0});
    dm_port_mix_set(&e->project, (DmPortMix){2, 0, 80, 64, false, false});
    check(ed_delete_note_port(e, 2), "port deletion runs");
    check(ed_pattern(e)->notes[1].port == 0 && e->project.automation_count == 1 &&
              e->project.automation[0].port == 0,
          "deleted port's notes use the default port and its events are discarded");
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
    check(e->project.clips[0].pattern == e->project.clips[1].pattern &&
              e->project.pattern_count == 1 && e->project.clips[0].offset == 0 &&
              e->project.clips[1].offset == 1,
          "slicing creates two linked windows into the same pattern");
    linked_slices(e);
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
    editor_init(e, w, r, audio, "", "", true);
    ui_frame();
    ui_root(0, 900);
    programs(e);
    note_curves(e);
    drum_channel(e);
    late_initial_program(e);
    delete_port(e);
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
