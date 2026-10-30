#include "internal.h"

static bool bend(Compile *c,Buffer *b,Channel *s,const M64Event *e) {
    double semitones=(s->bend/8192.0)*(s->range+s->cents/100.0);
    double multiplier=pow(2.0,semitones/12.0);
    if(multiplier<1.0/32768.0 || multiplier>=2.0) {
        if(!c->warned[50]){c->warned[50]=true;diagnose(c,M64_WARNING,50,e,"Pitch bend beyond the engine's multiplier range is clamped.");}
        if(multiplier<1.0/32768.0)multiplier=1.0/32768.0;else multiplier=65535.0/32768.0;
    }
    unsigned value=(unsigned)lround(multiplier*32768.0);if(value>65535)value=65535;
    if(value!=s->encoded_bend){put(b,0xde);put(b,value>>8);put(b,value);s->encoded_bend=value;}return true;
}
static bool control(Compile *c,Buffer *b,Channel *s,const M64Event *e) {
    unsigned type=e->status&0xf0;
    if(type==0xe0){s->bend=(int)(e->a|(e->b<<7))-8192;return bend(c,b,s,e);}
    if(type!=0xb0)return true;
    switch(e->a) {
    case 7:if(s->volume==e->b)return true;s->volume=e->b;break;
    case 11:if(s->expression==e->b)return true;s->expression=e->b;break;
    case 10:if(s->pan!=e->b){put(b,0xdd);put(b,(e->b*128+63)/127);s->pan=e->b;}return true;
    case 91:if(s->reverb!=e->b){put(b,0xd4);put(b,e->b);s->reverb=e->b;}return true;
    case 1:if(s->modulation!=e->b){put(b,0xd8);put(b,e->b/4);s->modulation=e->b;}return true;
    case 100:s->rpn_lsb=e->b;return true;
    case 101:s->rpn_msb=e->b;return true;
    case 6: case 38:
        if(!s->rpn_lsb&&!s->rpn_msb) {
            if(e->a==6)s->range=e->b;else s->cents=e->b;
            return bend(c,b,s,e);
        }
        if(!c->warned[51]){c->warned[51]=true;diagnose(c,M64_WARNING,51,e,"Only pitch-bend-range RPN is supported.");}
        return true;
    case 121:
        s->expression=127;s->bend=0;s->rpn_lsb=s->rpn_msb=127;s->encoded_bend=32768;s->modulation=0;
        put(b,0xd8);put(b,0);put(b,0xde);put(b,0x80);put(b,0);
        break;
    default:return true;
    }
    put(b,0xdf);put(b,(s->volume*s->expression+63)/127);return true;
}
static bool controller_stream(Compile *c,unsigned ch,Buffer *b) {
    Channel state={0};state.volume=100;state.expression=127;state.range=2;state.rpn_msb=state.rpn_lsb=127;state.pan=64;state.encoded_bend=32768;
    uint64_t now=0;
    for(uint64_t i=0;i<c->request->event_count;i++) {
        const M64Event *e=&c->request->events[i];unsigned type=e->status&0xf0;
        if(e->status>=0xf0||(int)(e->status&15)!=c->sources[ch]||(type!=0xb0&&type!=0xe0))continue;
        if(c->quality_grid&&(type==0xe0||e->a==1||e->a==7||e->a==10||e->a==11||e->a==91)) {
            uint64_t bucket=tick48(c,e->tick)/c->quality_grid;bool replaced=false;
            for(uint64_t next=i+1;next<c->request->event_count;next++) {
                const M64Event *later=&c->request->events[next];
                if(tick48(c,later->tick)/c->quality_grid>bucket)break;
                if(later->status==e->status&&(type==0xe0||later->a==e->a)){replaced=true;break;}
            }
            if(replaced)continue;
        }
        Buffer command={0};
        bool ok=control(c,&command,&state,e);
        if(command.size) {uint64_t t=tick48(c,e->tick);if(c->quality_grid)t=t/c->quality_grid*c->quality_grid;delay(b,0xfd,t-now);now=t;bytes(b,command.data,command.size);}
        if(command.failed)b->failed=true;
        free(command.data);if(!ok)return false;
    }
    put(b,0xf3); /* Keep layers alive after the last controller. */
    return !b->failed;
}
static size_t next_controller(const Buffer *b,size_t pos) {
    unsigned op=b->data[pos++];
    if(op==0xfd){if(b->data[pos++]&128)pos++;}
    else if(op==0xde)pos+=2;
    else if(op!=0xf3)pos++;
    return pos;
}
bool sequence(Compile *c) {
    Buffer *b=&c->result->files[M64_SEQUENCE];
    size_t channel_fix[16]={0},layer_fix[16][4]={{0}};
    unsigned mask=0;
    for(unsigned ch=0;ch<16;ch++)if(c->sources[ch]>=0)mask|=1u<<ch;
    put(b,0xd3);put(b,0x20); /* soften when the game mutes music */
    put(b,0xd5);put(b,0x32);
    put(b,0xd0);put(b,0); /* Explicit default note allocation policy. */
    put(b,0xdf);put(b,0); /* sequence transpose */
    put(b,0xdb);put(b,127);
    put(b,0xdd);put(b,(unsigned)fmax(1,round(120*c->tempo_scale)));
    put(b,0xd7);put(b,mask>>8);put(b,mask);
    for(unsigned ch=0;ch<16;ch++)if(mask&(1u<<ch)) {
        put(b,0x90+ch);channel_fix[ch]=b->size;put(b,0);put(b,0);
    }
    uint64_t now=0;
    unsigned last_tempo=(unsigned)fmax(1,round(120*c->tempo_scale));
    for(uint64_t i=0;i<c->request->event_count;i++) {
        const M64Event *e=&c->request->events[i];if(e->status!=0xff)continue;
        uint64_t t=tick48(c,e->tick);
        if(c->quality_grid) {
            t=t/c->quality_grid*c->quality_grid;
            bool replaced=false;
            for(uint64_t next=i+1;next<c->request->event_count;next++) {
                const M64Event *later=&c->request->events[next];
                if(tick48(c,later->tick)/c->quality_grid>t/c->quality_grid)break;
                if(later->status==0xff){replaced=true;break;}
            }
            if(replaced)continue;
        }
        unsigned value=(unsigned)fmax(1,round(60000000.0/e->a*c->tempo_scale));
        if(value==last_tempo)continue;
        delay(b,0xfd,t-now);now=t;last_tempo=value;
        put(b,0xdd);put(b,value);
    }
    uint64_t end=c->duration+(c->request->loop?0:48);
    delay(b,0xfd,end-now);
    if(c->request->loop){put(b,0xfb);put(b,0);put(b,0);}else put(b,0xff);

    Buffer streams[16]={0};size_t stream_start[16]={0};bool shared[16]={0},ok=true;
    for(unsigned ch=0;ch<16;ch++)if(c->sources[ch]>=0) {
        if(cancelled(c,2,ch*100/16)){ok=false;break;}
        address(b,channel_fix[ch],b->size);
        put(b,0xc4);put(b,0xc1);put(b,c->sources[ch]==9?127:0);
        put(b,0xdb);put(b,0);put(b,0xdf);put(b,100);put(b,0xe0);put(b,128);
        put(b,0xdd);put(b,64);put(b,0xdc);put(b,128);
        put(b,0xde);put(b,0x80);put(b,0);put(b,0xd4);put(b,0);
        put(b,0xd7);put(b,8);put(b,0xd8);put(b,0);put(b,0xd2);put(b,0);
        for(unsigned l=0;l<4;l++) {
            bool used=false;for(size_t n=0;n<c->count;n++)if(c->notes[n].output==ch&&c->notes[n].layer==l){used=true;break;}
            if(used){put(b,0x90+l);layer_fix[ch][l]=b->size;put(b,0);put(b,0);}
        }
        Buffer *stream=&streams[ch];
        if(!controller_stream(c,ch,stream)){c->result->status=M64_NOMEM;ok=false;break;}
        size_t saving=0,target=0;
        for(unsigned prev=0;prev<ch;prev++)if(streams[prev].size&&!shared[prev]) {
            Buffer *other=&streams[prev];size_t match=0;
            while(match<stream->size&&match<other->size&&stream->data[stream->size-1-match]==other->data[other->size-1-match])match++;
            size_t from=0,to=0;
            while(from<stream->size-match)from=next_controller(stream,from);
            while(to<other->size-match)to=next_controller(other,to);
            while(stream->size-from>saving+3&&other->size-to>saving+3) {
                size_t left=stream->size-from,right=other->size-to;
                if(left>right)from=next_controller(stream,from);
                else if(right>left)to=next_controller(other,to);
                else {
                    if(stream_start[prev]+to<=UINT16_MAX){saving=left-3;target=stream_start[prev]+to;}
                    break;
                }
            }
        }
        stream_start[ch]=b->size;
        if(saving) {
            bytes(b,stream->data,stream->size-saving-3);put(b,0xfb);
            size_t fix=b->size;put(b,0);put(b,0);address(b,fix,target);shared[ch]=true;
        } else bytes(b,stream->data,stream->size);
    }
    for(unsigned ch=0;ch<16;ch++)free(streams[ch].data);
    if(!ok)return false;
    if(!emit_layers(c,b,layer_fix))return false;
    if(b->failed){c->result->status=M64_NOMEM;return false;}
    if(b->address_failed){c->result->status=M64_LIMIT;return false;}
    return !cancelled(c,2,100);
}
