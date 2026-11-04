// Playback context belongs to the host; keyboard policy does not inspect guest addresses.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <mutex>
#include <unordered_set>
namespace GzPcPlayback {
inline std::mutex mutex;
inline std::unordered_set<const void *> owners;
inline std::atomic<unsigned> active_count{};
inline void SetActive(const void *owner, bool active) {
    if (!owner)
        return;
    std::lock_guard lock{mutex};
    if (active)
        owners.insert(owner);
    else
        owners.erase(owner);
    active_count.store(static_cast<unsigned>(owners.size()), std::memory_order_release);
}
inline bool Active() {
    return active_count.load(std::memory_order_acquire) != 0;
}
} // namespace GzPcPlayback
