#include "internal.h"

static void warning_once(Compile *c,unsigned code,const M64Event *e,const char *message) {
    if (!c->warned[code]) {c->warned[code]=true;diagnose(c,M64_WARNING,code,e,"%s",message);}
}
static void release_pedal(Compile *c,unsigned channel,uint64_t tick) {
    for(size_t i=0;i<c->count;i++) if(c->notes[i].channel==channel && c->notes[i].released && c->notes[i].end==UINT64_MAX) c->notes[i].end=tick;
}
static void end_key(Compile *c,unsigned channel,unsigned key,uint64_t tick,bool all) {
    Channel *ch=&c->channels[channel];
    int index=ch->active[key];
    while(index>=0) {
        Note *n=&c->notes[index];
        n->released=true;
        if(!ch->pedal) n->end=tick;
        ch->active[key]=n->next;
        if(!all)break;
        index=n->next;
    }
    if(ch->active[key]<0)ch->tail[key]=-1;
}
static bool same_definition(const M64Instrument *a,const M64Instrument *b) {
    return !strcmp(a->sample,b->sample)&&a->tuning==b->tuning&&a->release_rate==b->release_rate&&
           a->attack==b->attack&&a->decay==b->decay&&a->sustain==b->sustain;
}
static bool assign_instrument(Compile *c,Note *n) {
    if(n->channel==9) {
        if(c->drums[n->key]<0) {
            c->drums[n->key]=(int)c->drum_count;
            c->percussion[c->drum_count++]=n->key;
        }
    } else if(c->slots[n->program]<0) {
        for(unsigned i=0;i<c->program_count;i++) if((n->program&128)==(c->programs[i]&128) && same_definition(program_definition(c,n->program),program_definition(c,c->programs[i]))) {c->slots[n->program]=(int)i;return true;}
        if(c->program_count>=127) {c->result->status=M64_LIMIT;diagnose(c,M64_ERROR,20,NULL,"More than 127 distinct melodic instruments.");return false;}
        c->slots[n->program]=(int)c->program_count;
        c->programs[c->program_count++]=n->program;
    }
    return true;
}
bool timeline(Compile *c) {
    const M64Request *q=c->request;
    c->duration=tick48(c,q->duration);
    for(unsigned ch=0;ch<16;ch++) {
        Channel *s=&c->channels[ch];s->volume=100;s->expression=127;s->pan=64;s->range=2;s->rpn_lsb=s->rpn_msb=127;
        for(unsigned k=0;k<128;k++)s->active[k]=s->tail[k]=-1;
        c->sources[ch]=-1;
    }
    uint64_t previous=0;
    for(uint64_t i=0;i<q->event_count;i++) {
        const M64Event *e=&q->events[i];
        if((i && e->tick<previous)||e->tick>q->duration||e->status>255||
           (e->status<0xf0&&(e->status<0x80||e->a>127||e->b>127))||
           (e->status==0xff&&(!e->a||e->a>16777215))) {
            c->result->status=M64_INVALID;diagnose(c,M64_ERROR,3,e,"Invalid or unsorted timeline event.");return false;
        }
        previous=e->tick;
        if(!(i%1024)&&cancelled(c,1,(uint32_t)(i*100/(q->event_count?q->event_count:1))))return false;
        uint64_t t=tick48(c,e->tick);
        if(e->tick*48%q->ppq)warning_once(c,30,e,"Timing rounded to SM64's 48 ticks per quarter note.");
        if(e->status==0xff) {
            double bpm=60000000.0/e->a;
            bpm*=c->tempo_scale;
            if(bpm<1)warning_once(c,4,e,"Extremely slow tempos are clamped to the engine's minimum BPM.");
            if(fabs(bpm-round(bpm))>0.0001)warning_once(c,31,e,"Fractional tempos rounded to integer BPM.");
            continue;
        }
        if(e->status>=0xf0) {warning_once(c,32,e,"SysEx or unsupported source messages are omitted; GM channel 10 is treated as percussion.");continue;}
        unsigned channel=e->status&15,type=e->status&0xf0;Channel *s=&c->channels[channel];
        if(type==0xc0) {s->program=e->a;continue;}
        if(type==0x80||(type==0x90&&!e->b)) {
            if(s->active[e->a]<0)warning_once(c,33,e,"Unmatched note-off ignored.");
            end_key(c,channel,e->a,t,false);continue;
        }
        if(type==0x90) {
            if(!reserve((void **)&c->notes,&c->capacity,c->count+1,sizeof(Note))) {c->result->status=M64_NOMEM;return false;}
            Note n={0};n.start=t;n.end=UINT64_MAX;n.source=e->source_id;n.channel=channel;
            n.key=e->a;n.velocity=e->b;n.program=s->program|((channel!=9&&e->a>=117)?128u:0u);n.next=-1;
            if(channel!=9&&q->channel_programs)n.program|=channel<<8;
            int index=(int)c->count;c->notes[c->count++]=n;
            if(s->tail[e->a]>=0)c->notes[s->tail[e->a]].next=index;else s->active[e->a]=index;
            s->tail[e->a]=index;c->sources[channel]=(int)channel;
            if(channel==9&&s->program)warning_once(c,34,e,"GM standard drum mapping is used; alternate drum kits are approximated.");
        } else if(type==0xb0) {
            switch(e->a) {
            case 64: if(s->pedal&&e->b<64)release_pedal(c,channel,t);s->pedal=e->b>=64;break;
            case 120: case 123:
                for(unsigned key=0;key<128;key++)end_key(c,channel,key,t,true);
                if(e->a==120)release_pedal(c,channel,t);
                break;
            case 121: s->pedal=0;release_pedal(c,channel,t);break;
            case 0: case 32: if(e->b)warning_once(c,35,e,"Nonzero MIDI banks use the GM program approximation.");break;
            case 1: if(e->b)warning_once(c,41,e,"Modulation is approximated with SM64 vibrato depth.");break;
            case 7: case 10: case 11: case 91: case 100: case 101: case 6: case 38: break;
            default:
                if(!c->warned[128+e->a]) {
                    c->warned[128+e->a]=true;
                    diagnose(c,M64_WARNING,128+e->a,e,"MIDI controller %u is omitted.",e->a);
                }
                break;
            }
        } else if(type!=0xe0) warning_once(c,36,e,"Channel/polyphonic pressure is omitted.");
    }
    if(!c->count) {c->result->status=M64_INVALID;diagnose(c,M64_ERROR,5,NULL,"No notes in the selected arrangement.");return false;}
    uint64_t until[16][4]={{0}};int active[16][4];
    for(unsigned ch=0;ch<16;ch++)for(unsigned l=0;l<4;l++)active[ch][l]=-1;
    size_t write=0;
    for(size_t i=0;i<c->count;i++) {
        if(!(i%1024)&&cancelled(c,1,75))return false;
        Note n=c->notes[i];
        M64Event location={n.start*q->ppq/48,n.source,0x90|n.channel,n.key,n.velocity};
        if(n.end==UINT64_MAX) {
            n.end=c->duration;
            if(!c->warned[37]){c->warned[37]=true;diagnose(c,M64_WARNING,37,NULL,"Held or unterminated notes end at the arrangement boundary.");}
        }
        if(n.end<=n.start) {warning_once(c,38,NULL,"Notes shorter than one target tick were extended to one tick.");n.end=n.start+1;}
        if(n.end-n.start>32767)warning_once(c,21,&location,"Very long notes are retriggered in segments to fit the engine's note duration field.");
        if(n.end>c->duration)c->duration=n.end;
        unsigned voices=0;int output=-1,layer=-1;
        for(unsigned ch=0;ch<16;ch++)for(unsigned l=0;l<4;l++) {
            voices+=until[ch][l]>n.start;
            if(c->sources[ch]==(int)n.channel && until[ch][l]<=n.start && output<0){output=(int)ch;layer=(int)l;}
        }
        if(voices>=q->voice_limit&&!c->warned[22]) {
            c->warned[22]=true;
            diagnose(c,M64_WARNING,22,&location,"The arrangement exceeds the %u-voice warning threshold. Notes are retained; the game may steal voices during playback. Actual capacity depends on the target ROM.",q->voice_limit);
        }
        if(output<0)for(unsigned ch=0;ch<16;ch++)if(c->sources[ch]<0){c->sources[ch]=(int)n.channel;output=(int)ch;layer=0;warning_once(c,39,NULL,"Dense chords use additional SM64 channels; controller changes are mirrored.");break;}
        if(output<0) {
            int quietest=128;
            for(unsigned ch=0;ch<16;ch++)if(c->sources[ch]==(int)n.channel)for(unsigned l=0;l<4;l++) {
                int index=active[ch][l];
                if(index>=0&&(int)c->notes[index].velocity<quietest){quietest=(int)c->notes[index].velocity;output=(int)ch;layer=(int)l;}
            }
            warning_once(c,23,&location,"Dense chords exceed the engine's available layers. Quieter voices were shortened or omitted to retain the stronger notes.");
            if(output<0)continue;
            Note *old=&c->notes[active[output][layer]];
            if(old->start==n.start&&old->velocity>=n.velocity)continue;
            old->end=n.start;
        }
        n.output=(uint32_t)output;n.layer=(uint32_t)layer;until[output][layer]=n.end;active[output][layer]=(int)write;
        if(!assign_instrument(c,&n))return false;
        c->notes[write++]=n;
    }
    c->count=write;
    diagnose(c,M64_INFO,40,NULL,"GM timbres use SM64 sample approximations. Release tails and sound effects share the game's voice budget.");
    return !cancelled(c,1,100);
}
