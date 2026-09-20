/* Linux semantics exercised by the guest scheduler and Online worker. */
#include "../platform_compat.h"
#include <assert.h>

_Static_assert(sizeof(DWORD) == 4, "Win32 DWORD must stay 32-bit");
_Static_assert(sizeof(LONG) == 4, "Win32 LONG must stay 32-bit");
_Static_assert(sizeof(LONGLONG) == 8, "Win32 LONGLONG must stay 64-bit");

static volatile LONG atomic_count;
static DWORD increment_worker(LPVOID unused) {
    (void)unused;
    for (int i = 0; i < 10000; ++i) InterlockedIncrement(&atomic_count);
    return 0;
}
static void atomics_and_widths(void) {
    assert(InterlockedExchange(&atomic_count, -7) == 0);
    assert(InterlockedCompareExchange(&atomic_count, 1, 0) == -7);
    assert(InterlockedCompareExchange(&atomic_count, 0, -7) == -7);
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) { threads[i] = CreateThreadSimple(0, increment_worker, NULL); assert(threads[i]); }
    for (int i = 0; i < 4; ++i) assert(platform_thread_join(threads[i], 5000) == WAIT_OBJECT_0);
    assert(atomic_count == 40000);
    DWORD before = UINT32_MAX - 4, after = 3;
    assert((DWORD)(after - before) == 8);
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs); EnterCriticalSection(&cs); EnterCriticalSection(&cs);
    LeaveCriticalSection(&cs); LeaveCriticalSection(&cs); DeleteCriticalSection(&cs);
}

typedef struct { PlatformEvent *event; volatile LONG started, finished; } WaitState;
static DWORD event_waiter(LPVOID opaque) {
    WaitState* state = opaque;
    InterlockedIncrement(&state->started);
    assert(WaitForSingleObject(state->event, 3000) == WAIT_OBJECT_0);
    InterlockedIncrement(&state->finished);
    return 0;
}
static void wait_count(volatile LONG* count, LONG target) {
    uint64_t started = GetTickCount64();
    while (InterlockedCompareExchange(count, 0, 0) != target) {
        assert(GetTickCount64() - started < 2000); Sleep(1);
    }
}
static void event_semantics(int manual) {
    WaitState state = {0};
    state.event = CreateEventA(NULL, manual, FALSE, NULL); assert(state.event);
    assert(WaitForSingleObject(state.event, 0) == WAIT_TIMEOUT);
    uint64_t before = GetTickCount64();
    assert(WaitForSingleObject(state.event, 10) == WAIT_TIMEOUT);
    assert(GetTickCount64() - before >= 8);
    pthread_t a = CreateThreadSimple(0, event_waiter, &state);
    pthread_t b = CreateThreadSimple(0, event_waiter, &state); assert(a && b);
    wait_count(&state.started, 2); SetEvent(state.event);
    if (!manual) {
        wait_count(&state.finished, 1); Sleep(10);
        assert(InterlockedCompareExchange(&state.finished, 0, 0) == 1);
        SetEvent(state.event);
    }
    assert(platform_thread_join(a, 3000) == WAIT_OBJECT_0);
    assert(platform_thread_join(b, 3000) == WAIT_OBJECT_0);
    assert(state.finished == 2);
    assert(WaitForSingleObject(state.event, 0) == (manual ? WAIT_OBJECT_0 : WAIT_TIMEOUT));
    SetEvent(state.event); ResetEvent(state.event);
    assert(WaitForSingleObject(state.event, 0) == WAIT_TIMEOUT);
    platform_event_destroy(state.event);
}

typedef struct { PlatformEvent *request, *reply; int rounds; } Handoff;
static DWORD guest_handoff(LPVOID opaque) {
    Handoff* h = opaque;
    for (int i = 0; i < h->rounds; ++i) {
        assert(WaitForSingleObject(h->request, 3000) == WAIT_OBJECT_0);
        SetEvent(h->reply);
    }
    return 0;
}
static void scheduler_handoff(void) {
    Handoff h = {CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL), 10000};
    assert(h.request && h.reply);
    pthread_t guest = CreateThreadSimple(0, guest_handoff, &h); assert(guest);
    for (int i = 0; i < h.rounds; ++i) {
        SetEvent(h.request);
        assert(WaitForSingleObject(h.reply, 3000) == WAIT_OBJECT_0);
    }
    assert(platform_thread_join(guest, 5000) == WAIT_OBJECT_0);
    platform_event_destroy(h.request); platform_event_destroy(h.reply);
}
static DWORD delayed_thread(LPVOID unused) { (void)unused; Sleep(80); return 0; }
static volatile sig_atomic_t interrupted;
static void interrupt_handler(int signal) { (void)signal; interrupted = 1; }
static DWORD interrupt_sleep(LPVOID opaque) {
    pthread_t* thread = opaque;
    Sleep(10); assert(pthread_kill(*thread, SIGUSR1) == 0); return 0;
}
static void bounded_waits(void) {
    assert(WaitForSingleObject(NULL, 0) == WAIT_FAILED);
    pthread_t delayed = CreateThreadSimple(0, delayed_thread, NULL); assert(delayed);
    assert(platform_thread_join(delayed, 0) == WAIT_TIMEOUT);
    uint64_t before = GetTickCount64();
    assert(platform_thread_join(delayed, 10) == WAIT_TIMEOUT);
    assert(GetTickCount64() - before >= 8);
    assert(platform_thread_join(delayed, 1000) == WAIT_OBJECT_0);
    struct sigaction action = {0}; action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask); assert(sigaction(SIGUSR1, &action, NULL) == 0);
    pthread_t self = pthread_self();
    pthread_t sender = CreateThreadSimple(0, interrupt_sleep, &self); assert(sender);
    before = GetTickCount64(); Sleep(50);
    assert(interrupted && GetTickCount64() - before >= 48);
    assert(platform_thread_join(sender, 1000) == WAIT_OBJECT_0);
}
int main(void) {
    atomics_and_widths(); event_semantics(FALSE); event_semantics(TRUE);
    scheduler_handoff(); bounded_waits();
    puts("PASS: Linux Win32 widths, atomics, recursive lock, event reset/wake, 10000 scheduler handoffs, finite waits and interrupted sleep");
    return 0;
}
