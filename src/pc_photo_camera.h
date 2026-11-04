// Guest-independent photographic camera math. Native adapter remains separate.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
namespace GzPhoto {
inline constexpr float MinPhotoFov = 1.f, MaxPhotoFov = 170.f;
inline bool ValidPhotoFov(float fov) {
    return std::isfinite(fov) && fov >= MinPhotoFov && fov <= MaxPhotoFov;
}
struct Vec3 {
    float x{}, y{}, z{};
};
inline Vec3 Add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 Scale(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool Finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline bool Unit(Vec3 &v) {
    const float d = Dot(v, v);
    if (!std::isfinite(d) || d < 1e-10f)
        return false;
    v = Scale(v, 1 / std::sqrt(d));
    return true;
}
inline Vec3 Rotate(Vec3 v, Vec3 axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return Add(Add(Scale(v, c), Scale(Cross(axis, v), s)), Scale(axis, Dot(axis, v) * (1 - c)));
}
inline float Deadzone(float axis, float dead = .2f) {
    if (!std::isfinite(axis) || !std::isfinite(dead) || dead < 0 || dead >= 1)
        return 0;
    const float a = std::clamp(std::abs(axis), 0.f, 1.f);
    return a <= dead ? 0 : std::copysign((a - dead) / (1 - dead), axis);
}
struct Pose {
    Vec3 eye, target, up;
    float fov = 40;
};
struct CameraInput {
    float right{}, up{}, forward{}, yaw{}, pitch{}, roll{}, fov_delta{}, speed = 1;
};
class Camera {
    Pose initial{}, pose{};
    Vec3 forward{}, world_up{};
    float distance{}, yaw{}, pitch{}, roll{};
    bool available{};

  public:
    bool Begin(Pose value) {
        available = false;
        if (!Finite(value.eye) || !Finite(value.target) || !Finite(value.up) ||
            !ValidPhotoFov(value.fov))
            return false;
        auto f = Add(value.target, Scale(value.eye, -1)), u = value.up;
        const float d = std::sqrt(Dot(f, f));
        if (!std::isfinite(d) || !Unit(f) || !Unit(u))
            return false;
        auto r = Cross(f, u);
        if (!Unit(r))
            return false;
        initial = pose = value;
        forward = f;
        world_up = u;
        distance = d;
        yaw = pitch = roll = 0;
        available = true;
        return true;
    }
    bool Advance(CameraInput input, float seconds) {
        if (!available || !std::isfinite(seconds) || seconds < 0 || seconds > .1f)
            return false;
        for (float v : {input.right, input.up, input.forward, input.yaw, input.pitch, input.roll,
                        input.fov_delta, input.speed})
            if (!std::isfinite(v))
                return false;
        if (input.speed < 0 || input.speed > 1e6f)
            return false;
        if (input.right == 0 && input.up == 0 && input.forward == 0 && input.yaw == 0 &&
            input.pitch == 0 && input.roll == 0 && input.fov_delta == 0)
            return true;
        const float new_yaw = std::remainder(yaw + input.yaw, 6.28318530718f);
        const float new_pitch = std::clamp(pitch + input.pitch, -1.45f, 1.45f);
        const float new_roll = std::remainder(roll + input.roll, 6.28318530718f);
        Vec3 f = Rotate(forward, world_up, new_yaw), r = Cross(f, world_up);
        if (!Unit(r))
            return false;
        f = Rotate(f, r, new_pitch);
        if (!Unit(f))
            return false;
        auto u = Cross(r, f);
        if (!Unit(u))
            return false;
        r = Rotate(r, f, new_roll);
        u = Rotate(u, f, new_roll);
        auto step = Add(Add(Scale(r, input.right), Scale(u, input.up)), Scale(f, input.forward));
        const float length = Dot(step, step);
        if (length > 1)
            step = Scale(step, 1 / std::sqrt(length));
        const auto eye = Add(pose.eye, Scale(step, seconds * input.speed));
        const auto target = Add(eye, Scale(f, distance));
        if (!Finite(eye) || !Finite(target))
            return false;
        pose = {eye, target, u, std::clamp(pose.fov + input.fov_delta, MinPhotoFov, MaxPhotoFov)};
        yaw = new_yaw;
        pitch = new_pitch;
        roll = new_roll;
        return true;
    }
    void Reset() {
        if (available) {
            pose = initial;
            yaw = pitch = roll = 0;
        }
    }
    bool Reposition(Vec3 eye) {
        const auto target = Add(eye, Add(pose.target, Scale(pose.eye, -1)));
        if (!available || !Finite(eye) || !Finite(target))
            return false;
        pose.eye = eye;
        pose.target = target;
        return true;
    }
    Pose Current() const {
        return pose;
    }
    Pose Original() const {
        return initial;
    }
};
} // namespace GzPhoto
