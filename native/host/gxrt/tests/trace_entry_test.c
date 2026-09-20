/* Callback and register parity for the production scheduling entry hook. */
#include "abi_recompcore.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TRACE_COLD_NOINLINE __attribute__((noinline,cold))
struct State {
    uint32_t last,ring_addr[32],ring_lr[32],scene_lr;
    unsigned long ring_i;
    long calls,distinct,drawdone,scene_tick;
    uint8_t seen[1u<<21];
    uint64_t event_hash;
    unsigned events,simulating,deferred,depth;
};
static struct State states[2],*active;
static Context* context;
static void (*entry)(uint32_t,Context*);
volatile long g_guest_stopping;
#define g_last_fn (active->last)
#define g_call_count (active->calls)
#define g_ring_addr (active->ring_addr)
#define g_ring_lr (active->ring_lr)
#define g_ring_i (active->ring_i)
#define s_seen (active->seen)
#define g_distinct_fns (active->distinct)
#define g_drawdone_pending (active->drawdone)
#define g_scene_tick (active->scene_tick)
#define g_scene_lr (active->scene_lr)
static void event(unsigned value) { active->event_hash=(active->event_hash*0x100000001b3ull)^value;active->events++; }
static void mutate(Context* c,unsigned kind) {
    event(kind);c->gpr[kind%32]^=0x9e3779b9u+kind;c->lr+=kind;c->ctr^=kind;
    c->cr-=kind;c->xer^=kind<<3;c->timebase+=kind*13;c->msr^=(kind&1)?0x8000u:0;
    active->simulating^=(kind==3);active->deferred^=(kind==5);
}
int netplay_simulating(void) { event(21);return active->simulating; }
int netplay_async_deferred(void) { event(22);return active->deferred; }
void netplay_progress(Context* c) { mutate(c,1); }
void frontend_guest_checkpoint(void) { mutate(context,2); }
void frontend_tick_guest_time(void) { mutate(context,3); }
void wgseg_close(void) { mutate(context,4); }
int aurora_link_set_array(uint32_t a,uint32_t b,uint32_t c) { event(a);event(b);event(c);mutate(context,5);return 1; }
void frontend_service_alarms(Context* c) {
    mutate(c,6);
    if(!active->depth && (c->gpr[0]&3)==0) { active->depth++;entry(0x80375428u,c);active->depth--; }
}
void frontend_service_audio(Context* c) { mutate(c,7); }
void arq_drain(Context* c) {
    mutate(c,8);
    if(!active->depth && (c->gpr[0]&3)==1) { active->depth++;entry(0x8037750cu,c);active->depth--; }
}
void frontend_deliver_interrupts(Context* c) { mutate(c,9); }
void costumes_poll(Context* c) { mutate(c,10); }
void mods_trace(uint32_t a,Context* c) { event(a);mutate(c,11); }
void hooks_trace(uint32_t a,Context* c) { event(a);mutate(c,12); }
void frontend_audio_trace(Context* c,uint32_t a) { event(a);mutate(c,13); }
static void trace_diagnostics(uint32_t a,Context* c) { event(a);mutate(c,14); }
#include "production.inc"
#include "trace_entry_reference.inc"
static uint32_t random_state=0x8b78d821;
static uint32_t random32(void) { random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }
static const uint32_t addresses[]={
  0x8033CC38,0x8033C80C,0x8037750C,0x8037AD48,0x80375428,0x80177368,
  0x80177704,0x80168BF8,0x8035E800,0x80360950,0x8026688C,0x80266D70,
  0x8025BB5C,0x800D0FA0,0x800686E4,0x80370E44,0x8022DDA8,0x801A1E20,
  0x8025A998,0x8016E934,0x8016EBC0,0x800236DC,0x801AEDC8,0x8038912C,
  0x8038B120,0x8038B5AC,0x80000000,0x83FFFFFF,0,0xFFFFFFFF
};
int main(void) {
    for(unsigned i=0;i<200000;i++) {
        uint32_t addr=i%64<sizeof addresses/sizeof *addresses?addresses[i%64]:random32();
        Context a,b;uint8_t* bytes=(uint8_t*)&a;
        for(unsigned j=0;j<sizeof a;j++)bytes[j]=(uint8_t)random32();b=a;
        g_guest_stopping=(i%59)==0;
        states[0].simulating=states[1].simulating=(i/3000)%2;
        states[0].deferred=states[1].deferred=(i/2500)%2;
        active=&states[0];context=&a;entry=trace_enter_reference;entry(addr,&a);
        active=&states[1];context=&b;entry=trace_enter;entry(addr,&b);
        assert(!memcmp(&a,&b,sizeof a));
        assert(states[0].event_hash==states[1].event_hash && states[0].events==states[1].events);
        if((i&1023)==0)assert(!memcmp(states,states+1,sizeof states[0]));
    }
    assert(!memcmp(states,states+1,sizeof states[0]));
    puts("PASS: 200,000 entries; complete Context/counters/seen-map; callback ordering, re-entry, async-state changes, all hook addresses and diagnostics");return 0;
}
