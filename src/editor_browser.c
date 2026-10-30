#include "editor_internal.h"
#include "text/en.h"

static bool pattern_context(Editor *e, size_t i);
static void pattern_browser_header(Editor *e);


static void mixer_peak_update(Editor *e, uint64_t now);

static bool pattern_context(Editor *e, size_t i) {
    if (!ui_context(DM_TEXT_PATTERN_ACTIONS)) {
        return false;
    }
    if (ui_menu_item(DM_TEXT_RENAME, NULL, true)) {
        e->rename_pattern = (int)i;
        snprintf(e->rename_text, sizeof(e->rename_text), "%s", e->project.patterns[i].name);
    }
    if (ui_menu_item(DM_TEXT_DUPLICATE, NULL, true)) {
        ed_checkpoint(e);
        int id = dm_pattern_copy(&e->project, (int)i);
        if (id >= 0) {
            ed_open_pattern(e, id, true);
            ed_commit(e);
        }
    }
    bool removed = false;
    if (ui_menu_item(DM_TEXT_DELETE_PATTERN, NULL, true)) {
        ed_checkpoint(e);
        dm_pattern_remove(&e->project, (int)i);
        e->selected_pattern = e->project.pattern_count ? 0 : -1;
        ed_bind_pattern_channel(e);
        ed_commit(e);
        removed = true;
    }
    ui_popup_end();
    return removed;
}

void ed_choose_pattern_card(Editor *e, int id, bool selected) {
    bool keep = id == e->selected_pattern && (e->preview_override || e->preview_source_channel < 0);
    int program = e->preview_program, source = e->preview_source_channel;
    bool override = e->preview_override;
    ed_release_piano(e);
    if (selected && ui_mouse().ctrl) {
        ed_clear_pattern_selection(e);
    } else {
        for (size_t i = 0; i < e->project.clip_count; ++i) {
            e->project.clips[i].selected = 0;
        }
        ed_open_pattern(e, id, false);
        ed_bind_pattern_channel(e);
        if (keep) {
            e->preview_program = program;
            e->preview_source_channel = source;
            e->preview_override = override;
        }
        e->context_note = -1;
        e->automation_center =
            e->project.patterns[id].count ? e->project.patterns[id].notes[0].key : 60;
    }
}

static void pattern_browser_header(Editor *e) {
    ui_align_frame();
    ui_text(DM_TEXT_PATTERNS);
    ui_same();
    if (ui_button(DM_TEXT_NEW, 70, 0)) {
        ed_checkpoint(e);
        e->selected_pattern = dm_pattern_add(&e->project, NULL);
        if (e->selected_pattern >= 0 && e->automation_browser) {
            e->project.patterns[e->selected_pattern].kind = 1;
            e->project.patterns[e->selected_pattern].length = dm_bar(&e->project);
            char name[64];
            snprintf(name, sizeof(name), DM_TEXT_AUTOMATION_D, e->selected_pattern + 1);
            dm_pattern_name(&e->project, e->selected_pattern, name);
        }
        e->pattern_channel = e->selected_channel;
        e->transient_pattern = e->selected_pattern;
        ed_open_pattern(e, e->selected_pattern, true);
        ed_commit(e);
    }
    ui_same();
    float available = ui_available().x;
    float selector = fminf(128, available);
    UiPoint position = ui_cursor();
    ui_position(position.x + available - selector, position.y);
    ui_width(selector);
    if (ui_combo("##Pattern category",
                 e->automation_browser ? DM_TEXT_AUTOMATION : DM_TEXT_PATTERNS_160A13)) {
        if (ui_select(DM_TEXT_PATTERNS_160A13, !e->automation_browser, 0, 0)) {
            e->automation_browser = false;
        }
        if (ui_select(DM_TEXT_AUTOMATION, e->automation_browser, 0, 0)) {
            e->automation_browser = true;
        }
        ui_combo_end();
    }
    ui_separator();
}

uint32_t ed_pattern_color(Editor *e, int id) {
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].pattern == id) {
            return ed_track_color(e->project.clips[i].channel);
        }
    }
    return ed_track_color(id == e->selected_pattern ? e->pattern_channel : e->selected_channel);
}

void ed_pattern_panel(Editor *e, float width, float height) {
    ui_child(DM_TEXT_PATTERNS_160A13, width, height, true);
    if (ui_panel_clicked()) {
        ed_focus_edit(e, FOCUS_PATTERNS);
    }
    pattern_browser_header(e);
    int visible = 0;
    float card = (ui_available().x - 8) / 2;
    for (size_t i = 0; i < e->project.pattern_count; ++i) {
        if ((e->project.patterns[i].kind != 0) != e->automation_browser) {
            continue;
        }
        if (visible++ % 2) {
            ui_same();
        }
        ui_id((int)i);
        UiPoint at = ui_cursor();
        bool selected = e->pattern_highlight && e->selected_pattern == (int)i;
        ui_accent(selected);
        if (ui_button("##pattern", card, 92)) {
            ed_choose_pattern_card(e, (int)i, selected);
        }
        ui_accent_end();
        if (ui_drag_pattern((int)i, e->project.patterns[i].name)) {
            e->pattern_drag = (int)i;
        }
        UiRect r = {at.x, at.y, card, 92};
        ui_clip(r);
        ui_label((UiPoint){at.x + 9, at.y + 10}, TEXT, e->project.patterns[i].name);
        DmPattern *p = &e->project.patterns[i];
        for (size_t n = 0; n < p->count; ++n) {
            DmNote note = p->notes[n];
            float x = at.x + 9 + (float)(note.beat / p->length) * (card - 18),
                  y = at.y + 68 - (note.key - 48) * 1.1f;
            if (y < at.y + 32 || y > at.y + 81) {
                continue;
            }
            ui_rect((UiRect){x, y, fmaxf(2, (float)(note.length / p->length) * (card - 18)), 2},
                    ed_pattern_color(e, (int)i), 1);
        }
        if (p->kind) {
            ed_automation_preview(e, (int)i, (UiRect){at.x + 9, at.y + 33, card - 18, 48},
                                  ed_pattern_color(e, (int)i), p->length);
        }
        ui_unclip();
        if (ui_hover()) {
            ed_hover(e, DM_TEXT_SELECT_THIS_PATTERN_THEN_CLICK_A_PLAYLIST_ROW_TO_PLACE_IT);
        }
        if (pattern_context(e, i)) {
            ui_pop_id();
            break;
        }
        ui_pop_id();
    }
    if (ui_background_clicked()) {
        ed_clear_pattern_selection(e);
    }
    ui_child_end();
}

static void mixer_store(Editor *e, DmPortMix mix) {
    ed_checkpoint(e);
    if(!dm_port_mix_set(&e->project,mix)){ed_message(e,"Cannot allocate port mixer settings.");return;}
    ed_commit(e);
    dm_audio_part_control(e->audio,mix.port,mix.channel,7,mix.volume);
    dm_audio_part_control(e->audio,mix.port,mix.channel,10,mix.pan);
    int preview=e->context==2?e->pattern_channel:e->selected_channel;
    if(preview==mix.channel&&e->note_ports[preview]==mix.port)ed_keyboard_channel(e);
}
static void mixer_controls(Editor *e, int c, int port) {
    DmPortMix mix=dm_port_mix(&e->project,port,c), original=mix;
    DmPortMix reset={port,c,100,64,false,false};
    if(!port){DmChannel value=e->reset_channels[c];reset.volume=value.volume;reset.pan=value.pan;reset.mute=value.mute;reset.solo=value.solo;}
    int part=port*16+c; char text[32];
    UiPoint start=ui_cursor(), area=ui_available();
    float width=area.x, height=fmaxf(40,area.y-110);
    snprintf(text,sizeof(text),"%d",port);
    ui_center_text(text);
    snprintf(text,sizeof(text),"Port %d",port);ui_tip(text);
    ui_rect((UiRect){start.x+(width-16)*.5f,start.y+19,16,2},ed_port_color(port),1);
    UiPoint v={start.x+(width-34)*.5f,start.y+28};
    ui_position(v.x,v.y);
    ui_vslider("##volume",&mix.volume,height);
    if(ui_item_middle())mix.volume=reset.volume;
    ui_tip("Initial volume; middle-click to reset");
    float peak=e->mixer_peaks[part];
    ui_rect((UiRect){v.x+30,v.y,4,height},BG,2);
    ui_rect((UiRect){v.x+30,v.y+height*(1-fminf(peak,1)),4,height*fminf(peak,1)},peak>.95f?GOLD:ed_port_color(port),2);
    ui_position(start.x,v.y+height+4);
    ui_accent(mix.mute);if(ui_button(DM_TEXT_M,(width-2)*.5f,24))mix.mute=!mix.mute;ui_accent_end();
    if(ui_item_middle())mix.mute=reset.mute;
    ui_tip("Mute this port");ui_same();
    ui_accent(mix.solo);if(ui_button(DM_TEXT_S,(width-2)*.5f,24))mix.solo=!mix.solo;ui_accent_end();
    if(ui_item_middle())mix.solo=reset.solo;
    ui_tip("Solo this port");
    ui_position(start.x,v.y+height+32);
    ui_mixer_pan(&mix.pan,width);
    if(ui_item_middle())mix.pan=reset.pan;
    snprintf(text,sizeof(text),"Pan: %d; middle-click to reset",mix.pan);ui_tip(text);
    ui_position(start.x,v.y+height+58);
    ui_mixer_value(&mix.volume,width);
    if(mix.volume!=original.volume||mix.pan!=original.pan||mix.mute!=original.mute||mix.solo!=original.solo)mixer_store(e,mix);
}
bool ed_channel_has_clips(Editor *e, int channel) {
    for (size_t i = 0; i < e->project.clip_count; ++i) {
        if (e->project.clips[i].channel == channel) {
            return true;
        }
    }
    return false;
}

int ed_available_channel(Editor *e) {
    int spare = -1;
    for (int c = 0; c < 16; ++c) {
        DmChannel *channel = &e->project.channels[c];
        if (channel->enabled && channel->program < 0 && !ed_channel_has_clips(e, c)) {
            return c;
        }
        if (!channel->enabled && spare < 0) {
            spare = c;
        }
    }
    return spare;
}

static void mixer_peak_update(Editor *e, uint64_t now) {
    bool sample = now - e->mixer_peak_tick >= 25;
    if (sample) e->mixer_peak_tick = now;
    for (int c = 0; c < DM_PARTS; ++c) {
        if (sample) e->mixer_peak_targets[c] = dm_audio_peak(e->audio, c);
        e->mixer_peaks[c] = dm_visual_smooth(e->mixer_peaks[c], e->mixer_peak_targets[c],
                                              ui_delta_time(), .025f, .16f);
    }
}

void ed_mixer(Editor *e, float width, float height) {
    mixer_peak_update(e,SDL_GetTicks());
    ui_child(DM_TEXT_MIXER,width,height,false);
    if(ui_panel_clicked())ed_focus_edit(e,FOCUS_OTHER);
    ui_align_frame();ui_text(DM_TEXT_MIXER_6766FE);ui_separator();ui_compact_begin();
    ui_child(DM_TEXT_CHANNEL_STRIPS,0,0,true);ui_mixer_wheel();
    UiPoint start=ui_cursor();float strip_height=fmaxf(180,ui_available().y-16),x=start.x;
    for(int c=0;c<16;c++) {
        if(!e->project.channels[c].enabled)continue;
        bool used[DM_NOTE_PORTS];dm_channel_ports(&e->project,c,used);
        DmPattern *editing=ed_pattern(e);
        if(editing&&e->pattern_channel==c)for(size_t n=0;n<editing->count;n++)used[editing->notes[n].port]=true;
        unsigned count=0;for(unsigned port=0;port<DM_NOTE_PORTS;port++)count+=used[port];
        float group_width=count*64.0f+12;
        ui_id(c);ui_channel_color(ed_track_color(c));ui_position(x,start.y);
        ui_mixer_child("channel group",group_width,strip_height,true);
        ui_rect((UiRect){x+6,start.y,group_width-12,2},ed_track_color(c),1);
        char text[32];snprintf(text,sizeof(text),"%02d%s",c+1,c==9?DM_TEXT_DR:"");
        if(ui_mixer_header(text,group_width-12)){ed_release_piano(e);e->selected_channel=c;e->channel_selected=true;e->instrument_channel=c;}
        if(ui_item_middle()){ed_checkpoint(e);e->project.channels[c]=e->reset_channels[c];ed_commit(e);}
        ui_tip(c==9?DM_TEXT_GENERAL_MIDI_PERCUSSION:dm_program_name(e->project.channels[c].program));
        UiPoint row=ui_cursor();unsigned column=0;
        for(int port=0;port<DM_NOTE_PORTS;port++)if(used[port]) {
            ui_id(port);ui_position(row.x+column*64,row.y);ui_channel_color(ed_port_color(port));
            ui_mixer_child("port strip",64,strip_height-40,false);
            mixer_controls(e,c,port);
            ui_child_end();ui_channel_color_end();ui_pop_id();column++;
        }
        ui_child_end();ui_channel_color_end();ui_pop_id();x+=group_width+6;
    }
    ui_position(x,start.y+1);ui_space(1);ui_child_end();ui_compact_end();ui_child_end();
}
int ed_channels(Editor *e, int *list) {
    int n = 0, spare = ed_available_channel(e);
    for (int c = 0; c < 16; ++c) {
        if (e->project.channels[c].enabled || c == spare) {
            list[n++] = c;
        }
    }
    return n;
}
