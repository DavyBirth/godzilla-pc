// Do not deliver a held photo-mode button/stick into the resumed game.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace GzPhoto {
class PhotoReleaseGate {
    bool release{};

  public:
    bool Block(bool active, uint32_t buttons, bool axes_neutral) {
        if (active) {
            release = true;
            return true;
        }
        if (release) {
            if (!buttons && axes_neutral)
                release = false;
            return true;
        }
        return false;
    }
};
} // namespace GzPhoto
