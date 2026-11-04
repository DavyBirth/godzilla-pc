// Verified native function replacement, shared by scene/effect adapters.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace GzNative {
enum class HookResult {
    Installed,
    Removed,
    InvalidRange,
    SignatureMismatch,
    UnsafeInstructions,
    Unreadable,
    NoMemory,
    ProtectionFailed,
    CacheFailed,
    AlreadyInstalled,
    OwnershipLost
};
uint64_t HookSignature(std::span<const std::byte>);
struct HookRequest {
    void *entry{};
    size_t available{}, signature_bytes{}, displaced{};
    uint64_t signature{};
};
// Install/remove only while no guest thread can execute the target or relay.
// There is intentionally no destructor unpatch: a process-lifetime hook must
// never release executable memory while a native job may still return to it.
class FunctionHook {
    unsigned char *entry{};
    unsigned char *relay{};
    size_t length{};
    std::array<unsigned char, 32> original{}, patch{};

  public:
    FunctionHook() = default;
    FunctionHook(const FunctionHook &) = delete;
    FunctionHook &operator=(const FunctionHook &) = delete;
    HookResult Install(HookRequest, void *replacement);
    HookResult Remove();
    bool Installed() const {
        return relay != nullptr;
    }
    template <class Function> Function Original() const {
        return reinterpret_cast<Function>(relay);
    }
};
} // namespace GzNative
