/* PS4 mutexes, condition variables and read/write locks on Windows primitives.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Contracts from bbport's src/runtime_mutex.c and runtime_rwlock.c; static-initializer values,
 * per-type relock behaviour and error codes checked against shadPS4's
 * src/core/libraries/kernel/threads/{mutex,condvar,rwlock}.cpp.
 *
 * Windows has no timed or recursive SRWLOCK and its condition variables need their own lock,
 * so each guest object keeps its state (owner, count) under a short internal SRWLOCK and
 * blocks on a CONDITION_VARIABLE. Guest handles are pointers stored in a 64-bit slot:
 * mutex 0 = static initializer, 1 = adaptive initializer, 2 = destroyed;
 * cond and rwlock 0 = static initializer, 1 = destroyed. */
#include "gz.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>

enum { TYPE_ERRORCHECK = 1, TYPE_RECURSIVE = 2, TYPE_NORMAL = 3, TYPE_ADAPTIVE = 4 };
#define MUTEX_ADAPTIVE_INIT ((uintptr_t)1)
#define MUTEX_DESTROYED ((uintptr_t)2)
#define OBJECT_DESTROYED ((uintptr_t)1)

/* One lock creates every statically initialized handle, so racing first users agree. */
static SRWLOCK static_init = SRWLOCK_INIT;

/* ---- mutex attributes ---- */
typedef struct {
    int type, protocol;
} MutexAttr;
static ABI int32_t mutexattr_init(MutexAttr **out) {
    if (!out)
        return GZ_ERR(GZ_EINVAL);
    MutexAttr *a = malloc(sizeof(*a));
    if (!a)
        return GZ_ERR(GZ_ENOMEM);
    a->type = TYPE_ERRORCHECK;
    a->protocol = 0;
    *out = a;
    return 0;
}
static ABI int32_t mutexattr_settype(MutexAttr **attr, int type) {
    if (!attr || !*attr || type < TYPE_ERRORCHECK || type > TYPE_ADAPTIVE)
        return GZ_ERR(GZ_EINVAL);
    (*attr)->type = type;
    return 0;
}
/* Priority protocols need a PS4 scheduler: validated and recorded only (as bbport). */
static ABI int32_t mutexattr_setprotocol(MutexAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2)
        return GZ_ERR(GZ_EINVAL);
    (*attr)->protocol = protocol;
    return 0;
}
static ABI int32_t mutexattr_destroy(MutexAttr **attr) {
    if (!attr || !*attr)
        return GZ_ERR(GZ_EINVAL);
    free(*attr);
    *attr = NULL;
    return 0;
}

/* ---- mutex ---- */
typedef struct {
    SRWLOCK guard;               /* protects owner/count; held only briefly */
    CONDITION_VARIABLE released; /* owner became 0 */
    DWORD owner;                 /* Windows thread id, 0 = free */
    uint32_t count;
    int type;
    char name[32];
} Mutex;

static int32_t mutex_create(Mutex **out, int type, const char *name) {
    Mutex *m = calloc(1, sizeof(*m));
    if (!m)
        return GZ_ERR(GZ_ENOMEM);
    InitializeSRWLock(&m->guard);
    InitializeConditionVariable(&m->released);
    m->type = type;
    if (name)
        strncpy(m->name, name, sizeof(m->name) - 1);
    *out = m;
    return 0;
}
static ABI int32_t mutex_init(Mutex **out, MutexAttr **attr, const char *name) {
    if (!out || (attr && !*attr))
        return GZ_ERR(GZ_EINVAL);
    return mutex_create(out, attr ? (*attr)->type : TYPE_ERRORCHECK, name);
}
static int32_t mutex_get(Mutex **slot, Mutex **out) {
    if (!slot)
        return GZ_ERR(GZ_EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
    if (value == MUTEX_DESTROYED)
        return GZ_ERR(GZ_EINVAL);
    if (value > MUTEX_DESTROYED) {
        *out = (Mutex *)value;
        return 0;
    }
    int32_t e = 0;
    AcquireSRWLockExclusive(&static_init);
    value = *(uintptr_t *)slot;
    if (value < MUTEX_DESTROYED) {
        Mutex *m;
        e = mutex_create(&m, value == MUTEX_ADAPTIVE_INIT ? TYPE_ADAPTIVE : TYPE_ERRORCHECK, NULL);
        if (!e) {
            __atomic_store_n((uintptr_t *)slot, (uintptr_t)m, __ATOMIC_RELEASE);
            value = (uintptr_t)m;
        }
    } else if (value == MUTEX_DESTROYED)
        e = GZ_ERR(GZ_EINVAL);
    ReleaseSRWLockExclusive(&static_init);
    if (!e)
        *out = (Mutex *)value;
    return e;
}
/* timeout_ms: INFINITE to block, 0 for trylock, else a relative wait. The guard is held on entry
 * and on return. */
static int32_t mutex_acquire_locked(Mutex *m, DWORD timeout_ms, uint32_t count) {
    DWORD self = GetCurrentThreadId();
    if (m->owner == self) {
        if (m->type == TYPE_RECURSIVE) {
            if (m->count + count < m->count)
                return GZ_ERR(GZ_EAGAIN);
            m->count += count;
            return 0;
        }
        if (timeout_ms == 0)
            return GZ_ERR(GZ_EBUSY);
        if (timeout_ms != INFINITE) { /* shadPS4: a timed self-lock sleeps out its timeout */
            ReleaseSRWLockExclusive(&m->guard);
            Sleep(timeout_ms);
            AcquireSRWLockExclusive(&m->guard);
            return GZ_ERR(GZ_ETIMEDOUT);
        }
        if (m->type == TYPE_NORMAL)
            gz_stop(GZ_EXIT_RUNTIME_STOP,
                    "STOP: guest thread %lu relocked a NORMAL mutex it owns ('%s'): deadlock", self,
                    m->name);
        return GZ_ERR(GZ_EDEADLK);
    }
    ULONGLONG deadline = timeout_ms == INFINITE ? 0 : GetTickCount64() + timeout_ms;
    while (m->owner) {
        if (timeout_ms == 0)
            return GZ_ERR(GZ_EBUSY);
        DWORD wait = INFINITE;
        if (timeout_ms != INFINITE) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                return GZ_ERR(GZ_ETIMEDOUT);
            wait = (DWORD)(deadline - now);
        }
        SleepConditionVariableSRW(&m->released, &m->guard, wait, 0);
    }
    m->owner = self;
    m->count = count;
    return 0;
}
static int32_t mutex_acquire(Mutex **slot, DWORD timeout_ms) {
    Mutex *m;
    int32_t e = mutex_get(slot, &m);
    if (e)
        return e;
    AcquireSRWLockExclusive(&m->guard);
    e = mutex_acquire_locked(m, timeout_ms, 1);
    ReleaseSRWLockExclusive(&m->guard);
    return e;
}
static ABI int32_t mutex_lock(Mutex **slot) {
    return mutex_acquire(slot, INFINITE);
}
static ABI int32_t mutex_trylock(Mutex **slot) {
    return mutex_acquire(slot, 0);
}
static ABI int32_t mutex_timedlock(Mutex **slot, uint32_t usec) {
    return mutex_acquire(slot, gz_usec_to_ms(usec));
}
static ABI int32_t mutex_unlock(Mutex **slot) {
    if (!slot)
        return GZ_ERR(GZ_EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
    if (value == MUTEX_DESTROYED)
        return GZ_ERR(GZ_EINVAL);
    if (value < MUTEX_DESTROYED)
        return GZ_ERR(GZ_EPERM);
    Mutex *m = (Mutex *)value;
    int32_t e = 0;
    AcquireSRWLockExclusive(&m->guard);
    if (m->owner != GetCurrentThreadId())
        e = GZ_ERR(GZ_EPERM);
    else if (--m->count == 0) {
        m->owner = 0;
        WakeConditionVariable(&m->released);
    }
    ReleaseSRWLockExclusive(&m->guard);
    return e;
}
static ABI int32_t mutex_destroy(Mutex **slot) {
    if (!slot)
        return GZ_ERR(GZ_EINVAL);
    uintptr_t value = *(uintptr_t *)slot;
    if (value == MUTEX_DESTROYED)
        return GZ_ERR(GZ_EINVAL);
    if (value > MUTEX_DESTROYED) {
        Mutex *m = (Mutex *)value;
        AcquireSRWLockExclusive(&m->guard);
        int busy = m->owner != 0;
        ReleaseSRWLockExclusive(&m->guard);
        if (busy)
            return GZ_ERR(GZ_EBUSY);
        free(m);
    }
    *(uintptr_t *)slot = MUTEX_DESTROYED;
    return 0;
}

/* ---- condition variables ---- */
/* The waiter takes the cond's lock before it releases the guest mutex and keeps it until it
 * sleeps, so a signal sent after the mutex is released always finds it waiting (no lost wakeup).
 * `sequence` filters spurious Windows wakeups. */
typedef struct {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    uint64_t sequence;
} Cond;
static int32_t cond_create(Cond **out) {
    Cond *c = calloc(1, sizeof(*c));
    if (!c)
        return GZ_ERR(GZ_ENOMEM);
    InitializeSRWLock(&c->lock);
    InitializeConditionVariable(&c->cv);
    *out = c;
    return 0;
}
static ABI int32_t condattr_init(void **out) {
    if (!out)
        return GZ_ERR(GZ_EINVAL);
    *out = calloc(1, 16);
    return *out ? 0 : GZ_ERR(GZ_ENOMEM);
}
static ABI int32_t condattr_destroy(void **attr) {
    if (!attr || !*attr)
        return GZ_ERR(GZ_EINVAL);
    free(*attr);
    *attr = NULL;
    return 0;
}
static ABI int32_t cond_init(Cond **out, void **attr, const char *name) {
    (void)attr;
    (void)name;
    if (!out)
        return GZ_ERR(GZ_EINVAL);
    return cond_create(out);
}
static int32_t cond_get(Cond **slot, Cond **out) {
    if (!slot)
        return GZ_ERR(GZ_EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
    if (value == OBJECT_DESTROYED)
        return GZ_ERR(GZ_EINVAL);
    if (value) {
        *out = (Cond *)value;
        return 0;
    }
    int32_t e = 0;
    AcquireSRWLockExclusive(&static_init);
    value = *(uintptr_t *)slot;
    if (!value) {
        Cond *c;
        e = cond_create(&c);
        if (!e) {
            __atomic_store_n((uintptr_t *)slot, (uintptr_t)c, __ATOMIC_RELEASE);
            value = (uintptr_t)c;
        }
    } else if (value == OBJECT_DESTROYED)
        e = GZ_ERR(GZ_EINVAL);
    ReleaseSRWLockExclusive(&static_init);
    if (!e)
        *out = (Cond *)value;
    return e;
}
static int32_t cond_wait_ms(Cond **slot, Mutex **mutex_slot, DWORD timeout_ms) {
    Cond *c;
    int32_t e = cond_get(slot, &c);
    if (e)
        return e;
    if (!mutex_slot || *(uintptr_t *)mutex_slot <= MUTEX_DESTROYED)
        return GZ_ERR(GZ_EINVAL);
    Mutex *m = *mutex_slot;
    DWORD self = GetCurrentThreadId();
    AcquireSRWLockExclusive(&c->lock);
    /* Release the guest mutex completely (a recursive one may be held several times). */
    AcquireSRWLockExclusive(&m->guard);
    if (m->owner != self) {
        ReleaseSRWLockExclusive(&m->guard);
        ReleaseSRWLockExclusive(&c->lock);
        return GZ_ERR(GZ_EPERM);
    }
    uint32_t count = m->count;
    m->owner = 0;
    m->count = 0;
    WakeConditionVariable(&m->released);
    ReleaseSRWLockExclusive(&m->guard);
    /* Sleep until signalled or timed out. */
    uint64_t seen = c->sequence;
    ULONGLONG deadline = timeout_ms == INFINITE ? 0 : GetTickCount64() + timeout_ms;
    int timed_out = 0;
    while (c->sequence == seen) {
        DWORD wait = INFINITE;
        if (timeout_ms != INFINITE) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                timed_out = 1;
                break;
            }
            wait = (DWORD)(deadline - now);
        }
        SleepConditionVariableSRW(&c->cv, &c->lock, wait, 0);
    }
    ReleaseSRWLockExclusive(&c->lock);
    /* Take the mutex back with its previous count. */
    AcquireSRWLockExclusive(&m->guard);
    mutex_acquire_locked(m, INFINITE, count);
    ReleaseSRWLockExclusive(&m->guard);
    return timed_out ? GZ_ERR(GZ_ETIMEDOUT) : 0;
}
static ABI int32_t cond_wait(Cond **c, Mutex **m) {
    return cond_wait_ms(c, m, INFINITE);
}
static ABI int32_t cond_timedwait(Cond **c, Mutex **m, uint32_t usec) {
    return cond_wait_ms(c, m, gz_usec_to_ms(usec));
}
static int32_t cond_wake(Cond **slot, int all) {
    Cond *c;
    int32_t e = cond_get(slot, &c);
    if (e)
        return e;
    AcquireSRWLockExclusive(&c->lock);
    ++c->sequence;
    if (all)
        WakeAllConditionVariable(&c->cv);
    else
        WakeConditionVariable(&c->cv);
    ReleaseSRWLockExclusive(&c->lock);
    return 0;
}
static ABI int32_t cond_signal(Cond **c) {
    return cond_wake(c, 0);
}
static ABI int32_t cond_broadcast(Cond **c) {
    return cond_wake(c, 1);
}
static ABI int32_t cond_destroy(Cond **slot) {
    if (!slot)
        return GZ_ERR(GZ_EINVAL);
    uintptr_t value = *(uintptr_t *)slot;
    if (value == OBJECT_DESTROYED)
        return GZ_ERR(GZ_EINVAL);
    if (value)
        free((Cond *)value);
    *(uintptr_t *)slot = OBJECT_DESTROYED;
    return 0;
}

/* ---- read/write locks ---- */
/* A guest unlock does not say whether it releases a read or a write lock: the writer is
 * remembered by thread id, anything else is a reader. */
typedef struct {
    SRWLOCK guard;
    CONDITION_VARIABLE changed;
    DWORD writer;
    uint32_t readers;
} Rwlock;
static int32_t rwlock_create(Rwlock **out) {
    Rwlock *r = calloc(1, sizeof(*r));
    if (!r)
        return GZ_ERR(GZ_ENOMEM);
    InitializeSRWLock(&r->guard);
    InitializeConditionVariable(&r->changed);
    *out = r;
    return 0;
}
static ABI int32_t rwlock_init(Rwlock **out, void **attr, const char *name) {
    (void)attr;
    (void)name;
    if (!out)
        return GZ_ERR(GZ_EINVAL);
    return rwlock_create(out);
}
static int32_t rwlock_get(Rwlock **slot, Rwlock **out) {
    if (!slot)
        return GZ_ERR(GZ_EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
    if (value == OBJECT_DESTROYED)
        return GZ_ERR(GZ_EINVAL);
    if (value) {
        *out = (Rwlock *)value;
        return 0;
    }
    int32_t e = 0;
    AcquireSRWLockExclusive(&static_init);
    value = *(uintptr_t *)slot;
    if (!value) {
        Rwlock *r;
        e = rwlock_create(&r);
        if (!e) {
            __atomic_store_n((uintptr_t *)slot, (uintptr_t)r, __ATOMIC_RELEASE);
            value = (uintptr_t)r;
        }
    } else if (value == OBJECT_DESTROYED)
        e = GZ_ERR(GZ_EINVAL);
    ReleaseSRWLockExclusive(&static_init);
    if (!e)
        *out = (Rwlock *)value;
    return e;
}
static int32_t rwlock_take(Rwlock **slot, int write, int try_only) {
    Rwlock *r;
    int32_t e = rwlock_get(slot, &r);
    if (e)
        return e;
    DWORD self = GetCurrentThreadId();
    AcquireSRWLockExclusive(&r->guard);
    if (r->writer == self)
        e = try_only ? GZ_ERR(GZ_EBUSY) : GZ_ERR(GZ_EDEADLK);
    else {
        while (r->writer || (write && r->readers)) {
            if (try_only) {
                e = GZ_ERR(GZ_EBUSY);
                break;
            }
            SleepConditionVariableSRW(&r->changed, &r->guard, INFINITE, 0);
        }
        if (!e) {
            if (write)
                r->writer = self;
            else
                ++r->readers;
        }
    }
    ReleaseSRWLockExclusive(&r->guard);
    return e;
}
static ABI int32_t rwlock_rdlock(Rwlock **r) {
    return rwlock_take(r, 0, 0);
}
static ABI int32_t rwlock_wrlock(Rwlock **r) {
    return rwlock_take(r, 1, 0);
}
static ABI int32_t rwlock_tryrdlock(Rwlock **r) {
    return rwlock_take(r, 0, 1);
}
static ABI int32_t rwlock_trywrlock(Rwlock **r) {
    return rwlock_take(r, 1, 1);
}
static ABI int32_t rwlock_unlock(Rwlock **slot) {
    if (!slot || *(uintptr_t *)slot <= OBJECT_DESTROYED)
        return GZ_ERR(GZ_EINVAL);
    Rwlock *r = *slot;
    int32_t e = 0;
    AcquireSRWLockExclusive(&r->guard);
    if (r->writer == GetCurrentThreadId())
        r->writer = 0;
    else if (r->readers)
        --r->readers;
    else
        e = GZ_ERR(GZ_EPERM);
    if (!e)
        WakeAllConditionVariable(&r->changed);
    ReleaseSRWLockExclusive(&r->guard);
    return e;
}
static ABI int32_t rwlock_destroy(Rwlock **slot) {
    if (!slot)
        return GZ_ERR(GZ_EINVAL);
    uintptr_t value = *(uintptr_t *)slot;
    if (value == OBJECT_DESTROYED)
        return GZ_ERR(GZ_EINVAL);
    if (value) {
        Rwlock *r = (Rwlock *)value;
        if (r->writer || r->readers)
            return GZ_ERR(GZ_EBUSY);
        free(r);
    }
    *(uintptr_t *)slot = OBJECT_DESTROYED;
    return 0;
}

static const GzExport exports[] = {
    {"libkernel", "scePthreadMutexattrInit", (void *)mutexattr_init},
    {"libkernel", "scePthreadMutexattrSettype", (void *)mutexattr_settype},
    {"libkernel", "scePthreadMutexattrSetprotocol", (void *)mutexattr_setprotocol},
    {"libkernel", "scePthreadMutexattrDestroy", (void *)mutexattr_destroy},
    {"libkernel", "scePthreadMutexInit", (void *)mutex_init},
    {"libkernel", "scePthreadMutexLock", (void *)mutex_lock},
    {"libkernel", "scePthreadMutexTrylock", (void *)mutex_trylock},
    {"libkernel", "scePthreadMutexTimedlock", (void *)mutex_timedlock},
    {"libkernel", "scePthreadMutexUnlock", (void *)mutex_unlock},
    {"libkernel", "scePthreadMutexDestroy", (void *)mutex_destroy},
    {"libkernel", "scePthreadCondattrInit", (void *)condattr_init},
    {"libkernel", "scePthreadCondattrDestroy", (void *)condattr_destroy},
    {"libkernel", "scePthreadCondInit", (void *)cond_init},
    {"libkernel", "scePthreadCondWait", (void *)cond_wait},
    {"libkernel", "scePthreadCondTimedwait", (void *)cond_timedwait},
    {"libkernel", "scePthreadCondSignal", (void *)cond_signal},
    {"libkernel", "scePthreadCondBroadcast", (void *)cond_broadcast},
    {"libkernel", "scePthreadCondDestroy", (void *)cond_destroy},
    {"libkernel", "scePthreadRwlockInit", (void *)rwlock_init},
    {"libkernel", "scePthreadRwlockRdlock", (void *)rwlock_rdlock},
    {"libkernel", "scePthreadRwlockWrlock", (void *)rwlock_wrlock},
    {"libkernel", "scePthreadRwlockTryrdlock", (void *)rwlock_tryrdlock},
    {"libkernel", "scePthreadRwlockTrywrlock", (void *)rwlock_trywrlock},
    {"libkernel", "scePthreadRwlockUnlock", (void *)rwlock_unlock},
    {"libkernel", "scePthreadRwlockDestroy", (void *)rwlock_destroy},
};
uintptr_t gz_sync_resolve(const char *library, const char *symbol) {
    return GZ_LOOKUP(exports, library, symbol);
}
