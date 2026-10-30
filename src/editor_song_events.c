#include "editor_internal.h"
#include "text/en.h"

static void apply_initial_source(Editor *e);
static void song_event_fields(Editor *e);
static void song_event_details(Editor *e);
static void opaque_event_fields(Editor *e);

void ed_song_event_remove(Editor *e) {
    if (e->event_edit >= 0 && (size_t)e->event_edit < e->project.event_count) {
        size_t i = (size_t)e->event_edit;
        dm_bytes_free(&e->project.events[i].message);
        memmove(e->project.events + i, e->project.events + i + 1,
                (e->project.event_count - i - 1) * sizeof(DmEvent));
        --e->project.event_count;
    }
    if (e->event_auto_edit >= 0 && (size_t)e->event_auto_edit < e->project.automation_count) {
        size_t i = (size_t)e->event_auto_edit;
        memmove(e->project.automation + i, e->project.automation + i + 1,
                (e->project.automation_count - i - 1) * sizeof(DmAutomation));
        --e->project.automation_count;
    }
}

static void apply_initial_source(Editor *e) {
    if (e->event_beat != 0) {
        return;
    }
    if (e->event_auto_edit >= 0) {
        DmAutomation a = e->project.automation[e->event_auto_edit];
        if (a.beat == 0 && a.type == 0xc0) {
            e->project.channels[e->event_channel].program = e->event_program;
        }
        return;
    }
    if (e->event_edit < 0) {
        return;
    }
    DmEvent event = e->project.events[e->event_edit];
    if (event.beat != 0 || event.message.size < 2) {
        return;
    }
    const uint8_t *m = event.message.data;
    int channel = m[0] & 15;
    if ((m[0] & 0xf0) == 0xc0) {
        e->project.channels[channel].program = e->event_program;
    }
    if ((m[0] & 0xf0) == 0xb0 && m[1] == 7) {
        e->project.channels[channel].volume = e->event_channel;
    }
    if ((m[0] & 0xf0) == 0xb0 && m[1] == 10) {
        e->project.channels[channel].pan = e->event_channel;
    }
}

void ed_song_event_add(Editor *e) {
    ed_checkpoint(e);
    apply_initial_source(e);
    if (e->event_source_kind == 3 && e->event_edit >= 0) {
        DmEvent source = e->project.events[e->event_edit];
        DmBytes copy = {0};
        if (dm_bytes(&copy, source.message.data, source.message.size)) {
            if (copy.size >= 2 && copy.data[0] < 0xf0) {
                copy.data[1] = (uint8_t)e->event_program;
                if (copy.size == 3) {
                    copy.data[2] = (uint8_t)e->event_channel;
                }
            }
            ed_song_event_remove(e);
            dm_event_add(&e->project, e->event_beat, source.track, source.port, &copy);
            dm_bytes_free(&copy);
            ed_commit(e);
            e->song_event = false;
        }
        return;
    }
    ed_song_event_remove(e);
    uint8_t data[256];
    size_t size = 0;
    double beat = e->event_beat;
    if (e->event_kind == 0) {
        uint32_t us = (uint32_t)llround(60000000.0 / e->event_bpm);
        uint8_t tempo[] = {0xff, 0x51, 3, us >> 16, us >> 8, us};
        memcpy(data, tempo, 6);
        size = 6;
        if (beat == 0) {
            e->project.bpm = e->event_bpm;
        }
    } else if (e->event_kind == 1) {
        int power = 0;
        while ((1 << power) < e->event_denominator) {
            ++power;
        }
        uint8_t sig[] = {0xff, 0x58, 4, e->event_numerator, power, 24, 8};
        memcpy(data, sig, 7);
        size = 7;
        if (beat == 0) {
            e->project.numerator = e->event_numerator;
            e->project.denominator = e->event_denominator;
        }
    } else if (e->event_kind == 2) {
        size_t n = strlen(e->event_marker);
        data[0] = 0xff;
        data[1] = 6;
        data[2] = (uint8_t)n;
        memcpy(data + 3, e->event_marker, n);
        size = n + 3;
    } else {
        dm_automation_add(&e->project, (DmAutomation){beat, 0xc0, e->event_channel,
                                                      e->event_program, 0, -1, 0, 0, 0, false,0});
    }
    if (size) {
        DmBytes bytes = {data, size, size};
        dm_event_add(&e->project, beat, 0, 0, &bytes);
    }
    ed_commit(e);
    e->song_event = false;
}

static void song_event_fields(Editor *e) {
    if (e->event_kind == 0) {
        ui_float(DM_TEXT_BPM, &e->event_bpm, DM_BPM_MIN, DM_BPM_MAX);
    } else if (e->event_kind == 1) {
        ui_int(DM_TEXT_NUMERATOR, &e->event_numerator, 1, 255);
        char label[16];
        snprintf(label, sizeof(label), "%d", e->event_denominator);
        if (ui_combo(DM_TEXT_DENOMINATOR, label)) {
            for (int i = 0; i < 8; ++i) {
                char n[16];
                snprintf(n, sizeof(n), "%d", 1 << i);
                if (ui_select(n, e->event_denominator == (1 << i), 0, 0)) {
                    e->event_denominator = 1 << i;
                }
            }
            ui_combo_end();
        }
    } else if (e->event_kind == 2) {
        ui_input(DM_TEXT_MARKER, e->event_marker, sizeof(e->event_marker));
    } else {
        int channel = e->event_channel + 1;
        if (ui_int(DM_TEXT_TARGET_CHANNEL, &channel, 1, 16)) {
            e->event_channel = channel - 1;
        }
        if (ui_combo(DM_TEXT_PROGRAM, dm_program_name(e->event_program))) {
            for (int i = 0; i < 128; ++i) {
                if (ui_select(dm_program_name(i), e->event_program == i, 0, 0)) {
                    e->event_program = i;
                }
            }
            ui_combo_end();
        }
    }
}

static void song_event_details(Editor *e) {
    if (e->event_edit >= 0) {
        DmEvent *source = &e->project.events[e->event_edit];
        char info[128];
        snprintf(info, sizeof(info), DM_TEXT_SOURCE_TRACK_D_PORT_D_ZU_BYTES, source->track + 1,
                 source->port, source->message.size);
        ui_dim(info);
        size_t shown = source->message.size < 96 ? source->message.size : 96;
        for (size_t row = 0; row < shown; row += 16) {
            char hex[64] = {0};
            size_t used = 0;
            for (size_t j = row; j < shown && j < row + 16; ++j) {
                used += (size_t)snprintf(hex + used, sizeof(hex) - used, "%02X ",
                                         source->message.data[j]);
            }
            ui_dim(hex);
        }
        ui_dim(DM_TEXT_TEMPO_AND_METER_AFFECT_THIS_SEQUENCE_CHANNEL_MESSAGES_AFFECT_THEIR_MIDI_);
    }
    char current[160] = DM_TEXT_SELECT_A_SOURCE_EVENT;
    if (e->event_edit >= 0 && (size_t)e->event_edit < e->project.event_count) {
        DmEvent *event = &e->project.events[e->event_edit];
        snprintf(current, sizeof(current), DM_TEXT_D_S_BEAT_3F, e->event_edit + 1,
                 ed_event_label(&event->message), event->beat + 1);
    }
    if (ui_combo(DM_TEXT_SOURCE_EVENT, current)) {
        for (size_t i = 0; i < e->project.event_count; ++i) {
            char label[160];
            snprintf(label, sizeof(label), DM_TEXT_ZU_S_BEAT_3F, i + 1,
                     ed_event_label(&e->project.events[i].message), e->project.events[i].beat + 1);
            if (ui_select(label, e->event_edit == (int)i, 0, 0)) {
                ed_song_event_open(e, (int)i, false);
            }
        }
        ui_combo_end();
    }
}

static void opaque_event_fields(Editor *e) {
    DmBytes *ed_message = &e->project.events[e->event_edit].message;
    if (ed_message->size >= 2 && ed_message->data[0] < 0xf0) {
        ui_int(DM_TEXT_DATA_1, &e->event_program, 0, 127);
        if (ed_message->size == 3) {
            ui_int(DM_TEXT_DATA_2, &e->event_channel, 0, 127);
        }
    } else {
        ui_dim(DM_TEXT_OPAQUE_MIDI_PAYLOAD_RETAINED_YOU_CAN_CHANGE_ITS_TIME_OR_REMOVE_IT);
    }
}

void ed_song_event_position(Editor *e) {
    float beat = e->event_beat + 1;
    if (ui_float(DM_TEXT_BEAT, &beat, 1, 1000001)) {
        e->event_beat = beat - 1;
    }
    ui_same();
    if (ui_button(DM_TEXT_USE_PLAYHEAD, 0, 0)) {
        e->event_beat = (float)(!e->pattern_mode && dm_audio_playing(e->audio)
                                    ? dm_seconds_beat(&e->project, dm_audio_position(e->audio))
                                    : e->cursor_beat[0]);
    }
}

void ed_song_event_dialog(Editor *e) {
    if (!e->song_event) {
        return;
    }
    if (ui_dialog(DM_TEXT_SONG_EVENT, &e->song_event, 660, 520)) {
        const char *names[] = {DM_TEXT_TEMPO, DM_TEXT_TIME_SIGNATURE, DM_TEXT_MARKER,
                               DM_TEXT_PROGRAM_CHANGE};
        ui_text(e->event_source_kind ? DM_TEXT_EDIT_ARRANGEMENT_EVENT
                                     : DM_TEXT_ADD_ARRANGEMENT_EVENT);
        song_event_details(e);
        ed_song_event_position(e);
        if (e->event_source_kind != 3 && ui_combo(DM_TEXT_TYPE, names[e->event_kind])) {
            for (int i = 0; i < 4; ++i) {
                if (ui_select(names[i], i == e->event_kind, 0, 0)) {
                    e->event_kind = i;
                }
            }
            ui_combo_end();
        }
        if (e->event_source_kind != 3) {
            song_event_fields(e);
        } else {
            opaque_event_fields(e);
        }
        if (ui_button(e->event_source_kind ? DM_TEXT_APPLY_EVENT : DM_TEXT_ADD_EVENT, 120, 0)) {
            ed_song_event_add(e);
        }
        if (e->event_source_kind && ui_button(DM_TEXT_DELETE_EVENT, 120, 0)) {
            ed_checkpoint(e);
            ed_song_event_remove(e);
            ed_commit(e);
            e->song_event = false;
        }
    }
    ui_end();
}
