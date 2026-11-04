// Immutable per-frame settings shared by scene, presentation and PNG capture.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_session.h"
#include "pc_photo_grade.h"
#include "pc_photo_composition.h"
#include "pc_photo_effects.h"
#include <atomic>
#include <limits>
#include <mutex>
namespace GzPhoto {
struct PhotoSettings {
    Grade grade{};
    Guide guide = Guide::None;
    DepthOfField dof{};
    std::array<PhotoLight, 3> lights{};
    bool hide_hud{};
    float speed = 40; // World units per second; Godzilla stands about 64 units tall.
};
struct PhotoCapabilities {
    bool dof{}, lights{}, hud{}, spot_lights{};
};
inline bool Valid(const PhotoSettings &s) {
    if (!Valid(s.grade) || !Valid(s.dof) || int(s.guide) < 0 || int(s.guide) > 3 ||
        !std::isfinite(s.speed) || s.speed < 1 || s.speed > 1e6f)
        return false;
    for (const auto &light : s.lights)
        if (!Valid(light))
            return false;
    return true;
}
inline bool Supported(const PhotoSettings &s, PhotoCapabilities capabilities) {
    if (!Valid(s) || (!capabilities.dof && s.dof.mode != DofMode::Original) ||
        (!capabilities.hud && s.hide_hud))
        return false;
    for (const auto &light : s.lights)
        if (light.enabled &&
            (!capabilities.lights || (light.type == LightType::Spot && !capabilities.spot_lights)))
            return false;
    return true;
}
struct PhotoFrame {
    uint64_t ticket{}, revision{};
    SceneIdentity scene{};
    PhotoSettings settings{};
    PhotoCapabilities capabilities{};
    bool active{};
};
class PhotoChannel {
    mutable std::mutex mutex;
    std::atomic<bool> visible{};
    PhotoFrame frame{};
    uint64_t sequence{};

  public:
    uint64_t Open(SceneIdentity scene, PhotoSettings settings = {},
                  PhotoCapabilities capabilities = {}) {
        if (!scene.epoch || !scene.object || !Supported(settings, capabilities))
            return 0;
        std::lock_guard lock{mutex};
        if (frame.active || sequence == std::numeric_limits<uint64_t>::max())
            return 0;
        frame = {++sequence, 1, scene, settings, capabilities, true};
        visible.store(true, std::memory_order_release);
        return frame.ticket;
    }
    bool Edit(uint64_t ticket, PhotoSettings settings) {
        std::lock_guard lock{mutex};
        if (!frame.active || frame.ticket != ticket || !Supported(settings, frame.capabilities) ||
            frame.revision == std::numeric_limits<uint64_t>::max())
            return false;
        frame.settings = settings;
        ++frame.revision;
        return true;
    }
    bool Close(uint64_t ticket) {
        std::lock_guard lock{mutex};
        if (!frame.active || frame.ticket != ticket)
            return false;
        frame = {};
        visible.store(false, std::memory_order_release);
        return true;
    }
    bool Capabilities(uint64_t ticket, PhotoCapabilities capabilities) {
        std::lock_guard lock{mutex};
        if (!frame.active || frame.ticket != ticket ||
            frame.revision == std::numeric_limits<uint64_t>::max())
            return false;
        frame.capabilities = capabilities;
        if (!capabilities.lights)
            for (auto &light : frame.settings.lights)
                light.enabled = false;
        if (!capabilities.spot_lights)
            for (auto &light : frame.settings.lights)
                light.type = LightType::Point;
        if (!capabilities.dof)
            frame.settings.dof.mode = DofMode::Original;
        if (!capabilities.hud)
            frame.settings.hide_hud = false;
        ++frame.revision;
        return true;
    }
    PhotoFrame Read() const {
        if (!visible.load(std::memory_order_acquire))
            return {};
        std::lock_guard lock{mutex};
        return frame;
    }
};
inline PhotoChannel photo_channel;
// Scene-thread coordinator: presentation becomes active only after acquiring
// a verified camera/pause lease. Window/render threads edit settings, not RAM.
class PhotoCoordinator {
    Session session;
    PhotoChannel &channel;
    uint64_t ticket{};

  public:
    PhotoCoordinator(SceneBackend &backend, PhotoChannel &output)
        : session(backend), channel(output) {}
    bool Begin(uint32_t proven_pause_bits, PhotoSettings settings = {},
               PhotoCapabilities capabilities = {}) {
        if (!Supported(settings, capabilities) || !session.Begin(proven_pause_bits))
            return false;
        ticket = channel.Open(session.Identity(), settings, capabilities);
        if (!ticket) {
            session.End();
            return false;
        }
        return true;
    }
    RestoreResult End() {
        if (ticket) {
            channel.Close(ticket);
            ticket = 0;
        }
        return session.End();
    }
    bool Advance(CameraInput input, float seconds) {
        const bool result = session.Advance(input, seconds);
        if (!session.Active() && ticket) {
            channel.Close(ticket);
            ticket = 0;
        }
        return result;
    }
    uint64_t Ticket() const {
        return ticket;
    }
    const char *FailureReason() const {
        return session.FailureReason();
    }
    bool Reset() {
        const bool result = session.Reset();
        if (!session.Active() && ticket) {
            channel.Close(ticket);
            ticket = 0;
        }
        return result;
    }
};
} // namespace GzPhoto
