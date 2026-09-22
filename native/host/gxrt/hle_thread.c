/* Host-thread-backed cooperative scheduler for the GameCube OS thread layer.
 *
 * Why this exists: static recompilation is call-driven. A recompiled function
 * runs on the host call stack, so the guest's own scheduler cannot work --
 * OSLoadContext restores a saved PC and stack and `rfi`s to it, but our `rfi`
 * compiles to a plain `return`, so SelectThread just falls back to its caller.
 * The result is that only the boot thread ever runs: the game marks threads
 * runnable via OSWakeupThread and nothing ever executes them.
 *
 * So each guest OSThread gets its own host thread with its own Context, and the
 * thread API is HLE'd to hand off between them. Exactly ONE runs at a time,
 * serialized by per-thread auto-reset events, which preserves the single-threaded
 * execution the recompiled code assumes over shared g_ram -- this is a scheduler,
 * not real parallelism.
 *
 * GameCube OSThread layout used here (offsets from the Dolphin SDK):
 *   0x000 OSContext (gpr[32] at 0x00, cr/lr/ctr/xer at 0x80..0x8C, srr0 at 0x198)
 *   0x2C8 u16 state      1=READY 2=RUNNING 4=WAITING 8=MORIBUND
 *   0x2CC s32 suspend
 *   0x2D0 s32 priority (effective)
 */
#include "abi_recompcore.h"
#include "recomp_funcs.h"
#include <stdio.h>
#include <string.h>
#include "platform_compat.h"

/* Counters for the DVD completion path. The SDK sleeps on __DVDThreadQueue and
 * is woken by OSWakeupThread from the DI handler. Counting sleeps and wakes on
 * our own HLE'd thread layer tests that path end to end without requiring the
 * handler call to work -- and separates "the completion never signals" from
 * "the signal never wakes the sleeper". */
long g_os_sleeps = 0, g_os_wakes = 0, g_os_wakes_hit = 0;
long g_gtm_started=0, g_gtm_woke=0, g_gtm_nofn=0, g_gtm_ran=0, g_gtm_returned=0;
uint32_t g_sleepq[8], g_sleeplr[8]; long g_sleepn[8];
uint32_t g_wakeq[8]; long g_waken[8];

#define OSTHREAD_STATE     0x2C8
#define OSTHREAD_SUSPEND   0x2CC
#define OSTHREAD_PRIORITY  0x2D0
#define OS_CURRENT_THREAD  0x800000E4u

enum { TS_FREE = 0, TS_READY, TS_RUNNING, TS_SLEEPING, TS_SUSPENDED, TS_DEAD };

typedef struct {
    int      used;
    uint32_t osthread;        /* guest OSThread* -- the identity the game uses */
    uint32_t entry, arg;
    int      priority;        /* 0..31, lower value = higher priority */
    volatile int state;
    uint32_t sleep_q;         /* OSThreadQueue* this thread is blocked on */
#ifdef _WIN32
    HANDLE   handle;          /* Win32 thread handle for WaitForSingleObject */
#else
    pthread_t pthread;        /* POSIX thread for pthread_join */
#endif
    HANDLE   run;             /* auto-reset event: signalled => this thread may run */
    Context  ctx;
    Context* active_ctx;
} HThread;

#define MAXTH 24

/* Dump the table at the freeze. One switch happened and never returned, so the
 * question is whether the thread that was switched to has a live host thread
 * parked on its event -- SetEvent on an event nobody waits on parks the caller
 * for good. */
void thread_table_report(void);
static HThread s_th[MAXTH];
static int     s_cur = 0;              /* index of the thread currently running */
static CRITICAL_SECTION s_lock;
static int     s_inited = 0;

long g_sched_switches = 0;
long g_sched_threads  = 0;
long g_sched_deadlocks = 0;

extern int g_trace;

extern void frontend_guest_checkpoint(void);

static void sched_init_once(void)
{
    if (s_inited) return;
    InitializeCriticalSection(&s_lock);
    /* Thread 0 is the boot thread: already running, on the host thread that
     * called into the guest entry point. It has no run-event of its own until
     * something else needs to hand control back to it. */
    s_th[0].active_ctx = g_cur_ctx;
    s_th[0].osthread = mem_r32(OS_CURRENT_THREAD);
    s_th[0].used = 1;
    s_th[0].state = TS_RUNNING;
    s_th[0].priority = 16;
    s_th[0].run = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_inited = 1;
    g_sched_threads = 1;
}

static int find_by_osthread(uint32_t os)
{
    for (int i = 0; i < MAXTH; i++)
        if (s_th[i].used && s_th[i].osthread == os) return i;
    return -1;
}

/* Highest-priority runnable thread; -1 if every thread is blocked. */
static int pick_next(void)
{
    int best = -1;
    for (int i = 0; i < MAXTH; i++) {
        if (!s_th[i].used) continue;
        if (s_th[i].state != TS_READY && s_th[i].state != TS_RUNNING) continue;
        if (best < 0 || s_th[i].priority < s_th[best].priority) best = i;
    }
    return best;
}

/* Mirror the running thread's identity into the OS global the game reads. */
static void set_current_thread_global(int i)
{
    g_cur_ctx = s_th[i].active_ctx;
    mem_w32(OS_CURRENT_THREAD, s_th[i].osthread);
}

/* Hand control to the highest-priority runnable thread and block until we are
 * chosen again. Must be called with the current thread's state already set to
 * whatever it should be (READY to yield, SLEEPING to block, DEAD to exit). */
static void sched_yield_locked(void)
{
    int me = s_cur;
    int next = pick_next();

    while (next < 0) {
        /* The CPU idles with interrupts enabled when all threads are asleep.
         * Do not make sleepers runnable until their queue is actually woken. */
        extern void frontend_tick_guest_time(void);
        extern void frontend_deliver_interrupts(Context*);
        extern void arq_drain(Context*);
        LeaveCriticalSection(&s_lock);
        uint32_t old_msr = g_cur_ctx->msr;
        g_cur_ctx->msr |= 0x8000u;
        frontend_tick_guest_time();
        frontend_deliver_interrupts(g_cur_ctx);
        arq_drain(g_cur_ctx);
        g_cur_ctx->msr = old_msr;
        Sleep(1);
        EnterCriticalSection(&s_lock);
        next = pick_next();
    }

    if (next == me) {
        if (s_th[me].state == TS_READY) s_th[me].state = TS_RUNNING;
        set_current_thread_global(me);
        return;
    }

    s_cur = next;
    s_th[next].state = TS_RUNNING;
    set_current_thread_global(next);
    g_sched_switches++;

    HANDLE mine = s_th[me].run;
    SetEvent(s_th[next].run);                    /* let the chosen thread go */
    LeaveCriticalSection(&s_lock);
    if (s_th[me].state != TS_DEAD)
        WaitForSingleObject(mine, INFINITE);     /* ...and park until it is our turn */
    frontend_guest_checkpoint();
    EnterCriticalSection(&s_lock);
    s_cur = me;
    s_th[me].state = TS_RUNNING;
    set_current_thread_global(me);
}

static void guest_sched_yield(void)
{
    EnterCriticalSection(&s_lock);
    sched_yield_locked();
    LeaveCriticalSection(&s_lock);
}

/* ---- host thread body ------------------------------------------------------ */
#ifdef _WIN32
static DWORD WINAPI guest_thread_main(LPVOID p)
#else
static void* guest_thread_main(void* p)
#endif
{
    int i = (int)(intptr_t)p;
    g_gtm_started++;
    WaitForSingleObject(s_th[i].run, INFINITE);  /* wait to be scheduled first */
    frontend_guest_checkpoint();
    g_gtm_woke++;
    g_cur_ctx = s_th[i].active_ctx;

    RecFn fn = lookup_function(s_th[i].entry);
    if (lookup_is_stub(fn)) { g_gtm_nofn++; }
    else { g_gtm_ran++; fn(&s_th[i].ctx); g_gtm_returned++; }

    EnterCriticalSection(&s_lock);
    s_th[i].state = TS_DEAD;
    mem_w32(s_th[i].osthread + OSTHREAD_STATE, 0);
    sched_yield_locked();                        /* hand off; we never come back */
    LeaveCriticalSection(&s_lock);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- HLE overrides ---------------------------------------------------------
 * Signatures follow the SDK; arguments arrive in r3.. as usual. */

/* OSCreateThread(OSThread* t, void* func, void* param, void* stackTop,
 *                u32 stackSize, OSPriority prio, u16 attr) -> BOOL */
void func_8034B25C(Context* ctx)
{
    sched_init_once();
    EnterCriticalSection(&s_lock);

    uint32_t os    = ctx->gpr[3];
    uint32_t func  = ctx->gpr[4];
    uint32_t param = ctx->gpr[5];
    uint32_t stack = ctx->gpr[6];
    int      prio  = (int)ctx->gpr[8];

    int i = -1;
    for (int k = 1; k < MAXTH; k++) if (!s_th[k].used) { i = k; break; }
    if (i < 0) { ctx->gpr[3] = 0; LeaveCriticalSection(&s_lock); return; }   /* out of slots */

    memset(&s_th[i], 0, sizeof(s_th[i]));
    s_th[i].active_ctx = &s_th[i].ctx;
    s_th[i].used     = 1;
    s_th[i].osthread = os;
    s_th[i].entry    = func;
    s_th[i].arg      = param;
    s_th[i].priority = prio;
    /* GC semantics: a newly created thread starts suspended (suspend count 1)
     * and only becomes runnable when OSResumeThread drops it to 0. */
    s_th[i].state    = TS_SUSPENDED;
    s_th[i].run      = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s_th[i].run) {
        fprintf(stderr, "[thread] event allocation failed for slot %d\n", i);
        s_th[i].used = 0;
        LeaveCriticalSection(&s_lock);
        ctx->gpr[3] = 0;
        return;
    }

    /* Inherit the host wiring from the creator.
     *
     * The memset above zeroes the whole entry, and only registers were being
     * restored -- so ram, ram_size, mem2 and the external_read/write hooks were
     * left NULL and a created thread ran with no memory at all. Every access
     * resolved to nothing, which is why boot froze the moment it switched to
     * one (switches=1, then no guest code executed ever again), and why 5,123
     * byte writes at the DevCom relay buffer were silently discarded earlier in
     * this project: they came from a thread running on a null-ram context. */
    s_th[i].ctx.ram             = ctx->ram;
    s_th[i].ctx.ram_size        = ctx->ram_size;
    s_th[i].ctx.mem2            = ctx->mem2;
    s_th[i].ctx.mem2_size       = ctx->mem2_size;
    s_th[i].ctx.external_read   = ctx->external_read;
    s_th[i].ctx.external_write  = ctx->external_write;
    s_th[i].ctx.external_read32 = ctx->external_read32;
    s_th[i].ctx.external_write32= ctx->external_write32;

    /* Fresh guest CPU state: argument in r3, its own stack, and the small-data
     * bases inherited from the creator (the SDK sets them once at init). */
    s_th[i].ctx.gpr[1]  = stack & ~0xFu;
    s_th[i].ctx.gpr[2]  = ctx->gpr[2];
    s_th[i].ctx.gpr[13] = ctx->gpr[13];
    s_th[i].ctx.gpr[3]  = param;
    s_th[i].ctx.msr   = ctx->msr;

    /* Publish the fields the game reads back out of its own OSThread struct. */
    mem_w32(os + OSTHREAD_STATE, 1);
    mem_w32(os + OSTHREAD_SUSPEND, 1);
    mem_w32(os + OSTHREAD_PRIORITY, (uint32_t)prio);

#ifdef _WIN32
    s_th[i].handle = CreateThread(NULL, 4u << 20, guest_thread_main,
                                  (LPVOID)(intptr_t)i, 0, NULL);
#else
    {   pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4u << 20);
        int err = pthread_create(&s_th[i].pthread, &attr, guest_thread_main, (void*)(intptr_t)i);
        pthread_attr_destroy(&attr);
        if (err) {
            fprintf(stderr, "[thread] pthread_create failed for slot %d: %d\n", i, err);
            s_th[i].used = 0;
            LeaveCriticalSection(&s_lock);
            ctx->gpr[3] = 0;
            return;
        }
    }
#endif
    g_sched_threads++;
    LeaveCriticalSection(&s_lock);
    ctx->gpr[3] = 1;
}

/* OSResumeThread(OSThread*) -> s32 previous suspend count */
void func_8034B61C(Context* ctx)
{
    sched_init_once();
    EnterCriticalSection(&s_lock);
    int i = find_by_osthread(ctx->gpr[3]);
    int prev = 1;
    if (i >= 0) {
        if (s_th[i].state == TS_SUSPENDED) s_th[i].state = TS_READY;
        mem_w32(s_th[i].osthread + OSTHREAD_SUSPEND, 0);
        /* A higher-priority thread becoming runnable preempts us, matching the
         * SDK's reschedule-on-resume behaviour. */
        if (s_th[i].priority < s_th[s_cur].priority) {
            s_th[s_cur].state = TS_READY;
            sched_yield_locked();
        }
    }
    LeaveCriticalSection(&s_lock);
    ctx->gpr[3] = (uint32_t)prev;
}

/* OSSuspendThread(OSThread*) -> s32 previous suspend count */
void func_8034B8A4(Context* ctx)
{
    sched_init_once();
    EnterCriticalSection(&s_lock);
    int i = find_by_osthread(ctx->gpr[3]);
    if (i >= 0) {
        s_th[i].state = TS_SUSPENDED;
        mem_w32(s_th[i].osthread + OSTHREAD_SUSPEND, 1);
        if (i == s_cur) sched_yield_locked();
    }
    LeaveCriticalSection(&s_lock);
    ctx->gpr[3] = 0;
}

/* OSSleepThread(OSThreadQueue*) */
void func_8034BA14(Context* ctx)
{
    /* 44.8M sleeps against 20 wakes: record which queue and which caller, so
     * the missing signal can be named instead of guessed at. */
    { uint32_t q = ctx->gpr[3], lr = ctx->lr;
      for (int k = 0; k < 8; k++) {
          if (g_sleepq[k] == q) { g_sleepn[k]++; break; }
          if (g_sleepq[k] == 0) { g_sleepq[k] = q; g_sleeplr[k] = lr; g_sleepn[k] = 1; break; }
      } }
    g_os_sleeps++;
    /* Advance virtual time while the guest waits.
     *
     * Guest-driven time made runs reproducible, but on its own it deadlocks:
     * a blocked guest stops calling, so no retrace fires, so the interrupt that
     * would wake it never arrives. A sleep is guest progress too -- ticking here
     * keeps the timeline a pure function of guest behaviour (still fully
     * deterministic) while letting a waiting thread be woken. */
    {   extern void frontend_tick_guest_time(void);
        static long sl = 0;
        if (++sl >= 200) { sl = 0; frontend_tick_guest_time(); }
    }
    sched_init_once();
    EnterCriticalSection(&s_lock);
    s_th[s_cur].sleep_q = ctx->gpr[3];
    s_th[s_cur].state   = TS_SLEEPING;
    sched_yield_locked();
    LeaveCriticalSection(&s_lock);
}

/* OSWakeupThread(OSThreadQueue*) -- wakes every thread blocked on that queue */
void func_8034BB00(Context* ctx)
{
    { uint32_t q = ctx->gpr[3];
      for (int k = 0; k < 8; k++) {
          if (g_wakeq[k] == q) { g_waken[k]++; break; }
          if (g_wakeq[k] == 0) { g_wakeq[k] = q; g_waken[k] = 1; break; }
      } }
    g_os_wakes++;
    sched_init_once();
    EnterCriticalSection(&s_lock);
    uint32_t q = ctx->gpr[3];
    int woke_higher = 0;
    for (int i = 0; i < MAXTH; i++) {
        if (!s_th[i].used || s_th[i].state != TS_SLEEPING) continue;
        if (s_th[i].sleep_q != q) continue;
        s_th[i].state = TS_READY;
        s_th[i].sleep_q = 0;
        if (s_th[i].priority < s_th[s_cur].priority) woke_higher = 1;
    }
    if (woke_higher) { s_th[s_cur].state = TS_READY; sched_yield_locked(); }
    LeaveCriticalSection(&s_lock);
}

/* OSExitThread(void* val) */
void func_8034B37C(Context* ctx)
{
    (void)ctx;
    sched_init_once();
    EnterCriticalSection(&s_lock);
    s_th[s_cur].state = TS_DEAD;
    mem_w32(s_th[s_cur].osthread + OSTHREAD_STATE, 0);
    sched_yield_locked();
    LeaveCriticalSection(&s_lock);
#ifdef _WIN32
    ExitThread(0);
#else
    pthread_exit(NULL);
#endif
}

/* __OSReschedule() -- voluntary yield point the SDK sprinkles through the OS */
void func_8034B22C(Context* ctx)
{
    extern int netplay_simulating(void);
    if (netplay_simulating()) return;
    (void)ctx;
    sched_init_once();
    EnterCriticalSection(&s_lock);
    s_th[s_cur].state = TS_READY;
    sched_yield_locked();
    LeaveCriticalSection(&s_lock);
}

/* Let the host drive a scheduling point from outside the guest (once per frame),
 * so a thread that is merely runnable still gets to run. */
void sched_tick(void)
{
    if (!s_inited) return;
    guest_sched_yield();
}


/* The running thread's guest OSThread pointer.
 *
 * OSThread begins with an OSContext, so this is exactly the context the SDK's
 * interrupt handlers expect. Passing null was why calling them from the frontend
 * wedged in OSSetCurrentContext -- not because delivery is impossible, but
 * because we had no context to hand them. The thread layer has had one all
 * along. */
uint32_t frontend_current_osthread(void)
{
    return s_th[s_cur].used ? s_th[s_cur].osthread : 0u;
}

void thread_table_report(void)
{
    printf("thread table (cur=%d):\n", s_cur);
    for (int i = 0; i < MAXTH; i++) {
        if (!s_th[i].used) continue;
        printf("   [%2d] state=%d prio=%2d osthread=%08X entry=%08X sleep_q=%08X run=%p %s\n",
               i, s_th[i].state, s_th[i].priority, s_th[i].osthread,
               s_th[i].entry, s_th[i].sleep_q,
               (void*)s_th[i].run,
               (i == s_cur) ? "<== current" : "");
    }
}

/* Yield to any other runnable guest thread.
 *
 * A retrace wait that merely spins never lets the scheduler run anyone else:
 * after the single switch to the audio thread, the boot thread stayed parked in
 * WaitForSingleObject for the rest of the run (600 frames, 45,406 calls,
 * dvdreads=0). On hardware VIWaitForRetrace sleeps on retraceQueue and the
 * scheduler picks another thread; this is that hand-off. */
/* Block the caller until a retrace is posted.
 *
 * Yielding was useless here: pick_next is strict priority (GC semantics, no
 * time slicing) and the audio thread runs at priority 0 against the boot
 * thread's 16, so a yield re-picked the same thread every time -- measured as
 * switches=1 for an entire run. On hardware VIWaitForRetrace sleeps on
 * retraceQueue, which takes the waiter out of the runnable set and lets a lower
 * priority thread run. This is that, with a synthetic queue id. */
#define RETRACE_Q 0xFFFFFF01u

void hle_thread_sleep_retrace(void)
{
    sched_init_once();
    EnterCriticalSection(&s_lock);
    s_th[s_cur].sleep_q = RETRACE_Q;
    s_th[s_cur].state   = TS_SLEEPING;
    sched_yield_locked();
    LeaveCriticalSection(&s_lock);
}

void hle_thread_wake_retrace(void)
{
    sched_init_once();
    EnterCriticalSection(&s_lock);
    for (int i = 0; i < MAXTH; i++)
        if (s_th[i].used && s_th[i].state == TS_SLEEPING && s_th[i].sleep_q == RETRACE_Q) {
            s_th[i].state = TS_READY;
            s_th[i].sleep_q = 0;
        }
    LeaveCriticalSection(&s_lock);
}

void hle_thread_yield(void)
{
    sched_init_once();
    EnterCriticalSection(&s_lock);
    if (s_th[s_cur].state == TS_RUNNING) s_th[s_cur].state = TS_READY;
    sched_yield_locked();
    LeaveCriticalSection(&s_lock);
}

/* Wake parked threads only for exit, then join before renderer destruction. */
void hle_thread_stop_all(void) {
    if(!s_inited) return;
    EnterCriticalSection(&s_lock);
    for(unsigned i=0;i<MAXTH;i++) if(s_th[i].run) SetEvent(s_th[i].run);
    LeaveCriticalSection(&s_lock);
    for(unsigned i=1;i<MAXTH;i++) {
#ifdef _WIN32
        if(s_th[i].handle) {
            if(WaitForSingleObject(s_th[i].handle,3000)!=WAIT_OBJECT_0) {
                fprintf(stderr,"Guest worker %u did not stop\n",i);fflush(NULL);TerminateProcess(GetCurrentProcess(),8);
            }
        }
#else
        if(s_th[i].used && s_th[i].pthread) {
            if(platform_thread_join(s_th[i].pthread, 3000) != 0) {
                fprintf(stderr,"Guest worker %u did not stop\n",i);fflush(NULL);_exit(8);
            }
        }
#endif
    }
}

/* A RAM snapshot cannot rewind native thread continuations. Record exact
 * scheduler metadata and parked CPU contexts so rollback can refuse a restore
 * after either a switch or a non-switching resume/suspend/wakeup mutation.
 * Native handles and stacks are identities to compare, never state to restore. */
typedef struct {
    int used, priority, state;
    uint32_t osthread, entry, arg, sleep_q;
    uintptr_t handle, run, active_ctx;
    Context parked_ctx;
} ThreadCheckpoint;
typedef struct {
    int initialized, current;
    long switches;
    ThreadCheckpoint threads[MAXTH];
} SchedulerCheckpoint;

size_t frontend_thread_checkpoint(void* data)
{
    if (!data) return sizeof(SchedulerCheckpoint);
    SchedulerCheckpoint checkpoint;
    memset(&checkpoint, 0, sizeof checkpoint);
    if (s_inited) EnterCriticalSection(&s_lock);
    checkpoint.initialized = s_inited;
    checkpoint.current = s_cur;
    checkpoint.switches = g_sched_switches;
    for (int i = 0; i < MAXTH; ++i) {
        const HThread* thread = &s_th[i];
        ThreadCheckpoint* saved = &checkpoint.threads[i];
        saved->used = thread->used;
        if (!thread->used) continue;
        saved->priority = thread->priority;
        saved->state = thread->state;
        saved->osthread = thread->osthread;
        saved->entry = thread->entry;
        saved->arg = thread->arg;
        saved->sleep_q = thread->sleep_q;
#ifdef _WIN32
        saved->handle = (uintptr_t)thread->handle;
#else
        saved->handle = (uintptr_t)thread->pthread;
#endif
        saved->run = (uintptr_t)thread->run;
        saved->active_ctx = (uintptr_t)thread->active_ctx;
        /* The currently running CPU is restored by the main frame snapshot. */
        if (i != s_cur) saved->parked_ctx = thread->ctx;
    }
    if (s_inited) LeaveCriticalSection(&s_lock);
    memcpy(data, &checkpoint, sizeof checkpoint);
    return sizeof checkpoint;
}

int frontend_thread_checkpoint_matches(const void* data)
{
    SchedulerCheckpoint current;
    frontend_thread_checkpoint(&current);
    return memcmp(&current, data, sizeof current) == 0;
}

/* Put the scheduler back the way the snapshot found it, or report that it
 * cannot be done.
 *
 * A memory snapshot cannot rewind a native call stack, so the question is
 * whether any of them moved. This scheduler is cooperative: exactly one thread
 * runs at a time and every other one is parked inside sched_yield_locked, so a
 * thread's stack can only have moved if it was handed control -- and every
 * hand-off increments g_sched_switches. An unchanged switch count is therefore
 * proof that every parked continuation is exactly where the snapshot left it.
 *
 * What can still differ is bookkeeping the running thread changed by itself: a
 * thread resumed, suspended, or woken from a queue. Those are plain fields,
 * they describe the simulation being rewound, and putting them back is what
 * rewinding means. None of them touches a wait handle -- an event is only
 * signalled immediately before a switch -- so restoring them cannot leave a
 * thread's recorded state disagreeing with what it is actually waiting on.
 *
 * This used to refuse instead, and that was the more dangerous choice.
 * hle_thread_wake_retrace moves a sleeper to ready on every single video
 * frame, so during a match the refusal was not the rare case, it was most of
 * them. A refused correction is not a skipped optimisation: this machine has
 * already simulated frames on predicted inputs that turned out to be wrong,
 * and declining to replay them leaves it permanently disagreeing with its
 * opponent, with nothing left that would ever fix it. Content that streams
 * more -- more fighters, more stages, more music -- wakes those threads more
 * often and so diverged sooner, which is the shape of the reports.
 *
 * A thread appearing or disappearing is still refused. There is no native
 * stack to restore for a thread that did not exist when the snapshot was
 * taken, and none to take away from one that has since exited.
 */
int frontend_thread_checkpoint_restore(const void* data)
{
    SchedulerCheckpoint saved;
    int ok = 1;
    memcpy(&saved, data, sizeof saved);
    if (s_inited) EnterCriticalSection(&s_lock);
    if (saved.initialized != s_inited || saved.current != s_cur
        || saved.switches != g_sched_switches)
        ok = 0;
    for (int i = 0; ok && i < MAXTH; ++i) {
        const ThreadCheckpoint* was = &saved.threads[i];
        const HThread* now = &s_th[i];
        uintptr_t handle;
        if (was->used != now->used) { ok = 0; break; }
        if (!was->used) continue;
#ifdef _WIN32
        handle = (uintptr_t)now->handle;
#else
        handle = (uintptr_t)now->pthread;
#endif
        /* Identities, not state: the same thread, still waiting on the same
         * handle, still running the same guest entry point. */
        if (was->handle != handle || was->run != (uintptr_t)now->run
            || was->active_ctx != (uintptr_t)now->active_ctx
            || was->osthread != now->osthread || was->entry != now->entry)
            ok = 0;
    }
    if (ok) {
        for (int i = 0; i < MAXTH; ++i) {
            const ThreadCheckpoint* was = &saved.threads[i];
            HThread* thread = &s_th[i];
            if (!was->used) continue;
            thread->priority = was->priority;
            thread->state = was->state;
            thread->arg = was->arg;
            thread->sleep_q = was->sleep_q;
            /* No switch has happened, so a parked context cannot have moved.
             * Copying it back is a no-op that says so. */
            if (i != s_cur) thread->ctx = was->parked_ctx;
        }
    }
    if (s_inited) LeaveCriticalSection(&s_lock);
    return ok;
}

void hle_thread_release(void) {
 if(!s_inited)return;
 for(unsigned i=0;i<MAXTH;i++) {
#ifdef _WIN32
  if(s_th[i].handle)CloseHandle(s_th[i].handle);
#endif
  if(s_th[i].run)CloseHandle(s_th[i].run);
 }
 DeleteCriticalSection(&s_lock);s_inited=0;
}
