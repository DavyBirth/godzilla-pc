/* Host address space for guest memory on Windows: placeholders and views of one section.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * After shadPS4's src/core/address_space.cpp (Windows AddressSpace::Impl): the free parts of the
 * PS4 user range are reserved as placeholders at start, so Windows puts nothing else there; a
 * guest mapping replaces an exact placeholder with a view of a pagefile-backed section (the PS4's
 * physical memory), which lets the same physical page appear at several addresses.
 * Unlike shadPS4, flexible memory also comes from the section (as in bbport's memfd), so any
 * mapping can be split later (partial unmap or protect) without losing its contents.
 *
 * Every region has one protection: a protect splits regions first. Splitting a mapped region
 * unmaps its view, splits the placeholder and maps both halves again (contents stay in the
 * section). Placeholders from different initial reservations are never coalesced. */
#include "gz.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
typedef PVOID(WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                        void *, ULONG);
typedef BOOL(WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
static VirtualAlloc2Fn virtual_alloc2;
static MapViewOfFile3Fn map_view3;
static UnmapViewOfFile2Fn unmap_view2;

typedef struct {
    uintptr_t base;
    uint64_t size;
    uint64_t phys;
    DWORD prot;
    int mapped, origin;
} Region;
static Region *regions;
static size_t region_count, region_capacity;
static SRWLOCK lock = SRWLOCK_INIT;
static HANDLE section, process;
static unsigned char *backing;
static uint64_t backing_size;

static __attribute__((noreturn)) void vmem_fail(const char *what, uintptr_t address,
                                                uint64_t size) {
    gz_stop(GZ_EXIT_RUNTIME_STOP,
            "STOP: host memory: %s at 0x%llx+0x%llx failed (Windows error %lu)", what,
            (unsigned long long)address, (unsigned long long)size, GetLastError());
}
static void insert(size_t at, Region r) {
    if (region_count == region_capacity) {
        size_t capacity = region_capacity ? region_capacity * 2 : 256;
        Region *next = realloc(regions, capacity * sizeof(*regions));
        if (!next)
            gz_stop(GZ_EXIT_RUNTIME_STOP, "STOP: host memory: out of region slots");
        regions = next;
        region_capacity = capacity;
    }
    memmove(regions + at + 1, regions + at, (region_count - at) * sizeof(*regions));
    regions[at] = r;
    ++region_count;
}
static void erase(size_t at, size_t n) {
    memmove(regions + at, regions + at + n, (region_count - at - n) * sizeof(*regions));
    region_count -= n;
}
/* First region with base + size > address. */
static size_t find(uintptr_t address) {
    size_t lo = 0, hi = region_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (regions[mid].base + regions[mid].size <= address)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void map_view(Region *r) {
    /* A view cannot be mapped inaccessible: map it read/write, then protect (as shadPS4). */
    DWORD prot = r->prot == PAGE_NOACCESS ? PAGE_READWRITE : r->prot;
    if (map_view3(section, process, (void *)r->base, r->phys, r->size, MEM_REPLACE_PLACEHOLDER,
                  prot, NULL, 0) != (void *)r->base)
        vmem_fail("MapViewOfFile3", r->base, r->size);
    DWORD old;
    if (prot != r->prot && !VirtualProtect((void *)r->base, r->size, r->prot, &old))
        vmem_fail("VirtualProtect", r->base, r->size);
}
static void unmap_view(Region *r) {
    if (!unmap_view2(process, (void *)r->base, MEM_PRESERVE_PLACEHOLDER))
        vmem_fail("UnmapViewOfFile2", r->base, r->size);
}
/* Makes a region boundary at address (inside our placeholders). */
static void split_at(uintptr_t address) {
    size_t i = find(address);
    if (i == region_count || regions[i].base >= address)
        return;
    Region *r = &regions[i];
    uint64_t left = address - r->base;
    Region right = *r;
    right.base = address;
    right.size = r->size - left;
    if (r->mapped) {
        right.phys = r->phys + left;
        unmap_view(r);
    }
    if (!VirtualFree((void *)r->base, left, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        vmem_fail("placeholder split", r->base, left);
    r->size = left;
    if (r->mapped)
        map_view(r);
    insert(i + 1, right);
    if (right.mapped)
        map_view(&regions[i + 1]);
}
/* [address, end) as one free placeholder region; returns its index. */
static size_t placeholder(uintptr_t address, uintptr_t end) {
    split_at(address);
    split_at(end);
    size_t first = find(address), last = first;
    if (first == region_count || regions[first].base != address)
        vmem_fail("map outside reserved space", address, end - address);
    for (;;) {
        if (regions[last].mapped || regions[last].origin != regions[first].origin)
            vmem_fail("map over a mapping or a host hole", address, end - address);
        if (regions[last].base + regions[last].size >= end)
            break;
        if (last + 1 == region_count ||
            regions[last + 1].base != regions[last].base + regions[last].size)
            vmem_fail("map over a host hole", address, end - address);
        ++last;
    }
    if (last > first) {
        if (!VirtualFree((void *)address, end - address, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
            vmem_fail("placeholder coalesce", address, end - address);
        regions[first].size = end - address;
        erase(first + 1, last - first);
    }
    return first;
}

int gz_vmem_init(uintptr_t min, uintptr_t max, uint64_t size) {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) {
        virtual_alloc2 = (VirtualAlloc2Fn)(void *)GetProcAddress(kernelbase, "VirtualAlloc2");
        map_view3 = (MapViewOfFile3Fn)(void *)GetProcAddress(kernelbase, "MapViewOfFile3");
        unmap_view2 = (UnmapViewOfFile2Fn)(void *)GetProcAddress(kernelbase, "UnmapViewOfFile2");
    }
    if (!virtual_alloc2 || !map_view3 || !unmap_view2)
        return -1; /* Windows 10 1803 or newer */
    process = GetCurrentProcess();
    /* Reserve every free part of [min, max) as placeholders, so nothing else lands there. */
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    uintptr_t granularity = info.dwAllocationGranularity;
    int origin = 0;
    for (uintptr_t at = min; at < max;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)at, &m, sizeof(m)))
            return -1;
        uintptr_t start = (uintptr_t)m.BaseAddress, end = start + m.RegionSize;
        if (end > max)
            end = max;
        if (m.State == MEM_FREE) {
            uintptr_t a = (at + granularity - 1) & ~(granularity - 1), b = end & ~(granularity - 1);
            if (b > a) {
                if (virtual_alloc2(process, (void *)a, b - a, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                   PAGE_NOACCESS, NULL, 0) != (void *)a)
                    return -1;
                insert(region_count, (Region){a, b - a, 0, PAGE_NOACCESS, 0, origin++});
            }
        }
        at = end;
    }
    /* The PS4's physical memory: committed on demand by the pagefile (SEC_COMMIT, as shadPS4). */
    backing_size = size;
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                 (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!section)
        return -1;
    /* A host view of all of it, for runtime writes (zeroing released memory, later the GPU). */
    backing = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size);
    return backing ? 0 : -1;
}
unsigned char *gz_vmem_backing(void) {
    return backing;
}

void gz_vmem_map(uintptr_t address, uint64_t size, uint64_t phys, DWORD prot) {
    if (phys > backing_size || size > backing_size - phys)
        vmem_fail("map beyond physical memory", address, size);
    AcquireSRWLockExclusive(&lock);
    Region *r = &regions[placeholder(address, address + size)];
    r->mapped = 1;
    r->phys = phys;
    r->prot = prot;
    map_view(r);
    ReleaseSRWLockExclusive(&lock);
}
void gz_vmem_unmap(uintptr_t address, uint64_t size) {
    AcquireSRWLockExclusive(&lock);
    split_at(address);
    split_at(address + size);
    for (size_t i = find(address); i < region_count && regions[i].base < address + size; ++i)
        if (regions[i].mapped) {
            unmap_view(&regions[i]);
            regions[i].mapped = 0;
            regions[i].prot = PAGE_NOACCESS;
        }
    ReleaseSRWLockExclusive(&lock);
}
void gz_vmem_protect(uintptr_t address, uint64_t size, DWORD prot) {
    AcquireSRWLockExclusive(&lock);
    split_at(address);
    split_at(address + size);
    for (size_t i = find(address); i < region_count && regions[i].base < address + size; ++i) {
        if (!regions[i].mapped || regions[i].prot == prot)
            continue;
        DWORD old;
        if (!VirtualProtect((void *)regions[i].base, regions[i].size, prot, &old))
            vmem_fail("VirtualProtect", regions[i].base, regions[i].size);
        regions[i].prot = prot;
    }
    ReleaseSRWLockExclusive(&lock);
}
/* 0 when [address, address+size) lies in one reservation of ours; otherwise the next address
 * worth trying (past the host hole or reservation boundary). */
uintptr_t gz_vmem_blocked(uintptr_t address, uint64_t size) {
    AcquireSRWLockShared(&lock);
    uintptr_t retry = 0, end = address + size;
    size_t i = find(address);
    if (i == region_count)
        retry = UINTPTR_MAX;
    else if (regions[i].base > address)
        retry = regions[i].base;
    else {
        int origin = regions[i].origin;
        while (regions[i].base + regions[i].size < end) {
            if (i + 1 == region_count) {
                retry = UINTPTR_MAX;
                break;
            }
            if (regions[i + 1].base != regions[i].base + regions[i].size ||
                regions[i + 1].origin != origin) {
                retry = regions[i + 1].base;
                break;
            }
            ++i;
        }
    }
    ReleaseSRWLockShared(&lock);
    return retry;
}
