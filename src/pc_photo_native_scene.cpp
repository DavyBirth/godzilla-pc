// Pinned 01.01 scene calls. Configure never executes guest functions; tests
// exercise the bridge using only their own functions and synthetic memory.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_photo_native_scene.h"
#include "pc_pause_menu.h"
#include "pc_photo_channel.h"
#include "pc_photo_presets.h"
#include "pc_photo_native_effects.h"
#include "pc_photo_native_lights.h"
#include "pc_photo_dof_resources.h"
#include "pc_photo_game_lights.h"
#include "pc_photo_collision.h"
#include "native_hook.h"
#include "pc_features.h"
extern "C" {
#include "gz.h"
}
#include <algorithm>
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <memory>
#include <utility>

namespace GzPhotoNative {
namespace {
using namespace GzPhoto;
// Freeze all simulation groups; verified render-only units retain their native
// callback ordering through scoped leases below.
constexpr uint32_t PhotoPauseMask = UINT32_MAX;
using Render = uintptr_t(ABI *)(void *);
using Store = void(ABI *)(void *, uint32_t);
using Pause = void(ABI *)(uint32_t);
using Look = void(ABI *)(void *, const float *, const float *, const float *);
using Project = void(ABI *)(void *, float, float, float, float);
using LightUpload = uint32_t(ABI *)(void *, void *, const float *);
using ViewCopy = uintptr_t(ABI *)(void *, const void *);
unsigned char *image{};
uint64_t image_size{};
std::atomic<bool> configured{}, ready{}, exit_requested{}, toggle_requested{};
std::atomic<bool> input_allowed{true};
std::atomic<bool> reset_requested{};
std::atomic<uint32_t> observed_pause_mask{UINT32_MAX};
std::atomic<int> entry_notice{};
std::atomic<uint64_t> pause_revision{}, camera_revision{};
std::atomic<uintptr_t> camera_wrapper{};
std::mutex units_mutex, input_mutex;
std::recursive_mutex scene_mutex;
std::unordered_map<uintptr_t, uint64_t> unit_revisions;
CameraInput input{};
float input_seconds{};
struct PresetRequest {
    uint64_t ticket;
    Preset preset;
};
std::optional<PresetRequest> preset_request;
void clear_input() {
    std::lock_guard lock{input_mutex};
    input = {};
    input_seconds = 0;
    reset_requested = false;
    preset_request.reset();
}
DWORD owner_thread{};
uint64_t epoch{};
void *observed_unit{};
uint32_t observed_handle{};
unsigned observations{};
bool gameplay_publication{}; // Verified at the world-camera callback, not after HUD drawing.
uint64_t publication_count{};
// World cameras publish through the shared 0x14a190 path. The stage camera
// class 0x66fbf0 (wrapper +0x3c0) identifies the scene; see view_wrapper().
struct CameraProfile {
    uintptr_t table;
    size_t wrapper, active;
};
constexpr CameraProfile camera_profiles[]{{0x66fbf0, 0x3c0, 0x79ae58}};
const CameraProfile *observed_profile{};
// Pose substituted inside the native compositor while the session is held.
std::optional<Pose> override_pose;
// The frame draws the stage aggregator's view (class 0x665230, wrapper +0x5a0),
// which owns the stage camera at +0xb20. The aggregator recomposes it every
// frame through 0x14a130/0x14a160; the session substitutes its pose there so
// culling, sky and every derived pass follow the photo camera.
bool own_write{};
bool collision_profile{};
using RayQuery = bool(ABI *)(void *, const float *, const float *, void *, unsigned);
// PRaycastResult: two aligned vectors followed by two object descriptors.
struct alignas(16) NativeRayHit {
    float point[4]{}, normal[4]{};
    void *object{};
    void *descriptor{};
};
void *dof_owner{};
bool dof_init_profile{};
GzPhoto::DofResources photo_dof_resources;
void *lighting_owner{};
bool lighting_helpers{};
NativeLightApi light_api{};
GzNative::FunctionHook render_hook, destroy_hook, pause_hook, unit_hook, look_hook, project_hook,
    dof_hook, scheduler_hook;
GzNative::FunctionHook lighting_hook;
GzNative::FunctionHook game_lights_hook;
using GameLightsUpdate = uintptr_t(ABI *)(void *, void *);
GameLightsUpdate original_game_lights{};
GameLightLease game_light_lease;
bool game_lights_profile{}, game_lights_available{};
GzNative::FunctionHook hud_hook;
GzNative::FunctionHook view_hook, group_hook;
using GroupDraw = uintptr_t(ABI *)(void *, uint64_t, uint32_t);
GroupDraw original_group{};
using HudDraw = uintptr_t(ABI *)(void *, unsigned);
HudDraw original_hud{};
ViewCopy original_view{};
LightUpload original_lighting{};
Render original_render{}, original_destroy{}, original_dof{}, original_scheduler{};
Store original_pause_store{}, original_unit_store{};
Look original_look{};
Project original_project{};
bool read(const void *source, void *destination, size_t size) {
    SIZE_T got{};
    return source && ReadProcessMemory(GetCurrentProcess(), source, destination, size, &got) &&
           got == size;
}
template <class T> bool get(const void *p, size_t offset, T &value) {
    return p && read(static_cast<const unsigned char *>(p) + offset, &value, sizeof(value));
}
bool writable(void *p, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!p || !VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const auto protection = info.Protect & 0xff;
    return (protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY) &&
           size <= info.RegionSize - (uintptr_t(p) - uintptr_t(info.BaseAddress));
}
unsigned char *view_wrapper() {
    if (!observed_unit || !observed_profile)
        return nullptr;
    auto *unit = static_cast<unsigned char *>(observed_unit);
    if (observed_profile == &camera_profiles[0]) {
        uint64_t table{};
        auto *parent = unit - 0xb20;
        if (get(parent, 0, table) && table == uintptr_t(image + 0x665230))
            return parent + 0x5a0;
    }
    return unit + observed_profile->wrapper;
}

// Offline-studied radio display candidate. The parent has a no-op draw slot;
// its independent text/model children remain registered with their renderers.
// Preserve their native updates and timers, and lease only display data.
// Opt-in until portrait AND text restoration have been observed in game.
constexpr uintptr_t RadioTable = 0x653e10, RadioTextTable = 0x654e90, RadioModelTable = 0x655030;
bool radio_profile_verified{};
bool registered_unit(void *object, uint32_t handle, uintptr_t table) {
    void *entries{};
    uint32_t count{}, actual{};
    uint16_t generation{};
    void *found{};
    uintptr_t kind{};
    if (!object || !handle || !get(image, 0xa2b5a8, entries) || !entries ||
        !get(image, 0xa2b5b0, count) || count > 65536 || (handle & 0xffff) >= count)
        return false;
    auto *entry = static_cast<unsigned char *>(entries) + 16 * (handle & 0xffff);
    return get(entry, 0, generation) && generation == handle >> 16 && get(entry, 8, found) &&
           found == object && get(object, 0xb0, actual) && actual == handle &&
           get(object, 0, kind) && kind == uintptr_t(image + table);
}
struct RadioDisplayField {
    void *parent{};
    uint32_t parent_handle{};
    void *child{};
    uint32_t child_handle{};
    void *model{};
    void *target{};
    uint32_t before{};
    bool text{};
};
bool radio_model_alpha(void *child, void *expected, void *&target) {
    void *head{};
    void *node{};
    if (!get(child, 0x3b8, head) || !head || !get(head, 0, node))
        return false;
    for (unsigned n = 0; node && node != head && n < 256; ++n) {
        void *model{};
        if (!get(node, 0x10, model))
            return false;
        if (model == expected) {
            uintptr_t table{};
            void *uniforms{};
            return get(model, 0x10, table) && table == uintptr_t(image + 0x655010) &&
                   get(model, 0x90, uniforms) && uniforms && get(uniforms, 0x10, target) && target;
        }
        if (!get(node, 0, node))
            return false;
    }
    return false;
}
bool radio_field_matches(const RadioDisplayField &f) {
    const size_t offset = f.text ? 0x3c0 : 0x3d0;
    void *child{};
    uint32_t handle{};
    if (!registered_unit(f.parent, f.parent_handle, RadioTable) || !get(f.parent, offset, handle) ||
        handle != f.child_handle || !get(f.parent, offset + 8, child) || child != f.child ||
        !registered_unit(child, handle, f.text ? RadioTextTable : RadioModelTable))
        return false;
    if (f.text)
        return f.target == static_cast<unsigned char *>(child) + 0x3a8;
    void *target{};
    return radio_model_alpha(child, f.model, target) && target == f.target;
}
class RadioDisplayLease {
    std::vector<RadioDisplayField> fields;

    void hide(RadioDisplayField field) {
        const size_t bytes = field.text ? 1 : 4;
        if (!radio_field_matches(field) || !read(field.target, &field.before, bytes) ||
            !writable(field.target, bytes) || !field.before)
            return;
        if (field.text && field.before != 1)
            return;
        if (!field.text) {
            float alpha{};
            std::memcpy(&alpha, &field.before, 4);
            if (!std::isfinite(alpha) || alpha < 0 || alpha > 1)
                return;
        }
        // Reserve before mutation, so allocation failure cannot orphan a write.
        fields.push_back(field);
        const uint32_t zero{};
        std::memcpy(field.target, &zero, bytes);
    }

  public:
    size_t Count() const {
        return fields.size();
    }
    bool Enabled() const {
        const char *option = std::getenv("GZ_PHOTO_HIDE_RADIO");
        return !(option && !std::strcmp(option, "0")) && radio_profile_verified;
    }
    void Release() {
        for (const auto &field : fields) {
            const size_t bytes = field.text ? 1 : 4;
            uint32_t actual{};
            if (radio_field_matches(field) && read(field.target, &actual, bytes) && actual == 0 &&
                writable(field.target, bytes))
                std::memcpy(field.target, &field.before, bytes);
        }
        fields.clear();
    }
    void Apply() {
        if (!Enabled() || !fields.empty())
            return;
        void *entries{};
        uint32_t count{};
        if (!get(image, 0xa2b5a8, entries) || !entries || !get(image, 0xa2b5b0, count) ||
            count > 65536)
            return;
        for (unsigned i = 1; i < count; ++i) {
            auto *entry = static_cast<unsigned char *>(entries) + 16 * i;
            void *parent{};
            uint32_t handle{};
            uintptr_t table{};
            if (!get(entry, 8, parent) || !parent || !get(parent, 0, table) ||
                table != uintptr_t(image + RadioTable) || !get(parent, 0xb0, handle) ||
                !registered_unit(parent, handle, RadioTable))
                continue;
            for (bool text : {true, false}) {
                RadioDisplayField field;
                field.parent = parent;
                field.parent_handle = handle;
                field.text = text;
                const size_t offset = text ? 0x3c0 : 0x3d0;
                if (!get(parent, offset, field.child_handle) ||
                    !get(parent, offset + 8, field.child) ||
                    !registered_unit(field.child, field.child_handle,
                                     text ? RadioTextTable : RadioModelTable))
                    continue;
                if (text) {
                    field.target = static_cast<unsigned char *>(field.child) + 0x3a8;
                    hide(field);
                    continue;
                }
                // The portrait is an AvPlayer video on the monitor model; neither its
                // opacity nor its item list hides it (tested in game). Still open.
            }
        }
    }
} radio_display;
uint64_t unit_revision(void *unit) {
    std::lock_guard lock{units_mutex};
    auto it = unit_revisions.find(uintptr_t(unit));
    return it == unit_revisions.end() ? 0 : it->second;
}
struct UnitLease {
    void *unit{};
    uint32_t handle{}, before{}, after{};
    uint64_t revision{};
    bool owned{};
};
class NativeBackend final : public SceneBackend {
    std::vector<UnitLease> units;
    bool held{};
    uint32_t initial_mask{}, owned_mask{};
    // World time scales (stage object image+0xa183e0, +0x384/+0x388). The native
    // pause stops scheduled units; effects, aircraft and projectiles keep running
    // on these scales, so the session also holds them at zero.
    float saved_speed[2]{};
    bool speed_owned{};
    void freeze_speed() {
        speed_owned = false;
        if (const char *f = std::getenv("GZ_PHOTO_FREEZE"); f && *f == '0')
            return;
        auto *at = image + 0xa183e0 + 0x384;
        float v[2]{};
        if (!get(at, 0, v[0]) || !get(at, 4, v[1]) || !writable(at, 8))
            return;
        for (float x : v)
            if (!std::isfinite(x) || x < 0 || x > 16)
                return;
        saved_speed[0] = v[0];
        saved_speed[1] = v[1];
        // +0x384 is the frame delta (camera publication needs it); only the scale stops.
        // A scale of exactly zero also stops the view recomposition the photo camera
        // rides on; a negligible positive scale keeps it while the world stays still.
        const float still = 1e-6f;
        std::memcpy(at + 4, &still, 4);
        speed_owned = true;
        gz_log("Photo native: frame delta %.3f kept, world time scale %.3f held at zero", v[0],
               v[1]);
    }
    void release_speed() {
        if (!speed_owned)
            return;
        speed_owned = false;
        auto *at = image + 0xa183e0 + 0x384;
        float v[2]{};
        if (!get(at, 0, v[0]) || !get(at, 4, v[1]) || !writable(at, 8))
            return;
        // Restore only what this session still holds; a scale the game changed meanwhile wins.
        if (v[1] == 1e-6f)
            std::memcpy(at + 4, &saved_speed[1], 4);
    }
    uint64_t owned_revision{};
    SceneIdentity last_pose_identity{};
    Pose last_pose{};
    uint64_t last_pose_revision{};
    bool pose_observed{};
    const char *acquisition_reason = "none";
    bool identity(SceneIdentity expected, SceneState &current) {
        return Read(current) && current.identity == expected;
    }
    bool capture_units() {
        void *entries{};
        uint32_t count{};
        if (!get(image, 0xa2b5a8, entries) || !get(image, 0xa2b5b0, count) || !entries ||
            count < 2 || count > 65536) {
            acquisition_reason = "invalid unit registry";
            return false;
        }
        struct Entry {
            uint16_t generation;
            unsigned char padding[6];
            void *object;
        };
        static_assert(sizeof(Entry) == 16);
        std::vector<Entry> registry(count);
        if (!read(entries, registry.data(), registry.size() * sizeof(Entry))) {
            acquisition_reason = "unreadable unit registry";
            return false;
        }
        units.clear();
        units.reserve(count);
        for (unsigned i = 1; i < count; ++i) {
            if (!registry[i].object)
                continue;
            UnitLease unit;
            unit.unit = registry[i].object;
            if (!get(unit.unit, 0xb0, unit.handle) ||
                unit.handle != (uint32_t(registry[i].generation) << 16 | i) ||
                !get(unit.unit, 0x88, unit.before)) {
                acquisition_reason = "unit generation/handle mismatch";
                return false;
            }
            unit.revision = unit_revision(unit.unit);
            units.push_back(unit);
        }
        const bool found = std::any_of(units.begin(), units.end(), [](const UnitLease &unit) {
            return unit.unit == observed_unit && unit.handle == observed_handle;
        });
        if (!found)
            acquisition_reason = "camera missing from unit registry";
        return found;
    }
    void restore_units() {
        for (const auto &unit : units) {
            if (!unit.owned)
                continue;
            uint32_t handle{}, value{};
            if (get(unit.unit, 0xb0, handle) && handle == unit.handle &&
                unit_revision(unit.unit) == unit.revision && get(unit.unit, 0x88, value) &&
                value == unit.after)
                reinterpret_cast<Store>(image + 0x112a10)(unit.unit, unit.before);
        }
    }

  public:
    const char *AcquisitionReason() const {
        return acquisition_reason;
    }
    bool SnapshotView() {
        auto *wrapper = view_wrapper();
        if (!wrapper) {
            acquisition_reason = "world view unavailable";
            return false;
        }
        alignas(16) float eye[4], target[4], up[4], projection[4];
        if (!get(wrapper, 0xe0, eye) || !get(wrapper, 0xf0, target) || !get(wrapper, 0x100, up) ||
            !get(wrapper, 0x298, projection)) {
            acquisition_reason = "world view unreadable";
            return false;
        }
        // Substitution starts with the current pose, before the next native
        // recomposition could be mistaken for a foreign camera writer.
        camera_wrapper = uintptr_t(wrapper);
        override_pose = Pose{{eye[0], eye[1], eye[2]},
                             {target[0], target[1], target[2]},
                             {up[0], up[1], up[2]},
                             projection[0] * 57.295779513f};
        return true;
    }
    bool Inspect(SceneState &result, bool owner_only) noexcept {
        if (!configured || !owner_thread || (owner_only && GetCurrentThreadId() != owner_thread) ||
            !observed_unit || observations < 2 || !gameplay_publication || !gz_pc_gameplay())
            return false;
        void *unit{};
        uint64_t table{};
        uint32_t handle{}, mask{};
        if (!observed_profile || !get(image, observed_profile->active, unit) ||
            unit != observed_unit || !get(unit, 0, table) ||
            table != uintptr_t(image + observed_profile->table) || !get(unit, 0xb0, handle) ||
            handle != observed_handle || !get(image, 0xa2b590, mask))
            return false;
        alignas(16) float eye[4], target[4], up[4], projection[4];
        auto *wrapper = view_wrapper();
        if (!wrapper)
            return false;
        if (!get(wrapper, 0xe0, eye) || !get(wrapper, 0xf0, target) || !get(wrapper, 0x100, up) ||
            !get(wrapper, 0x298, projection))
            return false;
        Pose pose{{eye[0], eye[1], eye[2]},
                  {target[0], target[1], target[2]},
                  {up[0], up[1], up[2]},
                  projection[0] * 57.295779513f};
        Camera validation;
        if (!validation.Begin(pose) || !std::isfinite(projection[1]) || projection[1] <= 0 ||
            !std::isfinite(projection[2]) || !std::isfinite(projection[3]) || projection[2] <= 0 ||
            projection[3] <= projection[2])
            return false;
        const SceneIdentity key{epoch, uintptr_t(unit), handle};
        const auto revision = camera_revision.load();
        const auto same = [](Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
        if (held && pose_observed && key == last_pose_identity && revision == last_pose_revision &&
            (!same(pose.eye, last_pose.eye) || !same(pose.target, last_pose.target) ||
             !same(pose.up, last_pose.up) || pose.fov != last_pose.fov))
            return false;
        last_pose_identity = key;
        last_pose = pose;
        last_pose_revision = revision;
        pose_observed = true;
        result = {key, pose, mask, pause_revision.load(), revision, true};
        return true;
    }
    bool Read(SceneState &result) noexcept override {
        return Inspect(result, true);
    }
    bool SetPause(SceneIdentity expected, uint64_t revision, uint32_t before, uint32_t desired,
                  SceneState &result) noexcept override {
        try {
            SceneState current;
            if (held) {
                if (GetCurrentThreadId() == owner_thread)
                    radio_display.Release();
                uint32_t actual{};
                const bool ours =
                    GetCurrentThreadId() == owner_thread &&
                    expected == SceneIdentity{epoch, uintptr_t(observed_unit), observed_handle} &&
                    revision == owned_revision && pause_revision.load() == owned_revision &&
                    before == owned_mask && desired == initial_mask &&
                    get(image, 0xa2b590, actual) && actual == owned_mask;
                override_pose.reset();
                camera_wrapper = uintptr_t(view_wrapper());
                if (!ours) {
                    release_speed();
                    held = false;
                    units.clear();
                    return false;
                }
                release_speed();
                restore_units();
                reinterpret_cast<Store>(image + 0x113f70)(image + 0xa2b510, desired);
                held = false;
                units.clear();
                if (!Read(result))
                    result = {expected, {}, desired, pause_revision.load(), camera_revision.load(),
                              false};
                return true;
            }
            acquisition_reason = "scene/pause changed before acquisition";
            if (!identity(expected, current) || current.pause_revision != revision ||
                current.pause_mask != before)
                return false;
            if (!held) {
                // Acquisition from the game's own pause menu is intentionally
                // deferred; never take over an already held native pause.
                if (before || !desired) {
                    acquisition_reason = "native pause already held";
                    return false;
                }
                if (!capture_units())
                    return false;
                initial_mask = before;
                reinterpret_cast<Pause>(image + 0x1792d0)(desired);
                owned_mask = desired;
                owned_revision = pause_revision.load();
                for (auto &unit : units) {
                    uint32_t assigned{};
                    if (!get(unit.unit, 0x88, assigned)) {
                        acquisition_reason = "unreadable paused unit";
                        restore_units();
                        return false;
                    }
                    // Releasing every unit crashed (eboot+0x96f38). Only the
                    // world camera is rescheduled, with its simulation gated
                    // below, so the native view publication keeps running.
                    if (unit.unit != observed_unit || unit.handle != observed_handle)
                        continue;
                    reinterpret_cast<Store>(image + 0x112a10)(unit.unit, 0);
                    const auto after_revision = unit_revision(unit.unit);
                    unit.owned =
                        after_revision != unit.revision && get(unit.unit, 0x88, unit.after);
                    unit.revision = after_revision;
                }
                if (!SnapshotView()) {
                    restore_units();
                    reinterpret_cast<Store>(image + 0x113f70)(image + 0xa2b510, initial_mask);
                    return false;
                }
                held = true;
                freeze_speed();
                if (Read(result) && result.identity == expected && result.pause_mask == desired) {
                    acquisition_reason = "acquired";
                    return true;
                }
                acquisition_reason = "native pause did not preserve camera identity";
                release_speed();
                restore_units();
                reinterpret_cast<Store>(image + 0x113f70)(image + 0xa2b510, initial_mask);
                held = false;
                return false;
            }
            return false;
        } catch (...) {
            gz_stop(21, "Photo native: pause transaction failed");
        }
    }
    bool ConstrainCamera(const Pose &from, Pose &desired) noexcept override {
        const auto delta = Add(desired.eye, Scale(from.eye, -1));
        if (Dot(delta, delta) < 1e-10f)
            return true;
        if (!collision_profile)
            return false;
        void *world{};
        void *raw{};
        uintptr_t table{}, raw_table{}, ray{};
        if (!get(image, 0xa271d0, world) || !get(world, 0, table) ||
            table != uintptr_t(image + 0x66ef20) || !get(world, 0x80, raw) ||
            !get(raw, 0, raw_table) || raw_table != uintptr_t(image + 0x69f2c0) ||
            !get(reinterpret_cast<void *>(raw_table), 0x38, ray) ||
            ray != uintptr_t(image + 0x37f830))
            return false;
        // The native ray helper reads only +0x70 for group 15 (all groups).
        // Supply our own query context; never edit the world's filter tables.
        alignas(16) unsigned char context[0x78]{};
        std::memcpy(context + 0x70, &raw, sizeof(raw));
        auto query = [&](Vec3 a, Vec3 b, CameraHit &hit, bool &found) {
            alignas(16) const float start[]{a.x, a.y, a.z, 1}, end[]{b.x, b.y, b.z, 1};
            NativeRayHit result;
            found = reinterpret_cast<RayQuery>(image + 0x2d8890)(context, start, end, &result, 15);
            if (found)
                hit = {{result.point[0], result.point[1], result.point[2]},
                       {result.normal[0], result.normal[1], result.normal[2]}};
            return true;
        };
        Vec3 eye{};
        if (!CollideCamera(from.eye, desired.eye, eye, query))
            return false;
        static uint64_t logged_ticket{};
        const auto frame = photo_channel.Read();
        if (Dot(Add(eye, Scale(desired.eye, -1)), Add(eye, Scale(desired.eye, -1))) > 1e-6f &&
            logged_ticket != frame.ticket) {
            logged_ticket = frame.ticket;
            gz_log("Photo collision: surface stopped camera at %.3f %.3f %.3f (requested %.3f %.3f "
                   "%.3f)",
                   eye.x, eye.y, eye.z, desired.eye.x, desired.eye.y, desired.eye.z);
        }
        desired.target = Add(desired.target, Add(eye, Scale(desired.eye, -1)));
        desired.eye = eye;
        return true;
    }
    bool SetCamera(SceneIdentity expected, uint64_t revision, const Pose &pose,
                   SceneState &result) noexcept override {
        SceneState current;
        Camera validation;
        if (!validation.Begin(pose) || !identity(expected, current) ||
            current.camera_revision != revision)
            return false;
        if (!observed_profile)
            return false;
        auto *wrapper = view_wrapper();
        if (!wrapper)
            return false;
        alignas(16) float eye[4], target[4], up[4], projection[4];
        if (!get(wrapper, 0xe0, eye) || !get(wrapper, 0xf0, target) || !get(wrapper, 0x100, up) ||
            !get(wrapper, 0x298, projection))
            return false;
        eye[0] = pose.eye.x;
        eye[1] = pose.eye.y;
        eye[2] = pose.eye.z;
        target[0] = pose.target.x;
        target[1] = pose.target.y;
        target[2] = pose.target.z;
        up[0] = pose.up.x;
        up[1] = pose.up.y;
        up[2] = pose.up.z;
        own_write = true;
        reinterpret_cast<Look>(image + 0x14a130)(wrapper, eye, target, up);
        reinterpret_cast<Project>(image + 0x14a160)(wrapper, pose.fov * .01745329252f,
                                                    projection[1], projection[2], projection[3]);
        own_write = false;
        if (held)
            override_pose = pose;
        return Read(result);
    }
} backend;
PhotoCoordinator coordinator{backend, photo_channel};

// Resolve the published gameplay HUD through the generation-checked registry.
// Never write unit flags: exiting the photo session restores drawing naturally.
void *current_hud() {
    uint32_t handle{}, count{};
    void *entries{};
    if (!get(image, 0x79af20, handle) || !handle || !get(image, 0xa2b5b0, count) || count > 65536 ||
        (handle & 0xffff) >= count || !get(image, 0xa2b5a8, entries) || !entries)
        return nullptr;
    const auto *entry = static_cast<const unsigned char *>(entries) + 16 * (handle & 0xffff);
    uint16_t generation{};
    void *object{};
    uint32_t object_handle{};
    uintptr_t table{};
    if (!get(entry, 0, generation) || generation != (handle >> 16) || !get(entry, 8, object) ||
        !object || !get(object, 0xb0, object_handle) || object_handle != handle ||
        !get(object, 0, table) || table != uintptr_t(image + 0x670000))
        return nullptr;
    return object;
}
ABI uintptr_t draw_hud(void *object, unsigned layer = 0) {
    bool hidden = false;
    {
        std::lock_guard lock{scene_mutex};
        const auto frame = photo_channel.Read();
        SceneState state;
        hidden = frame.active && frame.capabilities.hud && frame.settings.hide_hud &&
                 backend.Inspect(state, false) && state.identity == frame.scene &&
                 object == current_hud();
        static uint64_t logged_ticket{};
        if (frame.active && logged_ticket != frame.ticket) {
            logged_ticket = frame.ticket;
            gz_log("Photo HUD: draw observed object=%p expected=%p capability=%d requested=%d "
                   "hidden=%d",
                   object, current_hud(), int(frame.capabilities.hud), int(frame.settings.hide_hud),
                   int(hidden));
        }
    }
    return hidden ? 0 : original_hud(object, layer);
}
ABI void pause_store(void *manager, uint32_t value) {
    std::lock_guard lock{scene_mutex};
    if (manager == image + 0xa2b510)
        ++pause_revision;
    original_pause_store(manager, value);
}
ABI void unit_store(void *unit, uint32_t value) {
    try {
        std::lock_guard scene_lock{scene_mutex};
        {
            std::lock_guard lock{units_mutex};
            ++unit_revisions[uintptr_t(unit)];
        }
        original_unit_store(unit, value);
    } catch (...) {
        gz_stop(21, "Photo native: cannot track unit pause ownership");
    }
}
ABI void look(void *wrapper, const float *eye, const float *target, const float *up) {
    std::lock_guard lock{scene_mutex};
    if (uintptr_t(wrapper) == camera_wrapper.load() && !own_write && override_pose &&
        GetCurrentThreadId() == owner_thread) {
        // Native compositor runs with the photo pose; its own sources stay intact.
        alignas(16) float e[4]{override_pose->eye.x, override_pose->eye.y, override_pose->eye.z,
                               eye[3]};
        alignas(16) float t[4]{override_pose->target.x, override_pose->target.y,
                               override_pose->target.z, target[3]};
        alignas(16) float u[4]{override_pose->up.x, override_pose->up.y, override_pose->up.z,
                               up[3]};
        original_look(wrapper, e, t, u);
        return;
    }
    if (uintptr_t(wrapper) == camera_wrapper.load())
        ++camera_revision;
    original_look(wrapper, eye, target, up);
}
ABI void project(void *wrapper, float fov, float aspect, float near_plane, float far_plane) {
    std::lock_guard lock{scene_mutex};
    if (uintptr_t(wrapper) == camera_wrapper.load() && !own_write && override_pose &&
        GetCurrentThreadId() == owner_thread) {
        original_project(wrapper, override_pose->fov * .01745329252f, aspect, near_plane,
                         far_plane);
        return;
    }
    if (uintptr_t(wrapper) == camera_wrapper.load())
        ++camera_revision;
    original_project(wrapper, fov, aspect, near_plane, far_plane);
}
// Called with scene_mutex held whenever the world camera's view is drawn.
bool observe_camera(void *unit, const CameraProfile *profile, uint32_t handle) {
    if (owner_thread && owner_thread != GetCurrentThreadId()) {
        ready = false;
        gameplay_publication = false;
        return false;
    }
    owner_thread = GetCurrentThreadId();
    if (unit != observed_unit || handle != observed_handle || profile != observed_profile) {
        if (Active())
            gz_log("Photo native: world camera changed during session");
        coordinator.End();
        observed_unit = unit;
        observed_handle = handle;
        observed_profile = profile;
        observations = 0;
        gameplay_publication = false;
        ++epoch;
        clear_input();
        dof_owner = lighting_owner = nullptr;
        {
            std::lock_guard units_lock{units_mutex};
            unit_revisions.clear();
        }
        camera_wrapper = uintptr_t(unit) + profile->wrapper;
        ++camera_revision;
    }
    if (observations < 3)
        ++observations;
    return true;
}
ABI uintptr_t copy_view(void *destination, const void *source) {
    {
        std::lock_guard lock{scene_mutex};
        void *published{};
        uint32_t publication_lock{};
        if (observed_unit && observed_profile && GetCurrentThreadId() == owner_thread &&
            destination == image + 0xa18460 &&
            source ==
                static_cast<unsigned char *>(observed_unit) + observed_profile->wrapper + 0x10 &&
            get(image, 0xa271f0, published) &&
            published == static_cast<unsigned char *>(observed_unit) + observed_profile->wrapper &&
            get(image, 0xa271e0, publication_lock) && publication_lock == 0)
            ++publication_count;
        // King of the Kaiju/Evolution: the parent camera (0x665230) copies its
        // own view inline (eboot+0xa1100) and the 0x14a190 render never runs.
        void *active{};
        uint64_t table{}, parent_table{};
        uint32_t handle{};
        if (destination == image + 0xa18460 && get(image, camera_profiles[0].active, active) &&
            get(active, 0, table) && table == uintptr_t(image + camera_profiles[0].table) &&
            get(static_cast<unsigned char *>(active) - 0xb20, 0, parent_table) &&
            parent_table == uintptr_t(image + 0x665230) &&
            source == static_cast<unsigned char *>(active) - 0xb20 + 0x5b0 &&
            get(active, 0xb0, handle) && get(image, 0xa271e0, publication_lock) &&
            publication_lock == 0 && observe_camera(active, &camera_profiles[0], handle)) {
            const auto result = original_view(destination, source);
            gameplay_publication = true;
            SceneState state;
            ready = backend.Read(state);
            observed_pause_mask = ready ? state.pause_mask : UINT32_MAX;
            return result;
        }
        if (destination == image + 0xa18460 && std::getenv("GZ_PHOTO_PROBE")) {
            static uintptr_t seen[16]{};
            static unsigned count{};
            const auto from = uintptr_t(__builtin_return_address(0)) - uintptr_t(image);
            if (count < 16 && std::find(seen, seen + count, from) == seen + count) {
                seen[count++] = from;
                void *active{};
                get(image, 0x79ae58, active);
                gz_log("Photo probe: view copy from=%llx source=%p active=%p thread=%lu",
                       (unsigned long long)from, source, active,
                       (unsigned long)GetCurrentThreadId());
            }
        }
    }
    return original_view(destination, source);
}
// 0x1139c0 draws every unit in the manager's draw lists selected by the mask
// bits. The gameplay HUD (radar, height, disaster level, destruction rate,
// slots) is one unit of class 0x6700a0 in the root list; while the photo
// session hides the HUD this walk repeats the native traversal and skips it.
constexpr uintptr_t GameplayHudTable = 0x6700a0;
// The separate screen-space scene draws mission goals, bonuses and stage
// announcements after the world view. Hiding the gameplay HUD alone misses it.
constexpr uintptr_t MissionOverlayTable = 0x654ef0;
ABI uintptr_t draw_group(void *manager, uint64_t mask, uint32_t argument) {
    const auto frame = photo_channel.Read();
    unsigned char *lists{};
    if (!frame.active || !frame.capabilities.hud || !frame.settings.hide_hud ||
        manager != image + 0xa2b510 || GetCurrentThreadId() != owner_thread ||
        !get(manager, 0x38, lists) || !lists)
        return original_group(manager, mask, argument);
    // Same order as 0x1139c0: per mask bit, a 32-byte list head at +0x38,
    // nodes linked at +0x18 with the unit at +8 and its draw callback at +0x80.
    for (unsigned bit = 0; mask; ++bit, mask >>= 1) {
        if (!(mask & 1))
            continue;
        auto *head = lists + 32 * bit;
        unsigned char *node{};
        if (!get(head, 0x18, node))
            continue;
        while (node && node != head) {
            unsigned char *unit{};
            uintptr_t table{}, callback{};
            if (!get(node, 8, unit) || !unit || !get(unit, 0, table) ||
                !get(unit, 0x80, callback) || !callback)
                gz_stop(21, "Photo native: unreadable draw list during HUD hiding");
            static std::vector<std::pair<unsigned, uintptr_t>> listed;
            const std::pair key{bit, table};
            if (std::getenv("GZ_PHOTO_LIST_UNITS") && listed.size() < 200 &&
                std::find(listed.begin(), listed.end(), key) == listed.end()) {
                listed.push_back(key);
                gz_log("Photo draw unit: bit=%u table=0x%llx", bit,
                       (unsigned long long)(table - uintptr_t(image)));
            }
            static uintptr_t extra_skip{};
            static unsigned polls{};
            if (const char *f = std::getenv("GZ_PHOTO_SKIP_TABLE"); f && ++polls % 200 == 1) {
                if (FILE *h = std::fopen(f, "r")) {
                    char b[32]{};
                    if (std::fgets(b, sizeof b, h))
                        extra_skip = uintptr_t(std::strtoull(b, nullptr, 16));
                    std::fclose(h);
                }
            }
            if (extra_skip && table == uintptr_t(image + extra_skip)) {
                if (!get(node, 0x18, node))
                    gz_stop(21, "Photo native: unreadable draw list link");
                continue;
            }
            // Radio frame, subtitle and portrait units (draw-list view of classes
            // 0x653e10/0x654e90/0x655030) are skipped the same way as the HUD.
            const auto offset = table - uintptr_t(image);
            const bool hidden = offset == GameplayHudTable || offset == MissionOverlayTable ||
                                offset == RadioTable + 0xa0 || offset == RadioTextTable + 0xa0 ||
                                offset == RadioModelTable + 0xa0;
            if (!hidden)
                reinterpret_cast<uintptr_t(ABI *)(void *, uint32_t)>(callback)(unit, argument);
            if (!get(node, 0x18, node))
                gz_stop(21, "Photo native: unreadable draw list link");
        }
    }
    return 0;
}
uintptr_t destroyed(void *unit, Render original) {
    std::unique_lock lock{scene_mutex};
    if (unit == observed_unit) {
        if (Active() && GetCurrentThreadId() != owner_thread)
            gz_stop(21, "Photo native: camera lifetime moved to an unverified thread");
        if (GetCurrentThreadId() == owner_thread)
            coordinator.End();
        clear_input();
        ready = false;
        gameplay_publication = false;
        observed_unit = nullptr;
        observed_profile = nullptr;
        camera_wrapper = 0;
        observations = 0;
        dof_owner = lighting_owner = nullptr;
        ++epoch;
        // Ownership belongs to this camera's lifetime. A later verified world
        // camera may be published by a different thread after changing modes.
        // Keep the cross-thread guard while a live camera/session exists.
        owner_thread = 0;
        {
            std::lock_guard units_lock{units_mutex};
            unit_revisions.clear();
        }
    }
    lock.unlock();
    return original(unit);
}
ABI uintptr_t destroy(void *unit) {
    return destroyed(unit, original_destroy);
}
ABI uintptr_t render(void *wrapper) {
    try {
        std::unique_lock lock{scene_mutex};
        const auto before_publication = publication_count;
        void *unit{};
        const CameraProfile *profile{};
        uint32_t handle{};
        for (const auto &candidate_profile : camera_profiles) {
            void *active{};
            uint64_t table{};
            auto *owner = static_cast<unsigned char *>(wrapper) - candidate_profile.wrapper;
            if (get(image, candidate_profile.active, active) && active == owner &&
                get(owner, 0, table) && table == uintptr_t(image + candidate_profile.table) &&
                get(owner, 0xb0, handle)) {
                unit = owner;
                profile = &candidate_profile;
                break;
            }
        }
        const bool candidate = profile != nullptr;
        if (!candidate && std::getenv("GZ_PHOTO_PROBE")) {
            static uintptr_t seen[16]{};
            static unsigned count{};
            if (count < 16 && std::find(seen, seen + count, uintptr_t(wrapper)) == seen + count) {
                seen[count++] = uintptr_t(wrapper);
                void *active{};
                uint64_t t1{}, t2{}, t3{};
                get(image, 0x79ae58, active);
                get(static_cast<unsigned char *>(wrapper) - 0x3c0, 0, t1);
                get(static_cast<unsigned char *>(wrapper) - 0x5a0, 0, t2);
                get(wrapper, 0, t3);
                gz_log("Photo probe: render wrapper=%p active=%p vt-3c0=%llx vt-5a0=%llx vt0=%llx "
                       "thread=%lu",
                       wrapper, active, (unsigned long long)t1, (unsigned long long)t2,
                       (unsigned long long)t3, (unsigned long)GetCurrentThreadId());
            }
        }
        if (candidate && !observe_camera(unit, profile, handle)) {
            lock.unlock();
            return original_render(wrapper);
        }
        lock.unlock();
        const auto result = original_render(wrapper);
        lock.lock();
        if (candidate && unit == observed_unit && handle == observed_handle &&
            GetCurrentThreadId() == owner_thread) {
            // Observe the native view copy itself: even inside the drawing
            // call a later HUD pass can replace the global current camera.
            gameplay_publication = publication_count != before_publication;
            SceneState state;
            ready = backend.Read(state);
            observed_pause_mask = ready ? state.pause_mask : UINT32_MAX;
        }
        return result;
    } catch (...) {
        gz_stop(21, "Photo native: scene callback failed");
    }
}
void begin_photo(std::unique_lock<std::recursive_mutex> &lock, const SceneState &state) {
    // The game normally allocates this effect only for cameras
    // that request DOF. Photo mode may borrow an otherwise empty
    // controller, through the same verified init/free routines.
    const char *enable = std::getenv("GZ_PHOTO_INIT_DOF");
    if (ready && state.pause_mask == 0 && dof_init_profile && (!enable || *enable != '0') &&
        !dof_owner) {
        auto *owner = image + 0xa18e80;
        void *context{};
        if (get(owner, 0xcf8, context) && context && writable(owner, 0xd18)) {
            lock.unlock();
            const bool made = photo_dof_resources.Acquire(
                {reinterpret_cast<std::byte *>(owner), 0xd18},
                [](void *p) { reinterpret_cast<Render>(image + 0x12a1c0)(p); },
                [](void *p) { reinterpret_cast<Render>(image + 0x12a310)(p); });
            lock.lock();
            if (made)
                dof_owner = owner;
            gz_log("Photo DOF: optional native resources initialized=%d", int(made));
        }
    }
    const bool hud_available = current_hud() != nullptr;
    PhotoSettings settings;
    settings.hide_hud = hud_available;
    const bool spots = game_lights_profile && game_lights_available;
    const bool entered =
        ready && state.pause_mask == 0 &&
        coordinator.Begin(PhotoPauseMask, settings,
                          PhotoCapabilities{dof_owner != nullptr,
                                            game_lights_available ||
                                                (lighting_helpers && lighting_owner != nullptr),
                                            hud_available, spots});
    entry_notice = entered ? 1 : 2;
    gz_log("Photo native: entry %s ready=%d pause=%x reason=%s owner=%lu scheduler=%lu",
           entered ? "accepted" : "refused", int(ready.load()), state.pause_mask,
           backend.AcquisitionReason(), static_cast<unsigned long>(owner_thread),
           static_cast<unsigned long>(GetCurrentThreadId()));
}
ABI uintptr_t scheduler(void *manager) {
    try {
        if (manager == image + 0xa2b510 && GetCurrentThreadId() == owner_thread) {
            // Keep the radio hidden through the frame's draw; restore when the photo ends.
            std::unique_lock lock{scene_mutex};
            if (!photo_channel.Read().active)
                radio_display.Release();
            // Consume an already validated entry at the simulation boundary. Waiting
            // until after original_scheduler advances aircraft/effects one extra tick.
            // Menu-close requests published by Tick below still use the same checks.
            SceneState state;
            if (!Active() && !exit_requested.load() && toggle_requested.load() &&
                backend.Read(state) && state.pause_mask == 0 && toggle_requested.exchange(false)) {
                ready = true;
                clear_input();
                begin_photo(lock, state);
            }
        }
    } catch (...) {
        gz_stop(21, "Photo native: scene entry callback failed");
    }
    const auto result = original_scheduler(manager);
    if (manager == image + 0xa2b510 && GetCurrentThreadId() == owner_thread)
        GzPauseMenu::Tick();
    std::unique_lock lock{scene_mutex};
    if (manager != image + 0xa2b510 || GetCurrentThreadId() != owner_thread)
        return result;
    try {
        SceneState state;
        ready = backend.Read(state);
        observed_pause_mask = ready ? state.pause_mask : UINT32_MAX;
        const bool closing = exit_requested.exchange(false) || (!ready && Active());
        if (closing) {
            if (Active())
                gz_log("Photo native: closing requested=%d ready=%d publication=%d pause=%x "
                       "camera_revision=%llu",
                       int(exit_requested.load()), int(ready.load()), int(gameplay_publication),
                       state.pause_mask, static_cast<unsigned long long>(camera_revision.load()));
            toggle_requested = false;
            coordinator.End();
            clear_input();
        }
        if (!closing && toggle_requested.exchange(false)) {
            clear_input();
            if (Active())
                coordinator.End();
            else {
                begin_photo(lock, state);
            }
        }
        if (Active()) {
            if (reset_requested.exchange(false)) {
                clear_input();
                coordinator.Reset();
            }
            CameraInput command;
            float seconds;
            std::optional<PresetRequest> request;
            {
                std::lock_guard lock{input_mutex};
                command = input;
                seconds = input_seconds;
                input = {};
                input_seconds = 0;
                request = std::move(preset_request);
                preset_request.reset();
            }
            if (request) {
                const auto frame = photo_channel.Read();
                SceneState current;
                if (frame.active && frame.ticket == request->ticket && input_allowed &&
                    !exit_requested && backend.Read(current) && current.identity == frame.scene) {
                    command = {};
                    seconds = 0;
                    command.fov_delta = request->preset.fov - current.pose.fov;
                    if (coordinator.Advance(command, 0)) {
                        photo_channel.Edit(frame.ticket,
                                           ApplyPresetSettings(request->preset, frame.settings,
                                                               frame.capabilities));
                    }
                    command = {};
                }
            }
            coordinator.Advance(command, std::clamp(seconds, 0.f, .1f));
            static unsigned trace{};
            if (SceneState traced;
                std::getenv("GODZILLA_PHOTO_TRACE") && ++trace % 30 == 0 && backend.Read(traced)) {
                uint32_t unit_mask{};
                get(observed_unit, 0x88, unit_mask);
                gz_log("Photo trace: eye=%.1f,%.1f,%.1f fov=%.1f unit_mask=%x", traced.pose.eye.x,
                       traced.pose.eye.y, traced.pose.eye.z, traced.pose.fov, unit_mask);
            }
            if (!Active())
                gz_log("Photo native: session ended: %s pause_revision=%llu camera_revision=%llu",
                       coordinator.FailureReason(),
                       static_cast<unsigned long long>(pause_revision.load()),
                       static_cast<unsigned long long>(camera_revision.load()));
        }
        const auto frame = photo_channel.Read();
        SceneState display_state;
        if (radio_display.Enabled() && frame.active && frame.settings.hide_hud &&
            frame.capabilities.hud && backend.Read(display_state) &&
            display_state.identity == frame.scene)
            radio_display.Apply();
        static uint64_t radio_logged_ticket{};
        if (radio_display.Count() && radio_logged_ticket != frame.ticket) {
            radio_logged_ticket = frame.ticket;
            gz_log(
                "Photo radio: opt-in display lease acquired %zu fields; visual validation pending",
                radio_display.Count());
        }
        return result;
    } catch (...) {
        gz_stop(21, "Photo native: scene scheduling callback failed");
    }
}
ABI uintptr_t draw_dof(void *owner) {
    std::unique_lock lock{scene_mutex};
    if (owner == image + 0xa18e80 && photo_dof_resources.Owned() && !Active()) {
        lock.unlock();
        const bool released = photo_dof_resources.Release(
            [](void *p) { reinterpret_cast<Render>(image + 0x12a310)(p); });
        lock.lock();
        dof_owner = nullptr;
        gz_log("Photo DOF: optional native resources released=%d", int(released));
    }
    if (std::getenv("GZ_PHOTO_PROBE") && gz_pc_gameplay()) {
        static void *seen[16]{};
        static unsigned count{};
        if (count < 16 && std::find(seen, seen + count, owner) == seen + count) {
            seen[count++] = owner;
            void *resource{}, *component{};
            uintptr_t table{};
            get(owner, 0xcf0, resource);
            get(owner, 0xd10, component);
            get(component, 0, table);
            gz_log("Photo DOF probe: owner=%p resource=%p component=%p table=%llx owner_thread=%d",
                   owner, resource, component, (unsigned long long)(table - uintptr_t(image)),
                   int(GetCurrentThreadId() == owner_thread));
        }
    }
    const auto frame = photo_channel.Read();
    SceneState current;
    void *component{};
    void *resource{};
    uint64_t table{};
    const bool valid =
        GetCurrentThreadId() == owner_thread && backend.Read(current) &&
        get(owner, 0xcf0, resource) && resource && get(owner, 0xd10, component) &&
        get(component, 0, table) &&
        (table == uintptr_t(image + 0x692870) || table == uintptr_t(image + 0x6928d0) ||
         table == uintptr_t(image + 0x692990));
    if (valid && !frame.active && !dof_owner)
        dof_owner = owner;
    if (!valid || owner != dof_owner || !frame.active || frame.scene != current.identity ||
        !frame.capabilities.dof) {
        lock.unlock();
        return original_dof(owner);
    }
    const size_t component_size = table == uintptr_t(image + 0x692870) ? 0x80 : 0xd0;
    if (!writable(owner, 0xd18) || !writable(component, component_size)) {
        lock.unlock();
        return original_dof(owner);
    }
    lock.unlock();
    return DrawWithNativeDof({static_cast<std::byte *>(owner), 0xd18},
                             {static_cast<std::byte *>(component), component_size},
                             uintptr_t(image), frame.settings.dof, true,
                             [&] { return original_dof(owner); });
}
struct GamePoolSnapshot {
    void *manager{};
    std::array<void *, 128> objects{};
    size_t count{}, free{};
    GameLightPoolView View() const {
        return {manager, {objects.data(), count}, free};
    }
};
bool game_pool_snapshot(void *owner, GamePoolSnapshot &pool) {
    void *expected{};
    uintptr_t begin{}, end{}, unused_begin{}, unused_end{};
    if (!get(image, 0xa41f90, expected) || expected != owner || !owner || !get(owner, 8, begin) ||
        !get(owner, 0x10, end) || !get(owner, 0x28, unused_begin) ||
        !get(owner, 0x30, unused_end) || end < begin || unused_end < unused_begin ||
        (end - begin) % 8 || (unused_end - unused_begin) % 8 || (end - begin) / 8 > 128 ||
        (unused_end - unused_begin) / 8 > 128)
        return false;
    pool.manager = owner;
    pool.count = (end - begin) / 8;
    pool.free = (unused_end - unused_begin) / 8;
    if (pool.count + pool.free > 128)
        return false;
    return !pool.count ||
           read(reinterpret_cast<void *>(begin), pool.objects.data(), pool.count * 8);
}
std::span<std::byte> game_light_bytes(void *object) {
    return writable(object, 0x84) ? std::span(static_cast<std::byte *>(object), size_t(0x84))
                                  : std::span<std::byte>{};
}
ABI uintptr_t update_game_lights(void *owner, void *context) {
    if (!gz_pc_gameplay() && !game_light_lease.Owned())
        return original_game_lights(owner, context);
    if (GetCurrentThreadId() == owner_thread) {
        std::unique_lock lock{scene_mutex};
        GamePoolSnapshot pool;
        SceneState state;
        const auto frame = photo_channel.Read();
        const char *option = std::getenv("GZ_PHOTO_GAME_LIGHTS");
        const bool pool_valid = game_pool_snapshot(owner, pool);
        const bool supported = (!option || *option != '0') && game_lights_profile && pool_valid &&
                               backend.Inspect(state, false);
        if (!frame.active || !supported || frame.scene != state.identity) {
            if (game_light_lease.Owned()) {
                game_light_lease.Release(pool.View(), game_light_bytes);
                gz_log("Photo game lights: released owned pool slots");
            }
            if (!frame.active)
                game_lights_available = supported && pool.free >= 3;
        } else if (frame.capabilities.lights) {
            const bool had_slots = game_light_lease.Owned();
            lock.unlock();
            const bool applied = game_light_lease.Apply(
                pool.View(), frame.settings.lights, light_api,
                [](void *manager) {
                    using Allocate = void *(ABI *)(void *);
                    return reinterpret_cast<Allocate>(image + 0x180f80)(manager);
                },
                game_light_bytes);
            lock.lock();
            if (!applied) {
                GamePoolSnapshot fresh;
                if (game_pool_snapshot(owner, fresh))
                    game_light_lease.Release(fresh.View(), game_light_bytes);
                auto capabilities = frame.capabilities;
                capabilities.lights = false;
                photo_channel.Capabilities(frame.ticket, capabilities);
                game_lights_available = false;
                gz_log("Photo game lights: pool unavailable; original lights retained");
            } else if (!had_slots && game_light_lease.Owned())
                gz_log("Photo game lights: acquired three native pool slots, ticket=%llu",
                       (unsigned long long)frame.ticket);
        }
    }
    const auto result = original_game_lights(owner, context);
    if (GetCurrentThreadId() == owner_thread && game_light_lease.Owned()) {
        std::lock_guard lock{scene_mutex};
        GamePoolSnapshot pool;
        if (game_pool_snapshot(owner, pool))
            game_light_lease.Remember(pool.View(), game_light_bytes);
    }
    if (std::getenv("GZ_PHOTO_PROBE") && gz_pc_gameplay()) {
        std::lock_guard lock{scene_mutex};
        static std::vector<std::pair<void *, bool>> seen;
        const std::pair key{owner, Active()};
        if (seen.size() < 24 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            uintptr_t used_begin{}, used_end{}, free_begin{}, free_end{}, visible_begin{},
                visible_end{};
            void *expected{};
            SceneState current;
            get(owner, 8, used_begin);
            get(owner, 0x10, used_end);
            get(owner, 0x28, free_begin);
            get(owner, 0x30, free_end);
            get(owner, 0x48, visible_begin);
            get(owner, 0x50, visible_end);
            get(image, 0xa41f90, expected);
            gz_log("Photo game lights probe: owner=%p expected=%p used=%llu free=%llu visible=%llu "
                   "photo=%d scene=%d thread=%lu",
                   owner, expected, (unsigned long long)((used_end - used_begin) / 8),
                   (unsigned long long)((free_end - free_begin) / 8),
                   (unsigned long long)((visible_end - visible_begin) / 8), int(Active()),
                   int(backend.Inspect(current, false)), (unsigned long)GetCurrentThreadId());
        }
    }
    return result;
}
ABI uint32_t upload_lights(void *owner, void *context, const float *matrix) {
    PhotoFrame frame;
    bool candidate{};
    {
        std::lock_guard lock{scene_mutex};
        SceneState current;
        uint32_t count{};
        void *points{};
        void *directions{};
        if (std::getenv("GZ_PHOTO_PROBE") && gz_pc_gameplay()) {
            static void *seen[16]{};
            static unsigned n{};
            if (n < 16 && std::find(seen, seen + n, owner) == seen + n) {
                seen[n++] = owner;
                get(owner, 0x4120, count);
                get(owner, 0x4258, points);
                get(owner, 0x4260, directions);
                gz_log("Photo lights probe: owner=%p count=%u points=%p directions=%p scene=%d",
                       owner, count, points, directions, int(backend.Inspect(current, false)));
            }
        }
        candidate = lighting_helpers && !game_lights_available && backend.Inspect(current, false) &&
                    get(owner, 0x4120, count) && count <= 128 && get(owner, 0x4258, points) &&
                    points && get(owner, 0x4260, directions) && directions;
        frame = photo_channel.Read();
        if (candidate && !frame.active && !lighting_owner && count <= 61)
            lighting_owner = owner;
        candidate = candidate && owner == lighting_owner && frame.active &&
                    frame.capabilities.lights && frame.scene == current.identity;
    }
    if (!candidate ||
        std::none_of(frame.settings.lights.begin(), frame.settings.lights.end(),
                     [](const PhotoLight &light) { return light.enabled && light.intensity > 0; }))
        return original_lighting(owner, context, matrix);
    struct Buffers {
        NativeLightPass pass;
        alignas(16) std::array<std::byte, NativeLightPass::RendererSize> source;
        bool busy{};
    };
    thread_local std::unique_ptr<Buffers> buffers;
    try {
        if (!buffers)
            buffers = std::make_unique<Buffers>();
        if (buffers->busy)
            return original_lighting(owner, context, matrix);
        struct Release {
            bool &busy;
            ~Release() {
                busy = false;
            }
        } release{buffers->busy};
        buffers->busy = true;
        if (!read(owner, buffers->source.data(), buffers->source.size()))
            return original_lighting(owner, context, matrix);
        const auto status =
            buffers->pass.Prepare(buffers->source, frame.settings.lights, light_api);
        if (status == NativeLightStatus::Prepared)
            return original_lighting(buffers->pass.Snapshot().data(), context, matrix);
        if (status != NativeLightStatus::NoLights) {
            auto capabilities = frame.capabilities;
            capabilities.lights = false;
            photo_channel.Capabilities(frame.ticket, capabilities);
            gz_log("Photo native lights: unavailable for this session (conversion/capacity=%d); "
                   "original light list retained",
                   int(status));
        }
        return original_lighting(owner, context, matrix);
    } catch (...) {
        gz_stop(21, "Photo native: light buffer preparation failed");
    }
}
bool light_helpers_match() {
    if (image_size < 0xa57d10)
        return false;
    for (const auto &profile :
         std::array<std::array<uint64_t, 3>, 4>{{{0x4b4bd0, 451, 0x7d709897dbf80576ull},
                                                 {0x49d7b0, 76, 0x886bb7d2e958d8a6ull},
                                                 {0x4c1650, 278, 0xa1cd860979527d1bull},
                                                 {0x2e6b30, 4515, 0xda890059a83e60e3ull}}}) {
        std::vector<std::byte> bytes(profile[1]);
        if (!read(image + profile[0], bytes.data(), bytes.size()) ||
            GzNative::HookSignature(bytes) != profile[2])
            return false;
    }
    uint64_t point{}, spot{};
    return get(image, 0x6a1530, point) && get(image, 0x6a1428, spot) &&
           point == uintptr_t(image + 0xa57c90) && spot == uintptr_t(image + 0xa57cf0);
}
bool radio_helpers_match() {
    for (const auto &profile :
         std::array<std::array<uint64_t, 2>, 9>{{{0x10310, 0x95005c877799d34full},
                                                 {0x10390, 0x6d4bcbca5f914f16ull},
                                                 {0x1049b, 0x68791f393421e1eaull},
                                                 {0x10575, 0xd04a7cf403e583bcull},
                                                 {0x1d770, 0xef087b7341389f41ull},
                                                 {0x1d810, 0x852c3038a37abf3bull},
                                                 {0x41f80, 0xf8218d453fdfabd2ull},
                                                 {0x496d0, 0x6a6507f3b81fa6e0ull},
                                                 {0x4c3d0, 0x4cf5abfee594ab92ull}}}) {
        std::array<std::byte, 96> bytes{};
        if (!read(image + profile[0], bytes.data(), bytes.size()) ||
            GzNative::HookSignature(bytes) != profile[1])
            return false;
    }
    for (const auto &binding :
         std::array<std::array<uintptr_t, 2>, 3>{{{RadioTable + 0x10, 0x10390},
                                                  {RadioTextTable + 0x28, 0x41f80},
                                                  {RadioModelTable + 0x28, 0x4bff0}}}) {
        uintptr_t target{};
        if (!get(image, binding[0], target) || target != uintptr_t(image + binding[1]))
            return false;
    }
    return true;
}
} // namespace
void Configure(void *base, uint64_t size) {
    const char *option = std::getenv("GZ_PHOTO_NATIVE");
    if ((option && std::strcmp(option, "1")) || configured || uintptr_t(base) != GZ_IMAGE_BASE ||
        size < 0xa2b600)
        return;
    image = static_cast<unsigned char *>(base);
    image_size = size;
    collision_profile =
        GzNative::HookSignature({reinterpret_cast<const std::byte *>(image + 0x2d8890), 551}) ==
            0xfc768205f19d2714ull &&
        GzNative::HookSignature({reinterpret_cast<const std::byte *>(image + 0x37f830), 158}) ==
            0x954e33c8aa3bfd2cull;
    gz_log("Photo collision: native geometry query profile=%d", int(collision_profile));
    radio_profile_verified = radio_helpers_match();
    lighting_helpers =
        light_helpers_match(); // Includes consumer body: check before patching its entry.
    dof_init_profile =
        GzNative::HookSignature({reinterpret_cast<const std::byte *>(image + 0x12a1c0), 96}) ==
            0x95c017c2c1cef654ull &&
        GzNative::HookSignature({reinterpret_cast<const std::byte *>(image + 0x12a310), 96}) ==
            0x884ddd5ee93dbec2ull;
    game_lights_profile =
        lighting_helpers &&
        GzNative::HookSignature({reinterpret_cast<const std::byte *>(image + 0x180f80), 96}) ==
            0x7431bc0a7339df1dull &&
        GzNative::HookSignature({reinterpret_cast<const std::byte *>(image + 0x182130), 96}) ==
            0x7a82b7abd4e4a058ull;
    struct Spec {
        GzNative::FunctionHook *hook;
        size_t at, displaced;
        uint64_t hash;
        void *callback;
    };
    const Spec specs[] = {
        {&destroy_hook, 0x18c660, 6, 0xe047ad4c03a29832ull, reinterpret_cast<void *>(destroy)},
        {&pause_hook, 0x113f70, 6, 0x676620f5e060099aull, reinterpret_cast<void *>(pause_store)},
        {&unit_hook, 0x112a10, 6, 0xe44d5ebc9d85c4dfull, reinterpret_cast<void *>(unit_store)},
        {&look_hook, 0x14a130, 12, 0x711e28ac3abfbec3ull, reinterpret_cast<void *>(look)},
        {&project_hook, 0x14a160, 8, 0x2598cabaa086ca2dull, reinterpret_cast<void *>(project)},
        {&dof_hook, 0x12a430, 6, 0xe602e4f9b239bc4cull, reinterpret_cast<void *>(draw_dof)},
        {&scheduler_hook, 0x112f60, 6, 0x94a16e6aaeb523b0ull, reinterpret_cast<void *>(scheduler)},
        {&lighting_hook, 0x2e6b30, 6, 0x83a2c1e9fe47c528ull,
         reinterpret_cast<void *>(upload_lights)},
        {&game_lights_hook, 0x1819d0, 6, 0xdb3310ac2a09ec8cull,
         reinterpret_cast<void *>(update_game_lights)},
        {&hud_hook, 0x1b42e0, 7, 0xeea969c839220bbeull, reinterpret_cast<void *>(draw_hud)},
        {&view_hook, 0x200430, 7, 0xabbb969c0283c725ull, reinterpret_cast<void *>(copy_view)},
        {&group_hook, 0x1139c0, 10, 0x945246e49c925e43ull, reinterpret_cast<void *>(draw_group)},
        // Last: its entry lies inside the look/project signature windows.
        {&render_hook, 0x14a190, 9, 0xc9452a83db968609ull, reinterpret_cast<void *>(render)}};
    size_t installed{};
    for (const auto &spec : specs) {
        const auto result = spec.hook->Install(
            {image + spec.at, size - spec.at, 96, spec.displaced, spec.hash}, spec.callback);
        if (result != GzNative::HookResult::Installed) {
            while (installed)
                if (specs[--installed].hook->Remove() != GzNative::HookResult::Removed)
                    gz_stop(21, "Photo native: hook rollback failed");
            gz_log("Photo native: signature/relay unavailable at %zx; native mode disabled",
                   spec.at);
            return;
        }
        ++installed;
    }
    original_render = render_hook.Original<Render>();
    original_destroy = destroy_hook.Original<Render>();
    original_pause_store = pause_hook.Original<Store>();
    original_unit_store = unit_hook.Original<Store>();
    original_look = look_hook.Original<Look>();
    original_project = project_hook.Original<Project>();
    original_dof = dof_hook.Original<Render>();
    original_scheduler = scheduler_hook.Original<Render>();
    original_lighting = lighting_hook.Original<LightUpload>();
    original_game_lights = game_lights_hook.Original<GameLightsUpdate>();
    original_hud = hud_hook.Original<HudDraw>();
    original_view = view_hook.Original<ViewCopy>();
    original_group = group_hook.Original<GroupDraw>();
    light_api = {reinterpret_cast<NativeLightApi::PackMatrix>(image + 0x49d7b0),
                 reinterpret_cast<NativeLightApi::Collect>(image + 0x4b4bd0),
                 uintptr_t(image + 0xa57c90), uintptr_t(image + 0xa57cf0), lighting_helpers};
    configured = true;
    gz_log("Photo native: EXPERIMENTAL bridges installed; waiting for scene observation; light "
           "helper profile=%d radio display profile=%d",
           int(lighting_helpers), int(radio_profile_verified));
}
bool Available() {
    return configured && ready && input_allowed && observed_pause_mask.load() == 0;
}
bool Active() {
    return photo_channel.Read().active;
}
bool InputFocused() {
    return input_allowed.load();
}
std::optional<Pose> CameraPose(uint64_t ticket) {
    std::lock_guard lock{scene_mutex};
    SceneState state;
    const auto frame = photo_channel.Read();
    if (!frame.active || (ticket && frame.ticket != ticket) || !backend.Inspect(state, false) ||
        state.identity != frame.scene)
        return {};
    return state.pose;
}
bool QueuePreset(uint64_t ticket, const Preset &preset) {
    if (!Valid(preset))
        return false;
    std::lock_guard lock{scene_mutex};
    const auto frame = photo_channel.Read();
    if (!input_allowed || exit_requested || !frame.active || frame.ticket != ticket)
        return false;
    std::lock_guard input_lock{input_mutex};
    input = {};
    input_seconds = 0;
    reset_requested = false;
    preset_request = PresetRequest{ticket, preset};
    return true;
}
const char *UnavailableReason() {
    if (!configured)
        return "Modo foto nao habilitado nesta abertura.";
    if (!input_allowed)
        return "Volte para a janela do jogo para abrir o modo foto.";
    if (!gz_pc_gameplay())
        return "Abra o modo foto durante uma fase, fora da pausa.";
    if (!ready)
        return "Modo foto: camera da fase ainda nao reconhecida. Diagnostico salvo no registro.";
    if (observed_pause_mask.load() != 0)
        return "Saia da pausa do jogo e pressione F10 durante a fase.";
    return "Modo foto: solicitando entrada...";
}
const char *TakeNotice() {
    switch (entry_notice.exchange(0)) {
    case 1:
        return "Modo foto aberto. Tab alterna painel e camera; F12 fotografa; Esc sai.";
    case 2:
        return "Nao foi possivel abrir o modo foto nesta cena. Motivo salvo no registro.";
    case 3:
        return UnavailableReason();
    default:
        return nullptr;
    }
}
void Toggle() {
    if (input_allowed && !exit_requested && (Available() || Active()))
        toggle_requested = true;
    else if (configured) {
        entry_notice = 3;
        std::lock_guard lock{scene_mutex};
        void *active{};
        void *published{};
        uintptr_t table{};
        float projection[4]{};
        get(image, 0x79ae58, active);
        get(image, 0xa271f0, published);
        get(active, 0, table);
        get(active, 0x3c0 + 0x298, projection);
        gz_log(
            "Photo native: entry rejected: %s owner=%lu caller=%lu observations=%u active=%p "
            "observed=%p table=%llx published=%p projection=%g/%g/%g/%g publication=%d count=%llu",
            UnavailableReason(), static_cast<unsigned long>(owner_thread),
            static_cast<unsigned long>(GetCurrentThreadId()), observations, active, observed_unit,
            static_cast<unsigned long long>(table), published, projection[0], projection[1],
            projection[2], projection[3], int(gameplay_publication),
            static_cast<unsigned long long>(publication_count));
    }
}
void Exit() {
    if (configured) {
        gz_log("Photo native: exit requested active=%d", int(Active()));
        exit_requested = true;
        toggle_requested = false;
        clear_input();
    }
}
void Focus(bool value) {
    const char *lease = std::getenv("GZ_TEST_INPUT_FILE");
    const char *background = std::getenv("GZ_TEST_PHOTO_BACKGROUND");
    const bool isolated_test = lease && *lease && background && !std::strcmp(background, "1");
    input_allowed = value || isolated_test;
    if (!input_allowed.load())
        Exit();
}
void Motion(float right, float up, float forward, float yaw, float pitch, float seconds,
            float speed) {
    if (!input_allowed || !Active() || !std::isfinite(seconds) || seconds < 0 || seconds > .1f)
        return;
    for (float value : {right, up, forward, yaw, pitch, speed})
        if (!std::isfinite(value))
            return;
    if (speed < 0 || speed > 1e6f)
        return;
    std::lock_guard lock{input_mutex};
    input.right = right;
    input.up = up;
    input.forward = forward;
    input.yaw += yaw;
    input.pitch += pitch;
    input.speed = speed;
    input_seconds = std::min(.1f, input_seconds + seconds);
}
void Lens(float delta) {
    if (Active() && std::isfinite(delta)) {
        std::lock_guard lock{input_mutex};
        input.fov_delta = std::clamp(input.fov_delta + delta, -95.f, 95.f);
    }
}
void Roll(float delta) {
    if (input_allowed && Active() && std::isfinite(delta)) {
        std::lock_guard lock{input_mutex};
        input.roll += delta;
    }
}
void Reset() {
    if (input_allowed && Active())
        reset_requested = true;
}
} // namespace GzPhotoNative
