// Scene-thread photo lifecycle, separate from the native adapter's bindings.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_camera.h"
#include <cstdint>

namespace GzPhoto {
// Epoch must change across loading/unloading, even if an allocation is reused.
struct SceneIdentity {
    uint64_t epoch{}, object{};
    uint32_t handle{};
    bool operator==(const SceneIdentity &) const = default;
};
struct SceneState {
    SceneIdentity identity{};
    Pose pose{};
    uint32_t pause_mask{};
    uint64_t pause_revision{}, camera_revision{};
    bool verified{};
};
// All operations run on the scene owner thread. Comparisons and mutations must
// be indivisible there. Revisions count EVERY writer, including original game
// code and same-value writes; comparing the value alone cannot detect takeover.
// An adapter without that guarantee must return false from Read.
class SceneBackend {
  public:
    virtual ~SceneBackend() = default;
    virtual bool Read(SceneState &) noexcept = 0;
    virtual bool SetPause(SceneIdentity, uint64_t revision, uint32_t expected, uint32_t desired,
                          SceneState &result) noexcept = 0;
    virtual bool SetCamera(SceneIdentity, uint64_t revision, const Pose &desired,
                           SceneState &result) noexcept = 0;
    virtual bool ConstrainCamera(const Pose &, Pose &) noexcept {
        return true;
    }
};
struct RestoreResult {
    bool camera{}, pause{};
};
class Session {
    SceneBackend &scene;
    SceneState original{}, owned{};
    Camera camera;
    bool active{};
    const char *failure = "none";

  public:
    explicit Session(SceneBackend &backend) : scene(backend) {}
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    // Owner must End on the scene thread before destruction. No destructor
    // calls into a possibly destroyed native scene or from the render thread.
    bool Begin(uint32_t proven_pause_bits) noexcept {
        if (active || !proven_pause_bits)
            return false;
        SceneState before{}, after{};
        if (!scene.Read(before) || !before.verified || !before.identity.epoch ||
            !before.identity.object || !camera.Begin(before.pose))
            return false;
        const uint32_t desired = before.pause_mask | proven_pause_bits;
        if (desired == before.pause_mask)
            return false; // Scene already paused by another owner.
        if (!scene.SetPause(before.identity, before.pause_revision, before.pause_mask, desired,
                            after))
            return false;
        original = before;
        owned = after;
        active = true;
        return true;
    }
    RestoreResult End() noexcept {
        if (!active)
            return {};
        active = false;
        SceneState restored{};
        // Restore the lens/pose before releasing our pause. Never copy back
        // matrices, stale pointers, aspect or scene-owned clipping planes.
        const bool camera_restored =
            scene.SetCamera(owned.identity, owned.camera_revision, original.pose, restored);
        const bool pause_restored = scene.SetPause(owned.identity, owned.pause_revision,
                                                   owned.pause_mask, original.pause_mask, restored);
        return {camera_restored, pause_restored};
    }
    bool Advance(CameraInput input, float seconds) noexcept {
        if (!active)
            return false;
        SceneState current{}, updated{};
        bool valid = false;
        if (!scene.Read(current))
            failure = "scene read failed";
        else if (!current.verified || current.identity != owned.identity)
            failure = "scene identity changed";
        else if (current.pause_revision != owned.pause_revision ||
                 current.pause_mask != owned.pause_mask)
            failure = "pause writer changed";
        else if (current.camera_revision != owned.camera_revision)
            failure = "camera writer changed";
        else {
            failure = "none";
            valid = true;
        }
        if (!valid) {
            End();
            return false;
        }
        const auto before = camera;
        if (!camera.Advance(input, seconds))
            return false;
        auto next = camera.Current();
        if (!scene.ConstrainCamera(before.Current(), next) || !camera.Reposition(next.eye)) {
            camera = before;
            return false;
        }
        const auto same_vector = [](Vec3 a, Vec3 b) {
            return a.x == b.x && a.y == b.y && a.z == b.z;
        };
        if (same_vector(next.eye, owned.pose.eye) && same_vector(next.target, owned.pose.target) &&
            same_vector(next.up, owned.pose.up) && next.fov == owned.pose.fov)
            return true;
        if (!scene.SetCamera(owned.identity, owned.camera_revision, next, updated)) {
            failure = "camera write rejected";
            End();
            return false;
        }
        owned = updated;
        return true;
    }
    bool Active() const noexcept {
        return active;
    }
    const char *FailureReason() const noexcept {
        return failure;
    }
    bool Reset() noexcept {
        if (!active)
            return false;
        camera.Reset();
        return Advance({}, 0);
    }
    SceneIdentity Identity() const noexcept {
        return active ? owned.identity : SceneIdentity{};
    }
};
} // namespace GzPhoto
