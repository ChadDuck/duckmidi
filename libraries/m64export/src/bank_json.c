#include "internal.h"

/* Locale-independent JSON floating point: compiler/host may use comma decimals. */
static void number(Buffer *b,double value) {
    char text[64];snprintf(text,sizeof(text),"%.12g",value);
    for(char *s=text;*s;s++)if(*s==',')*s='.';
    bytes(b,text,strlen(text));
}
static void envelope(Buffer *b,const M64Instrument *d) {
    format(b,"[[%u,32700],[%u,%u],\"hang\"]",d->attack,d->decay,d->sustain);
}
bool bank_json(Compile *c) {
    Buffer *b=&c->result->files[M64_BANK_JSON];
    format(b,"{\n  \"sample_bank\": \"instruments\",\n  \"envelopes\": {\n");
    unsigned total=c->program_count+c->drum_count;
    for(unsigned i=0;i<total;i++) {
        const M64Instrument *d=i<c->program_count?program_definition(c,c->programs[i]):drum_definition(c,c->percussion[i-c->program_count]);
        format(b,"    \"env%u\": ",i);envelope(b,d);format(b,"%s\n",i+1<total?",":"");
    }
    format(b,"  },\n  \"instruments\": {\n");
    for(unsigned i=0;i<c->program_count;i++) {
        const M64Instrument *d=program_definition(c,c->programs[i]);
        const Register *r=&c->registers[i];
        const M64Instrument *normal=r->enabled&&r->normal_hi<0?&r->high:d;
        format(b,"    \"inst%u\": {\"release_rate\": %u, \"envelope\": \"env%u\", \"sound\": {\"sample\": \"%s\", \"tuning\": ",i,d->release_rate,i,normal->sample);
        double origin=pow(2.0,-21.0/12.0)*((c->programs[i]&128)?2.0:1.0);
        number(b,normal->tuning*origin);format(b,"}");
        if(r->enabled&&r->normal_hi>=0) {
            format(b,", \"normal_range_hi\": %d, \"sound_hi\": {\"sample\": \"%s\", \"tuning\": ",r->normal_hi,r->high.sample);
            number(b,r->high.tuning*origin);format(b,"}");
        }
        format(b,"}%s\n",i+1<c->program_count||c->drum_count?",":"");
    }
    if(c->drum_count) {
        format(b,"    \"percussion\": [\n");
        for(unsigned i=0;i<c->drum_count;i++) {
            const M64Instrument *d=drum_definition(c,c->percussion[i]);
            format(b,"      {\"release_rate\": %u, \"pan\": 64, \"envelope\": \"env%u\", \"sound\": {\"sample\": \"%s\", \"tuning\": ",d->release_rate,c->program_count+i,d->sample);
            number(b,d->tuning);format(b,"}}%s\n",i+1<c->drum_count?",":"");
        }
        format(b,"    ]\n");
    }
    format(b,"  },\n  \"instrument_list\": [");
    for(unsigned i=0;i<c->program_count;i++)format(b,"%s\"inst%u\"",i?", ":"",i);
    format(b,"]\n}\n");
    format(&c->result->files[M64_SEQUENCE_JSON],"{\"%s\": [\"%s\"]}\n",c->request->sequence_name,c->request->bank_name);
    Buffer *m=&c->result->files[M64_MANIFEST];
    format(m,"{\n  \"target\": \"n64decomp/sm64 US\",\n  \"sample_bank\": \"instruments\",\n  \"sample_assets_included\": false,\n  \"sequence_bytes\": %llu,\n  \"sequence_memory_warning_bytes\": 16128,\n  \"sequence_address_max_offset\": 65535,\n  \"additional_timing_grid_ticks\": %u,\n  \"tempo_scale\": ",(unsigned long long)c->result->files[M64_SEQUENCE].size,c->quality_grid);
    number(m,c->tempo_scale);format(m,",\n  \"sequence_load_bytes\": %llu,\n  \"minimum_temporary_sequence_pool_bytes\": %llu,\n  \"programs\": [\n",(unsigned long long)sequence_load_bytes(c),(unsigned long long)(sequence_load_bytes(c)+16));
    bool comma=false;
    for(unsigned p=0;p<4096;p++)if(c->slots[p]>=0) {
        const M64Instrument *d=program_definition(c,p);
        format(m,"%s    {\"gm_program\": %u, \"channel\": %d, \"high_octave_variant\": %s, \"slot\": %d, \"sample\": \"%s\", \"c4_tuning\": ",comma?",\n":"",p&127,c->request->channel_programs?(int)(p>>8):-1,(p&128)?"true":"false",c->slots[p],d->sample);number(m,d->tuning);
        const Register *r=&c->registers[c->slots[p]];
        if(r->enabled){format(m,", \"register\": {\"normal_range_hi\": %d, \"sample\": \"%s\", \"c4_tuning\": ",r->normal_hi,r->high.sample);number(m,r->high.tuning);format(m,"}");}
        format(m,"}");comma=true;
    }
    format(m,"\n  ],\n  \"drums\": [\n");
    for(unsigned i=0;i<c->drum_count;i++) {
        unsigned key=c->percussion[i];const M64Instrument *d=drum_definition(c,key);
        format(m,"%s    {\"gm_note\": %u, \"slot\": %u, \"sample\": \"%s\", \"tuning\": ",i?",\n":"",key,i,d->sample);number(m,d->tuning);format(m,"}");
    }
    format(m,"\n  ],\n  \"diagnostics\": [\n");
    for(size_t i=0;i<c->result->count;i++) {
        Diagnostic *d=&c->result->diagnostics[i];
        format(m,"%s    {\"severity\": %u, \"code\": %u, \"source_id\": %llu, \"tick\": %llu, \"channel\": %d, \"message\": \"",i?",\n":"",d->view.severity,d->view.code,(unsigned long long)d->view.source_id,(unsigned long long)d->view.tick,d->view.channel);
        for(const char *s=d->text;*s;s++){if(*s=='"'||*s=='\\')put(m,'\\');put(m,(unsigned char)*s);}
        format(m,"\"}");
    }
    format(m,"\n  ]\n}\n");
    for(unsigned i=0;i<M64_ARTIFACT_COUNT;i++)if(c->result->files[i].failed){c->result->status=M64_NOMEM;return false;}
    return !cancelled(c,3,100);
}
