/* PS4 threads (scePthread*), once, thread-specific keys and sleeps on Windows threads.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Contracts from bbport's src/runtime_thread.c and runtime_kernel.c
 * (github.com/deadinside28/bloodborne_pc). Godzilla has no static TLS and never reads fs:
 * so no guest TCB is built. Priorities and affinity are recorded, not
 * enforced. A guest thread ends through ExitThread after its key destructors: longjmp would
 * unwind through guest frames, which have no Windows unwind data. */
#include "gz.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATTR_MAGIC UINT32_C(0x41545452)
#define DEFAULT_STACK (1024 * 1024)
#define MIN_STACK (64 * 1024)
#define HOST_STACK_MARGIN (256 * 1024) /* runtime code and Windows calls on the guest's stack */
#define DEFAULT_PRIO 700

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct Thread {
    HANDLE handle;
    DWORD id;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, joined, host_owned;
    volatile LONG finished;
    struct Thread *next;
} Thread;

static SRWLOCK lock = SRWLOCK_INIT;
static ThreadAttr *attributes;
static Thread *threads;
static _Thread_local Thread *current;
static LONG created_count;

static const ThreadAttr default_attr = {.magic = ATTR_MAGIC,
                                        .policy = 1,
                                        .prio = DEFAULT_PRIO,
                                        .inherit = 4,
                                        .stack = DEFAULT_STACK,
                                        .guard = 4096,
                                        .affinity = 0x7f};
static Thread *new_thread(void) {
    Thread *t = calloc(1, sizeof(*t));
    if (t)
        t->attr = default_attr;
    return t;
}
static void publish(Thread *t) {
    AcquireSRWLockExclusive(&lock);
    t->next = threads;
    threads = t;
    ReleaseSRWLockExclusive(&lock);
}
static void set_host_name(const char *name) {
    WCHAR wide[32];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 32);
    wide[31] = 0;
    SetThreadDescription(GetCurrentThread(), wide);
}
/* Host threads that run guest code without being created by it (the main thread). */
static Thread *thread_current(void) {
    if (current)
        return current;
    Thread *t = new_thread();
    if (!t)
        gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: cannot allocate a thread record");
    t->id = GetCurrentThreadId();
    t->host_owned = 1;
    snprintf(t->name, sizeof(t->name), "host");
    publish(t);
    current = t;
    return t;
}
void gz_thread_attach_main(void) {
    Thread *t = thread_current();
    snprintf(t->name, sizeof(t->name), "main");
    set_host_name("guest main");
}
void gz_thread_attach_host(const char *name) {
    Thread *t = thread_current();
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "hle");
    set_host_name(t->name);
}
static Thread *find_thread(void *handle) {
    Thread *found = NULL;
    AcquireSRWLockShared(&lock);
    for (Thread *t = threads; t; t = t->next)
        if (t == handle) {
            found = t;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return found;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot)
        return NULL;
    ThreadAttr *found = NULL;
    AcquireSRWLockShared(&lock);
    for (ThreadAttr *a = attributes; a; a = a->next)
        if (a == *slot && a->magic == ATTR_MAGIC) {
            found = a;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return found;
}

/* ---- thread-specific data (keys) ---- */
#define KEYS 256
typedef void(ABI *KeyDestructor)(void *);
static struct {
    int used;
    KeyDestructor destructor;
} keys[KEYS];
static SRWLOCK key_lock = SRWLOCK_INIT;
static _Thread_local void *key_values[KEYS];
static ABI int32_t key_create(uint32_t *key, KeyDestructor destructor) {
    if (!key)
        return GZ_ERR(GZ_EINVAL);
    AcquireSRWLockExclusive(&key_lock);
    for (uint32_t i = 1; i < KEYS; ++i)
        if (!keys[i].used) {
            keys[i].used = 1;
            keys[i].destructor = destructor;
            ReleaseSRWLockExclusive(&key_lock);
            *key = i;
            return 0;
        }
    ReleaseSRWLockExclusive(&key_lock);
    return GZ_ERR(GZ_EAGAIN);
}
static ABI int32_t key_delete(uint32_t key) {
    if (!key || key >= KEYS)
        return GZ_ERR(GZ_EINVAL);
    AcquireSRWLockExclusive(&key_lock);
    int32_t r = keys[key].used ? 0 : GZ_ERR(GZ_EINVAL);
    keys[key].used = 0;
    keys[key].destructor = NULL;
    ReleaseSRWLockExclusive(&key_lock);
    return r;
}
static ABI int32_t key_set(uint32_t key, void *value) {
    if (!key || key >= KEYS || !keys[key].used)
        return GZ_ERR(GZ_EINVAL);
    key_values[key] = value;
    return 0;
}
static ABI void *key_get(uint32_t key) {
    return key && key < KEYS ? key_values[key] : NULL;
}
/* POSIX: destructors run up to 4 rounds while values remain. */
static void keys_cleanup(void) {
    for (int round = 0; round < 4; ++round) {
        int any = 0;
        for (uint32_t i = 1; i < KEYS; ++i) {
            void *value = key_values[i];
            if (!value || !keys[i].used || !keys[i].destructor)
                continue;
            key_values[i] = NULL;
            any = 1;
            keys[i].destructor(value);
        }
        if (!any)
            break;
    }
}

/* ---- once: 0 = not run, 2 = running, 1 = done ---- */
static ABI int32_t thread_once(int32_t *once, void(ABI *routine)(void)) {
    if (!once || !routine)
        return GZ_ERR(GZ_EINVAL);
    for (;;) {
        int32_t state = __atomic_load_n(once, __ATOMIC_ACQUIRE);
        if (state == 1)
            return 0;
        if (state == 0 &&
            __atomic_compare_exchange_n(once, &state, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once, 1, __ATOMIC_RELEASE);
            return 0;
        }
        SwitchToThread();
    }
}

/* ---- attributes ---- */
static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out)
        return GZ_ERR(GZ_EINVAL);
    ThreadAttr *a = malloc(sizeof(*a));
    if (!a)
        return GZ_ERR(GZ_ENOMEM);
    *a = default_attr;
    AcquireSRWLockExclusive(&lock);
    a->next = attributes;
    attributes = a;
    ReleaseSRWLockExclusive(&lock);
    *out = a;
    return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a = find_attr(slot);
    if (!a)
        return GZ_ERR(GZ_EINVAL);
    AcquireSRWLockExclusive(&lock);
    ThreadAttr **link = &attributes;
    while (*link != a)
        link = &(*link)->next;
    *link = a->next;
    a->magic = 0;
    ReleaseSRWLockExclusive(&lock);
    free(a);
    *slot = NULL;
    return 0;
}
static ABI int32_t attr_get(Thread *thread, ThreadAttr **out) {
    ThreadAttr *a = find_attr(out);
    if (!a || !thread)
        return GZ_ERR(GZ_EINVAL);
    Thread *t = find_thread(thread);
    if (!t)
        return GZ_ERR(GZ_ESRCH);
    ThreadAttr *next = a->next;
    *a = t->attr;
    a->magic = ATTR_MAGIC;
    a->next = next;
    a->detached = t->detached;
    return 0;
}
static ABI int32_t attr_set_stacksize(ThreadAttr **slot, uint64_t size) {
    ThreadAttr *a = find_attr(slot);
    if (!a || size < 16384)
        return GZ_ERR(GZ_EINVAL);
    a->stack = size;
    return 0;
}
static ABI int32_t attr_set_detachstate(ThreadAttr **slot, int state) {
    ThreadAttr *a = find_attr(slot);
    if (!a || (state != 0 && state != 1))
        return GZ_ERR(GZ_EINVAL);
    a->detached = state;
    return 0;
}
static ABI int32_t attr_set_schedpolicy(ThreadAttr **slot, int policy) {
    ThreadAttr *a = find_attr(slot);
    if (!a || policy < 1 || policy > 3)
        return GZ_ERR(GZ_EINVAL);
    a->policy = policy;
    return 0;
}
static ABI int32_t attr_set_inheritsched(ThreadAttr **slot, int inherit) {
    ThreadAttr *a = find_attr(slot);
    if (!a || (inherit != 0 && inherit != 4))
        return GZ_ERR(GZ_EINVAL);
    a->inherit = inherit;
    return 0;
}
static ABI int32_t attr_set_schedparam(ThreadAttr **slot, const int *param) {
    ThreadAttr *a = find_attr(slot);
    if (!a || !param)
        return GZ_ERR(GZ_EINVAL);
    a->prio = *param;
    return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot, uint64_t mask) {
    ThreadAttr *a = find_attr(slot);
    if (!a)
        return GZ_ERR(GZ_EINVAL);
    a->affinity = mask;
    return 0;
}
static ABI int32_t attr_get_affinity(ThreadAttr **slot, uint64_t *mask) {
    ThreadAttr *a = find_attr(slot);
    if (!a || !mask)
        return GZ_ERR(GZ_EINVAL);
    *mask = a->affinity;
    return 0;
}

/* ---- threads ---- */
static void thread_finish(Thread *t) {
    keys_cleanup();
    InterlockedExchange(&t->finished, 1);
}
void gz_thread_detach_host(void) {
    if (current && current->host_owned) {
        thread_finish(current);
        current = NULL;
    }
}
static DWORD WINAPI host_start(void *p) {
    Thread *t = p;
    current = t;
    set_host_name(t->name);
    t->result = t->entry(t->argument);
    thread_finish(t);
    return 0;
}
static ABI int32_t thread_create(Thread **out, ThreadAttr **attr_slot, GuestEntry entry,
                                 void *argument, const char *name) {
    if (!out || !entry)
        return GZ_ERR(GZ_EINVAL);
    ThreadAttr *a = NULL;
    if (attr_slot && !(a = find_attr(attr_slot)))
        return GZ_ERR(GZ_EINVAL);
    Thread *t = new_thread();
    if (!t)
        return GZ_ERR(GZ_ENOMEM);
    if (a) {
        t->attr = *a;
        t->attr.next = NULL;
    }
    t->entry = entry;
    t->argument = argument;
    t->detached = t->attr.detached;
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "guest");
    uint64_t stack = t->attr.stack < MIN_STACK ? MIN_STACK : t->attr.stack;
    publish(t);
    *out = t; /* published before the thread can run and look at itself */
    /* A stack size without STACK_SIZE_PARAM_IS_A_RESERVATION commits it all: guest code does not
     * touch its stack page by page, so a lazily growing stack would fault. */
    t->handle = CreateThread(NULL, (SIZE_T)(stack + HOST_STACK_MARGIN), host_start, t,
                             CREATE_SUSPENDED, &t->id);
    if (!t->handle)
        gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: CreateThread failed for guest thread '%s' (error %lu)",
                t->name, GetLastError());
    InterlockedIncrement(&created_count);
    gz_log("Runtime: guest thread '%s' created (id %lu, stack %llu KiB, prio %d)", t->name, t->id,
           (unsigned long long)(stack / 1024), t->attr.prio);
    ResumeThread(t->handle);
    return 0;
}
static ABI int32_t thread_join(Thread *thread, void **result) {
    Thread *t = find_thread(thread);
    if (!t || t->host_owned)
        return GZ_ERR(GZ_ESRCH);
    if (t == current)
        return GZ_ERR(GZ_EDEADLK);
    if (t->detached || t->joined)
        return GZ_ERR(GZ_EINVAL);
    t->joined = 1;
    WaitForSingleObject(t->handle, INFINITE);
    if (result)
        *result = t->result;
    return 0;
}
static ABI int32_t thread_detach(Thread *thread) {
    Thread *t = find_thread(thread);
    if (!t)
        return GZ_ERR(GZ_ESRCH);
    if (t->detached)
        return GZ_ERR(GZ_EINVAL);
    t->detached = 1;
    return 0;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    Thread *t = thread_current();
    if (t->host_owned)
        gz_stop(GZ_EXIT_RUNTIME_STOP,
                "STOP: scePthreadExit on the main thread is not supported yet");
    t->result = value;
    thread_finish(t);
    ExitThread(0);
}
static ABI void *thread_self(void) {
    return thread_current();
}
static ABI int32_t thread_equal(Thread *a, Thread *b) {
    return a == b;
}
static ABI int32_t thread_yield(void) {
    SwitchToThread();
    return 0;
}
static ABI int32_t thread_get_prio(Thread *thread, int *prio) {
    Thread *t = find_thread(thread);
    if (!t)
        return GZ_ERR(GZ_ESRCH);
    if (!prio)
        return GZ_ERR(GZ_EINVAL);
    *prio = t->attr.prio;
    return 0;
}
static ABI int32_t thread_set_prio(Thread *thread, int prio) {
    Thread *t = find_thread(thread);
    if (!t)
        return GZ_ERR(GZ_ESRCH);
    t->attr.prio = prio;
    return 0;
}
static ABI int32_t thread_set_affinity(Thread *thread, uint64_t mask) {
    Thread *t = find_thread(thread);
    if (!t)
        return GZ_ERR(GZ_ESRCH);
    t->attr.affinity = mask;
    return 0;
}
static ABI int32_t thread_get_affinity(Thread *thread, uint64_t *mask) {
    Thread *t = find_thread(thread);
    if (!t)
        return GZ_ERR(GZ_ESRCH);
    if (!mask)
        return GZ_ERR(GZ_EINVAL);
    *mask = t->attr.affinity;
    return 0;
}

/* ---- sleeps ---- */
/* Windows' default timer granularity is 15.6 ms: short sleeps use a high-resolution waitable
 * timer (Windows 10 1803+), one per thread. */
static _Thread_local HANDLE sleep_timer;
static void sleep_us(uint64_t usec) {
    if (!usec) {
        SwitchToThread();
        return;
    }
    if (!sleep_timer)
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_ALL_ACCESS);
    if (sleep_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(usec * 10); /* relative, 100 ns units */
        if (SetWaitableTimer(sleep_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(sleep_timer, INFINITE);
            return;
        }
    }
    Sleep(gz_usec_to_ms(usec));
}
static ABI int32_t kernel_usleep(uint32_t usec) {
    sleep_us(usec);
    return 0;
}
static ABI int32_t kernel_sleep(uint32_t seconds) {
    sleep_us((uint64_t)seconds * 1000000);
    return 0;
}

static const GzExport exports[] = {
    {"libkernel", "scePthreadSelf", (void *)thread_self},
    {"libkernel", "pthread_self", (void *)thread_self},
    {"libkernel", "scePthreadEqual", (void *)thread_equal},
    {"libkernel", "pthread_equal", (void *)thread_equal},
    {"libkernel", "scePthreadCreate", (void *)thread_create},
    {"libkernel", "scePthreadJoin", (void *)thread_join},
    {"libkernel", "scePthreadDetach", (void *)thread_detach},
    {"libkernel", "scePthreadExit", (void *)thread_exit},
    {"libkernel", "scePthreadYield", (void *)thread_yield},
    {"libkernel", "scePthreadGetprio", (void *)thread_get_prio},
    {"libkernel", "scePthreadSetprio", (void *)thread_set_prio},
    {"libkernel", "scePthreadSetaffinity", (void *)thread_set_affinity},
    {"libkernel", "scePthreadGetaffinity", (void *)thread_get_affinity},
    {"libkernel", "scePthreadAttrInit", (void *)attr_init},
    {"libkernel", "scePthreadAttrDestroy", (void *)attr_destroy},
    {"libkernel", "scePthreadAttrGet", (void *)attr_get},
    {"libkernel", "scePthreadAttrSetstacksize", (void *)attr_set_stacksize},
    {"libkernel", "scePthreadAttrSetdetachstate", (void *)attr_set_detachstate},
    {"libkernel", "scePthreadAttrSetschedpolicy", (void *)attr_set_schedpolicy},
    {"libkernel", "scePthreadAttrSetinheritsched", (void *)attr_set_inheritsched},
    {"libkernel", "scePthreadAttrSetschedparam", (void *)attr_set_schedparam},
    {"libkernel", "scePthreadAttrSetaffinity", (void *)attr_set_affinity},
    {"libkernel", "scePthreadAttrGetaffinity", (void *)attr_get_affinity},
    {"libkernel", "scePthreadOnce", (void *)thread_once},
    {"libkernel", "scePthreadKeyCreate", (void *)key_create},
    {"libkernel", "scePthreadKeyDelete", (void *)key_delete},
    {"libkernel", "scePthreadSetspecific", (void *)key_set},
    {"libkernel", "scePthreadGetspecific", (void *)key_get},
    {"libkernel", "sceKernelUsleep", (void *)kernel_usleep},
    {"libkernel", "sceKernelSleep", (void *)kernel_sleep},
};
uintptr_t gz_thread_resolve(const char *library, const char *symbol) {
    return GZ_LOOKUP(exports, library, symbol);
}
