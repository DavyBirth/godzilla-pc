/* Core runtime contracts needed before and during libc start: TLS of linked modules, stack
 * protector, errno, process parameters. Everything else is a logging stub (loader.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Contracts from bbport's src/runtime.c (github.com/deadinside28/bloodborne_pc). */
#define _CRT_RAND_S
#include "gz.h"
#ifdef GZ_GPU
#include "gpu.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <math.h>
static ABI void guest_sincosf(float x, float *s, float *c) {
    *s = sinf(x);
    *c = cosf(x);
}

/* Dynamic TLS of linked modules: IDs 2..7 (1 would be the eboot's static TLS; Godzilla has none). */
#define TLS_MODULES 8
static struct {
    const void *data;
    uint64_t filesz, memsz;
} tls_modules[TLS_MODULES];
static _Thread_local unsigned char *tls_blocks[TLS_MODULES];

void gz_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz) {
    if (module < 2 || module >= TLS_MODULES)
        gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: unsupported TLS module id %llu",
                (unsigned long long)module);
    tls_modules[module].data = data;
    tls_modules[module].filesz = filesz;
    tls_modules[module].memsz = memsz;
}

/* __tls_get_addr(&{module, offset}): each thread gets its own copy of the module's TLS template. */
static ABI void *guest_tls_get_addr(const uint64_t *index) {
    if (!index || index[0] < 2 || index[0] >= TLS_MODULES || !tls_modules[index[0]].data ||
        index[1] >= tls_modules[index[0]].memsz)
        gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: unsupported TLS module/offset");
    unsigned char **block = &tls_blocks[index[0]];
    if (!*block) {
        *block = calloc(1, tls_modules[index[0]].memsz);
        if (!*block)
            gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: cannot allocate module TLS");
        memcpy(*block, tls_modules[index[0]].data, tls_modules[index[0]].filesz);
    }
    return *block + index[1];
}

static uint64_t stack_canary;
static ABI __attribute__((noreturn)) void guest_stack_chk_fail(void) {
    gz_stop(GZ_EXIT_STACK_CHECK, "STOP: guest stack protector detected corruption");
}

static _Thread_local int32_t guest_errno;
int32_t *gz_errno(void) {
    return &guest_errno;
}
static ABI int32_t *guest_error(void) {
    return &guest_errno;
}

static void *process_param;
void gz_set_procparam(void *param) {
    process_param = param;
}
static ABI int32_t guest_compiled_sdk(int32_t *out) {
    if (!out || !process_param)
        return (int32_t)0x80020016;
    memcpy(out, (unsigned char *)process_param + 16, sizeof(*out));
    return 0;
}
static ABI void *guest_get_procparam(void) {
    return process_param;
}

static void **application_heap_api;
void **gz_application_heap_api(void) {
    return application_heap_api;
}
static ABI void guest_set_heap_api(void **api) {
    application_heap_api = api;
    gz_log("Runtime: application heap API registered at %p", (void *)api);
}

static const char *progname = "eboot.bin";
static int32_t need_libc_internal = 1; /* SDK marker variable referenced by Fios2 */

void gz_runtime_start(void) {
    unsigned halves[2];
    if (rand_s(&halves[0]) || rand_s(&halves[1]))
        gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: cannot initialize stack canary");
    memcpy(&stack_canary, halves, sizeof(stack_canary));
    stack_canary &= ~UINT64_C(255);
}

typedef struct {
    const char *library, *symbol;
    void *address;
    int is_data;
} Export;
static const Export exports[] = {
    {"libkernel", "__tls_get_addr", (void *)guest_tls_get_addr, 0},
    {"libkernel", "sceKernelGetCompiledSdkVersion", (void *)guest_compiled_sdk, 0},
    {"libkernel", "__stack_chk_fail", (void *)guest_stack_chk_fail, 0},
    {"libkernel", "__stack_chk_guard", &stack_canary, 1},
    {"libkernel", "__error", (void *)guest_error, 0},
    {"libkernel", "__progname", &progname, 1},
    {"libkernel", "sceKernelGetProcParam", (void *)guest_get_procparam, 0},
    {"libkernel", "_sceKernelRtldSetApplicationHeapAPI", (void *)guest_set_heap_api, 0},
    {"libSceLibcInternal", "Need_sceLibcInternal", &need_libc_internal, 1},
};

uintptr_t gz_lookup(const GzExport *table, size_t count, const char *library, const char *symbol) {
    for (size_t i = 0; i < count; ++i)
        if ((!table[i].library || !strcmp(table[i].library, library)) &&
            !strcmp(table[i].symbol, symbol))
            return (uintptr_t)table[i].function;
    return 0;
}

uintptr_t gz_resolve(const char *library, const char *symbol, int is_data) {
    if (!is_data && !strcmp(library, "libSceLibcInternal") && !strcmp(symbol, "sincosf"))
        return (uintptr_t)guest_sincosf;
    for (size_t i = 0; i < sizeof(exports) / sizeof(*exports); ++i)
        if (exports[i].is_data == is_data && !strcmp(exports[i].library, library) &&
            !strcmp(exports[i].symbol, symbol))
            return (uintptr_t)exports[i].address;
    if (is_data)
        return 0;
    uintptr_t found;
    if ((found = gz_sync_resolve(library, symbol)))
        return found;
    if ((found = gz_thread_resolve(library, symbol)))
        return found;
    if ((found = gz_memory_resolve(library, symbol)))
        return found;
    if ((found = gz_sema_resolve(library, symbol)))
        return found;
    if ((found = gz_services_resolve(library, symbol)))
        return found;
    if ((found = gz_file_resolve(library, symbol)))
        return found;
    if ((found = gz_savedata_resolve(library, symbol)))
        return found;
    if ((found = gz_kernel_resolve(library, symbol)))
        return found;
    if ((found = gz_rtc_resolve(library, symbol)))
        return found;
#ifdef GZ_GPU
    if ((found = gz_gpu_resolve(library, symbol)))
        return found;
#endif
    return 0;
}
