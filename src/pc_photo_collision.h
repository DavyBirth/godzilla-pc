// Camera movement against a scene-owned geometry query, independent of the game.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_camera.h"
#include <array>
namespace GzPhoto {
struct CameraHit {
    Vec3 position, normal;
};
// Query returns false for an unavailable scene, true for a valid query (including
// a miss). Sweep the centre and six offsets to keep a small clearance around it.
template <class Query>
bool CollideCamera(Vec3 from, Vec3 desired, Vec3 &resolved, Query query, float radius = .5f) {
    if (!Finite(from) || !Finite(desired) || !std::isfinite(radius) || radius <= 0)
        return false;
    Vec3 position = from, remaining = Add(desired, Scale(from, -1));
    const std::array<Vec3, 7> offsets{{{},
                                       {radius, 0, 0},
                                       {-radius, 0, 0},
                                       {0, radius, 0},
                                       {0, -radius, 0},
                                       {0, 0, radius},
                                       {0, 0, -radius}}};
    for (int pass = 0; pass < 3; ++pass) {
        const float length2 = Dot(remaining, remaining);
        if (length2 < 1e-10f)
            break;
        const float length = std::sqrt(length2);
        float fraction = 1;
        Vec3 normal{};
        bool blocked = false;
        for (auto offset : offsets) {
            CameraHit hit{};
            bool found = false;
            const auto start = Add(position, offset), end = Add(start, remaining);
            if (!query(start, end, hit, found))
                return false;
            if (!found)
                continue;
            if (!Finite(hit.position) || !Unit(hit.normal))
                return false;
            // A clearance probe can begin across a thin wall while the camera
            // centre is still outside it. Native two-sided meshes orient the
            // returned normal against the ray, even when that probe is leaving.
            // Let the centre move away from an already overlapping contact;
            // inward motion still collides and cannot cross the wall.
            const auto away = Add(position, Scale(hit.position, -1));
            if (Dot(away, away) <= radius * radius * 1.21f && Dot(remaining, away) > 1e-6f)
                continue;
            // Ignore surfaces we are leaving, so touching the floor does not
            // trap the camera when the player tries to raise it again.
            if (Dot(remaining, hit.normal) >= -1e-6f)
                continue;
            const float t = Dot(Add(hit.position, Scale(start, -1)), remaining) / length2;
            if (!std::isfinite(t) || t < -.001f || t > 1.001f)
                return false;
            // If the centre already overlaps the clearance region, an offset
            // on its near side can hit only after the centre crossed the wall.
            // Keep the current position for inward motion instead of using that
            // late hit; outward motion was allowed above.
            const bool overlapping =
                Dot(away, away) <= radius * radius * 1.21f && Dot(remaining, away) < -1e-6f;
            const float safe = overlapping ? 0.f : std::clamp(t - .02f / length, 0.f, 1.f);
            if (safe < fraction) {
                fraction = safe;
                normal = hit.normal;
                blocked = true;
            }
        }
        position = Add(position, Scale(remaining, fraction));
        if (!blocked)
            break;
        remaining = Scale(remaining, 1 - fraction);
        remaining = Add(remaining, Scale(normal, -Dot(remaining, normal)));
    }
    if (!Finite(position))
        return false;
    resolved = position;
    return true;
}
} // namespace GzPhoto
