// Optional native effect resources, owned only when both original slots were empty.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
namespace GzPhoto {
class DofResources {
    std::span<std::byte> owner;
    uintptr_t resource{}, component{};
    static uintptr_t Pointer(std::span<std::byte> bytes, size_t offset) {
        uintptr_t value{};
        if (bytes.size() >= offset + sizeof(value))
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

  public:
    bool Owned() const {
        return !owner.empty();
    }
    template <class Initialize, class Destroy>
    bool Acquire(std::span<std::byte> bytes, Initialize initialize, Destroy destroy) {
        if (Owned() || bytes.size() < 0xd18 || Pointer(bytes, 0xcf0) || Pointer(bytes, 0xd10) ||
            !Pointer(bytes, 0xcf8))
            return false;
        initialize(bytes.data());
        resource = Pointer(bytes, 0xcf0);
        component = Pointer(bytes, 0xd10);
        if (!resource || !component) {
            if (resource || component)
                destroy(bytes.data());
            resource = component = 0;
            return false;
        }
        owner = bytes;
        return true;
    }
    template <class Destroy> bool Release(Destroy destroy) {
        if (!Owned())
            return false;
        auto bytes = owner;
        owner = {};
        // A scene transition or native camera can replace/free the effect.
        // Never destroy that replacement or restore a stale pointer over it.
        const bool ours = Pointer(bytes, 0xcf0) == resource && Pointer(bytes, 0xd10) == component;
        resource = component = 0;
        if (ours)
            destroy(bytes.data());
        return ours;
    }
};
} // namespace GzPhoto
