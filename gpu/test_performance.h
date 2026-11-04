// Optional host presentation timing. No GPU waits or game-memory access.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "test_capture.h"
#include "frame_stats.h"
#include <algorithm>
#include <cmath>
#include <numeric>

struct GzFrameTimings {
    std::vector<double> intervals;
    double last_ms{};
    bool has_last{};
    unsigned long long sequence{}, excluded{};
    void Break() {
        has_last = false;
        ++excluded;
    }
    void Add(double now_ms) {
        if (has_last && now_ms > last_ms && intervals.size() < 4096)
            intervals.push_back(now_ms - last_ms);
        last_ms = now_ms;
        has_last = true;
    }
    void Write(std::ostream &out) {
        auto sorted = intervals;
        std::sort(sorted.begin(), sorted.end());
        const double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
        const double p99 = sorted.empty() ? 0 : sorted[size_t(std::ceil(sorted.size() * .99)) - 1];
        out << "{\"sequence\":" << ++sequence << ",\"interval_count\":" << sorted.size()
            << ",\"elapsed_ms\":" << sum
            << ",\"average_fps\":" << (sum ? 1000 * sorted.size() / sum : 0)
            << ",\"p99_ms\":" << p99 << ",\"maximum_ms\":" << (sorted.empty() ? 0 : sorted.back())
            << ",\"excluded_calls\":" << excluded << ",\"intervals_ms\":[";
        for (size_t i = 0; i < intervals.size(); ++i)
            out << (i ? "," : "") << intervals[i];
        out << "],\"scope\":\"host present-call completion intervals; not GPU scanout or "
               "simulation FPS\"}\n";
        intervals.clear();
    }
};

inline void gz_test_observe_present(bool presented, bool reused = false) {
    GzFrameStats::Observe(presented, reused);
    static const char *directory = std::getenv("GZ_TEST_CAPTURE_DIR");
    if (!directory || !*directory)
        return;
    static GzFrameTimings timings;
    if (!presented || reused) {
        timings.Break();
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    static auto last_publish = now;
    timings.Add(std::chrono::duration<double, std::milli>(now.time_since_epoch()).count());
    if (now - last_publish < std::chrono::seconds(1))
        return;
    last_publish = now;
    const auto root = std::filesystem::u8path(directory);
    std::ofstream output(root / "performance.tmp");
    timings.Write(output);
    output.close();
    if (output)
        gz_test_publish(root / "performance.tmp", root / "performance.json");
}
