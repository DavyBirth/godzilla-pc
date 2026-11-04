/* Godzilla PS4 native port: shared declarations of the Windows loader and runtime.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Structure follows bbport (github.com/deadinside28/bloodborne_pc, src/runtime.h). */
#ifndef GZ_H
#define GZ_H
#include <stdint.h>
#include <stddef.h>

/* Every function the guest calls, or that calls guest code, uses the PS4's System V ABI. */
#define ABI __attribute__((sysv_abi))

/* Where the PS4 loads the eboot; the image is mapped at exactly this address. */
#define GZ_IMAGE_BASE UINT64_C(0x800000000)

/* Exit codes, for scripts and the diary. */
enum {
    GZ_EXIT_STUB_STOP = 20,
    GZ_EXIT_RUNTIME_STOP = 21,
    GZ_EXIT_STACK_CHECK = 22,
    GZ_EXIT_FAULT = 30
};

/* Log to the console and the log file (thread-safe). */
void gz_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* Logs, flushes and ends the process at once (guest threads may still run). */
__attribute__((noreturn)) void gz_stop(int code, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Host implementation of an import, by library and symbol name (imports.tsv); 0 if none. */
uintptr_t gz_resolve(const char *library, const char *symbol, int is_data);

/* Each runtime file exports a table of host functions; library NULL matches any library. */
typedef struct {
    const char *library, *symbol;
    void *function;
} GzExport;
uintptr_t gz_lookup(const GzExport *table, size_t count, const char *library, const char *symbol);
#define GZ_LOOKUP(table, library, symbol)                                                          \
    gz_lookup(table, sizeof(table) / sizeof(*(table)), library, symbol)
uintptr_t gz_sync_resolve(const char *library, const char *symbol);
uintptr_t gz_thread_resolve(const char *library, const char *symbol);
uintptr_t gz_memory_resolve(const char *library, const char *symbol);
uintptr_t gz_sema_resolve(const char *library, const char *symbol);
uintptr_t gz_services_resolve(const char *library, const char *symbol);

uintptr_t gz_file_resolve(const char *library, const char *symbol);
uintptr_t gz_savedata_resolve(const char *library, const char *symbol);
uintptr_t gz_kernel_resolve(const char *library, const char *symbol);
uintptr_t gz_rtc_resolve(const char *library, const char *symbol);
void gz_kernel_start(void);

/* Host folder of the merged game (the PS4's /app0). */
const char *gz_app0(void);
/* The guest's errno (thread-local; __error). */
int32_t *gz_errno(void);

/* Guest file system (rt_file.c): PS4 mount points on host folders; paths are UTF-8. */
void gz_file_configure(const char *app0, const char *user);
const char *gz_user_dir(void);
int gz_file_mount(const char *guest, const char *host);
void gz_file_unmount(const char *guest);
int gz_file_translate(const char *guest, char *out, size_t size);
void gz_file_report(void);
int gz_widen(const char *utf8, wchar_t *out, int count);
int gz_make_dirs(const char *path);
void gz_savedata_configure(const char *app0);

/* Guest memory (rt_memory.c), on the host address space of rt_vmem.c. Started before any
 * guest thread so the PS4 user range is ours before Windows places anything there. */
void gz_memory_start(void);
void gz_memory_report(void);
/* GPU reads/writes use the backing view so protected guest pages cannot recurse into VEH. */
uint64_t gz_memory_clamp(uintptr_t address, uint64_t size);
int gz_memory_copy(uintptr_t address, void *buffer, uint64_t size, int write);
void gz_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
void gz_memory_replay_gpu(void);
int gz_vmem_init(uintptr_t min, uintptr_t max, uint64_t backing_size);
unsigned char *gz_vmem_backing(void);
void gz_vmem_map(uintptr_t address, uint64_t size, uint64_t phys, unsigned long windows_prot);
void gz_vmem_unmap(uintptr_t address, uint64_t size);
void gz_vmem_protect(uintptr_t address, uint64_t size, unsigned long windows_prot);
uintptr_t gz_vmem_blocked(uintptr_t address, uint64_t size);

/* PS4 kernel errors: 0x8002xxxx with a FreeBSD errno. POSIX-style APIs return the bare errno. */
#define GZ_ERR(n) ((int32_t)(UINT32_C(0x80020000) | (uint32_t)(n)))
enum {
    GZ_EPERM = 1,
    GZ_ESRCH = 3,
    GZ_EDEADLK = 11,
    GZ_ENOMEM = 12,
    GZ_EBUSY = 16,
    GZ_EINVAL = 22,
    GZ_EAGAIN = 35,
    GZ_ETIMEDOUT = 60
};

/* Relative timeout in microseconds -> Windows milliseconds (rounded up; never 0 for a wait). */
static inline unsigned long gz_usec_to_ms(uint64_t usec) {
    uint64_t ms = (usec + 999) / 1000;
    return ms >= 0xfffffffe ? 0xfffffffe : (unsigned long)ms;
}

/* Linked-module TLS (module IDs 2..7, __tls_get_addr) and the eboot's process parameters. */
void gz_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void gz_set_procparam(void *param);
/* The game's malloc/free table (_sceKernelRtldSetApplicationHeapAPI), NULL until registered. */
void **gz_application_heap_api(void);
void gz_runtime_start(void);
/* Gives the calling host thread (the guest main thread) a guest thread record. */
void gz_thread_attach_main(void);
void gz_thread_attach_host(const char *name);
void gz_thread_detach_host(void);
#endif
