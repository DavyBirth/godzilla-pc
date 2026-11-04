// Optional manual-play measurements. Disabled path performs one atomic load.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../src/frame_timing.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <filesystem>
#include <fstream>
extern "C" const char *gz_user_dir(void);
namespace GzFrameStats {
inline bool Record() {
    static const bool value = [] {
        const char *option = std::getenv("GZ_FRAME_STATS");
        return option && option[0] == '1';
    }();
    return value;
}
inline std::atomic<bool> visible{false};
inline std::mutex mutex;
inline GzFrameTiming::Summary published;
inline GzFrameTiming::Summary Snapshot() {
    std::lock_guard lock{mutex};
    return published;
}
inline void Observe(bool presented, bool reused) {
    static bool enabled{};
    static GzFrameTiming::Window window;
    if (!visible.load(std::memory_order_relaxed) && !Record()) {
        if (enabled) {
            window = {};
            enabled = false;
            std::lock_guard lock{mutex};
            published = {};
        }
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    static auto last_publish = now;
    if (!enabled) {
        window = {};
        enabled = true;
        last_publish = now;
    }
    if (!presented || reused)
        window.Break();
    else
        window.Add(std::chrono::duration<double, std::milli>(now.time_since_epoch()).count());
    if (now - last_publish < std::chrono::seconds(1))
        return;
    last_publish = now;
    const auto summary = window.Publish();
    {
        std::lock_guard lock{mutex};
        published = summary;
    }
    if (Record()) {
        // Stream opened once; never opens a screenshot/readback/GPU wait path.
        static std::ofstream output = [] {
            try {
                const auto folder = std::filesystem::u8path(gz_user_dir()) / "logs";
                std::filesystem::create_directories(folder);
                return std::ofstream(
                    folder / ("performance-" +
                              std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 Clock::now().time_since_epoch())
                                                 .count()) +
                              ".jsonl"));
            } catch (...) {
                return std::ofstream{};
            }
        }();
        output << "{\"interval_count\":" << summary.count << ",\"average_fps\":" << summary.fps
               << ",\"mean_ms\":" << summary.mean_ms << ",\"p99_ms\":" << summary.p99_ms
               << ",\"maximum_ms\":" << summary.maximum_ms << ",\"excluded\":" << summary.excluded
               << ",\"scope\":\"host presentation intervals; not simulation FPS or GPU execution "
                  "time\"}\n";
        output.flush();
    }
}
} // namespace GzFrameStats
