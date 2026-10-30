#include "internal.h"

/* Permit the inaudible final fixed-point rounding at nominal 4x, not a
   semitone of excess range. The US mixer itself caps at 3.99992. */
#define RATE_LIMIT 4.00002
typedef struct { uint64_t tick; double factor; } Change;
static double factor(const Channel *s) {
    double bend=pow(2.0,(s->bend/8192.0)*(s->range+s->cents/100.0)/12.0);
    bend=fmax(1.0/32768.0,fmin(65535.0/32768.0,bend));
    bend=floor(bend*32768.0+.5)/32768.0;
    /* D8 extent = (CC1/4)*8; US vibrato extent /4096, pitch table <=2.
       Bound the positive excursion rather than assuming a vibrato phase. */
    return bend*(1.0+(s->modulation/4)/512.0);
}
static void control(Channel *s,const M64Event *e) {
    if((e->status&240)==224){s->bend=(int)(e->a|(e->b<<7))-8192;return;}
    if((e->status&240)!=176)return;
    switch(e->a) {
    case 1:s->modulation=e->b;break;
    case 100:s->rpn_lsb=e->b;break;
    case 101:s->rpn_msb=e->b;break;
    case 6:if(!s->rpn_lsb&&!s->rpn_msb)s->range=e->b;break;
    case 38:if(!s->rpn_lsb&&!s->rpn_msb)s->cents=e->b;break;
    case 121:s->bend=0;s->modulation=0;s->rpn_lsb=s->rpn_msb=127;break;
    }
}
static size_t upper(const Change *v,size_t n,uint64_t tick) {
    size_t lo=0;while(lo<n){size_t m=lo+(n-lo)/2;if(v[m].tick<=tick)lo=m+1;else n=m;}return lo;
}
static double maximum(const double *tree,size_t base,size_t l,size_t r) {
    double value=0;
    for(l+=base,r+=base;l<r;l/=2,r/=2){if(l&1)value=fmax(value,tree[l++]);if(r&1)value=fmax(value,tree[--r]);}
    return value;
}
static bool note_factors(Compile *c) {
    for(unsigned ch=0;ch<16;ch++) {
        Change *changes=NULL;size_t count=0,capacity=0;
        if(!reserve((void **)&changes,&capacity,1,sizeof(*changes)))return false;
        changes[count++]=(Change){0,1};Channel s={0};s.range=2;s.rpn_lsb=s.rpn_msb=127;
        for(uint64_t i=0;i<c->request->event_count;i++) {
            if(!(i%4096)&&cancelled(c,1,90)){free(changes);return false;}
            const M64Event *e=&c->request->events[i];if(e->status>=240||(e->status&15)!=ch)continue;
            if((e->status&240)!=224&&(e->status&240)!=176)continue;
            if(c->quality_grid&&((e->status&240)==224||e->a==1)) {
                uint64_t bucket=tick48(c,e->tick)/c->quality_grid;bool replaced=false;
                for(uint64_t j=i+1;j<c->request->event_count;j++) {
                    const M64Event *later=&c->request->events[j];
                    if(tick48(c,later->tick)/c->quality_grid>bucket)break;
                    if(later->status==e->status&&((e->status&240)==224||later->a==e->a)){replaced=true;break;}
                }
                if(replaced)continue;
            }
            double before=factor(&s);control(&s,e);double after=factor(&s);if(before==after)continue;
            uint64_t tick=tick48(c,e->tick);
            if(c->quality_grid)tick=tick/c->quality_grid*c->quality_grid;
            if(tick==changes[count-1].tick)changes[count-1].factor=after;
            else {
                if(!reserve((void **)&changes,&capacity,count+1,sizeof(*changes))){free(changes);return false;}
                changes[count++]=(Change){tick,after};
            }
        }
        size_t base=1;while(base<count)base*=2;
        double *tree=calloc(base*2,sizeof(*tree));if(!tree){free(changes);return false;}
        for(size_t i=0;i<count;i++)tree[base+i]=changes[i].factor;
        for(size_t i=base-1;i;i--)tree[i]=fmax(tree[i*2],tree[i*2+1]);
        for(size_t i=0;i<c->count;i++)if(c->notes[i].channel==ch&&c->notes[i].end>c->notes[i].start) {
            Note *n=&c->notes[i];size_t l=upper(changes,count,n->start)-1,r=upper(changes,count,n->end-1);
            n->peak_factor=maximum(tree,base,l,r);
        }
        free(tree);free(changes);
    }
    return true;
}
bool select_registers(Compile *c) {
    if(!note_factors(c)){if(c->result->status==M64_OK)c->result->status=M64_NOMEM;return false;}
    double demand[127]={0},factors[127]={0};
    for(size_t i=0;i<c->count;i++) {
        const Note *n=&c->notes[i];if(n->channel==9||n->end<=n->start)continue;
        unsigned slot=(unsigned)c->slots[n->program];
        demand[slot]=fmax(demand[slot],pow(2.0,((double)n->key-60)/12.0)*n->peak_factor);
        factors[slot]=fmax(factors[slot],n->peak_factor);
    }
    for(unsigned slot=0;slot<c->program_count;slot++) {
        unsigned p=c->programs[slot];const M64Instrument *d=program_definition(c,p);
        if(demand[slot]*d->tuning<=RATE_LIMIT)continue;
        const StockRegister *original=NULL,*best=NULL;
        for(size_t j=0;j<stock_register_count;j++)if(!strcmp(d->sample,stock_registers[j].sample)){original=&stock_registers[j];break;}
        if(!original)continue;
        for(size_t j=0;j<stock_register_count;j++) {
            const StockRegister *candidate=&stock_registers[j];
            if(candidate->family!=original->family||candidate->tuning>=original->tuning)continue;
            double rate=demand[slot]*d->tuning*candidate->tuning/original->tuning;
            bool fits=rate<=RATE_LIMIT,best_fits=best&&demand[slot]*d->tuning*best->tuning/original->tuning<=RATE_LIMIT;
            if(!best||(fits&&!best_fits)||(fits&&best_fits&&candidate->tuning>best->tuning)||(!fits&&!best_fits&&candidate->tuning<best->tuning))best=candidate;
        }
        if(!best)continue;
        Register *r=&c->registers[slot];r->enabled=true;r->high=*d;r->high.sample=best->sample;
        r->high.tuning=d->tuning*best->tuning/original->tuning;
        r->normal_hi=(int)floor(60+12*log2(RATE_LIMIT/(d->tuning*factors[slot]))+1e-9)-((p&128)?12:0);
        diagnose(c,M64_INFO,58,NULL,"Higher-register stock sample %s selected above encoded key %d for %s; source note pitches are retained.",best->sample,r->normal_hi,d->sample);
    }
    typedef struct {unsigned count,lo,hi;double rate,demand;} Issue;
    Issue issues[16][128]={0};
    for(size_t i=0;i<c->count;i++) {
        if(!(i%4096)&&cancelled(c,1,95))return false;
        const Note *n=&c->notes[i];const M64Instrument *d;
        if(n->end<=n->start)continue;
        if(n->channel==9)d=drum_definition(c,n->key);
        else {
            d=program_definition(c,n->program);const Register *r=&c->registers[c->slots[n->program]];
            unsigned key=n->key-((n->program&128)?12:0);
            if(r->enabled&&(int)key>r->normal_hi)d=&r->high;
        }
        double rate=d->tuning*n->peak_factor*(n->channel==9?1:pow(2.0,((double)n->key-60)/12.0));
        Issue *issue=&issues[n->channel][n->channel==9?n->key:n->program&127];
        issue->demand=fmax(issue->demand,n->peak_factor*(n->channel==9?1:pow(2.0,((double)n->key-60)/12.0)));
        if(rate<=RATE_LIMIT)continue;
        if(!issue->count)issue->lo=issue->hi=n->key;
        issue->count++;if(n->key<issue->lo)issue->lo=n->key;if(n->key>issue->hi)issue->hi=n->key;issue->rate=fmax(issue->rate,rate);
    }
    for(unsigned ch=0;ch<16;ch++)for(unsigned p=0;p<128;p++)if(issues[ch][p].count) {
        Issue *s=&issues[ch][p];M64Event e={0,0,0x90|ch,s->lo,0};
        M64Result *result=c->result;
        if(!reserve((void **)&result->ranges,&result->range_capacity,result->range_count+1,sizeof(M64RangeIssue))){result->status=M64_NOMEM;return false;}
        M64RangeIssue *issue=&result->ranges[result->range_count++];
        *issue=(M64RangeIssue){.channel=ch,.program=p,.percussion=ch==9,.note_low=s->lo,.note_high=s->hi,.note_count=s->count,.demand=s->demand,.maximum_rate=s->rate};
        const M64Instrument *definition=ch==9?drum_definition(c,p):program_definition(c,p|(c->request->channel_programs?ch<<8:0));
        snprintf(issue->sample,sizeof(issue->sample),"%s",definition->sample);
        diagnose(c,M64_WARNING,59,&e,"Channel %u %s %u: %u note(s), MIDI keys %u-%u, need up to %.3fx sample playback. SM64 caps near 4x; pitches may flatten. No higher stock register can cover these notes; change the mapping or prepare a suitable sample.",ch+1,ch==9?"drum note":"GM program",p,s->count,s->lo,s->hi,s->rate);
    }
    return !cancelled(c,1,100);
}
