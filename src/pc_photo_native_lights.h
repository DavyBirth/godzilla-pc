// Convert owned PLight values through the native collector, into a private
// renderer snapshot. The original scene's light array is never modified.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_native_effects.h"
namespace GzPhoto {
struct NativeLightIterator {
    void *current{};
    void *end{};
    uint32_t stride{}, remaining{}, member{}, padding{};
    void *root{};
    void *next{};
    void *type{};
};
static_assert(sizeof(NativeLightIterator) == 0x38 &&
              offsetof(NativeLightIterator, remaining) == 0x14 &&
              offsetof(NativeLightIterator, member) == 0x18);
struct NativeLightApi {
    using PackMatrix = void(__attribute__((sysv_abi)) *)(float *, const float *);
    using Collect = void(__attribute__((sysv_abi)) *)(void *, NativeLightIterator *);
    PackMatrix pack{};
    Collect collect{};
    uint64_t point{}, spot{};
    bool verified{};
};
class PhyreLightEncoder final : public NativeLightEncoder {
    NativeLightApi api;

  public:
    explicit PhyreLightEncoder(NativeLightApi value) : api(value) {}
    bool Encode(const PhotoLight &light, std::span<float, 16> output,
                NativeLightBinding &binding) noexcept override {
        if (!api.verified || !api.pack || !api.point || !api.spot || !Valid(light))
            return false;
        auto forward = light.direction;
        Unit(forward);
        // The native spotlight transform stores the axis pointing back toward
        // the source. PhotoLight.direction is the outgoing direction of light.
        if (light.type == LightType::Spot)
            forward = Scale(forward, -1);
        Vec3 reference = std::abs(forward.y) > .99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        auto right = Cross(reference, forward);
        if (!Unit(right))
            return false;
        auto up = Cross(forward, right);
        if (!Unit(up))
            return false;
        alignas(16) const float matrix[16] = {right.x,
                                              right.y,
                                              right.z,
                                              0,
                                              up.x,
                                              up.y,
                                              up.z,
                                              0,
                                              forward.x,
                                              forward.y,
                                              forward.z,
                                              0,
                                              light.position.x,
                                              light.position.y,
                                              light.position.z,
                                              1};
        api.pack(output.data(), matrix); // PMatrix4 -> PMatrix4x3, 0x49d7b0.
        const float outer = light.outer_cone * .00872664626f;
        const float inner = std::min(light.inner_cone * .00872664626f, outer - 1e-5f);
        binding = {0, light.type == LightType::Point ? api.point : api.spot, std::max(0.f, inner),
                   outer, true};
        return true;
    }
};
enum class NativeLightStatus {
    Prepared,
    NoLights,
    Unsupported,
    InvalidSource,
    Capacity,
    ConversionFailed
};
class NativeLightPass {
  public:
    static constexpr size_t RendererSize = 0x4340, InputStart = 0x60, InputStride = 0x80,
                            InputCount = 0x4120;

  private:
    NativeLightFrame frame;
    alignas(16) std::array<std::byte, RendererSize> converted{}, snapshot{};
    bool prepared{};

  public:
    NativeLightStatus Prepare(std::span<const std::byte> source,
                              const std::array<PhotoLight, 3> &lights, NativeLightApi api) {
        prepared = false;
        if (!api.verified || !api.collect)
            return NativeLightStatus::Unsupported;
        if (source.size() != RendererSize)
            return NativeLightStatus::InvalidSource;
        LightRig rig;
        for (unsigned i = 0; i < lights.size(); ++i) {
            auto light = lights[i];
            light.enabled = light.enabled && light.intensity > 0;
            if (!rig.Set(i, light))
                return NativeLightStatus::InvalidSource;
        }
        PhyreLightEncoder encoder{api};
        if (!frame.Prepare(rig, encoder))
            return NativeLightStatus::ConversionFailed;
        const auto records = frame.Records();
        if (records.empty())
            return NativeLightStatus::NoLights;
        uint32_t count{};
        std::memcpy(&count, source.data() + InputCount, 4);
        // Native upload consumes at most 64 point/spot entries. Never evict
        // original lights or advertise additional sources that cannot be drawn.
        if (count > 64 || records.size() > 64 - count)
            return NativeLightStatus::Capacity;
        converted.fill(std::byte{});
        NativeLightIterator iterator{const_cast<NativeLightRecord *>(records.data()), nullptr,
                                     uint32_t(sizeof(NativeLightRecord)), uint32_t(records.size())};
        api.collect(converted.data(), &iterator);
        uint32_t produced{};
        std::memcpy(&produced, converted.data() + InputCount, 4);
        if (produced != records.size())
            return NativeLightStatus::ConversionFailed;
        for (unsigned i = 0; i < produced; ++i) {
            uint32_t type{};
            uint64_t source_record{};
            const auto *entry = converted.data() + InputStart + i * InputStride;
            std::memcpy(&type, entry + 0x50, 4);
            std::memcpy(&source_record, entry + 0x78, 8);
            if ((type != 0 && type != 2) || source_record != uintptr_t(records.data() + i))
                return NativeLightStatus::ConversionFailed;
        }
        std::memcpy(snapshot.data(), source.data(), source.size());
        std::memcpy(snapshot.data() + InputStart + count * InputStride,
                    converted.data() + InputStart, produced * InputStride);
        count += produced;
        std::memcpy(snapshot.data() + InputCount, &count, 4);
        prepared = true;
        return NativeLightStatus::Prepared;
    }
    std::span<std::byte> Snapshot() {
        return prepared ? std::span<std::byte>(snapshot) : std::span<std::byte>{};
    }
};
} // namespace GzPhoto
