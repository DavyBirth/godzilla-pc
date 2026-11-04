/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations, protection changes
 * and queries, on the host address space of rt_vmem.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from bbport's src/runtime_memory.c (github.com/deadinside28/bloodborne_pc) without its
 * GPU hooks and dma-buf chunks (M5). Guest addresses stay in the PS4 user range below 1 TiB:
 * GPU descriptors encode 40-bit addresses.
 * Physical layout of the section: direct memory [0, POOL_SIZE), flexible [POOL_SIZE, +FLEX_SPAN). */
#include "gz.h"
#ifdef GZ_GPU
#include "gpu.h"
#endif
#include <windows.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
#define PAGE UINT64_C(16384)
#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
#define LIMIT 4096
#define INVALID GZ_ERR(GZ_EINVAL)
#define NO_MEMORY GZ_ERR(GZ_ENOMEM)
#define ACCESS GZ_ERR(13)
#define NO_SPACE GZ_ERR(35)
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80

/* sceKernelGetDirectMemorySize on a retail PS4: 5056 MiB; GZ_DMEM_MB raises it. */
static uint64_t pool_size;
#define POOL_SIZE pool_size

enum { KIND_RESERVED = 1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct {
    uint64_t start, size;
    int type, used;
} Block;
typedef struct {
    uintptr_t start, end;
    int kind, prot, type;
    uint64_t phys;
} Vma;
typedef struct {
    uintptr_t start, end;
    uint64_t offset;
    int32_t protection, memory_type;
    uint32_t flags;
    char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct {
    void *start;
    uint64_t offset, length;
    int8_t prot, type;
    int16_t reserved;
    int32_t operation;
} BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo) == 72, "PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry) == 32, "PS4 batch map entry layout");

/* Region table lock. Code under it may nest (bbport): a thread holding it exclusively skips
 * relocking, and nested reads on one thread take it once (SRW locks are not recursive). */
static SRWLOCK lock = SRWLOCK_INIT;
static _Thread_local unsigned exclusive_depth, shared_depth;
static void write_lock(void) {
    if (!exclusive_depth++)
        AcquireSRWLockExclusive(&lock);
}
static void write_unlock(void) {
    if (!--exclusive_depth)
        ReleaseSRWLockExclusive(&lock);
}
static void read_lock(void) {
    if (!exclusive_depth && !shared_depth++)
        AcquireSRWLockShared(&lock);
}
static void read_unlock(void) {
    if (!exclusive_depth && !--shared_depth)
        ReleaseSRWLockShared(&lock);
}

static Block blocks[LIMIT];
static Vma *vmas;
static size_t vma_count, vma_capacity;
static uint64_t flex_bitmap[FLEX_SPAN / PAGE / 64];
static size_t allocations, maps, flexible_maps, protects;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) {
    return a >= PAGE && a <= POOL_SIZE && !(a & (a - 1));
}
static uint64_t align_up(uint64_t n, uint64_t a) {
    return (n + a - 1) & ~(a - 1);
}
/* PS4 protection bits: 1 CPU read, 2 CPU write, 4 CPU exec, 0x10 GPU read, 0x20 GPU write.
 * GPU access needs host access for the future GPU backend (as bbport and shadPS4). */
static DWORD host_prot(int prot) {
    int read = (prot & 0x11) != 0, write = (prot & 0x22) != 0, exec = (prot & 4) != 0;
    if (exec)
        return write ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return write ? PAGE_READWRITE : read ? PAGE_READONLY : PAGE_NOACCESS;
}
/* Released physical memory reads as zero when reused (bbport punches a hole in its memfd). */
static void zero_phys(uint64_t phys, uint64_t size) {
    memset(gz_vmem_backing() + phys, 0, size);
}

static int flex_test(uint64_t page) {
    return (int)((flex_bitmap[page / 64] >> (page % 64)) & 1);
}
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p = first; p < first + count; ++p) {
        if (used)
            flex_bitmap[p / 64] |= UINT64_C(1) << (p % 64);
        else
            flex_bitmap[p / 64] &= ~(UINT64_C(1) << (p % 64));
    }
}
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages = size / PAGE, total = FLEX_SPAN / PAGE, run = 0;
    for (uint64_t p = 0; p < total; ++p) {
        run = flex_test(p) ? 0 : run + 1;
        if (run == pages) {
            flex_set(p + 1 - pages, pages, 1);
            return POOL_SIZE + (p + 1 - pages) * PAGE;
        }
    }
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys - POOL_SIZE) / PAGE, size / PAGE, 0);
    zero_phys(phys, size);
}

static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo = 0, hi = vma_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (vmas[mid].end <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (vma_count == vma_capacity) {
        size_t capacity = vma_capacity ? vma_capacity * 2 : 256;
        Vma *next = realloc(vmas, capacity * sizeof(*vmas));
        if (!next)
            return -1;
        vmas = next;
        vma_capacity = capacity;
    }
    memmove(vmas + at + 1, vmas + at, (vma_count - at) * sizeof(*vmas));
    vmas[at] = v;
    ++vma_count;
    return 0;
}
static void vma_erase(size_t at, size_t n) {
    memmove(vmas + at, vmas + at + n, (vma_count - at - n) * sizeof(*vmas));
    vma_count -= n;
}
static int split_at(uintptr_t a) {
    size_t i = vma_index(a);
    if (i == vma_count || vmas[i].start >= a)
        return 0;
    Vma right = vmas[i];
    right.start = a;
    if (right.kind != KIND_RESERVED)
        right.phys += a - vmas[i].start;
    vmas[i].end = a;
    return vma_insert(i + 1, right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i = vma_index(start);
    for (uintptr_t at = start; at < end; ++i) {
        if (i == vma_count || vmas[i].start > at ||
            (!allow_reserved && vmas[i].kind == KIND_RESERVED))
            return 0;
        at = vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i = vma_index(start); i < vma_count && vmas[i].start < end; ++i)
        if (!ignore_reserved || vmas[i].kind != KIND_RESERVED)
            return 1;
    return 0;
}
/* First fit in the PS4 user range from the hint, skipping guest mappings and the holes the
 * host occupied before our reservation. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at = align_up(hint < USER_MIN ? USER_MIN : hint, alignment);
    while (at + size <= USER_MAX) {
        size_t i = vma_index(at);
        if (i < vma_count && vmas[i].start < at + size) {
            at = align_up(vmas[i].end, alignment);
            continue;
        }
        uintptr_t retry = gz_vmem_blocked(at, size);
        if (!retry)
            return at;
        if (retry == UINTPTR_MAX)
            return 0;
        at = align_up(retry, alignment);
    }
    return 0;
}
/* Forgets [start, end) in the table and the host (the caller maps something new there). */
static void drop_range(uintptr_t start, uintptr_t end) {
    int error;
    size_t i = carve(start, end, &error), n = 0;
    while (i + n < vma_count && vmas[i + n].end <= end && vmas[i + n].start >= start) {
        Vma *v = &vmas[i + n];
        if (v->kind != KIND_RESERVED) {
#ifdef GZ_GPU
            gz_gpu_unmap(v->start, v->end - v->start);
#endif
            gz_vmem_unmap(v->start, v->end - v->start);
        }
        if (v->kind == KIND_FLEXIBLE) {
            flexible_bytes -= v->end - v->start;
            flex_free(v->phys, v->end - v->start);
        }
        ++n;
    }
    vma_erase(i, n);
}
/* Places a mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment, int kind,
                     int type, uint64_t phys) {
    uintptr_t address = (uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address % PAGE)
            return INVALID;
        if (address < USER_MIN || address + size > USER_MAX || gz_vmem_blocked(address, size))
            return NO_MEMORY;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address, address + size, 1))
            return NO_MEMORY;
    } else {
        address = find_free(address, size, alignment);
        if (!address)
            return NO_MEMORY;
    }
    drop_range(address, address + size);
    if (kind != KIND_RESERVED)
        gz_vmem_map(address, size, phys, host_prot(prot));
    if (vma_insert(vma_index(address), (Vma){address, address + size, kind, prot, type, phys}))
        return NO_MEMORY;
#ifdef GZ_GPU
    if (kind != KIND_RESERVED)
        gz_gpu_map(address, size);
#endif
    if (kind == KIND_FLEXIBLE)
        flexible_bytes += size;
    *inout = (void *)address;
    return 0;
}
static int block_type(uint64_t phys) {
    for (int i = 0; i < LIMIT; ++i)
        if (blocks[i].used && phys >= blocks[i].start && phys - blocks[i].start < blocks[i].size)
            return blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at = phys; at < phys + size;) {
        int found = 0;
        for (int i = 0; i < LIMIT; ++i)
            if (blocks[i].used && at >= blocks[i].start && at - blocks[i].start < blocks[i].size) {
                at = blocks[i].start + blocks[i].size;
                found = 1;
                break;
            }
        if (!found)
            return 0;
    }
    return 1;
}

/* ---- direct memory ---- */
static ABI uint64_t direct_size(void) {
    return POOL_SIZE;
}
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size, uint64_t alignment,
                                   int type, int64_t *out) {
    if (!alignment)
        alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) ||
        type < 0 || type > 10)
        return INVALID;
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low)
        return NO_SPACE;
    write_lock();
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i = 0; i < LIMIT; ++i)
        if (!blocks[i].used) {
            slot = i;
            break;
        }
    for (;;) {
        if (slot < 0 || start > end || size > end - start) {
            write_unlock();
            return NO_SPACE;
        }
        int overlap = 0;
        for (int i = 0; i < LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start + b.size && b.start < start + size) {
                start = align_up(b.start + b.size, alignment);
                overlap = 1;
                break;
            }
        }
        if (!overlap)
            break;
    }
    blocks[slot] = (Block){start, size, type, 1};
    *out = (int64_t)start;
    ++allocations;
    live_bytes += size;
    write_unlock();
    gz_log("Runtime: direct allocation offset=0x%" PRIx64 ", size=0x%" PRIx64 ", type=%d", start,
           size, type);
    return 0;
}
static ABI int32_t direct_allocate_main(uint64_t size, uint64_t alignment, int type, int64_t *out) {
    return direct_allocate(0, (int64_t)POOL_SIZE, size, alignment, type, out);
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                 uint64_t alignment) {
    if (!alignment)
        alignment = PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE ||
        !valid_alignment(alignment) || (prot & ~0x37))
        return INVALID;
    if (!direct_range_allocated((uint64_t)physical, size))
        return INVALID;
    int32_t result = place(out, size, prot, flags, alignment, KIND_DIRECT,
                           block_type((uint64_t)physical), (uint64_t)physical);
    if (!result)
        ++maps;
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical,
                              uint64_t alignment) {
    write_lock();
    int32_t result = map_direct_locked(out, size, prot, flags, physical, alignment);
    write_unlock();
    if (!result)
        gz_log("Runtime: mapped direct memory %p, size=0x%" PRIx64 ", prot=0x%x", *out, size, prot);
    else
        gz_log("Runtime: mapping direct memory 0x%" PRIx64 ", size=0x%" PRIx64
               " failed (error 0x%x)",
               (uint64_t)physical, size, (unsigned)result);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags,
                                    int64_t physical, uint64_t alignment, const char *name) {
    (void)name;
    return direct_map(out, size, prot, flags, physical, alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start % PAGE || !size)
        return INVALID;
    drop_range(start, start + align_up(size, PAGE));
    return 0;
}
static ABI int32_t kernel_munmap(void *address, uint64_t size) {
    write_lock();
    int32_t result = unmap_locked((uintptr_t)address, size);
    write_unlock();
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE)
        return INVALID;
    if (!size)
        return 0;
    write_lock();
    if (!direct_range_allocated(start, size)) {
        write_unlock();
        return INVALID;
    }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i = 0; i < vma_count;) {
        Vma v = vmas[i];
        uint64_t length = v.end - v.start;
        if (v.kind == KIND_DIRECT && v.phys < start + size && start < v.phys + length) {
            uint64_t a = v.phys > start ? v.phys : start,
                     b = v.phys + length < start + size ? v.phys + length : start + size;
            unmap_locked(v.start + (a - v.phys), b - a);
            i = vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i = 0; i < LIMIT; ++i) {
        Block *b = &blocks[i];
        if (!b->used || b->start >= start + size || start >= b->start + b->size)
            continue;
        uint64_t a = b->start > start ? b->start : start,
                 e = b->start + b->size < start + size ? b->start + b->size : start + size;
        Block left = *b, right = *b;
        left.size = a - b->start;
        right.start = e;
        right.size = b->start + b->size - e;
        b->used = 0;
        if (left.size)
            *b = left;
        if (right.size)
            for (int j = 0; j < LIMIT; ++j)
                if (!blocks[j].used) {
                    blocks[j] = right;
                    break;
                }
        live_bytes -= e - a;
    }
    zero_phys(start, size);
    write_unlock();
    return 0;
}

/* ---- flexible memory ---- */
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size % PAGE || (prot & ~0x37))
        return INVALID;
    if (flexible_bytes + size > FLEXIBLE_SIZE)
        return NO_MEMORY;
    uint64_t phys = flex_alloc(size);
    if (phys == UINT64_MAX)
        return NO_MEMORY;
    int32_t result = place(inout, size, prot, flags, PAGE, KIND_FLEXIBLE, 0, phys);
    if (result)
        flex_free(phys, size);
    else
        ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result = map_flexible_locked(inout, size, prot, flags);
    write_unlock();
    if (!result)
        gz_log("Runtime: mapped flexible memory %p, size=0x%" PRIx64 ", prot=0x%x", *inout, size,
               prot);
    else
        gz_log("Runtime: flexible map of 0x%" PRIx64 " bytes failed (0x%x), in use=0x%" PRIx64,
               size, (unsigned)result, flexible_bytes);
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags,
                                      const char *name) {
    (void)name;
    return map_flexible(inout, size, prot, flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) {
    return kernel_munmap(address, size);
}
/* POSIX mmap: the game's libc grows its malloc heap with anonymous private mappings (bbport).
 * They come from flexible memory; files are not mapped. MAP_FAILED (-1) and errno on failure. */
static ABI void *posix_mmap(void *addr, uint64_t size, int prot, int flags, int fd,
                            int64_t offset) {
    enum { MAP_ANON_FLAG = 0x1000, POSIX_ENODEV = 19 };
    if (!(flags & MAP_ANON_FLAG) || fd != -1 || offset) {
        *gz_errno() = POSIX_ENODEV;
        return (void *)-1;
    }
    if (!size) {
        *gz_errno() = GZ_EINVAL;
        return (void *)-1;
    }
    void *out = (flags & MAP_FIXED_FLAG) ? addr : NULL;
    int32_t result = map_flexible(&out, align_up(size, PAGE), prot & 0x37, flags & MAP_FIXED_FLAG);
    gz_log("Runtime: mmap 0x%" PRIx64 " bytes for the game (its heap grows)", size);
    if (result) {
        *gz_errno() = result == NO_MEMORY ? GZ_ENOMEM : GZ_EINVAL;
        return (void *)-1;
    }
    return out;
}
static ABI int32_t posix_munmap(void *address, uint64_t size) {
    int32_t r = kernel_munmap(address, size);
    if (r) {
        *gz_errno() = GZ_EINVAL;
        return -1;
    }
    return 0;
}
static ABI int32_t available_flexible(uint64_t *out) {
    if (!out)
        return INVALID;
    read_lock();
    *out = FLEXIBLE_SIZE - flexible_bytes;
    read_unlock();
    return 0;
}

/* ---- reservations, protection, queries ---- */
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment)
        alignment = PAGE;
    if (!inout || !size || size % PAGE || !valid_alignment(alignment))
        return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout, (uintptr_t)*inout + size, 0)) {
        write_unlock();
        return NO_MEMORY;
    }
    int32_t result = place(inout, size, 0, flags, alignment, KIND_RESERVED, 0, 0);
    write_unlock();
    if (!result)
        gz_log("Runtime: reserved virtual range %p, size=0x%" PRIx64, *inout, size);
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end = start + align_up(size, PAGE);
    start &= ~(uintptr_t)(PAGE - 1);
    if ((prot & ~0x37) || !covered(start, end, 0))
        return INVALID;
    int error;
    size_t i = carve(start, end, &error);
    if (error)
        return NO_MEMORY;
    gz_vmem_protect(start, end - start, host_prot(prot));
    for (; i < vma_count && vmas[i].start < end; ++i) {
        vmas[i].prot = prot;
        if (type >= 0)
            vmas[i].type = type;
    }
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result = protect_locked((uintptr_t)address, size, prot, -1);
    write_unlock();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i = vma_index((uintptr_t)address);
    if (i == vma_count || vmas[i].start > (uintptr_t)address || vmas[i].kind == KIND_RESERVED) {
        read_unlock();
        return ACCESS;
    }
    /* The whole contiguous run with identical protection. */
    size_t first = i, last = i;
    while (first && vmas[first - 1].end == vmas[first].start &&
           vmas[first - 1].kind != KIND_RESERVED && vmas[first - 1].prot == vmas[i].prot)
        --first;
    while (last + 1 < vma_count && vmas[last + 1].start == vmas[last].end &&
           vmas[last + 1].kind != KIND_RESERVED && vmas[last + 1].prot == vmas[i].prot)
        ++last;
    if (start)
        *start = (void *)vmas[first].start;
    if (end)
        *end = (void *)vmas[last].end;
    if (prot)
        *prot = (uint32_t)vmas[i].prot;
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info,
                                 uint64_t info_size) {
    if (!info || info_size != sizeof(*info))
        return INVALID;
    read_lock();
    size_t i = vma_index((uintptr_t)address);
    if (i == vma_count || (vmas[i].start > (uintptr_t)address && !(flags & 1))) {
        read_unlock();
        return ACCESS;
    }
    Vma v = vmas[i];
    memset(info, 0, sizeof(*info));
    info->start = v.start;
    info->end = v.end;
    info->offset = v.kind == KIND_DIRECT ? v.phys : 0;
    info->protection = v.prot;
    info->memory_type = v.type;
    info->flags = (v.kind == KIND_FLEXIBLE ? 1u : 0) | (v.kind == KIND_DIRECT ? 2u : 0) |
                  (v.kind != KIND_RESERVED ? 16u : 0);
    strcpy(info->name, v.kind == KIND_RESERVED ? "reserved"
                       : v.kind == KIND_DIRECT ? "direct"
                                               : "flexible");
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i = 0; i < LIMIT; ++i)
        if (blocks[i].used && phys >= blocks[i].start && phys - blocks[i].start < blocks[i].size) {
            if (type)
                *type = blocks[i].type;
            if (start)
                *start = (void *)(uintptr_t)blocks[i].start;
            if (end)
                *end = (void *)(uintptr_t)(blocks[i].start + blocks[i].size);
            read_unlock();
            return 0;
        }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count < 0)
        return INVALID;
    int32_t result = 0;
    int done = 0;
    write_lock();
    for (; done < count; ++done) {
        BatchEntry *e = &entries[done];
        switch (e->operation) {
        case 0:
            result = map_direct_locked(&e->start, e->length, (uint8_t)e->prot, flags,
                                       (int64_t)e->offset, 0);
            break;
        case 1:
            result = unmap_locked((uintptr_t)e->start, e->length);
            break;
        case 2:
            result = protect_locked((uintptr_t)e->start, e->length, (uint8_t)e->prot, -1);
            break;
        case 3:
            result = map_flexible_locked(&e->start, e->length, (uint8_t)e->prot, flags);
            break;
        case 4:
            result = protect_locked((uintptr_t)e->start, e->length, (uint8_t)e->prot, e->type);
            break;
        default:
            result = INVALID;
        }
        if (result)
            break;
    }
    write_unlock();
    if (processed)
        *processed = done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries, count, processed, MAP_FIXED_FLAG);
}

uint64_t gz_memory_clamp(uintptr_t address, uint64_t size) {
    if (!size || address + size < address)
        return 0;
    /* Constants in a caller-provided native image are not part of the PS4 user heap. */
    if (address < USER_MIN && address >= 0x10000) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return 0;
        uint64_t available = (uintptr_t)info.BaseAddress + info.RegionSize - address;
        return available < size ? available : size;
    }
    read_lock();
    uintptr_t at = address, end = address + size;
    for (size_t i = vma_index(at); at < end && i < vma_count; ++i) {
        if (vmas[i].start > at || vmas[i].kind == KIND_RESERVED)
            break;
        at = vmas[i].end < end ? vmas[i].end : end;
    }
    read_unlock();
    return at - address;
}
int gz_memory_copy(uintptr_t address, void *buffer, uint64_t size, int write) {
    if (address + size < address)
        return 0;
    if (address < USER_MIN) {
        if (write || gz_memory_clamp(address, size) != size)
            return 0;
        memcpy(buffer, (void *)address, size);
        return 1;
    }
    read_lock();
    if (write && !covered(address, address + size, 0)) {
        read_unlock();
        return 0;
    }
    unsigned char *out = buffer, *backing = gz_vmem_backing();
    uintptr_t end = address + size;
    for (size_t i = vma_index(address); address < end;) {
        int mapped = i < vma_count && vmas[i].start <= address && vmas[i].kind != KIND_RESERVED;
        uintptr_t next = i == vma_count            ? end
                         : vmas[i].start > address ? vmas[i].start
                                                   : vmas[i].end;
        if (next > end)
            next = end;
        uint64_t n = next - address;
        if (mapped) {
            unsigned char *physical = backing + vmas[i].phys + address - vmas[i].start;
            if (write)
                memcpy(physical, out, n);
            else
                memcpy(out, physical, n);
        } else if (!write)
            memset(out, 0, n);
        address = next;
        out += n;
        if (i < vma_count && address >= vmas[i].end)
            ++i;
    }
    read_unlock();
    return 1;
}
void gz_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    if (!size || address + size < address)
        return;
    const uintptr_t start = address & ~(uintptr_t)4095;
    const uintptr_t end = align_up(address + size, 4096);
    read_lock();
    for (size_t i = vma_index(start); i < vma_count && vmas[i].start < end; ++i) {
        Vma *v = &vmas[i];
        if (v->kind == KIND_RESERVED)
            continue;
        uintptr_t a = v->start > start ? v->start : start, b = v->end < end ? v->end : end;
        DWORD old, prot = !read                       ? PAGE_NOACCESS
                          : write && (v->prot & 0x22) ? host_prot(v->prot)
                                                      : PAGE_READONLY;
        if (!VirtualProtect((void *)a, b - a, prot, &old)) {
            DWORD error = GetLastError();
            read_unlock();
            gz_stop(23, "STOP: GPU VirtualProtect at 0x%" PRIxPTR " (error %lu)", a, error);
        }
    }
    read_unlock();
}
void gz_memory_replay_gpu(void) {
#ifdef GZ_GPU
    read_lock();
    for (size_t i = 0; i < vma_count; ++i)
        if (vmas[i].kind != KIND_RESERVED)
            gz_gpu_map(vmas[i].start, vmas[i].end - vmas[i].start);
    read_unlock();
#endif
}
void gz_memory_report(void) {
    gz_log("Runtime: direct allocations=%zu, maps=%zu, live=0x%" PRIx64
           "; flexible maps=%zu, in use=0x%" PRIx64 "; protects=%zu, regions=%zu",
           allocations, maps, live_bytes, flexible_maps, flexible_bytes, protects, vma_count);
}

void gz_memory_start(void) {
    const char *env = getenv("GZ_DMEM_MB");
    uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
    if (mb < 5056 || mb > 16384)
        mb = 5056;
    pool_size = mb * 1024 * 1024;
    if (gz_vmem_init(USER_MIN, USER_MAX, POOL_SIZE + FLEX_SPAN))
        gz_stop(GZ_EXIT_RUNTIME_STOP,
                "STOP: cannot set up guest memory (Windows error %lu; Windows 10 1803 or newer is "
                "needed)",
                GetLastError());
}

static const GzExport exports[] = {
    {"libkernel", "sceKernelGetDirectMemorySize", (void *)direct_size},
    {"libkernel", "sceKernelAllocateDirectMemory", (void *)direct_allocate},
    {"libkernel", "sceKernelAllocateMainDirectMemory", (void *)direct_allocate_main},
    {"libkernel", "sceKernelMapDirectMemory", (void *)direct_map},
    {"libkernel", "sceKernelMapNamedDirectMemory", (void *)direct_map_named},
    {"libkernel", "sceKernelReleaseDirectMemory", (void *)direct_release},
    {"libkernel", "sceKernelMunmap", (void *)kernel_munmap},
    {"libkernel", "sceKernelMapFlexibleMemory", (void *)map_flexible},
    {"libkernel", "sceKernelMapNamedFlexibleMemory", (void *)map_flexible_named},
    {"libkernel", "sceKernelReleaseFlexibleMemory", (void *)release_flexible},
    {"libkernel", "sceKernelAvailableFlexibleMemorySize", (void *)available_flexible},
    {"libkernel", "sceKernelReserveVirtualRange", (void *)reserve_range},
    {"libkernel", "sceKernelMprotect", (void *)kernel_mprotect},
    {"libkernel", "sceKernelQueryMemoryProtection", (void *)query_protection},
    {"libkernel", "sceKernelVirtualQuery", (void *)virtual_query},
    {"libkernel", "sceKernelGetDirectMemoryType", (void *)direct_memory_type},
    {"libkernel", "sceKernelBatchMap", (void *)batch_map},
    {"libkernel", "sceKernelBatchMap2", (void *)batch_map2},
    {"libkernel", "mmap", (void *)posix_mmap},
    {"libkernel", "munmap", (void *)posix_munmap},
};
uintptr_t gz_memory_resolve(const char *library, const char *symbol) {
    return GZ_LOOKUP(exports, library, symbol);
}
