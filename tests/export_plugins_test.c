#include "editor_export_internal.h"
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#endif
static void wait_work(Editor *e) {
    uint64_t start=SDL_GetTicks();
    while(ed_export_busy(e)&&SDL_GetTicks()-start<15000){ed_export_poll(e);if(ed_export_review_open(e))ed_export_review_decide(e,1);SDL_Delay(1);}
    assert(!ed_export_busy(e));
}
void test_export_dropdown(Editor *e,int target);
void test_export_mapping_row(Editor *e,int field_index,int row);
void test_export_warning(Editor *e);
void test_m64_sequence_dropdown(Editor *e,int target);
static size_t find(Editor *e,const char *id) {
    DmExportPlugins *h=ed_export_host(e);
    for(size_t i=0;i<h->registry.count;i++)if(!strcmp(h->registry.formats[i].api->id,id))return i;
    assert(!"missing test exporter");return 0;
}
static DmeUi settings(Editor *e,size_t index) {
    DmeUi ui={0};uint64_t start=SDL_GetTicks();
    while(!ed_export_settings(e,index,&ui)&&SDL_GetTicks()-start<15000){ed_export_poll(e);SDL_Delay(1);}
    assert(ui.count);return ui;
}
static const DmeField *field(DmeUi ui,const char *id){for(uint32_t i=0;i<ui.count;i++)if(!strcmp(ui.fields[i].id,id))return &ui.fields[i];assert(!"field missing");return NULL;}
static void set_text(Editor *e,size_t i,const char *id,const char *value){
    if(!strcmp(id,"sequence_name"))assert(ed_export_set(e,i,"sequence",(DmeValue){DME_CHOICE,35,NULL}));
    assert(ed_export_set(e,i,id,(DmeValue){DME_TEXT,0,value}));
}
static void set_int(Editor *e,size_t i,const char *id,int kind,int value){assert(ed_export_set(e,i,id,(DmeValue){(uint32_t)kind,value,NULL}));}
static void capture(Editor *e,const char *name) {
    ui_frame();editor_frame(e,1000,1000);ui_render(e->renderer);
    SDL_Surface *surface=SDL_RenderReadPixels(e->renderer,NULL);assert(surface);
    assert(SDL_SaveBMP(surface,name));SDL_DestroySurface(surface);
}
static bool contains(DmBytes b,const char *s){size_t n=strlen(s);for(size_t i=0;i+n<=b.size;i++)if(!memcmp(b.data+i,s,n))return true;return false;}
int main(void) {
    assert(SDL_Init(SDL_INIT_VIDEO));SDL_Window *window=SDL_CreateWindow("Exporter plugins validation",1000,1000,SDL_WINDOW_HIDDEN);assert(window);
    SDL_Renderer *renderer=SDL_CreateRenderer(window,"software");assert(renderer&&ui_init(window,renderer,NULL));
    Editor *e=calloc(1,sizeof(*e));assert(e);editor_init(e,window,renderer,NULL,"",true);
    size_t m64=find(e,"org.duckmidi.sm64"),fixture=find(e,"org.duckmidi.fixture");assert(ed_export_count(e)==2);
    int pattern=dm_pattern_add(&e->project,"generated melodic fixture");assert(pattern>=0);
    assert(dm_note_add(&e->project.patterns[pattern],(DmNote){0,1,60,100,0}));
    e->project.channels[0].enabled=e->project.channels[1].enabled=true;e->project.channels[0].program=e->project.channels[1].program=0;
    assert(dm_clip_add(&e->project,(DmClip){.beat=0,.pattern=pattern,.channel=0}));assert(dm_clip_add(&e->project,(DmClip){.beat=0,.pattern=pattern,.channel=1}));
    int drums=dm_pattern_add(&e->project,"generated percussion fixture");assert(drums>=0);
    assert(dm_note_add(&e->project.patterns[drums],(DmNote){0,.25,36,110,0}));e->project.channels[9].enabled=true;
    assert(dm_clip_add(&e->project,(DmClip){.beat=0,.pattern=drums,.channel=9}));
    e->active=true;e->export_window=true;e->export_type=(int)m64+1;ed_changed(e);
    assert(SDL_CreateDirectory("agents/reports/export-plugins/worker"));
    DmeUi ui=settings(e,m64);const DmeField *mapped=field(ui,"mapping");assert(mapped->row_count==2);
    e->project.patterns[pattern].notes[0].key=127;ed_changed(e);settings(e,m64);
    const char *review_path="agents/reports/export-plugins/worker/review.txt";
    FILE *sentinel=fopen(review_path,"wb");assert(sentinel);fputs("keep",sentinel);fclose(sentinel);
    ed_export_start(e,m64,review_path);
    uint64_t review_start=SDL_GetTicks();
    while(!ed_export_review_open(e)&&ed_export_busy(e)&&SDL_GetTicks()-review_start<15000){ed_export_poll(e);SDL_Delay(1);}
    assert(ed_export_review_open(e));DmeReview review={0};DmExportPlugins *host=ed_export_host(e);
    assert(host->work.api->review(host->work.job,&review)==DME_OK&&review.count==2&&review.rows[0].recommended_count&&review.rows[0].supported);
    capture(e,"agents/reports/export-plugins/review-mappings.bmp");capture(e,"agents/reports/export-plugins/review-mappings.bmp");capture(e,"agents/reports/export-plugins/review-mappings.bmp");
    ed_export_review_decide(e,0);assert(!ed_export_busy(e));
    DmBytes preserved={0};char review_error[512];assert(dm_read(review_path,&preserved,review_error,sizeof(review_error))&&preserved.size==4&&!memcmp(preserved.data,"keep",4));dm_bytes_free(&preserved);
    assert(field(settings(e,m64),"mapping")->rows[0].value==0);
    ed_export_start(e,m64,review_path);review_start=SDL_GetTicks();
    while(!ed_export_review_open(e)&&ed_export_busy(e)&&SDL_GetTicks()-review_start<15000){ed_export_poll(e);SDL_Delay(1);}
    assert(ed_export_review_open(e));assert(host->work.api->review(host->work.job,&review)==DME_OK);int substitute=review.rows[0].value;
    ed_export_review_decide(e,2);wait_work(e);assert(strstr(e->status,"complete"));
    assert(field(settings(e,m64),"mapping")->rows[0].value==substitute&&substitute!=0);
    assert(ed_export_set(e,m64,"reset_mappings",(DmeValue){DME_ACTION,0,NULL}));
    e->project.patterns[pattern].notes[0].key=60;ed_changed(e);settings(e,m64);
    e->project.patterns[pattern].notes[0].port=1;ed_changed(e);settings(e,m64);
    assert(dm_write("agents/reports/export-plugins/worker/ports-preserved.zip","sentinel",8,review_error,sizeof(review_error)));
    ed_export_start(e,m64,"agents/reports/export-plugins/worker/ports-preserved.zip");wait_work(e);assert(strstr(e->status,"ports"));
    assert(dm_read("agents/reports/export-plugins/worker/ports-preserved.zip",&preserved,review_error,sizeof(review_error))&&preserved.size==8&&!memcmp(preserved.data,"sentinel",8));dm_bytes_free(&preserved);
    e->project.patterns[pattern].notes[0].port=0;ed_changed(e);settings(e,m64);
    assert(!strcmp(ed_export_name(e,m64),"M64"));
    assert(field(ui,"sequence")->value.integer==3);
    for(unsigned i=0;i<ui.count;i++)assert(strcmp(ui.fields[i].id,"voices")&&strcmp(ui.fields[i].id,"notice")&&strcmp(ui.fields[i].id,"sequence_name"));
    test_m64_sequence_dropdown(e,2);
    ed_export_start(e,m64,"agents/reports/export-plugins/worker/preset.zip");wait_work(e);assert(strstr(e->status,"complete"));
    DmBytes preset={0};char preset_error[512];assert(dm_read("agents/reports/export-plugins/worker/preset.zip",&preset,preset_error,sizeof(preset_error)));
    assert(contains(preset,"02_menu_title_screen.m64")&&!contains(preset,"README.txt"));dm_bytes_free(&preset);
    test_m64_sequence_dropdown(e,35);
    assert(!strcmp(field(settings(e,m64),"sequence_name")->value.text,"02_menu_title_screen"));
    set_text(e,m64,"sequence_name","03_custom_retained");
    set_int(e,m64,"sequence",DME_CHOICE,3);assert(!ed_export_set(e,m64,"sequence_name",(DmeValue){DME_TEXT,0,"hidden"}));
    set_int(e,m64,"sequence",DME_CHOICE,35);assert(!strcmp(field(settings(e,m64),"sequence_name")->value.text,"03_custom_retained"));
    assert(!ed_export_set(e,m64,"sequence",(DmeValue){DME_CHOICE,36,NULL}));
    mapped=field(settings(e,m64),"mapping");
    assert(!strcmp(mapped->rows[0].id,"melodic.0.0")&&!strcmp(mapped->rows[1].id,"melodic.1.0"));
    assert(!ed_export_set(e,m64,"voices",(DmeValue){DME_INT,64,NULL}));
    assert(!ed_export_set(e,m64,"melodic.7.12",(DmeValue){DME_CHOICE,40,NULL}));
    set_text(e,m64,"sequence_name","03_custom_worker");set_text(e,m64,"bank_name","26_custom_worker");
    set_int(e,m64,"melodic.1.0",DME_CHOICE,40);set_int(e,m64,"percussion",DME_BOOL,1);
    ui=settings(e,m64);mapped=field(ui,"mapping");assert(mapped->row_count==1&&!strcmp(mapped->rows[0].id,"drum.36"));
    set_int(e,m64,"drum.36",DME_CHOICE,38);set_int(e,m64,"percussion",DME_BOOL,0);
    DmBytes before={0},after={0};assert(dm_pack(&e->project,&before));
    assert(SDL_CreateDirectory("agents/reports/export-plugins/worker"));
    ed_export_start(e,m64,"agents/reports/export-plugins/worker/mapped.zip");wait_work(e);assert(strstr(e->status,"complete"));
    assert(!ed_export_warning_open(e));
    assert(dm_pack(&e->project,&after));assert(before.size==after.size&&!memcmp(before.data,after.data,before.size));dm_bytes_free(&before);dm_bytes_free(&after);
    DmBytes archive={0};char error[512];assert(dm_read("agents/reports/export-plugins/worker/mapped.zip",&archive,error,sizeof(error)));
    assert(contains(archive,"44_grand_piano")&&contains(archive,"02_violin")&&contains(archive,"12_snare"));dm_bytes_free(&archive);
    test_export_dropdown(e,(int)fixture+1);test_export_dropdown(e,(int)m64+1);
    DmeUi current=settings(e,m64);int mapping_index=0;while(strcmp(current.fields[mapping_index].id,"mapping"))mapping_index++;
    test_export_mapping_row(e,mapping_index,1);
    capture(e,"agents/reports/export-plugins/m64-options.bmp");
    set_int(e,m64,"percussion",DME_BOOL,1);settings(e,m64);capture(e,"agents/reports/export-plugins/m64-percussion.bmp");
    set_int(e,m64,"reset_mappings",DME_ACTION,0);set_int(e,m64,"percussion",DME_BOOL,0);
    mapped=field(settings(e,m64),"mapping");assert(mapped->rows[0].value==0&&mapped->rows[1].value==0);
    set_text(e,m64,"sequence_name","../invalid");ed_export_start(e,m64,"agents/reports/export-plugins/worker/invalid.zip");wait_work(e);assert(strstr(e->status,"Invalid request"));
    set_text(e,m64,"sequence_name","03_custom_worker");
    ed_export_start(e,m64,"AGENTS.md/invalid-parent.zip");wait_work(e);assert(strstr(e->status,"Cannot save"));
    ed_export_start(e,m64,"agents/reports/export-plugins/worker");wait_work(e);assert(strstr(e->status,"folder"));
#ifdef _WIN32
    const char *locked="agents/reports/export-plugins/worker/locked.zip";
    assert(dm_write(locked,"sentinel",8,error,sizeof(error)));assert(SetFileAttributesA(locked,FILE_ATTRIBUTE_READONLY));
    ed_export_start(e,m64,locked);wait_work(e);bool reported=strstr(e->status,"Cannot save")!=NULL;
    assert(SetFileAttributesA(locked,FILE_ATTRIBUTE_NORMAL));assert(reported&&e->export_window);
    assert(dm_read(locked,&archive,error,sizeof(error)));assert(archive.size==8&&!memcmp(archive.data,"sentinel",8));dm_bytes_free(&archive);
    capture(e,"agents/reports/export-plugins/export-error.bmp");
#endif
    assert(SDL_CreateDirectory("agents/reports/export-plugins/worker/\xc3\xa9 space"));
    ed_export_start(e,m64,"agents/reports/export-plugins/worker/\xc3\xa9 space/fixture.zip");wait_work(e);assert(strstr(e->status,"complete"));
    settings(e,fixture);set_text(e,fixture,"title","Other DLL");set_int(e,fixture,"amount",DME_INT,19);set_int(e,fixture,"source.0",DME_CHOICE,20);
    e->export_type=(int)fixture+1;capture(e,"agents/reports/export-plugins/fixture-options.bmp");
    ed_export_start(e,fixture,"agents/reports/export-plugins/worker/fixture.txt");wait_work(e);assert(strstr(e->status,"complete"));
    assert(!ed_export_warning_open(e));
    assert(dm_read("agents/reports/export-plugins/worker/fixture.txt",&archive,error,sizeof(error)));assert(contains(archive,"Other DLL")&&contains(archive,"amount=19 mapped=20"));dm_bytes_free(&archive);
    set_int(e,fixture,"mode",DME_CHOICE,7);
    ed_export_start(e,fixture,"agents/reports/export-plugins/worker/fixture-warning.txt");wait_work(e);
    assert(ed_export_warning_open(e)&&strstr(ed_export_host(e)->warning,"Independent fixture audible-change warning"));
    test_export_warning(e);
    for(int mode=1;mode<=5;mode++) {
        set_int(e,fixture,"mode",DME_CHOICE,mode);
        assert(dm_write("agents/reports/export-plugins/worker/preserved.txt","sentinel",8,error,sizeof(error)));
        ed_export_start(e,fixture,"agents/reports/export-plugins/worker/preserved.txt");wait_work(e);assert(!strstr(e->status,"complete"));
        assert(!ed_export_warning_open(e));
        assert(dm_read("agents/reports/export-plugins/worker/preserved.txt",&archive,error,sizeof(error)));assert(archive.size==8&&!memcmp(archive.data,"sentinel",8));dm_bytes_free(&archive);
    }
    set_int(e,fixture,"mode",DME_CHOICE,6);ed_export_start(e,fixture,"agents/reports/export-plugins/worker/canceled.txt");
    assert(!ed_export_set(e,fixture,"amount",(DmeValue){DME_INT,3,NULL}));
    ed_export_free(e);assert(!e->export_plugins);
    m64=find(e,"org.duckmidi.sm64");fixture=find(e,"org.duckmidi.fixture");
    const char *samples[]={"samples/imp_night.mid","samples/e2m1.mus","samples/D_46_D15.mid","samples/d_01_51.mus"};
    assert(SDL_CreateDirectory("agents/reports/m64export/worker"));
    for(unsigned i=0;i<4;i++) {
        DmBytes file={0};assert(dm_read(samples[i],&file,error,sizeof(error)));
        assert(dm_import(&e->project,file.data,file.size,0,0,error,sizeof(error)));dm_bytes_free(&file);
        ++e->project_session;ed_changed(e);settings(e,m64);char path[DM_PATH];
        set_text(e,m64,"sequence_name","03_custom_worker");set_text(e,m64,"bank_name","26_custom_worker");
        snprintf(path,sizeof(path),"agents/reports/m64export/worker/sample-%u.zip",i);ed_export_start(e,m64,path);wait_work(e);assert(strstr(e->status,"complete"));
        printf("%s: %s\n",samples[i],e->status);
        assert(ed_export_warning_open(e));
        if(i==0)assert(strstr(ed_export_host(e)->warning,"GM program 56")&&strstr(ed_export_host(e)->warning,"pitches may flatten"));
        test_export_warning(e);
    }
    char suggested[DM_PATH];snprintf(e->project.name,sizeof(e->project.name),"Project song");
    snprintf(e->export_folder,DM_PATH,"agents/reports/export-plugins/worker/");
    assert(ed_export_suggest(e,"zip",suggested,sizeof(suggested))&&strstr(suggested,"Project song.zip"));
    e->project.name[0]=0;snprintf(e->export_source,DM_PATH,"C:/source/original-song.mus");
    assert(ed_export_suggest(e,"zip",suggested,sizeof(suggested))&&strstr(suggested,"original-song.zip"));
    snprintf(e->path,DM_PATH,"C:/projects/Saved project.duck");assert(ed_export_suggest(e,"zip",suggested,sizeof(suggested))&&strstr(suggested,"Saved project.zip"));e->path[0]=0;
    snprintf(e->project.name,sizeof(e->project.name),"CON");assert(ed_export_suggest(e,"zip",suggested,sizeof(suggested))&&strstr(suggested,"_CON.zip"));
    e->project.name[0]=0;e->export_type=(int)m64+1;
    const SDL_DialogFileFilter *filter;const char *save_name;assert(ed_export_picker(e,&filter,&save_name));assert(!strcmp(filter->pattern,"zip"));
    SDL_Event saved;SDL_zero(saved);saved.type=ed_dialog_event;saved.user.code=DIALOG_EXPORT_PLUGIN;
    saved.user.data1=SDL_strdup("agents/reports/export-plugins/worker/remembered");e->dialog_busy=true;
    editor_event(e,&saved);wait_work(e);assert(strstr(e->status,"complete"));
    assert(!strcmp(e->export_folder,"agents/reports/export-plugins/worker/"));
    assert(SDL_GetPathInfo("agents/reports/export-plugins/worker/remembered.zip",NULL));
    snprintf(e->settings_path,DM_PATH,"agents/reports/export-plugins/worker/export-preferences.ini");
    assert(dm_preferences_save(e,ed_shortcut_schema(),ed_action_names));e->export_folder[0]=0;
    assert(dm_preferences_load(e,ed_shortcut_schema()));assert(!strcmp(e->export_folder,"agents/reports/export-plugins/worker/"));
    ed_export_free(e);dm_project_free(&e->project);free(e);ui_shutdown();SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    puts("PASS generic exporter UI, independent DLL format, per-channel/percussion mappings, snapshots, publication failures, cancellation/unload, source fidelity fixtures and naming/preferences");return 0;
}
