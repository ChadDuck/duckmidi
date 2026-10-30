#include "editor_internal.h"
/* Stable project key indices, independent of SDL's binary representation. */
static const SDL_Scancode piano_keys[DM_PIANO_KEYS]={
    SDL_SCANCODE_Z,SDL_SCANCODE_S,SDL_SCANCODE_X,SDL_SCANCODE_D,SDL_SCANCODE_C,
    SDL_SCANCODE_V,SDL_SCANCODE_G,SDL_SCANCODE_B,SDL_SCANCODE_H,SDL_SCANCODE_N,
    SDL_SCANCODE_J,SDL_SCANCODE_M,SDL_SCANCODE_COMMA,SDL_SCANCODE_L,SDL_SCANCODE_PERIOD,
    SDL_SCANCODE_SEMICOLON,SDL_SCANCODE_SLASH,
    SDL_SCANCODE_Q,SDL_SCANCODE_2,SDL_SCANCODE_W,SDL_SCANCODE_3,SDL_SCANCODE_E,
    SDL_SCANCODE_R,SDL_SCANCODE_5,SDL_SCANCODE_T,SDL_SCANCODE_6,SDL_SCANCODE_Y,
    SDL_SCANCODE_7,SDL_SCANCODE_U,SDL_SCANCODE_I,SDL_SCANCODE_9,SDL_SCANCODE_O,
    SDL_SCANCODE_0,SDL_SCANCODE_P,SDL_SCANCODE_LEFTBRACKET,SDL_SCANCODE_EQUALS,SDL_SCANCODE_RIGHTBRACKET
};
int ed_piano_index(SDL_Scancode key){for(int i=0;i<DM_PIANO_KEYS;i++)if(piano_keys[i]==key)return i;return -1;}
SDL_Scancode ed_piano_scancode(int index){return index>=0&&index<DM_PIANO_KEYS?piano_keys[index]:SDL_SCANCODE_UNKNOWN;}
int ed_piano_key(SDL_Scancode key){int i=ed_piano_index(key);return i<0?-1:i<17?48+i:60+i-17;}
int ed_keyboard_note(Editor *e,SDL_Scancode key,int channel) {
    int index=ed_piano_index(key);if(index<0)return -1;
    if(channel==9) {
        const uint8_t *mapping=e->project.percussion_override?e->project.percussion_keys:e->percussion_global;
        for(int note=0;note<128;note++)if(mapping[note]==index+1)return note;
    }
    return ed_piano_key(key)+e->piano_octave*12;
}
void ed_percussion_open(Editor *e) {
    ed_release_piano(e);e->percussion_window=true;e->percussion_listening=false;e->percussion_reveal=true;
    memcpy(e->percussion_draft,e->project.percussion_override?e->project.percussion_keys:e->percussion_global,128);
    e->percussion_feedback[0]=0;
}
bool ed_percussion_key(Editor *e,const SDL_KeyboardEvent *key) {
    if(!e->percussion_window||!e->percussion_listening)return false;
    if(key->scancode==SDL_SCANCODE_ESCAPE){e->percussion_listening=false;e->percussion_feedback[0]=0;return true;}
    if(key->repeat)return true;
    int index=ed_piano_index(key->scancode);
    if(index<0||ed_normalized(key->mod)){snprintf(e->percussion_feedback,sizeof(e->percussion_feedback),"Press a piano key without modifiers; Escape stops listening.");return true;}
    for(unsigned i=0;i<128;i++)if(e->percussion_draft[i]==index+1)e->percussion_draft[i]=0;
    e->percussion_draft[e->percussion_note]=(uint8_t)(index+1);e->percussion_listening=false;
    snprintf(e->percussion_feedback,sizeof(e->percussion_feedback),"Assigned %s. Choose where to save this mapping.",SDL_GetScancodeName(key->scancode));return true;
}
void ed_percussion_save(Editor *e,bool project) {
    ed_release_piano(e);e->percussion_listening=false;
    if(project) {
        ed_checkpoint(e);e->project.percussion_override=true;memcpy(e->project.percussion_keys,e->percussion_draft,128);ed_commit(e);
        snprintf(e->percussion_feedback,sizeof(e->percussion_feedback),"Saved for this project. Save the project file to keep it.");
    } else {
        uint8_t before[128];memcpy(before,e->percussion_global,128);memcpy(e->percussion_global,e->percussion_draft,128);
        if(!dm_preferences_save(e,ed_shortcut_schema(),ed_action_names)) {
            memcpy(e->percussion_global,before,128);snprintf(e->percussion_feedback,sizeof(e->percussion_feedback),"Could not save global preferences.");return;
        }
        snprintf(e->percussion_feedback,sizeof(e->percussion_feedback),"%s",e->project.percussion_override?"Saved globally. This project still uses its own mapping.":"Saved to global preferences.");
    }
}
void ed_percussion_dialog(Editor *e) {
    if(!e->percussion_window){e->percussion_listening=false;return;}
    if(ui_dialog("Percussion keyboard mapping",&e->percussion_window,780,580)) {
        ui_wrap(e->project.percussion_override?"This project uses its own percussion keyboard mapping.":"This project uses the global percussion keyboard mapping.");
        float width=ui_available().x,left=width*.42f,height=fmaxf(120,ui_available().y-105);
        if(ui_child("percussion-sources",left,height,true)) {
          float origin=ui_cursor().y;
          for(int note=0;note<128;note++) {
            if(e->percussion_reveal&&note==e->percussion_note)ui_scroll_to(fmaxf(0,ui_cursor().y-origin-50));
            char label[160];snprintf(label,sizeof(label),"%d: %s",note,dm_drum_name(note));ui_id(note);
            if(ui_select(label,e->percussion_note==note,0,0)) {
                e->percussion_note=note;e->percussion_listening=false;
                dm_audio_keyboard_program(e->audio,9,0);dm_audio_audition(e->audio,9,note,100,.25);
            }
            ui_pop_id();
          }
          e->percussion_reveal=false;
        }
        ui_child_end();ui_same();
        if(ui_child("percussion-keys",width-left-ui_item_spacing_x(),height,true)) {
            ui_wrap(dm_drum_name(e->percussion_note));
            if(ui_button(e->percussion_listening?"Listening...":"Listen for key",ui_available().x,0)){e->percussion_listening=true;e->percussion_feedback[0]=0;}
            if(*e->percussion_feedback)ui_wrap(e->percussion_feedback);
            ui_separator();ui_text("Assigned mappings");
            for(unsigned note=0;note<128;note++)if(e->percussion_draft[note]) {
                char label[200];snprintf(label,sizeof(label),"%u: %s  ->  %s",note,dm_drum_name((int)note),SDL_GetScancodeName(ed_piano_scancode(e->percussion_draft[note]-1)));ui_wrap(label);
            }
            if(ui_button("Reset mappings",ui_available().x,0)){memset(e->percussion_draft,0,128);e->percussion_listening=false;snprintf(e->percussion_feedback,sizeof(e->percussion_feedback),"Default piano behavior restored in this draft. Choose where to save it.");}
        }
        ui_child_end();
        float button=(ui_available().x-ui_item_spacing_x())*.5f;
        if(ui_button("Save for current project",button,0))ed_percussion_save(e,true);
        ui_same();if(ui_button("Save to preferences (global)",button,0))ed_percussion_save(e,false);
        if(e->project.percussion_override&&ui_button("Use global mapping for this project",ui_available().x,0)) {
            ed_release_piano(e);ed_checkpoint(e);e->project.percussion_override=false;memset(e->project.percussion_keys,0,128);ed_commit(e);ed_percussion_open(e);
        }
    }
    ui_end();if(!e->percussion_window)e->percussion_listening=false;
}
