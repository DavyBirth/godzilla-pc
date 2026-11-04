/* libkernel process services: clocks, sleeping, time zone, signal bookkeeping, a narrow sysctl
 * and process queries. Guest values use FreeBSD numbering.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * From bbport's src/runtime_kernel.c (github.com/deadinside28/bloodborne_pc), on Windows clocks:
 * QueryPerformanceCounter for monotonic time, GetSystemTimePreciseAsFileTime for wall time. */
#define _CRT_RAND_S
#include "gz.h"
#include <windows.h>
#include <intrin.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 16384
typedef struct {
    int64_t sec, nsec;
} GuestTimespec;
typedef struct {
    int64_t sec, usec;
} GuestTimeval;
typedef struct {
    int32_t minuteswest, dsttime;
} GuestTimezone;

static int32_t fail_posix(int32_t e) {
    *gz_errno() = e;
    return -1;
}

/* ---- clocks ---- */
static LARGE_INTEGER qpc_frequency, qpc_start;
static uint64_t qpc_ns(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t ticks = (uint64_t)now.QuadPart, f = (uint64_t)qpc_frequency.QuadPart;
    return ticks / f * 1000000000 + ticks % f * 1000000000 / f;
}
static uint64_t wall_100ns(void) { /* since 1970 */
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    return ((uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime) - UINT64_C(116444736000000000);
}
static uint64_t filetime_100ns(FILETIME a, FILETIME b) {
    return ((uint64_t)a.dwHighDateTime << 32 | a.dwLowDateTime) +
           ((uint64_t)b.dwHighDateTime << 32 | b.dwLowDateTime);
}
/* PS4 clock ids: 0/9/10 realtime, 4/5/7/8/11/12 monotonic and uptime, 13 seconds, 14 thread CPU,
 * 2/15 process CPU, 16-19 network clocks (monotonic). */
static int clock_read(uint32_t id, GuestTimespec *ts) {
    if (!ts)
        return GZ_EINVAL;
    uint64_t ns;
    FILETIME c, e, k, u;
    switch (id) {
    case 0:
    case 9:
    case 10:
    case 13:
        ns = wall_100ns() * 100;
        break;
    case 4:
    case 5:
    case 7:
    case 8:
    case 11:
    case 12:
    case 16:
    case 17:
    case 18:
    case 19:
        ns = qpc_ns();
        break;
    case 14:
        GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
        ns = filetime_100ns(k, u) * 100;
        break;
    case 2:
    case 15:
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        ns = filetime_100ns(k, u) * 100;
        break;
    default:
        return GZ_EINVAL;
    }
    ts->sec = (int64_t)(ns / 1000000000);
    ts->nsec = id == 13 ? 0 : (int64_t)(ns % 1000000000);
    return 0;
}
static ABI int32_t kernel_clock_gettime(uint32_t id, GuestTimespec *ts) {
    int e = clock_read(id, ts);
    return e ? GZ_ERR(e) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id, GuestTimespec *ts) {
    int e = clock_read(id, ts);
    return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id, GuestTimespec *ts) {
    GuestTimespec probe;
    if (clock_read(id, &probe))
        return fail_posix(GZ_EINVAL);
    if (ts) {
        ts->sec = 0;
        ts->nsec = 100;
    }
    return 0;
}
static uint64_t start_ns;
static ABI uint64_t process_time(void) {
    return (qpc_ns() - start_ns) / 1000;
}
static ABI uint64_t process_time_counter(void) {
    return qpc_ns() - start_ns;
}
static ABI uint64_t process_time_frequency(void) {
    return 1000000000;
}
static ABI uint64_t read_tsc(void) {
    return __rdtsc();
}
static uint64_t tsc_hz;
static ABI uint64_t tsc_frequency(void) {
    if (!tsc_hz) { /* measured once against the performance counter */
        uint64_t a = qpc_ns(), t0 = __rdtsc();
        Sleep(20);
        uint64_t b = qpc_ns(), t1 = __rdtsc();
        tsc_hz = (t1 - t0) * 1000000000 / (b - a ? b - a : 1);
    }
    return tsc_hz;
}

/* ---- sleeping (high-resolution waitable timer, as rt_thread.c) ---- */
static _Thread_local HANDLE timer;
static void sleep_ns(uint64_t ns) {
    if (!ns) {
        SwitchToThread();
        return;
    }
    if (!timer)
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100);
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep(gz_usec_to_ms((ns + 999) / 1000));
}
static ABI int32_t posix_nanosleep(const GuestTimespec *rq, GuestTimespec *rem) {
    if (!rq || rq->nsec < 0 || rq->nsec >= 1000000000 || rq->sec < 0)
        return fail_posix(GZ_EINVAL);
    sleep_ns((uint64_t)rq->sec * 1000000000 + (uint64_t)rq->nsec);
    if (rem) {
        rem->sec = 0;
        rem->nsec = 0;
    }
    return 0;
}
static ABI int32_t kernel_nanosleep(const GuestTimespec *rq, GuestTimespec *rem) {
    return posix_nanosleep(rq, rem) ? GZ_ERR(*gz_errno()) : 0;
}
static ABI int32_t posix_usleep(uint32_t usec) {
    sleep_ns((uint64_t)usec * 1000);
    return 0;
}
static ABI uint32_t posix_sleep(uint32_t seconds) {
    sleep_ns((uint64_t)seconds * 1000000000);
    return 0;
}

/* ---- wall time and time zone ---- */
static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz)
        return GZ_ERR(GZ_EINVAL);
    TIME_ZONE_INFORMATION info;
    DWORD mode = GetTimeZoneInformation(&info);
    tz->minuteswest = (int32_t)info.Bias;
    tz->dsttime = mode == TIME_ZONE_ID_DAYLIGHT;
    return 0;
}
static ABI int32_t posix_gettimeofday(GuestTimeval *tv, GuestTimezone *tz) {
    if (tv) {
        uint64_t t = wall_100ns();
        tv->sec = (int64_t)(t / 10000000);
        tv->usec = (int64_t)(t % 10000000 / 10);
    }
    if (tz)
        kernel_gettimezone(tz);
    return 0;
}
static ABI int32_t kernel_gettimeofday(GuestTimeval *tv) {
    if (!tv)
        return GZ_ERR(GZ_EINVAL);
    return posix_gettimeofday(tv, NULL);
}

/* ---- process ---- */
static ABI int32_t get_pagesize(void) {
    return PAGE;
}
static ABI int32_t get_pid(void) {
    return 1000;
}
static ABI int32_t yield(void) {
    SwitchToThread();
    return 0;
}
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    gz_stop(status, "Runtime: the game called _exit(%d)", status);
}
static ABI __attribute__((noreturn)) void raise_exception(uint32_t code, uint64_t argument) {
    gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: the game raised debug exception code=0x%x argument=0x%llx",
            code, (unsigned long long)argument);
}
static ABI int32_t print_backtrace(void) {
    gz_log("Runtime: the game asked for a backtrace (not available)");
    return 0;
}
typedef struct {
    uint32_t bits[4];
} GuestSigset;
static _Thread_local GuestSigset signal_mask;
static uintptr_t handlers[128];
/* Handlers are recorded: no host signal reaches the guest. */
static ABI uintptr_t guest_signal(int sig, uintptr_t handler) {
    if (sig <= 0 || sig >= 128) {
        *gz_errno() = GZ_EINVAL;
        return (uintptr_t)-1;
    } /* SIG_ERR */
    uintptr_t old = handlers[sig];
    handlers[sig] = handler;
    gz_log("Runtime: signal(%d) handler recorded (host signals are not forwarded)", sig);
    return old;
}
static ABI int32_t guest_sigprocmask(int how, const GuestSigset *set, GuestSigset *old) {
    if (old)
        *old = signal_mask;
    if (!set)
        return 0;
    for (int i = 0; i < 4; ++i) {
        if (how == 1)
            signal_mask.bits[i] |= set->bits[i]; /* SIG_BLOCK */
        else if (how == 2)
            signal_mask.bits[i] &= ~set->bits[i]; /* SIG_UNBLOCK */
        else if (how == 3)
            signal_mask.bits[i] = set->bits[i]; /* SIG_SETMASK */
        else
            return fail_posix(GZ_EINVAL);
    }
    return 0;
}
static ABI int32_t guest_sigfillset(GuestSigset *set) {
    if (!set)
        return fail_posix(GZ_EINVAL);
    memset(set, 0xff, sizeof(*set));
    return 0;
}
static ABI int32_t guest_sigemptyset(GuestSigset *set) {
    if (!set)
        return fail_posix(GZ_EINVAL);
    memset(set, 0, sizeof(*set));
    return 0;
}
typedef struct {
    GuestTimeval utime, stime;
    int64_t rest[14];
} GuestRusage;
static ABI int32_t guest_getrusage(int who, GuestRusage *out) {
    if (!out || (who != 0 && who != 1))
        return fail_posix(GZ_EINVAL);
    FILETIME c, e, k, u;
    if (who == 0)
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    else
        GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    memset(out, 0, sizeof(*out));
    uint64_t user = ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10,
             kernel = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;
    out->utime = (GuestTimeval){(int64_t)(user / 1000000), (int64_t)(user % 1000000)};
    out->stime = (GuestTimeval){(int64_t)(kernel / 1000000), (int64_t)(kernel % 1000000)};
    return 0;
}
static ABI int32_t guest_sysctl(const int32_t *name, uint32_t namelen, void *old, uint64_t *oldlen,
                                const void *new_value, uint64_t newlen) {
    (void)newlen;
    if (!name || namelen < 2 || new_value)
        return fail_posix(GZ_EINVAL);
    if (name[0] == 1 && name[1] == 37) { /* kern.arandom */
        if (!old || !oldlen)
            return fail_posix(GZ_EINVAL);
        for (uint64_t i = 0; i < *oldlen; i += 4) {
            unsigned r;
            rand_s(&r);
            memcpy((char *)old + i, &r, *oldlen - i < 4 ? *oldlen - i : 4);
        }
        return 0;
    }
    if (name[0] == 6 && (name[1] == 7 || name[1] == 3)) { /* hw.pagesize / hw.ncpu */
        if (!oldlen)
            return fail_posix(GZ_EINVAL);
        int32_t value = name[1] == 7 ? PAGE : 7;
        if (old) {
            if (*oldlen < 4)
                return fail_posix(GZ_ENOMEM);
            memcpy(old, &value, 4);
        }
        *oldlen = 4;
        return 0;
    }
    gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: unsupported sysctl %d.%d (%u levels)", name[0], name[1],
            namelen);
}
static ABI int32_t ok_void(void) {
    return 0;
}

void gz_kernel_start(void) {
    QueryPerformanceFrequency(&qpc_frequency);
    QueryPerformanceCounter(&qpc_start);
    start_ns = qpc_ns();
}

typedef struct {
    int32_t version, align;
    uint64_t ids[2];
} TitleWorkaround;
typedef struct {
    int32_t app_id, mmap_flags, attribute_exe, attribute2;
    char cusa_name[10];
    unsigned char bytes[6];
    uint64_t preload;
    int32_t attribute1, has_sfo;
    TitleWorkaround workaround;
} AppInfo;
_Static_assert(sizeof(AppInfo) == 72, "PS4 application info layout");
static ABI int32_t app_info(int32_t pid, AppInfo *info) {
    if (pid != 1000)
        return (int32_t)0x80020001;
    if (info) {
        memset(info, 0, sizeof(*info));
        memcpy(info->cusa_name, "CUSA02046", 9);
        info->has_sfo = 1;
    }
    return 0;
}
static ABI int32_t workaround(TitleWorkaround *tw, int32_t bit, int32_t *result) {
    if (!tw || !result)
        return (int32_t)0x8002000e;
    if (bit < 0 || bit >= 0x3a)
        return (int32_t)0x80020016;
    *result = (tw->ids[bit >> 6] >> (bit & 63)) & 1;
    return 0;
}
static ABI const char *sandbox_word(void) {
    return "sys";
}
static const GzExport exports[] = {
    {NULL, "sceKernelGetAppInfo", (void *)app_info},
    {NULL, "sceKernelTitleWorkaroundIsEnabled", (void *)workaround},
    {NULL, "sceKernelGetFsSandboxRandomWord", (void *)sandbox_word},
    {NULL, "sceKernelClockGettime", (void *)kernel_clock_gettime},
    {NULL, "clock_gettime", (void *)posix_clock_gettime},
    {NULL, "clock_getres", (void *)posix_clock_getres},
    {NULL, "sceKernelGetProcessTime", (void *)process_time},
    {NULL, "sceKernelGetProcessTimeCounter", (void *)process_time_counter},
    {NULL, "sceKernelGetProcessTimeCounterFrequency", (void *)process_time_frequency},
    {NULL, "sceKernelReadTsc", (void *)read_tsc},
    {NULL, "sceKernelGetTscFrequency", (void *)tsc_frequency},
    {NULL, "usleep", (void *)posix_usleep},
    {NULL, "sleep", (void *)posix_sleep},
    {NULL, "nanosleep", (void *)posix_nanosleep},
    {NULL, "sceKernelNanosleep", (void *)kernel_nanosleep},
    {NULL, "sceKernelGettimezone", (void *)kernel_gettimezone},
    {NULL, "gettimeofday", (void *)posix_gettimeofday},
    {NULL, "sceKernelGettimeofday", (void *)kernel_gettimeofday},
    {NULL, "getpagesize", (void *)get_pagesize},
    {NULL, "getpid", (void *)get_pid},
    {NULL, "sched_yield", (void *)yield},
    {NULL, "_exit", (void *)hard_exit},
    {NULL, "sceKernelDebugRaiseException", (void *)raise_exception},
    {NULL, "sceKernelDebugRaiseExceptionOnReleaseMode", (void *)raise_exception},
    {NULL, "sceKernelPrintBacktraceWithModuleInfo", (void *)print_backtrace},
    {NULL, "signal", (void *)guest_signal},
    {NULL, "sigprocmask", (void *)guest_sigprocmask},
    {NULL, "_sigprocmask", (void *)guest_sigprocmask},
    {NULL, "sigfillset", (void *)guest_sigfillset},
    {NULL, "sigemptyset", (void *)guest_sigemptyset},
    {NULL, "getrusage", (void *)guest_getrusage},
    {NULL, "sysctl", (void *)guest_sysctl},
    {NULL, "sceKernelMlock", (void *)ok_void},
    {NULL, "msync", (void *)ok_void}, /* as shadPS4 */
    {NULL, "sceKernelMunlock", (void *)ok_void},
};
uintptr_t gz_kernel_resolve(const char *library, const char *symbol) {
    return GZ_LOOKUP(exports, library, symbol);
}
