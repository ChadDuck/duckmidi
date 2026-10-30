/* M64 pre-export mapping UI data and recommendation policy. No editor/UI toolkit. */
#include "m64_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
typedef struct {
    M64RangeIssue issue;
    int selected;
    char id[48],label[180],description[384],current[256];
    DmeChoice recommended[8];
    unsigned recommended_count;
} ReviewItem;
struct M64ReviewUi {
    unsigned count;
    ReviewItem *items;
    DmeReviewRow *rows;
    DmeChoice melodic[128],drums[128];
    char melodic_labels[128][220],drum_labels[128][220];
};
static int drum_family(int note) {
    if(note==35||note==36)return 0;
    if(note==37||note==38||note==39||note==40)return 1;
    if(note==41||note==43||note==45||note==47||note==48||note==50)return 2;
    if(note==42||note==44||note==46)return 3;
    if(note==49||note==51||note==52||note==53||note==55||note==57||note==59)return 4;
    if(note>=60&&note<=81)return 5;
    return 6;
}
static int score(int original,int preset,bool percussion) {
    if(percussion)return (drum_family(original)==drum_family(preset)?0:200)+abs(original-preset);
    int value=abs(original/8-preset/8)*100+abs(original-preset);
    /* Prefer names matching the actual native timbre over GM approximations
       sharing that sample (e.g. French Horn before Tuba for the stock horns). */
    if((original/8==7&&preset==60)||(original/8==5&&preset==40)||(original/8==0&&preset==0)||(original/8==10&&preset==80))value-=16;
    return value;
}
void m64_review_free(M64PluginJob *job) {
    if(!job||!job->review_ui)return;
    free(job->review_ui->items);free(job->review_ui->rows);free(job->review_ui);job->review_ui=NULL;
}
bool m64_review_build(M64PluginJob *job,const DmeContext *context,const M64Result *result) {
    const M64Api *api=m64_get_api_3();m64_review_free(job);
    uint64_t count=api->range_count(result);if(!count)return true;if(count>2048)return false;
    M64ReviewUi *ui=calloc(1,sizeof(*ui));if(!ui)return false;job->review_ui=ui;ui->count=(unsigned)count;
    ui->items=calloc(ui->count,sizeof(*ui->items));ui->rows=calloc(ui->count,sizeof(*ui->rows));
    if(!ui->items||!ui->rows){m64_review_free(job);return false;}
    for(unsigned p=0;p<128;p++) {
        snprintf(ui->melodic_labels[p],220,"%u: %s / %s",p,context->program_names[p],api->default_program(p)->sample);
        snprintf(ui->drum_labels[p],220,"%u: %s / %s",p,context->drum_names[p],api->default_drum(p)->sample);
        ui->melodic[p]=(DmeChoice){(int32_t)p,ui->melodic_labels[p]};ui->drums[p]=(DmeChoice){(int32_t)p,ui->drum_labels[p]};
    }
    for(unsigned i=0;i<ui->count;i++) {
        ReviewItem *item=&ui->items[i];if(api->range_at(result,i,&item->issue)!=M64_OK){m64_review_free(job);return false;}
        M64RangeIssue *issue=&item->issue;unsigned ch=issue->channel,p=issue->program;
        int original=issue->percussion?job->drum_presets[p]:job->presets[ch][p];
        snprintf(item->id,48,issue->percussion?"drum.%u":"melodic.%u.%u",issue->percussion?p:ch,p);
        snprintf(item->label,180,"Channel %u: %s (%s %u)",ch+1,issue->percussion?context->drum_names[p]:context->program_names[p],issue->percussion?"note":"GM",p);
        snprintf(item->current,256,"%s",issue->percussion?ui->drum_labels[original]:ui->melodic_labels[original]);
        snprintf(item->description,384,"The current stock mapping cannot cover MIDI keys %u-%u (%u notes; up to %.3fx playback, limit near 4x). Choose a supported substitute, or export unchanged and accept possible pitch clipping.",issue->note_low,issue->note_high,issue->note_count,issue->maximum_rate);
        bool visited[128]={0};
        for(unsigned pass=0;pass<128&&item->recommended_count<8;pass++) {
            int best=-1,best_score=INT_MAX;
            for(int candidate=0;candidate<128;candidate++) {
                if(visited[candidate]||(issue->percussion&&(candidate<35||candidate>81))||!api->preset_supported(issue,(uint32_t)candidate))continue;
                int rank=score(original,candidate,issue->percussion!=0);if(rank<best_score){best=candidate;best_score=rank;}
            }
            if(best<0)break;
            visited[best]=true;
            const M64Instrument *definition=issue->percussion?api->default_drum((unsigned)best):api->default_program((unsigned)best);bool duplicate=false;
            for(unsigned n=0;n<item->recommended_count;n++) {
                unsigned preset=(unsigned)item->recommended[n].value;
                const M64Instrument *other=issue->percussion?api->default_drum(preset):api->default_program(preset);
                if(!strcmp(definition->sample,other->sample)&&definition->tuning==other->tuning)duplicate=true;
            }
            if(!duplicate)item->recommended[item->recommended_count++]=issue->percussion?ui->drums[best]:ui->melodic[best];
        }
        item->selected=item->recommended_count?item->recommended[0].value:original;
    }
    return true;
}
int32_t DME_CALL m64_review_describe(void *user,DmeReview *review) {
    M64PluginJob *job=user;M64ReviewUi *ui=job->review_ui;
    if(!ui){*review=(DmeReview){0};return DME_OK;}
    const M64Api *api=m64_get_api_3();
    for(unsigned i=0;i<ui->count;i++) {
        ReviewItem *item=&ui->items[i];
        ui->rows[i]=(DmeReviewRow){.id=item->id,.label=item->label,.description=item->description,.current_mapping=item->current,.value=item->selected,.supported=api->preset_supported(&item->issue,(unsigned)item->selected),.recommended=item->recommended,.recommended_count=item->recommended_count,.choices=item->issue.percussion?ui->drums:ui->melodic,.choice_count=128};
    }
    *review=(DmeReview){"Some selected mappings exceed SM64's stock sample range. Recommended substitutes preserve the note pitches but change the instrument sound. Only the affected instruments are listed below.",ui->rows,ui->count};return DME_OK;
}
int32_t DME_CALL m64_review_set(void *user,const char *id,int32_t value,char *error,uint64_t cap) {
    M64PluginJob *job=user;M64ReviewUi *ui=job->review_ui;
    if(ui&&id&&value>=0&&value<128)for(unsigned i=0;i<ui->count;i++)if(!strcmp(id,ui->items[i].id)){ui->items[i].selected=value;return DME_OK;}
    snprintf(error,(size_t)cap,"Invalid review mapping.");return DME_INVALID;
}
int32_t DME_CALL m64_review_apply(void *instance,void *user,char *error,uint64_t cap) {
    (void)error;(void)cap;M64PluginUi *settings=instance;M64PluginJob *job=user;M64ReviewUi *ui=job->review_ui;const M64Api *api=m64_get_api_3();
    if(ui)for(unsigned i=0;i<ui->count;i++) {
        ReviewItem *item=&ui->items[i];unsigned p=item->issue.program,ch=item->issue.channel;int selected=item->selected;
        if(item->issue.percussion){job->drums[p]=*api->default_drum((unsigned)selected);settings->drums[p]=job->drum_presets[p]=selected;}
        else {job->melodic[ch*128+p]=*api->default_program((unsigned)selected);settings->melodic[ch][p]=job->presets[ch][p]=selected;}
    }
    return DME_OK;
}
