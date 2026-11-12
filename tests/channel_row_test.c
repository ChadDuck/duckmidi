#include "editor/internal.h"
#define TEST_NAME "channel rows"
#include "test_common.h"
void test_channel_row_ui(Editor *e, void (*check)(bool, const char *));
extern void recording_feedback_audio_render(DmAudio *, int);
extern int recording_feedback_audio_bank(DmAudio *, int, bool);

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

static void channel_swap(Editor *e) {
    ed_new_project(e);
    DmProject *p = &e->project;
    p->channels[0] = (DmChannel){true, true, false, 25, 80, 32, false};
    p->channels[MIDI_DRUM_CHANNEL] = (DmChannel){true, false, true, 16, 90, 96, true};
    p->lane_count[0] = 2;
    p->lane_count[MIDI_DRUM_CHANNEL] = 1;
    p->note_rows[0] = 3;
    p->note_rows[MIDI_DRUM_CHANNEL] = 2;
    strcpy(p->lane_names[0][1], "Melody lane");
    strcpy(p->lane_names[MIDI_DRUM_CHANNEL][0], "Drum lane");
    int lane = dm_pattern_add(p, "Swap automation");
    check(lane >= 0, "swap automation pattern");
    p->patterns[lane].kind = 1;
    check(dm_clip_add(p, (DmClip){.pattern = 0, .channel = 0, .row = 2}) &&
              dm_clip_add(p, (DmClip){.pattern = lane, .channel = MIDI_DRUM_CHANNEL, .lane = 1}),
          "swap placements on both channels");
    check(dm_automation_add(p, (DmAutomation){.pattern = -1,
                                              .channel = 0,
                                              .type = MIDI_CONTROL,
                                              .a = MIDI_CC_VOLUME,
                                              .b = 70}) &&
              dm_automation_add(p, (DmAutomation){.pattern = lane,
                                                  .channel = MIDI_DRUM_CHANNEL,
                                                  .type = MIDI_CONTROL,
                                                  .a = MIDI_CC_PAN,
                                                  .b = 40}),
          "swap song and pattern automation");

    for (int status = MIDI_NOTE_OFF; status < MIDI_SYSTEM; status += MIDI_CHANNELS) {
        for (int i = 0; i < 2; ++i) {
            uint8_t bytes[] = {(uint8_t)(status | (i ? MIDI_DRUM_CHANNEL : 0)), 60, 70};
            DmBytes message = {bytes, (status & MIDI_SHORT_MASK) == MIDI_PROGRAM ? 2 : 3, 3};
            check(dm_event_add(p, 2, 7 + i, 0, &message), "swap raw channel message");
        }
    }

    uint8_t tempo[] = {MIDI_META, MIDI_META_TEMPO, 3, 7, 161, 32};
    DmBytes message = {tempo, sizeof(tempo), sizeof(tempo)};
    check(dm_event_add(p, 3, 8, 0, &message), "swap conductor event");
    check(dm_port_mix_set(
              p,
              (DmPortMix){
                  .port = 2, .channel = 0, .volume = 50, .pan = 31, .mute = true, .program = 42}) &&
              dm_port_mix_set(p, (DmPortMix){.port = 2,
                                             .channel = MIDI_DRUM_CHANNEL,
                                             .volume = 60,
                                             .pan = 91,
                                             .solo = true}),
          "swap port mixer settings");
    DmPatchReplacement replacement = {.key = {.port = 2, .channel = 0, .program = 25}};
    check(dm_patch_replacement_set(p, &replacement), "swap melodic sample route");
    replacement.key.channel = MIDI_DRUM_CHANNEL;
    replacement.key.drums = true;
    check(dm_patch_replacement_set(p, &replacement), "swap drum sample route");
    dm_patterns_fit(p);
    DmBytes original = {0}, restored = {0};
    check(dm_pack(p, &original), "pack swap baseline");
    check(!dm_channel_swap(p, -1, 0) && !dm_channel_swap(p, 0, MIDI_CHANNELS) &&
              !dm_channel_swap(p, 0, 0),
          "invalid and identical channel swaps do nothing");
    e->channel_selected = true;
    e->selected_channel = e->pattern_channel = e->piano_channel = 0;
    e->preview_source_channel = 0;
    e->channel_expanded[0] = true;
    e->channel_expanded[MIDI_DRUM_CHANNEL] = false;
    e->note_ports[0] = 2;
    e->note_ports[MIDI_DRUM_CHANNEL] = 3;
    e->reset_channels[0] = p->channels[0];
    e->reset_channels[MIDI_DRUM_CHANNEL] = p->channels[MIDI_DRUM_CHANNEL];
    e->lane_channel = e->song_event_channel = e->remove_note_row_channel = 0;
    e->last_automation.channel = 0;
    e->instrument_source.key.channel = 0;
    e->midi_sample_source.key.channel = MIDI_DRUM_CHANNEL;
    e->mixer_peaks[2 * MIDI_CHANNELS] = .75f;
    int undo = e->undo_count;
    uint64_t revision = e->export_revision;

    for (int focus = FOCUS_NONE; focus <= FOCUS_OTHER; ++focus) {
        if (focus != FOCUS_PLAYLIST) {
            e->edit_focus = (EditFocus)focus;
            ed_shortcut(e, SDL_SCANCODE_DOWN, SDL_KMOD_ALT, false);
            check(p->clips[0].channel == 0 && e->undo_count == undo,
                  "channel shortcut requires actual playlist focus");
        }
    }

    e->edit_focus = FOCUS_PLAYLIST;
    e->channel_selected = false;
    ed_shortcut(e, SDL_SCANCODE_DOWN, SDL_KMOD_ALT, false);
    check(p->clips[0].channel == 0 && e->undo_count == undo,
          "channel shortcut requires a selected channel");
    e->channel_selected = true;
    ed_shortcut(e, SDL_SCANCODE_UP, SDL_KMOD_ALT, false);
    check(e->undo_count == undo, "moving the first channel up is a no-op");

    for (int i = 0; i < ACT_COUNT; ++i) {
        if (i != ACT_CHANNEL_UP && i != ACT_CHANNEL_DOWN) {
            check(!ed_same_binding(e->bindings[i], e->bindings[ACT_CHANNEL_UP]) &&
                      !ed_same_binding(e->bindings[i], e->bindings[ACT_CHANNEL_DOWN]),
                  "move channel defaults do not collide with other shortcuts");
        }
    }

    ed_shortcut(e, SDL_SCANCODE_DOWN, SDL_KMOD_ALT, false);
    check(e->undo_count == undo + 1 && e->dirty && e->export_revision > revision,
          "one channel move is one changed and published edit");
    check(p->channels[MIDI_DRUM_CHANNEL].program == 25 && p->channels[MIDI_DRUM_CHANNEL].mute &&
              !p->channels[MIDI_DRUM_CHANNEL].drums && p->channels[0].program == 16 &&
              p->channels[0].solo && p->channels[0].drums,
          "channel identity, instrument, volume, pan and rhythm type move across channel 10");
    check(p->channels[MIDI_DRUM_CHANNEL].volume == 80 && p->channels[MIDI_DRUM_CHANNEL].pan == 32 &&
              p->channels[0].volume == 90 && p->channels[0].pan == 96 &&
              p->lane_count[MIDI_DRUM_CHANNEL] == 2 && p->lane_count[0] == 1 &&
              p->note_rows[MIDI_DRUM_CHANNEL] == 3 && p->note_rows[0] == 2 &&
              !strcmp(p->lane_names[MIDI_DRUM_CHANNEL][1], "Melody lane") &&
              !strcmp(p->lane_names[0][0], "Drum lane"),
          "channel rows, lane names and mixes swap");
    check(p->clips[0].channel == MIDI_DRUM_CHANNEL && p->clips[0].row == 2 &&
              p->clips[1].channel == 0 && p->clips[1].lane == 1 &&
              p->automation[0].channel == MIDI_DRUM_CHANNEL && p->automation[1].channel == 0,
          "clips and both automation scopes keep their owning channel");

    for (size_t i = 0; i + 1 < p->event_count; ++i) {
        const DmEvent *event = &p->events[i];
        check((event->message.data[0] & MIDI_CHANNEL_MASK) == (i % 2 ? 0 : MIDI_DRUM_CHANNEL) &&
                  event->track == 7 + (int)(i % 2) && event->beat == 2 && event->port == 0 &&
                  event->message.data[1] == 60,
              "raw channel messages change channel without changing track, port, time or data");
    }

    check(!memcmp(p->events[p->event_count - 1].message.data, tempo, sizeof(tempo)),
          "conductor events stay unchanged");
    check(p->port_mix[0].channel == MIDI_DRUM_CHANNEL && p->port_mix[0].program == 42 &&
              p->port_mix[0].volume == 50 && p->port_mix[0].mute && p->port_mix[1].channel == 0 &&
              p->port_mix[1].solo && p->patch_replacements[0].key.channel == MIDI_DRUM_CHANNEL &&
              p->patch_replacements[1].key.channel == 0 && p->patch_replacements[1].key.drums,
          "port mixer and sample replacements keep their owning channel");
    check(e->selected_channel == MIDI_DRUM_CHANNEL && e->pattern_channel == MIDI_DRUM_CHANNEL &&
              e->piano_channel == MIDI_DRUM_CHANNEL &&
              e->preview_source_channel == MIDI_DRUM_CHANNEL &&
              e->channel_expanded[MIDI_DRUM_CHANNEL] && !e->channel_expanded[0] &&
              e->note_ports[MIDI_DRUM_CHANNEL] == 2 && e->note_ports[0] == 3 &&
              e->reset_channels[MIDI_DRUM_CHANNEL].program == 25 &&
              e->lane_channel == MIDI_DRUM_CHANNEL && e->song_event_channel == MIDI_DRUM_CHANNEL &&
              e->remove_note_row_channel == MIDI_DRUM_CHANNEL &&
              e->last_automation.channel == MIDI_DRUM_CHANNEL &&
              e->instrument_source.key.channel == MIDI_DRUM_CHANNEL &&
              e->midi_sample_source.key.channel == 0 &&
              e->mixer_peaks[2 * MIDI_CHANNELS + MIDI_DRUM_CHANNEL] == .75f,
          "selection, previews and per-channel editor state follow across disabled channels");
    ed_shortcut(e, SDL_SCANCODE_DOWN, SDL_KMOD_ALT, true);
    check(e->undo_count == undo + 1, "moving the final enabled channel down is a no-op");
    ed_undo(e, false);
    check(dm_pack(p, &restored) && original.size == restored.size &&
              !memcmp(original.data, restored.data, original.size),
          "one undo restores every byte of channel-owned project data");
    dm_bytes_free(&restored);
    ed_undo(e, true);
    check(p->clips[0].channel == MIDI_DRUM_CHANNEL && p->channels[0].drums,
          "redo restores the channel swap");
    Binding down = e->bindings[ACT_CHANNEL_DOWN];
    e->bindings[ACT_CHANNEL_DOWN] =
        (Binding){.key = SDL_SCANCODE_DOWN, .mods = SDL_KMOD_CTRL | SDL_KMOD_ALT};
    e->selected_channel = 0;
    undo = e->undo_count;
    ed_shortcut(e, SDL_SCANCODE_DOWN, SDL_KMOD_ALT, false);
    check(e->undo_count == undo, "rebinding removes the old move shortcut");
    ed_shortcut(e, SDL_SCANCODE_DOWN, SDL_KMOD_CTRL | SDL_KMOD_ALT, false);
    check(p->clips[0].channel == 0 && e->selected_channel == MIDI_DRUM_CHANNEL,
          "rebound move shortcut exchanges the channel numbers");
    e->bindings[ACT_CHANNEL_DOWN] = down;
    ed_shortcut(e, SDL_SCANCODE_UP, SDL_KMOD_ALT, false);
    check(e->selected_channel == 0 && p->clips[0].channel == MIDI_DRUM_CHANNEL,
          "Alt Up moves the selected channel to the preceding enabled row");
    dm_bytes_free(&original);
}

static void channel_swap_drums(Editor *e) {
    ed_new_project(e);
    DmProject *p = &e->project;
    check(dm_note_add(&p->patterns[0], (DmNote){.length = 1, .key = 60, .velocity = 100}),
          "melodic channel swap note");
    int drums = dm_pattern_add(p, "Percussion");
    check(drums >= 0 &&
              dm_note_add(&p->patterns[drums], (DmNote){.length = 1, .key = 38, .velocity = 90}) &&
              dm_clip_add(p, (DmClip){.pattern = 0, .channel = 0}) &&
              dm_clip_add(p, (DmClip){.pattern = drums, .channel = MIDI_DRUM_CHANNEL}),
          "melodic and percussion swap arrangement");
    e->selected_channel = 0;
    e->channel_selected = true;
    e->edit_focus = FOCUS_PLAYLIST;
    ed_shortcut(e, SDL_SCANCODE_DOWN, SDL_KMOD_ALT, false);
    check(ed_transport_publish(e, 0, true), "play swapped melody and percussion channels");
    recording_feedback_audio_render(e->audio, 256);
    check(recording_feedback_audio_bank(e->audio, MIDI_DRUM_CHANNEL, false) == 0 &&
              recording_feedback_audio_bank(e->audio, 0, false) == 128,
          "synth playback keeps melody on the melodic bank and percussion on the drum bank");
    dm_audio_transport(e->audio, false);
    DmSchedule schedule = {0};
    check(dm_schedule(p, &schedule), "swapped channels build a playback schedule");
    bool melody = false, percussion = false, melody_part = false, drum_part = false;

    for (size_t i = 0; i < schedule.count; ++i) {
        const DmScheduled *event = &schedule.items[i];
        melody |= event->status == (MIDI_NOTE_ON | MIDI_DRUM_CHANNEL) && event->a == 60 && event->b;
        percussion |= event->status == MIDI_NOTE_ON && event->a == 38 && event->b;

        if (event->status == MIDI_SYSEX && event->sysex.size == 10) {
            const uint8_t *data = event->sysex.data;
            melody_part |= data[0] == 0x41 && data[5] == 0x10 && data[6] == 0x15 && data[7] == 0;
            drum_part |= data[0] == 0x41 && data[5] == 0x11 && data[6] == 0x15 && data[7] == 1;
        }
    }

    check(
        melody && percussion && melody_part && drum_part,
        "playback moves notes and sends melodic channel 10 and percussion channel 1 rhythm SysEx");
    dm_schedule_free(&schedule);
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration = 0;
    char error[256];
    check(dm_export_timeline(p, &events, &count, &duration, error, sizeof(error)),
          "swapped channels export a timeline");
    melody = percussion = false;
    int sysex = 0;

    for (size_t i = 0; i < count; ++i) {
        melody |= events[i].status == (MIDI_NOTE_ON | MIDI_DRUM_CHANNEL) && events[i].a == 60;
        percussion |= events[i].status == MIDI_NOTE_ON && events[i].a == 38;
        sysex += events[i].status == MIDI_SYSEX;
    }

    check(melody && percussion && sysex == 2,
          "export timeline includes moved notes and rhythm SysEx");
    free(events);
    DmBytes midi = {0};
    DmProject imported;
    dm_project_init(&imported);
    check(dm_midi_export(p, &midi, false, DM_METADATA_STRIP_ALL, error, sizeof(error)) &&
              dm_import(&imported, midi.data, midi.size, 0, 0, error, sizeof(error)) &&
              imported.channels[0].drums && !imported.channels[MIDI_DRUM_CHANNEL].drums,
          "standard MIDI round trip preserves swapped percussion and melodic channel types");
    check(dm_channel_swap(&imported, 0, MIDI_DRUM_CHANNEL), "swap imported GS rhythm parts back");
    int rhythm_parts = 0;

    for (size_t i = 0; i < imported.event_count; ++i) {
        const DmBytes *message = &imported.events[i].message;
        int channel;
        bool is_drum;

        if (dm_midi_rhythm_part(message, &channel, &is_drum)) {
            check(is_drum == (channel == MIDI_DRUM_CHANNEL) && message->size == 12,
                  "imported GS part address follows its moved channel");
            unsigned sum = 0;

            for (int j = 6; j <= 10; ++j) {
                sum += message->data[j];
            }

            check(!(sum & MIDI_DATA_MASK), "moved GS rhythm-part checksum stays valid");
            ++rhythm_parts;
        }
    }

    check(rhythm_parts == 2, "both imported rhythm-part assignments move");
    DmBytes swapped_midi = {0};
    DmProject roundtrip;
    dm_project_init(&roundtrip);
    check(dm_midi_export(&imported, &swapped_midi, false, DM_METADATA_STRIP_ALL, error,
                         sizeof(error)) &&
              dm_import(&roundtrip, swapped_midi.data, swapped_midi.size, 0, 0, error,
                        sizeof(error)) &&
              !roundtrip.channels[0].drums && roundtrip.channels[MIDI_DRUM_CHANNEL].drums,
          "swapping imported GS parts back preserves their types through another MIDI export");
    dm_project_free(&roundtrip);
    dm_bytes_free(&swapped_midi);
    dm_project_free(&imported);
    dm_bytes_free(&midi);
}

static void new_project_channels(Editor *e) {
    ed_new_project(e);
    check(e->project.channels[0].enabled && e->project.channels[0].program == 0,
          "new projects start with channel 1 on program 0");
    check(e->project.channels[MIDI_DRUM_CHANNEL].enabled, "new projects keep the drum channel");
    check(ed_available_channel(e) == 1, "+ New defaults to the first free channel");
}

static void parallel_rows(Editor *e) {
    ed_new_project(e);
    check(e->project.note_rows[0] == 1 && ed_add_note_row(e, 0) && e->project.note_rows[0] == 2,
          "adding a note row appends an empty row");
    check(dm_note_add(&e->project.patterns[0],
                      (DmNote){.beat = 0, .length = 1, .key = 64, .velocity = 100}),
          "parallel row note fixture");
    int placed = ed_place_pattern(e, 0, ed_row_code(0, 0, 1), 0);
    check(placed >= 0 && e->project.clips[placed].row == 1,
          "a pattern can be placed on the appended note row");
    e->project.lane_count[0] = 1;
    e->channel_expanded[0] = true;
    int rows[ED_PLAYLIST_ROWS];
    int count = ed_playlist_channels(e, rows);
    check(count == 5 && rows[0] == ed_row_code(0, 0, 0) && rows[1] == ed_row_code(0, 0, 1) &&
              rows[2] == ed_row_code(0, ED_ROW_CHANNEL_EVENTS, 0) &&
              rows[3] == ed_row_code(0, 1, 0),
          "channel events follow every note row and precede automation lanes");

    DmBytes midi = {0};
    char error[256] = {0};
    DmProject imported;
    dm_project_init(&imported);
    check(dm_midi_export(&e->project, &midi, false, DM_METADATA_STRIP_ALL, error, sizeof(error)),
          "parallel note row exports to MIDI");
    check(dm_import(&imported, midi.data, midi.size, 0, 0, error, sizeof(error)), error);
    DmSchedule schedule = {0};
    check(dm_schedule(&imported, &schedule), "exported MIDI schedules");
    bool heard = false;

    for (size_t i = 0; i < schedule.count; ++i) {
        DmScheduled note = schedule.items[i];
        heard |= note.status == MIDI_NOTE_ON && note.a == 64 && note.b == 100;
    }

    check(heard, "a placement on note row two exports its notes on its instrument channel");
    dm_schedule_free(&schedule);
    dm_project_free(&imported);
    dm_bytes_free(&midi);

    ed_focus_edit(e, FOCUS_PLAYLIST);
    ed_clipboard(e, CLIPBOARD_COPY);
    ed_paste(e);
    check(e->project.clip_count == 2 && e->project.clips[1].row == 1,
          "copy and paste preserve the placement's note row");
    ed_duplicate_clips(e);
    check(e->project.clip_count == 3 && e->project.clips[2].row == 1,
          "duplicating a placement preserves its note row");
    int undo = e->undo_count;
    check(ed_remove_note_row(e, 0) && e->project.note_rows[0] == 1 && !e->project.clip_count &&
              e->undo_count == undo + 1,
          "removing the bottom note row deletes its placements in one undo step");
    ed_undo(e, false);
    check(e->project.note_rows[0] == 2 && e->project.clip_count == 3 &&
              e->project.clips[0].row == 1,
          "undo restores the removed note row and its placements");
    ed_undo(e, true);
    check(e->project.note_rows[0] == 1 && !e->project.clip_count,
          "redo removes the note row and its placements again");
    ed_undo(e, false);

    DmClip *clip = &e->project.clips[0];
    ed_move_clip(e, clip, ed_row_code(MIDI_DRUM_CHANNEL, 0, 1), 2);
    check(clip->channel == MIDI_DRUM_CHANNEL && clip->row == 0,
          "moving to a channel with fewer note rows clamps the row");
    ed_move_clip(e, clip, ed_row_code(0, 0, 1), 2);
    check(clip->channel == 0 && clip->row == 1,
          "moving back selects the target channel's second note row");
    ed_move_clip(e, clip, ed_row_code(0, 0, 0), 2);
    check(clip->row == 0, "moving vertically between note rows changes the row");
    ed_move_clip(e, clip, ed_row_code(0, 0, 1), 2);

    while (e->project.note_rows[0] < DM_NOTE_ROWS) {
        check(ed_add_note_row(e, 0), "append rows up to the limit");
    }

    undo = e->undo_count;
    check(!ed_add_note_row(e, 0) && e->undo_count == undo,
          "adding beyond the row limit leaves history unchanged");
    check(!ed_add_note_row(e, 1) && !ed_remove_note_row(e, MIDI_DRUM_CHANNEL),
          "disabled channels and the final note row cannot be modified");

    ed_new_project(e);
    check(ed_add_note_row(e, 0) && ed_place_pattern(e, 0, ed_row_code(0, 0, 0), 0) >= 0 &&
              ed_place_pattern(e, 0, ed_row_code(0, 0, 1), 0) >= 0,
          "parallel placement identity fixture");
    e->project.clips[0].selected = 1;
    e->project.clips[1].selected = 0;
    check(ed_add_note_row(e, 0), "add a row without changing the placement selection");
    ed_undo(e, false);
    check(e->project.clips[0].selected && !e->project.clips[1].selected,
          "undo distinguishes identical placements on different note rows");
    e->project.clips[0].selected = 0;
    e->project.clips[1].selected = 1;
    ed_focus_edit(e, FOCUS_PLAYLIST);
    ed_clipboard(e, CLIPBOARD_COPY);
    int automation = dm_pattern_add(&e->project, "Automation");
    check(automation >= 0, "parallel automation fixture");
    e->project.patterns[automation].kind = 1;
    e->project.lane_count[0] = 1;
    check(ed_place_pattern(e, automation, ed_row_code(0, 1, 0), 0) >= 0,
          "automation stays below the note rows");
    check(ed_remove_note_row(e, 0), "remove the clipboard placement's original note row");
    check(e->project.clip_count == 2 && e->project.clips[0].lane == 0 &&
              e->project.clips[1].lane == 1 && e->project.lane_count[0] == 1,
          "removing a note row preserves the other note row and automation placements");
    ed_paste(e);
    check(e->project.clip_count == 3 && e->project.clips[2].row == 0,
          "pasting clamps the copied note row to the current channel's row count");

    ed_new_project(e);
    check(ed_add_note_row(e, 0) && ed_add_note_row(e, 0) &&
              ed_place_pattern(e, 0, ed_row_code(0, 0, 0), 0) >= 0 &&
              ed_place_pattern(e, 0, ed_row_code(0, 0, 1), 2) >= 0,
          "group note-row drag fixture");
    e->project.clips[0].selected = e->project.clips[1].selected = 1;
    count = ed_playlist_channels(e, rows);
    UiMouse mouse = {.pos = {300, 124}, .clicked = true};
    ed_playlist_hit_click(e, mouse, 0, 0);
    mouse = (UiMouse){.pos = {300, 188}, .left = true};
    ed_playlist_drag(e, (UiRect){0, 0, 600, 400}, &e->playlist_view, 64, 40, rows, count, mouse);
    check(e->project.clips[0].row == 1 && e->project.clips[1].row == 2,
          "a group drag preserves the spacing between parallel note rows");
    mouse.left = false;
    mouse.released = true;
    ed_playlist_drag(e, (UiRect){0, 0, 600, 400}, &e->playlist_view, 64, 40, rows, count, mouse);
}

static void channel_events(Editor *e) {
    ed_new_project(e);
    int row = ed_row_code(0, ED_ROW_CHANNEL_EVENTS, 0);
    int automation = dm_pattern_add(&e->project, "Volume");
    check(automation >= 0, "channel event automation fixture");
    e->project.patterns[automation].kind = 1;
    e->project.patterns[automation].length = 8;
    e->project.lane_count[0] = 1;
    check(ed_place_pattern(e, 0, row, 0) < 0 && ed_place_pattern(e, automation, row, 0) < 0 &&
              !e->project.clip_count,
          "neither note nor automation patterns can be placed on channel events");
    check(ed_place_pattern(e, 0, ed_row_code(0, 0, 0), 0) >= 0, "channel event drag fixture");
    DmClip before = e->project.clips[0];
    ed_move_clip(e, &e->project.clips[0], row, 4);
    check(e->project.clips[0].beat == before.beat && e->project.clips[0].lane == before.lane,
          "moving a pattern to channel events leaves its placement unchanged");
    e->project.clips[0].length = 24;
    check(ed_place_pattern(e, automation, ed_row_code(0, 1, 0), 8) >= 0,
          "volume automation occupies bars three and four");
    e->project.clips[1].length = 8;
    e->project.channels[0].volume = 100;
    check(dm_automation_add(&e->project, (DmAutomation){.beat = 4,
                                                        .type = MIDI_CONTROL,
                                                        .channel = 0,
                                                        .a = MIDI_CC_VOLUME,
                                                        .b = 50,
                                                        .pattern = -1}) &&
              dm_automation_add(&e->project, (DmAutomation){.type = MIDI_CONTROL,
                                                            .channel = 0,
                                                            .a = MIDI_CC_VOLUME,
                                                            .b = 75,
                                                            .pattern = automation}),
          "channel volume 50 precedes pattern volume 75");
    DmExportEvent *events = NULL;
    size_t count = 0;
    uint64_t duration = 0;
    char error[256] = {0};
    check(dm_export_timeline(&e->project, &events, &count, &duration, error, sizeof(error)), error);
    int values[3] = {-1, -1, -1};
    const uint64_t ticks[] = {4 * DM_PPQ, 8 * DM_PPQ, 16 * DM_PPQ};

    for (size_t i = 0; i < count; ++i) {
        if (events[i].status == MIDI_CONTROL && events[i].a == MIDI_CC_VOLUME &&
            events[i].port == 0) {
            for (int j = 0; j < 3; ++j) {
                if (events[i].tick <= ticks[j]) {
                    values[j] = events[i].b;
                }
            }
        }
    }

    check(values[0] == 50 && values[1] == 75 && values[2] == 50,
          "channel volume persists, yields to an active pattern, then resumes at its end");
    free(events);
    int rows[ED_PLAYLIST_ROWS];
    int row_count = ed_playlist_channels(e, rows);
    UiRect area = {0, 0, 1200, 400}, grid = {200, 28, 1000, 372};
    e->playlist_view.scroll_x = e->playlist_view.scroll_y = 0;
    ed_playlist_click(e, area, grid, &e->playlist_view, 64, 40, rows, row_count,
                      (UiMouse){.pos = {360, 176}, .clicked = true}, -1);
    check(e->song_event && e->song_event_channel == 0 && !e->song_event_add &&
              e->event_beat == 4 && e->midi_draft.data[1] == MIDI_CC_VOLUME &&
              e->midi_draft.data[2] == 50,
          "clicking a populated channel bar opens that channel in Edit mode");
    ed_playlist_click(e, area, grid, &e->playlist_view, 64, 40, rows, row_count,
                      (UiMouse){.pos = {1000, 176}, .clicked = true}, -1);
    check(e->song_event && e->song_event_channel == 0 && e->song_event_add && e->event_beat == 20,
          "clicking an empty channel bar opens Add mode at its beat");
    e->song_event = false;
    e->song_event_channel = -1;
    e->selected_channel = MIDI_DRUM_CHANNEL;
    ed_playlist_label_action(e, 0, ED_ROW_CHANNEL_EVENTS, (UiMouse){.clicked = true});
    check(e->selected_channel == 0, "channel event label selects its channel");
    ed_playlist_label_action(e, 0, ED_ROW_CHANNEL_EVENTS, (UiMouse){.right_clicked = true});
    check(!e->lane_settings && e->instrument_channel < 0,
          "channel event label cannot open lane settings or instrument editing");
}

int main(void) {
    Editor *e = test_editor_open("channel rows",
                                 (TestDesktop){.soundfont = TEST_SOUNDFONT, .paused = true});
    int list[16];
    check(ed_channels(e, list) == 2 && list[0] == 0 && list[1] == MIDI_DRUM_CHANNEL,
          "new projects show only channels 1 and 10");
    channel_swap(e);
    channel_swap_drums(e);
    channel_events(e);
    parallel_rows(e);
    new_project_channels(e);
    test_channel_row_ui(e, check);
    removal(e);
    ed_new_project(e);
    check(ed_place_pattern(e, 0, 1, 0) < 0 && !e->project.channels[1].enabled,
          "placing a pattern cannot activate an unused channel");
    ed_open_instrument(e, -1, false);
    ed_apply_instrument(e);
    check(ed_place_pattern(e, 0, 1, 0) >= 0, "place a pattern on a newly created channel");
    ed_focus_edit(e, FOCUS_PLAYLIST);
    ed_clipboard(e, CLIPBOARD_COPY);
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
    test_editor_close(e);
}
