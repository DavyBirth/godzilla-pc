// Preview-only composition guides. They are never part of GradePixels/PNG.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_grade.h"
#include <array>
namespace GzPhoto {
enum class Guide { None, Thirds, Golden, Center };
struct GuideLine {
    float x1{}, y1{}, x2{}, y2{};
};
struct GuideGeometry {
    std::array<GuideLine, 4> lines{};
    unsigned count{};
};
inline GuideGeometry CompositionGuides(unsigned width, unsigned height, int frame, Guide guide) {
    GuideGeometry result;
    if (!width || !height || frame < 0 || frame > 5)
        return result;
    const auto r = VisibleFrame(width, height, frame);
    if (guide == Guide::Thirds || guide == Guide::Golden) {
        const float offset = guide == Guide::Thirds ? 1.f / 3.f : .38196601125f;
        for (float t : {offset, 1 - offset}) {
            result.lines[result.count++] = {r.x + t * r.w, r.y, r.x + t * r.w, r.y + r.h};
            result.lines[result.count++] = {r.x, r.y + t * r.h, r.x + r.w, r.y + t * r.h};
        }
    } else if (guide == Guide::Center) {
        const float x = r.x + r.w * .5f, y = r.y + r.h * .5f;
        const float dx = std::min(8.f / width, r.w * .5f), dy = std::min(8.f / height, r.h * .5f);
        result.lines[0] = {x - dx, y, x + dx, y};
        result.lines[1] = {x, y - dy, x, y + dy};
        result.count = 2;
    }
    return result;
}
} // namespace GzPhoto
