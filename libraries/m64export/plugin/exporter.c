/* Generic duckmidi plugin adapter. Conversion stays in ../src; UI in ui.c. */
#define DME_BUILD_PLUGIN
#include "m64_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef M64PluginJob ExportJob;
static void *DME_CALL create_job(const void *instance,char *error,uint64_t cap) {
    const M64PluginUi *s=instance;const M64Api *api=m64_get_api_3();
    char bank_lower[sizeof(s->bank)];
    for(size_t i=0;i<sizeof(bank_lower);i++) {
        unsigned char c=(unsigned char)s->bank[i];
        bank_lower[i]=(char)(c>='A'&&c<='Z'?c+('a'-'A'):c);
    }
    if(!strcmp(bank_lower,"manifest")||!strcmp(bank_lower,"sequences-entry")) {
        snprintf(error,(size_t)cap,"Choose a bank name other than manifest or sequences-entry.");return NULL;
    }
    ExportJob *j=calloc(1,sizeof(*j));
    if(!j){snprintf(error,(size_t)cap,"Cannot allocate SM64 export settings.");return NULL;}
    snprintf(j->sequence,97,"%s",s->sequence);snprintf(j->bank,97,"%s",s->bank);
    for(unsigned ch=0;ch<16;ch++)for(unsigned p=0;p<128;p++)j->melodic[ch*128+p]=*api->default_program((uint32_t)s->melodic[ch][p]);
    for(unsigned p=0;p<128;p++)j->drums[p]=*api->default_drum((uint32_t)s->drums[p]);
    memcpy(j->presets,s->melodic,sizeof(j->presets));memcpy(j->drum_presets,s->drums,sizeof(j->drum_presets));
    j->request.target=M64_TARGET_SM64_US;j->request.loop=(uint32_t)s->loop;j->request.voice_limit=16;
    j->request.sequence_name=j->sequence;j->request.bank_name=j->bank;
    j->request.channel_programs=j->melodic;j->request.drums=j->drums;
    return j;
}
static int32_t M64_CALL cancel(void *user){const DmeHost *h=user;return h->cancelled(h->user);}
static void M64_CALL progress(void *user,uint32_t stage,uint32_t percent){const DmeHost *h=user;h->progress(h->user,(stage*100+percent)/4);}
static int32_t DME_CALL run(void *job,const DmeContext *c,const DmeHost *host,char *error,uint64_t cap) {
    ExportJob *j=job;const M64Api *api=m64_get_api_3();
    if(!c||c->event_count>2000000||(!c->events&&c->event_count)){snprintf(error,(size_t)cap,"Invalid SM64 source timeline.");return DME_INVALID;}
    for(uint64_t i=0;i<c->event_count;i++)if(c->events[i].port){snprintf(error,(size_t)cap,"M64 cannot preserve independent note ports. Remove note colors or export MIDI instead.");return DME_INVALID;}
    M64Event *events=c->event_count?calloc((size_t)c->event_count,sizeof(*events)):NULL;
    if(c->event_count&&!events){snprintf(error,(size_t)cap,"Cannot allocate SM64 timeline.");return DME_NOMEM;}
    for(uint64_t i=0;i<c->event_count;i++)events[i]=(M64Event){c->events[i].tick,c->events[i].source_id,c->events[i].status,c->events[i].a,c->events[i].b};
    j->request.events=events;j->request.event_count=c->event_count;j->request.duration=c->duration;j->request.ppq=c->ppq;
    M64Hooks hooks={(void *)host,cancel,progress};M64Result *result=NULL;
    int status=api->compile(&j->request,&hooks,&result);
    for(uint64_t i=0;i<api->diagnostic_count(result);i++) {
        M64DiagnosticView d;if(api->diagnostic_at(result,i,&d)!=M64_OK)continue;
        uint32_t flags=d.code==22||d.code==23||d.code==54||d.code==57?DME_DISPLAY:0;
        if(d.severity==M64_WARNING)flags|=DME_POPUP;
        host->diagnostic(host->user,d.severity,flags,d.message);
        if(d.severity==M64_ERROR&&!*error)snprintf(error,(size_t)cap,"%s",d.message);
    }
    bool ok=status==M64_OK;
    char names[M64_ARTIFACT_COUNT][128];
    snprintf(names[0],128,"%s.m64",j->sequence);snprintf(names[1],128,"%s.json",j->bank);
    snprintf(names[2],128,"sequences-entry.json");snprintf(names[3],128,"manifest.json");
    for(unsigned i=0;i<M64_ARTIFACT_COUNT&&ok;i++) {
        M64ArtifactView a;ok=!host->cancelled(host->user)&&api->artifact_at(result,i,&a)==M64_OK&&host->artifact(host->user,names[i],a.data,a.size);
    }
    api->result_free(result);free(events);
    if(host->cancelled(host->user)||status==M64_CANCELLED)return DME_CANCELLED;
    if(!ok&&!*error)snprintf(error,(size_t)cap,"SM64 conversion failed (status %d).",status);
    return ok?DME_OK:status==M64_NOMEM?DME_NOMEM:DME_FAILED;
}
static int32_t DME_CALL preflight(void *job,const DmeContext *c,const DmeHost *host,char *error,uint64_t cap) {
    ExportJob *j=job;const M64Api *api=m64_get_api_3();
    if(!c||c->event_count>2000000||(!c->events&&c->event_count)){snprintf(error,(size_t)cap,"Invalid source timeline.");return DME_INVALID;}
    for(uint64_t i=0;i<c->event_count;i++)if(c->events[i].port){snprintf(error,(size_t)cap,"M64 cannot preserve independent note ports. Remove note colors or export MIDI instead.");return DME_INVALID;}
    M64Event *events=calloc((size_t)c->event_count+1,sizeof(*events));if(!events)return DME_NOMEM;
    for(uint64_t i=0;i<c->event_count;i++)events[i]=(M64Event){c->events[i].tick,c->events[i].source_id,c->events[i].status,c->events[i].a,c->events[i].b};
    j->request.events=events;j->request.event_count=c->event_count;j->request.duration=c->duration;j->request.ppq=c->ppq;
    M64Hooks hooks={(void *)host,cancel,progress};M64Result *result=NULL;
    int status=api->analyze(&j->request,&hooks,&result);bool ok=status==M64_OK&&m64_review_build(j,c,result);
    if(status!=M64_OK)for(uint64_t i=0;i<api->diagnostic_count(result);i++){M64DiagnosticView d;api->diagnostic_at(result,i,&d);if(d.severity==M64_ERROR){snprintf(error,(size_t)cap,"%s",d.message);break;}}
    api->result_free(result);free(events);j->request.events=NULL;
    if(status==M64_CANCELLED||host->cancelled(host->user))return DME_CANCELLED;
    if(!ok&&!*error)snprintf(error,(size_t)cap,"Cannot analyze the selected sample ranges.");
    return ok?DME_OK:DME_FAILED;
}
static void DME_CALL destroy_job(void *job){m64_review_free(job);free(job);}
static const DmeExporter exporter={
    "org.duckmidi.sm64","M64","zip","M64 sequence package","Save M64 package (.zip)...",
    "Native SM64 US sequence and sound-bank export.",DME_ZIP,
    m64_ui_create,m64_ui_destroy,m64_ui_inspect,m64_ui_describe,m64_ui_set,m64_ui_reset,
    create_job,run,destroy_job,preflight,m64_review_describe,m64_review_set,m64_review_apply
};
static const DmeExporter *DME_CALL get_exporter(uint32_t i){return i?NULL:&exporter;}
static const DmePlugin plugin={DME_ABI,sizeof(DmePlugin),"org.duckmidi.m64export","SM64 exporter",1,get_exporter};
DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi){return abi==DME_ABI?&plugin:NULL;}
