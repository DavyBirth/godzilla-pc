// Windows x64 relays preserve registers/flags and the caller's System V ABI.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_hook.h"
#include <windows.h>
#include <Zydis/Zydis.h>
#include <cstring>
#include <limits>
#include <vector>
#include <exception>
#include <mutex>
namespace GzNative {
namespace {
std::mutex hook_mutex;
bool read(const void *source, void *target, size_t size) {
    SIZE_T got{};
    return ReadProcessMemory(GetCurrentProcess(), source, target, size, &got) && got == size;
}
void jump(unsigned char *target, const void *address) {
    const unsigned char indirect[] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(target, indirect, 6);
    std::memcpy(target + 6, &address, 8);
}
bool relocatable(std::span<const unsigned char> bytes) {
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        return false;
    size_t at{};
    while (at < bytes.size()) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + at, bytes.size() - at,
                                                 &instruction, operands)))
            return false;
        switch (instruction.meta.category) {
        case ZYDIS_CATEGORY_CALL:
        case ZYDIS_CATEGORY_COND_BR:
        case ZYDIS_CATEGORY_UNCOND_BR:
        case ZYDIS_CATEGORY_RET:
        case ZYDIS_CATEGORY_SYSCALL:
        case ZYDIS_CATEGORY_SYSRET:
        case ZYDIS_CATEGORY_INTERRUPT:
            return false;
        default:
            break;
        }
        for (unsigned i = 0; i < instruction.operand_count; ++i) {
            const auto &operand = operands[i];
            if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                (operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP))
                return false;
            if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative)
                return false;
        }
        at += instruction.length;
    }
    return at == bytes.size();
}
unsigned char *allocate_near(uintptr_t entry) {
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    const uintptr_t step = system.dwAllocationGranularity;
    const uintptr_t center = entry - entry % step;
    for (uintptr_t i = 1; i <= 256; ++i)
        for (bool above : {true, false}) {
            const uintptr_t distance = i * step;
            if ((above && center > (std::numeric_limits<uintptr_t>::max)() - distance) ||
                (!above && center < distance))
                continue;
            const uintptr_t address = above ? center + distance : center - distance;
            if (address < reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress) ||
                address > reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress))
                continue;
            auto *memory = static_cast<unsigned char *>(VirtualAlloc(
                reinterpret_cast<void *>(address), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (memory)
                return memory;
        }
    return nullptr;
}
// Continuing after an unrecoverable protection/cache failure could execute a
// partially patched instruction. This terminates only this broken process.
[[noreturn]] void fatal() {
    RaiseFailFastException(nullptr, nullptr, 0);
    std::terminate();
}
void restore_protection(void *address, size_t length, DWORD protection) {
    DWORD ignored{};
    if (!VirtualProtect(address, length, protection, &ignored))
        fatal();
}
} // namespace
uint64_t HookSignature(std::span<const std::byte> bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (auto value : bytes)
        hash = (hash ^ std::to_integer<unsigned char>(value)) * 1099511628211ull;
    return hash;
}
HookResult FunctionHook::Install(HookRequest request, void *replacement) {
    std::lock_guard lock{hook_mutex};
    if (Installed())
        return HookResult::AlreadyInstalled;
    if (!request.entry || !replacement || request.entry == replacement || request.displaced < 5 ||
        request.displaced > original.size() || request.signature_bytes < request.displaced ||
        request.signature_bytes > 4096 || request.signature_bytes > request.available)
        return HookResult::InvalidRange;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(request.entry, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        request.signature_bytes >
            region.RegionSize - (uintptr_t(request.entry) - uintptr_t(region.BaseAddress)))
        return HookResult::Unreadable;
    const DWORD access = region.Protect & 0xff;
    if (access != PAGE_EXECUTE && access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE &&
        access != PAGE_EXECUTE_WRITECOPY)
        return HookResult::ProtectionFailed;
    std::vector<std::byte> signature(request.signature_bytes);
    if (!read(request.entry, signature.data(), signature.size()))
        return HookResult::Unreadable;
    if (HookSignature(signature) != request.signature)
        return HookResult::SignatureMismatch;
    const auto bytes =
        std::span(reinterpret_cast<const unsigned char *>(signature.data()), request.displaced);
    if (!relocatable(bytes))
        return HookResult::UnsafeInstructions;
    auto *code = static_cast<unsigned char *>(request.entry);
    auto *memory = allocate_near(uintptr_t(code));
    if (!memory)
        return HookResult::NoMemory;
    const int64_t delta = int64_t(uintptr_t(memory + 64)) - int64_t(uintptr_t(code + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return HookResult::NoMemory;
    }
    std::memcpy(memory, bytes.data(), bytes.size());
    jump(memory + bytes.size(), code + bytes.size());
    jump(memory + 64, replacement);
    DWORD protection{};
    if (!VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &protection)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return HookResult::ProtectionFailed;
    }
    if (!FlushInstructionCache(GetCurrentProcess(), memory, 78)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return HookResult::CacheFailed;
    }
    original.fill(0);
    patch.fill(0x90);
    std::memcpy(original.data(), bytes.data(), bytes.size());
    patch[0] = 0xe9;
    const int32_t relative = int32_t(delta);
    std::memcpy(patch.data() + 1, &relative, 4);
    if (!VirtualProtect(code, bytes.size(), PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return HookResult::ProtectionFailed;
    }
    std::memcpy(code, patch.data(), bytes.size());
    if (!FlushInstructionCache(GetCurrentProcess(), code, bytes.size())) {
        std::memcpy(code, original.data(), bytes.size());
        if (!FlushInstructionCache(GetCurrentProcess(), code, bytes.size()))
            fatal();
        restore_protection(code, bytes.size(), protection);
        VirtualFree(memory, 0, MEM_RELEASE);
        return HookResult::CacheFailed;
    }
    restore_protection(code, bytes.size(), protection);
    entry = code;
    relay = memory;
    length = bytes.size();
    return HookResult::Installed;
}
HookResult FunctionHook::Remove() {
    std::lock_guard lock{hook_mutex};
    if (!Installed())
        return HookResult::InvalidRange;
    std::array<unsigned char, 32> current{};
    if (!read(entry, current.data(), length))
        return HookResult::Unreadable;
    if (std::memcmp(current.data(), patch.data(), length))
        return HookResult::OwnershipLost;
    DWORD protection{};
    if (!VirtualProtect(entry, length, PAGE_EXECUTE_READWRITE, &protection))
        return HookResult::ProtectionFailed;
    std::memcpy(entry, original.data(), length);
    if (!FlushInstructionCache(GetCurrentProcess(), entry, length))
        fatal();
    restore_protection(entry, length, protection);
    VirtualFree(relay, 0, MEM_RELEASE);
    entry = relay = nullptr;
    length = 0;
    return HookResult::Removed;
}
} // namespace GzNative
