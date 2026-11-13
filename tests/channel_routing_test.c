#include "editor/internal.h"
#include <assert.h>
#define TEST_NAME "channel routing"
#include "test_common.h"
#include "../shared/pattern_generation.h"
extern int recording_feedback_audio_port_cc(DmAudio *, unsigned, int, int);
extern int recording_feedback_audio_port_bend(DmAudio *, unsigned, int);
extern bool recording_feedback_audio_port_voice(DmAudio *, unsigned, int, int);
extern void test_port_strips_ui(Editor *);
extern void test_note_color_popup_ui(Editor *);

extern void recording_feedback_audio_render(DmAudio *, int);
extern int recording_feedback_audio_preset(DmAudio *, int, bool);
extern int recording_feedback_audio_bank(DmAudio *, int, bool);
extern unsigned recording_feedback_audio_voice(DmAudio *, int, int);
extern void test_channel_routing_ui(Editor *, void (*)(bool, const char *));

static void fixture(Editor *e) {
    ed_release_piano(e);
    ed_new_project(e);
    e->project.clip_count = e->project.automation_count = 0;
    e->selected_pattern = 0;
    e->selected_channel = e->pattern_channel = 0;
    e->selected_clip = e->playlist_context_clip = -1;
    e->preview_override = false;
    e->preview_source_channel = -1;
    e->edit_focus = FOCUS_PLAYLIST;
    e->drag = DRAG_NONE;
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
    e->project.channels[2].enabled = true;
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

/* A pitch bend keeps its own root through branching and note edits. */
static void note_curves(Editor *e) {
    fixture(e);
    DmPattern *p = ed_pattern(e);
    e->automation_cc = DM_EVENT_BEND;
    e->automation_raw = false;
    e->bend_range = 12;
    ed_note_bend_line(e, .25, .75, 10240, &p->notes[0]);
    ed_note_bend_line(e, 2, 3, 12288, &p->notes[1]);

    size_t root = 0;

    while (e->project.automation[root].bend_note != p->notes[1].bend_id) {
        ++root;
        check(root < e->project.automation_count, "second section has an attached root");
    }

    DmAutomation branch = ed_split_curve_node(e, (int)root, e->project.automation[root + 1], 2.5);
    check(!branch.bend_note && dm_automation_add(&e->project, branch),
          "a branch belongs to the section without acquiring a root of its own");
    DmAutomation original[31];
    check(e->project.automation_count == SDL_arraysize(original), "section setup and resets");
    memcpy(original, e->project.automation, sizeof(original));

    DmNote before[3];
    memcpy(before, p->notes, sizeof(before));
    before[1].selected = 1;
    p->notes[1].key = 65;
    p->notes[1].beat = 3;
    ed_follow_note_curves(e, 0, before, 3, 0);
    p->notes[1].key = 66;
    p->notes[1].beat = 4;
    ed_follow_note_curves(e, 0, before, 3, 1);

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation expected = original[i];

        if (abs(expected.curve) == 2) {
            expected.beat += 2;

            if (expected.type == MIDI_PITCH_BEND) {
                expected.anchor = 66;
            }
        }

        check(ed_same_event(expected, e->project.automation[i]),
              "only the moved note's root, branch, range and reset move");
    }

    UiRect lane = {0, 0, 640, 300};
    View view = {0};
    ed_automation_lane_click(e, lane, &view, 80, (UiMouse){.pos = {320, 150}}, (int)root);
    ed_automation_node_drag(e, lane, &view, 80,
                            (UiMouse){.left = true, .shift = true, .pos = {340, 150}});
    ed_automation_node_drag(e, lane, &view, 80, (UiMouse){.released = true, .pos = {340, 150}});
    memcpy(original, e->project.automation, sizeof(original));
    memcpy(before, p->notes, sizeof(before));
    p->notes[1].beat = 5;
    p->notes[1].key = 69;
    ed_follow_note_curves(e, 0, before, 3, 0);

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        check(ed_same_event(original[i], e->project.automation[i]),
              "moving the root in Events detaches the whole section");
    }
}

static void independent_bends(Editor *e) {
    fixture(e);
    DmPattern *p = ed_pattern(e);
    p->notes[0].length = 2; /* Its end meets the next note's onset. */
    e->automation_cc = DM_EVENT_BEND;
    e->automation_raw = false;
    e->automation_center = 64;
    UiRect lane = {0, 0, 640, 300};
    View view = {0};
    UiMouse click = {.pos = {160, 150}, .released = true};
    e->drag = DRAG_EVENT_LINE;
    e->marquee_start = click.pos;
    ed_automation_drag(e, lane, &view, 80, click);
    check(e->project.automation_count == 1, "Events places one independent pitch point");
    DmAutomation point = e->project.automation[0];
    check(point.fixed_anchor && point.anchor == 64 && !point.bend_note &&
              ed_event_value(point) == MIDI_BEND_CENTER,
          "a pitch point placed on the new note's onset sets a neutral MIDI bend");

    e->automation_raw = true;
    e->drag = DRAG_EVENT_LINE;
    e->marquee_start = (UiPoint){480, 145};
    ed_automation_drag(e, lane, &view, 80,
                       (UiMouse){.pos = {560, 105}, .released = true, .alt = true});
    check(e->project.automation_count == 16, "MIDI Values places an independent curve with setup");
    DmAutomation original[16];
    memcpy(original, e->project.automation, sizeof(original));
    DmNote before[3];
    memcpy(before, p->notes, sizeof(before));
    p->notes[1].beat += 1;
    p->notes[1].key = 70;
    p->notes[2].beat += 1;
    p->notes[2].key = 72;
    ed_follow_note_curves(e, 0, before, 3, 0);

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        check(ed_same_event(original[i], e->project.automation[i]) &&
                  !e->project.automation[i].bend_note,
              "Events bends keep their stored position in pitch and MIDI value views");
    }

    check(ed_automation_pitch(e, point) == 64, "an independent point keeps its pitch reference");
    dm_pattern_fit(&e->project, p);

    DmBytes saved = {0};
    DmProject loaded;
    dm_project_init(&loaded);
    char error[256] = {0};
    check(dm_pack(&e->project, &saved) &&
              dm_unpack(&loaded, saved.data, saved.size, error, sizeof(error)) &&
              ed_same_event(loaded.automation[0], point),
          "saving and reopening preserves an independent point's pitch reference");
    dm_project_free(&loaded);
    dm_bytes_free(&saved);
}

static void scheduled_bend(Editor *e, int *start, int *middle, int *range, int *reset) {
    DmSchedule schedule = {0};
    check(dm_schedule(&e->project, &schedule), "build the pattern playback schedule");
    *start = *middle = *reset = -1;
    *range = 2;

    for (size_t i = 0; i < schedule.count; ++i) {
        DmScheduled event = schedule.items[i];
        int type = event.status & MIDI_STATUS_MASK;

        if (type == MIDI_CONTROL && event.a == MIDI_CC_DATA && event.seconds <= .25) {
            *range = event.b;
        }

        if (type != MIDI_PITCH_BEND) {
            continue;
        }

        int value = event.a + (event.b << 7);

        if (fabs(event.seconds - .25) < 1e-6) {
            *start = value;
        } else if (fabs(event.seconds - .5) < 1e-6) {
            *middle = value;
        } else if (fabs(event.seconds - .75) < 1e-6) {
            *reset = value;
        }
    }

    dm_schedule_free(&schedule);
}

static void detached_pitch_playback(Editor *e) {
    fixture(e);
    DmPattern *p = ed_pattern(e);
    p->notes[0].key = 55;
    p->notes[0].length = 1.5;
    p->notes[1].beat = 1.5;
    p->notes[1].key = 55;
    e->project.bpm = 120;
    e->automation_cc = DM_EVENT_BEND;
    e->automation_raw = false;
    e->automation_center = 55;
    e->bend_range = 12;
    check(dm_clip_add(&e->project, (DmClip){.pattern = 0}), "place the bend fixture");
    ed_note_bend_line(e, .5, 1.5, MIDI_BEND_CENTER, &p->notes[0]);
    size_t root = 0;

    while (e->project.automation[root].bend_root != DM_BEND_ROOT_PITCH) {
        ++root;
        check(root < e->project.automation_count, "drawn curve has a Pitch root");
    }

    UiRect lane = {0, 0, 640, 300};
    View view = {0};
    ed_automation_lane_click(e, lane, &view, 80, (UiMouse){.pos = {40, 150}}, (int)root);
    ed_automation_node_drag(e, lane, &view, 80,
                            (UiMouse){.left = true, .ctrl = true, .pos = {40, 210}});
    ed_automation_node_drag(e, lane, &view, 80, (UiMouse){.released = true, .pos = {40, 210}});

    DmAutomation *point = &e->project.automation[root];
    DmAutomation *end = point + 1;
    check(!point->bend_note && point->bend_root == DM_BEND_ROOT_PITCH &&
              fabs(ed_automation_pitch(e, *point) - 52) < .002,
          "editing the root in Pitch detaches it and retains its drawn pitch");

    p->notes[0].key = 52;
    int start, middle, range, reset;
    scheduled_bend(e, &start, &middle, &range, &reset);
    check(start == MIDI_BEND_CENTER && middle == 9216 && range == 12 && reset == MIDI_BEND_CENTER,
          "detached Pitch playback glides from the sounding note and restores the original value");
    check(ed_event_value(*point) == 6144 && ed_event_value(*end) == MIDI_BEND_CENTER,
          "compiling playback leaves the original curve and its display reference intact");

    p->notes[0].key = 53;
    scheduled_bend(e, &start, &middle, &range, &reset);
    check(start == 6144 && middle == 7168, "a root off the sounding pitch retains its offsets");
    p->notes[0].key = 52;
    p->notes[0].length = .5;
    scheduled_bend(e, &start, &middle, &range, &reset);
    check(start == 6144 && middle == 7168, "a note ending at the root is no longer sounding");

    p->notes[0].length = 1.5;
    ed_pitch_edit_units(e, end, true);
    scheduled_bend(e, &start, &middle, &range, &reset);
    check(point->bend_root == DM_BEND_ROOT_MIDI && start == 6144 && middle == 7168,
          "a MIDI Values edit preserves literal offsets throughout the section");

    p->notes[0].key = 46;
    ed_pitch_edit_units(e, point, false);
    ed_set_event_value(point, 2048);
    ed_set_event_value(end, 14336);
    scheduled_bend(e, &start, &middle, &range, &reset);
    check(start == MIDI_BEND_CENTER && range > 12 &&
              fabs((middle - MIDI_BEND_CENTER) * range / (double)MIDI_BEND_CENTER - 9) < .003,
          "the bend range expands when needed to preserve the glide's interval");
}

/* A channel's type changes in place, including when channel 10 is already in use. */
static void drum_channel(Editor *e) {
    fixture(e);
    e->project.channels[2].enabled = true;
    int placed = ed_place_pattern(e, 0, 2, 0), drum = ed_place_pattern(e, 0, MIDI_DRUM_CHANNEL, 0);
    check(placed >= 0 && drum >= 0, "drum fixture");
    ed_open_instrument(e, 2, false);
    e->instrument_drums = true;
    e->instrument_program = 25;
    ed_apply_instrument(e);
    check(e->project.clips[placed].channel == 2 && dm_channel_drums(&e->project, 2) &&
              e->project.channels[2].program == 25 &&
              e->project.clips[drum].channel == MIDI_DRUM_CHANNEL &&
              dm_channel_drums(&e->project, MIDI_DRUM_CHANNEL),
          "changing type keeps the selected channel and other percussion parts");
    DmProject imported;
    dm_project_init(&imported);
    DmBytes midi = {0};
    char error[256] = {0};
    check(dm_midi_export(&e->project, &midi, false, DM_METADATA_STRIP_ALL, error, sizeof(error)) &&
              dm_import(&imported, midi.data, midi.size, 0, 0, error, sizeof(error)) &&
              dm_channel_drums(&imported, 2) && imported.channels[2].program == 25,
          "a drum kit on channel 3 survives a standard MIDI round trip");
    dm_bytes_free(&midi);
    dm_project_free(&imported);
    ed_open_instrument(e, 2, false);
    e->instrument_program = 0;
    SDL_KeyboardEvent key = {.scancode = SDL_SCANCODE_Z};
    check(ed_instrument_note(e, &key), "audition the draft drum kit");
    recording_feedback_audio_render(e->audio, 256);
    check(recording_feedback_audio_bank(e->audio, 2, true) == 128,
          "audition uses the drum bank on channel 3");
    ed_apply_instrument(e);
    check(ed_transport_publish(e, 0, true), "start playback with the new drum kit");
    recording_feedback_audio_render(e->audio, 256);
    check(recording_feedback_audio_bank(e->audio, 2, false) == 128,
          "song playback uses the drum bank on channel 3");
    ed_open_instrument(e, 2, false);
    e->instrument_drums = false;
    ed_apply_instrument(e);
    recording_feedback_audio_render(e->audio, 256);
    check(dm_audio_playing(e->audio) && recording_feedback_audio_bank(e->audio, 2, false) == 0,
          "editing the type switches the bank during playback");
    dm_audio_transport(e->audio, false);
}

/* Opening program changes after beat 0 follow the Instrument dialog; later ones are kept. */
static void late_initial_program(Editor *e) {
    fixture(e);
    e->project.channels[3].enabled = true;
    ed_place_pattern(e, 0, 3, 2);
    uint8_t opening[] = {0xc3, 66}, later[] = {0xc3, 40};
    DmBytes message = {opening, 2, 2};
    dm_event_add(&e->project, 1, 3, 0, &message);
    message = (DmBytes){later, 2, 2};
    dm_event_add(&e->project, 5, 3, 0, &message);
    ed_open_instrument(e, 3, false);
    e->instrument_program = 0;
    ed_apply_instrument(e);
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
    ed_open_instrument(e, 3, false);
    e->instrument_program = 40;
    ed_apply_instrument(e);
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
    ed_open_instrument(e, 3, false);
    SDL_KeyboardEvent key = {.scancode = SDL_SCANCODE_Z};
    check(ed_instrument_note(e, &key), "audition instrument-dialog key");
    recording_feedback_audio_render(e->audio, 256);
    check(e->held_channels[SDL_SCANCODE_Z] == 3 &&
              recording_feedback_audio_voice(e->audio, 3, e->held[SDL_SCANCODE_Z]) != UINT32_MAX,
          "instrument audition uses the selected channel, including external routing");
    check(recording_feedback_audio_voice(e->audio, 0, e->held[SDL_SCANCODE_Z]) == UINT32_MAX &&
              recording_feedback_audio_preset(e->audio, 0, false) == before[0],
          "audition does not touch unrelated channel one");
    ed_close_instrument(e);
    recording_feedback_audio_render(e->audio, 256);
    position = dm_audio_position(e->audio);
    check(ed_swap_channels(e, 3, 4), "move a channel while playing");
    recording_feedback_audio_render(e->audio, 256);
    check(dm_audio_playing(e->audio) && dm_audio_position(e->audio) >= position,
          "moving a channel keeps the transport running from its position");
    check(recording_feedback_audio_preset(e->audio, 3, false) == 4 &&
              recording_feedback_audio_preset(e->audio, 4, false) == 40,
          "moved channels play their new instruments without restarting playback");
    ed_undo(e, false);
    recording_feedback_audio_render(e->audio, 256);
    check(recording_feedback_audio_preset(e->audio, 3, false) == 40 &&
              recording_feedback_audio_preset(e->audio, 4, false) == 4,
          "undoing a channel move restores the playing instruments");
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
    e->project.channels[2].enabled = true;
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

/* A pattern not on the playlist takes the selected channel's instrument until one is chosen in
   the pattern editor; a placed pattern shows the dropdown only in pattern playback. */
static void unplaced_preview(Editor *e) {
    fixture(e);
    e->pattern_mode = false;
    e->project.channels[0].program = 0;
    e->project.channels[2] = (DmChannel){true, false, false, 33, 100, 64};
    e->project.channels[9] = (DmChannel){true, false, false, 0, 100, 64};
    e->project.channels[9].drums = true;
    ed_open_pattern(e, 0, false);
    check(!ed_pattern_placed(e) && ed_preview_instrument_shown(e),
          "unplaced pattern shows its instrument dropdown");
    e->selected_channel = 2;
    e->channel_selected = true;
    ed_follow_preview_frame(e);
    check(e->pattern_channel == 2 && e->preview_program == 33 && !e->preview_override,
          "selecting a playlist channel switches the unplaced pattern's instrument");
    ed_open_instrument(e, 2, false);
    e->instrument_program = 40;
    ed_apply_instrument(e);
    check(e->preview_program == 40, "changing the selected channel's instrument follows at once");
    e->pattern_mode = true;
    e->selected_channel = 9;
    ed_follow_preview_frame(e);
    check(e->pattern_channel == 9 && e->preview_program == 128,
          "selecting a drum channel switches to percussion in pattern playback");
    e->pattern_mode = false;
    e->selected_channel = 2;
    ed_follow_preview_frame(e);
    ed_select_preview_instrument(e, 5);
    check(e->preview_override && e->preview_program == 5, "dropdown choice is explicit");
    e->selected_channel = 0;
    ed_follow_preview_frame(e);
    check(e->preview_program == 5 && e->pattern_channel == 2,
          "explicit choice persists when another channel is selected");
    ed_open_instrument(e, 2, false);
    e->instrument_program = 41;
    ed_apply_instrument(e);
    ed_follow_preview_frame(e);
    check(e->preview_program == 5, "explicit choice persists when a channel's instrument changes");
    ed_reset_preview_instrument(e);
    check(!e->preview_override && e->preview_program == 0,
          "reset returns to the selected channel's instrument");
    e->selected_channel = 2;
    ed_follow_preview_frame(e);
    check(e->pattern_channel == 2 && e->preview_program == 41,
          "after reset, channel selection switches the instrument again");
    e->channel_selected = false;
    e->selected_channel = 0;
    ed_follow_preview_frame(e);
    check(e->pattern_channel == 2 && e->preview_program == 41,
          "clearing the channel selection keeps the last instrument");

    int index = ed_place_pattern(e, 0, 2, 0);
    check(index >= 0 && ed_pattern_placed(e), "pattern is placed");
    ed_select_placement_route(e, index);
    check(!ed_preview_instrument_shown(e), "placed pattern hides its dropdown in song playback");
    e->pattern_mode = true;
    check(ed_preview_instrument_shown(e), "pattern playback shows the dropdown again");
    e->pattern_mode = false;
    e->selected_channel = 0;
    e->channel_selected = true;
    ed_follow_preview_frame(e);
    check(e->pattern_channel == 2 && e->preview_program == 41,
          "placed pattern keeps its placement's instrument when another channel is selected");
    ed_remove_placement(e, index);
    ed_follow_preview_frame(e);
    check(!ed_pattern_placed(e) && e->preview_source_channel < 0 && e->pattern_channel == 0 &&
              e->preview_program == 0,
          "removing the last placement follows the selected channel again");
}

static void imports(void) {
    DmProject source, loaded;
    dm_project_init(&source);
    dm_project_init(&loaded);

    for (int c = 0; c < 16; ++c) {
        source.channels[c] = (DmChannel){true, false, false, c, 100, 64, c == MIDI_DRUM_CHANNEL};
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

/* Notes, controllers and bends on ports 0, 1 and 127 stay apart in MIDI, the project file,
   generated patterns, live playback and a rendered file. */
static void independent_ports(Editor *e) {
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
    assert(dm_audio_soundfont(e->audio, TEST_SOUNDFONT, error, sizeof(error)));
    assert(test_audio_wait(e->audio));
    assert(dm_audio_publish(e->audio, p, 0, true, error, sizeof(error)));
    recording_feedback_audio_render(e->audio, 128);
    assert(recording_feedback_audio_port_cc(e->audio, 127, 0, 11) == 64);
    dm_audio_transport(e->audio, false);
    snprintf(p->soundfont, DM_PATH, TEST_SOUNDFONT);
    assert(dm_audio_export(e->audio, p, "agents/reports/channel-routing-ports.wav", NULL, error,
                           sizeof(error)));
    assert(test_audio_wait(e->audio));
    e->selected_pattern = e->pattern_channel = 0;
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

/* Each port of a channel has its own volume, pan, mute and solo. */
static void port_mixer(Editor *e) {
    dm_project_free(&e->project);
    dm_project_init(&e->project);
    e->active = true;
    e->selected_pattern = dm_pattern_add(&e->project, "Ports");
    e->selected_channel = e->pattern_channel = 0;
    e->channel_selected = true;
    e->project.channels[0].program = 0;
    DmPattern *p = ed_pattern(e);
    p->length = 1;
    assert(dm_note_add(p, (DmNote){0, .5, 60, 100, 0, 0}) &&
           dm_note_add(p, (DmNote){0, .5, 72, 100, 0, 1}));
    assert(dm_clip_add(&e->project, (DmClip){.pattern = 0, .channel = 0}));
    assert(dm_port_mix_set(&e->project, (DmPortMix){0, 0, 70, 20, false, false}));
    assert(dm_port_mix_set(&e->project, (DmPortMix){1, 0, 35, 100, false, false}));
    DmBytes data = {0};
    DmProject copy;
    dm_project_init(&copy);
    char error[512];
    assert(dm_pack(&e->project, &data) &&
           dm_unpack(&copy, data.data, data.size, error, sizeof(error)));
    assert(dm_port_mix(&copy, 1, 0).volume == 35 && dm_port_mix(&copy, 0, 0).pan == 20);
    assert(dm_project_copy(&copy, &e->project));
    copy.port_mix[0].volume = 99;
    assert(e->project.port_mix[0].volume == 35);
    dm_project_free(&copy);
    dm_bytes_free(&data);
    assert(dm_audio_publish(e->audio, &e->project, 0, true, error, sizeof(error)));
    recording_feedback_audio_render(e->audio, 128);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 0, 7) == 70 &&
           recording_feedback_audio_port_cc(e->audio, 1, 0, 7) == 35);
    dm_audio_part_control(e->audio, 1, 0, 7, 88);
    recording_feedback_audio_render(e->audio, 64);
    assert(recording_feedback_audio_port_cc(e->audio, 0, 0, 7) == 70 &&
           recording_feedback_audio_port_cc(e->audio, 1, 0, 7) == 88);
    DmPortMix mix = dm_port_mix(&e->project, 1, 0);
    mix.mute = true;
    assert(dm_port_mix_set(&e->project, mix));
    DmExportEvent *events = NULL;
    size_t count;
    uint64_t duration;
    assert(dm_export_timeline(&e->project, &events, &count, &duration, error, sizeof(error)));
    int notes[2] = {0};

    for (size_t i = 0; i < count; i++) {
        if ((events[i].status & 0xf0) == 0x90 && events[i].b) {
            notes[events[i].port]++;
        }
    }

    assert(notes[0] == 1 && notes[1] == 0);
    free(events);
    mix.mute = false;
    mix.solo = true;
    assert(dm_port_mix_set(&e->project, mix));
    assert(dm_export_timeline(&e->project, &events, &count, &duration, error, sizeof(error)));
    notes[0] = notes[1] = 0;

    for (size_t i = 0; i < count; i++) {
        if ((events[i].status & 0xf0) == 0x90 && events[i].b) {
            notes[events[i].port]++;
        }
    }

    assert(notes[0] == 0 && notes[1] == 1);
    free(events);
    mix.solo = false;
    assert(dm_port_mix_set(&e->project, mix));
    assert(dm_midi(&e->project, &data, error, sizeof(error)));
    assert(data.size);
    dm_bytes_free(&data);
    dm_audio_transport(e->audio, false);
    puts(
        "PASS independent port volume/pan, live routing, mute/solo, mixer persistence and deep copy");
}

int main(void) {
    Editor *e = test_editor_open("Channel routing",
                                 (TestDesktop){.soundfont = TEST_SOUNDFONT, .paused = true});
    SDL_Renderer *r = e->renderer;
    ui_frame();
    ui_root(0, 900);
    programs(e);
    note_curves(e);
    independent_bends(e);
    detached_pitch_playback(e);
    drum_channel(e);
    late_initial_program(e);
    delete_port(e);
    placements(e);
    unplaced_preview(e);
    imports();
    ui_end();
    ui_render(r);
    fixture(e);
    test_channel_routing_ui(e, check);
    independent_ports(e);
    test_note_color_popup_ui(e);
    port_mixer(e);
    test_port_strips_ui(e);
    printf("PASS channel routing: %d checks\n", checks);
    test_editor_close(e);
}
