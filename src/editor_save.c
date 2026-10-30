#include "editor_internal.h"
#include "text/en.h"
typedef struct DmSaveJob {
    DmProject project;
    SDL_Thread *thread;
    SDL_AtomicInt done,cancel;
    uint64_t session,revision;
    bool success;
    char path[DM_PATH],error[512];
} DmSaveJob;
static int save_worker(void *user) {
    DmSaveJob *job=user;DmBytes data={0};DmOutput output={0};
    bool ok=!SDL_GetAtomicInt(&job->cancel)&&dm_pack(&job->project,&data);
    if(!ok&&!SDL_GetAtomicInt(&job->cancel))snprintf(job->error,sizeof(job->error),"Cannot serialize the project snapshot.");
    if(ok)ok=dm_output_begin(&output,job->path,job->error,sizeof(job->error));
    if(output.file) {
        for(size_t i=0;i<data.size&&ok;i+=65536) {
            size_t n=data.size-i;if(n>65536)n=65536;
            ok=!SDL_GetAtomicInt(&job->cancel)&&dm_output_write(&output,data.data+i,n,job->error,sizeof(job->error));
        }
        ok=dm_output_finish(&output,ok&&!SDL_GetAtomicInt(&job->cancel),job->error,sizeof(job->error));
    }
    job->success=ok;dm_bytes_free(&data);SDL_SetAtomicInt(&job->done,1);return 0;
}
bool ed_save_busy(Editor *e){return e->save_job!=NULL;}
void ed_save_cancel(Editor *e){if(e->save_job)SDL_SetAtomicInt(&e->save_job->cancel,1);}
void ed_save_free(Editor *e) {
    DmSaveJob *job=e->save_job;if(!job)return;
    SDL_SetAtomicInt(&job->cancel,1);SDL_WaitThread(job->thread,NULL);
    dm_project_free(&job->project);free(job);e->save_job=NULL;
}
bool ed_save_to(Editor *e,const char *path) {
    if(ed_save_busy(e)){ed_message(e,"A project save is already running.");return false;}
    if(!path||!*path||strlen(path)>=DM_PATH){ed_message(e,"Invalid project save path.");return false;}
    DmSaveJob *job=calloc(1,sizeof(*job));
    if(!job){ed_message(e,"Cannot allocate project save worker.");return false;}
    dm_project_init(&job->project);
    if(!dm_project_copy(&job->project,&e->project)){dm_project_free(&job->project);free(job);ed_message(e,"Cannot snapshot project for saving.");return false;}
    snprintf(job->path,sizeof(job->path),"%s",path);job->session=e->project_session;job->revision=e->export_revision;
    job->thread=SDL_CreateThread(save_worker,"Project save",job);
    if(!job->thread){dm_project_free(&job->project);free(job);ed_message(e,"Cannot start project save worker.");return false;}
    e->save_job=job;ed_message(e,"Saving project...");return true;
}
void ed_save_poll(Editor *e) {
    DmSaveJob *job=e->save_job;if(!job||!SDL_GetAtomicInt(&job->done))return;
    SDL_WaitThread(job->thread,NULL);e->save_job=NULL;
    bool current=job->session==e->project_session,unchanged=current&&job->revision==e->export_revision;
    if(job->success&&current) {
        snprintf(e->path,sizeof(e->path),"%s",job->path);
        if(unchanged)e->dirty=false;
        ed_add_recent(e,job->path);ed_preference_save(e);
        ed_message(e,unchanged?DM_TEXT_PROJECT_SAVED:"Project snapshot saved; newer edits remain unsaved.");
    } else if(current) {
        char message[640];snprintf(message,sizeof(message),"Project save failed: %s",SDL_GetAtomicInt(&job->cancel)?"Cancelled.":job->error);ed_message(e,message);
    }
    bool proceed=job->success&&unchanged&&e->pending_action;
    if(current&&e->pending_action&&!proceed)e->confirm=true;
    dm_project_free(&job->project);free(job);
    if(proceed)ed_pending_execute(e);
}
