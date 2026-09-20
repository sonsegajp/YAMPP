#include "../controller_input.h"
/* Converted from the native-ABI original: Context is CPUState under the
 * recompcore ABI, so r[]->gpr[] and f[]->fpr[]. The HLE policy itself is
 * unchanged -- it is the part of this project GXRuntime deliberately leaves to
 * the frontend, and the part worth keeping. */
/* Game-specific HLE overrides for GALE01.
 *
 * Each function here replaces a recompiled body. The recompiler skips emitting
 * anything listed in games/gale01/game.py OVERRIDES, so the forward declaration
 * and the dispatch table both resolve to these definitions instead.
 */
#include "abi_recompcore.h"
#include "platform_compat.h"
#include "../aurora_shim/aurora_shim.h"

/* Was provided by host_rt.c, which is gone; arq_drain still needs it.
 * Re-expressed against GXRuntime's ARAM API. mm is a main-RAM address, ar an
 * ARAM address, matching the call sites in arq_drain. */
static void aram_copy(int to_mram, uint32_t mm, uint32_t ar, uint32_t len)
{
    extern Context* g_cur_ctx;
    extern void aram_dma_to_ram(unsigned char* ram, uint32_t ra, uint32_t aa, uint32_t n);
    extern void aram_dma_to_aram(const unsigned char* ram, uint32_t ra, uint32_t aa, uint32_t n);
    if (!g_cur_ctx || !len) return;
    if (to_mram) aram_dma_to_ram(g_cur_ctx->ram, mm, ar, len);
    else         aram_dma_to_aram(g_cur_ctx->ram, mm, ar, len);
}
long g_arq_hle = 0, g_arq_hle_badcb = 0;
void irq_raise(uint32_t cause);
extern long g_trace_count;
#define INT_CAUSE_DSP 0x40u
void func_800192A8(Context*);
void func_8001CC84(Context*);
/* Arguments seen by the ARQ HLE. The HLE takes source/dest straight from
 * r7/r8, so if those are zero the caller supplied them and the HLE is
 * reproducing the fault, not causing it. */
uint32_t g_arqa[8][7];
int g_arqa_n = 0;

/* Deferred ARQ completions. Small ring: the queue is never deep in practice
 * (the guest posts one chunk at a time and waits), and a fixed ring cannot
 * grow without bound if something goes wrong. */
#define ARQ_DEFER_MAX 32
static uint32_t s_defer_req[ARQ_DEFER_MAX], s_defer_cb[ARQ_DEFER_MAX];
static uint32_t s_defer_type[ARQ_DEFER_MAX], s_defer_src[ARQ_DEFER_MAX];
static uint32_t s_defer_dst[ARQ_DEFER_MAX], s_defer_len[ARQ_DEFER_MAX];
static int s_defer_head = 0, s_defer_tail = 0;
long g_arq_deferred = 0, g_arq_drained = 0, g_arq_defer_lost = 0;
static int s_draining = 0;

static void arq_defer(uint32_t req, uint32_t cb,
                      uint32_t type, uint32_t src, uint32_t dst, uint32_t len)
{
    int next = (s_defer_tail + 1) % ARQ_DEFER_MAX;
    if (next == s_defer_head) { g_arq_defer_lost++; return; }
    s_defer_req[s_defer_tail] = req;
    s_defer_cb[s_defer_tail]  = cb;
    s_defer_type[s_defer_tail] = type; s_defer_src[s_defer_tail] = src;
    s_defer_dst[s_defer_tail]  = dst;  s_defer_len[s_defer_tail] = len;
    s_defer_tail = next;
    g_arq_deferred++;
}

/* Run pending completions. Re-entrancy is refused rather than queued: a
 * callback that posts another request must not start draining from inside the
 * drain, which is the exact nesting this change exists to remove. */
/* Is the chain stalled *waiting* on a completion, or stalled on something else
 * entirely? Queue depth at exit decides it: non-empty means the drain never
 * reached it; empty means nothing was pending and ARAM is not the blocker. */
/* Deferred DVD completions, for the same reason as the ARQ ones.
 *
 * HSD_DevComDVDWakeUp posts a read and only then sets HSD_DevCom_804D77F5 = 1.
 * A synchronous DVD callback runs *inside* that post, clears F5, and returns --
 * so the poster's set lands afterwards and the flag latches at 1 forever. Every
 * later WakeUp then early-returns and DevCom stops issuing, which is exactly
 * what was measured: F5=01 at exit, 9 reads of the ~125 needed, ARQ queue empty
 * (so ARAM was never the blocker). Deferring restores the hardware ordering:
 * post, set busy, and let the completion clear it later. */
static uint32_t s_dvdq_cb[64], s_dvdq_res[64], s_dvdq_fi[64];
static int s_dvdq_head = 0, s_dvdq_tail = 0;
static uint64_t s_dvdq_due[64];
extern uint64_t frontend_guest_timebase(void);

void dvd_defer(uint32_t cb, uint32_t result, uint32_t fi)
{
    int nt = (s_dvdq_tail + 1) % 64;
    if (nt == s_dvdq_head) return;            /* full: drop rather than corrupt */
    /* Asynchronous optical read: command overhead plus conservative 3 MiB/s
     * transfer time. This permits AX to synchronize stream headers before
     * the subsequent audio data completion starts a voice. */
    s_dvdq_due[s_dvdq_tail]=frontend_guest_timebase()+24300u+(uint64_t)result*40500000u/(3u*1024u*1024u);
    s_dvdq_cb[s_dvdq_tail] = cb;
    s_dvdq_res[s_dvdq_tail] = result;
    s_dvdq_fi[s_dvdq_tail] = fi;
    s_dvdq_tail = nt;
}

long dvd_queue_depth(void)
{
    int d = s_dvdq_tail - s_dvdq_head;
    if (d < 0) d += 64;
    return (long)d;
}

long g_dvd_drained = 0;

long arq_queue_depth(void)
{
    int d = s_defer_tail - s_defer_head;
    if (d < 0) d += ARQ_DEFER_MAX;
    return (long)d;
}

void arq_drain(Context* ctx)
{
    extern int netplay_devices_deferred(void);
    if (netplay_devices_deferred()) return;
    if (s_draining || !(ctx->msr & 0x8000u)) return;
    s_draining = 1;
    Context saved_context = *ctx;
    ctx->msr &= ~0x8000u;
    for (int guard = 0; guard < ARQ_DEFER_MAX && s_defer_head != s_defer_tail; guard++) {
        uint32_t req = s_defer_req[s_defer_head];
        uint32_t cb  = s_defer_cb[s_defer_head];
        uint32_t ty  = s_defer_type[s_defer_head], sr = s_defer_src[s_defer_head];
        uint32_t ds  = s_defer_dst[s_defer_head],  ln = s_defer_len[s_defer_head];
        /* The copy happens here, not at post time: the transfer must have a
         * duration the guest can observe as in-flight. */
        if (ln) { if (ty == 0) aram_copy(0, sr, ds, ln); else aram_copy(1, ds, sr, ln); }
        s_defer_head = (s_defer_head + 1) % ARQ_DEFER_MAX;
        RecFn f = lookup_function(cb);
        if (!lookup_is_stub(f)) { ctx->gpr[3] = req; f(ctx); g_arq_drained++; }
        else   g_arq_hle_badcb++;
    }
    /* DVD completions drain from the same points, for the same reason. */
    for (int guard = 0; guard < 64 && s_dvdq_head != s_dvdq_tail; guard++) {
        if(frontend_guest_timebase()<s_dvdq_due[s_dvdq_head]) break;
        uint32_t cb = s_dvdq_cb[s_dvdq_head];
        uint32_t rs = s_dvdq_res[s_dvdq_head], fi = s_dvdq_fi[s_dvdq_head];
        s_dvdq_head = (s_dvdq_head + 1) % 64;
        RecFn f = lookup_function(cb);
        if (!lookup_is_stub(f)) { ctx->gpr[3] = rs; ctx->gpr[4] = fi; f(ctx); g_dvd_drained++; }
    }
    uint64_t advanced_time = ctx->timebase;
    *ctx = saved_context;
    ctx->timebase = advanced_time;
    s_draining = 0;
}
#include "recomp_funcs.h"
#include <stdio.h>
#include "gxruntime/dvd.h"
#include "gxruntime/aram.h"
void frontend_di_mark_complete(void);
uint32_t frontend_di_dma_address(void);
long g_hle_dvd_reads = 0;
long g_hle_cb_ran = 0, g_hle_cb_missing = 0;

/* Declared early: the poll-loop override below calls it, and this file has bitten
 * us on declaration order before. */
void frontend_deliver_interrupts(Context* ctx);

/* __VIRetraceHandler(interrupt, context) -- the real recompiled handler. */
void func_8034E964(Context*);

extern long g_vi_retrace;
void vblank_wait(void);
long vblank_seq(void);
void frontend_tick_guest_time(void);
void aram_drain_host(void);

/* VIWaitForRetrace
 *
 * The original sleeps the calling thread on the retrace queue and only returns
 * once the VI interrupt handler has bumped the global retrace count:
 *
 *     r30 = retraceCount
 *     do { OSSleepThread(&retraceQueue); } while (retraceCount == r30);
 *
 * That needs two things we do not have yet: a VI interrupt firing
 * asynchronously, and a thread scheduler able to switch stacks. Recompiled code
 * runs on the host call stack, so OSLoadContext cannot resume a different thread
 * -- its `rfi` just returns.
 *
 * So we drive the retrace synchronously instead: run the real recompiled
 * __VIRetraceHandler, which bumps the count, commits the register writes that
 * VISetNextFrameBuffer left pending (this is what finally puts a real address in
 * VI_TFBL, i.e. what makes a picture possible at all), and runs the registered
 * post-retrace callbacks. Then return, because by construction the count has
 * changed and the original loop would have exited.
 *
 * The handler takes (__OSInterrupt interrupt, OSContext* context). It uses the
 * context only to pass on to callbacks, so a null is safe here and keeps us from
 * inventing a fake exception frame.
 */
void func_8034F314(Context* ctx)
{
    /* Wait for the SDK retrace counter to advance. Only the VI interrupt
     * dispatcher invokes __VIRetraceHandler; calling it here fabricated
     * hundreds of thousands of retraces per second. */
    uint32_t previous = mem_r32(0x804D7420u);
    while (mem_r32(0x804D7420u) == previous) {
        frontend_tick_guest_time();
        frontend_deliver_interrupts(ctx);
        arq_drain(ctx);
        Sleep(1);
    }

}

/* PADRead(PADStatus status[4]) -> u32 reset-button mask
 *
 * The recompiled body drives SI, polling for a controller-ready handshake we do
 * not model, so its `err` field never leaves NOT_READY/TRANSFER. Melee's
 * db_GetGameLaunchButtonState loops on exactly that field, which is what pins
 * boot in a retrace+PADRead idle loop before the game ever starts.
 *
 * Read Aurora controllers, or an isolated diagnostic timeline. PADStatus is 12 bytes:
 *   0x00 u16 button   0x02 s8 stickX     0x03 s8 stickY
 *   0x04 s8 substickX 0x05 s8 substickY  0x06 u8 triggerL  0x07 u8 triggerR
 *   0x08 u8 analogA   0x09 u8 analogB    0x0A s8 err       0x0B u8 pad
 * err = 0 is PAD_ERR_NONE. Sticks read 0 = centred, which is what the SDK's own
 * origin-correction produces for an untouched stick.
 */
void func_8034DA00(Context* ctx)
{
    extern int aurora_link_get_pad(unsigned, AushimPadStatus*);
    extern volatile long g_frames_posted;
    extern long g_padread;
    g_padread++;
    uint32_t p=ctx->gpr[3];
    for (unsigned port=0; port<4; ++port, p+=12) {
        AushimPadStatus pad={0};
        pad.error=-1;
        const char* timeline=getenv("MELEE_INPUT");
        if (timeline) pad.error = port==0 ? 0 : -1;
        else aurora_link_get_pad(port,&pad);
        {
            const char* script=timeline;
            while (script && *script) {
                char* next;
                long first=strtol(script,&next,10);
                if (*next!=':') break;
                uint16_t buttons=(uint16_t)strtoul(next+1,&next,16);
                if (*next!=':') break;
                long length=strtol(next+1,&next,10);
                long x=0,y=0,input_port=0,cx=0,cy=0;
                if (*next==':') x=strtol(next+1,&next,10);
                if (*next==':') y=strtol(next+1,&next,10);
                if (*next==':') input_port=strtol(next+1,&next,10);
                /* Optional C-stick fields for menu-camera capture. Existing
                   timelines stop at port and keep the C-stick neutral. */
                if (*next==':') cx=strtol(next+1,&next,10);
                if (*next==':') cy=strtol(next+1,&next,10);
                if (input_port==(long)port) {
                    pad.error=0;
                    if (g_frames_posted>=first && g_frames_posted<first+length) {
                        pad.buttons|=buttons;
                        pad.stick_x=(int8_t)(x<-100?-100:x>100?100:x);
                        pad.stick_y=(int8_t)(y<-100?-100:y>100?100:y);
                        pad.cstick_x=(int8_t)(cx<-100?-100:cx>100?100:cx);
                        pad.cstick_y=(int8_t)(cy<-100?-100:cy>100?100:cy);
                    }
                }
                script=(*next==',')?next+1:NULL;
            }
        }
        {   extern void netplay_capture_pad(unsigned,const AushimPadStatus*);
            extern int netplay_lobby_open(void);
            netplay_capture_pad(port,&pad);
            if (netplay_lobby_open()) { pad.buttons=0; pad.stick_x=pad.stick_y=pad.cstick_x=pad.cstick_y=0; pad.trigger_left=pad.trigger_right=0; pad.analog_a=pad.analog_b=0; }
        }
        { extern void controller_local_input(Context*,unsigned,unsigned); controller_local_input(ctx,port,pad.buttons); }
        mem_w16(p,pad.buttons & ~YAMPP_PAD_TAP_JUMP_OFF);
        mem_w8(p+2,pad.stick_x); mem_w8(p+3,pad.stick_y);
        mem_w8(p+4,pad.cstick_x); mem_w8(p+5,pad.cstick_y);
        mem_w8(p+6,pad.trigger_left); mem_w8(p+7,pad.trigger_right);
        mem_w8(p+8,pad.analog_a); mem_w8(p+9,pad.analog_b);
        mem_w8(p+10,pad.error); mem_w8(p+11,0);
    }
    ctx->gpr[3]=0;

}

/* __write_console(handle, buffer, size_t* count, idle_proc) -> int
 *
 * The CodeWarrior stdio sink. OSReport, __assert and OSPanic all funnel through
 * it, so without this the game's own diagnostics -- including the assertion text
 * that names exactly what it is unhappy about -- are written into a void. Route
 * them to stderr. This is the cheapest possible source of truth: the program
 * tells us what is wrong in its own words.
 */
void func_80325F20(Context* ctx)
{
    uint32_t buf = ctx->gpr[4];
    uint32_t n   = ctx->gpr[5] ? mem_r32(ctx->gpr[5]) : 0;
    if (n > 4096) n = 4096;                 /* a sane bound on guest-supplied length */
    for (uint32_t i = 0; i < n; i++)
        fputc(mem_r8(buf + i), stderr);
    fflush(stderr);
    ctx->gpr[3] = 0;
}

/* __ARChecksize() -> u32 ARAM size in bytes
 *
 * The real routine discovers how much ARAM is fitted by writing marker words
 * (0xDEADBEEF / 0xBAD1BAD0) at candidate addresses and reading them back to see
 * where the address space wraps. It is ~3.8 KB of probing that depends on
 * aliasing behaviour of absent memory, and it exists to answer a question we
 * already know the answer to: our runtime allocates the ARAM, so we define its
 * size rather than discover it.
 *
 * Report the retail 16 MB. This is the same class of HLE as the EXI and DSP
 * work: stand in for a hardware handshake whose outcome we control.
 */
void func_80351010(Context* ctx)
{
    mem_w32(0x800000D0u, 0x01000000u);
    mem_w32(0x804D74B4u, 0x01000000u); /* __AR_Size: __ARChecksize is void. */
}


/* __AXOutInitDSP is deliberately NOT stubbed.
 *
 * It was, and that caused a guest fault in AXDriver_8038DCFC: the function does
 * two things, and only one of them concerns the DSP. It allocates and wires AX's
 * output buffers, then tells the DSP about them. Skipping the whole function
 * skipped the allocation too, so the driver later walked a pointer that had
 * never been set and read outside the 24 MB aperture.
 *
 * That is the third time a stub chosen for the DSP's sake broke something that
 * merely sits next to the DSP -- first the synth bank tables, then this. Letting
 * the real function run keeps its allocations honest; if it then blocks talking
 * to the absent core, the right stub is the innermost wait, not the whole
 * routine. */



/* OSLoadContext(OSContext*)
 *
 * Restores the full register set -- including r1 -- from a saved OSContext and
 * `rfi`s to it. Our `rfi` compiles to a plain return, so the restore happens but
 * the jump does not: the caller resumes with another thread's registers grafted
 * onto its own execution. When the context has never been populated, r1 becomes
 * garbage, and the ABI check caught exactly that:
 *
 *     OSLoadContext  r1 0x804EE9D8 -> 0x00000000
 *     OSLoadContext  r1 0x00000000 -> 0x30310000
 *
 * Everything downstream -- lmw restoring garbage non-volatiles, the runaway
 * strcpy, the shredded OS globals -- follows from those two lines.
 *
 * Context switching is the host scheduler's job (host/hle_thread.c), which hands
 * off between real host threads rather than swapping register files. So this is a
 * no-op: leave the running context alone. The other thread primitives are already
 * overridden for the same reason; this one was simply missed.
 */
void func_80345174(Context* ctx) { (void)ctx; }

/* The flag __AXOutInitDSP spins on after DSPAddTask: r13-0x4154, r13=0x804DB6A0. */
#define AX_TASK_STARTED_FLAG 0x804D754Cu

/* DSP handshake stubs -- kept, and now with evidence.
 *
 * These were added while the r1 corruption was live, so they were re-tested once
 * it was fixed, on the same "remove and measure" basis that deleted the
 * SFX-wait stub. Removing all three:
 *
 *     with stubs   16,634,189 guest calls, 2 disc reads
 *     without       3,716     guest calls, 0 disc reads, stuck in __DSP_debug_printf
 *
 * So unlike the SFX-wait stub, these earn their place: without them boot does not
 * get past DSP init at all. They stand in for a DSP core we do not run, and
 * porting one is what eventually replaces them.
 */
void func_80336820(Context* ctx) { ctx->gpr[3] = 0; }   /* __DSP_boot_task: started */
void func_803360CC(Context* ctx) { ctx->gpr[3] = 1; }   /* DSPCheckInit: ready      */

void func_803360D4(Context* ctx)                      /* DSPAddTask: queued       */
{
    mem_w32(AX_TASK_STARTED_FLAG, 1);
    ctx->gpr[3] = 0;
}

/* ARQPostRequest(req, owner, type, priority, source, dest, length, callback)
 *   r3   req        r4  owner   r5  type    r6  priority
 *   r7   source     r8  dest    r9  length  r10 callback
 *
 * The real routine links the request into one of two priority queues and lets
 * the ARAM interrupt drain them. That queue deadlocks here: its head at
 * r13-16856 is corrupted by a wild write (call 3761) before the second post,
 * so the poster self-links the node and nothing ever completes. DI and the ARAM
 * engine were both excluded by logging every transfer, and the poster's own
 * arguments are valid, so the queue is not the defect -- it is the victim.
 *
 * Performing the transfer and the callback inline gives the same observable
 * result without the queue, the in-flight marker or the interrupt round-trip.
 * The field offsets match the real routine's stores at 0x8035213C-0x80352158,
 * so anything that inspects the request afterwards still sees what it expects.
 *
 * Direction follows the ISR's own reading at 0x80352044: type 0 is
 * main->ARAM with (source=main, dest=ARAM); non-zero swaps the two.
 */
#if 0   /* override removed -- the recompiled ARQPostRequest is in use */
void func_80352114(Context* ctx)
{
    uint32_t req  = ctx->gpr[3];
    uint32_t type = ctx->gpr[5];
    uint32_t src  = ctx->gpr[7];
    uint32_t dst  = ctx->gpr[8];
    uint32_t len  = ctx->gpr[9];
    uint32_t cb   = ctx->gpr[10];

    if (g_arqa_n < 8) {
        g_arqa[g_arqa_n][0] = req;  g_arqa[g_arqa_n][1] = type;
        g_arqa[g_arqa_n][2] = src;  g_arqa[g_arqa_n][3] = dst;
        g_arqa[g_arqa_n][4] = len;  g_arqa[g_arqa_n][5] = cb;
        g_arqa[g_arqa_n][6] = (uint32_t)g_trace_count; g_arqa_n++;
    }
    /* This HLE writes seven words through req. devComARQR is 0x804C62A0..0x804C6320;
     * anything outside that is writing over unrelated guest state. */
    if (g_arq_hle < 6)
        printf("[arq] req=%08x type=%x src=%08x dst=%08x len=%x cb=%08x  dvdDC=%08x\n",
               req, type, src, dst, len, cb, mem_r32(0x804D77F8u));
    if (req) {
        mem_w32(req + 0x00, 0);            /* next -- never linked */
        mem_w32(req + 0x04, ctx->gpr[4]);    /* owner */
        mem_w32(req + 0x08, type);
        mem_w32(req + 0x10, src);
        mem_w32(req + 0x14, dst);
        mem_w32(req + 0x18, len);
        mem_w32(req + 0x1C, cb);
    }

    g_arq_hle++;   /* the copy is performed by arq_drain, not here */

    /* Do NOT invoke the callback here.
     *
     * On hardware ARQPostRequest returns immediately and the completion
     * callback runs later, from the ARAM interrupt. Calling it inline made the
     * whole DevCom state machine re-enter itself in one synchronous chain --
     * measured as
     *
     *   memcpy <- HSD_SynthSFXSampleLoadCallback <- HSD_DevComARAMWakeUp
     *          <- HSD_DevComDVDWakeUp <- HSD_DevComDVDARAMEndCallback
     *          <- HSD_DevComDVDCallback <- cbForReadAsync <- cbForStateBusy
     *
     * which advanced hsd_SynthSFXBank[0] a second time (0x4500 -> 0x1F7C80)
     * and made the synth.c:205 capacity check fail. Queue it and drain it from
     * a point where the guest is not already inside a completion. */
    if (cb) {
        /* Queue, then drain immediately.
         *
         * Three placements were measured. Inline and drain-at-post both let
         * the completion run at the same point in the DVD state machine, and
         * hsd_SynthSFXBank[0] double-advances (0x4500 -> 0x1F7C80), failing
         * the synth.c:205 check. Deferring to vblank or to the guest's poll
         * loop avoids that but is strictly worse: HSD_SynthSFXSampleLoadCallback
         * then never runs at all, the pending counter at 0x804D772C stays 1
         * forever, and HSD_SynthSFXWaitForLoadCompletion spins (1,340,999 poll
         * calls) on a load that can never finish.
         *
         * The byte counts settle which is closer to correct: draining here
         * reads 2,084,480 bytes against a main.ssm of 2,065,152 -- the whole
         * file -- versus 35,712 when deferred. The completion chain is
         * load-bearing, so it runs. arq_drain still refuses re-entry, which
         * flattens the recursion without suppressing it.
         *
         * The bank double-advance is therefore a separate, still-open bug and
         * not something drain placement can fix. */
        arq_defer(req, cb, type, src, dst, len);
        /* Signal ARAM DMA completion the way the hardware does.
         *
         * The completion is drained from irq_raise's tail in host_rt.c -- the
         * only point that is both prompt and outside every DevCom callback.
         * But irq_raise only runs when something raises an interrupt, and with
         * the register path bypassed nothing did, so the stream stopped after
         * three transfers. On hardware an ARAM DMA completion raises the DSP
         * interrupt; raising it here restores the drive without reintroducing
         * the inline nesting. */
        /* Do NOT raise the completion interrupt synchronously here.
         *
         * Measured: arq[0] posts at call 3712 and a synchronous raise delivered
         * its HSD_DevComARAMCallback at 3713 -- before HSD_DevComARAMWakeUp set
         * the ARAM busy byte at 0x804D77F4 at call 3725. The clear therefore ran
         * before the set, the flag latched at 1 for the rest of the run, and
         * every later WakeUp bailed at 0x8038ECF8, stopping the stream.
         *
         * A completion must never outrun the code that posted it. Leaving the
         * request queued lets the DI interrupts from the DVD reads drive
         * arq_drain from irq_raise's tail, which is after the poster has
         * returned. */
    }
}
#endif

/* VIGetRetraceCount -- two instructions on hardware:
 *     lwz r3, -16992(r13)   ; the global at 0x804D7420
 *     blr
 *
 * The HSD video layer polls this in a tight spin (measured: 47,345,449 calls)
 * and never calls VIWaitForRetrace, so the HLE that drives __VIRetraceHandler
 * from the wait path never runs and the count never advances. The spin cannot
 * end and boot stops at the first frame.
 *
 * Advancing the count here on host vblank boundaries keeps the guest's notion
 * of a retrace tied to a real 60 Hz event -- the same property the wait-path
 * HLE was careful to preserve, and for the same reason: every per-frame timer
 * in the game is counted in retraces, so a free-running count runs the whole
 * game clock at thousands of times real speed.
 *
 * The handler is what commits the pending VI_TFBL write, so this is also the
 * path that makes a framebuffer address reach the VI registers.
 */
void func_8035017C(Context* ctx)
{
    frontend_tick_guest_time();
    frontend_deliver_interrupts(ctx);
    arq_drain(ctx);
    ctx->gpr[3] = mem_r32(0x804D7420u);

}

#if 0   /* override removed from OVERRIDES; the emitter now generates this. */
/* lb_800195D0 -- the poll the guest runs while waiting for the SFX load.
 * On hardware it is just:
 *     lb_800192A8(0x8002955C);
 *     lb_8001CC84();
 * Both callees stay recompiled; this override exists only to add a drain point.
 *
 * Deferred ARQ completions need to run somewhere the guest is *outside* a
 * completion, or the DevCom state machine re-enters itself and double-advances
 * hsd_SynthSFXBank[0] (that is what caused the synth.c:205 assert). Draining at
 * the post point does not qualify even with re-entry refused -- it is still
 * inside the DVD callback. Draining only on vblank does qualify but is far too
 * slow: each completion issues the next read, so the 2 MB stream fell to 3
 * reads. This loop is both outside any completion and runs constantly
 * (measured 1,340,999 calls), which is exactly the combination needed.
 */
void func_800195D0(Context* ctx)
{
    frontend_deliver_interrupts(ctx);   /* re-enabled to capture the fault */
    /* Advance HSD's raw pad queue from here, not from PADRead.
     *
     * gm_801A4D34 waits with `if (HSD_PadGetRawQueueCount() == 0) loop`, and the
     * loop body calls only lb_800195D0 and lb_80019894 -- PADRead is never
     * reached while the guest is waiting, so a counter bump placed there never
     * ran. Same trap as the stranded ARAM completion: a host action placed in a
     * routine the guest stops calling exactly when it is blocked. */
    {
        uint8_t n = mem_r8(0x804C1F7Bu);
        if (n == 0) mem_w8(0x804C1F7Bu, 1);
    }
    /* Deliver any stranded ARAM completion here.
     *
     * Measured arm/fire pairing: arms at 364, 400, 3716, 6871; fires at 364,
     * 400, 3725. The arm at 6871 -- the streaming ARAM transfer -- never fires,
     * because every other fire site depends on guest activity (an MMIO read, or
     * a VIGetRetraceCount poll) and after 6871 the guest is blocked in THIS loop
     * waiting for exactly that completion. It touches no MMIO and polls no
     * retrace, so nothing ever drains it.
     *
     * This loop, by contrast, runs constantly while the guest waits, and runs on
     * the guest thread, so draining here is both reachable and race-free. */
    aram_drain_host();
    arq_drain(ctx);
    ctx->gpr[3] = 0x8002955Cu;
    func_800192A8(ctx);
    func_8001CC84(ctx);
}
#endif

/* __OSInitAudioSystem: the SDK's audio bring-up, which polls the DSP control
 * register at 0xCC00500A for a DSP-LLE handshake. GXRuntime runs a host-audio
 * path and states that it does not run DSP-LLE, so that handshake can never
 * complete and boot spun there: 85,403,830 reads of a constant 0x8A5 with the
 * guest frozen at 368 calls.
 *
 * Reported as succeeded. This is the same class of override as DSPCheckInit and
 * DSPAddTask, which were already needed for the same reason -- this is simply
 * the lower SDK layer they did not cover. It costs DSP-LLE audio, which the
 * host-audio path replaces, and not audio state. */
void func_80344534(Context* ctx)
{
    ctx->gpr[3] = 0;
    return;
}


/* Pump pending interrupts from the HSD poll loop. This is where the guest spends
 * its time waiting, and it is outside any handler, so it is a safe delivery
 * point -- the same property that made the old host's deferred completions safe.
 */
void func_800195D0_deliver_hook(Context* ctx) { frontend_deliver_interrupts(ctx); }

#if 0   /* kept as the record of why the lower boundary fails; the emitter
         * now generates func_80337098 itself, so linking both collides. */
/* DVDLowRead(u32 offset, u32 length, DVDLowCallback cb) -> BOOL
 *
 * The lowest layer: the actual hardware transfer. Everything above it --
 * DVDReadAsyncPrio, the command queue, the completion path and all the SDK
 * bookkeeping DVDGetDriveStatus polls -- now runs as real guest code, so that
 * state stays consistent by construction rather than by us replicating it.
 * We supply only the bytes, which is the part no runtime can infer. */
void func_80337098(Context* ctx)
{
    /* Real signature, read off the arguments themselves rather than guessed:
     *   DVDLowRead(void* addr, u32 length, u32 offset, DVDLowCallback cb)
     * The first log showed r3 = 0x804C2AC0 (a RAM destination) and
     * r5 = 0x49235A80 (the file's disc offset, matching the earlier trace), so
     * the mapping I had assumed was shifted by one register. */
    uint32_t dst = ctx->gpr[3];
    uint32_t len = ctx->gpr[4];
    uint32_t off = ctx->gpr[5];
    uint32_t cb  = ctx->gpr[6];

    uint32_t mar = frontend_di_dma_address();
    if (g_hle_dvd_reads < 6)
        printf("[low] off=0x%X len=0x%X mar=0x%08X cb=0x%08X\n",
               off, len, mar, cb);
    if (len) dvd_read_to_guest(ctx, mar, off, len);
    g_hle_dvd_reads++;
    frontend_di_mark_complete();

    /* The callback resolves to cbForStateBusy -- the SDK's DVD state machine
     * for the busy state, which takes a result code in r3. Passing 0 reads as
     * "zero bytes transferred" and stops the machine after one command, which
     * is exactly the observed dvdreads=1. Report the transferred length. */
    if (cb) { RecFn f = lookup_function(cb); if (!lookup_is_stub(f)) { ctx->gpr[3] = len; f(ctx); } }
    ctx->gpr[3] = 1;
}
#endif

/* ARQPostRequest(ARQRequest* req, u32 owner, u32 type, u32 priority,
 *                u32 source, u32 dest, u32 length, ARQCallback cb)
 * ARQRequest: next +0x00, owner +0x04, type +0x08, priority +0x0C,
 *             source +0x10, dest +0x14, length +0x18, callback +0x1C
 * type 0 = MRAM->ARAM, 1 = ARAM->MRAM. */
long g_hle_arq = 0;
void func_80352114(Context* ctx)
{
    uint32_t req = ctx->gpr[3];
    uint32_t type = ctx->gpr[5];
    uint32_t src  = ctx->gpr[7];
    uint32_t dst  = ctx->gpr[8];
    /* PPC EABI passes the first EIGHT arguments in r3-r10, and this function
     * takes exactly eight. Reading length and callback off the stack was wrong:
     * the transfers ran (transfers=2) with a garbage length and a garbage
     * callback, so nothing downstream could have advanced. */
    uint32_t len  = ctx->gpr[9];
    uint32_t cb   = ctx->gpr[10];

    mem_w32(req + 0x04u, ctx->gpr[4]);
    mem_w32(req + 0x08u, type);
    mem_w32(req + 0x0Cu, ctx->gpr[6]);
    mem_w32(req + 0x10u, src);
    mem_w32(req + 0x14u, dst);
    mem_w32(req + 0x18u, len);
    mem_w32(req + 0x1Cu, cb);

    if (len) {
        /* aram_dma_* take the RAM base pointer, a RAM address and an ARAM
         * address. type 0 = MRAM->ARAM (src is RAM, dst is ARAM). */
        if (type == 0u) aram_dma_to_aram(ctx->ram, src, dst, len);
        else            aram_dma_to_ram(ctx->ram, dst, src, len);
    }
    g_hle_arq++;

    if (cb) {
        RecFn f = lookup_function(cb);
        if (!lookup_is_stub(f)) {
            /* On hardware ARQPostRequest queues and returns; the completion runs
             * later from the ARAM interrupt, so it can never execute inside the
             * caller's frame. Running it inline here breaks that invariant, and
             * DevCom depends on it: HSD_DevComDVDCallback does
             *
             *     ARQPostRequest(..., HSD_DevComDVDStdCallback);
             *     dvdDC->src  += DEVCOM_BUF_SIZE;
             *     dvdDC->dest += DEVCOM_BUF_SIZE;
             *     dvdDC->size -= DEVCOM_BUF_SIZE;
             *
             * and dvdDC is a global (0x804D77F8). The inline callback re-enters
             * HSD_DevComDVDWakeUp, which reassigns that global and leaves it
             * NULL when nothing is issuable, so all three advances were landing
             * on guest 0x0C/0x10/0x14 -- measured -- and the transfer re-read
             * one chunk 1,187 times instead of progressing.
             *
             * Keeping the drain here (byte counts showed it is load-bearing)
             * while restoring the global reproduces what the caller would have
             * observed had the completion been deferred, which is what the
             * hardware guarantees. */
            /* Restoring dvdDC fixed the NULL stores (size now decrements on the
             * real object) but not the shape of the bug: running the completion
             * inline re-enters the whole DevCom chain from *inside*
             * ARQPostRequest, before the caller reaches its advance lines. Every
             * nested level then re-reads the same chunk -- which is why each
             * callback still began at size 0x1F3780.
             *
             * Queue it instead. The copy above has already placed the bytes, so
             * pass a zero length: arq_drain runs the callback only. The drain
             * sites in the retrace path are reachable now that interrupt
             * delivery works (238/240 frames), which was not true when deferral
             * was last measured. */
            (void)f;
            arq_defer(req, cb, 0u, 0u, 0u, 0u);
        }
    }
}


/* DVDReadAsyncPrio(DVDFileInfo* fi, void* addr, s32 len, s32 off,
 *                  DVDCallback cb, s32 prio) -> BOOL
 * DVDFileInfo: startAddr +0x30, length +0x34, callback +0x38.
 *
 * Restored after the DVDLowRead experiment: overriding here bypasses the SDK's
 * async state machine, which is exactly why it works in a frontend that cannot
 * deliver interrupt-driven completions. Measured 1,188 reads / 1,187 ARAM
 * transfers against 1 read for the lower boundary. */
void func_80337CF8(Context* ctx)
{
    uint32_t fi  = ctx->gpr[3];
    uint32_t dst = ctx->gpr[4];
    int32_t  len = (int32_t)ctx->gpr[5];
    int32_t  off = (int32_t)ctx->gpr[6];
    uint32_t cb  = ctx->gpr[7];
    uint32_t start = mem_r32(fi + 0x30u);

    /* Drain pending ARAM completions here as well as at retrace. Deferring the
     * ARQ callback is what lets the DevCom caller finish its cursor advance,
     * but the retrace drain alone left the chain starved (reads 1189 -> 4):
     * DevCom only posts more work once a completion lands, so the drain has to
     * be reachable from wherever the chain moves, not just once a frame. */
    arq_drain(ctx);

    /* Does the transfer cursor advance, or does the same region get re-read?
     * 0x1F3780 bytes in 0x4000 chunks is ~128 reads; we perform 1,189. */
    if (g_hle_dvd_reads < 12 || (g_hle_dvd_reads % 200) == 0)
        printf("[rd] #%ld off=%08x len=%x cb=%08x dvdDC=%08x src=%08x dest=%08x size=%08x f5=%02x\n",
               g_hle_dvd_reads, (uint32_t)off, (uint32_t)len, cb,
               mem_r32(0x804D77F8u),
               mem_r32(mem_r32(0x804D77F8u) + 0x10u),
               mem_r32(mem_r32(0x804D77F8u) + 0x14u),
               mem_r32(mem_r32(0x804D77F8u) + 0x18u),
               mem_r32(0x804D77F4u) & 0xFFu);
    /* dvd_read_to_guest silently drops bytes when the destination does not fit
     * inside GC_RAM_BASE + ram_size and external_write is NULL. 5,123 bytes
     * were discarded that way, starting exactly at the DevCom relay buffer. */
    if (g_hle_dvd_reads < 3)
        printf("[ctx] ram=%p ram_size=%08x ext_write=%p  dst=%08x len=%x\n",
               (void*)ctx->ram, (unsigned)ctx->ram_size,
               (void*)(size_t)ctx->external_write, dst, (unsigned)len);
    if (len > 0) dvd_read_to_guest(ctx, dst, start + (uint32_t)off, (uint32_t)len);
    g_hle_dvd_reads++;
    frontend_di_mark_complete();

    /* Complete the DVDCommandBlock the SDK's waiters poll. DVDFileInfo starts
     * with one (dvd.h): next/prev +0x00/+0x04, command +0x08, state +0x0C,
     * offset +0x10, length +0x14, addr +0x18, currTransferSize +0x1C,
     * transferredSize +0x20.
     *
     * The HLE delivered bytes and fired the callback but left every one of these
     * untouched, so anything waiting on the block saw a command that had never
     * finished -- which is what "the async state machine needs completions" has
     * meant all along, stated as named fields instead of a hypothesis. */
    mem_w32(fi + 0x0Cu, 0);                      /* state = DVD_STATE_END */
    mem_w32(fi + 0x10u, (uint32_t)off);
    mem_w32(fi + 0x14u, (uint32_t)len);
    mem_w32(fi + 0x18u, dst);
    mem_w32(fi + 0x1Cu, (uint32_t)len);          /* currTransferSize */
    mem_w32(fi + 0x20u, (uint32_t)len);          /* transferredSize */
    mem_w32(fi + 0x38u, cb);
    if (cb) {
        RecFn f = lookup_function(cb);
        if (!lookup_is_stub(f)) { dvd_defer(cb, (uint32_t)len, fi); g_hle_cb_ran++; }
        else   { g_hle_cb_missing++; }
    }
    if (0) {
        RecFn f = lookup_function(cb);
        if (f) {
            /* Does the callback advance the transfer, or not? Sample the
             * object it is documented to mutate, on both sides of the call. */
            uint32_t dc0 = mem_r32(0x804D77F8u);
            uint32_t sz0 = dc0 ? mem_r32(dc0 + 0x14u) : 0;
            ctx->gpr[3] = (uint32_t)len; ctx->gpr[4] = fi; f(ctx); g_hle_cb_ran++;
            {   uint32_t dc1 = mem_r32(0x804D77F8u);
                /* Read back through dc0, not dc1: the global is cleared by the
                 * nested WakeUp, so dc1 is NULL and says nothing about size. */
                uint32_t sz1 = dc0 ? mem_r32(dc0 + 0x14u) : 0;
                uint32_t dst1 = dc0 ? mem_r32(dc0 + 0x10u) : 0;
                uint32_t ty1  = dc0 ? mem_r32(dc0 + 0x18u) : 0;
                if (g_hle_cb_ran < 8)
                    printf("[cb] dc %08x->%08x  size %08x->%08x  dest=%08x tyword=%08x  lowmem[0C/10/14]=%08x/%08x/%08x\n",
                           dc0, dc1, sz0, sz1, dst1, ty1,
                           mem_r32(0x0Cu), mem_r32(0x10u), mem_r32(0x14u));
            }
        }
        else   { g_hle_cb_missing++; }
    }
    ctx->gpr[3] = 1;
}

/* DVDGetDriveStatus() -> s32
 * DVD_STATE_END (0) = no command in progress. True by construction here: the
 * DVDReadAsyncPrio HLE completes every transfer before returning, so there is
 * never an outstanding command for the guest to observe. */
void func_80339B4C(Context* ctx) { ctx->gpr[3] = 0; }

/* HSD_SynthSFXWaitForLoadCompletion()
 *
 * Returns immediately. The transfers this waits on are performed synchronously
 * by the DVD and ARQ HLEs, so the data is in place before this is reached; the
 * wait exists only to block until an async completion arrives, and this
 * frontend has no way to deliver one. Boot spun here 21.7M times. */
void func_80388B0C(Context* ctx) { ctx->gpr[3] = 0; }

#if 0   /* reverted: wedged the frame loop (gmMain 239 -> 1). */
/* lbDvd_800189EC: Melee's own DVD wait wrapper, the sole driver of the spin.
 * Returns success -- the transfers it waits on are already complete, performed
 * synchronously by the DVD/ARQ HLEs before this is reached. */
void func_800189EC(Context* ctx) { ctx->gpr[3] = 0; }
#endif

/* Guest-thread snapshot; no handles or host call stacks are serialized. */
size_t frontend_io_snapshot(void* data, int restore) {
  unsigned char* bytes = data; size_t offset = 0;
#define RB_COPY(field) do { if (bytes) { if (restore) memcpy(&(field), bytes + offset, sizeof(field)); else memcpy(bytes + offset, &(field), sizeof(field)); } offset += sizeof(field); } while (0)
  RB_COPY(s_defer_req);
  RB_COPY(s_defer_cb);
  RB_COPY(s_defer_type);
  RB_COPY(s_defer_src);
  RB_COPY(s_defer_dst);
  RB_COPY(s_defer_len);
  RB_COPY(s_defer_head);
  RB_COPY(s_defer_tail);
  RB_COPY(s_dvdq_cb);
  RB_COPY(s_dvdq_res);
  RB_COPY(s_dvdq_fi);
  RB_COPY(s_dvdq_head);
  RB_COPY(s_dvdq_tail);
  RB_COPY(s_dvdq_due);
#undef RB_COPY
  return offset;
}
