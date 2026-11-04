// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"
#include <vector>
#include "pc_photo_channel.h"
union SDL_Event;
namespace Vulkan {
class Instance;
}
namespace GzPcOverlay {
void Initialize(const Vulkan::Instance &, vk::Format, unsigned);
void Shutdown();
bool Event(const SDL_Event &);
bool Render(vk::CommandBuffer, vk::ImageView, vk::Extent2D, const GzPhoto::PhotoFrame &,
            unsigned source_width, unsigned source_height);
void RequestCapture();
bool TakeCapture();
void SaveCapture(std::vector<unsigned char> pixels, unsigned width, unsigned height,
                 GzPhoto::Grade grade = {});
bool MouseUi();
void TogglePhotoPanel();
void PhotoGamepad(unsigned buttons, bool connected);
bool KeyboardUi();
void PixelDensity(float density);
void FinishSaves();
} // namespace GzPcOverlay
