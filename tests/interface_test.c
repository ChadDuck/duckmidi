#include "editor_internal.h"
extern void test_interface_ui(Editor *, void (*)(bool, const char *));
static int checks;

static void check(bool ok, const char *text) {
    ++checks;

    if (!ok) {
        fprintf(stderr, "FAIL interface: %s\n", text);
        exit(1);
    }
}

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

static void event_editing(Editor *e) {
    editor_smoke(e);
    const uint8_t initial_volume[] = {0xb2, 7, 120};
    source_event(e, initial_volume, sizeof(initial_volume), 0);
    check(e->event_channel == 2,
          "initial volume inspector keeps channel separate from controller value");
    const uint8_t pedal[] = {0xb2, 66, 0};
    source_event(e, pedal, sizeof(pedal), 2);
    check(e->event_channel == 2, "controller decoder reads its channel from the MIDI status byte");
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

    for (int c = 0; c < 16; ++c) {
        if (c != 9) {
            check(ed_track_color(c) != ed_track_color(9), "percussion color is reserved");
        }
    }
}

static void cursor_preferences(Editor *e) {
    char saved[DM_PATH];
    snprintf(saved, sizeof(saved), "%s", e->settings_path);
    snprintf(e->settings_path, sizeof(e->settings_path),
             "agents/reports/interface-preferences.ini");
    e->os_cursor = true;
    check(dm_preferences_save(e, ed_shortcut_schema(), ed_action_names),
          "save isolated OS cursor preference");
    e->os_cursor = false;
    check(dm_preferences_load(e, ed_shortcut_schema()) && e->os_cursor,
          "OS cursor preference roundtrips");
    snprintf(e->settings_path, sizeof(e->settings_path), "%s", saved);
    e->os_cursor = false;
}

int main(void) {
    check(SDL_Init(SDL_INIT_VIDEO), "SDL");
    SDL_Window *w = SDL_CreateWindow("Interface", 1400, 900, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, "direct3d11");
    check(w && r && ui_init(w, r, NULL), "hidden renderer and cached cursors");
    Editor *e = calloc(1, sizeof(*e));
    editor_init(e, w, r, NULL, "", true);
    ui_frame();
    ui_root(0, 900);
    event_editing(e);
    cursor_preferences(e);
    ui_end();
    ui_render(r);
    editor_smoke(e);
    test_interface_ui(e, check);
    printf("PASS interface: %d checks\n", checks);
    editor_free(e);
    free(e);
    ui_shutdown();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
