#include "editor/internal.h"
#define TEST_NAME "inspector"
#include "test_common.h"
extern void test_channel_events_ui(Editor *, void (*)(bool, const char *));
extern void test_midi_browser_ui(Editor *, void (*)(bool, const char *));

static void source_event(Editor *e, const uint8_t *bytes, size_t size, double beat) {
    DmBytes b = {(uint8_t *)bytes, size, size};
    check(dm_event_add(&e->project, beat, 0, 0, &b), "create isolated event");

    for (size_t i = 0; i < e->project.event_count; ++i) {
        if (e->project.events[i].beat == beat && e->project.events[i].message.size == size &&
            !memcmp(e->project.events[i].message.data, bytes, size)) {
            ed_midi_select(e, (MidiRow){beat, (int)i, false});
            return;
        }
    }

    check(false, "find created event");
}

/* The MIDI event window edits friendly fields and raw bytes, and rejects invalid messages. */
static void event_editing(Editor *e) {
    editor_smoke(e);
    const uint8_t initial_volume[] = {0xb2, 7, 120};
    source_event(e, initial_volume, sizeof(initial_volume), 0);
    check((e->midi_draft.data[0] & MIDI_CHANNEL_MASK) == 2,
          "initial volume inspector keeps channel separate from controller value");
    const uint8_t expression[] = {0xb2, 11, 90};
    source_event(e, expression, sizeof(expression), 1);
    check(e->midi_draft.size == 3 && e->midi_draft.data[2] == 90,
          "MIDI event window drafts the controller value independently of the channel");
    e->midi_draft.data[2] = 70;
    check(ed_midi_apply_draft(e), "apply edited controller value");
    bool applied = false;

    for (size_t i = 0; i < e->project.event_count; ++i) {
        const DmEvent *event = &e->project.events[i];
        applied |= event->beat == 1 && event->message.size == 3 && event->message.data[0] == 0xb2 &&
                   event->message.data[1] == 11 && event->message.data[2] == 70;
    }

    check(applied, "MIDI event window applies the edited controller value");
    const uint8_t pedal[] = {0xb2, 66, 0};
    source_event(e, pedal, sizeof(pedal), 2);
    check((e->midi_draft.data[0] & MIDI_CHANNEL_MASK) == 2,
          "controller decoder reads its channel from the MIDI status byte");
    check(strstr(ed_midi_row_name(e, (MidiRow){2, e->midi_event_index, false}), "Sostenuto"),
          "controller row names explain their musical purpose");
    e->midi_values = true;
    e->midi_hex = true;
    snprintf((char *)e->midi_text.data, e->midi_text.capacity, "B2 42 7F");
    check(ed_midi_apply_draft(e), "apply hexadecimal pedal message");
    check(e->project.events[e->project.event_count - 1].message.data[2] == 127,
          "hexadecimal edit reaches the selected source event");
    source_event(e, pedal, sizeof(pedal), 3);
    int undo_count = e->undo_count;
    snprintf((char *)e->midi_text.data, e->midi_text.capacity, "B2 42 80");
    check(!ed_midi_apply_draft(e) && e->undo_count == undo_count && *e->midi_error,
          "out-of-range MIDI data is rejected without history or project mutation");
    snprintf((char *)e->midi_text.data, e->midi_text.capacity, "B2 42");
    check(!ed_midi_parse_bytes(e), "truncated MIDI message rejected");
    snprintf((char *)e->midi_text.data, e->midi_text.capacity, "B2 42 -1");
    check(!ed_midi_parse_bytes(e), "negative raw bytes rejected");
    e->midi_hex = false;
    snprintf((char *)e->midi_text.data, e->midi_text.capacity, "194 81");
    check(ed_midi_apply_draft(e), "decimal byte editing can change message type and instrument");
    const uint8_t program[] = {0xc2, 81};
    source_event(e, program, sizeof(program), 4);
    check(
        strstr(ed_midi_row_name(e, (MidiRow){4, e->midi_event_index, false}), dm_program_name(81)),
        "program row shows instrument name");
    e->midi_values = false;
    e->midi_draft.data[1] = 40;
    check(ed_midi_apply_draft(e), "friendly draft applies instrument selection");
    uint8_t marker[] = {0xff, 6, 1, 'A'};
    source_event(e, marker, sizeof(marker), 5);
    uint8_t text[200];
    memset(text, 'x', sizeof(text));
    check(ed_midi_replace_payload(e, text, sizeof(text)) && dm_message_valid(&e->midi_draft) &&
              ed_midi_payload_offset(&e->midi_draft) == 4,
          "text editing writes multi-byte MIDI payload lengths correctly");
    check(ed_midi_apply_draft(e), "apply edited metadata payload");
    source_event(e, marker, sizeof(marker), 6);
    e->midi_values = true;
    e->midi_hex = true;
    snprintf((char *)e->midi_text.data, e->midi_text.capacity, "FF 06 05 41");
    check(!ed_midi_apply_draft(e), "invalid metadata length is rejected");
    e->midi_values = false;
    int pattern_id = dm_pattern_add(&e->project, "Local automation");
    e->project.patterns[pattern_id].kind = 1;
    dm_automation_add(
        &e->project,
        (DmAutomation){.type = 0xb0, .channel = 2, .a = 7, .b = 30, .pattern = pattern_id});
    int index = -1;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        if (e->project.automation[i].pattern == pattern_id) {
            index = (int)i;
        }
    }

    size_t rows = ed_midi_rows(e);
    check(rows == e->project.event_count + e->project.automation_count,
          "MIDI browser includes controller and pressure automation events");
    ed_midi_select(e, (MidiRow){0, index, true});
    int volume = e->project.channels[2].volume;
    e->midi_draft.data[2] = 80;
    check(ed_midi_apply_draft(e) && e->project.channels[2].volume == volume,
          "editing pattern-local controller does not overwrite global mixer default");
}

static void imported_channel_events(Editor *e) {
    ed_new_project(e);
    /* One generated track: channel 3 changes instrument and volume at beat one. */
    static const uint8_t midi[] = {
        'M',
        'T',
        'h',
        'd',
        0,
        0,
        0,
        6,
        0,
        0,
        0,
        1,
        1,
        0xe0,
        'M',
        'T',
        'r',
        'k',
        0,
        0,
        0,
        33,
        0,
        MIDI_PROGRAM | 2,
        1,
        0,
        MIDI_NOTE_ON | 2,
        60,
        100,
        0x83,
        0x60,
        MIDI_NOTE_OFF | 2,
        60,
        0,
        0,
        MIDI_PROGRAM | 2,
        41,
        0,
        MIDI_CONTROL | 2,
        MIDI_CC_VOLUME,
        80,
        0x83,
        0x60,
        MIDI_NOTE_ON | 2,
        64,
        100,
        0x83,
        0x60,
        MIDI_NOTE_OFF | 2,
        64,
        0,
        0,
        MIDI_META,
        MIDI_META_END,
        0,
    };
    char error[256] = {0};
    check(dm_import(&e->project, midi, sizeof(midi), 0, 0, error, sizeof(error)), error);
    check(ed_song_event_rows(e, 1, 4, 2) == 1,
          "imported mid-song program belongs to its channel event row");
    MidiRow row = ((MidiRow *)e->midi_rows.data)[0];
    ed_midi_select(e, row);
    check(e->midi_draft.size == 2 && e->midi_draft.data[0] == (MIDI_PROGRAM | 2) &&
              e->midi_draft.data[1] == 41,
          "channel row retains the imported program and channel");
    check(ed_song_event_rows(e, 1, 4, -1) == 0 && ed_song_event_rows(e, 1, 4, 0) == 0,
          "song events and other channels exclude the imported program");
    bool volume = false;

    for (size_t i = 0; i < e->project.automation_count; ++i) {
        DmAutomation a = e->project.automation[i];
        volume |= a.pattern >= 0 && a.type == MIDI_CONTROL && a.a == MIDI_CC_VOLUME && a.b == 80;
    }

    check(volume, "imported volume stays in its pattern");
    const uint8_t pan[] = {MIDI_CONTROL | 2, MIDI_CC_PAN, 20};
    DmBytes message = {(uint8_t *)pan, sizeof(pan), sizeof(pan)};
    check(dm_event_add(&e->project, 0, 0, 0, &message), "raw initial pan fixture");
    size_t before = ed_song_event_rows(e, 0, 4, 2);
    check(before == 2 && dm_event_add(&e->project, 2, 0, 0, &message) &&
              ed_song_event_rows(e, 0, 4, 2) == before + 1,
          "channel rows include raw mid-song pan but omit mixer-folded beat-zero controls");
}

int main(void) {
    Editor *e = test_editor_open("Inspector", (TestDesktop){0});
    imported_channel_events(e);
    ed_new_project(e);
    test_channel_events_ui(e, check);
    ui_frame();
    ui_root(0, 900);
    event_editing(e);
    ui_end();
    ui_render(e->renderer);
    editor_smoke(e);
    test_midi_browser_ui(e, check);
    editor_smoke(e);
    uint8_t bytes[] = {0xb2, 7, 120};
    DmBytes message = {bytes, 3, 3};

    for (int i = 0; i < 50000; ++i) {
        check(dm_event_add(&e->project, i * .25, 0, 0, &message), "source fixture");
        check(dm_automation_add(
                  &e->project,
                  (DmAutomation){.beat = i * .25, .pattern = 0, .type = 0xb0, .a = 1, .b = 64}),
              "automation fixture");
    }

    uint64_t begin = SDL_GetPerformanceCounter();

    for (int i = 0; i < 25; ++i) {
        check(ed_midi_rows(e) == 100000, "all events retained");
    }

    double elapsed =
        1000.0 * (SDL_GetPerformanceCounter() - begin) / SDL_GetPerformanceFrequency() / 25;
    MidiRow *rows = (MidiRow *)e->midi_rows.data;

    for (int i = 0; i < 50000; ++i) {
        check(!rows[i * 2].automation && rows[i * 2].index == i && rows[i * 2].beat == i * .25,
              "stable source order at equal beats");
        check(rows[i * 2 + 1].automation && rows[i * 2 + 1].index == i &&
                  rows[i * 2 + 1].beat == i * .25,
              "stable automation order at equal beats");
    }

    printf("PASS inspector: 100000 sorted rows; average rebuild %.3f ms (25 runs)\n", elapsed);
    test_editor_close(e);
}
