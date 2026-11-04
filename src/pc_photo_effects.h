// Scene-independent photo lights and focus controls, in world-space units.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_camera.h"
#include <array>
#include <optional>
namespace GzPhoto {
enum class LightType { Point, Spot };
struct PhotoLight {
    bool enabled{};
    LightType type = LightType::Point;
    Vec3 color{1, 1, 1}, position{}, direction{0, 0, -1};
    float intensity = 1, inner_range = 0, outer_range = 100;
    float inner_cone = 30, outer_cone = 45; // Full cone angles, degrees.
};
inline bool Valid(const PhotoLight &l) {
    if (l.type != LightType::Point && l.type != LightType::Spot)
        return false;
    if (!Finite(l.color) || !Finite(l.position) || !Finite(l.direction))
        return false;
    if (l.color.x < 0 || l.color.x > 1 || l.color.y < 0 || l.color.y > 1 || l.color.z < 0 ||
        l.color.z > 1)
        return false;
    for (float v : {l.intensity, l.inner_range, l.outer_range, l.inner_cone, l.outer_cone})
        if (!std::isfinite(v))
            return false;
    auto direction = l.direction;
    return Unit(direction) && l.intensity >= 0 && l.intensity <= 10000 && l.inner_range >= 0 &&
           l.outer_range > l.inner_range && l.outer_range <= 1e7f && l.inner_cone > 0 &&
           l.inner_cone <= l.outer_cone && l.outer_cone < 179;
}
class LightRig {
    std::array<PhotoLight, 3> lights{};

  public:
    const std::array<PhotoLight, 3> &Lights() const {
        return lights;
    }
    bool Set(unsigned slot, PhotoLight value) {
        if (slot >= lights.size() || !Valid(value))
            return false;
        Unit(value.direction);
        lights[slot] = value;
        return true;
    }
    bool Move(unsigned slot, Vec3 delta) {
        if (slot >= lights.size() || !Finite(delta))
            return false;
        auto value = lights[slot];
        value.position = Add(value.position, delta);
        return Set(slot, value);
    }
    bool Aim(unsigned slot, float yaw_degrees, float pitch_degrees) {
        if (slot >= lights.size() || !std::isfinite(yaw_degrees) || !std::isfinite(pitch_degrees) ||
            std::abs(pitch_degrees) > 89)
            return false;
        auto value = lights[slot];
        const float yaw = std::remainder(yaw_degrees, 360.f) * .01745329252f,
                    pitch = pitch_degrees * .01745329252f;
        value.direction = {std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                           -std::cos(pitch) * std::cos(yaw)};
        return Set(slot, value);
    }
    void DisableAll() {
        for (auto &light : lights)
            light.enabled = false;
    }
};
// Geometric reference for native-adapter validation. This is not a screen-space
// replacement for the game's material lighting, shadowing or visibility tests.
inline float GeometricLightWeight(PhotoLight light, Vec3 position, Vec3 normal) {
    if (!light.enabled || !Valid(light) || !Finite(position) || !Unit(normal))
        return 0;
    auto to_light = Add(light.position, Scale(position, -1));
    const float distance = std::sqrt(Dot(to_light, to_light));
    if (!std::isfinite(distance) || distance >= light.outer_range || !Unit(to_light))
        return 0;
    const float diffuse = std::max(0.f, Dot(normal, to_light));
    float fade =
        1 - std::clamp((distance - light.inner_range) / (light.outer_range - light.inner_range),
                       0.f, 1.f);
    fade = fade * fade * (3 - 2 * fade);
    if (light.type == LightType::Spot) {
        Unit(light.direction);
        const float cosine = Dot(light.direction, Scale(to_light, -1));
        const float inner = std::cos(light.inner_cone * .00872664626f),
                    outer = std::cos(light.outer_cone * .00872664626f);
        const float cone = inner == outer
                               ? (cosine >= inner ? 1.f : 0.f)
                               : std::clamp((cosine - outer) / (inner - outer), 0.f, 1.f);
        fade *= cone * cone * (3 - 2 * cone);
    }
    return diffuse * fade * light.intensity;
}
enum class DofMode { Original, Manual, Disabled };
struct DepthOfField {
    DofMode mode = DofMode::Original;
    float distance = 100, range = 5, blur_range = 5; // Game-specific initialization, 01.01.
};
inline bool Valid(DepthOfField d) {
    return (d.mode == DofMode::Original || d.mode == DofMode::Manual ||
            d.mode == DofMode::Disabled) &&
           std::isfinite(d.distance) && std::isfinite(d.range) && std::isfinite(d.blur_range) &&
           d.distance > 0 && d.distance <= 1e7f && d.range >= 0 && d.range <= 1e7f &&
           d.blur_range > 0 && d.blur_range <= 1e7f;
}
// Explicit calibration is mandatory: the PS4 camera does not establish how
// many world units equal one metre. Do not invent a physical aperture scale.
inline std::optional<float> FocusDistance(Vec3 eye, Vec3 forward, Vec3 world_hit) {
    if (!Finite(eye) || !Finite(world_hit) || !Unit(forward))
        return {};
    const float depth = Dot(Add(world_hit, Scale(eye, -1)), forward);
    if (!std::isfinite(depth) || depth <= 0 || depth > 1e7f)
        return {};
    return depth;
}
} // namespace GzPhoto
