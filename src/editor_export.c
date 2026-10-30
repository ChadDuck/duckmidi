#include "editor_export_internal.h"
#include "text/en.h"
#include <limits.h>

DmExportPlugins *ed_export_host(Editor *e) {
    if(e->export_plugins)return e->export_plugins;
    DmExportPlugins *h=calloc(1,sizeof(*h));if(!h)return NULL;
    h->pending_format=-1;h->message_format=-1;
    const char *base=SDL_GetBasePath();char directory[DM_PATH];
    if(!base||snprintf(directory,sizeof(directory),"%splugins",base)>=(int)sizeof(directory))
        snprintf(h->registry.errors,sizeof(h->registry.errors),"Cannot locate exporter plugins.");
    else dm_export_registry_load(&h->registry,directory);
    e->export_plugins=h;return h;
}
size_t ed_export_count(Editor *e){DmExportPlugins *h=ed_export_host(e);return h?h->registry.count:0;}
const char *ed_export_name(Editor *e,size_t index) {
    DmExportPlugins *h=ed_export_host(e);return h&&index<h->registry.count?h->registry.formats[index].api->name:"Unavailable exporter";
}
DmExportFormat *ed_export_selected(Editor *e) {
    DmExportPlugins *h=ed_export_host(e);
    return h&&e->export_type>0&&(size_t)e->export_type<=h->registry.count?&h->registry.formats[e->export_type-1]:NULL;
}
void ed_export_error(Editor *e,const char *message) {
    DmExportPlugins *h=ed_export_host(e);
    if(h){snprintf(h->message,sizeof(h->message),"%s",message);h->message_format=e->export_type-1;}
    ed_message(e,message);e->export_window=true;
}
static int32_t DME_CALL cancelled(void *user){return SDL_GetAtomicInt(&((DmPluginWork *)user)->cancel);}
static void DME_CALL progress(void *user,uint32_t percent){SDL_SetAtomicInt(&((DmPluginWork *)user)->progress,(int)(percent>100?100:percent));}
static void DME_CALL diagnostic(void *user,uint32_t severity,uint32_t flags,const char *message) {
    DmPluginWork *w=user;if(!message)return;
    if(severity==DME_ERROR&&!*w->message)snprintf(w->message,sizeof(w->message),"%s",message);
    if(severity==DME_WARNING)w->warnings++;
    if((flags&DME_POPUP)&&severity==DME_WARNING) {
        size_t used=strlen(w->popup),remaining=sizeof(w->popup)-used;
        if(!w->popup_truncated) {
            int length=snprintf(w->popup+used,remaining,"%s%s",used?"\n\n":"",message);
            if(length<0||(size_t)length>=remaining)w->popup_truncated=true;
        }
    }
    if(flags&DME_DISPLAY) {
        size_t used=strlen(w->summary);
        snprintf(w->summary+used,sizeof(w->summary)-used,"%s%.1000s",used?"\n":"",message);
    }
}
static bool artifact_name(const char *name) {
    if(!name||!*name||SDL_strnlen(name,256)>=256||!strcmp(name,".")||!strcmp(name,".."))return false;
    for(const unsigned char *s=(const unsigned char *)name;*s;s++)if(*s<32||strchr("/\\:*?\"<>|",*s))return false;
    return true;
}
static int32_t DME_CALL artifact(void *user,const char *name,const uint8_t *data,uint64_t size) {
    DmPluginWork *w=user;if(cancelled(w))return 0;
    if(w->review){snprintf(w->message,sizeof(w->message),"Exporter attempted output during preflight.");return 0;}
    if(!artifact_name(name)||(!data&&size)||size>512ULL*1024*1024||w->artifact_count==64||size>SIZE_MAX-w->total_bytes) {
        snprintf(w->message,sizeof(w->message),"Exporter returned an invalid artifact or exceeded the output limit.");return 0;
    }
    for(size_t i=0;i<w->artifact_count;i++)if(!SDL_strcasecmp(name,w->artifacts[i].name)) {
        snprintf(w->message,sizeof(w->message),"Exporter returned duplicate artifact names: %.255s.",name);return 0;
    }
    DmPluginArtifact *a=&w->artifacts[w->artifact_count];
    if(!dm_bytes(&a->data,data,(size_t)size)){snprintf(w->message,sizeof(w->message),"Not enough memory for exported artifacts.");return 0;}
    snprintf(a->name,sizeof(a->name),"%s",name);w->artifact_count++;w->total_bytes+=(size_t)size;return 1;
}
static bool publish(DmPluginWork *w) {
    SDL_PathInfo info;
    if(SDL_GetPathInfo(w->destination,&info)&&info.type==SDL_PATHTYPE_DIRECTORY){snprintf(w->message,sizeof(w->message),"Cannot save export: destination is a folder, not a file.");return false;}
    if(!w->artifact_count||(w->api->container==DME_SINGLE_FILE&&w->artifact_count!=1)) {
        snprintf(w->message,sizeof(w->message),"Exporter returned an invalid number of artifacts.");return false;
    }
    DmBytes archive={0};const void *data=w->artifacts[0].data.data;size_t size=w->artifacts[0].data.size;
    bool ok=true;
    if(w->api->container==DME_ZIP) {
        DmArchiveEntry entries[64];
        for(size_t i=0;i<w->artifact_count;i++)entries[i]=(DmArchiveEntry){w->artifacts[i].name,w->artifacts[i].data.data,w->artifacts[i].data.size};
        ok=dm_zip(entries,w->artifact_count,&archive,w->message,sizeof(w->message));data=archive.data;size=archive.size;
    }
    DmOutput output={0};
    if(ok)ok=dm_output_begin(&output,w->destination,w->message,sizeof(w->message));
    if(output.file) {
        for(size_t pos=0;pos<size&&ok;pos+=16384) {
            size_t n=size-pos;if(n>16384)n=16384;
            ok=!cancelled(w)&&dm_output_write(&output,(const uint8_t *)data+pos,n,w->message,sizeof(w->message));
        }
        ok=dm_output_finish(&output,ok&&!cancelled(w),w->message,sizeof(w->message));
    }
    dm_bytes_free(&archive);
    if(!ok&&!cancelled(w)) {
        char reason[2048];snprintf(reason,sizeof(reason),"%.2000s",*w->message?w->message:"Unknown output failure.");
        snprintf(w->message,sizeof(w->message),"Cannot save export: %s\nDestination: %s",reason,w->destination);
    }
    return ok;
}
static bool resolve(DmPluginWork *w) {
    DmProject p;dm_project_init(&p);DmExportEvent *events=NULL;size_t count=0;uint64_t duration=0;
    bool ok=!cancelled(w)&&dm_unpack(&p,w->snapshot.data,w->snapshot.size,w->message,sizeof(w->message));
    if(ok)ok=dm_export_timeline(&p,&events,&count,&duration,w->message,sizeof(w->message));
    if(ok&&count){w->events=calloc(count,sizeof(*w->events));if(!w->events){ok=false;snprintf(w->message,sizeof(w->message),"Not enough memory for export timeline.");}}
    if(ok) {
        for(size_t i=0;i<count;i++)w->events[i]=(DmeEvent){events[i].tick,events[i].source_id,events[i].status,events[i].a,events[i].b,events[i].port};
        snprintf(w->project_name,sizeof(w->project_name),"%s",p.name);
        w->context=(DmeContext){.events=w->events,.event_count=count,.duration=duration,.project_session=w->session,.ppq=DM_PPQ,.project_name=w->project_name,.source_name=w->source_name};
        for(unsigned i=0;i<128;i++){w->context.program_names[i]=dm_program_name((int)i);w->context.drum_names[i]=dm_drum_name((int)i);}
    }
    free(events);dm_project_free(&p);return ok&&!cancelled(w);
}
static int worker(void *user) {
    DmPluginWork *w=user;bool ok=w->prepared||resolve(w);w->prepared=ok;
    if(ok&&w->inspection)w->success=true;
    else if(ok) {
        DmeHost host={w,cancelled,progress,diagnostic,artifact};char error[2048]={0};
        int32_t status=w->review?w->api->preflight(w->job,&w->context,&host,error,sizeof(error)):w->api->run(w->job,&w->context,&host,error,sizeof(error));
        if(status==DME_OK&&!*w->message&&!cancelled(w))w->success=w->review||publish(w);
        else if(!*w->message)snprintf(w->message,sizeof(w->message),"%s",*error?error:"Exporter failed without a diagnostic.");
        if(w->success&&!w->review)snprintf(w->message,sizeof(w->message),"%.128s export complete (%u warnings): %.4095s%s%.3700s",w->api->name,w->warnings,w->destination,*w->summary?"\n":"",w->summary);
    }
    if(cancelled(w)&&!w->success)snprintf(w->message,sizeof(w->message),"Export canceled.");
    dm_bytes_free(&w->snapshot);SDL_SetAtomicInt(&w->done,1);return 0;
}
static void clear_work(DmPluginWork *w) {
    if(w->job)w->api->destroy_job(w->job);
    for(size_t i=0;i<64;i++)dm_bytes_free(&w->artifacts[i].data);
    free(w->events);dm_bytes_free(&w->snapshot);memset(w,0,sizeof(*w));
}
bool ed_export_review_open(Editor *e){return e->export_plugins&&e->export_plugins->review_pending;}
void ed_export_review_decide(Editor *e,int decision) {
    DmExportPlugins *h=e->export_plugins;if(!h||!h->review_pending)return;
    DmPluginWork *w=&h->work;h->review_pending=false;
    if(!decision){clear_work(w);ed_message(e,"Export canceled.");return;}
    if(w->revision!=e->export_revision||w->session!=e->project_session){clear_work(w);ed_export_error(e,"The arrangement changed. Start the export again to review its current mappings.");return;}
    if(decision==2&&w->api->review_apply(h->registry.formats[w->format_index].instance,w->job,w->message,sizeof(w->message))!=DME_OK){ed_export_error(e,*w->message?w->message:"Cannot apply reviewed mappings.");clear_work(w);return;}
    w->review=false;w->success=false;w->message[0]=w->summary[0]=w->popup[0]=0;w->warnings=0;
    SDL_SetAtomicInt(&w->done,0);SDL_SetAtomicInt(&w->progress,0);
    w->thread=SDL_CreateThread(worker,"Exporter",w);
    if(!w->thread){ed_export_error(e,"Cannot start export worker.");clear_work(w);}
}
void ed_export_poll(Editor *e) {
    DmExportPlugins *h=e->export_plugins;if(!h)return;DmPluginWork *w=&h->work;
    if(!w->thread||!SDL_GetAtomicInt(&w->done))return;
    SDL_WaitThread(w->thread,NULL);w->thread=NULL;
    if(w->review&&w->success) {
        DmeReview review={0};
        if(w->api->review(w->job,&review)==DME_OK&&dm_export_review_valid(&review,w->message,sizeof(w->message))) {
            h->review_pending=true;h->review_selected=0;
            if(!review.count)ed_export_review_decide(e,1);
            return;
        }
        w->success=false;
        if(!*w->message)snprintf(w->message,sizeof(w->message),"Cannot read export mapping review.");
    }
    if(w->inspection) {
        if(w->success&&w->revision==e->export_revision&&w->session==e->project_session) {
            free(h->events);h->events=w->events;w->events=NULL;h->context=w->context;h->context.events=h->events;
            snprintf(h->project_name,sizeof(h->project_name),"%s",w->project_name);snprintf(h->source_name,sizeof(h->source_name),"%s",w->source_name);
            h->context.project_name=h->project_name;h->context.source_name=h->source_name;
            h->revision=w->revision;h->session=w->session;h->context_valid=true;h->inspection_failed=false;
            memset(h->inspected,0,sizeof(h->inspected));
        } else if(!w->success) {
            snprintf(h->message,sizeof(h->message),"Cannot read export arrangement: %.8000s",w->message);
            h->inspection_failed=true;h->failed_revision=w->revision;h->failed_session=w->session;
            h->message_format=-1;
        }
    } else {
        snprintf(h->message,sizeof(h->message),"%s",w->message);h->message_format=w->format_index;ed_message(e,w->message);
        if(w->success&&*w->popup) {
            snprintf(h->warning,sizeof(h->warning),"%s export was saved with audible changes or playback limitations.\n\n%s%s\n\nSaved to: %s\n\nYour source project was not changed.",w->api->name,w->popup,w->popup_truncated?"\n\nAdditional warnings were omitted from this popup; review the exporter's diagnostics.":"",w->destination);
            h->warning_open=true;
        }
        if(!w->success){e->export_window=true;e->export_type=w->format_index+1;}
    }
    clear_work(w);
}
static bool begin(Editor *e,bool inspection,int index,const char *path) {
    DmExportPlugins *h=ed_export_host(e);if(!h)return false;
    DmPluginWork *w=&h->work;
    if(w->thread||h->review_pending){if(!inspection)ed_export_error(e,"An export task is already running.");return false;}
    memset(w,0,sizeof(*w));w->inspection=inspection;w->revision=e->export_revision;w->session=e->project_session;w->format_index=index;
    snprintf(w->source_name,sizeof(w->source_name),"%s",e->export_source);
    if(!inspection) {
        if(index<0||(size_t)index>=h->registry.count){ed_export_error(e,"The selected exporter is unavailable.");return false;}
        if(!path||strlen(path)>=DM_PATH){ed_export_error(e,"Export path is too long.");return false;}
        w->review=true;w->api=h->registry.formats[index].api;
        w->job=w->api->create_job(h->registry.formats[index].instance,w->message,sizeof(w->message));
        if(!w->job){ed_export_error(e,*w->message?w->message:"Cannot freeze exporter settings.");clear_work(w);return false;}
        snprintf(w->destination,sizeof(w->destination),"%s",path);
    }
    if(!dm_pack(&e->project,&w->snapshot)){ed_export_error(e,"Cannot snapshot project for export.");clear_work(w);return false;}
    w->thread=SDL_CreateThread(worker,inspection?"Export inspection":"Exporter",w);
    if(!w->thread){char message[512];snprintf(message,sizeof(message),"Cannot start export worker: %.450s",SDL_GetError());ed_export_error(e,message);clear_work(w);return false;}
    if(!inspection){h->message[0]=0;ed_message(e,"Exporting...");}
    return true;
}
bool ed_export_context(Editor *e) {
    DmExportPlugins *h=ed_export_host(e);if(!h)return false;
    ed_export_poll(e);
    if(!h->context_valid||h->revision!=e->export_revision||h->session!=e->project_session) {
        if(h->inspection_failed&&h->failed_revision==e->export_revision&&h->failed_session==e->project_session)return false;
        if(!h->work.thread&&!h->review_pending&&!begin(e,true,-1,NULL)) {
            h->inspection_failed=true;h->failed_revision=e->export_revision;h->failed_session=e->project_session;
        }
        return false;
    }
    return !h->work.thread&&!h->review_pending;
}
bool ed_export_settings(Editor *e,size_t index,DmeUi *ui) {
    DmExportPlugins *h=ed_export_host(e);if(!h||index>=h->registry.count||!ed_export_context(e))return false;
    DmExportFormat *f=&h->registry.formats[index];
    if(!h->inspected[index]) {
        char error[512]={0};
        if(f->api->inspect(f->instance,&h->context,error,sizeof(error))!=DME_OK){ed_export_error(e,*error?error:"Exporter cannot inspect this arrangement.");return false;}
        h->inspected[index]=1;
        memset(f->selected_rows,0,sizeof(f->selected_rows));
    }
    char error[512];
    if(f->api->describe(f->instance,ui)!=DME_OK||!dm_export_ui_valid(ui,error,sizeof(error))){ed_export_error(e,"Exporter returned an invalid settings schema.");return false;}
    return true;
}
bool ed_export_set(Editor *e,size_t index,const char *id,DmeValue value) {
    DmExportPlugins *h=ed_export_host(e);
    if(!h||index>=h->registry.count||h->work.thread||h->review_pending||e->dialog_busy)return false;
    DmExportFormat *f=&h->registry.formats[index];char error[512]={0};
    if(f->api->set(f->instance,id,&value,error,sizeof(error))!=DME_OK){ed_export_error(e,*error?error:"Invalid exporter setting.");return false;}
    return true;
}
void ed_export_start(Editor *e,size_t index,const char *path){begin(e,false,(int)index,path);}
void ed_export_free(Editor *e) {
    DmExportPlugins *h=e->export_plugins;if(!h)return;
    SDL_SetAtomicInt(&h->work.cancel,1);
    if(h->work.thread)SDL_WaitThread(h->work.thread,NULL);
    clear_work(&h->work);free(h->events);dm_export_registry_free(&h->registry);free(h);e->export_plugins=NULL;
}
static const char *leaf(const char *path) {
    const char *s=strrchr(path,'/'),*b=strrchr(path,'\\');if(!s||(b&&b>s))s=b;return s?s+1:path;
}
bool ed_export_suggest(Editor *e,const char *extension,char *out,size_t cap) {
    char name[DM_PATH];bool source=!*e->project.name||!strcmp(e->project.name,DM_TEXT_UNTITLED);
    snprintf(name,sizeof(name),"%s",source?leaf(*e->path?e->path:e->export_source):e->project.name);
    if(source){char *dot=strrchr(name,'.');if(dot)*dot=0;}
    for(unsigned char *s=(unsigned char *)name;*s;s++)if(*s<32||strchr("/\\:*?\"<>|",*s))*s='_';
    size_t len=strlen(name);while(len&&(name[len-1]==' '||name[len-1]=='.'))name[--len]=0;
    if(!*name)snprintf(name,sizeof(name),"export");
    const char *devices[]={"CON","PRN","AUX","NUL","COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9","LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"};
    char stem[16];size_t n=strcspn(name,".");if(n<sizeof(stem)){memcpy(stem,name,n);stem[n]=0;for(size_t i=0;i<sizeof(devices)/sizeof(*devices);i++)if(!SDL_strcasecmp(stem,devices[i])){if(strlen(name)+1>=sizeof(name))return false;memmove(name+1,name,strlen(name)+1);name[0]='_';break;}}
    return snprintf(out,cap,"%s%s.%s",e->export_folder,name,extension)<(int)cap;
}
bool ed_export_picker(Editor *e,const SDL_DialogFileFilter **filter,const char **suggested) {
    DmExportPlugins *h=ed_export_host(e);DmExportFormat *f=ed_export_selected(e);
    if(!h||!f||h->work.thread||h->review_pending){ed_export_error(e,"The selected exporter is unavailable or busy.");return false;}
    if(!ed_export_suggest(e,f->api->extension,h->suggested,sizeof(h->suggested))){ed_export_error(e,"The remembered export path is too long.");return false;}
    h->pending_format=e->export_type-1;h->filter=(SDL_DialogFileFilter){f->api->filter_name,f->api->extension};
    *filter=&h->filter;*suggested=h->suggested;return true;
}
const char *ed_export_extension(Editor *e) {
    DmExportPlugins *h=ed_export_host(e);
    if(!h||h->pending_format<0||(size_t)h->pending_format>=h->registry.count)return "";
    return h->registry.formats[h->pending_format].api->extension;
}
void ed_export_picker_done(Editor *e,const char *path) {
    DmExportPlugins *h=ed_export_host(e);if(!h)return;
    int index=h->pending_format;h->pending_format=-1;
    if(path&&index>=0)begin(e,false,index,path);
}
