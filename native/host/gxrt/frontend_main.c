/* Melee frontend for GXRuntime.
 *
 * Replaces host/host_rt.c + host/sdl_host.c. The device model, GX command
 * parser, software rasteriser and XFB decoder that lived there are GXRuntime's
 * job; what remains here is Melee-specific policy and the vblank/ARAM seam the
 * HLE expects.
 */
#include "platform_compat.h"
#include <gxruntime/gx_recomp.h>
#include "abi_recompcore.h"
#include "gxruntime/boot.h"
#include "gxruntime/loader.h"
#include "gxruntime/interrupts.h"
#include "gxruntime/vi_clock.h"
#include "gxruntime/headless_backend.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gxruntime/exi.h"
#include "gxruntime/yuyv_decode.h"
#include "gxruntime/dvd.h"

static CPUState       s_cpu;
extern DolInterrupts  g_irq;      /* owned by frontend_bus.c */
void frontend_bus_init(CPUState* cpu);
void frontend_bus_report(void);
static DolViClock     s_viclk;
static DolLayout      s_layout;
static RecFn          s_entry;
extern uint32_t g_last_fn; extern long g_call_count; extern long g_distinct_fns;
extern long g_dvd_readasync, g_dvd_lowread, g_dvd_readabs;
extern long g_os_sleeps, g_os_wakes;
extern long g_hle_dvd_reads;
extern long g_ar_post, g_ar_dma, g_ar_isr;
extern long g_hle_arq;
extern long g_vi_configure, g_vi_setnextfb, g_vi_flush;
extern unsigned g_dgs_r13, g_dgs_v[3], g_dgs_writer[3];
extern long g_dgs_changes[3];
extern long g_dvd_init, g_dvdfs_init, g_dvd_clearq;
extern long g_pc_cache, g_pc_CC4, g_pc_E64;
extern long g_lbfile;
extern long g_dc_req, g_dc_dvdwake, g_dc_aramwake;
extern long g_cb4, g_cb5;
extern long g_hle_cb_ran, g_hle_cb_missing;
extern long g_w658, g_w8BC, g_wother;
extern long g_scene_tick; extern unsigned g_scene_lr;
extern long g_c178C; extern unsigned g_c178C_lr;
extern long g_lm_195D0, g_lm_192A8, g_lm_gmMain, g_lm_HSDinit;
extern unsigned g_lr195[16]; extern int g_lr195_n;
extern unsigned g_lrCC84[4]; extern int g_lrCC84_n;
extern unsigned g_w658_lr, g_w8BC_lr;
extern unsigned g_poll_first, g_poll_last, g_poll_writer; extern long g_poll_changes;

extern DolInterrupts g_irq;
long g_irq_delivered = 0;
/* Frontend-owned boot OSThread. Seeding OS_CURRENT_THREAD (0x800000E4) alone was
 * fragile: the SDK owns that global and OSInit overwrites it, so delivery
 * worked in one run and not the next depending on when the edge landed. Keeping
 * our own reference makes the fallback deterministic instead of timing-dependent
 * -- this seeding has silently broken twice. */
uint32_t g_boot_osthread = 0;
/* Sticky VI latch. The host frame loop asserts retrace on its own thread; the
 * guest checks for pending interrupts on the guest thread's MSR[EE] edges. The
 * two are unsynchronised, so the device's cause bit was consumed or overwritten
 * before any edge observed it (measured: cause & mask == 0 at every edge).
 * A latch cannot be missed -- the same "latch rather than drop" fix this project
 * already needed once, for masked interrupts on the old host. */
volatile long g_vi_latch = 0;
long g_pe_delivered = 0;
/* One counter per exit. "delivered=0" has been ambiguous for three runs; naming
 * the exit turns it into a fact. */
long g_x_nopend = 0, g_x_noee = 0, g_x_noctx = 0, g_x_nocause = 0, g_x_nofn = 0;
uint32_t frontend_current_osthread(void);

/* Invoke a guest interrupt handler with (interrupt number, OSContext*). */
static void ctx_deliver(CPUState* cpu, RecFn fn, uint32_t which, uint32_t osctx)
{
    CPUState saved = *cpu;
    cpu->msr &= ~0x8000u;
    cpu->gpr[3] = which;
    cpu->gpr[4] = osctx;
    fn(cpu);
    uint64_t advanced_tb = cpu->timebase;
    *cpu = saved;
    cpu->timebase = advanced_tb;

}
long g_skip_nopend = 0, g_skip_noee = 0;

/* Host fault reporter. The old standalone host had this and the GXRuntime
 * frontend did not, which is why two ticks of crash analysis produced narrative
 * instead of a location: the guest call stack was being read as a crash
 * signature while the actual faulting address was never captured. */
static volatile LONG s_term_requested;
#ifdef _WIN32
static LONG CALLBACK fe_veh(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    {
        HMODULE owner=NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)ep->ExceptionRecord->ExceptionAddress,&owner);
        char module[MAX_PATH]={0};if(owner)GetModuleFileNameA(owner,module,sizeof module);
        fprintf(stderr,"[fault] module=%s\n",module);
        char* base = (char*)owner;
        char* pc   = (char*)ep->ExceptionRecord->ExceptionAddress;
        fprintf(stderr, "\n[fault] code=0x%08lX host_pc=%p base=%p rva=0x%llX\n",
                code, (void*)pc, (void*)base,
                (unsigned long long)(pc - base));
    }
    if (code == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, "[fault] %s address 0x%p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    fprintf(stderr, "[fault] guest last_fn=0x%08X calls=%ld irq_delivered=%ld\n",
            g_last_fn, g_call_count, g_irq_delivered);
    fprintf(stderr, "[fault] g_cur_ctx=%p  expected=%p  tid=%lu\n",
            (void*)g_cur_ctx, (void*)&s_cpu, GetCurrentThreadId());
    CONTEXT trace = *ep->ContextRecord;
    for (unsigned i=0; i<24 && trace.Rip; ++i) {
        DWORD64 base=0; PRUNTIME_FUNCTION fn=RtlLookupFunctionEntry(trace.Rip,&base,NULL);
        HMODULE module=NULL;char name[MAX_PATH]={0};
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)trace.Rip,&module);
        if(module)GetModuleFileNameA(module,name,sizeof name);
        fprintf(stderr,"[fault-stack] %s+0x%llx\n",name,(unsigned long long)(trace.Rip-(uintptr_t)module));
        if(!fn) { trace.Rip=*(DWORD64*)trace.Rsp;trace.Rsp+=8; }
        else { PVOID data;DWORD64 frame;RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,trace.Rip,fn,&trace,&data,&frame,NULL); }
    }
    fflush(stderr);
    ExitProcess(1);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void fe_signal_handler(int sig, siginfo_t* info, void* ucontext)
{
    (void)ucontext; (void)info; (void)sig;
    const char msg[] = "\n[fault] signal received\n";
    (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
    _exit(1);
}
static void fe_term_handler(int sig) {
    if (s_term_requested) _exit(8);
    InterlockedExchange(&s_term_requested, 1);
    (void)sig;
}
static void fe_alarm_handler(int sig) { (void)sig; _exit(8); }
#endif
/* Deliver pending external interrupts to the guest.
 *
 * GXRuntime tracks pending causes and the PI mask correctly, but nothing ever
 * consumed that state -- dol_interrupts_external_pending is defined and called
 * by nobody -- so DI completion, VI retrace and PE finish were all recorded and
 * then dropped. The guest had never taken an interrupt.
 *
 * Rather than synthesise an exception frame, call the SDK's own dispatcher
 * (__OSDispatchInterrupt, 0x8034783C). It is real guest code that reads PI
 * cause/mask itself and invokes the registered handler, which is exactly what
 * the 0x500 vector reaches on hardware. Runs on the guest thread, so the
 * handler sees a coherent CPU state.
 */
void frontend_deliver_interrupts(CPUState* cpu)
{
    extern int netplay_simulating(void);
    if (netplay_simulating()) return;
    extern DolExi g_exi;
    dol_interrupts_set_source(&g_irq,DOL_PI_CAUSE_EXI,dol_exi_interrupt_pending(&g_exi));
    /* Count the two early-exit reasons separately. irq_delivered=0 says
     * delivery never fires but not which gate closed it -- pending state or
     * MSR[EE]. One counter each answers that without another guess. */
    if (!g_vi_latch && !dol_interrupts_external_pending(&g_irq)) {
        /* Sample cause/mask alongside the pending verdict AT the edge. If
         * cause & mask is non-zero here while pending is false, the API applies
         * a further condition; if it is zero, the cause is transient between
         * edges. Two different next steps, so measure rather than assume. */
        static int shown = 0;
        if (shown < 4) {
            printf("[edge] pending=0 cause=0x%X mask=0x%X and=0x%X\n",
                   dol_interrupts_pi_cause(&g_irq), dol_interrupts_pi_mask(&g_irq),
                   dol_interrupts_pi_cause(&g_irq) & dol_interrupts_pi_mask(&g_irq));
            shown++;
        }
        g_skip_nopend++; return;
    }
    if ((cpu->msr & 0x8000u) == 0u) { g_skip_noee++; return; }
    /* Dispatch by pending cause, not one hardcoded handler.
     *
     * The previous version always called __DVDInterruptHandler, but DI (0x04)
     * is never raised: DVDReadAsyncPrio is HLE'd, so di_execute never runs. The
     * cause actually pending is VI (0x100), asserted by our own frame loop.
     * Delivering the handler that matches the cause is the whole point. */
    /* Prefer the SDK's own OS_CURRENT_THREAD global (0x800000E4) over our
     * thread table. Boot runs on the frontend's initial host thread, not on an
     * OSThread created through our HLE, so the table has no current entry and
     * yielded 0 -- delivery never got past the guard. The SDK maintains this
     * global from OSInit onward, and OSThread begins with an OSContext, so it is
     * valid exactly when handlers need it. */
    uint32_t osctx = mem_read32(cpu, 0x800000E4u);      /* SDK's, once it owns one */
    if (!osctx) osctx = frontend_current_osthread();    /* a created guest thread */
    if (!osctx) osctx = g_boot_osthread;                /* ours, always valid */
    if (!osctx) { g_x_noctx++; return; }
    uint32_t cause = dol_interrupts_pi_cause(&g_irq) & dol_interrupts_pi_mask(&g_irq);
    if (g_vi_latch) { cause |= 0x100u; g_vi_latch = 0; }   /* consume the latch */

    /* PE finish first. The VI branch returns as soon as it delivers, so a
     * cause checked after it only lands on frames where VI is absent -- and
     * this is the one the guest is actually blocked on (GXWaitDrawDone ->
     * OSSleepThread(&FinishQueue)). __OS_INTERRUPT_PI_PE_FINISH = 19. */
    if (cause & 0x400u) {                      /* PE draw-done */
        static RecFn pe_h = 0;
        if (!pe_h) pe_h = lookup_function(0x8033CF4Cu);   /* GXFinishInterruptHandler */
        if (!lookup_is_stub(pe_h)) {
            dol_interrupts_set_source(&g_irq, 0x400u, false);
            g_irq_delivered++;
            g_pe_delivered++;
            ctx_deliver(cpu, pe_h, 19u, osctx);
            return;
        }
    }
    if (cause & 0x100u) {                      /* VI retrace */
        static RecFn vi_h = 0;
        if (!vi_h) vi_h = lookup_function(0x8034E964u);   /* __VIRetraceHandler */
        if (!lookup_is_stub(vi_h)) {
            dol_interrupts_set_source(&g_irq, 0x100u, false);
            g_irq_delivered++;
            ctx_deliver(cpu, vi_h, 24u, osctx);           /* __OS_INTERRUPT_PI_VI */
            return;
        }
    }
    if (cause & DOL_PI_CAUSE_EXI) {
        RecFn handler=lookup_function(0x8034783Cu); /* original OS interrupt dispatcher */
        if(!lookup_is_stub(handler)){g_irq_delivered++;ctx_deliver(cpu,handler,4u,osctx);return;}
    }
    if (!cause) { g_x_nocause++; return; }
    if (cause & 0x004u) {                      /* DI transfer complete */
        static RecFn di_h = 0;
        if (!di_h) di_h = lookup_function(0x80336B20u);   /* __DVDInterruptHandler */
        if (!lookup_is_stub(di_h)) {
            dol_interrupts_set_source(&g_irq, 0x004u, false);
            g_irq_delivered++;
            ctx_deliver(cpu, di_h, 21u, osctx);           /* __OS_INTERRUPT_PI_DI */
            return;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI guest_thread(LPVOID p)
#else
static void* guest_thread(void* p)
#endif
{
    (void)p;
    g_cur_ctx = &s_cpu;
    s_entry(&s_cpu);
    printf("[guest] entry returned\n");
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

long g_vi_retrace = 0;
static long s_vblank_seq = 0;

/* ---- the vblank / ARAM seam the HLE policy calls -------------------------
 * Previously these reached into our own device model. GXRuntime owns the
 * hardware now, so they drive its VI clock and interrupt controller instead:
 * the HLE still decides *when* Melee's SDK needs a retrace, GXRuntime decides
 * what a retrace actually is. */

void vblank_wait(void)
{
    /* Deliberately does NOT raise anything.
     *
     * Retraces used to be raised here, on the host thread, while the guest
     * thread read and cleared the same interrupt state from trace_enter with no
     * synchronisation. Three runs of one unchanged binary gave distinct=467,
     * 471 and 644 -- a spread wider than every improvement measured this
     * session, which made single-run measurement worthless.
     *
     * Retraces are now driven by guest progress (see frontend_tick_guest_time),
     * so the whole interrupt timeline is a pure function of executed guest
     * code and repeats exactly. The host loop only paces wall-clock. */
}

/* Called from the guest thread. One retrace per fixed slice of guest progress:
 * same execution, same retrace points, same delivery order, every run. */
/* Frames the host has posted, and frames the guest has consumed. The host owns
 * the *rate* (a real 60 Hz), the guest owns *when* it observes them, so a guest
 * spinning in a retrace wait can always make progress without inventing its own
 * clock -- which is what produced 808,177 retraces in 120 frames. */
volatile long g_frames_posted = 0;
long g_copies_applied = 0;
u32 g_last_copy_dest = 0, g_last_copy_w = 0, g_last_copy_h = 0;
static u32 s_trace_seen = 0;
static   long g_frames_taken  = 0;
/* After a netplay stall the guest must not replay every missed retrace at once. */
void frontend_drop_frame_backlog(void) { g_frames_taken = g_frames_posted; }

/* One hardware timebase shared by every guest thread, with subframe precision.
 * DVD and AX completions must interleave while the game waits between retraces. */
volatile LONG g_guest_stopping;
void frontend_guest_checkpoint(void) {
    if(g_guest_stopping) {
#ifdef _WIN32
        ExitThread(0);
#else
        pthread_exit(NULL);
#endif
    }
}
uint64_t frontend_guest_timebase(void) {
    extern int netplay_simulating(void);
    extern int netplay_clock(uint64_t*);
    uint64_t rollback_now;
    if (netplay_clock(&rollback_now)) {
        if (g_cur_ctx) g_cur_ctx->timebase = rollback_now;
        return rollback_now;
    }
    if (netplay_simulating() && g_cur_ctx) return g_cur_ctx->timebase;
    frontend_guest_checkpoint();
    static LARGE_INTEGER origin,frequency;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency); origin=now;
    }
    uint64_t ticks=(uint64_t)(now.QuadPart-origin.QuadPart)*40500000u/frequency.QuadPart;
    s_cpu.timebase=ticks;
    if(g_cur_ctx) g_cur_ctx->timebase=ticks;
    return ticks;
}
void frontend_tick_guest_time(void)
{
    extern int netplay_simulating(void);
    if (netplay_simulating()) return;
    { extern void frontend_card_poll(void); extern int netplay_async_deferred(void); if (!netplay_async_deferred()) frontend_card_poll(); }
    frontend_guest_timebase();
    if (g_frames_taken >= g_frames_posted) return;   /* no frame due yet */
    g_frames_taken++;
    u64 per = dol_vi_clock_timebase_ticks_per_retrace(&s_viclk);
    /* Advance the guest timebase.
     *
     * Nothing in the tree ever wrote s_cpu.timebase, so every guest mftb
     * returned 0 and no elapsed-time test could ever pass. That is what the
     * halt was: __EXIProbe (via the pad alarm -> lbSnap_8001D2BC -> CARDProbe
     * -> EXIProbe) debounces the memory-card probe against OSGetTime and spun
     * forever on a clock that never moved. Anything else waiting on time --
     * alarms, DVD timeouts, probe settling -- was equally stuck. */
    /* The shared hardware clock advances independently of retraces. */
    dol_vi_clock_advance(&s_viclk, per);
    u64 tb_ticks = 0;
    if (dol_vi_clock_pop_retrace(&s_viclk, &tb_ticks)) {
        dol_interrupts_assert_vi_retrace(&g_irq);
        g_vi_latch = 1;
        {   /* Only when the guest requested it -- see module_rt.c. */
            extern volatile long g_drawdone_pending;
            if (g_drawdone_pending) {
                g_drawdone_pending = 0;
                /* Parse the frame's FIFO before signalling draw-done.
                 *
                 * dol_gx_recomp_push_fifo only buffers bytes and emits
                 * FIFO_BYTES events -- replay_stream, the actual command
                 * parser, runs only from dol_gx_recomp_replay_fifo, which
                 * nothing called. That is why 1.7 MB of GX traffic produced
                 * 8,192 trace events of which draw=0, copy_dest=0, bp_reg=0:
                 * the bytes were being collected and never decoded. */
                { extern DolGxRecompState g_gx;
                  extern int aurora_link_call_dl(const void*, u32);
                  u32 n = dol_gx_recomp_fifo_size(&g_gx);
                  const u8* fifo = dol_gx_recomp_fifo_bytes(&g_gx);
                  if (n) {
                      dol_gx_recomp_replay_fifo(&g_gx, fifo, n);   /* diagnostics */
                      /* Submission moved out of here.
                         * These display lists must be issued between
                         * aurora_begin_frame and aurora_end_frame; this
                         * function runs from the host loop AND from guest
                         * threads, at points with no active render pass. The
                         * same bug was fixed once tonight for GXSetArray and
                         * missed here. The host loop now calls
                         * aurora_submit_frame() inside the frame. */
                        (void)0;
                  } }
                /* Consume the EFB copies. gx_recomp reports them; it has no
                 * rasteriser and never writes guest memory, so the XFB stayed
                 * all-zero (decoding to uniform green) even with the parser on.
                 * Honour the clear case first: it is the whole-screen fill the
                 * SDK issues before drawing, and it proves the consumer path
                 * end to end without any shading. */
                { extern DolGxRecompState g_gx;
                  for (u32 k = s_trace_seen; k < g_gx.trace_count; k++) {
                      const DolGxRecompTraceEvent* ev = &g_gx.trace[k];
                      if (ev->kind != DOL_GX_RECOMP_EVENT_COPY_DESTINATION) continue;
                      if (!ev->d) continue;                  /* d = clear flag */
                      u32 cw = ev->g >> 16, ch = ev->g & 0xFFFFu;
                      if (!cw || !ch) continue;
                      u32 ar = g_gx.bp_valid[0x4Fu] ? g_gx.bp_regs[0x4Fu] : 0;
                      u32 gb = g_gx.bp_valid[0x50u] ? g_gx.bp_regs[0x50u] : 0;
                      int R = (int)(ar & 0xFFu), G = (int)((gb >> 8) & 0xFFu), B = (int)(gb & 0xFFu);
                      int Y = (( 66*R + 129*G +  25*B + 128) >> 8) + 16;
                      int U = ((-38*R -  74*G + 112*B + 128) >> 8) + 128;
                      int V = ((112*R -  94*G -  18*B + 128) >> 8) + 128;
                      if (Y < 0) Y = 0; if (Y > 255) Y = 255;
                      if (U < 0) U = 0; if (U > 255) U = 255;
                      if (V < 0) V = 0; if (V > 255) V = 255;
                      u32 base = 0x80000000u | (ev->a & 0x0FFFFFFFu);
                      for (u32 y = 0; y < ch; y++)
                          for (u32 x = 0; x < cw; x += 2) {
                              u32 ea = base + (y * cw + x) * 2u;
                              mem_write8(&s_cpu, ea + 0, (u8)Y);
                              mem_write8(&s_cpu, ea + 1, (u8)U);
                              mem_write8(&s_cpu, ea + 2, (u8)Y);
                              mem_write8(&s_cpu, ea + 3, (u8)V);
                          }
                      g_copies_applied++;
                      g_last_copy_dest = base;   /* what the capture should sample */
                      g_last_copy_w = cw; g_last_copy_h = ch;
                  }
                  /* Do NOT reset trace_count: gx_recomp writes a copy's extra
                   * fields via trace[trace_count-1], so zeroing it makes the
                   * next event index trace[-1] and corrupt adjacent state --
                   * measured as a fresh OSPanic/PPCHalt at 7,594 calls.
                   * Consume from a high-water mark instead. */
                  s_trace_seen = g_gx.trace_count; }
                dol_interrupts_commit_pe_finish(&g_irq);
            }
        }
        s_vblank_seq++;
        /* Release anyone blocked in the retrace wait. This has to happen where
         * the retrace pops, not inside the wait itself: a sleeping waiter
         * cannot wake waiters, so waking from there was circular and the guest
         * saw 2 retraces in 240 frames. */
        { extern void hle_thread_wake_retrace(void); hle_thread_wake_retrace(); }
    }
}

long vblank_seq(void) { return s_vblank_seq; }
long vblank_seq_rt(void) { return s_vblank_seq; }

/* Wall-clock sampling profiler.
 *
 * The call census answers "which function is entered most", which is not the
 * same question as "where does the time go" -- and the guest now executes 60x
 * fewer calls while runs take minutes, so the two answers must differ. Sample
 * g_last_fn on a timer: whatever the guest is stuck inside will dominate the
 * samples even if it is entered once. */
static unsigned int s_samp[0x10000];
static uint32_t     s_sampaddr[0x10000];
static volatile int s_sampling = 1;
long g_samples = 0;
#ifdef _WIN32
static DWORD WINAPI sampler_thread(LPVOID p2)
#else
static void* sampler_thread(void* p2)
#endif
{
    (void)p2;
    /* Optional match-only samples keep loading/menu waits out of CPU profiles.
     * Collection remains observational; no guest timing or logic is changed. */
    const char* first_text = getenv("MELEE_SAMPLE_START");
    const char* last_text = getenv("MELEE_SAMPLE_END");
    long first = first_text ? strtol(first_text, NULL, 10) : 0;
    long last = last_text ? strtol(last_text, NULL, 10) : 0;
    while (s_sampling) {
        long frame = g_frames_posted;
        if (frame < first || (last > 0 && frame > last)) { Sleep(1); continue; }
        uint32_t a = g_last_fn;
        unsigned slot = (a >> 2) & 0xFFFFu;
        s_samp[slot]++; s_sampaddr[slot] = a; g_samples++;
        Sleep(1);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
void sampler_report(void)
{
    s_sampling = 0;
    const char* path = getenv("MELEE_SAMPLE_PATH");
    const char* top_text = getenv("MELEE_SAMPLE_TOP");
    int top = top_text ? atoi(top_text) : 15;
    if (top < 1) top = 1;
    if (top > 1024) top = 1024;
    FILE* f = fopen(path && *path ? path : "build/prof.txt", "w");
    if (!f) return;
    fprintf(f, "wall-clock samples=%ld\n", g_samples);
    for (int rank = 0; rank < top; rank++) {
        unsigned best = 0; int bi = -1;
        for (int i = 0; i < 0x10000; i++) if (s_samp[i] > best) { best = s_samp[i]; bi = i; }
        if (bi < 0 || !best) break;
        fprintf(f, "%2d. %08X  %u samples (%.1f%%)\n", rank + 1, s_sampaddr[bi], best,
                g_samples ? 100.0 * best / (double)g_samples : 0.0);
        s_samp[bi] = 0;
    }
    fclose(f);
}

/* ARAM/DI completion draining was our own deferral machinery, built to stop
 * completions running inside an interrupt handler. GXRuntime's ARAM and DI
 * devices own that ordering, so this is now only the PE-finish commit the HSD
 * render path waits on -- the one piece of that work that was policy, not
 * device emulation. */
void aram_drain_host(void) { dol_interrupts_commit_pe_finish(&g_irq); }

/* Everything aurora needs for one frame, in order, inside the frame. */

/* Dump aurora's render target right after a completed frame. Doing this after
 * the loop caught nothing: the target is presented and reused, so by then it is
 * blank regardless of what was drawn. */
void fe_dump_aurora(void)
{
    extern int aurora_link_readback(unsigned char*, u32*, u32*, u32);
    static unsigned char rb[1280 * 960 * 3];
    static u32 best_nonzero = 0;
    u32 rw = 0, rh = 0;
    if (aurora_link_readback(rb, &rw, &rh, (u32)sizeof rb) && rw && rh) {
        const char* capture_dir = getenv("MELEE_CAPTURE_DIR");
        static int capture_interval;
        if (!capture_interval) { const char* v=getenv("MELEE_CAPTURE_INTERVAL");capture_interval=v?atoi(v):30;if(capture_interval<1)capture_interval=30; }
        if (capture_dir && g_frames_posted % capture_interval == 0) {
            char name[1024];
            snprintf(name, sizeof name, "%s/frame_%06ld.ppm", capture_dir, g_frames_posted);
            FILE* frame = fopen(name, "wb");
            if (frame) {
                fprintf(frame, "P6\n%u %u\n255\n", rw, rh);
                fwrite(rb, 1, (size_t)rw * rh * 3u, frame);
                fclose(frame);
            }
        }
        u32 i, nz = 0;
        for (i = 0; i < rw * rh * 3u; i++) if (rb[i]) nz++;
        if (nz > best_nonzero) {
            FILE* af = fopen("build/frame_aurora_best.ppm", "wb");
            best_nonzero = nz;
            if (af) {
                fprintf(af, "P6\n%u %u\n255\n", rw, rh);
                fwrite(rb, 1, (size_t)rw * rh * 3u, af);
                fclose(af);
            }
            printf("[rb] new best %ux%u nonzero_bytes=%u\n", rw, rh, nz);
            fflush(stdout);
        }
    }
}

int aurora_submit_frame(void)
{
    extern int aurora_link_call_dl_translated(const u8*,u32,u8*,u32);
    extern const u8* wgseg_take(u32*);
    u32 length=0;
    const u8* data=wgseg_take(&length);
    if (!data) return 0; /* No frame ownership: the guest may still be drawing. */
    if (!length) { extern void costume_art_frame_done(Context*); extern void wgseg_release(void); costume_art_frame_done(&s_cpu); wgseg_release(); return 0; }
    if (!aurora_link_call_dl_translated(data,length,NULL,s_cpu.ram_size)) {
        fprintf(stderr,"Failed to translate Melee frame (%u bytes)\\n",length);
        ExitProcess(5);
    }
    return 1;
}

int main(int argc, char** argv)
{
    /* Unpacking a disc image, which the first-run setup cannot do itself for a
       compressed one: melee-pc --extract <image> <folder> */
    if (argc > 3 && strcmp(argv[1], "--extract") == 0) {
        extern int extract_disc_image(const char*, const char*);
        return extract_disc_image(argv[2], argv[3]);
    }
    const int hosted = getenv("MELEE_RUNTIME_HOSTED") != NULL;
    const char* dol = (argc > 1) ? argv[1]
                    : "data/GALE01/sys/main.dol";
    int frames = (argc > 2) ? atoi(argv[2]) : 0;

    setvbuf(stdout, NULL, _IONBF, 0);

#ifdef _WIN32
    /* Dynamic guest execution commits its code cache with a first-chance VEH.
     * Report only unhandled faults in that mode so its own handler can run. */
    PVOID fault_handler = NULL;
    LPTOP_LEVEL_EXCEPTION_FILTER previous_fault = NULL;
    if (getenv("MELEE_MEX_BASE_DOL") || hosted) previous_fault = SetUnhandledExceptionFilter(fe_veh);
    else fault_handler = AddVectoredExceptionHandler(1, fe_veh);
#else
    {   struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = fe_signal_handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
    }
    signal(SIGTERM, fe_term_handler);
    signal(SIGINT, fe_term_handler);
#endif
    g_trace = 1;   /* frontier tracking */
    if (!cpu_init(&s_cpu)) { fprintf(stderr, "cpu_init failed\n"); return 2; }
    frontend_bus_init(&s_cpu);   /* devices + MMIO bus + CPU callbacks */
    dol_vi_clock_init(&s_viclk);

    static DolHeadlessBackend backend;
    dol_headless_backend_init(&backend);
    dol_headless_backend_install(&backend);

    if (!dol_load_into_ram(&s_cpu, dol, &s_layout)) {
        fprintf(stderr, "dol_load_into_ram failed: %s\n", dol);
        return 2;
    }
    /* Mount the disc. Without it the first real file read asserts --
     * "DVDReadAsync(): specified area is out of the file" -- and the SDK calls
     * PPCHalt. GXRuntime's dvd.c owns FST parsing; the frontend only supplies
     * the image. */
    {
        const char* disc = getenv("MELEE_DISC");
        if (!disc || !*disc || !dvd_open_image(disc)) {
#ifdef _WIN32
            fprintf(stderr, "Launch with Run-Melee.cmd to select and validate the ISO from XML configuration.\n");
#else
            fprintf(stderr, "Launch with sh run-melee.sh /path/to/your-melee-1.02.iso, or set MELEE_DISC.\n");
#endif
            return 2;
        }
        printf("disc mounted: %s (ready=%d)\n", disc, (int)dvd_image_ready());
    }

    if (!mex_runtime_init(&s_cpu)) return 3;

    boot_setup_os_globals(&s_cpu, &s_layout);

    /* Seed the FST into guest RAM and point low memory at it.
     *
     * boot.c writes 0 to FST_START (0x80000038) and FST_MAXLEN (0x8000003C),
     * which is correct for a runtime that does not assume a disc. On real
     * hardware the apploader loads the FST and fills these in; we load main.dol
     * directly and skip the apploader, so nothing did. DVDReadAsync validates a
     * request against that table before issuing any hardware command -- with a
     * null FST every read is "out of the file", which is why the assert fired
     * with DI reads still at zero. */
    {
        const char* fst_path = getenv("MELEE_FST");
        FILE* ff = fopen(fst_path ? fst_path : "data/GALE01/sys/fst.bin", "rb");
        if (ff) {
            fseek(ff, 0, SEEK_END); long fsz = ftell(ff); fseek(ff, 0, SEEK_SET);
            u32 fst_addr = (0x817FEC60u - (u32)fsz) & ~31u;   /* just below arena hi */
            for (long k = 0; k < fsz; k++) {
                int ch = fgetc(ff);
                if (ch < 0) break;
                mem_write8(&s_cpu, fst_addr + (u32)k, (u8)ch);
            }
            fclose(ff);
            {extern void mods_initialize(Context*,uint32_t,uint32_t);mods_initialize(&s_cpu,fst_addr,(u32)fsz);}
            /* Low-memory globals hold PHYSICAL addresses; the SDK converts with
             * OSPhysicalToCached when it reads them. Storing the cached address
             * left __DVDFSInit pointing at the wrong place, so every FST lookup
             * returned a zero-length entry and DVDReadAsync's
             * `offset < fileInfo->length` check rejected every read. */
            mem_write32(&s_cpu, 0x80000038u, fst_addr & 0x0FFFFFFFu);
            mem_write32(&s_cpu, 0x8000003Cu, (u32)fsz);
            /* Lower ArenaHi to the FST base. Measured: seeding the FST inside
             * the arena let OSInit zero it -- pointer intact, bytes all 00,
             * entries=0 -- so every lookup returned -1. boot.c keeps the boot
             * stack above GC_ARENA_HI for exactly this reason. On hardware the
             * apploader reserves FST space at the top of the arena; this does
             * the same. */
            /* Boot-thread OSThread, so interrupt delivery has a context.
             * Measured: the OSContext guard rejected delivery 14,937,708 times
             * because OS_CURRENT_THREAD (0x800000E4) is never written -- our
             * thread-layer HLE owns thread creation and boot creates none. An
             * OSThread begins with an OSContext, so a zeroed block is a valid
             * place for a handler to save into. Reserved below the FST, outside
             * the arena OSInit clears. */
            u32 boot_thread = (fst_addr - 0x400u) & ~31u;
            for (u32 k = 0; k < 0x400u; k += 4) mem_write32(&s_cpu, boot_thread + k, 0);
            mem_write32(&s_cpu, 0x800000E4u, boot_thread);
            g_boot_osthread = boot_thread;
            printf("boot OSThread at 0x%08X (OS_CURRENT_THREAD seeded)\n", boot_thread);

            /* Host-owned guest area for menu textures and tables (hooks.c). */
            u32 host_area = (boot_thread - 0x40000u) & ~31u;
            { extern void hooks_init(Context*, uint32_t, uint32_t); hooks_init(&s_cpu, host_area, 0x40000u); }
            u32 costume_area = (host_area - 0x28000u) & ~31u;
            { extern void costumes_initialize(Context*,uint32_t,unsigned); costumes_initialize(&s_cpu,costume_area,0x28000u); }
            mem_write32(&s_cpu, 0x80000034u, costume_area);
            printf("ArenaHi lowered to 0x%08X (FST reserved)\n", fst_addr);
            printf("FST seeded at 0x%08X size 0x%lX\n", fst_addr, fsz);
        } else {
            fprintf(stderr, "warning: could not open sys/fst.bin\n");
        }
    }
    printf("loaded %s  entry=0x%08X\n", dol, s_layout.entry_point);

    RecFn fn = lookup_function(s_layout.entry_point);
    if (lookup_is_stub(fn)) { fprintf(stderr, "entry 0x%08X not in function table\n",
                       s_layout.entry_point); return 3; }

    /* The guest blocks: boot waits on retrace, and retrace only happens when
     * something advances the VI clock. Running the entry inline meant the clock
     * never moved and boot waited forever. Guest on its own thread, host loop
     * drives time -- the same split the old standalone host used. */
    s_entry = fn;
    /* Bring aurora up before the frame loop. Guest RAM stays owned here; the
     * shim only needs the base so it can translate guest EAs. */
    {   extern int aurora_link_init(void*, u32, unsigned, unsigned);
        aurora_link_init(s_cpu.ram, s_cpu.ram_size, 640, 480); }
#ifdef _WIN32
    SetEnvironmentVariableA("MELEE_RUNTIME_DOL", dol);
#else
    setenv("MELEE_RUNTIME_DOL", dol, 1);
#endif
    { extern void netplay_init(void); netplay_init(); }

#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 8u << 20, guest_thread, NULL, 0, NULL);
    HANDLE sample_thread = NULL;
    if (getenv("MELEE_SAMPLE_PATH") || getenv("MELEE_SAMPLE_START"))
        sample_thread = CreateThread(NULL, 1u << 20, sampler_thread, NULL, 0, NULL);
    if (!th) { fprintf(stderr, "cannot start guest thread\n"); return 4; }
#else
    pthread_t th, sampler_th;
    {   pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        if (pthread_create(&th, &attr, guest_thread, NULL) != 0) {
            fprintf(stderr, "cannot start guest thread\n"); return 4;
        }
        pthread_attr_destroy(&attr);
    }
    if (getenv("MELEE_SAMPLE_PATH") || getenv("MELEE_SAMPLE_START")) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1u << 20);
        pthread_create(&sampler_th, &attr, sampler_thread, NULL);
        pthread_attr_destroy(&attr);
    }
#endif

    /* Sample the call count during the loop, not only at the end: a frozen
     * count means the guest is genuinely wedged with no calls at all, a
     * climbing one means it is looping and the recorded frontier is misleading.
     * One run separates those. */
    LARGE_INTEGER pace_freq, pace_start;
    QueryPerformanceFrequency(&pace_freq);
    QueryPerformanceCounter(&pace_start);
    LARGE_INTEGER perf_origin=pace_start;
    FILE* perf_file = NULL;
    const char* perf_path = getenv("MELEE_PERF_CSV");
    if (perf_path && *perf_path) perf_file = fopen(perf_path,"w");
    if (perf_file) fprintf(perf_file,"host,seconds,rendered,scene_ticks,begin_ms,translate_ms,end_ms,ready_age_ms,guest_wait_ms\n");
    { int aurora_frame_open = 0;
    for (int i = 0; frames == 0 || i < frames; i++) {
        extern int aurora_link_quit_requested(void);
        extern int netplay_content_reload_requested(void);
        if(netplay_content_reload_requested())break;
        if (aurora_link_quit_requested() || s_term_requested) break;
        vblank_wait();
        { extern void music_frame(void); music_frame(); }
        g_frames_posted++;          /* VI runs at 60 Hz independently of FIFO readiness. */
        { static long capture_frame=-1;
          if(capture_frame<0) { const char* value=getenv("MELEE_RENDERDOC_FRAME");capture_frame=value?atol(value):0; }
          if(capture_frame && g_frames_posted==capture_frame) {
              extern int aurora_link_renderdoc_capture(const char*);
              const char* path=getenv("MELEE_RENDERDOC_PATH");
              fprintf(stderr,"[renderdoc] triggered=%d host=%ld\n",aurora_link_renderdoc_capture(path?path:"build/renderdoc/melee"),g_frames_posted);
          }
        }
render_ready_frame: ;
        LARGE_INTEGER perf_start={0},perf_begin={0},perf_submit={0},perf_end={0};
        int perf_rendered=0;
        LONGLONG perf_fifo_ready=0;
        if(perf_file) QueryPerformanceCounter(&perf_start);
        {   extern void aurora_link_frame_begin(void);
            extern void aurora_link_update(void);
            extern void aurora_link_frame_end(void);
            extern int aurora_submit_frame(void);
            extern long g_gxt_display_copies;
            /* Bracket each stage: the crash is silent, so the last marker
             * printed names the stage it dies in. */
            static long fr = 0;
            int trace = (fr < 8);
            if (trace) { printf("[f%ld] begin\n", fr); fflush(stdout); }
            { extern int aurora_link_test_triangle(void);
              static int tri = -1;
              if (tri < 0) { const char* e = getenv("MELEE_TEST_TRI"); tri = (e && *e) ? 1 : 0; }
              if (tri) { aurora_link_test_triangle(); fr++; continue; } }
            if (!aurora_frame_open) {
                aurora_link_frame_begin();
                aurora_frame_open = 1;
            } else {
                aurora_link_update();
            }
            if(perf_file) QueryPerformanceCounter(&perf_begin);
            if (trace) { printf("[f%ld] submit\n", fr); fflush(stdout); }
            { long copies_before = g_gxt_display_copies;
              if (aurora_submit_frame()) {
                  perf_rendered=1;
                  {extern volatile LONGLONG g_fifo_ready_ticks;perf_fifo_ready=g_fifo_ready_ticks;}
                  if(perf_file) QueryPerformanceCounter(&perf_submit);
                  if (trace) { printf("[f%ld] end at GXCopyDisp\n", fr); fflush(stdout); }
                  /* Translation owns immutable geometry/texture snapshots, and
                   * Aurora owns the command stream until end_frame drains it.
                   * Restore temporary portrait RAM before waking the guest. */
                  { extern void costume_art_frame_done(Context*); costume_art_frame_done(&s_cpu); }
                  static int overlap=-1;
                  if(overlap<0){const char* opt=getenv("MELEE_OVERLAP_RENDER");overlap=(!opt||atoi(opt)!=0)&&!getenv("MELEE_SERIAL_RENDER");}
                  if(overlap){extern void wgseg_release(void);wgseg_release();}
                  aurora_link_frame_end();
                  /* All guest arrays and texture bytes are now captured in
                   * Aurora's owned frame packet. Its bounded frame-slot queue
                   * lets the next guest frame run while the worker submits this
                   * one; readback and shutdown retain their own synchronization. */
                  { static int serial = -1;
                    if (serial < 0) serial = getenv("MELEE_SERIAL_RENDER") != NULL;
                    if (serial) { extern void aurora_link_synchronize(void); aurora_link_synchronize(); }
                  }
                  if(perf_file) QueryPerformanceCounter(&perf_end);
                  aurora_frame_open = 0;
                  { static int capture = -1;
                    if (capture < 0) {
                        const char* e = getenv("MELEE_CAPTURE_AURORA");
                        capture = (e && *e) ? 1 : 0;
                    }
                    if (capture) fe_dump_aurora(); }
                  if(!overlap){extern void wgseg_release(void);wgseg_release();}
              }
            }
            if (trace) { printf("[f%ld] done\n", fr); fflush(stdout); }
            fr++; }
        if(perf_file) {
            extern volatile LONGLONG g_fifo_ready_ticks,g_fifo_wait_ticks;
            static LONGLONG last_wait=0;
            double scale=1000.0/pace_freq.QuadPart;
            fprintf(perf_file,"%d,%.6f,%d,%ld,%.4f,%.4f,%.4f,%.4f,%.4f\n",i+1,
                (double)(perf_start.QuadPart-perf_origin.QuadPart)/pace_freq.QuadPart,
                perf_rendered,g_scene_tick,(perf_begin.QuadPart-perf_start.QuadPart)*scale,
                perf_rendered?(perf_submit.QuadPart-perf_begin.QuadPart)*scale:0,
                perf_rendered?(perf_end.QuadPart-perf_submit.QuadPart)*scale:0,
                perf_rendered?(perf_begin.QuadPart-perf_fifo_ready)*scale:0,
                (g_fifo_wait_ticks-last_wait)*scale);
            last_wait=g_fifo_wait_ticks;
        }
        /* Pop it from here too. When every guest thread is blocked in the
         * retrace wait nothing calls the tick, and the wake lives inside it --
         * so the guest saw 2 retraces in 240 frames and then slept forever.
         * The host is the only party guaranteed to still be running. */
        /* Pace at a real retrace, not 1 ms.
         *
         * Sleep(1) gave the guest ~120 ms of wall-clock for 120 "frames", so a
         * console game had a sixteenth of real time to boot -- the wall-clock
         * profiler made this visible by collecting only 121 samples for an
         * entire run. Everything the SDK does "after N frames" was being
         * starved in wall-clock terms, which is the same failure the
         * VIWaitForRetrace HLE already documents for its own path. */
        /* Rendering is part of the 16.67 ms budget, not an extra delay. */
        LONGLONG deadline=pace_start.QuadPart+(pace_freq.QuadPart*(i+1))/60;
        LARGE_INTEGER pace_now;
        for (;;) {
            QueryPerformanceCounter(&pace_now);
            LONGLONG left=deadline-pace_now.QuadPart;
            if (left<=0) break;
            DWORD ms=(DWORD)(left*1000/pace_freq.QuadPart);
            { extern int wgseg_wait_ready(DWORD);
              if(wgseg_wait_ready(ms>1?ms-1:0)) goto render_ready_frame;
              if(ms<=1) SwitchToThread(); }
            if (aurora_link_quit_requested() || s_term_requested) { left = 0; break; }
        }
        if (pace_now.QuadPart-deadline>pace_freq.QuadPart/4)
            pace_start.QuadPart=pace_now.QuadPart-(pace_freq.QuadPart*(i+1))/60;
        if ((i % 60) == 59)
            printf("  [%3d] calls=%ld distinct=%ld dvdreads=%ld last_fn=0x%08X present=%llu\n",
                   i + 1, g_call_count, g_distinct_fns, g_hle_dvd_reads, g_last_fn,
                   (unsigned long long)backend.present_count);
    }
    if (aurora_frame_open) {
        extern void aurora_link_frame_end(void);
        aurora_link_frame_end();
    }
    }

    if(perf_file) fclose(perf_file);
    /* Stop guest execution before unloading renderer DLLs or inspecting RAM. */
    InterlockedExchange(&g_guest_stopping,1);
    { extern void netplay_shutdown(void); netplay_shutdown(); }
    { extern void hle_thread_stop_all(void);hle_thread_stop_all(); }
    { extern void frontend_card_flush(void); frontend_card_flush(); }
#ifdef _WIN32
    if(WaitForSingleObject(th,3000)!=WAIT_OBJECT_0) {
        fprintf(stderr,"Guest did not stop at a safe point\n");fflush(NULL);
        TerminateProcess(GetCurrentProcess(),8);
    }
#else
    signal(SIGALRM, fe_alarm_handler);
    alarm(5);
    if(platform_thread_join(th,3000)!=0) {
        fprintf(stderr,"Guest did not stop at a safe point\n");fflush(NULL);
        _exit(8);
    }
    alarm(0);
#endif
    { extern void frontend_card_flush(void); frontend_card_flush(); }
    {
        FILE* dump=fopen("build/native-game/last-context.bin","wb");
        if (dump) { fwrite(g_cur_ctx,1,sizeof(*g_cur_ctx),dump); fclose(dump); }
        dump=fopen("build/native-game/last-memory.bin","wb");
        if (dump) { fwrite(s_cpu.ram,1,s_cpu.ram_size,dump); fclose(dump); }
    }
    sampler_report();
#ifdef _WIN32
    if (sample_thread) { WaitForSingleObject(sample_thread, INFINITE); CloseHandle(sample_thread); }
    CloseHandle(th);
#endif
    if (hosted) {
        extern int netplay_content_reload_requested(void);
        extern void runtime_release(Context*);
        int result = netplay_content_reload_requested() ? 73 : 0;
        runtime_release(&s_cpu);
#ifdef _WIN32
        if (fault_handler) RemoveVectoredExceptionHandler(fault_handler);
        SetUnhandledExceptionFilter(previous_fault);
#endif
        fprintf(stderr,"[content-runtime] released; result=%d\n",result);
        return result;
    }
    frontend_bus_report();
    printf("poll 0x80433318: first=0x%08X last=0x%08X changes=%ld writer=0x%08X\n",
           g_poll_first, g_poll_last, g_poll_changes, g_poll_writer);
    {   extern uint32_t g_sleepq[8], g_sleeplr[8], g_wakeq[8]; extern long g_sleepn[8], g_waken[8];
        printf("PE finish delivered=%ld\n", g_pe_delivered);
    printf("sleep queues:\n");
        for (int k=0;k<8;k++) if (g_sleepq[k]) printf("   q=%08X  sleeps=%ld  caller=%08X\n", g_sleepq[k], g_sleepn[k], g_sleeplr[k]);
        printf("wake queues:\n");
        for (int k=0;k<8;k++) if (g_wakeq[k]) printf("   q=%08X  wakes=%ld\n", g_wakeq[k], g_waken[k]);
    }
    {   extern void thread_table_report(void); thread_table_report(); }
    {   extern long g_gtm_started,g_gtm_woke,g_gtm_nofn,g_gtm_ran,g_gtm_returned;
        printf("created threads: started=%ld woke=%ld entry_missing=%ld ran=%ld returned=%ld\n",
               g_gtm_started,g_gtm_woke,g_gtm_nofn,g_gtm_ran,g_gtm_returned); }
    {   extern long g_sched_switches, g_sched_deadlocks;
        printf("sched: switches=%ld  deadlock-breaks=%ld\n", g_sched_switches, g_sched_deadlocks); }
    printf("thread: OSSleepThread=%ld  OSWakeupThread=%ld\n", g_os_sleeps, g_os_wakes);
    printf("dvd HLE: reads=%ld\n", g_hle_dvd_reads);
    /* HSD_DevComDVDWakeUp early-returns while F5 != 0. If the synchronous DVD
     * callback clears it before the outer WakeUp sets it, it latches at 1 and
     * every later WakeUp bails -- which would stop DevCom issuing. */
    {   extern unsigned int g_fncount[0x10000];
        extern uint32_t g_fnaddr[0x10000];
        /* To its own file: stderr warnings interleave with stdout and shredded
         * the previous census into unreadable fragments. */
        /* Full executed-function dump, for intersecting with the GX symbol
         * set offline: the top-20 census cannot answer "which GX entry points
         * does Melee actually call". */
        {   FILE* af = fopen("build/allfns.txt", "w");
            if (af) {
                for (int i2 = 0; i2 < 0x10000; i2++)
                    if (g_fncount[i2]) fprintf(af, "%08X %u\n", g_fnaddr[i2], g_fncount[i2]);
                fclose(af);
            } }
        FILE* hf = fopen("build/hot.txt", "w");
        if (hf) {
            for (int rank = 0; rank < 20; rank++) {
                unsigned int best = 0; int bi = -1;
                for (int i2 = 0; i2 < 0x10000; i2++)
                    if (g_fncount[i2] > best) { best = g_fncount[i2]; bi = i2; }
                if (bi < 0 || best == 0) break;
                fprintf(hf, "%2d. %08X  calls=%u\n", rank + 1, g_fnaddr[bi], best);
                g_fncount[bi] = 0;
            }
            fclose(hf);
        }
    }
    {   extern long g_vi_retrace_handler,g_vi_setpostcb,g_vi_postcb,g_vi_hsdinit,g_pad_fill;
        printf("vi chain: HSD_VIInit=%ld SetPostCB=%ld __VIRetraceHandler=%ld HSD_VIPostRetraceCB=%ld padfill=%ld\n",
               g_vi_hsdinit, g_vi_setpostcb, g_vi_retrace_handler, g_vi_postcb, g_pad_fill); }
    {   extern long g_pad_renewraw,g_pad_renew,g_pad_init,g_padread,g_gmmain;
        printf("pad chain: HSD_PadInit=%ld PADRead=%ld RenewRaw=%ld RenewStatus=%ld  gmMain=%ld\n",
               g_pad_init, g_padread, g_pad_renewraw, g_pad_renew, g_gmmain); }
    {   extern uint32_t g_lr195D0[8];
        printf("callers of lb_800195D0:");
        for (int k = 0; k < 8; k++) if (g_lr195D0[k]) printf(" %08X", g_lr195D0[k]);
        printf("\n"); }
    {   extern long g_alarm_set, g_alarm_fired; extern uint32_t g_alarm_obj, g_alarm_handler;
        extern long g_alarm_visitA, g_alarm_visitB, g_alarm_nolookup;
        printf("alarm sites: VIWaitForRetrace=%ld  func_8035017C=%ld  nolookup=%ld\n",
               g_alarm_visitA, g_alarm_visitB, g_alarm_nolookup);
        extern long g_alarm_at_call, g_alarm_at_seq;
        printf("alarm registered at: call=%ld  retrace=%ld  (of %ld retraces)\n",
               g_alarm_at_call, g_alarm_at_seq, s_vblank_seq);
        printf("alarm: OSSetPeriodicAlarm calls=%ld  obj=%08X handler=%08X  fired=%ld\n",
               g_alarm_set, g_alarm_obj, g_alarm_handler, g_alarm_fired); }
    printf("devcom flags: F5(dvd busy)=%02x  F4(aram busy)=%02x\n",
           (mem_read32(&s_cpu, 0x804D77F4u) >> 16) & 0xFFu,
           (mem_read32(&s_cpu, 0x804D77F4u) >> 24) & 0xFFu);
    { extern long arq_queue_depth(void); extern long g_arq_drained;
      printf("arq queue: depth_at_exit=%ld  drained=%ld\n", arq_queue_depth(), g_arq_drained); }
    printf("arq HLE: transfers=%ld\n", g_hle_arq);
    { printf("callers of lb_8001CC84:");
      for (int q = 0; q < g_lrCC84_n; q++) printf(" 0x%08X", g_lrCC84[q]);
      printf("\n"); }
    { printf("callers of lb_800195D0:");
      for (int q = 0; q < g_lr195_n; q++) printf(" 0x%08X", g_lr195[q]);
      printf("\n"); }
    printf("landmarks: lb_195D0=%ld lb_192A8=%ld gmMain=%ld HSD_Init=%ld\n",
           g_lm_195D0, g_lm_192A8, g_lm_gmMain, g_lm_HSDinit);
    printf("caller 0x8030178C: calls=%ld lr=0x%08X\n", g_c178C, g_c178C_lr);
    printf("scene: tick=%ld dispatcher_lr=0x%08X\n", g_scene_tick, g_scene_lr);
    printf("lb writers: init658=%ld(lr=0x%08X) init8BC=%ld(lr=0x%08X) others=%ld\n",
           g_w658, g_w658_lr, g_w8BC, g_w8BC_lr, g_wother);
    printf("preload transitions: CachePreloadedFile=%ld  _80017CC4=%ld  _80017E64=%ld\n",
           g_pc_cache, g_pc_CC4, g_pc_E64);
    printf("lbFile_800164A4 (async loader) = %ld\n", g_lbfile);
    printf("dvd callback dispatch: ran=%ld  UNRESOLVED=%ld\n", g_hle_cb_ran, g_hle_cb_missing);
    printf("devcom callbacks: req4(2MB/ARAM)=%ld  req5(preload)=%ld\n", g_cb4, g_cb5);
    printf("devcom: Request=%ld  DVDWakeUp=%ld  ARAMWakeUp=%ld\n",
           g_dc_req, g_dc_dvdwake, g_dc_aramwake);
    { printf("preloadCache @0x80432078:");
      for (int q = 0; q < 24; q++) printf(" %08X", mem_read32(&s_cpu, 0x80432078u + (u32)(q*4)));
      printf("\n"); }
    printf("hsdwait: [0x80433320]=%u [0x80433324]=%u [0x8043337C]=%u  (r13=0x%08X)\n",
           g_dgs_v[0], g_dgs_v[1], g_dgs_v[2], g_dgs_r13);
    printf("  writers: w0=0x%08X(%ld) w1=0x%08X(%ld) w2=0x%08X(%ld)\n",
           g_dgs_writer[0], g_dgs_changes[0], g_dgs_writer[1], g_dgs_changes[1],
           g_dgs_writer[2], g_dgs_changes[2]);
    printf("dvd init: DVDInit=%ld __DVDFSInit=%ld __DVDClearWaitingQueue=%ld\n",
           g_dvd_init, g_dvdfs_init, g_dvd_clearq);
    printf("vi: VIConfigure=%ld VISetNextFrameBuffer=%ld VIFlush=%ld\n",
           g_vi_configure, g_vi_setnextfb, g_vi_flush);
    printf("aram: ARQPostRequest=%ld ARStartDMA=%ld ARQ_ISR=%ld\n",
           g_ar_post, g_ar_dma, g_ar_isr);
    printf("dvd: ReadAsyncPrio=%ld  LowRead=%ld  ReadAbsAsyncPrio=%ld\n",
           g_dvd_readasync, g_dvd_lowread, g_dvd_readabs);
    printf("irq exits: noctx=%ld nocause=%ld\n", g_x_noctx, g_x_nocause);
    printf("irq: delivered=%ld  skipped_nopending=%ld  skipped_noEE=%ld  cause=0x%X mask=0x%X\n",
           g_irq_delivered, g_skip_nopend, g_skip_noee,
           dol_interrupts_pi_cause(&g_irq), dol_interrupts_pi_mask(&g_irq));
    {   extern uint32_t g_ring_addr[32], g_ring_lr[32]; extern unsigned long g_ring_i;
        printf("last 12 guest entries (newest first):\n");
        for (int k = 1; k <= 12; k++) {
            unsigned long idx = g_ring_i - (unsigned long)k;
            printf("   fn=%08X  called_from=%08X\n", g_ring_addr[idx & 31], g_ring_lr[idx & 31]);
        } }
    printf("guest: calls=%ld last_fn=0x%08X\n", g_call_count, g_last_fn);
    printf("EFB copies applied=%ld\n", g_copies_applied);
    { extern int trace_was_entered(uint32_t);
      printf("geometry: stage_model=%d collision_surfaces=%d collision_lines=%d\n",
          trace_was_entered(0x801C5DB0u),trace_was_entered(0x80059554u),trace_was_entered(0x80059404u)); }

    {   extern long g_au_frames, g_au_draws; extern int g_au_ready;
        extern void aurora_link_shutdown(void);
        extern long g_au_dl_bytes;
        extern long g_au_arraybases; extern unsigned char g_au_attr_seen[64];
        printf("GXSetArray attrs seen:");
        for (int k=0;k<64;k++) if (g_au_attr_seen[k]) printf(" %d",k);
        printf("\n");
        /* Does ANY display list in guest memory enable TEX0? A CP write to VCD_HI
     * is [08][60][u32]. Scanning RAM directly avoids the walker entirely --
     * three probes in a row have been limited by its inability to size draws,
     * and this question does not need a parser. */
    {   u32 hits = 0, shown = 0;
        for (u32 a = 0; a + 6 <= s_cpu.ram_size; a++) {
            if (s_cpu.ram[a] != 0x08u || s_cpu.ram[a+1] != 0x60u) continue;
            {   u32 v = ((u32)s_cpu.ram[a+2]<<24)|((u32)s_cpu.ram[a+3]<<16)|
                        ((u32)s_cpu.ram[a+4]<<8) | s_cpu.ram[a+5];
                if (!v) continue;
                hits++;
                if (shown < 5) { shown++;
                    printf("[vcdhi] at 0x%08X value=%08X (TEX0 bits=%u)\n",
                           0x80000000u + a, v, v & 3u); }
            }
        }
        printf("[vcdhi] non-zero VCD_HI writes found in RAM: %u\n", hits);
        fflush(stdout); }
    { extern long g_seg_guard_hits; printf("segment guard hits=%ld\n", g_seg_guard_hits); }
    { extern void frontend_audio_report(void); frontend_audio_report(); }
    printf("aurora: ready=%d frames=%ld dl_calls=%ld bytes=%ld arraybases=%ld\n", g_au_ready, g_au_frames, g_au_draws, g_au_dl_bytes, g_au_arraybases);
    /* Readback must precede shutdown: the framebuffer is destroyed with the
     * device, which is why the first attempt reported "unavailable".
     * aurora's own output first -- the XFB capture below reads guest memory,
     * which aurora never writes, so it can only ever show the EFB-copy
     * consumer's result. */
    {   extern int aurora_link_readback(unsigned char*, u32*, u32*, u32);
        static unsigned char rb[1280 * 960 * 3];
        u32 rw = 0, rh = 0;
        if (aurora_link_readback(rb, &rw, &rh, (u32)sizeof rb) && rw && rh) {
            FILE* af = fopen("build/frame_aurora.ppm", "wb");
            if (af) {
                fprintf(af, "P6\n%u %u\n255\n", rw, rh);
                fwrite(rb, 1, (size_t)rw * rh * 3u, af);
                fclose(af);
                printf("wrote build/frame_aurora.ppm (%ux%u)\n", rw, rh);
            }
        } else {
            printf("aurora readback unavailable\n");
        }
        fflush(stdout);
    }

        /* Aurora currently completes an explicit shutdown cleanly but faults
         * later while Windows unloads the shim's static C++ runtime. Tests can
         * stop after the GPU readback without running DLL detach, which avoids
         * an OS error dialog while keeping the normal teardown path available
         * for fixing and verification. */
        { extern void frontend_card_flush(void);frontend_card_flush(); }
        { const char* fast_exit = getenv("MELEE_TEST_FAST_EXIT");
          if (fast_exit && *fast_exit) {
              fflush(NULL);
              { extern int netplay_content_reload_requested(void); TerminateProcess(GetCurrentProcess(),netplay_content_reload_requested()?73:0); }
          } }

        aurora_link_shutdown(); }
    printf("frames=%d retraces=%ld present=%llu xfb=0x%08X vi=%ux%u\n",
           frames, s_vblank_seq, (unsigned long long)backend.present_count,
           backend.current_xfb, backend.vi.vi_width, backend.vi.vi_height);

    /* Decode with GXRuntime's own YUYV routine rather than another hand-rolled
     * one -- the last thing the old host did that this replaces. */
    /* Sample the buffer the GP actually copied to.
     *
     * backend.current_xfb is what VI is scanning out, but Melee double-buffers:
     * the copies landed at 0x004F99E0 while VI pointed at 0x004FF9E0, so the
     * capture was reading a buffer nothing had written and reporting uniform
     * green (YUYV zeros) as though the consumer had failed. */
    u32 cap = g_last_copy_dest ? g_last_copy_dest : backend.current_xfb;
    if (cap) {
        unsigned w = g_last_copy_dest && g_last_copy_w ? g_last_copy_w
                   : (backend.vi.vi_width  ? backend.vi.vi_width  : 640);
        unsigned h = g_last_copy_dest && g_last_copy_h ? g_last_copy_h
                   : (backend.vi.vi_height ? backend.vi.vi_height : 480);
        printf("capture: xfb_vi=0x%08X  last_copy=0x%08X  using 0x%08X (%ux%u)\n",
               backend.current_xfb, g_last_copy_dest, cap, w, h);
        FILE* f = fopen("build/frame_gxrt.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (unsigned y = 0; y < h; y++)
                for (unsigned x = 0; x < w; x += 2) {
                    u32 ea = cap + (y * w + x) * 2u;
                    u8 px[8];
                    dol_yuyv_pair_to_rgba8(mem_read8(&s_cpu, ea + 0),
                                           mem_read8(&s_cpu, ea + 1),
                                           mem_read8(&s_cpu, ea + 2),
                                           mem_read8(&s_cpu, ea + 3), px);
                    fputc(px[0], f); fputc(px[1], f); fputc(px[2], f);
                    fputc(px[4], f); fputc(px[5], f); fputc(px[6], f);
                }
            fclose(f);
            printf("wrote build/frame_gxrt.ppm (%ux%u)\n", w, h);
        }
    } else {
        printf("no XFB presented yet\n");
    }
    fflush(NULL);
    { extern int netplay_content_reload_requested(void); ExitProcess(netplay_content_reload_requested()?73:0); }
    return 0;
}
