/* Exercise the production scheduler checkpoint against real HLE mutations.
 * The parked thread has lower priority and never runs a native continuation. */
#include <stdlib.h>
#include "../hle_thread.c"

Context* g_cur_ctx;
int g_trace;
RecFn lookup_function(uint32_t address) { (void)address; return NULL; }
int lookup_is_stub(RecFn fn) { return fn == NULL; }
void frontend_guest_checkpoint(void) {}
void frontend_tick_guest_time(void) {}
void frontend_deliver_interrupts(Context* ctx) { (void)ctx; }
void arq_drain(Context* ctx) { (void)ctx; }
int netplay_simulating(void) { return 0; }

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
static Context cpu;
static unsigned char* checkpoint;
static unsigned cases;

static void parked_thread(int state, uint32_t queue) {
    memset(&s_th[1], 0, sizeof s_th[1]);
    s_th[1].used = 1;
    s_th[1].priority = s_th[0].priority + 4;
    s_th[1].state = state;
    s_th[1].sleep_q = queue;
    s_th[1].osthread = 0x80002000u;
    s_th[1].entry = 0x80003000u;
    s_th[1].active_ctx = &s_th[1].ctx;
    s_th[1].ctx.ram = cpu.ram;
    s_th[1].ctx.ram_size = cpu.ram_size;
    s_th[1].ctx.gpr[3] = 0x12345678u;
    cpu.gpr[3] = s_th[1].osthread;
    frontend_thread_checkpoint(checkpoint);
    CHECK(frontend_thread_checkpoint_matches(checkpoint));
}
/* The mutation is real -- the checkpoint no longer matches -- and a restore
 * puts it back rather than giving up. That distinction is the whole point:
 * every one of these happens during an ordinary match, a retrace wake on every
 * single video frame, and refusing to rewind them left the simulation running
 * on inputs that had already been corrected everywhere else. */
static void changed_without_switch(void) {
    HThread before = s_th[1];
    CHECK(g_sched_switches == 0);
    CHECK(s_cur == 0);
    CHECK(!frontend_thread_checkpoint_matches(checkpoint));
    CHECK(frontend_thread_checkpoint_restore(checkpoint));
    CHECK(frontend_thread_checkpoint_matches(checkpoint));
    /* Restoring puts back the bookkeeping and nothing else: the wait handle,
     * the guest thread it stands for and its entry point are identities, not
     * state, and a restore that changed one would be describing a different
     * thread. */
    CHECK(s_th[1].run == before.run && s_th[1].osthread == before.osthread
          && s_th[1].entry == before.entry && s_th[1].active_ctx == before.active_ctx);
    /* Leave the caller looking at the mutated state it set up, as before. */
    s_th[1] = before;
    frontend_thread_checkpoint(checkpoint);
    CHECK(frontend_thread_checkpoint_matches(checkpoint));
    ++cases;
}

int main(void) {
    cpu.ram_size = 0x10000;
    cpu.ram = calloc(1, cpu.ram_size);
    CHECK(cpu.ram);
    cpu.msr = 0x8000u;
    g_cur_ctx = &cpu;
    mem_w32(OS_CURRENT_THREAD, 0x80001000u);
    checkpoint = malloc(frontend_thread_checkpoint(NULL));
    CHECK(checkpoint);
    frontend_thread_checkpoint(checkpoint);
    CHECK(frontend_thread_checkpoint_matches(checkpoint));
    sched_init_once();
    CHECK(!frontend_thread_checkpoint_matches(checkpoint));
    ++cases;

    parked_thread(TS_SUSPENDED, 0);
    func_8034B61C(&cpu); /* OSResumeThread: lower priority, no switch. */
    CHECK(s_th[1].state == TS_READY);
    CHECK(mem_r32(s_th[1].osthread + OSTHREAD_SUSPEND) == 0);
    changed_without_switch();

    parked_thread(TS_READY, 0);
    func_8034B8A4(&cpu); /* OSSuspendThread(other): no switch. */
    CHECK(s_th[1].state == TS_SUSPENDED);
    CHECK(mem_r32(s_th[1].osthread + OSTHREAD_SUSPEND) == 1);
    changed_without_switch();

    parked_thread(TS_SLEEPING, 0x80004000u);
    cpu.gpr[3] = 0x80004000u;
    func_8034BB00(&cpu); /* OSWakeupThread: ready + queue change, no switch. */
    CHECK(s_th[1].state == TS_READY && s_th[1].sleep_q == 0);
    changed_without_switch();

    parked_thread(TS_SLEEPING, RETRACE_Q);
    hle_thread_wake_retrace();
    CHECK(s_th[1].state == TS_READY && s_th[1].sleep_q == 0);
    changed_without_switch();

    parked_thread(TS_SLEEPING, 0x80004000u);
    cpu.gpr[3] = 0x80005000u;
    func_8034BB00(&cpu); /* Unrelated wake does not invalidate checkpoint. */
    CHECK(frontend_thread_checkpoint_matches(checkpoint));
    cpu.gpr[9] ^= 0x55u; /* Current CPU is restored separately. */
    CHECK(frontend_thread_checkpoint_matches(checkpoint));
    ++cases;

    s_th[1].ctx.gpr[9] ^= 0x55u; /* A parked continuation changed. */
    changed_without_switch();
    s_th[1].priority++;
    changed_without_switch();
    s_th[1].sleep_q++;
    changed_without_switch();
    /* A thread that actually ran has a native call stack no snapshot reaches,
     * so this one is still refused. */
    g_sched_switches++;
    CHECK(!frontend_thread_checkpoint_matches(checkpoint));
    CHECK(!frontend_thread_checkpoint_restore(checkpoint));
    g_sched_switches--;
    ++cases;

    /* So is a thread that has appeared or gone since the snapshot. */
    parked_thread(TS_READY, 0);
    s_th[1].used = 0;
    CHECK(!frontend_thread_checkpoint_restore(checkpoint));
    s_th[1].used = 1;
    CHECK(frontend_thread_checkpoint_restore(checkpoint));
    ++cases;

    /* A different thread reusing the slot is not the same thread, however
     * closely its bookkeeping matches. */
    s_th[1].osthread ^= 0x1000u;
    CHECK(!frontend_thread_checkpoint_restore(checkpoint));
    s_th[1].osthread ^= 0x1000u;
    s_th[1].entry ^= 0x1000u;
    CHECK(!frontend_thread_checkpoint_restore(checkpoint));
    s_th[1].entry ^= 0x1000u;
    CHECK(frontend_thread_checkpoint_restore(checkpoint));
    ++cases;

#ifdef _WIN32
    CHECK(CloseHandle(s_th[0].run));
#else
    platform_event_destroy(s_th[0].run);
#endif
    DeleteCriticalSection(&s_lock);
    free(checkpoint);
    free(cpu.ram);
    printf("scheduler checkpoint: %u cases passed; non-switching resume, suspend, wakeup and"
           " retrace wake are rewound, an actual switch and a changed thread set are refused\n", cases);
    return 0;
}
