#include "editor_internal.h"
enum {VISIBLE_PORTS=8};
uint32_t ed_port_color(unsigned port) {
    static const uint32_t colors[VISIBLE_PORTS]={COLOR(83,190,244),COLOR(242,151,73),COLOR(183,130,243),COLOR(242,110,135),COLOR(76,211,184),COLOR(168,211,91),COLOR(233,140,216),COLOR(116,156,248)};
    return port?colors[(port-1)%VISIBLE_PORTS]:COLOR(192,201,211);
}
unsigned ed_note_port(Editor *e){DmPattern *p=ed_pattern(e);return p&&p->kind?(unsigned)p->port:e->note_ports[e->pattern_channel];}
static void channel_ports(Editor *e,bool *used) {
    memset(used,0,DM_NOTE_PORTS*sizeof(*used));
    for(size_t i=0;i<e->project.clip_count;i++)if(e->project.clips[i].channel==e->pattern_channel) {
        DmPattern *p=&e->project.patterns[e->project.clips[i].pattern];
        for(size_t j=0;j<p->count;j++)used[p->notes[j].port]=true;
    }
    DmPattern *p=ed_pattern(e);if(p)for(size_t j=0;j<p->count;j++)used[p->notes[j].port]=true;
}
float ed_port_buttons_width(Editor *e) {
    bool used[DM_NOTE_PORTS];channel_ports(e,used);unsigned count=0;
    for(unsigned p=1;p<DM_NOTE_PORTS;p++)if(used[p])count++;
    if(!count){e->note_ports[e->pattern_channel]=0;return 0;}
    count=1;for(unsigned p=1;p<=VISIBLE_PORTS;p++)if(used[p])count++;
    return count*(22+ui_item_spacing_x());
}
void ed_port_buttons(Editor *e) {
    bool used[DM_NOTE_PORTS];channel_ports(e,used);bool colored=false;
    for(unsigned p=1;p<DM_NOTE_PORTS;p++)colored|=used[p];
    if(!colored){e->note_ports[e->pattern_channel]=0;return;}
    used[0]=true;
    for(unsigned port=0;port<=VISIBLE_PORTS;port++)if(used[port]) {
        ui_id((int)port);
        if(ui_port_color("##event-port",ed_port_color(port),ed_note_port(e)==port,22)) {
            ed_release_piano(e);
            e->note_ports[e->pattern_channel]=(uint8_t)port;DmPattern *p=ed_pattern(e);
            if(p&&p->kind&&p->port!=(int)port){ed_checkpoint(e);p->port=(int)port;ed_commit(e);}
            e->automation_node=-1;e->event_selection.size=0;
        }
        char hint[80];snprintf(hint,sizeof(hint),"Edit events for note port %u",port);ui_tip(hint);ui_pop_id();ui_same();
    }
}
bool ed_color_notes(Editor *e,unsigned port) {
    DmPattern *p=ed_pattern(e);if(!p||p->kind||port>=DM_NOTE_PORTS)return false;
    bool changed=false;for(size_t i=0;i<p->count;i++)changed|=p->notes[i].selected&&p->notes[i].port!=port;
    if(!changed)return false;
    ed_checkpoint(e);for(size_t i=0;i<p->count;i++)if(p->notes[i].selected)p->notes[i].port=(uint8_t)port;
    e->note_ports[e->pattern_channel]=(uint8_t)port;ed_commit(e);return true;
}
void ed_note_color_dialog(Editor *e) {
    bool request=e->note_color_popup;e->note_color_popup=false;
    if(ui_mouse_popup("Note colors",request)) {
        UiPoint top=ui_cursor();float width=(VISIBLE_PORTS+1)*25+VISIBLE_PORTS*ui_item_spacing_x();
        ui_text("Assign note port");ui_same();ui_position(top.x+width-24,top.y);ui_info("Independent MIDI ports may be unsupported by other applications and cannot be exported to M64.");
        for(unsigned port=0;port<=VISIBLE_PORTS;port++) {
            ui_id((int)port);if(port)ui_same();
            if(ui_port_color("##note-port",ed_port_color(port),ed_note_port(e)==port,25)){ed_color_notes(e,port);ui_close_popup();}
            char hint[80];snprintf(hint,sizeof(hint),port?"Note port %u":"Default: remove explicit color",port);ui_tip(hint);ui_pop_id();
        }
        ui_popup_end();
    }
}
