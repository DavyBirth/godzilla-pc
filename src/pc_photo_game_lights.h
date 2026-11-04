// Borrow the game's dynamic light pool. Never evict or edit an existing light.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_native_lights.h"
namespace GzPhoto {
struct GameLightPoolView {
    void *manager{};
    std::span<void *const> active;
    size_t free_count{};
    bool Contains(void *object) const {
        return std::find(active.begin(), active.end(), object) != active.end();
    }
};
class GameLightLease {
    struct Slot {
        void *object{};
        std::array<std::byte, 0x40> record{};
        std::array<std::byte, 0x30> matrix{};
        uint8_t enabled{};
        bool remembered{};
    };
    std::array<Slot, 3> slots;
    void *manager{};
    NativeLightFrame staged;
    static bool SelfMatrix(std::span<std::byte> bytes) {
        uintptr_t pointer{};
        if (bytes.size() < 0x84)
            return false;
        std::memcpy(&pointer, bytes.data() + 0x10, 8);
        return pointer == uintptr_t(bytes.data() + 0x50) && bytes[0x41] == std::byte{1};
    }
    static bool Matches(const Slot &slot, std::span<std::byte> bytes) {
        return SelfMatrix(bytes) &&
               (!slot.remembered || (!std::memcmp(bytes.data(), slot.record.data(), 0x40) &&
                                     !std::memcmp(bytes.data() + 0x50, slot.matrix.data(), 0x30) &&
                                     bytes[0x40] == std::byte(slot.enabled)));
    }

  public:
    bool Owned() const {
        return manager != nullptr;
    }
    template <class Resolve> void Release(GameLightPoolView pool, Resolve resolve) {
        if (manager == pool.manager)
            for (auto &slot : slots)
                if (slot.object && pool.Contains(slot.object)) {
                    auto bytes = resolve(slot.object);
                    if (Matches(slot, bytes))
                        bytes[0x41] = std::byte{};
                }
        manager = nullptr;
        slots = {};
    }
    template <class Resolve> void Remember(GameLightPoolView pool, Resolve resolve) {
        if (manager != pool.manager)
            return;
        for (auto &slot : slots)
            if (slot.object && pool.Contains(slot.object)) {
                auto bytes = resolve(slot.object);
                if (!SelfMatrix(bytes))
                    continue;
                std::memcpy(slot.record.data(), bytes.data(), 0x40);
                std::memcpy(slot.matrix.data(), bytes.data() + 0x50, 0x30);
                slot.enabled = uint8_t(bytes[0x40]);
                slot.remembered = true;
            }
    }
    template <class Allocate, class Resolve>
    bool Apply(GameLightPoolView pool, const std::array<PhotoLight, 3> &lights, NativeLightApi api,
               Allocate allocate, Resolve resolve) {
        const bool any = std::any_of(lights.begin(), lights.end(), [](const PhotoLight &light) {
            return light.enabled && light.intensity > 0;
        });
        if (!pool.manager || pool.active.size() > 128 || pool.free_count > 128)
            return false;
        if (!Owned() && !any)
            return true;
        bool fresh = false;
        if (!Owned()) {
            if (pool.free_count < 3)
                return false;
            manager = pool.manager;
            fresh = true;
            for (auto &slot : slots) {
                slot.object = allocate(manager);
                if (!slot.object || !SelfMatrix(resolve(slot.object))) {
                    // These objects were just allocated by us and not published
                    // to a draw yet. Native update will recycle them immediately.
                    for (auto &partial : slots)
                        if (partial.object) {
                            auto bytes = resolve(partial.object);
                            if (SelfMatrix(bytes))
                                bytes[0x41] = std::byte{};
                        }
                    manager = nullptr;
                    slots = {};
                    return false;
                }
            }
        }
        if (manager != pool.manager)
            return false;
        for (const auto &slot : slots)
            if ((!fresh && !pool.Contains(slot.object)) || !Matches(slot, resolve(slot.object)))
                return false;
        LightRig rig;
        for (unsigned i = 0; i < lights.size(); ++i) {
            auto light = lights[i];
            light.enabled = light.enabled && light.intensity > 0;
            if (!rig.Set(i, light))
                return false;
        }
        PhyreLightEncoder encoder{api};
        if (!staged.Prepare(rig, encoder))
            return false;
        for (auto &slot : slots)
            resolve(slot.object)[0x40] = std::byte{};
        for (unsigned i = 0; i < staged.Records().size(); ++i) {
            auto &slot = slots[staged.SourceSlot(i)];
            auto bytes = resolve(slot.object);
            auto record = staged.Records()[i];
            const auto *matrix = reinterpret_cast<const void *>(record.world_transform);
            record.world_transform = uintptr_t(bytes.data() + 0x50);
            std::memcpy(bytes.data(), &record, 0x40);
            std::memcpy(bytes.data() + 0x50, matrix, 0x30);
            bytes[0x40] = std::byte{1};
            slot.remembered = false;
        }
        return true;
    }
};
} // namespace GzPhoto
