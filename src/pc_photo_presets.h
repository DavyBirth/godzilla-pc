// Versioned host settings; never persist world coordinates or guest pointers.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_grade.h"
#include "pc_photo_camera.h"
#include "pc_photo_channel.h"
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
namespace GzPhoto {
// Only light appearance is portable; position and direction stay in the scene.
struct PresetLight {
    bool enabled{};
    LightType type = LightType::Point;
    Vec3 color{1, 1, 1};
    float intensity = 1, inner_range = 0, outer_range = 100, inner_cone = 30, outer_cone = 45;
};
struct Preset {
    Grade grade;
    float fov = 40, speed = 40;
    int guide = 0;
    DepthOfField dof{};
    std::array<PresetLight, 3> lights{};
    bool hide_hud{};
};
inline void ApplyLight(const PresetLight &source, PhotoLight &target) {
    target.enabled = source.enabled;
    target.type = source.type;
    target.color = source.color;
    target.intensity = source.intensity;
    target.inner_range = source.inner_range;
    target.outer_range = source.outer_range;
    target.inner_cone = source.inner_cone;
    target.outer_cone = source.outer_cone;
}
inline bool Valid(const PresetLight &p) {
    PhotoLight light;
    ApplyLight(p, light);
    return Valid(light);
}
inline bool Valid(const Preset &p) {
    return Valid(p.grade) && ValidPhotoFov(p.fov) && std::isfinite(p.speed) && p.speed >= 1 &&
           p.speed <= 1e6f && p.guide >= 0 && p.guide <= 3 && Valid(p.dof) &&
           std::all_of(p.lights.begin(), p.lights.end(),
                       [](const auto &light) { return Valid(light); });
}
inline Preset CapturePreset(const PhotoSettings &settings, float fov) {
    Preset p;
    p.grade = settings.grade;
    p.fov = fov;
    p.speed = settings.speed;
    p.guide = int(settings.guide);
    p.dof = settings.dof;
    p.hide_hud = settings.hide_hud;
    for (unsigned i = 0; i < 3; ++i) {
        const auto &l = settings.lights[i];
        p.lights[i] = {l.enabled,     l.type,        l.color,      l.intensity,
                       l.inner_range, l.outer_range, l.inner_cone, l.outer_cone};
    }
    return p;
}
inline PhotoSettings ApplyPresetSettings(const Preset &p, PhotoSettings settings,
                                         PhotoCapabilities capabilities) {
    settings.grade = p.grade;
    settings.guide = Guide(p.guide);
    settings.speed = p.speed;
    settings.dof = p.dof;
    settings.hide_hud = p.hide_hud && capabilities.hud;
    if (!capabilities.dof)
        settings.dof.mode = DofMode::Original;
    for (unsigned i = 0; i < 3; ++i) {
        ApplyLight(p.lights[i], settings.lights[i]);
        if (!capabilities.lights)
            settings.lights[i].enabled = false;
        if (!capabilities.spot_lights)
            settings.lights[i].type = LightType::Point;
    }
    return settings;
}
inline std::optional<std::string> EncodePreset(const Preset &p) {
    if (!Valid(p))
        return {};
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    const auto &g = p.grade;
    stream << std::setprecision(std::numeric_limits<float>::max_digits10) << "GZPHOTO 2\n"
           << g.exposure << ' ' << g.contrast << ' ' << g.saturation << ' ' << g.filter << ' '
           << g.grain << ' ' << g.vignette << ' ' << g.frame << ' ' << g.strength << '\n'
           << p.fov << ' ' << p.speed << ' ' << p.guide << '\n';
    stream << int(p.dof.mode) << ' ' << p.dof.distance << ' ' << p.dof.range << ' '
           << p.dof.blur_range << ' ' << int(p.hide_hud) << '\n';
    for (const auto &l : p.lights)
        stream << int(l.enabled) << ' ' << int(l.type) << ' ' << l.color.x << ' ' << l.color.y
               << ' ' << l.color.z << ' ' << l.intensity << ' ' << l.inner_range << ' '
               << l.outer_range << ' ' << l.inner_cone << ' ' << l.outer_cone << '\n';
    return stream.str();
}
inline std::optional<Preset> DecodePreset(std::string_view text) {
    if (text.size() > 4096)
        return {};
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    std::string magic;
    int version{};
    Preset p;
    auto &g = p.grade;
    if (!(stream >> magic >> version) || magic != "GZPHOTO" || (version != 1 && version != 2))
        return {};
    if (!(stream >> g.exposure >> g.contrast >> g.saturation >> g.filter >> g.grain >> g.vignette >>
          g.frame >> g.strength >> p.fov >> p.speed >> p.guide))
        return {};
    if (version == 2) {
        int mode{}, hud{};
        if (!(stream >> mode >> p.dof.distance >> p.dof.range >> p.dof.blur_range >> hud) ||
            mode < 0 || mode > 2 || (hud != 0 && hud != 1))
            return {};
        p.dof.mode = DofMode(mode);
        p.hide_hud = hud != 0;
        for (auto &l : p.lights) {
            int enabled{}, type{};
            if (!(stream >> enabled >> type >> l.color.x >> l.color.y >> l.color.z >> l.intensity >>
                  l.inner_range >> l.outer_range >> l.inner_cone >> l.outer_cone) ||
                (enabled != 0 && enabled != 1) || type < 0 || type > 1)
                return {};
            l.enabled = enabled != 0;
            l.type = LightType(type);
        }
    }
    if (!Valid(p))
        return {};
    stream >> std::ws;
    if (!stream.eof())
        return {};
    return p;
}
// Eight fixed slots, under userdata/photos/presets. Atomic replacement; a failed
// save preserves the existing slot. Missing/invalid files return no settings.
bool SavePreset(unsigned slot, const Preset &);
std::optional<Preset> LoadPreset(unsigned slot);
} // namespace GzPhoto
