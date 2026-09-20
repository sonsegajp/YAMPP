/* Module-side runtime for the RecompCore StaticRecompABI v2 target.
 *
 * Everything the generated bodies call that isn't an inline in abi_recompcore.h:
 * indirect-call resolution, Gekko paired-single quantized load/store, and the
 * trace/HLE hooks. Deliberately small â€” the chassis (Dolphin) owns memory,
 * hardware, threading and timing; this file only serves the recompiled code.
 */
#include "abi_recompcore.h"
#include <stdio.h>
#include "recomp_funcs.h"
#include <stdlib.h>
#include <math.h>

/* ---- indirect call resolution -------------------------------------------
 * g_fnents is emitted sorted by address, so a binary search is exact and needs
 * no build step. Returns NULL for an address we don't cover, which propagates
 * out of dispatch() as "not covered" and hands the PC back to the interpreter. */
static int ent_cmp(const void* k, const void* e)
{
    uint32_t a = *(const uint32_t*)k, b = ((const FnEnt*)e)->addr;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static void lookup_null_stub(Context* ctx) { (void)ctx; }

static void lookup_stub(Context* ctx) {
    fprintf(stderr, "[fatal] unresolved guest call target=0x%08X caller=0x%08X\n",
            ctx->lr, ctx->gpr[0]);
    fflush(stderr);
    exit(9);
}

int lookup_is_stub(RecFn fn) { return fn == NULL || fn == lookup_stub || fn == lookup_null_stub; }

RecFn lookup_function(uint32_t addr)
{
    { extern RecFn hooks_lookup(uint32_t); RecFn hook=hooks_lookup(addr); if(hook) return hook; }
    { extern RecFn costumes_lookup(uint32_t); RecFn costume=costumes_lookup(addr); if(costume) return costume; }
    if (g_mex_active) { RecFn dynamic = mex_runtime_lookup(addr); if (dynamic) return dynamic; }
    extern void sdk_write_mtx4x3(Context*), sdk_write_nrm_mtx3x3(Context*);
    if (addr == 0x80341408u) return sdk_write_mtx4x3;
    if (addr == 0x8034143Cu) return sdk_write_nrm_mtx3x3;
    { extern int netplay_session_active(void);
      extern void netplay_name_text(Context*), netplay_name_list_full(Context*);
      if (netplay_session_active()) {
        if (addr == 0x8023754Cu) return netplay_name_text;
        if (addr == 0x802375ECu) return netplay_name_list_full;
      }
    }
    extern RecFn mods_lookup(uint32_t);
    RecFn mod=mods_lookup(addr);if(mod)return mod;
    const FnEnt* e = (const FnEnt*)bsearch(&addr, g_fnents, (size_t)g_fnents_count,
                                           sizeof(FnEnt), ent_cmp);
    if (e) return e->fn;
    if (!addr) return lookup_null_stub;
    static int miss_shown;
    if (miss_shown < 20) { miss_shown++; fprintf(stderr, "[lookup] miss: 0x%08X\n", addr); }
    return lookup_stub;
}

/* ---- Gekko paired-single quantized load/store ----------------------------
 * GQR[i] carries a load type/scale (bits 16..18 / 24..29) and a store type/scale
 * (bits 0..2 / 8..13). Types 4..7 are the signed/unsigned 8- and 16-bit fixed
 * point formats; 0 is raw float. GQR0 is float-by-convention, which compilers
 * assume, so we read it as 0 rather than trusting whatever is in the register. */
static const int kTypeSize[8] = {4, 4, 4, 4, 1, 2, 1, 2};

static int type_size(int t) { return kTypeSize[t & 7]; }

static double dequant(uint32_t ea, int type, int scale)
{
    // Raw floating-point loads do not use the quantization scale. Matrix and
    // skinning code predominantly uses this format; avoid a libm call per lane.
    if ((type & 7) < 4) return (double)mem_rf32(ea);
    double s = ldexp(1.0, -(scale & 0x20 ? (scale | ~0x3F) : scale));
    switch (type & 7) {
    case 4:  return (double)mem_r8(ea) * s;
    case 5:  return (double)mem_r16(ea) * s;
    case 6:  return (double)(int8_t)mem_r8(ea) * s;
    case 7:  return (double)(int16_t)mem_r16(ea) * s;
    default: return (double)mem_rf32(ea);
    }
}

static void quant(uint32_t ea, double v, int type, int scale)
{
    if ((type & 7) < 4) { mem_wf32(ea, (float)v); return; }
    double s = ldexp(1.0, (scale & 0x20 ? (scale | ~0x3F) : scale));
    double x = v * s;
    switch (type & 7) {
    case 4: x = x < 0.0 ? 0.0 : (x > 255.0 ? 255.0 : x);             mem_w8 (ea, (uint8_t)x);           break;
    case 5: x = x < 0.0 ? 0.0 : (x > 65535.0 ? 65535.0 : x);         mem_w16(ea, (uint16_t)x);          break;
    case 6: x = x < -128.0 ? -128.0 : (x > 127.0 ? 127.0 : x);       mem_w8 (ea, (uint8_t)(int8_t)x);   break;
    case 7: x = x < -32768.0 ? -32768.0 : (x > 32767.0 ? 32767.0 : x); mem_w16(ea, (uint16_t)(int16_t)x); break;
    default: mem_wf32(ea, (float)v);                                                                    break;
    }
}

void ru_psq_load(Context* ctx, int frD, uint32_t ea, int w, int gqr)
{
    uint32_t g = gqr ? ctx->gqr[gqr] : 0;
    int type = (g >> 16) & 7, scale = (g >> 24) & 63;
    ctx->fpr[frD] = dequant(ea, type, scale);
    ctx->ps1[frD] = w ? 1.0 : dequant(ea + type_size(type), type, scale);
}

void ru_psq_store(Context* ctx, int frS, uint32_t ea, int w, int gqr)
{
    uint32_t g = gqr ? ctx->gqr[gqr] : 0;
    int type = g & 7, scale = (g >> 8) & 63;
    quant(ea, ctx->fpr[frS], type, scale);
    if (!w) quant(ea + type_size(type), ctx->ps1[frS], type, scale);
}

/* ---- hooks ---------------------------------------------------------------
 * sc / mftb reach the chassis: it owns exception dispatch and the timebase, and
 * ctx->timebase is part of the shared CPU state, so we read straight from it. */
void hle_syscall(Context* ctx) { ctx->exception |= 1u; }

uint32_t hle_timebase(int upper)
{
    extern uint64_t frontend_guest_timebase(void);
    uint64_t tb=frontend_guest_timebase();
    return upper?(uint32_t)(tb>>32):(uint32_t)tb;
}

/* Records the frontier so a blocked guest can be located. Was a no-op stub;
 * with the guest on its own thread and nothing presenting, "where is it stuck"
 * is the only question that matters. */
uint32_t g_last_fn = 0;
long     g_call_count = 0;
/* Distinct functions reached -- the discriminator the call count cannot give.
 * A rising count means boot is advancing into new code; a flat one means the
 * same handful repeating, i.e. a stall wearing a frame loop's clothes. */
static unsigned char s_seen[1u << 21];
long g_distinct_fns = 0;
void frontend_deliver_interrupts(Context* ctx);
/* Is the guest even asking for data? Two outcomes: no calls means the fault is
 * in the SDK layer above DVD; calls that never reach di_execute means it is
 * between the SDK and the device. */
long g_dvd_readasync = 0, g_dvd_lowread = 0, g_dvd_readabs = 0;
/* ARAM leg. Read #3 hands off to HSD_DevComDVDCallback, which stages data to
 * ARAM, and nothing follows. Counting the post, the DMA start and the ISR
 * separates "never issued" from "issued, never completed". */
long g_ar_post = 0, g_ar_dma = 0, g_ar_isr = 0;
/* VI leg: with file streaming open, whether the guest ever configures VI and
 * publishes a framebuffer is the last gate before anything can be presented. */
long g_vi_configure = 0, g_vi_setnextfb = 0, g_vi_flush = 0;
/* The 12 candidate writers of lb_80433318, found by dataflow over the emitted
 * code. Entry counters plus the LR of whoever calls the two bulk initialisers:
 * confirms none runs, and names the caller that should have. */
long g_w658 = 0, g_w8BC = 0, g_wother = 0;
/* Scene dispatch: gm_801AEDC8 is the per-frame scene tick. Its LR names the
 * dispatcher, which is the site that would select a different scene. */
long g_scene_tick = 0; uint32_t g_scene_lr = 0;
/* The other caller of func_8001C8BC. Since the gm scene machine never runs,
 * this early-path caller is the one that could still initialise lb_80433318. */
long g_c178C = 0; uint32_t g_c178C_lr = 0;
/* Preload-cache state transitions (lbdvd.c). Entries start at state 1 and must
 * advance (2 -> 3 -> 4). Ours never leave 1, so the spin never exits. Counting
 * the functions that perform those writes says which transition is missing. */
long g_pc_cache = 0, g_pc_CC4 = 0, g_pc_E64 = 0;
/* lbFile_800164A4: Melee's async file loader. lbdvd.c:288 registers
 * lbDvd_80017E64 with it as the completion callback -- the callback that moves a
 * PreloadEntry out of its loading state. That callback never fires, so either
 * this loader is not reached, or it is reached and never completes. */
long g_lbfile = 0;
/* DevCom -- the queue everything lands on. lbFile_800164A4 enqueues here with
 * lbDvd_80017E64 as the completion callback; that callback never fires. These
 * counters say whether the request is posted and whether the wake-ups that
 * drain the queue ever run. */
long g_dc_req = 0, g_dc_dvdwake = 0, g_dc_aramwake = 0;
/* Callbacks of the two requests that matter: #4 (2MB, ARAM dest, type 0x23 --
 * the head-of-line candidate) and #5 (the preload whose callback never fires).
 * If #4's fires and #5's does not, the blockage is not head-of-line. */
long g_cb4 = 0, g_cb5 = 0;
/* Landmarks the old standalone host reached (20.87M calls, frontier #988).
 * Comparing against a known-further-along run localises the divergence far
 * faster than walking the stall backward one caller at a time. */
long g_lm_195D0 = 0, g_lm_192A8 = 0, g_lm_gmMain = 0, g_lm_HSDinit = 0;
/* Who spins on lb_800195D0 21.7M times? Its LR names the outer loop, which is
 * the frame boot is actually stuck in. */
uint32_t g_lr195[16]; int g_lr195_n = 0;  /* widened from 4: the ceiling was mine, not the data's */
/* Callers of the polling loop itself. gmMain now runs per frame, so this names
 * where inside the frame it blocks -- a different question from before, when
 * nothing was running at all. */
uint32_t g_lrCC84[4]; int g_lrCC84_n = 0;
uint32_t g_w658_lr = 0, g_w8BC_lr = 0;
/* DVDGetDriveStatus polls three small-data-area globals (r13-17392, -17400,
 * -17416). Capturing r13 turns those offsets into real addresses, which the
 * address watch can then attribute to a writer. */
uint32_t g_dgs_r13 = 0, g_dgs_v[3];
uint32_t g_dgs_writer[3]; long g_dgs_changes[3];
/* Upstream check: if the DVD subsystem's own init never ran, the state
 * DVDGetDriveStatus polls would never be populated regardless of anything the
 * HLE does. Distinguishes "HLE skipped bookkeeping" from "init never happened". */
long g_dvd_init = 0, g_dvdfs_init = 0, g_dvd_clearq = 0;
/* Watch the global the 161M-call loop revolves around (0x80430000+13080).
 * Sampling it at function entries names its writer to function granularity,
 * with no model of what the SDK is doing -- the technique that named the NaN
 * view-matrix writer in a single run. */
#define POLL_ADDR 0x80433320u   /* base+8: one of the three fields the loop reads (+8, +12, +92) */
uint32_t g_poll_first = 0xDEADBEEFu, g_poll_last = 0, g_poll_writer = 0;
long     g_poll_changes = 0;
unsigned int g_fncount[0x10000];
uint32_t g_ring_addr[32], g_ring_lr[32]; unsigned long g_ring_i = 0;
uint32_t g_lr195D0[8];
long g_setvcd_calls = 0, g_xfspecs_calls = 0, g_dirty_calls = 0, g_begin_calls = 0;
long g_calldl_calls = 0, g_setvtxdesc_calls = 0;
extern long vblank_seq_rt(void);
long g_alarm_at_call=0, g_alarm_at_seq=0;
volatile long g_drawdone_pending = 0;
long g_alarm_set=0; uint32_t g_alarm_obj=0, g_alarm_handler=0;
long g_alarm_fired=0, g_alarm_nolookup=0, g_alarm_visitA=0, g_alarm_visitB=0;
long g_pad_renewraw=0,g_pad_renew=0,g_pad_init=0,g_padread=0,g_gmmain=0;
long g_vi_retrace_handler=0,g_vi_setpostcb=0,g_vi_postcb=0,g_vi_hsdinit=0,g_pad_fill=0;
uint32_t     g_fnaddr[0x10000];
int trace_was_entered(uint32_t addr) {
    uint32_t idx=(addr-0x80000000u)>>2;
    return idx<(1u<<24) && (s_seen[idx>>3] & (1u<<(idx&7)))!=0;
}
/* Keep rare entry work out of the instruction-cache hot scheduling path. */
#if defined(__GNUC__)
#define TRACE_COLD_NOINLINE __attribute__((noinline, cold))
#elif defined(_MSC_VER)
#define TRACE_COLD_NOINLINE __declspec(noinline)
#else
#define TRACE_COLD_NOINLINE
#endif
static TRACE_COLD_NOINLINE void trace_diagnostics(uint32_t addr, Context* ctx)
{
    /* Per-function call census. "Where is it spinning" has been the question at
     * every stall this session, and each time it was answered by an instrument
     * rather than by reading code. Direct-mapped on the address so it costs one
     * add; collisions are harmless for ranking. */
    {   unsigned slot = (addr >> 2) & 0xFFFFu;
        g_fncount[slot]++;
        g_fnaddr[slot] = addr;   /* hashed slots are unreadable; keep the address */
        /* The pad queue the guest spins on is filled from the post-retrace
         * callback chain, which is also what flips the XFB. Count each link so
         * the break shows up as a specific zero rather than a guess. */
        if (addr == 0x8034E964u) g_vi_retrace_handler++;   /* __VIRetraceHandler */
        /* Draw-done is a request, not a heartbeat.
         *
         * PE finish was being committed unconditionally every frame, so the
         * finish callback ran before the guest had set the XFB to WAITDONE and
         * the SDK panicked:
         *   assertion "_p->xfb[idx].status == HSD_VI_XFB_WAITDONE" failed in
         *   video.c on line 722
         * which halted the CPU via OSPanic -> PPCHalt. Arm it only when the
         * guest actually asks (GXSetDrawDone), matching the hardware order:
         * copy EFB->XFB, mark WAITDONE, request draw-done, then the interrupt. */
        /* Where does the VCD flush happen, and is it reaching WGPIPE?
         * GXSetVtxDesc only marks state dirty; __GXSetVCD emits the CP writes.
         * Counting both tells us whether the flush runs at all. */
        if (addr == 0x8033C260u) {                      /* __GXSetVCD */
            g_setvcd_calls++;
            /* The flush reads VCD_HI from gpr[4]+24. Log that address and the
             * value there, so the state field can be watched rather than
             * inferred from what lands on the wire. */
            if (ctx && g_setvcd_calls <= 6) {
                uint32_t base = ctx->gpr[4];
                printf("[gxstate] base=%08X vcdlo@+20=%08X vcdhi@+24=%08X\n",
                       base, mem_r32(base + 20u), mem_r32(base + 24u));
                fflush(stdout);
            }
        }
        if (addr == 0x8033D050u) g_dirty_calls++;       /* __GXSetDirtyState */
        if (addr == 0x803410D8u) g_calldl_calls++;      /* GXCallDisplayList, direct */
        if (addr == 0x8033BF00u) g_setvtxdesc_calls++;  /* GXSetVtxDesc, direct */
        if (addr == 0x8033D0DCu) g_begin_calls++;       /* GXBegin */
        if (addr == 0x8033BDA8u) g_xfspecs_calls++;     /* __GXXfVtxSpecs */
        /* GXSetVtxDesc(attr, type): does the game ever request TEX0 (attr 13)?
         * gx_recomp's VCD says no and aurora's shader says yes; this reads the
         * request at the source, independent of which byte stream carries it. */
        if (addr == 0x8033BF00u && ctx) {
            static unsigned char seen[32];
            uint32_t at = ctx->gpr[3], ty = ctx->gpr[4];
            if (at < 32u && !seen[at]) {
                seen[at] = 1;
                printf("[vtxdesc] attr=%u type=%u\n", at, ty);
                fflush(stdout);
            }
        }

        /* GXSetArray(attr, data, size, stride): intercept at the source rather
         * than walking the FIFO. Melee emits CP_REG_ARRAYBASE_ID with a guest
         * pointer, which aurora rejects, and a FIFO rewrite would have to walk
         * draw commands (whose length depends on vertex size) to find them.
         * Here the arguments are already in registers. */
        /* GXCallDisplayList(list, nbytes).
         *
         * Records the list; the bytes are substituted where the GX_CMD_CALL_DL
         * opcode reaches WGPIPE (see wr_wgpipe), which is where the GP would
         * execute it. Currently disabled -- the splice path is under repair
         * after a simplification dropped several of its corrections; PLAN.md
         * has the current state. Flip the 0 to re-enable.
         *
         * (The six stacked comments previously here were successive edits'
         * rationales, several contradicting each other and none describing the
         * live code. They have been replaced by this one.) */
        if (addr == 0x803410D8u && ctx) {
            uint32_t ea = ctx->gpr[3], n = ctx->gpr[4];
            uint32_t off = ea & 0x0FFFFFFFu;
            if (0 && ctx->ram && n && off + n <= ctx->ram_size) {   /* see PLAN */
                extern void wgseg_pending_dl(const unsigned char*, uint32_t);
                wgseg_pending_dl(ctx->ram + off, n);
            }
        }


        if (addr == 0x8034EBD0u) g_vi_setpostcb++;         /* VISetPostRetraceCallback */
        if (addr == 0x80375C34u) g_vi_postcb++;            /* HSD_VIPostRetraceCB */
        if (addr == 0x803767B8u) g_vi_hsdinit++;           /* HSD_VIInit */
        if (addr == 0x800195FCu) g_pad_fill++;             /* fn_800195FC */
        if (addr == 0x803769FCu) g_pad_renewraw++;         /* HSD_PadRenewRawStatus */
        if (addr == 0x80377CE8u) g_pad_renew++;            /* HSD_PadRenewStatus */
        if (addr == 0x80377D98u) g_pad_init++;             /* HSD_PadInit */
        if (addr == 0x8034DA00u) g_padread++;              /* PADRead */
        if (addr == 0x8015FBA4u) g_gmmain++;               /* gmMainLib_8015FBA4 */
        /* OSSetPeriodicAlarm(alarm, start, period, handler): EABI puts alarm in
         * r3, the two 64-bit times in r5:r6 and r7:r8, and the handler in r9.
         * This is how Melee drives HSD_PadRenewRawStatus -- fn_800195FC is
         * registered here, not as a retrace callback, which is why the pad queue
         * never filled. Capturing at entry (rather than overriding) leaves the
         * generated code untouched, so no full re-emit is needed. */
        if (addr == 0x80343A30u && ctx) {
            g_alarm_set++;
            /* When is it registered? If late in boot, few retraces remain and
             * the fire count is explained without any rate problem. */
            g_alarm_at_call = g_call_count;
            g_alarm_at_seq  = vblank_seq_rt();
            g_alarm_obj = ctx->gpr[3];
            g_alarm_handler = ctx->gpr[9];
        }
        /* Who drives the 1.34M-call poll? The census ranks functions but not
         * callers, and every stall this session was named by an LR capture. */
        if (addr == 0x800195D0u && ctx) {
            uint32_t lr = ctx->lr; int seen = 0;
            for (int k = 0; k < 8; k++) {
                if (g_lr195D0[k] == lr) { seen = 1; break; }
                if (g_lr195D0[k] == 0) { g_lr195D0[k] = lr; seen = 1; break; }
            }
            (void)seen;
        }
    }

    static int diagnostic_watches=-1;
    if(diagnostic_watches<0) diagnostic_watches=getenv("MELEE_TRACE_DIAGNOSTICS")!=NULL;
    if(diagnostic_watches) {
    {   uint32_t v = mem_r32(POLL_ADDR) ^ (mem_r32(0x80433324u) << 1)
                                        ^ (mem_r32(0x8043337Cu) << 2);
        if (g_poll_first == 0xDEADBEEFu) { g_poll_first = v; g_poll_last = v; }
        else if (v != g_poll_last) {
            g_poll_changes++;
            if (!g_poll_writer) g_poll_writer = g_last_fn;
            g_poll_last = v;
        }
    }
    if (addr == 0x80339B4Cu && !g_dgs_r13) {
        g_dgs_r13 = ctx->gpr[13];
        g_dgs_v[0] = mem_r32(g_dgs_r13 - 17392u);
        g_dgs_v[1] = mem_r32(g_dgs_r13 - 17400u);
        g_dgs_v[2] = mem_r32(g_dgs_r13 - 17416u);
    }
    /* Watch the three drive-status globals for their first writer. Guest code
     * sets them, so the last traced function at the moment of change names it
     * to function granularity -- no model of the SDK required. */
    /* Repointed at the three HSD fields the idle loop actually waits on
     * (0x80433318 + 8/12/92). Four overrides were spent on the DVD path after
     * the census had already shown these were the polled values; watching them
     * for a writer is the step that should have come first. */
    {
        static const uint32_t addrs[3] = { 0x80433320u, 0x80433324u, 0x8043337Cu };
        for (int q = 0; q < 3; q++) {
            uint32_t v = mem_r32(addrs[q]);
            if (v != g_dgs_v[q]) {
                g_dgs_v[q] = v; g_dgs_changes[q]++;
                if (!g_dgs_writer[q]) g_dgs_writer[q] = g_last_fn;
            }
        }
    }
    } /* diagnostic memory watches */
    if (addr == 0x80337DE8u) g_dvd_init++;     /* DVDInit */
    if (addr == 0x80337934u) g_dvdfs_init++;   /* __DVDFSInit */
    if (addr == 0x8033A150u) g_dvd_clearq++;   /* __DVDClearWaitingQueue */
    if (addr == 0x8001CC84u && ctx && g_lrCC84_n < 4) {
        int seen = 0;
        for (int q = 0; q < g_lrCC84_n; q++) if (g_lrCC84[q] == ctx->lr) seen = 1;
        if (!seen) g_lrCC84[g_lrCC84_n++] = ctx->lr;
    }
    if (addr == 0x800195D0u) {
        g_lm_195D0++;
        if (ctx && g_lr195_n < 16) {
            int seen = 0;
            for (int q = 0; q < g_lr195_n; q++) if (g_lr195[q] == ctx->lr) seen = 1;
            if (!seen) g_lr195[g_lr195_n++] = ctx->lr;
        }
    }    /* HSD poll loop (old host's home) */
    if (addr == 0x800192A8u) g_lm_192A8++;    /* its inner call */
    if (addr == 0x8015FDA0u) g_lm_gmMain++;   /* gmMain -- old host reached this */
    if (addr == 0x803755A8u) g_lm_HSDinit++;  /* HSD_Init */
    if (addr == 0x803883B4u) g_cb4++;   /* request #4's completion */
    if (addr == 0x80017E64u) g_cb5++;   /* request #5's completion (preload) */
    if (addr == 0x8038F6D4u) {
        g_dc_req++;
        /* HSD_DevComRequest(file, offset, dest, size, type, pri, cb, args)
         * = r3..r10. Five calls total, so print them all: the one that never
         * completes must differ from the four that do. */
        if (ctx && g_dc_req <= 6)
            printf("[dc] #%ld file=%d off=0x%X dest=0x%08X size=0x%X type=0x%X pri=%d cb=0x%08X\n",
                   g_dc_req, (int)ctx->gpr[3], ctx->gpr[4], ctx->gpr[5],
                   ctx->gpr[6], ctx->gpr[7], (int)ctx->gpr[8], ctx->gpr[9]);
    }
    if (addr == 0x8038F4F0u) g_dc_dvdwake++;   /* HSD_DevComDVDWakeUp */
    if (addr == 0x8038ECDCu) g_dc_aramwake++;  /* HSD_DevComARAMWakeUp */
    if (addr == 0x800164A4u) g_lbfile++;
    if (addr == 0x80017AB0u) g_pc_cache++;   /* lbDvd_CachePreloadedFile: 1 -> 2 */
    if (addr == 0x80017CC4u) g_pc_CC4++;
    if (addr == 0x80017E64u) g_pc_E64++;
    if (addr == 0x8030178Cu) { g_c178C++; if (!g_c178C_lr && ctx) g_c178C_lr = ctx->lr; }

    if (addr == 0x8001C658u) { g_w658++; if (!g_w658_lr) g_w658_lr = ctx ? ctx->lr : 0; }
    if (addr == 0x8001C8BCu) { g_w8BC++; if (!g_w8BC_lr) g_w8BC_lr = ctx ? ctx->lr : 0; }
    if (addr == 0x8001C600u || addr == 0x8001CE00u || addr == 0x8001D164u ||
        addr == 0x8001CAF4u || addr == 0x8001CBACu || addr == 0x8001CBBCu ||
        addr == 0x8001CC30u) g_wother++;
    if (addr == 0x8034F7DCu) g_vi_configure++;
    if (addr == 0x80350094u) g_vi_setnextfb++;
    if (addr == 0x8034FF78u) g_vi_flush++;
    if (addr == 0x80352114u) g_ar_post++;   /* ARQPostRequest */
    if (addr == 0x80350CD0u) g_ar_dma++;    /* ARStartDMA */
    if (addr == 0x80351FE0u) g_ar_isr++;    /* __ARQInterruptServiceRoutine */
    if (addr == 0x80337CF8u) g_dvd_readasync++;
    if (addr == 0x80337098u) g_dvd_lowread++;
    if (addr == 0x80339888u) g_dvd_readabs++;
    /* DVDFastOpen(entrynum, fileInfo): r3 = entry, r4 = out struct.
     * DVDConvertPathToEntrynum(path): r3 = path string.
     * Three outcomes are distinguishable here without inference -- lookup never
     * called, called and resolving -1, or resolving fine but yielding a zero
     * length. */
    if (addr == 0x8033796Cu) {
        static int n = 0;
        if (n < 6) {
            char path[64]; int k = 0;
            uint32_t pa = ctx->gpr[3];
            for (; k < 63; k++) { uint8_t c = mem_r8(pa + (uint32_t)k); if (!c) break; path[k] = (char)c; }
            path[k] = 0;
            printf("[dvd] ConvertPathToEntrynum(\"%s\")\n", path);
            /* Read the pointer and the FST head back FROM GUEST MEMORY at the
             * moment of the lookup. Pointer intact + bytes right => the walk is
             * at fault; either wrong => the seed is clobbered or misplaced. */
            {
                uint32_t fs = mem_r32(0x80000038u), fl = mem_r32(0x8000003Cu);
                uint32_t cached = 0x80000000u | (fs & 0x0FFFFFFFu);
                printf("      lomem: FST_START=0x%08X FST_MAXLEN=0x%08X\n",
                       fs, fl);
                printf("      head \n0x%08X:", cached);
                for (int q = 0; q < 12; q++) printf(" %02X", mem_r8(cached + (uint32_t)q));
                printf("   entries=%u\n", mem_r32(cached + 8u));
            }
            n++;
        }
    }
    if (addr == 0x80337C60u) {
        static int n = 0;
        if (n < 6) { printf("[dvd] FastOpen entry=%d fileInfo=0x%08X\n",
                            (int)ctx->gpr[3], ctx->gpr[4]); n++; }
    }
    /* DCFlushRange(void* start, u32 length): r3, r4. The call counter cannot
     * tell a hang from a long finite loop; the arguments can. */
    if (addr == 0x8034480Cu) {
        static int n = 0;
        if (n < 6) {
            printf("[dcf] DCFlushRange r3=0x%08X r4=0x%08X lines=%u  LR=0x%08X\n",
                   ctx->gpr[3], ctx->gpr[4], (unsigned)((ctx->gpr[4] + 31u) >> 5), ctx->lr);
            n++;
        }
    }
}

static TRACE_COLD_NOINLINE void trace_service_arq(Context* ctx)
{
        extern void arq_drain(Context*);
        uint32_t g[32];memcpy(g,ctx->gpr,sizeof g);
        uint32_t lr=ctx->lr,ctr=ctx->ctr,cr=ctx->cr,xer=ctx->xer;
        arq_drain(ctx);
        memcpy(ctx->gpr,g,sizeof g);ctx->lr=lr;ctx->ctr=ctr;ctx->cr=cr;ctx->xer=xer;
}

void trace_enter(uint32_t addr, Context* ctx)
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
        trace_service_arq(ctx);
    }
    uint32_t ee=ctx->msr&0x8000u;
    if(!netplay_simulating() && ee && !prev_ee) frontend_deliver_interrupts(ctx);
    prev_ee=ee;
    /* One address dispatch preserves the original per-address callback order. */
    extern void costumes_poll(Context*), mods_trace(uint32_t,Context*), hooks_trace(uint32_t,Context*);
    extern void frontend_audio_trace(Context*,uint32_t);
    switch (addr) {
    case 0x800236DCu: hooks_trace(addr,ctx); break;
    case 0x800686E4u: mods_trace(addr,ctx); break;
    case 0x800D0FA0u: mods_trace(addr,ctx); break;
    case 0x800EE5C0u: hooks_trace(addr,ctx); break;
    case 0x80168BF8u: mods_trace(addr,ctx); break;
    case 0x8016E934u: hooks_trace(addr,ctx); break;
    case 0x8016EBC0u: hooks_trace(addr,ctx); break;
    case 0x80177368u: mods_trace(addr,ctx); hooks_trace(addr,ctx); break;
    case 0x80177704u: mods_trace(addr,ctx); break;
    case 0x801A1E20u: hooks_trace(addr,ctx); break;
    case 0x801A43A0u: hooks_trace(addr,ctx); break;
    case 0x801AEDC8u: g_scene_tick++;if(!g_scene_lr)g_scene_lr=ctx->lr; break;
    case 0x8022DDA8u: hooks_trace(addr,ctx); break;
    case 0x8025A998u: hooks_trace(addr,ctx); break;
    case 0x8025BB5Cu: mods_trace(addr,ctx); break;
    case 0x8026688Cu: mods_trace(addr,ctx); hooks_trace(addr,ctx); break;
    case 0x80266D70u: mods_trace(addr,ctx); break;
    case 0x8035E800u: mods_trace(addr,ctx); break;
    case 0x80360950u: mods_trace(addr,ctx); break;
    case 0x80370E44u: mods_trace(addr,ctx); break;
    case 0x80375428u: mods_trace(addr,ctx); hooks_trace(addr,ctx); break;
    case 0x8037750Cu: costumes_poll(ctx); hooks_trace(addr,ctx); break;
    case 0x8037AD48u: mods_trace(addr,ctx); break;
    case 0x8038912Cu: frontend_audio_trace(ctx,addr); break;
    case 0x8038B120u: frontend_audio_trace(ctx,addr); break;
    case 0x8038B5ACu: frontend_audio_trace(ctx,addr); break;
    default: break;
    }
    if(diagnostics<0) diagnostics=getenv("MELEE_TRACE_DIAGNOSTICS")!=NULL;
    if(diagnostics) trace_diagnostics(addr,ctx);
}

/* The guest can wait for DMA in an inlined loop containing no function calls.
 * Preserve all registers as an interrupt would, and allow queued device work
 * to finish at a bounded instruction interval. Memory writes persist. */
uint32_t g_recomp_poll_budget = 4096;
void recomp_poll(Context* ctx) {
    static int active;
    extern void frontend_tick_guest_time(void);
    extern void arq_drain(Context*);
    g_recomp_poll_budget = 4096;
    { extern void netplay_progress(Context*); netplay_progress(ctx); }
    extern int netplay_async_deferred(void);
    extern int netplay_simulating(void);
    if (active || netplay_simulating() || !(ctx->msr & 0x8000u)) return;
    active = 1;
    Context saved = *ctx;
    frontend_tick_guest_time();
    frontend_deliver_interrupts(ctx);
    if (!netplay_async_deferred()) {
        { extern void frontend_service_alarms(Context*); frontend_service_alarms(ctx); }
        arq_drain(ctx);
        { extern void frontend_service_audio(Context*); frontend_service_audio(ctx); }
    }
    uint64_t now = ctx->timebase;
    *ctx = saved;
    ctx->timebase = now;
    active = 0;
}
