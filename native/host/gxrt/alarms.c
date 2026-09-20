/* Run the real SDK alarm dispatcher; it owns ordering, cancellation and periods. */
#include "abi_recompcore.h"
extern long g_alarm_fired;
void frontend_service_alarms(Context* ctx) {
    extern int netplay_async_deferred(void);
    if (netplay_async_deferred()) return;
    static int active;
    if (!ctx || active || !(ctx->msr & 0x8000u)) return;
    uint32_t head=mem_r32(0x804D7358u);
    if (!head || (head & RAM_MASK)+40u > ctx->ram_size) return;
    extern uint64_t frontend_guest_timebase(void);
    uint64_t now=frontend_guest_timebase()+mem_r64(0x800030D8u);
    if ((int64_t)(now-mem_r64(head+8u))<0) return;
    uint32_t osctx=mem_r32(0x800000E4u);
    if (!osctx) return;
    RecFn dispatch=lookup_function(0x80343BC8u);
    if (lookup_is_stub(dispatch)) return;
    active=1;
    Context saved=*ctx;
    ctx->msr &= ~0x8000u;
    ctx->gpr[3]=8;
    ctx->gpr[4]=osctx;
    dispatch(ctx);
    uint64_t advanced=ctx->timebase;
    *ctx=saved;
    ctx->timebase=advanced;
    active=0;
    g_alarm_fired++;
}
