/* Godzilla PS4 native port: Windows loader. Maps out/boot-linked.bin (scripts/link_modules.py)
 * at the PS4 address, binds imports and runs the original x86-64 code natively.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Based on bbport's src/probe.c (github.com/deadinside28/bloodborne_pc): same BBPROBE5 format
 * and checks; Windows memory, Vectored Exception Handler and name-based import binding. */
#include "gz.h"
#ifdef GZ_GPU
#include "gpu.h"
#endif
#include <windows.h>
#include <cpuid.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t address, size, flags;
} Segment;
typedef struct {
    uint64_t target, kind, value, addend;
} Reloc;
typedef struct {
    uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module;
} LinkedModule;
typedef struct {
    char scoped[128], library[64], symbol[128];
} Import;

static const char *module_names[] = {"libc.prx", "libSceFios2.prx"};
static unsigned char *image;
static uint64_t image_size, entry, procparam, module_count, import_count, segment_count;
static LinkedModule modules[16];
static Segment *segments;
static Import *imports;
static unsigned char *traps, *data_traps;
static size_t page_size;
static int stubs_return_zero, list_stubs;
static int gpu_test, no_gpu;
static char app0[MAX_PATH], user_dir[MAX_PATH];
const char *gz_app0(void) {
    return app0;
}
static volatile LONG *stub_calls;

/* ---- log ---- */
static FILE *log_file;
static CRITICAL_SECTION log_lock;
static void vlog(const char *format, va_list args) {
    char line[2048];
    vsnprintf(line, sizeof(line), format, args);
    EnterCriticalSection(&log_lock);
    fputs(line, stdout);
    fputc('\n', stdout);
    if (log_file) {
        fputs(line, log_file);
        fputc('\n', log_file);
        fflush(log_file);
    }
    LeaveCriticalSection(&log_lock);
}
void gz_log(const char *format, ...) {
    va_list a;
    va_start(a, format);
    vlog(format, a);
    va_end(a);
}
void gz_stop(int code, const char *format, ...) {
    va_list a;
    va_start(a, format);
    vlog(format, a);
    va_end(a);
    if (code != 1) {
        gz_memory_report();
        gz_file_report();
    }
    gz_log("Exit code %d", code);
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), (UINT)code); /* guest threads may still be running */
    for (;;) {
    }
}
static __attribute__((noreturn)) void fail(const char *message) {
    gz_stop(1, "ERROR: %s", message);
}

/* ---- where an address is, for messages ---- */
static void describe(uintptr_t address, char *out, size_t size) {
    uintptr_t base = (uintptr_t)image;
    if (address >= base && address - base < image_size) {
        uint64_t offset = address - base;
        for (uint64_t m = 0; m < module_count; ++m)
            if (offset >= modules[m].base && offset - modules[m].base < modules[m].size) {
                snprintf(out, size, "%s+0x%" PRIx64, m < 2 ? module_names[m] : "module",
                         offset - modules[m].base);
                return;
            }
        snprintf(out, size, "eboot+0x%" PRIx64, offset);
    } else if (traps && address >= (uintptr_t)traps &&
               address < (uintptr_t)traps + import_count * 32) {
        uint64_t i = (address - (uintptr_t)traps) / 32;
        snprintf(out, size, "stub of %s", imports[i].symbol);
    } else {
        HMODULE module = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)address, &module) &&
            module) {
            char path[MAX_PATH] = "?";
            GetModuleFileNameA(module, path, sizeof(path));
            const char *name = strrchr(path, '\\');
            snprintf(out, size, "host %s+0x%" PRIxPTR, name ? name + 1 : path,
                     address - (uintptr_t)module);
        } else
            snprintf(out, size, "0x%" PRIxPTR, address);
    }
}

/* ---- import stubs: log the call; stop (default) or return 0 ---- */
ABI uint64_t gz_stub_called(uint64_t index, const uint64_t *args, uint64_t return_address);
/* Each stub: mov r11d, index; movabs r10, gz_stub_thunk; jmp r10. The thunk keeps the six
 * SysV argument registers and passes them, with the index and return address, to C. */
__asm__(".text\n.globl gz_stub_thunk\ngz_stub_thunk:\n"
        " push %rbp\n mov %rsp,%rbp\n sub $64,%rsp\n"
        " mov %rdi,0(%rsp)\n mov %rsi,8(%rsp)\n mov %rdx,16(%rsp)\n"
        " mov %rcx,24(%rsp)\n mov %r8,32(%rsp)\n mov %r9,40(%rsp)\n"
        " mov %r11,%rdi\n mov %rsp,%rsi\n mov 8(%rbp),%rdx\n"
        " call gz_stub_called\n leave\n ret\n");
void gz_stub_thunk(void);

ABI uint64_t gz_stub_called(uint64_t index, const uint64_t *args, uint64_t return_address) {
    if (index >= import_count)
        fail("bad stub index");
    LONG n = InterlockedIncrement(&stub_calls[index]);
    char caller[192];
    describe((uintptr_t)return_address, caller, sizeof(caller));
    const Import *im = &imports[index];
    if (!stubs_return_zero)
        gz_stop(GZ_EXIT_STUB_STOP,
                "STOP: first unimplemented PS4 function: %s::%s (%s)\n"
                "  args: rdi=0x%" PRIx64 " rsi=0x%" PRIx64 " rdx=0x%" PRIx64 " rcx=0x%" PRIx64
                " r8=0x%" PRIx64 " r9=0x%" PRIx64 "\n"
                "  called from %s (thread %lu)",
                im->library, im->symbol, im->scoped, args[0], args[1], args[2], args[3], args[4],
                args[5], caller, GetCurrentThreadId());
    if (n <= 3)
        gz_log("STUB %s::%s(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64
               ", 0x%" PRIx64 ") from %s -> 0%s",
               im->library, im->symbol, args[0], args[1], args[2], args[3], args[4], args[5],
               caller, n == 3 ? " (further calls not logged)" : "");
    return 0;
}

/* ---- faults ---- */
static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return 0;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return 0;
    return (uintptr_t)p + n <= (uintptr_t)info.BaseAddress + info.RegionSize;
}
static const char *exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "integer division by zero";
    case EXCEPTION_INT_OVERFLOW:
        return "integer overflow";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "misaligned data";
    default:
        return NULL;
    }
}
static _Thread_local int final_exception;
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
#ifdef GZ_GPU
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2 &&
        e->ExceptionRecord->ExceptionInformation[0] != 8 &&
        gz_gpu_handle_fault(e, (void *)e->ExceptionRecord->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    /* Windows drivers may handle their own access faults later in the VEH/SEH chain. */
    if (!final_exception && code == EXCEPTION_ACCESS_VIOLATION && image) {
        uintptr_t rip = e->ContextRecord->Rip;
        HMODULE owner = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rip, &owner);
        if (!(rip >= (uintptr_t)image && rip - (uintptr_t)image < image_size) && owner &&
            owner != GetModuleHandleA(NULL))
            return EXCEPTION_CONTINUE_SEARCH;
    }
#endif
    const char *name = exception_name(code);
    if (!name)
        return EXCEPTION_CONTINUE_SEARCH; /* C++ exceptions, debugger events, ... */
    CONTEXT *c = e->ContextRecord;
    char where[192], target[192] = "";
    describe((uintptr_t)c->Rip, where, sizeof(where));
    gz_log("FAULT: %s at %s (thread %lu)", name, where, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR kind = e->ExceptionRecord->ExceptionInformation[0],
                  address = e->ExceptionRecord->ExceptionInformation[1];
        if (data_traps && address >= (uintptr_t)data_traps &&
            address < (uintptr_t)data_traps + import_count * page_size) {
            const Import *im = &imports[(address - (uintptr_t)data_traps) / page_size];
            snprintf(target, sizeof(target), " = unimplemented PS4 variable %s::%s", im->library,
                     im->symbol);
        }
        gz_log("  %s address 0x%" PRIxPTR "%s",
               kind == 0   ? "read of"
               : kind == 1 ? "write to"
                           : "execute at",
               (uintptr_t)address, target);
    }
    if (readable((void *)c->Rip, 16)) {
        const unsigned char *b = (const unsigned char *)c->Rip;
        char hex[64] = "";
        for (int i = 0; i < 16; ++i)
            snprintf(hex + i * 3, 4, "%02x ", b[i]);
        gz_log("  code: %s", hex);
        /* Legacy prefixes before the opcode: FS/GS mean a TLS access the loader does not handle yet. */
        for (int i = 0; i < 4 && (b[i] == 0x66 || b[i] == 0x67 || b[i] == 0xf2 || b[i] == 0xf3 ||
                                  b[i] == 0x64 || b[i] == 0x65);
             ++i)
            if (b[i] == 0x64 || b[i] == 0x65)
                gz_log("  NOTE: %s-segment instruction: guest TLS access through %s (TLS is not "
                       "emulated)",
                       b[i] == 0x64 ? "FS" : "GS", b[i] == 0x64 ? "fs:" : "gs:");
    }
    gz_log("  rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64, c->Rax,
           c->Rbx, c->Rcx, c->Rdx);
    gz_log("  rsi=%016" PRIx64 " rdi=%016" PRIx64 " rbp=%016" PRIx64 " rsp=%016" PRIx64, c->Rsi,
           c->Rdi, c->Rbp, c->Rsp);
    gz_log("  r8 =%016" PRIx64 " r9 =%016" PRIx64 " r10=%016" PRIx64 " r11=%016" PRIx64, c->R8,
           c->R9, c->R10, c->R11);
    gz_log("  r12=%016" PRIx64 " r13=%016" PRIx64 " r14=%016" PRIx64 " r15=%016" PRIx64, c->R12,
           c->R13, c->R14, c->R15);
    /* Frame chain through rbp (guest code keeps frame pointers in most functions). */
    uint64_t rbp = c->Rbp;
    for (int depth = 0; depth < 16 && rbp && readable((void *)rbp, 16); ++depth) {
        const uint64_t *frame = (const uint64_t *)rbp;
        describe((uintptr_t)frame[1], where, sizeof(where));
        gz_log("  #%d %s", depth, where);
        if (frame[0] <= rbp)
            break;
        rbp = frame[0];
    }
    gz_stop(GZ_EXIT_FAULT, "STOP: fault");
}
static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *e) {
    final_exception = 1;
    return on_exception(e);
}

/* ---- CPU: the guest uses the Jaguar's instructions directly (bbport check_cpu) ---- */
static void check_cpu(void) {
    unsigned a, b, c, d, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d))
        leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d))
        leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d))
        ext1_c = c;
    int avx = (leaf1_c & bit_AVX) && (leaf1_c & bit_OSXSAVE);
    if (avx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6; /* the OS saves SSE and AVX state */
    }
    if (avx && (leaf7_b & bit_BMI) && (leaf1_c & bit_MOVBE) && (ext1_c & bit_LZCNT) &&
        (leaf1_c & bit_POPCNT) && (leaf1_c & bit_SSE42))
        return;
    fail("this CPU lacks AVX, BMI1, MOVBE, LZCNT, POPCNT or SSE4.2, which the PS4 code uses "
         "directly");
}

/* ---- boot file ---- */
static uint64_t read64(FILE *f) {
    uint64_t v;
    if (fread(&v, 8, 1, f) != 1)
        fail("truncated boot file");
    return v;
}
static size_t round_page(size_t n) {
    return (n + page_size - 1) & ~(page_size - 1);
}
static int mapped(uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < segment_count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes)
            return 1;
    return 0;
}
static int executable(uint64_t address) {
    for (uint64_t i = 0; i < segment_count; ++i)
        if ((segments[i].flags & 1) && address >= segments[i].address &&
            address - segments[i].address < segments[i].size)
            return 1;
    return 0;
}
static void protect(void *p, size_t size, unsigned flags) {
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1)
        mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2)
        mode = PAGE_READWRITE;
    else if (flags & 4)
        mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old))
        fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
}
static void load_imports(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        fail("cannot open imports.tsv; run scripts\\prepare_all.ps1");
    char line[512];
    uint64_t count = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long index;
        Import im;
        if (sscanf(line, "%llu\t%127[^\t]\t%63[^\t]\t%127[^\t\r\n]", &index, im.scoped, im.library,
                   im.symbol) != 4 ||
            index != count || index >= import_count)
            fail("invalid imports.tsv");
        if (strcmp(im.scoped, imports[index].scoped))
            fail("imports.tsv does not match boot-linked.bin; run scripts\\prepare_all.ps1");
        imports[index] = im;
        ++count;
    }
    fclose(f);
    if (count != import_count)
        fail("imports.tsv is incomplete");
}

/* ---- guest main thread ---- */
static void ABI guest_exit_handler(void) {
    gz_log("Runtime: guest process finalizer callback reached");
}
static DWORD WINAPI guest_main(void *unused) {
    (void)unused;
    gz_thread_attach_main();
    /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
    for (uint64_t m = 0; m < module_count; ++m) {
        gz_log("Starting %s (init at image+0x%" PRIx64 ")", m < 2 ? module_names[m] : "module",
               modules[m].init);
        typedef int(ABI * ModuleInit)(uint64_t, void *, void *);
        int result = ((ModuleInit)(image + modules[m].init))(0, NULL, NULL);
        gz_log("%s initializer returned %d", m < 2 ? module_names[m] : "module", result);
        if (result)
            fail("module initializer failed");
    }
    static struct {
        uint64_t argc;
        const char *argv[2];
    } params = {1, {"/app0/eboot.bin", NULL}};
    gz_log("Entering the game's code at eboot+0x%" PRIx64 " (0x%" PRIxPTR ")", entry,
           (uintptr_t)(image + entry));
    typedef void(ABI * Entry)(void *, void(ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit_handler);
    gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: the game's entry function returned");
}

int main(int argc, char **argv) {
    InitializeCriticalSection(&log_lock);
    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char *out = NULL, *log_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--log") && i + 1 < argc)
            log_path = argv[++i];
        else if (!strcmp(argv[i], "--stubs-return-zero"))
            stubs_return_zero = 1;
        else if (!strcmp(argv[i], "--list-stubs"))
            list_stubs = 1;
        else if (!strcmp(argv[i], "--gpu-test"))
            gpu_test = 1;
        else if (!strcmp(argv[i], "--no-gpu"))
            no_gpu = 1;
        else if (!strcmp(argv[i], "--app0") && i + 1 < argc)
            snprintf(app0, sizeof(app0), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--user") && i + 1 < argc)
            snprintf(user_dir, sizeof(user_dir), "%s", argv[++i]);
        else if (!out && argv[i][0] != '-')
            out = argv[i];
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }
    if (!out) {
        fprintf(stderr,
                "Usage: %s <out folder from prepare_all.ps1> [--app0 game folder] [--user folder] "
                "[--log file] [--list-stubs] [--stubs-return-zero] [--gpu-test] [--no-gpu]\n",
                argv[0]);
        return 1;
    }
    /* Defaults next to the out folder (scripts\prepare_all.ps1 layout): the game in CUSA02046,
     * saves and other writable data in user. */
    if (!app0[0])
        snprintf(app0, sizeof(app0), "%s\\..\\CUSA02046", out);
    if (!user_dir[0])
        snprintf(user_dir, sizeof(user_dir), "%s\\..\\user", out);
    if (log_path && !(log_file = fopen(log_path, "w")))
        fail("cannot create log file");
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    page_size = system_info.dwPageSize;
    /* Reserve the PS4 address before anything else can land there. */
    void *reserved = VirtualAlloc((void *)GZ_IMAGE_BASE, 64 << 20, MEM_RESERVE, PAGE_NOACCESS);
    check_cpu();
    gz_kernel_start();
    gz_runtime_start();
    gz_memory_start();
    gz_file_configure(app0, user_dir);
    gz_savedata_configure(app0);
    AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);

    char path[1024];
    snprintf(path, sizeof(path), "%s\\boot-linked.bin", out);
    FILE *f = fopen(path, "rb");
    if (!f)
        fail("cannot open boot-linked.bin; run scripts\\prepare_all.ps1");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "BBPROBE5", 8))
        fail("boot-linked.bin: expected BBPROBE5");
    image_size = read64(f);
    entry = read64(f);
    segment_count = read64(f);
    uint64_t reloc_count = read64(f);
    import_count = read64(f);
    uint64_t flags = read64(f);
    if (!(flags & 1))
        fail("prepare.py could not verify libc _init_env");
    if (!image_size || image_size > 512u << 20 || entry >= image_size || !segment_count ||
        segment_count > 64 || reloc_count > 1000000 || !import_count || import_count > 100000)
        fail("boot file limits exceeded");
    procparam = read64(f);
    uint64_t main_tls[4];
    for (int i = 0; i < 4; ++i)
        main_tls[i] = read64(f);
    if (main_tls[2])
        fail("this eboot has static TLS; the Windows loader does not support it yet");
    module_count = read64(f);
    if (!module_count || module_count > sizeof(modules) / sizeof(*modules))
        fail("invalid module count");
    for (uint64_t m = 0; m < module_count; ++m) {
        LinkedModule *x = &modules[m];
        x->base = read64(f);
        x->size = read64(f);
        x->init = read64(f);
        x->tls_address = read64(f);
        x->tls_memsz = read64(f);
        x->tls_filesz = read64(f);
        x->tls_module = read64(f);
        if (x->base >= image_size || !x->size || x->size > image_size - x->base ||
            x->init < x->base || x->init - x->base >= x->size ||
            (x->tls_module &&
             (x->tls_address >= image_size || x->tls_memsz > image_size - x->tls_address ||
              x->tls_memsz > 1 << 20 || x->tls_filesz > x->tls_memsz || x->tls_module < 2 ||
              x->tls_module > 7)))
            fail("invalid linked module metadata");
    }
    uint64_t binding_count = read64(f);
    if (binding_count > import_count)
        fail("invalid binding count");
    uint64_t *bindings = calloc(import_count, sizeof(*bindings)),
             *binding_kinds = calloc(import_count, sizeof(*binding_kinds));
    segments = calloc(segment_count, sizeof(*segments));
    imports = calloc(import_count, sizeof(*imports));
    stub_calls = calloc(import_count, sizeof(*stub_calls));
    Reloc *relocs = calloc(reloc_count ? reloc_count : 1, sizeof(*relocs));
    if (!bindings || !binding_kinds || !segments || !imports || !stub_calls || !relocs)
        fail("allocation failed");
    for (uint64_t i = 0; i < binding_count; ++i) {
        uint64_t index = read64(f), address = read64(f), kind = read64(f);
        int inside = 0;
        for (uint64_t m = 0; m < module_count; ++m)
            if (address >= modules[m].base && address - modules[m].base < modules[m].size)
                inside = 1;
        if (index >= import_count || !inside || (kind != 1 && kind != 2) || bindings[index])
            fail("invalid native binding");
        bindings[index] = address;
        binding_kinds[index] = kind;
    }
    for (uint64_t i = 0; i < segment_count; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > image_size ||
            segments[i].size > image_size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7)
            fail("bad segment");
    }
    for (uint64_t i = 0; i < import_count; ++i) {
        if (fread(imports[i].scoped, 1, 128, f) != 128)
            fail("truncated import names");
        if (!memchr(imports[i].scoped, 0, 128))
            fail("unterminated import name");
    }
    for (uint64_t i = 0; i < reloc_count; ++i) {
        Reloc *r = &relocs[i];
        r->target = read64(f);
        r->kind = read64(f);
        r->value = read64(f);
        r->addend = read64(f);
        if (!mapped(r->target, 8) || r->kind > 2 || (r->kind && r->value >= import_count) ||
            (r->kind != 2 && r->addend) || r->addend >= page_size)
            fail("bad relocation");
    }
    for (uint64_t m = 0; m < module_count; ++m)
        if (!executable(modules[m].init) ||
            (modules[m].tls_module && !mapped(modules[m].tls_address, modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (!mapped(procparam, 64))
        fail("unmapped procparam");
    if (!executable(entry))
        fail("entry is not executable");
    for (uint64_t i = 0; i < import_count; ++i)
        if (bindings[i] &&
            (!mapped(bindings[i], 1) || (binding_kinds[i] == 1 && !executable(bindings[i]))))
            fail("bad native export");

    /* Map the image at the PS4 address. */
    if (reserved)
        VirtualFree(reserved, 0, MEM_RELEASE);
    image = VirtualAlloc((void *)GZ_IMAGE_BASE, round_page(image_size), MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE);
    if (image != (unsigned char *)GZ_IMAGE_BASE)
        fail("cannot map the game at 0x800000000 (address in use)");
    if (fread(image, 1, image_size, f) != image_size || fgetc(f) != EOF)
        fail("incorrect memory image size");
    fclose(f);
    snprintf(path, sizeof(path), "%s\\imports.tsv", out);
    load_imports(path);
#ifdef GZ_GPU
    /* Read process parameters only after loading the image. */
    uint32_t gpu_sdk;
    memcpy(&gpu_sdk, image + procparam + 16, sizeof(gpu_sdk));
    if (!no_gpu)
        gz_gpu_start(gpu_sdk, list_stubs);
#endif

    /* Stubs for functions, no-access pages for variables: any use names the import. */
    traps =
        VirtualAlloc(NULL, round_page(import_count * 32), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    data_traps =
        VirtualAlloc(NULL, import_count * page_size, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (!traps || !data_traps)
        fail("cannot allocate import stubs");
    uintptr_t thunk = (uintptr_t)gz_stub_thunk;
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        uint32_t index = (uint32_t)i;
        t[0] = 0x41;
        t[1] = 0xbb;
        memcpy(t + 2, &index, 4); /* mov r11d, index */
        t[6] = 0x49;
        t[7] = 0xba;
        memcpy(t + 8, &thunk, 8); /* movabs r10, thunk */
        t[16] = 0x41;
        t[17] = 0xff;
        t[18] = 0xe2; /* jmp r10 */
    }
    uint64_t host = 0, native = 0, stubbed = 0;
    unsigned char *seen = calloc(import_count, 1);
    for (uint64_t i = 0; i < reloc_count; ++i) {
        const Reloc *r = &relocs[i];
        uintptr_t value = (uintptr_t)image + r->value;
        if (r->kind) {
            const Import *im = &imports[r->value];
            uintptr_t resolved = gz_resolve(im->library, im->symbol, r->kind == 2);
            int how;
            if (resolved) {
                value = resolved + r->addend;
                how = 1;
            } else if (bindings[r->value]) {
                if (binding_kinds[r->value] != r->kind ||
                    !mapped(bindings[r->value], r->addend + 1))
                    fail("native export kind/range mismatch");
                value = (uintptr_t)image + bindings[r->value] + r->addend;
                how = 2;
            } else {
                value = r->kind == 2 ? (uintptr_t)(data_traps + page_size * r->value + r->addend)
                                     : (uintptr_t)(traps + 32 * r->value);
                how = 3;
            }
            if (!seen[r->value]) {
                seen[r->value] = 1;
                how == 1 ? ++host : how == 2 ? ++native : ++stubbed;
                if (list_stubs && how == 3)
                    gz_log("stub %s %s::%s", r->kind == 2 ? "variable" : "function", im->library,
                           im->symbol);
            }
        }
        memcpy(image + r->target, &value, 8);
    }
    protect(traps, round_page(import_count * 32), 5);
    protect(image, round_page(image_size), 0);
    for (uint64_t i = 0; i < segment_count; ++i)
        protect(image + segments[i].address, round_page(segments[i].size),
                (unsigned)segments[i].flags);
    gz_log("Mapped %" PRIu64 " bytes at 0x%" PRIxPTR ", %" PRIu64 " segments, %" PRIu64
           " relocations",
           image_size, (uintptr_t)image, segment_count, reloc_count);
    gz_log("Imports used: %" PRIu64 " by the runtime, %" PRIu64
           " by the game's own modules, %" PRIu64 " stubs (%s)",
           host, native, stubbed,
           stubs_return_zero ? "log and return 0" : "stop at the first call");
    if (list_stubs) {
        fflush(NULL);
        return 0;
    }
#ifdef GZ_GPU
    extern void gz_pc_configure(void *, uint64_t);
    gz_pc_configure(image, image_size);
    if (gpu_test) {
        if (no_gpu)
            fail("--gpu-test cannot be combined with --no-gpu");
        gz_gpu_self_test();
        gz_stop(0, "GPU TEST: PASS");
    }
#else
    if (gpu_test)
        fail("--gpu-test requires GZ_GPU=ON");
#endif

    for (uint64_t m = 0; m < module_count; ++m)
        if (modules[m].tls_module)
            gz_set_module_tls(modules[m].tls_module, image + modules[m].tls_address,
                              modules[m].tls_filesz, modules[m].tls_memsz);
    gz_set_procparam(image + procparam);

    /* The guest runs on its own thread with a fully committed 16 MiB stack: PS4 code does not
     * probe the stack page by page, so a lazily growing Windows stack could fault. */
    HANDLE thread = CreateThread(NULL, 16u << 20, guest_main, NULL, 0, NULL);
    if (!thread)
        fail("cannot create the guest main thread");
    WaitForSingleObject(thread, INFINITE);
    return 0;
}
