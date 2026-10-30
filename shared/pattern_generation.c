#include "pattern_generation.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {DmNote note;int channel;} Flat;
typedef struct {size_t first,count;double beat;uint64_t hash;} Group;
typedef struct {uint64_t key;size_t count,last;int id;} Entry;
typedef struct {int source,event,output,next;} AutoGroup;
static bool cancelled(const DmGenerationHooks *h){return h&&h->cancelled&&h->cancelled(h->user);}
static uint64_t mix(uint64_t x){x^=x>>30;x*=UINT64_C(0xbf58476d1ce4e5b9);x^=x>>27;x*=UINT64_C(0x94d049bb133111eb);return x^(x>>31);}
static uint64_t real_hash(double x){return mix((uint64_t)llround(x*1e9));}
static int flat_compare(const void *a,const void *b) {
    const Flat *x=a,*y=b;if(x->channel!=y->channel)return x->channel-y->channel;
    if(x->note.beat!=y->note.beat)return x->note.beat<y->note.beat?-1:1;
    if(x->note.key!=y->note.key)return (int)x->note.key-y->note.key;
    if(x->note.port!=y->note.port)return (int)x->note.port-y->note.port;
    if(x->note.length!=y->note.length)return x->note.length<y->note.length?-1:1;
    return (int)x->note.velocity-y->note.velocity;
}
static size_t table_size(size_t n){size_t size=16;while(size<n*2)size*=2;return size;}
static Entry *lookup(Entry *table,size_t size,uint64_t key) {
    size_t at=(size_t)mix(key)&(size-1);while(table[at].count&&table[at].key!=key)at=(at+1)&(size-1);return &table[at];
}
static uint64_t window_hash(const Group *g,const uint64_t *prefix,const uint64_t *powers,size_t start,size_t length) {
    return (prefix[start+length-1]-prefix[start]*powers[length-1])*UINT64_C(1000000007)+g[start+length-1].hash;
}
static int control_group(DmAutomation a) {
    if(a.type==0xe0)return 128;
    if(a.type==0xc0)return 129;
    if(a.type==0xd0)return 130;
    if(a.type==0xa0)return 131;
    if(a.a==6||a.a==38||(a.a>=96&&a.a<=101))return 128;
    return a.a==65?5:a.a;
}
static bool separate_controls(const DmProject *source,DmProject *next,const bool *placed,const DmGenerationHooks *hooks,DmGenerationStats *stats) {
    bool modular=false;
    for(size_t i=0;i<source->clip_count;i++)modular|=source->patterns[source->clips[i].pattern].kind||source->clips[i].length>0;
    for(size_t i=0;i<source->automation_count;i++){DmAutomation a=source->automation[i];modular|=a.pattern>=0&&a.curve&&a.type==0xb0&&(a.a==5||a.a==64||a.a==66);}
    size_t size=table_size(source->automation_count+1),count=0;Entry *table=calloc(size,sizeof(*table));
    AutoGroup *groups=calloc(source->automation_count+1,sizeof(*groups));
    int *heads=malloc((source->pattern_count+1)*sizeof(int)),*tails=malloc((source->pattern_count+1)*sizeof(int));
    if(!table||!groups||!heads||!tails){free(table);free(groups);free(heads);free(tails);return false;}
    for(size_t i=0;i<=source->pattern_count;i++)heads[i]=tails[i]=-1;
    bool ok=true;int lanes[16][132]={{0}};
    for(size_t i=0;i<source->automation_count&&ok;i++) {
        if(!(i%1024)&&cancelled(hooks)){ok=false;break;}
        DmAutomation a=source->automation[i];if(a.pattern<0||!placed[a.pattern]||source->patterns[a.pattern].kind)continue;
        int event=control_group(a);uint64_t key=((uint64_t)(unsigned)a.pattern<<15)|((uint64_t)a.port<<8)|(unsigned)event;
        Entry *entry=lookup(table,size,key);
        if(!entry->count) {
            char name[64];snprintf(name,sizeof(name),"Controls %zu - %s",count+1,event==128?"Pitch bend":event==129?"Program":event==130?"Pressure":event==131?"Poly pressure":"Controller");
            int id=dm_pattern_add(next,name);if(id<0){ok=false;break;}
            DmPattern *p=&next->patterns[id];p->kind=1;p->control_scope=modular?1:2;p->event=event==128?-2:event==129?-1:event==130?-4:event==131?-5:event;
            p->length=source->patterns[a.pattern].length;p->port=a.port;p->reference=source->patterns[a.pattern].count?source->patterns[a.pattern].notes[0].key:source->patterns[a.pattern].reference;
            groups[count]=(AutoGroup){a.pattern,event,id,-1};
            if(tails[a.pattern]>=0)groups[tails[a.pattern]].next=(int)count;else heads[a.pattern]=(int)count;
            tails[a.pattern]=(int)count;entry->key=key;entry->count=1;entry->id=(int)count++;stats->automation_patterns++;
        }
        int id=groups[entry->id].output;a.pattern=id;next->patterns[id].length=fmax(next->patterns[id].length,a.beat);
        if(!dm_automation_add(next,a))ok=false;
    }
    next->clip_count=0;
    for(size_t i=0;i<source->clip_count&&ok;i++) {
        DmClip c=source->clips[i];if(source->patterns[c.pattern].kind){ok=dm_clip_add(next,c);continue;}
        int original=c.pattern;
        for(int index=heads[original];index>=0&&ok;index=groups[index].next) {
            AutoGroup g=groups[index];
            if(!lanes[c.channel][g.event]) {
                int lane=next->lane_count[c.channel];bool created=lane<DM_LANES;if(created)lane++;if(!lane)lane=1;
                next->lane_count[c.channel]=lane;lanes[c.channel][g.event]=lane;
                if(created)snprintf(next->lane_names[c.channel][lane-1],64,"Generated controls");
            }
            c.pattern=g.output;c.lane=lanes[c.channel][g.event];c.length=dm_clip_length(source,source->clips[i]);c.selected=0;
            ok=dm_clip_add(next,c);
        }
    }
    free(table);free(groups);free(heads);free(tails);return ok;
}
static bool matches(const DmPattern *p,const Flat *flat,size_t first,size_t end,double origin) {
    if(p->count!=end-first)return false;
    for(size_t i=first;i<end;i++) {
        DmNote a=p->notes[i-first],b=flat[i].note;
        if(a.key!=b.key||a.port!=b.port||a.velocity!=b.velocity||fabs(a.length-b.length)>1e-10||fabs(a.beat-(b.beat-origin))>1e-10)return false;
    }
    return true;
}
static bool channel_patterns(DmProject *next,const Flat *flat,size_t count,int channel,Entry *patterns,size_t pattern_slots,const DmGenerationHooks *hooks,DmGenerationStats *stats) {
    Group *groups=calloc(count+1,sizeof(*groups));uint64_t *prefix=calloc(count+1,sizeof(uint64_t)),*powers=calloc(count+1,sizeof(uint64_t));
    size_t *best=calloc(count+1,sizeof(size_t));double *scores=calloc(count+1,sizeof(double));
    size_t slots=table_size(count+1);Entry *table=calloc(slots,sizeof(*table));
    if(!groups||!prefix||!powers||!best||!scores||!table){free(groups);free(prefix);free(powers);free(best);free(scores);free(table);return false;}
    size_t n=0;bool ok=true;
    for(size_t i=0;i<count;) {
        size_t end=i+1;while(end<count&&fabs(flat[end].note.beat-flat[i].note.beat)<1e-10)end++;
        uint64_t hash=mix(end-i);
        for(size_t j=i;j<end;j++)hash=mix(hash^flat[j].note.key^((uint64_t)flat[j].note.velocity<<8)^((uint64_t)flat[j].note.port<<16)^real_hash(flat[j].note.length));
        groups[n++]=(Group){i,end-i,flat[i].note.beat,hash};i=end;
    }
    powers[0]=1;
    for(size_t i=0;i<n;i++){powers[i+1]=powers[i]*UINT64_C(1000000007);prefix[i+1]=prefix[i]*UINT64_C(1000000007)+mix(groups[i].hash^(i+1<n?real_hash(groups[i+1].beat-groups[i].beat):0));}
    for(size_t length=1;length<=n&&length<=4096;length=length<64?length+1:length*2) {
        if(cancelled(hooks)){ok=false;break;}
        memset(table,0,slots*sizeof(*table));
        for(size_t i=0;i+length<=n;i++) {
            uint64_t hash=window_hash(groups,prefix,powers,i,length);Entry *e=lookup(table,slots,hash);
            if(!e->count){e->key=hash;e->count=1;e->last=i;}
            else if(i>=e->last+length){e->count++;e->last=i;}
        }
        for(size_t i=0;i+length<=n;i++) {
            Entry *e=lookup(table,slots,window_hash(groups,prefix,powers,i,length));if(e->count<2)continue;
            double score=(double)(length-1)*(e->count-1)+(length==1?.5:0);
            if(score>scores[i]||(score==scores[i]&&length>best[i])){scores[i]=score;best[i]=length;}
        }
    }
    for(size_t i=0;i<n&&ok;) {
        if(cancelled(hooks)){ok=false;break;}
        size_t length=best[i];if(!length){length=1;while(length<16&&i+length<n&&!best[i+length])length++;}
        size_t first=groups[i].first,end=groups[i+length-1].first+groups[i+length-1].count;
        double origin=groups[i].beat;uint64_t hash=window_hash(groups,prefix,powers,i,length);
        size_t at=(size_t)mix(hash)&(pattern_slots-1);int id=-1;
        while(patterns[at].count) {
            if(patterns[at].key==hash&&matches(&next->patterns[patterns[at].id],flat,first,end,origin)){id=patterns[at].id;break;}
            at=(at+1)&(pattern_slots-1);
        }
        if(id<0) {
            char name[64];snprintf(name,sizeof(name),"Generated notes %zu",stats->note_patterns+1);id=dm_pattern_add(next,name);if(id<0){ok=false;break;}
            DmPattern *p=&next->patterns[id];p->length=1.0/DM_PPQ;
            for(size_t j=first;j<end;j++){DmNote note=flat[j].note;note.beat-=origin;note.selected=0;if(!dm_note_add(p,note)){ok=false;break;}}
            patterns[at]=(Entry){hash,1,0,id};stats->note_patterns++;
        } else stats->linked_placements++;
        if(ok)ok=dm_clip_add(next,(DmClip){.beat=origin,.pattern=id,.channel=channel,.length=next->patterns[id].length});
        stats->notes+=end-first;i+=length;
    }
    free(groups);free(prefix);free(powers);free(best);free(scores);free(table);return ok;
}
bool dm_generate_patterns(const DmProject *source,DmProject *output,const DmGenerationHooks *hooks,DmGenerationStats *stats,char *error,size_t cap) {
    DmProject next;dm_project_init(&next);*stats=(DmGenerationStats){0};Flat *flat=NULL;size_t count=0,capacity=0;
    bool *placed=calloc(source->pattern_count+1,sizeof(bool));Entry *table=NULL;bool ok=placed&&dm_project_copy(&next,source);
    double ends[16]={0};
    for(size_t i=0;i<source->clip_count&&ok;i++) {
        if(cancelled(hooks)){ok=false;break;}
        DmClip clip=source->clips[i];const DmPattern *p=&source->patterns[clip.pattern];if(p->kind)continue;
        placed[clip.pattern]=true;double length=dm_clip_length(source,clip);ends[clip.channel]=fmax(ends[clip.channel],clip.beat+length);
        for(size_t j=0;j<p->count;j++) {
            DmNote note=p->notes[j];if(note.beat>=length)continue;
            note.length=fmin(note.length,length-note.beat);note.beat+=clip.beat;
            if(count>=2000000||!dm_reserve((void **)&flat,&capacity,count+1,sizeof(*flat))){ok=false;break;}
            flat[count++]=(Flat){note,clip.channel};
        }
    }
    if(ok)ok=separate_controls(source,&next,placed,hooks,stats);
    if(ok&&count)qsort(flat,count,sizeof(*flat),flat_compare);
    size_t slots=table_size(count+1);if(ok){table=calloc(slots,sizeof(*table));ok=table!=NULL;}
    size_t first=0;
    for(int channel=0;channel<16&&ok;channel++) {
        if(hooks&&hooks->progress)hooks->progress(hooks->user,(unsigned)(channel*100/16));
        size_t end=first;while(end<count&&flat[end].channel==channel)end++;
        size_t before=next.clip_count;
        if(end>first)ok=channel_patterns(&next,flat+first,end-first,channel,table,slots,hooks,stats);
        if(ends[channel]>0&&next.clip_count==before&&ok) {
            int id=dm_pattern_add(&next,"Generated silence");ok=id>=0;
            if(ok){next.patterns[id].length=ends[channel];ok=dm_clip_add(&next,(DmClip){.pattern=id,.channel=channel});stats->note_patterns++;}
        }
        if(next.clip_count>before) {
            DmClip *last=&next.clips[next.clip_count-1];last->length=fmax(dm_clip_length(&next,*last),ends[channel]-last->beat);
        }
        first=end;
    }
    if(ok) {
        for(size_t i=source->pattern_count;i>0;i--)if(placed[i-1]&&!source->patterns[i-1].kind)dm_pattern_remove(&next,(int)i-1);
        memcpy(next.channels,source->channels,sizeof(next.channels));stats->placements=next.clip_count;
        dm_project_free(output);*output=next;dm_project_init(&next);
    } else snprintf(error,cap,"%s",cancelled(hooks)?"Pattern generation cancelled.":"Cannot generate patterns: memory, note or pattern limit exceeded.");
    free(placed);free(flat);free(table);dm_project_free(&next);return ok;
}
