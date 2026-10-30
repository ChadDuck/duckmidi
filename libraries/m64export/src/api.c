#include "internal.h"
#include <limits.h>
bool reserve(void **p, size_t *cap, size_t count, size_t width) {
    if (count <= *cap) return true;
    if (!width || count > SIZE_MAX / width) return false;
    size_t n = *cap ? *cap : 32;
    while (n < count) { if (n > SIZE_MAX / 2) { n = count; break; } n *= 2; }
    if (n > SIZE_MAX / width) n = count;
    void *q = realloc(*p, n * width);
    if (!q) return false;
    *p = q; *cap = n; return true;
}
void bytes(Buffer *b, const void *p, size_t n) {
    if (b->failed) return;
    if (n > SIZE_MAX - b->size || !reserve((void **)&b->data, &b->capacity, b->size+n, 1)) {
        b->failed = true; return;
    }
    if (n) memcpy(b->data+b->size, p, n);
    b->size += n;
}
void put(Buffer *b, unsigned v) { uint8_t x = (uint8_t)v; bytes(b, &x, 1); }
void format(Buffer *b, const char *fmt, ...) {
    va_list a, copy; va_start(a, fmt); va_copy(copy, a);
    int n = vsnprintf(NULL, 0, fmt, copy); va_end(copy);
    if (n < 0 || (size_t)n >= SIZE_MAX-b->size ||
        !reserve((void **)&b->data, &b->capacity, b->size+(size_t)n+1, 1)) b->failed = true;
    if (!b->failed) { vsnprintf((char *)b->data+b->size, (size_t)n+1, fmt, a); b->size += (size_t)n; }
    va_end(a);
}
void variable(Buffer *b, uint64_t v) {
    if (v > 32767) { b->failed = true; return; }
    if (v > 127) put(b, 0x80 | (unsigned)(v >> 8));
    put(b, (unsigned)v);
}
void delay(Buffer *b, unsigned command, uint64_t ticks) {
    while (ticks) { uint64_t n = ticks > 32767 ? 32767 : ticks; put(b,command); variable(b,n); ticks -= n; }
}
void address(Buffer *b, size_t offset, size_t target) {
    /* US readers assign the signed read to u16 before adding seqData. */
    if (target > UINT16_MAX || offset+1 >= b->size) { b->address_failed = true; return; }
    b->data[offset] = (uint8_t)(target >> 8); b->data[offset+1] = (uint8_t)target;
}
void diagnose(Compile *c, uint32_t severity, uint32_t code, const M64Event *e, const char *fmt, ...) {
    M64Result *r = c->result;
    if (r->count >= 1024) return;
    if (!reserve((void **)&r->diagnostics,&r->capacity,r->count+1,sizeof(Diagnostic))) { r->status=M64_NOMEM; return; }
    Diagnostic *d = &r->diagnostics[r->count++]; memset(d,0,sizeof(*d));
    d->view.severity=severity; d->view.code=code; d->view.source_id=e?e->source_id:0;
    d->view.tick=e?e->tick:0; d->view.channel=e && e->status < 0xf0 ? (int32_t)(e->status&15) : -1;
    va_list a; va_start(a,fmt); vsnprintf(d->text,sizeof(d->text),fmt,a); va_end(a);
}
bool cancelled(Compile *c, uint32_t stage, uint32_t percent) {
    if (c->hooks && c->hooks->should_cancel && c->hooks->should_cancel(c->hooks->user)) {
        c->result->status=M64_CANCELLED; return true;
    }
    if (c->hooks && c->hooks->progress) c->hooks->progress(c->hooks->user,stage,percent);
    return c->result->status != M64_OK;
}
uint64_t tick48(const Compile *c, uint64_t tick) {
    return (uint64_t)floor((double)tick*48*c->tempo_scale/c->request->ppq+0.5);
}
static bool lossless_sequence(Compile *c) {
    /* Try different phrase boundaries before changing any musical data. The
       final pass inlines phrases: long scripts can extend beyond 64 KiB when
       all of their actual branch targets remain representable. */
    const unsigned chunks[]={4,8,16,32,2,1,3,6,12,24,48,64,0};
    Buffer best={0};
    for(size_t i=0;i<sizeof(chunks)/sizeof(chunks[0]);i++) {
        c->phrase_notes=chunks[i];
        bool ok=sequence(c);
        if(c->result->status!=M64_OK&&c->result->status!=M64_LIMIT){free(best.data);return false;}
        Buffer *current=&c->result->files[M64_SEQUENCE];
        if(ok&&(!best.data||current->size<best.size)) {
            Buffer swap=best;best=*current;*current=swap;
        }
        free(current->data);*current=(Buffer){0};
        c->result->status=M64_OK;
        if(best.data&&best.size<=0x3f00)break;
    }
    c->result->files[M64_SEQUENCE]=best;
    if(!best.data)c->result->status=M64_LIMIT;
    return best.data!=NULL;
}
uint64_t sequence_load_bytes(const Compile *c) {
    /* assemble_sound pads the table length to 16 bytes. The US loader adds
       another 16, and the right temporary-pool side reserves 16 more. */
    return (((uint64_t)c->result->files[0].size+15)&~15ULL)+16;
}
static void sequence_memory_warning(Compile *c) {
    uint64_t temporary=sequence_load_bytes(c)+16;
    if(temporary>0x4400)
        diagnose(c,M64_WARNING,57,NULL,"This sequence needs at least %llu bytes of temporary sequence memory. Stock US 17408-byte pools are too small and can overwrite bank data; enlarge the target ROM's pools before use.",(unsigned long long)temporary);
    else if(temporary>0x3f00)
        diagnose(c,M64_WARNING,57,NULL,"This sequence needs at least %llu bytes of temporary sequence memory. Verify the target audio preset; some stock US pools are only 16128 bytes.",(unsigned long long)temporary);
}
static bool fit_sequence(Compile *c) {
    size_t diagnostics=c->result->count;
    bool baseline_warned[256];memcpy(baseline_warned,c->warned,sizeof(baseline_warned));
    if(lossless_sequence(c)) {
        sequence_memory_warning(c);
        return true;
    }
    if(c->result->status!=M64_OK&&c->result->status!=M64_LIMIT)return false;
    Note *original=malloc(c->count*sizeof(*original));
    if(!original){c->result->status=M64_NOMEM;return false;}
    memcpy(original,c->notes,c->count*sizeof(*original));
    uint64_t original_duration=c->duration;
    const uint32_t grids[]={2,3,4,6,8,12,24,48,96,192,384,768,1536,3072,6144,12288,24576,49152};
    bool ok=false;
    for(size_t pass=0;pass<sizeof(grids)/sizeof(grids[0]);pass++) {
        if(cancelled(c,2,0)&&c->result->status!=M64_LIMIT)break;
        free(c->result->files[0].data);c->result->files[0]=(Buffer){0};
        c->result->status=M64_OK;c->result->count=diagnostics;c->quality_grid=grids[pass];c->duration=original_duration;
        memcpy(c->warned,baseline_warned,sizeof(baseline_warned));
        int previous[16][4];for(unsigned ch=0;ch<16;ch++)for(unsigned l=0;l<4;l++)previous[ch][l]=-1;
        unsigned quantum=pass<2?4:pass<5?8:16;
        for(size_t i=0;i<c->count;i++) {
            Note n=original[i];uint64_t grid=c->quality_grid;
            if(n.end<=n.start){c->notes[i]=n;continue;}
            n.start=(n.start+grid/2)/grid*grid;n.end=(n.end+grid/2)/grid*grid;
            if(n.end<=n.start)n.end=n.start+1;
            n.velocity=(n.velocity+quantum/2)/quantum*quantum;if(n.velocity>127)n.velocity=127;if(!n.velocity)n.velocity=1;
            int prior=previous[n.output][n.layer];
            if(prior>=0&&c->notes[prior].end>n.start) {
                Note *old=&c->notes[prior];
                if(old->start==n.start&&old->velocity>n.velocity){if(old->end<n.end)old->end=n.end;n.end=n.start;}
                else old->end=n.start;
            }
            c->notes[i]=n;
            if(n.end>n.start)previous[n.output][n.layer]=(int)i;
            if(n.end>c->duration)c->duration=n.end;
        }
        if(lossless_sequence(c)){ok=true;break;}
        if(c->result->status!=M64_OK&&c->result->status!=M64_LIMIT)break;
    }
    free(original);
    if(ok){diagnose(c,M64_WARNING,54,NULL,"Lossless encoding could not represent all script addresses. Last-resort conversion reduced timing/velocity/controller detail and may merge notes (grid %u ticks).",c->quality_grid);sequence_memory_warning(c);}
    else if(c->result->status==M64_OK||c->result->status==M64_LIMIT){c->result->status=M64_LIMIT;diagnose(c,M64_ERROR,55,NULL,"Script addresses cannot be represented even after last-resort reduction.");}
    return ok;
}
const M64Instrument *program_definition(const Compile *c, uint32_t p) {
    if(c->request->channel_programs)return &c->request->channel_programs[(p>>8)*128+(p&127)];
    return &(c->request->programs ? c->request->programs : gm_programs)[p & 127];
}
const M64Instrument *drum_definition(const Compile *c, uint32_t p) {
    return &(c->request->drums ? c->request->drums : gm_drums)[p];
}
static bool name_valid(const char *s) {
    if (!s || !*s || strlen(s)>96) return false;
    for (;*s;s++) if (!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-')) return false;
    return true;
}
static bool instrument_valid(const M64Instrument *i) {
    if (!i->sample || !*i->sample || strlen(i->sample)>96 || !isfinite(i->tuning) || i->tuning<=0 || i->tuning>128 ||
        i->release_rate>255 || !i->attack || i->attack>65532 || !i->decay || i->decay>65532 || i->sustain>32767) return false;
    for (const char *s=i->sample;*s;s++) if (!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||strchr("_#-",*s))) return false;
    return true;
}
static int32_t compile_common(const M64Request *q,const M64Hooks *h,M64Result **out,bool analysis) {
    if (!out) return M64_INVALID;
    *out=NULL;
    M64Result *r=calloc(1,sizeof(*r)); if (!r) return M64_NOMEM;
    *out=r; Compile c={0}; c.request=q;c.hooks=h;c.result=r;c.tempo_scale=1;
    if (!q || q->target!=M64_TARGET_SM64_US || !q->ppq || q->ppq>1000000 || !q->voice_limit || q->voice_limit>64 || q->loop>1 ||
        q->event_count>2000000 || (q->event_count&&!q->events) || q->duration>UINT32_MAX || !name_valid(q->sequence_name)||!name_valid(q->bank_name)) {
        r->status=M64_INVALID; diagnose(&c,M64_ERROR,1,NULL,"Invalid request, names, limits, or target (SM64 US required)."); return r->status;
    }
    for(unsigned i=0;i<4096;i++)c.slots[i]=-1;
    for (unsigned i=0;i<128;i++) {
        c.drums[i]=-1;
        if (!instrument_valid(program_definition(&c,i))||!instrument_valid(drum_definition(&c,i))) {
            r->status=M64_INVALID; diagnose(&c,M64_ERROR,2,NULL,"Invalid instrument definition at index %u.",i); return r->status;
        }
    }
    if(q->channel_programs)for(unsigned i=0;i<16*128;i++)if(!instrument_valid(&q->channel_programs[i])) {
        r->status=M64_INVALID;diagnose(&c,M64_ERROR,2,NULL,"Invalid channel instrument definition at index %u.",i);return r->status;
    }
    for(uint64_t i=0;i<q->event_count;i++)if(q->events[i].status==0xff&&q->events[i].a) {
        double tempo=60000000.0/q->events[i].a;
        if(tempo*c.tempo_scale>255)c.tempo_scale=255/tempo;
    }
    if(c.tempo_scale<1)diagnose(&c,M64_WARNING,56,NULL,"High tempos are represented with a reduced tick resolution to preserve playback speed.");
    if (!cancelled(&c,0,0) && timeline(&c) && (analysis||fit_sequence(&c)) && select_registers(&c) && !analysis) {
        r->compiled=bank_json(&c);
    }
    free(c.notes);
    for (unsigned i=0;i<M64_ARTIFACT_COUNT;i++) if (r->files[i].failed && r->status==M64_OK) r->status=M64_NOMEM;
    if (r->status!=M64_OK) for (unsigned i=0;i<M64_ARTIFACT_COUNT;i++) { free(r->files[i].data); memset(&r->files[i],0,sizeof(Buffer)); }
    return r->status;
}
static uint64_t M64_CALL diagnostic_count(const M64Result *r) { return r?r->count:0; }
static int32_t M64_CALL diagnostic_at(const M64Result *r,uint64_t i,M64DiagnosticView *v) {
    if (!r||!v||i>=r->count) return M64_INVALID;
    *v=r->diagnostics[i].view;v->message=r->diagnostics[i].text;return M64_OK;
}
static int32_t M64_CALL compile_request(const M64Request *q,const M64Hooks *h,M64Result **out){return compile_common(q,h,out,false);}
static int32_t M64_CALL analyze(const M64Request *q,const M64Hooks *h,M64Result **out){return compile_common(q,h,out,true);}
static uint64_t M64_CALL artifact_count(const M64Result *r) { return r&&r->compiled&&r->status==M64_OK?M64_ARTIFACT_COUNT:0; }
static int32_t M64_CALL artifact_at(const M64Result *r,uint64_t i,M64ArtifactView *v) {
    if (!r||!v||!r->compiled||r->status!=M64_OK||i>=M64_ARTIFACT_COUNT) return M64_INVALID;
    v->kind=(uint32_t)i;v->data=r->files[i].data;v->size=r->files[i].size;return M64_OK;
}
static void M64_CALL result_free(M64Result *r) {
    if (r) {for(unsigned i=0;i<M64_ARTIFACT_COUNT;i++)free(r->files[i].data);free(r->ranges);free(r->diagnostics);free(r);}
}
static const M64Instrument *M64_CALL default_program(uint32_t i) {return i<128?&gm_programs[i]:NULL;}
static const M64Instrument *M64_CALL default_drum(uint32_t i) {return i<128?&gm_drums[i]:NULL;}
static uint64_t M64_CALL range_count(const M64Result *r){return r&&r->status==M64_OK?r->range_count:0;}
static int32_t M64_CALL range_at(const M64Result *r,uint64_t i,M64RangeIssue *issue){if(!r||!issue||i>=range_count(r))return M64_INVALID;*issue=r->ranges[i];return M64_OK;}
static int32_t M64_CALL preset_supported(const M64RangeIssue *issue,uint32_t preset) {
    if(!issue||preset>=128||issue->percussion>1||!isfinite(issue->demand)||issue->demand<=0)return 0;
    const M64Instrument *d=issue->percussion?&gm_drums[preset]:&gm_programs[preset];double tuning=d->tuning;
    if(!issue->percussion)for(size_t i=0;i<stock_register_count;i++)if(!strcmp(d->sample,stock_registers[i].sample)) {
        for(size_t j=0;j<stock_register_count;j++)if(stock_registers[j].family==stock_registers[i].family)tuning=fmin(tuning,d->tuning*stock_registers[j].tuning/stock_registers[i].tuning);
        break;
    }
    return tuning*issue->demand<=4.00002;
}
static const M64Api api={compile_request,diagnostic_count,diagnostic_at,artifact_count,artifact_at,result_free,default_program,default_drum,analyze,range_count,range_at,preset_supported};
M64_PUBLIC const M64Api *M64_CALL m64_get_api_3(void) {return &api;}
