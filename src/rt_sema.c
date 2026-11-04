/* PS4 kernel semaphores and event flags on Windows SRW locks and condition variables.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Semaphores: bbport's src/runtime_sema.c (32-bit ids, counted tokens handed to FIFO waiters;
 * priority order approximated by FIFO). Event flags: shadPS4's
 * src/core/libraries/kernel/threads/event_flag.cpp (bbport has none). */
#include "gz.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* ---- semaphores ---- */
typedef struct Waiter {
    int32_t need, result;
    int done;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    Waiter *first;
    CONDITION_VARIABLE changed;
    char name[32];
    struct Sema *next;
} Sema;
static SRWLOCK sema_lock = SRWLOCK_INIT;
static Sema *registry;
static uint32_t next_id = 1;

static Sema *find(uint32_t id) {
    for (Sema *s = registry; s; s = s->next)
        if (s->id == id)
            return s;
    return NULL;
}
static ABI int32_t sem_create(uint32_t *out, const char *name, uint32_t attr, int32_t initial,
                              int32_t maximum, const void *options) {
    if (!out || !name || attr > 2 || initial < 0 || maximum <= 0 || initial > maximum)
        return GZ_ERR(GZ_EINVAL);
    if (options)
        gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: semaphore options are not implemented");
    Sema *s = calloc(1, sizeof(*s));
    if (!s)
        return GZ_ERR(GZ_ENOMEM);
    InitializeConditionVariable(&s->changed);
    strncpy(s->name, name, sizeof(s->name) - 1);
    AcquireSRWLockExclusive(&sema_lock);
    s->id = next_id++;
    s->attr = attr;
    s->count = s->initial = initial;
    s->maximum = maximum;
    s->next = registry;
    registry = s;
    *out = s->id;
    ReleaseSRWLockExclusive(&sema_lock);
    gz_log("Runtime: semaphore '%s' id=%u, initial=%d, max=%d", s->name, s->id, initial, maximum);
    return 0;
}
static int32_t wait_count(uint32_t id, int32_t need, uint32_t *timeout, int block) {
    AcquireSRWLockExclusive(&sema_lock);
    Sema *s = find(id);
    int32_t result = 0;
    if (!s)
        result = GZ_ERR(GZ_ESRCH);
    else if (need <= 0 || need > s->maximum)
        result = GZ_ERR(GZ_EINVAL);
    else if (s->count >= need && !s->first)
        s->count -= need;
    else if (!block)
        result = GZ_ERR(GZ_EBUSY);
    else if (timeout && !*timeout)
        result = GZ_ERR(GZ_ETIMEDOUT);
    else {
        Waiter w = {.need = need};
        Waiter **tail = &s->first;
        while (*tail)
            tail = &(*tail)->next;
        *tail = &w;
        ++s->active;
        ULONGLONG deadline = timeout ? GetTickCount64() + gz_usec_to_ms(*timeout) : 0;
        while (!w.done) {
            DWORD wait = INFINITE;
            if (timeout) {
                ULONGLONG now = GetTickCount64();
                if (now >= deadline) { /* timed out: leave the queue */
                    Waiter **p = &s->first;
                    while (*p != &w)
                        p = &(*p)->next;
                    *p = w.next;
                    w.result = GZ_ERR(GZ_ETIMEDOUT);
                    w.done = 1;
                    break;
                }
                wait = (DWORD)(deadline - now);
            }
            SleepConditionVariableSRW(&s->changed, &sema_lock, wait, 0);
        }
        result = w.result;
        if (timeout) {
            ULONGLONG now = GetTickCount64();
            *timeout = result || now >= deadline ? 0 : (uint32_t)((deadline - now) * 1000);
        }
        if (!--s->active && s->deleted)
            free(s);
    }
    ReleaseSRWLockExclusive(&sema_lock);
    return result;
}
static ABI int32_t sem_wait(uint32_t id, int32_t need, uint32_t *timeout) {
    return wait_count(id, need, timeout, 1);
}
static ABI int32_t sem_poll(uint32_t id, int32_t need) {
    return wait_count(id, need, NULL, 0);
}
static ABI int32_t sem_signal(uint32_t id, int32_t count) {
    AcquireSRWLockExclusive(&sema_lock);
    Sema *s = find(id);
    int32_t result = 0;
    if (!s)
        result = GZ_ERR(GZ_ESRCH);
    else if (count <= 0 || count > s->maximum - s->count)
        result = GZ_ERR(GZ_EINVAL);
    else {
        s->count += count;
        /* Tokens go to waiters in queue order, as far as they reach. */
        Waiter **p = &s->first;
        while (*p) {
            Waiter *w = *p;
            if (w->need > s->count) {
                p = &w->next;
                continue;
            }
            s->count -= w->need;
            *p = w->next;
            w->done = 1;
        }
        WakeAllConditionVariable(&s->changed);
    }
    ReleaseSRWLockExclusive(&sema_lock);
    return result;
}
static int32_t wake_all(Sema *s, int32_t result) {
    int32_t n = 0;
    while (s->first) {
        Waiter *w = s->first;
        s->first = w->next;
        w->result = result;
        w->done = 1;
        ++n;
    }
    WakeAllConditionVariable(&s->changed);
    return n;
}
static ABI int32_t sem_cancel(uint32_t id, int32_t count, int32_t *waiters) {
    AcquireSRWLockExclusive(&sema_lock);
    Sema *s = find(id);
    int32_t result = 0;
    if (!s)
        result = GZ_ERR(GZ_ESRCH);
    else if (count > s->maximum)
        result = GZ_ERR(GZ_EINVAL);
    else {
        int32_t n = wake_all(s, GZ_ERR(85)); /* ECANCELED */
        if (waiters)
            *waiters = n;
        s->count = count < 0 ? s->initial : count;
    }
    ReleaseSRWLockExclusive(&sema_lock);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    AcquireSRWLockExclusive(&sema_lock);
    Sema **p = &registry;
    while (*p && (*p)->id != id)
        p = &(*p)->next;
    if (!*p) {
        ReleaseSRWLockExclusive(&sema_lock);
        return GZ_ERR(GZ_ESRCH);
    }
    Sema *s = *p;
    *p = s->next;
    s->deleted = 1;
    wake_all(s, GZ_ERR(13)); /* EACCES */
    if (!s->active)
        free(s);
    ReleaseSRWLockExclusive(&sema_lock);
    return 0;
}

/* ---- event flags ---- */
enum {
    EVF_SINGLE = 0x10,
    EVF_MULTI = 0x20,
    WAIT_AND = 1,
    WAIT_OR = 2,
    CLEAR_ALL = 0x10,
    CLEAR_PAT = 0x20
};
enum { EVF_SET, EVF_CANCELED, EVF_DELETED };
typedef struct {
    SRWLOCK lock;
    CONDITION_VARIABLE changed;
    uint64_t bits;
    int waiting, multi, status;
    char name[32];
} EventFlag;

static ABI int32_t evf_create(EventFlag **out, const char *name, uint32_t attr, uint64_t pattern,
                              const void *options) {
    (void)options;
    if (!out || !name || (attr & ~0x33))
        return GZ_ERR(GZ_EINVAL);
    if (strlen(name) >= 32)
        return GZ_ERR(63); /* ENAMETOOLONG */
    EventFlag *e = calloc(1, sizeof(*e));
    if (!e)
        return GZ_ERR(GZ_ENOMEM);
    InitializeSRWLock(&e->lock);
    InitializeConditionVariable(&e->changed);
    e->bits = pattern;
    e->multi = (attr & EVF_MULTI) != 0;
    strcpy(e->name, name);
    *out = e;
    gz_log("Runtime: event flag '%s' (%s), initial pattern 0x%llx", e->name,
           e->multi ? "multi" : "single", (unsigned long long)pattern);
    return 0;
}
static int satisfied(const EventFlag *e, uint64_t bits, int mode) {
    return e->status != EVF_SET ||
           ((mode & WAIT_AND) ? (e->bits & bits) == bits : (e->bits & bits) != 0);
}
/* timeout NULL: wait forever; *timeout == 0 with poll set: do not wait (EBUSY). */
static int32_t evf_wait_common(EventFlag *e, uint64_t bits, uint32_t mode, uint64_t *result,
                               uint32_t *timeout, int poll) {
    if (!e)
        return GZ_ERR(GZ_ESRCH);
    int wait_mode = mode & 0xf, clear = mode & 0xf0;
    if (!bits || (wait_mode != WAIT_AND && wait_mode != WAIT_OR) ||
        (clear && clear != CLEAR_ALL && clear != CLEAR_PAT))
        return GZ_ERR(GZ_EINVAL);
    AcquireSRWLockExclusive(&e->lock);
    if (!e->multi && e->waiting) {
        ReleaseSRWLockExclusive(&e->lock);
        return GZ_ERR(GZ_EPERM);
    }
    int32_t r = 0;
    ULONGLONG start = GetTickCount64();
    if (!satisfied(e, bits, wait_mode)) {
        if (poll)
            r = GZ_ERR(GZ_EBUSY);
        else {
            ULONGLONG deadline = timeout ? start + gz_usec_to_ms(*timeout) : 0;
            ++e->waiting;
            while (!satisfied(e, bits, wait_mode)) {
                DWORD wait = INFINITE;
                if (timeout) {
                    ULONGLONG now = GetTickCount64();
                    if (now >= deadline) {
                        r = GZ_ERR(GZ_ETIMEDOUT);
                        break;
                    }
                    wait = (DWORD)(deadline - now);
                }
                SleepConditionVariableSRW(&e->changed, &e->lock, wait, 0);
            }
            --e->waiting;
        }
    }
    if (result)
        *result = e->bits;
    if (timeout && !poll) {
        ULONGLONG used = (GetTickCount64() - start) * 1000;
        *timeout = r || used >= *timeout ? 0 : *timeout - (uint32_t)used;
    }
    if (!r) {
        if (e->status == EVF_CANCELED)
            r = GZ_ERR(85);
        else if (e->status == EVF_DELETED)
            r = GZ_ERR(13);
        else if (clear == CLEAR_ALL)
            e->bits = 0;
        else if (clear == CLEAR_PAT)
            e->bits &= ~bits;
    }
    if (e->status != EVF_SET)
        WakeAllConditionVariable(&e->changed); /* lets a canceller see the count drop */
    ReleaseSRWLockExclusive(&e->lock);
    return r;
}
static ABI int32_t evf_wait(EventFlag *e, uint64_t bits, uint32_t mode, uint64_t *result,
                            uint32_t *timeout) {
    return evf_wait_common(e, bits, mode, result, timeout, 0);
}
static ABI int32_t evf_poll(EventFlag *e, uint64_t bits, uint32_t mode, uint64_t *result) {
    return evf_wait_common(e, bits, mode, result, NULL, 1);
}
static ABI int32_t evf_set(EventFlag *e, uint64_t bits) {
    if (!e)
        return GZ_ERR(GZ_ESRCH);
    AcquireSRWLockExclusive(&e->lock);
    e->bits |= bits;
    WakeAllConditionVariable(&e->changed);
    ReleaseSRWLockExclusive(&e->lock);
    return 0;
}
/* Clear keeps only the bits in the pattern (shadPS4: m_bits &= bits). */
static ABI int32_t evf_clear(EventFlag *e, uint64_t bits) {
    if (!e)
        return GZ_ERR(GZ_ESRCH);
    AcquireSRWLockExclusive(&e->lock);
    e->bits &= bits;
    ReleaseSRWLockExclusive(&e->lock);
    return 0;
}
/* Wakes every waiter with ECANCELED, waits until they have left, then sets the pattern. */
static ABI int32_t evf_cancel(EventFlag *e, uint64_t pattern, int32_t *waiters) {
    if (!e)
        return GZ_ERR(GZ_ESRCH);
    AcquireSRWLockExclusive(&e->lock);
    if (waiters)
        *waiters = e->waiting;
    e->status = EVF_CANCELED;
    e->bits = pattern;
    WakeAllConditionVariable(&e->changed);
    while (e->waiting)
        SleepConditionVariableSRW(&e->changed, &e->lock, INFINITE, 0);
    e->status = EVF_SET;
    ReleaseSRWLockExclusive(&e->lock);
    return 0;
}
static ABI int32_t evf_delete(EventFlag *e) {
    if (!e)
        return GZ_ERR(GZ_ESRCH);
    AcquireSRWLockExclusive(&e->lock);
    e->status = EVF_DELETED;
    WakeAllConditionVariable(&e->changed);
    while (e->waiting)
        SleepConditionVariableSRW(&e->changed, &e->lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&e->lock);
    free(e);
    return 0;
}

static const GzExport exports[] = {
    {"libkernel", "sceKernelCreateSema", (void *)sem_create},
    {"libkernel", "sceKernelWaitSema", (void *)sem_wait},
    {"libkernel", "sceKernelPollSema", (void *)sem_poll},
    {"libkernel", "sceKernelSignalSema", (void *)sem_signal},
    {"libkernel", "sceKernelCancelSema", (void *)sem_cancel},
    {"libkernel", "sceKernelDeleteSema", (void *)sem_delete},
    {"libkernel", "sceKernelCreateEventFlag", (void *)evf_create},
    {"libkernel", "sceKernelWaitEventFlag", (void *)evf_wait},
    {"libkernel", "sceKernelPollEventFlag", (void *)evf_poll},
    {"libkernel", "sceKernelSetEventFlag", (void *)evf_set},
    {"libkernel", "sceKernelClearEventFlag", (void *)evf_clear},
    {"libkernel", "sceKernelCancelEventFlag", (void *)evf_cancel},
    {"libkernel", "sceKernelDeleteEventFlag", (void *)evf_delete},
};
uintptr_t gz_sema_resolve(const char *library, const char *symbol) {
    return GZ_LOOKUP(exports, library, symbol);
}
