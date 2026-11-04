// Pinned Phyre layouts, owned staging and scoped native drawing overrides.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_effects.h"
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>
#include <utility>
namespace GzPhoto {
// These fields are copied by the original DOF material-parameter transfer.
// Staging is NOT evidence that a given live object is the main viewport effect.
// Game-specific renderer tests OWNER+768, independently of Phyre's base +49.
struct NativeDofStage {
    std::vector<std::byte> component;
    std::optional<uint32_t> owner_enabled;
};
inline std::optional<NativeDofStage> StageNativeDof(std::span<const std::byte> source,
                                                    uint64_t image_base, DepthOfField dof) {
    if (!Valid(dof) || source.size() < 0x60 || source.size() > 0x1000)
        return {};
    uint64_t table{};
    std::memcpy(&table, source.data(), 8);
    if (table < image_base || (table - image_base != 0x692870 && table - image_base != 0x6928d0 &&
                               table - image_base != 0x692990))
        return {};
    NativeDofStage staged{{source.begin(), source.end()}, {}};
    if (dof.mode == DofMode::Original)
        return staged;
    staged.owner_enabled = dof.mode == DofMode::Manual ? 1u : 0u;
    if (dof.mode == DofMode::Manual) {
        std::memcpy(staged.component.data() + 0x54, &dof.distance, 4);
        std::memcpy(staged.component.data() + 0x58, &dof.range, 4);
        std::memcpy(staged.component.data() + 0x5c, &dof.blur_range, 4);
    }
    return staged;
}
// Native drawing adapter: renderer and component must remain alive on their
// owner thread for the entire callback. Verification includes primary-view and
// scene identity, not just a class match. No hook is installed by this helper.
template <class Draw>
decltype(auto) DrawWithNativeDof(std::span<std::byte> owner, std::span<std::byte> component,
                                 uint64_t image_base, DepthOfField dof, bool verified,
                                 Draw &&draw) {
    if (!verified || dof.mode == DofMode::Original)
        return std::forward<Draw>(draw)();
    auto staged = StageNativeDof(component, image_base, dof);
    uint64_t pointer{};
    if (owner.size() >= 0xd18)
        std::memcpy(&pointer, owner.data() + 0xd10, 8);
    if (!staged || !staged->owner_enabled || owner.size() < 0xd18 ||
        pointer != reinterpret_cast<uintptr_t>(component.data()))
        return std::forward<Draw>(draw)();
    struct Restore {
        std::span<std::byte> owner, component;
        uint32_t before{}, written{};
        std::array<std::byte, 12> parameters{}, replacement{};
        bool manual{};
        ~Restore() {
            if (!std::memcmp(owner.data() + 0x768, &written, 4))
                std::memcpy(owner.data() + 0x768, &before, 4);
            if (manual)
                for (size_t at = 0; at < 12; at += 4)
                    if (!std::memcmp(component.data() + 0x54 + at, replacement.data() + at, 4))
                        std::memcpy(component.data() + 0x54 + at, parameters.data() + at, 4);
        }
    } restore{owner, component};
    restore.written = *staged->owner_enabled;
    restore.manual = dof.mode == DofMode::Manual;
    std::memcpy(&restore.before, owner.data() + 0x768, 4);
    std::memcpy(restore.parameters.data(), component.data() + 0x54, 12);
    std::memcpy(restore.replacement.data(), staged->component.data() + 0x54, 12);
    std::memcpy(owner.data() + 0x768, &restore.written, 4);
    if (restore.manual)
        std::memcpy(component.data() + 0x54, restore.replacement.data(), 12);
    return std::forward<Draw>(draw)();
}
// PLight's reflected data size is 0x40. Type and transform are native pointers,
// not inline values. Backend must own their lifetimes until jobs finish.
struct alignas(16) NativeLightRecord {
    float color[4]{};
    uint64_t world_transform{}, shadow_caster{}, type{};
    float inner_cone{}, outer_cone{}, intensity{}, inner_range{}, outer_range{}, scattering{};
};
static_assert(sizeof(NativeLightRecord) == 0x40 &&
              offsetof(NativeLightRecord, world_transform) == 0x10 &&
              offsetof(NativeLightRecord, type) == 0x20 &&
              offsetof(NativeLightRecord, inner_cone) == 0x28 &&
              offsetof(NativeLightRecord, intensity) == 0x30 &&
              offsetof(NativeLightRecord, outer_range) == 0x38);
// Native angle convention and transform semantics must be verified by the
// backend; this bridge accepts them explicitly instead of guessing a unit.
struct NativeLightBinding {
    uint64_t transform{}, type{};
    float inner_cone{}, outer_cone{};
    bool verified{};
};
inline std::optional<NativeLightRecord> StageNativeLight(PhotoLight light,
                                                         NativeLightBinding binding) {
    if (!Valid(light) || !binding.verified || !binding.transform || !binding.type ||
        !std::isfinite(binding.inner_cone) || !std::isfinite(binding.outer_cone))
        return {};
    NativeLightRecord output{};
    output.color[0] = light.color.x;
    output.color[1] = light.color.y;
    output.color[2] = light.color.z;
    output.color[3] = 1;
    output.world_transform = binding.transform;
    output.type = binding.type;
    output.inner_cone = binding.inner_cone;
    output.outer_cone = binding.outer_cone;
    output.intensity = light.enabled ? light.intensity : 0;
    output.inner_range = light.inner_range;
    output.outer_range = light.outer_range;
    return output;
}
class NativeLightEncoder {
  public:
    virtual ~NativeLightEncoder() = default;
    // Receives position/direction from the selected source. Native basis, enum
    // identities and cone convention belong to the backend, not light controls.
    virtual bool Encode(const PhotoLight &, std::span<float, 16> transform,
                        NativeLightBinding &) noexcept = 0;
};
// One per in-flight render slot. Stable addresses are important: native jobs
// can retain matrix pointers after collection. Do not update or destroy before
// that slot's jobs/fence finish. No pointers ever refer to the encoder's stack.
class NativeLightFrame {
    alignas(16) std::array<std::array<float, 16>, 3> transforms{};
    std::array<NativeLightRecord, 3> records{};
    std::array<unsigned, 3> source_slots{};
    unsigned count{};

  public:
    NativeLightFrame() = default;
    NativeLightFrame(const NativeLightFrame &) = delete;
    NativeLightFrame &operator=(const NativeLightFrame &) = delete;
    bool Prepare(const LightRig &rig, NativeLightEncoder &encoder) {
        alignas(16) decltype(transforms) next_transforms{};
        decltype(records) next_records{};
        decltype(source_slots) next_slots{};
        unsigned next_count{};
        for (unsigned slot = 0; slot < rig.Lights().size(); ++slot) {
            const auto &light = rig.Lights()[slot];
            if (!light.enabled)
                continue;
            NativeLightBinding binding;
            if (!encoder.Encode(light, next_transforms[next_count], binding))
                return false;
            for (float value : next_transforms[next_count])
                if (!std::isfinite(value))
                    return false;
            binding.transform = reinterpret_cast<uintptr_t>(transforms[next_count].data());
            auto record = StageNativeLight(light, binding);
            if (!record)
                return false;
            next_records[next_count] = *record;
            next_slots[next_count] = slot;
            ++next_count;
        }
        transforms = next_transforms;
        records = next_records;
        source_slots = next_slots;
        count = next_count;
        return true;
    }
    std::span<const NativeLightRecord> Records() const {
        return {records.data(), count};
    }
    unsigned SourceSlot(unsigned record) const {
        return record < count ? source_slots[record] : 3;
    }
};
} // namespace GzPhoto
