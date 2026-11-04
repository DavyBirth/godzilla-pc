// GPU resources live until presenter shutdown, after all slot fences complete.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_post.h"
#include "pc_photo_present_policy.h"
namespace GzPhotoGpu {
void TrackSource(unsigned slot, vk::Format);
vk::Format SourceFormat(unsigned slot);
GzPhoto::PhotoEncoding SourceEncoding(unsigned slot);
GzPhoto::PhotoEncoding Encoding(vk::Format);
bool Draw(const Vulkan::Instance &, vk::CommandBuffer, unsigned slot, unsigned slots,
          vk::ImageView source, unsigned width, unsigned height, vk::ImageView target, vk::Extent2D,
          vk::Format, const GzPhoto::PhotoPlan &);
void Shutdown();
} // namespace GzPhotoGpu
