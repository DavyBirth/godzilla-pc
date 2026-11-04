// Per-frame policy shared with capture; independent of Vulkan calls.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_channel.h"
namespace GzPhoto {
enum class PhotoEncoding { Unsupported, Unorm8, Srgb8 };
struct PhotoPlan {
    Grade grade{};
    Guide guide = Guide::None;
    bool draw_grade{}, can_capture{}, suppressed{};
};
inline PhotoPlan PlanPhoto(const PhotoFrame &frame, PhotoEncoding source, PhotoEncoding target) {
    PhotoPlan plan;
    plan.can_capture = source != PhotoEncoding::Unsupported;
    if (!frame.active || !Valid(frame.settings))
        return plan;
    plan.guide = frame.settings.guide;
    if (!plan.can_capture || source != target) {
        plan.suppressed = !Neutral(frame.settings.grade);
        return plan;
    }
    plan.grade = frame.settings.grade;
    plan.draw_grade = Valid(plan.grade) && !Neutral(plan.grade);
    return plan;
}
} // namespace GzPhoto
