/* Pre-optimization host entry oracle; no guest code/data. */
void trace_enter_reference(uint32_t addr, Context* ctx)
{
    extern int netplay_async_deferred(void);
    extern int netplay_simulating(void);
    static unsigned slice,service_calls,arq_calls;
    static uint32_t prev_ee;
    static int diagnostics=-1;
    extern volatile long g_guest_stopping;
    if(g_guest_stopping) { extern void frontend_guest_checkpoint(void);frontend_guest_checkpoint(); }
    { extern void netplay_progress(Context*); netplay_progress(ctx); }
    g_last_fn=addr;g_call_count++;
    g_ring_addr[g_ring_i&31]=addr;g_ring_lr[g_ring_i&31]=ctx->lr;g_ring_i++;
    uint32_t index=(addr-0x80000000u)>>2;
    if(index<(1u<<24)) {
        unsigned byte=index>>3,mask=1u<<(index&7);
        if(!(s_seen[byte]&mask)) { s_seen[byte]|=mask;g_distinct_fns++; }
    }
    if(!netplay_simulating() && ++slice>=800) { extern void frontend_tick_guest_time(void);slice=0;frontend_tick_guest_time(); }
    if(addr==0x8033CC38u) {
        extern void wgseg_close(void);g_drawdone_pending=1;wgseg_close();
    } else if(addr==0x8033C80Cu) {
        extern int aurora_link_set_array(uint32_t,uint32_t,uint32_t);
        aurora_link_set_array(ctx->gpr[3],ctx->gpr[4],ctx->gpr[5]);
    }
    if(!netplay_async_deferred() && (++service_calls&63u)==0) {
        extern void frontend_service_alarms(Context*);
        extern void frontend_service_audio(Context*);
        frontend_service_alarms(ctx);frontend_service_audio(ctx);
    }
    if(!netplay_async_deferred() && (++arq_calls&255u)==0) {
        extern void arq_drain(Context*);
        uint32_t g[32];memcpy(g,ctx->gpr,sizeof g);
        uint32_t lr=ctx->lr,ctr=ctx->ctr,cr=ctx->cr,xer=ctx->xer;
        arq_drain(ctx);
        memcpy(ctx->gpr,g,sizeof g);ctx->lr=lr;ctx->ctr=ctr;ctx->cr=cr;ctx->xer=xer;
    }
    uint32_t ee=ctx->msr&0x8000u;
    if(!netplay_simulating() && ee && !prev_ee) frontend_deliver_interrupts(ctx);
    prev_ee=ee;
    if(addr==0x8037750Cu) { extern void costumes_poll(Context*); costumes_poll(ctx); }
    if(addr==0x8037AD48u||addr==0x80375428u||addr==0x80177368u||addr==0x80177704u||addr==0x80168BF8u||addr==0x8035E800u||addr==0x80360950u||addr==0x8026688Cu||addr==0x80266D70u||addr==0x8025BB5Cu||addr==0x800D0FA0u||addr==0x800686E4u||addr==0x80370E44u) { extern void mods_trace(uint32_t,Context*);mods_trace(addr,ctx); }
    if(addr==0x8022DDA8u||addr==0x801A1E20u||addr==0x8026688Cu||addr==0x8025A998u||addr==0x8016E934u||addr==0x8016EBC0u||addr==0x80177368u||addr==0x800236DCu||addr==0x8037750Cu||addr==0x80375428u) { extern void hooks_trace(uint32_t,Context*);hooks_trace(addr,ctx); }
    if(addr==0x801AEDC8u) { g_scene_tick++;if(!g_scene_lr)g_scene_lr=ctx->lr; }
    if(addr==0x8038912Cu || addr==0x8038B120u || addr==0x8038B5ACu) {
        extern void frontend_audio_trace(Context*,uint32_t);frontend_audio_trace(ctx,addr);
    }
    if(diagnostics<0) diagnostics=getenv("MELEE_TRACE_DIAGNOSTICS")!=NULL;
    if(diagnostics) trace_diagnostics(addr,ctx);
}
