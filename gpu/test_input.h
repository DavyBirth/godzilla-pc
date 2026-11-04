// Optional host test controller. No dependency on SDL, GPU, or game structures.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <string>

struct GzTestPadSample {
    std::uint32_t buttons{};
    std::array<std::uint8_t, 4> axes{128, 128, 128, 128};
};

class GzTestPadLease {
  public:
    template <typename Accepted>
    bool Update(const char *path, std::uint64_t now, Accepted accepted) {
        std::ifstream input(path);
        input >> std::ws;
        if (!input || input.peek() == '-')
            return false;
        std::uint64_t next{};
        std::uint32_t buttons{}, x{}, y{}, z{}, w{}, milliseconds{};
        std::string extra;
        if (!(input >> next >> buttons >> x >> y >> z >> w >> milliseconds) || (input >> extra) ||
            next <= sequence || (buttons & ~0x10ffffu) || x > 255 || y > 255 || z > 255 ||
            w > 255 || milliseconds > 10250)
            return false;
        sequence = next;
        expires = now + milliseconds;
        current = {buttons, {std::uint8_t(x), std::uint8_t(y), std::uint8_t(z), std::uint8_t(w)}};
        accepted(next, buttons, x, y, z, w, milliseconds);
        return true;
    }

    GzTestPadSample Sample(std::uint64_t now) const {
        return now < expires ? current : GzTestPadSample{};
    }

  private:
    std::uint64_t sequence{}, expires{};
    GzTestPadSample current{};
};
