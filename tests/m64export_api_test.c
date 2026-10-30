#include "m64export/m64export.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const M64Api *api=m64_get_api_3();
    assert(api && !api->default_program(128) && !api->default_drum(128));
    for(unsigned i=0;i<128;i++)assert(api->default_program(i)->sample && api->default_drum(i)->sample);
    M64Event events[]={{0,1,0x90,60,100},{48,2,0x80,60,0}};
    M64Request q={0};q.events=events;q.event_count=2;q.duration=48;q.ppq=48;
    q.target=M64_TARGET_SM64_US;q.voice_limit=16;q.sequence_name="03_custom_test";q.bank_name="26_custom_test";
    M64Result *r=NULL;
    assert(api->compile(&q,NULL,&r)==M64_OK && api->artifact_count(r)==4);
    M64ArtifactView a;
    assert(api->artifact_at(r,4,&a)==M64_INVALID);
    api->result_free(r);
    events[0].a=events[1].a=127;
    assert(api->analyze(&q,NULL,&r)==M64_OK&&api->artifact_count(r)==0&&api->range_count(r)==1);
    M64RangeIssue issue;assert(api->range_at(r,0,&issue)==M64_OK&&issue.channel==0&&issue.note_high==127&&issue.note_count==1);
    assert(!api->preset_supported(&issue,0));
    unsigned supported=0;for(unsigned i=0;i<128;i++)supported+=api->preset_supported(&issue,i)!=0;
    assert(supported&&api->range_at(r,1,&issue)==M64_INVALID);api->result_free(r);
    q.sequence_name="../escape";
    assert(api->compile(&q,NULL,&r)==M64_INVALID && api->artifact_count(r)==0);
    api->result_free(r);api->result_free(NULL);
    puts("PASS native static C API");
    return 0;
}
