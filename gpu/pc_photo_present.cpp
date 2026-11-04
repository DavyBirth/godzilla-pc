// Lazy photo pipelines, bounded by the four supported 8-bit surface formats.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_photo_present.h"
#include <array>
#include <atomic>
#include <memory>
#include <stdexcept>
namespace GzPhotoGpu {
namespace {
std::array<std::atomic<int>, 32> source_formats{};
struct Context {
    vk::Format format{};
    unsigned slots{};
    std::unique_ptr<GzPhoto::PostProcessor> pass;
};
std::array<Context, 4> contexts;
} // namespace
GzPhoto::PhotoEncoding Encoding(vk::Format format) {
    using GzPhoto::PhotoEncoding;
    if (format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eB8G8R8A8Srgb)
        return PhotoEncoding::Srgb8;
    if (format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eB8G8R8A8Unorm)
        return PhotoEncoding::Unorm8;
    return PhotoEncoding::Unsupported;
}
void TrackSource(unsigned slot, vk::Format format) {
    if (slot < source_formats.size())
        source_formats[slot].store(int(format), std::memory_order_release);
}
vk::Format SourceFormat(unsigned slot) {
    return slot < source_formats.size()
               ? vk::Format(source_formats[slot].load(std::memory_order_acquire))
               : vk::Format::eUndefined;
}
GzPhoto::PhotoEncoding SourceEncoding(unsigned slot) {
    return Encoding(SourceFormat(slot));
}
bool Draw(const Vulkan::Instance &instance, vk::CommandBuffer cmd, unsigned slot, unsigned slots,
          vk::ImageView source, unsigned width, unsigned height, vk::ImageView target,
          vk::Extent2D extent, vk::Format format, const GzPhoto::PhotoPlan &plan) {
    if (!plan.draw_grade)
        return false;
    if (!slots || slots > source_formats.size() || slot >= slots ||
        SourceEncoding(slot) != Encoding(format) ||
        Encoding(format) == GzPhoto::PhotoEncoding::Unsupported)
        return false;
    Context *context{};
    for (auto &candidate : contexts)
        if (candidate.pass && candidate.format == format && candidate.slots == slots) {
            context = &candidate;
            break;
        }
    if (!context)
        for (auto &candidate : contexts)
            if (!candidate.pass) {
                context = &candidate;
                break;
            }
    if (!context)
        throw std::runtime_error(
            "Photo pipeline format/slot lifecycle changed; GPU-idle reset required");
    if (!context->pass) {
        auto pass = std::make_unique<GzPhoto::PostProcessor>(instance, format, slots);
        context->format = format;
        context->slots = slots;
        context->pass = std::move(pass);
    }
    return context->pass->Render(cmd, slot, source, width, height, target, extent, plan.grade);
}
void Shutdown() {
    // Presenter completed all scheduler work before this call. Never wait here
    // while holding a submit mutex or destroy a live format on resize.
    for (auto &context : contexts)
        context = {};
    for (auto &format : source_formats)
        format.store(0, std::memory_order_release);
}
} // namespace GzPhotoGpu
