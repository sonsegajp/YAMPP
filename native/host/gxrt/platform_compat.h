/* Cross-platform compatibility layer: Win32 ↔ POSIX.
 * Include this instead of <windows.h> in gxrt source files. */
#ifndef MELEE_PLATFORM_COMPAT_H
#define MELEE_PLATFORM_COMPAT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef _WIN32
#include <windows.h>
#else

#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/syscall.h>

#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif

/* Preserve Win32 widths on LP64 hosts (Linux long is 64-bit). */
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef void* LPVOID;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

/* Atomic operations */
static inline LONG InterlockedExchange(volatile LONG* target, LONG value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
static inline LONG InterlockedCompareExchange(volatile LONG* target, LONG value, LONG comparand) {
    __atomic_compare_exchange_n(target, &comparand, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
static inline LONG InterlockedIncrement(volatile LONG* target) {
    return __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST);
}

/* High-resolution timing */
typedef struct { long long QuadPart; } LARGE_INTEGER;
static inline void QueryPerformanceCounter(LARGE_INTEGER* li) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    li->QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static inline void QueryPerformanceFrequency(LARGE_INTEGER* li) {
    li->QuadPart = 1000000000LL; /* nanoseconds */
}
static inline uint64_t GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static inline DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

/* Preserve finite millisecond waits and resume after a signal interruption. */
static inline void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec remaining = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
}
static inline void SwitchToThread(void) { sched_yield(); }

/* Process control */
static inline void ExitProcess(unsigned code) { _exit((int)code); }
#define TerminateProcess(h, code) _exit((int)(code))
#define GetCurrentProcessId() ((DWORD)getpid())
static inline DWORD GetCurrentThreadId(void) { return (DWORD)syscall(SYS_gettid); }

/* Directory creation */
static inline int CreateDirectoryA(const char* path, void* sa) {
    (void)sa;
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}
#define ERROR_ALREADY_EXISTS EEXIST
static inline DWORD GetLastError(void) { return (DWORD)errno; }

/* Dynamic loading */
#define HMODULE void*
static inline void* LoadLibraryA(const char* path) { return dlopen(path, RTLD_NOW); }
static inline void* GetProcAddress(void* module, const char* name) { return dlsym(module, name); }
static inline void* GetModuleHandleA(const char* name) { return dlopen(name, RTLD_NOW | RTLD_NOLOAD); }

/* Critical section → pthread mutex */
typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &attr);
    pthread_mutexattr_destroy(&attr);
}
static inline void EnterCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_lock(cs); }
static inline void LeaveCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_unlock(cs); }
static inline void DeleteCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_destroy(cs); }

/* SRWLOCK → pthread rwlock */
typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void AcquireSRWLockExclusive(SRWLOCK* lock) { pthread_rwlock_wrlock(lock); }
static inline void ReleaseSRWLockExclusive(SRWLOCK* lock) { pthread_rwlock_unlock(lock); }
static inline void AcquireSRWLockShared(SRWLOCK* lock) { pthread_rwlock_rdlock(lock); }
static inline void ReleaseSRWLockShared(SRWLOCK* lock) { pthread_rwlock_unlock(lock); }

/* Event handles are distinct from pthread_t; threads use platform_thread_join. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int signaled;
    int manual_reset;
} PlatformEvent;

#define HANDLE PlatformEvent*
#define INVALID_HANDLE_VALUE ((PlatformEvent*)0)
#define WAIT_OBJECT_0 ((DWORD)0)
#define WAIT_TIMEOUT ((DWORD)258)
#define WAIT_FAILED ((DWORD)0xFFFFFFFFu)
#define INFINITE ((DWORD)0xFFFFFFFFu)

static inline PlatformEvent* CreateEventA(void* sa, int manual, int initial, const char* name) {
    (void)sa; (void)name;
    PlatformEvent* ev = (PlatformEvent*)calloc(1, sizeof(PlatformEvent));
    if (!ev) return NULL;
    if (pthread_mutex_init(&ev->mutex, NULL) != 0) { free(ev); return NULL; }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&ev->mutex); free(ev); return NULL;
    }
    int error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!error) error = pthread_cond_init(&ev->cond, &attr);
    pthread_condattr_destroy(&attr);
    if (error) { pthread_mutex_destroy(&ev->mutex); free(ev); return NULL; }
    ev->signaled = initial != 0;
    ev->manual_reset = manual != 0;
    return ev;
}
static inline int SetEvent(PlatformEvent* ev) {
    if (!ev) return 0;
    pthread_mutex_lock(&ev->mutex);
    ev->signaled = 1;
    if (ev->manual_reset) pthread_cond_broadcast(&ev->cond);
    else pthread_cond_signal(&ev->cond);
    pthread_mutex_unlock(&ev->mutex);
    return 1;
}
static inline int ResetEvent(PlatformEvent* ev) {
    if (!ev) return 0;
    pthread_mutex_lock(&ev->mutex);
    ev->signaled = 0;
    pthread_mutex_unlock(&ev->mutex);
    return 1;
}
static inline DWORD WaitForSingleObject(PlatformEvent* ev, DWORD timeout) {
    if (!ev) return WAIT_FAILED;
    pthread_mutex_lock(&ev->mutex);
    struct timespec deadline;
    int error = 0;
    if (timeout != INFINITE) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout / 1000u;
        deadline.tv_nsec += (long)(timeout % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }
    while (!ev->signaled && !error) {
        if (!timeout) { error = ETIMEDOUT; break; }
        error = timeout == INFINITE ? pthread_cond_wait(&ev->cond, &ev->mutex)
            : pthread_cond_timedwait(&ev->cond, &ev->mutex, &deadline);
    }
    DWORD result = ev->signaled ? WAIT_OBJECT_0 : error == ETIMEDOUT ? WAIT_TIMEOUT : WAIT_FAILED;
    if (ev->signaled && !ev->manual_reset) ev->signaled = 0;
    pthread_mutex_unlock(&ev->mutex);
    return result;
}
/* Caller must first join all waiters, as with closing an in-use Win32 handle. */
static inline void platform_event_destroy(PlatformEvent* ev) {
    if (!ev) return;
    pthread_cond_destroy(&ev->cond);
    pthread_mutex_destroy(&ev->mutex);
    free(ev);
}

/* Thread creation */
typedef struct {
    pthread_t thread;
    int valid;
} PlatformThread;

typedef DWORD (*PlatformThreadProc)(LPVOID);
typedef struct {
    PlatformThreadProc proc;
    LPVOID param;
} PlatformThreadArgs;

static inline void* _platform_thread_wrapper(void* arg) {
    PlatformThreadArgs* ta = (PlatformThreadArgs*)arg;
    PlatformThreadProc proc = ta->proc;
    LPVOID param = ta->param;
    free(ta);
    proc(param);
    return NULL;
}

/* Thread handles use platform_thread_join, never the event wait function. */
/* Returns 0 on failure (callers must check). */
static inline pthread_t CreateThreadSimple(size_t stack_size,
                                           PlatformThreadProc proc,
                                           LPVOID param) {
    pthread_t t = 0;
    PlatformThreadArgs* ta = (PlatformThreadArgs*)malloc(sizeof(*ta));
    if (!ta) return 0;
    ta->proc = proc;
    ta->param = param;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size) pthread_attr_setstacksize(&attr, stack_size);
    if (pthread_create(&t, &attr, _platform_thread_wrapper, ta) != 0) {
        free(ta);
        t = 0;
    }
    pthread_attr_destroy(&attr);
    return t;
}

static inline DWORD platform_thread_join(pthread_t t, DWORD timeout_ms) {
    int error;
    if (timeout_ms == INFINITE) error = pthread_join(t, NULL);
    else if (!timeout_ms) error = pthread_tryjoin_np(t, NULL);
    else {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000u;
        deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
        error = pthread_timedjoin_np(t, NULL, &deadline);
    }
    return !error ? WAIT_OBJECT_0 : (error == ETIMEDOUT || error == EBUSY) ? WAIT_TIMEOUT : WAIT_FAILED;
}

/* File operations */
static inline int MoveFileExA(const char* src, const char* dst, DWORD flags) {
    (void)flags;
    return rename(src, dst) == 0;
}
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8

/* Signal-based fault handler (replaces VEH) */
#define EXCEPTION_ACCESS_VIOLATION   SIGSEGV
#define EXCEPTION_ILLEGAL_INSTRUCTION SIGILL
#define EXCEPTION_INT_DIVIDE_BY_ZERO SIGFPE
#define EXCEPTION_STACK_OVERFLOW     SIGSEGV

#endif /* _WIN32 */
#endif /* MELEE_PLATFORM_COMPAT_H */
