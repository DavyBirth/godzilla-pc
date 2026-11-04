// Presentation intervals, separate from guest simulation and GPU execution time.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>
namespace GzFrameTiming {
struct Summary {
    unsigned count{}, excluded{};
    double fps{}, mean_ms{}, p99_ms{}, maximum_ms{};
};
class Window {
    std::array<double, 4096> samples{};
    size_t count{};
    unsigned excluded{};
    double previous{};
    bool have_previous{};

  public:
    void Break() {
        have_previous = false;
        ++excluded;
    }
    void Add(double milliseconds) {
        if (!std::isfinite(milliseconds)) {
            Break();
            return;
        }
        if (have_previous && milliseconds > previous && count < samples.size())
            samples[count++] = milliseconds - previous;
        previous = milliseconds;
        have_previous = true;
    }
    Summary Publish() {
        Summary result{};
        result.count = static_cast<unsigned>(count);
        result.excluded = excluded;
        if (count) {
            const double total = std::accumulate(samples.begin(), samples.begin() + count, 0.0);
            std::sort(samples.begin(), samples.begin() + count);
            result.mean_ms = total / count;
            result.fps = 1000 / result.mean_ms;
            result.p99_ms = samples[size_t(std::ceil(count * .99)) - 1];
            result.maximum_ms = samples[count - 1];
        }
        count = 0;
        excluded = 0;
        return result;
    }
};
} // namespace GzFrameTiming
