/* A complete second exporter, built only into isolated validation directories. */
#define DME_BUILD_PLUGIN
#ifdef FIXTURE_MISSING_ENTRY
#define duckmidi_export_plugin fixture_wrong_entry
#endif
#include "../include/duckmidi_export_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {int enabled,amount,mode,mapped;char title[80];DmeField fields[7];DmeMappingRow row;} State;
static const DmeChoice modes[]={{0,"Normal"},{1,"Unsafe name"},{2,"Duplicate names"},{3,"Explicit failure"},{4,"No artifacts"},{5,"Ignore host rejection"},{6,"Wait for cancellation"},{7,"Popup warning"}};
static const DmeChoice mapping[]={{10,"Choice ten"},{20,"Choice twenty"}};
static void DME_CALL reset(void *user){State *s=user;s->enabled=1;s->amount=7;s->mode=0;s->mapped=10;snprintf(s->title,80,"Fixture title");}
static void *DME_CALL create(void){
#ifdef FIXTURE_CREATE_FAIL
    return NULL;
#else
    State *s=calloc(1,sizeof(*s));if(s)reset(s);return s;
#endif
}
static void DME_CALL destroy(void *s){free(s);}
static int32_t DME_CALL inspect(void *s,const DmeContext *c,char *error,uint64_t cap){(void)s;(void)error;(void)cap;return c?DME_OK:DME_INVALID;}
static int32_t DME_CALL describe(void *user,DmeUi *ui){
    State *s=user;s->row=(DmeMappingRow){"source.0","Fixture source",s->mapped,10};
    s->fields[0]=(DmeField){.id="enabled",.label="Fixture enabled",.type=DME_BOOL,.value={DME_BOOL,s->enabled,NULL}};
    s->fields[1]=(DmeField){.id="amount",.label="Fixture amount",.type=DME_INT,.minimum=1,.maximum=20,.value={DME_INT,s->amount,NULL}};
    s->fields[2]=(DmeField){.id="title",.label="Fixture title",.type=DME_TEXT,.text_capacity=80,.value={DME_TEXT,0,s->title}};
    s->fields[3]=(DmeField){.id="mode",.label="Fixture mode",.type=DME_CHOICE,.choices=modes,.choice_count=8,.value={DME_CHOICE,s->mode,NULL}};
    s->fields[4]=(DmeField){.id="map",.label="Fixture mapping",.type=DME_MAPPING,.choices=mapping,.choice_count=2,.rows=&s->row,.row_count=1};
    s->fields[5]=(DmeField){.id="notice",.label="Fixture notice",.type=DME_NOTICE,.help="This exporter is provided entirely by another DLL."};
    s->fields[6]=(DmeField){.id="reset",.label="Reset fixture",.type=DME_ACTION};
#ifdef FIXTURE_BAD_UI
    s->fields[1].minimum=99;
#endif
    *ui=(DmeUi){s->fields,7};return DME_OK;
}
static int32_t DME_CALL set(void *user,const char *id,const DmeValue *v,char *error,uint64_t cap){
    State *s=user;
    if(!strcmp(id,"enabled")&&v->type==DME_BOOL&&(v->integer==0||v->integer==1)){s->enabled=v->integer;return DME_OK;}
    if(!strcmp(id,"amount")&&v->type==DME_INT&&v->integer>=1&&v->integer<=20){s->amount=v->integer;return DME_OK;}
    if(!strcmp(id,"mode")&&v->type==DME_CHOICE&&v->integer>=0&&v->integer<8){s->mode=v->integer;return DME_OK;}
    if(!strcmp(id,"source.0")&&v->type==DME_CHOICE&&(v->integer==10||v->integer==20)){s->mapped=v->integer;return DME_OK;}
    if(!strcmp(id,"title")&&v->type==DME_TEXT&&v->text&&strlen(v->text)<80){snprintf(s->title,80,"%s",v->text);return DME_OK;}
    if(!strcmp(id,"reset")&&v->type==DME_ACTION){reset(s);return DME_OK;}
    snprintf(error,(size_t)cap,"Invalid fixture setting.");return DME_INVALID;
}
static void *DME_CALL freeze(const void *user,char *error,uint64_t cap){(void)error;(void)cap;State *s=malloc(sizeof(*s));if(s)memcpy(s,user,sizeof(*s));return s;}
static int32_t DME_CALL run(void *user,const DmeContext *c,const DmeHost *h,char *error,uint64_t cap){
    State *s=user;
    if(s->mode==6)for(uint64_t i=0;i<100000000;i++)if(!(i%1024)&&h->cancelled(h->user))return DME_CANCELLED;
    if(h->cancelled(h->user))return DME_CANCELLED;
    h->progress(h->user,50);
    if(s->mode==3){h->diagnostic(h->user,DME_WARNING,DME_POPUP,"Unpublished fixture warning");snprintf(error,(size_t)cap,"Fixture explicit failure.");return DME_FAILED;}
    if(s->mode==7)h->diagnostic(h->user,DME_WARNING,DME_POPUP,"Independent fixture audible-change warning.");
    if(s->mode==4)return DME_OK;
    char out[512];int n=snprintf(out,sizeof(out),"fixture: %s\nenabled=%d amount=%d mapped=%d events=%llu project=%s\n",s->title,s->enabled,s->amount,s->mapped,(unsigned long long)c->event_count,c->project_name);
    const char *name=s->mode==1?"../escape.txt":"fixture.txt";
    int ok=h->artifact(h->user,name,(const uint8_t *)out,(uint64_t)n);
    if(s->mode==2)ok=h->artifact(h->user,name,(const uint8_t *)out,(uint64_t)n)&&ok;
    if(s->mode==5){h->artifact(h->user,"../ignored.txt",(const uint8_t *)out,(uint64_t)n);return DME_OK;}
    h->progress(h->user,100);return ok?DME_OK:DME_FAILED;
}
#ifdef FIXTURE_DUPLICATE
#define FIXTURE_ID "org.duckmidi.sm64"
#else
#define FIXTURE_ID "org.duckmidi.fixture"
#endif
static int32_t DME_CALL preflight(void *j,const DmeContext *c,const DmeHost *h,char *e,uint64_t n){(void)j;(void)c;(void)h;(void)e;(void)n;return DME_OK;}
static int32_t DME_CALL review(void *j,DmeReview *r){(void)j;*r=(DmeReview){0};return DME_OK;}
static int32_t DME_CALL review_set(void *j,const char *id,int32_t v,char *e,uint64_t n){(void)j;(void)id;(void)v;(void)e;(void)n;return DME_INVALID;}
static int32_t DME_CALL review_apply(void *s,void *j,char *e,uint64_t n){(void)s;(void)j;(void)e;(void)n;return DME_OK;}
static const DmeExporter exporter={FIXTURE_ID,"Fixture text","txt","Fixture document","Save fixture...","Independent plugin fixture",DME_SINGLE_FILE,create,destroy,inspect,describe,set,reset,freeze,run,destroy,preflight,review,review_set,review_apply};
static const DmeExporter *DME_CALL at(uint32_t i){return i?NULL:&exporter;}
#ifndef FIXTURE_ABI
#define FIXTURE_ABI DME_ABI
#endif
static const DmePlugin plugin={FIXTURE_ABI,sizeof(DmePlugin),"org.duckmidi.fixture-module","Fixture plugin",1,at};
DME_PUBLIC const DmePlugin *DME_CALL duckmidi_export_plugin(uint32_t abi){return abi==DME_ABI?&plugin:NULL;}
