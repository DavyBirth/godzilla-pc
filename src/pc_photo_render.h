// CPU/shader ABI. No Vulkan dependency and no original game state.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_grade.h"
#include <type_traits>
namespace GzPhoto {
struct GradePush {
    Grade grade;
    int encoded{};
    unsigned width{}, height{};
};
static_assert(std::is_standard_layout_v<Grade> && sizeof(Grade) == 32);
static_assert(offsetof(Grade, strength) == 28 && offsetof(Grade, frame) == 24);
static_assert(sizeof(GradePush) == 44 && offsetof(GradePush, encoded) == 32 &&
              offsetof(GradePush, height) == 40);
struct PhotoViewport {
    unsigned x{}, y{}, width{}, height{};
};
inline PhotoViewport FitPhotoViewport(unsigned width, unsigned height, unsigned target_width,
                                      unsigned target_height) {
    if (!width || !height || !target_width || !target_height)
        return {};
    const float aspect = float(width) / height, target_aspect = float(target_width) / target_height;
    unsigned w = target_width, h = target_height;
    if (aspect > target_aspect)
        h = unsigned(target_width / aspect);
    else
        w = unsigned(target_height * aspect);
    return {(target_width - w) / 2, (target_height - h) / 2, w, h};
}
} // namespace GzPhoto
