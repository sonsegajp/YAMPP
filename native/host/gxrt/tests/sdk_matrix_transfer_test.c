/* Differential oracle: the check script extracts the two untouched generated
 * routines and the real generic PSQ helpers into separate translation units. */
#include "abi_recompcore.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void func_80341408(Context*), func_8034143C(Context*);
void sdk_write_mtx4x3(Context*), sdk_write_nrm_mtx3x3(Context*);
Context *g_cur_ctx;
int g_trace = 1;
static unsigned trace_calls;
static uint32_t traced;
void trace_enter(uint32_t address, Context* ctx) {
    (void)ctx; traced = address; ++trace_calls;
}

typedef struct { uint32_t address; uint64_t value; unsigned size, write; } Event;
static Event events[64];
static unsigned event_count, mutate_io;
static uint32_t external_words[12];
static unsigned char ram[1024], initial_ram[1024], expected_ram[1024];
static uint32_t random_state = 0x19203141u;
static uint32_t next_random(void) {
    uint32_t x=random_state; x^=x<<13; x^=x>>17; x^=x<<5;
    return random_state=x;
}
static void record(uint32_t address, uint64_t value, unsigned size, unsigned write) {
    assert(event_count < 64);
    Event* e=&events[event_count++];
    e->address=address; e->value=value; e->size=size; e->write=write;
}
static uint64_t read_external(CPUState* cpu, uint32_t address, uint8_t size) {
    assert(size==4 && address>=0xd0000100u && address<0xd0000180u);
    uint32_t value=external_words[((address-0xd0000100u)/4)%12];
    if(mutate_io) { cpu->gpr[3]^=0x40u; cpu->timebase+=13; }
    record(address,value,size,0); return value;
}
static void write_external(CPUState* cpu, uint32_t address, uint64_t value, uint8_t size) {
    record(address,value,size,1);
    if(mutate_io) { cpu->gpr[4]^=0x10u; cpu->gpr[7]++; cpu->timebase+=17; }
}
static void put_word(unsigned char* p, uint32_t v) {
    p[0]=(unsigned char)(v>>24);p[1]=(unsigned char)(v>>16);
    p[2]=(unsigned char)(v>>8);p[3]=(unsigned char)v;
}
static void reset_capture(void) {
    memset(events,0,sizeof events);event_count=trace_calls=traced=0;
    memcpy(ram,initial_ram,sizeof ram);
}

static void run_case(unsigned normal, unsigned source, unsigned destination, unsigned iteration) {
    static const uint32_t sources[]={0x80000100u,0xc0000100u,0x80000103u,0xd0000100u};
    static const uint32_t destinations[]={0xcc008000u,0x80000200u,0xc0000200u,0x80000100u,0x80000104u,0x80000120u};
    static const uint32_t special[]={0,0x80000000u,0x00000001u,0x007fffffu,0x00800000u,
        0x3f800000u,0xbf800000u,0x7f7fffffu,0xff7fffffu,0x7f800000u,0xff800000u,
        0x7fc00001u,0xffc01234u,0x7f800001u,0xff800123u,0x7fffffffu};
    mutate_io=(iteration>>1)&1;
    Context initial; memset(&initial,0xa7,sizeof initial);
    initial.ram=ram;initial.ram_size=sizeof ram;
    initial.external_read=read_external;initial.external_write=write_external;
    for(unsigned i=0;i<32;i++){initial.gpr[i]=next_random();initial.fpr[i]=next_random();initial.ps1[i]=next_random();}
    for(unsigned i=0;i<8;i++)initial.gqr[i]=next_random();
    initial.gpr[3]=sources[source];initial.gpr[4]=destinations[destination];
    for(unsigned i=0;i<sizeof initial_ram;i++)initial_ram[i]=(unsigned char)next_random();
    for(unsigned i=0;i<12;i++){
        uint32_t bits=iteration<32 ? special[(iteration+i)%16] : next_random();
        external_words[i]=bits;
        if(source!=3)put_word(initial_ram+(sources[source]&RAM_MASK)+i*4,bits);
    }
    Context expected=initial;g_cur_ctx=&expected;reset_capture();
    if(normal)func_8034143C(&expected);else func_80341408(&expected);
    Event reference[64];memcpy(reference,events,sizeof reference);unsigned n=event_count;
    unsigned reference_trace=trace_calls;uint32_t reference_address=traced;
    memcpy(expected_ram,ram,sizeof ram);
    Context actual=initial;g_cur_ctx=&actual;reset_capture();
    if(normal)sdk_write_nrm_mtx3x3(&actual);else sdk_write_mtx4x3(&actual);
    if(memcmp(&expected,&actual,sizeof actual)||memcmp(ram,expected_ram,sizeof ram)||
       n!=event_count||memcmp(reference,events,sizeof events)||reference_trace!=trace_calls||reference_address!=traced){
        fprintf(stderr,"matrix parity failure normal=%u source=%u destination=%u iteration=%u context=%d ram=%d events=%d trace=%u/%u\n",
            normal,source,destination,iteration,memcmp(&expected,&actual,sizeof actual),memcmp(ram,expected_ram,sizeof ram),memcmp(reference,events,sizeof events),reference_trace,trace_calls);
        exit(1);
    }
}
int main(void) {
    unsigned cases=0;
    for(unsigned normal=0;normal<2;normal++)for(unsigned source=0;source<4;source++)
        for(unsigned destination=0;destination<6;destination++)for(unsigned i=0;i<2048;i++){
            g_trace=(i&1);run_case(normal,source,destination,i);cases++;
        }
    printf("matrix transfer parity PASS: %u cases; FIFO read/write order, complete Context, RAM aliases, nonfinite float bits, and trace enabled/disabled\n",cases);
    return 0;
}
