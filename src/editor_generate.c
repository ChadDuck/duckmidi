#include "editor_internal.h"
#include "../shared/pattern_generation.h"
typedef struct DmPatternJob {
    DmProject source,result;
    DmGenerationStats stats;
    SDL_Thread *thread;
    SDL_AtomicInt cancel,done,progress;
    uint64_t revision,session;
    bool success;
    char error[512];
    uint8_t overview[16][512];
} DmPatternJob;
static bool cancelled(void *user){return SDL_GetAtomicInt(&((DmPatternJob *)user)->cancel)!=0;}
static void progress(void *user,unsigned value){SDL_SetAtomicInt(&((DmPatternJob *)user)->progress,(int)value);}
static int generate(void *user) {
    DmPatternJob *job=user;DmGenerationHooks hooks={job,cancelled,progress};
    job->success=dm_generate_patterns(&job->source,&job->result,&hooks,&job->stats,job->error,sizeof(job->error));
    if(job->success) {
        int diff[16][2][513]={0};double duration=fmax(1,dm_duration(&job->result));
        for(size_t i=0;i<job->result.clip_count;i++) {
            DmClip c=job->result.clips[i];int kind=job->result.patterns[c.pattern].kind;
            int start=(int)fmin(511,floor(c.beat/duration*512)),end=(int)fmin(512,ceil((c.beat+dm_clip_length(&job->result,c))/duration*512));
            if(end<=start)end=start+1;
            diff[c.channel][kind][start]++;diff[c.channel][kind][end]--;job->overview[c.channel][start]|=4;
        }
        for(int ch=0;ch<16;ch++)for(int kind=0;kind<2;kind++){int active=0;for(int i=0;i<512;i++){active+=diff[ch][kind][i];if(active)job->overview[ch][i]|=(uint8_t)(1<<kind);}}
    }
    SDL_SetAtomicInt(&job->done,1);return 0;
}
bool ed_generate_busy(Editor *e){return e->pattern_job&&e->pattern_job->thread!=NULL;}
const DmProject *ed_generate_preview(Editor *e){DmPatternJob *job=e->pattern_job;return job&&!job->thread&&job->success?&job->result:NULL;}
void ed_generate_free(Editor *e) {
    DmPatternJob *job=e->pattern_job;if(!job)return;SDL_SetAtomicInt(&job->cancel,1);
    if(job->thread)SDL_WaitThread(job->thread,NULL);
    dm_project_free(&job->source);dm_project_free(&job->result);free(job);e->pattern_job=NULL;
}
void ed_generate_start(Editor *e) {
    if(ed_generate_busy(e))return;
    ed_generate_free(e);
    DmPatternJob *job=calloc(1,sizeof(*job));if(!job){ed_message(e,"Cannot allocate pattern analysis.");return;}
    dm_project_init(&job->source);dm_project_init(&job->result);job->revision=e->export_revision;job->session=e->project_session;
    if(!dm_project_copy(&job->source,&e->project)){dm_project_free(&job->source);free(job);ed_message(e,"Cannot snapshot the arrangement.");return;}
    job->thread=SDL_CreateThread(generate,"Pattern generation",job);
    if(!job->thread){dm_project_free(&job->source);free(job);ed_message(e,"Cannot start pattern analysis worker.");return;}
    e->pattern_job=job;e->generate_window=true;
}
void ed_generate_poll(Editor *e) {
    DmPatternJob *job=e->pattern_job;if(!job)return;
    if(!e->generate_window)SDL_SetAtomicInt(&job->cancel,1);
    if(job->thread&&SDL_GetAtomicInt(&job->done)){SDL_WaitThread(job->thread,NULL);job->thread=NULL;dm_project_free(&job->source);}
    if(!e->generate_window&&!job->thread)ed_generate_free(e);
}
bool ed_generate_apply(Editor *e) {
    DmPatternJob *job=e->pattern_job;if(!ed_generate_preview(e)||e->record)return false;
    if(job->session!=e->project_session||job->revision!=e->export_revision){snprintf(job->error,sizeof(job->error),"The arrangement changed. Generate again before applying.");return false;}
    ed_release_piano(e);ed_checkpoint(e);dm_project_free(&e->project);e->project=job->result;dm_project_init(&job->result);
    e->selected_pattern=-1;e->selected_clip=-1;e->drag=0;e->automation_node=-1;e->clip_drag.size=0;
    for(size_t i=0;i<e->project.clip_count;i++)if(!e->project.patterns[e->project.clips[i].pattern].kind){e->selected_pattern=e->project.clips[i].pattern;e->pattern_channel=e->project.clips[i].channel;break;}
    ed_deselect(e);ed_open_pattern(e,e->selected_pattern,false);ed_commit(e);e->generate_window=false;ed_generate_free(e);ed_message(e,"Generated patterns applied. Undo restores the previous arrangement.");return true;
}
static void preview(Editor *e,DmPatternJob *job) {
    float available=ui_available().x,left=fminf(240,available*.3f),height=fmaxf(140,ui_available().y-50);
    if(ui_child("generated-patterns",left,height,true)) {
        ui_text("Patterns");size_t shown=job->result.pattern_count<500?job->result.pattern_count:500;
        for(size_t i=0;i<shown;i++)ui_text(job->result.patterns[i].name);
        if(shown<job->result.pattern_count)ui_dim("Showing the first 500 patterns.");
    }
    ui_child_end();ui_same();
    if(ui_child("generated-placements",available-left-ui_item_spacing_x(),height,true)) {
        ui_text("Arrangement preview");ui_dim("Green: notes; gold: automation. Marks show placement starts.");
        UiPoint at=ui_cursor();float width=ui_available().x,row=22;ui_canvas("generation-overview",width,row*16);
        for(int ch=0;ch<16;ch++) {
            char label[8];snprintf(label,sizeof(label),"%02d",ch+1);ui_label((UiPoint){at.x,at.y+ch*row},DIM,label);
            for(int kind=0;kind<2;kind++)for(int i=0;i<512;) {
                if(!(job->overview[ch][i]&(1<<kind))){i++;continue;}
                int end=i+1;while(end<512&&(job->overview[ch][end]&(1<<kind)))end++;
                UiRect box={at.x+28+(width-28)*i/512,at.y+ch*row+2+kind*9,(width-28)*(end-i)/512,8};
                ui_rect(box,kind?GOLD:GREEN,0);i=end;
            }
            for(int i=0;i<512;i++)if(job->overview[ch][i]&4){float x=at.x+28+(width-28)*i/512;ui_line((UiPoint){x,at.y+ch*row+2},(UiPoint){x,at.y+ch*row+20},BG,1);}
        }
        UiMouse mouse=ui_mouse();int channel=(int)((mouse.pos.y-at.y)/row);
        if(channel>=0&&channel<16&&mouse.pos.x>=at.x+28&&mouse.pos.x<at.x+width) {
            double beat=(mouse.pos.x-at.x-28)/(width-28)*fmax(1,dm_duration(&job->result));char tip[1024];int used=snprintf(tip,sizeof(tip),"Channel %d, beat %.2f",channel+1,beat);
            for(size_t i=0;i<job->result.clip_count&&used<800;i++){DmClip c=job->result.clips[i];if(c.channel==channel&&beat>=c.beat&&beat<c.beat+dm_clip_length(&job->result,c))used+=snprintf(tip+used,sizeof(tip)-(size_t)used,"\n%s (beat %.2f, length %.2f)",job->result.patterns[c.pattern].name,c.beat,dm_clip_length(&job->result,c));}
            ui_tip(tip);
        }
    }
    ui_child_end();
}
void ed_generate_dialog(Editor *e) {
    if(!e->generate_window)return;
    bool expanded=ed_generate_preview(e)!=NULL;
    if(ui_fixed_dialog("Generate Patterns",&e->generate_window,expanded?950:540,expanded?650:210)) {
        ui_wrap("Analyze and separate midi arrangement to duckmidi-compatible patterns.");
        bool busy=ed_generate_busy(e);ui_disabled(busy);
        if(ui_button("Generate",140,0))ed_generate_start(e);
        ui_disabled_end();ui_same();ui_disabled(busy||!ed_generate_preview(e)||e->record);
        if(ui_button("Apply",140,0))ed_generate_apply(e);
        ui_disabled_end();
        DmPatternJob *job=e->pattern_job;
        if(job&&job->thread){ui_progress(SDL_GetAtomicInt(&job->progress)/100.0f,300,0,"Analyzing arrangement");if(ui_button("Cancel generation",0,0))SDL_SetAtomicInt(&job->cancel,1);}
        else if(job) {
            if(*job->error)ui_wrap(job->error);
            if(job->success){char summary[256];snprintf(summary,sizeof(summary),"%zu note patterns, %zu control patterns, %zu placements; %zu linked repeats; %zu notes retained.",job->stats.note_patterns,job->stats.automation_patterns,job->stats.placements,job->stats.linked_placements,job->stats.notes);ui_wrap(summary);preview(e,job);}
        }
    }
    ui_end();
}
