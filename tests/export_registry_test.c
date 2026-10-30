#include "export_plugins.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char artifact_text[1024];
static int32_t DME_CALL cancelled(void *user){(void)user;return 0;}
static void DME_CALL progress(void *user,uint32_t value){(void)user;(void)value;}
static void DME_CALL diagnostic(void *user,uint32_t severity,uint32_t flags,const char *message){(void)user;(void)severity;(void)flags;(void)message;}
static int32_t DME_CALL artifact(void *user,const char *name,const uint8_t *data,uint64_t size){
    (void)user;assert(!strcmp(name,"fixture.txt")&&size<sizeof(artifact_text));
    memcpy(artifact_text,data,(size_t)size);artifact_text[size]=0;return 1;
}
int main(void) {
    assert(SDL_Init(0));DmExportRegistry r;
    dm_export_registry_load(&r,"build/development/export-registry-fixtures");
    printf("registered=%zu\n%s\n",r.count,r.errors);assert(r.count==2);
    assert(strstr(r.errors,"ABI 3"));assert(strstr(r.errors,"Duplicate exporter ID"));
    assert(strstr(r.errors,"settings schema"));assert(strstr(r.errors,"entry point"));assert(strstr(r.errors,"allocate exporter"));
    for(size_t i=0;i<r.count;i++){DmeUi ui={0};assert(r.formats[i].api->describe(r.formats[i].instance,&ui)==DME_OK);char error[256];assert(dm_export_ui_valid(&ui,error,sizeof(error)));}
    for(size_t i=0;i<r.count;i++) {
        DmExportFormat *f=&r.formats[i];char message[256]={0};
        if(!strcmp(f->api->id,"org.duckmidi.fixture")) {
            void *job=f->api->create_job(f->instance,message,sizeof(message));assert(job);
            DmeValue v={DME_TEXT,0,"Changed after freezing"};assert(f->api->set(f->instance,"title",&v,message,sizeof(message))==DME_OK);
            DmeContext context={.project_name="Isolated fixture"};DmeHost host={NULL,cancelled,progress,diagnostic,artifact};
            assert(f->api->run(job,&context,&host,message,sizeof(message))==DME_OK);
            assert(strstr(artifact_text,"Fixture title")&&!strstr(artifact_text,"Changed after freezing"));
            f->api->destroy_job(job);
            void *other=f->api->create();assert(other);DmeUi ui={0};assert(f->api->describe(other,&ui)==DME_OK);
            assert(!strcmp(ui.fields[2].value.text,"Fixture title"));f->api->destroy(other);
        } else {
            DmeValue v={DME_TEXT,0,"MANIFEST"};assert(f->api->set(f->instance,"bank_name",&v,message,sizeof(message))==DME_OK);
            assert(!f->api->create_job(f->instance,message,sizeof(message))&&strstr(message,"bank name"));
        }
    }
    DmeField fields[2]={{.id="choice",.label="Choice",.type=DME_CHOICE,.value={DME_CHOICE,9,NULL}}};
    DmeChoice choices[]={{1,"one"},{2,"two"}};fields[0].choices=choices;fields[0].choice_count=2;
    DmeUi bad={fields,1};char error[256];assert(!dm_export_ui_valid(&bad,error,sizeof(error)));
    fields[0].value.integer=1;assert(dm_export_ui_valid(&bad,error,sizeof(error)));
    fields[1]=fields[0];bad.count=2;assert(!dm_export_ui_valid(&bad,error,sizeof(error)));
    dm_export_registry_free(&r);assert(!r.count&&!r.module_count);
    dm_export_registry_load(&r,"build/development/export-no-such-directory");assert(!r.count);dm_export_registry_free(&r);
    SDL_Quit();puts("PASS plugin discovery, independent formats, invalid ABI/entry/schema/duplicates/allocation, and unload");return 0;
}
