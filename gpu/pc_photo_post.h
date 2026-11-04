// Host-only photo preview, adapted from Kyoei's own postprocessing.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"
#include "pc_photo_render.h"
#include <memory>
namespace Vulkan {
class Instance;
}
namespace GzPhoto {
// Create lazily when a verified photo session requests non-neutral grading.
// Destroy/recreate after GPU completion on surface-format or slot-count change.
class PostProcessor {
    struct State;
    std::unique_ptr<State> state;

  public:
    PostProcessor(const Vulkan::Instance &, vk::Format, unsigned slots);
    ~PostProcessor();
    PostProcessor(const PostProcessor &) = delete;
    PostProcessor &operator=(const PostProcessor &) = delete;
    // Caller completed this slot's presentation fence before descriptor updates.
    // Target uses the construction format. Source must have matching sRGB
    // encoding; RGBA/BGRA channel ordering is handled by Vulkan image views.
    // Source: sampled image in GENERAL, visible to fragment reads; target:
    // COLOR_ATTACHMENT_OPTIMAL, visible to attachment reads/writes. Caller owns
    // transitions before/after; no source image is modified and UI draws later.
    bool Render(vk::CommandBuffer, unsigned slot, vk::ImageView source, unsigned width,
                unsigned height, vk::ImageView target, vk::Extent2D extent, Grade);
};
} // namespace GzPhoto
