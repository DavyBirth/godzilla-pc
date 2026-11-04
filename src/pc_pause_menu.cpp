// Native pause/photo handoff, gated by the loaded script's marker.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_pause_menu.h"
#include "pc_photo_native_scene.h"
#include "native_hook.h"
extern "C" {
#include "gz.h"
}
#include <windows.h>
#include <cstring>
#include <cstdlib>
#include <array>
#include <algorithm>

namespace GzPauseMenu {
namespace {
unsigned char *image{};
constexpr uint32_t Marker = 0x475a5048;
constexpr std::array<int, 5> Order{0, 4, 1, 2, 3};
using Update = void(ABI *)(void *);
using Script = void(ABI *)(void *, unsigned, unsigned);
Update original{};
Script script{};
GzNative::FunctionHook hook;
struct Pending {
    void *mission{};
    uint32_t handle{};
    ULONGLONG deadline{};
} pending;
template <class T> T get(const void *p, size_t offset) {
    T value{};
    SIZE_T got{};
    if (p)
        ReadProcessMemory(GetCurrentProcess(), static_cast<const char *>(p) + offset, &value,
                          sizeof(value), &got);
    return got == sizeof(value) ? value : T{};
}
template <class T> void set(void *p, size_t offset, T value) {
    std::memcpy(static_cast<char *>(p) + offset, &value, sizeof(value));
}
bool mission_valid(void *p, uint32_t handle) {
    return p && get<void *>(image, 0x6c3568) == p &&
           get<uintptr_t>(p, 0) == uintptr_t(image + 0x66e480) && get<uint32_t>(p, 0xb0) == handle;
}
// The candidate script's opening entry (header slot 0x38) starts with the
// marker command appended by tools/patch_pause_photo.py; check the loaded
// script itself (selector+0x2f8 is its base) rather than VM variables.
bool candidate_script(void *selector) {
    auto *base = get<unsigned char *>(selector, 0x2f8);
    const auto offset = get<int32_t>(base, 0x38);
    const uint32_t expected[6]{0, 24, 11, 0, 0x401e, Marker};
    if (std::getenv("GZ_PHOTO_PROBE")) {
        static uint64_t last{};
        auto *pc = get<unsigned char *>(selector, 0x320);
        const uint64_t key = uint64_t(get<uint16_t>(selector, 0x300)) << 32 |
                             get<uint16_t>(selector, 0x302) << 16 | get<uint16_t>(selector, 0x304);
        if (key != last) {
            last = key;
            gz_log("PC pause probe: script %x/%x/%x pc=+%llx", get<uint16_t>(selector, 0x300),
                   get<uint16_t>(selector, 0x302), get<uint16_t>(selector, 0x304),
                   (unsigned long long)(pc && base ? pc - base : 0));
        }
    }
    for (int i = 0; i < 6; ++i)
        if (!base || offset <= 0 || get<uint32_t>(base, size_t(offset) + 4 * i) != expected[i])
            return false;
    return true;
}
ABI void update(void *self) {
    auto *selector = get<void *>(self, 0x3d8);
    if (std::getenv("GZ_PHOTO_PROBE")) {
        static ULONGLONG last{};
        const auto now = GetTickCount64();
        if (now - last > 1000) {
            last = now;
            gz_log("PC pause probe: update type=%x state=%d/%d marker=%d cursor=%d pressed=%x "
                   "repeated=%x",
                   get<uint16_t>(self, 0xa0), get<short>(self, 0x98), get<short>(self, 0x9a),
                   int(candidate_script(selector)), get<int>(selector, 0x1d0),
                   get<uint32_t>(self, 0x3e0), get<uint32_t>(self, 0x3e8));
        }
    }
    if (get<uint16_t>(self, 0xa0) != 0x722 || get<short>(self, 0x98) != 6 ||
        !candidate_script(selector) || get<short>(self, 0x9a) != 1) {
        original(self);
        return;
    }
    auto *mission = get<void *>(image, 0x6c3568);
    const auto handle = get<uint32_t>(mission, 0xb0);
    const int cursor = get<int>(selector, 0x1d0);
    auto found = std::find(Order.begin(), Order.end(), cursor);
    if (!mission_valid(mission, handle) || get<short>(mission, 0x98) != 3 || found == Order.end()) {
        original(self);
        return;
    }
    const auto pressed = get<uint32_t>(self, 0x3e0);
    const auto confirm = get<uint32_t>(image, 0xa183e0 + 0xf830);
    if (confirm && (pressed & confirm) && cursor == 4) {
        // Exactly the native Voltar completion: result0, close animation,
        // substate2. The original block pulses the selection band and uses
        // the confirmation sound; the wrapper destroys/releases pause.
        pending = {mission, handle, GetTickCount64() + 10000};
        set<int>(self, 0x17ea8, 0);
        script(selector, 2, 4);
        set<short>(self, 0x9a, 2);
        gz_log("PC pause: photo selected; waiting for native pause to close");
        return;
    }
    const auto repeated = get<uint32_t>(self, 0x3e8);
    // Confirm/cancel retain the original priority over directional repeats.
    const auto cancel = get<uint32_t>(image, 0xa183e0 + 0xf834) | 0x100000u;
    if (!(pressed & (confirm | cancel)) && (repeated & 0x50)) {
        const auto index = found - Order.begin();
        const int next = Order[(index + ((repeated & 0x10) ? 4 : 1)) % Order.size()];
        set<int>(selector, 0x1d0, next);
        script(selector, 1, 4);
        set<uint32_t>(self, 0x3e8, repeated & ~0x50u);
        original(self);
        set<uint32_t>(self, 0x3e8, repeated);
        if (std::getenv("GZ_PHOTO_PROBE"))
            gz_log("PC pause probe: cursor %d -> %d -> %d repeats=%x", cursor, next,
                   get<int>(selector, 0x1d0), repeated);
    } else
        original(self);
}
} // namespace

void Tick() {
    if (!pending.mission)
        return;
    if (GetTickCount64() >= pending.deadline || !GzPhotoNative::InputFocused() ||
        !mission_valid(pending.mission, pending.handle)) {
        gz_log("PC pause: pending photo cancelled (scene changed or timeout)");
        pending = {};
        return;
    }
    const auto state = get<short>(pending.mission, 0x98);
    if (state != 2 && state != 3) {
        pending = {};
        return;
    }
    if (state == 2 && get<uint32_t>(image, 0xa2b510 + 0x350) == 0 && GzPhotoNative::Available()) {
        pending = {};
        GzPhotoNative::Toggle();
        gz_log("PC pause: native pause released; photo entry queued");
    }
}
void Configure(void *base, uint64_t size) {
    const auto *enabled = std::getenv("GZ_PAUSE_PHOTO");
    if ((enabled && !std::strcmp(enabled, "0")) || !base || size < 0xa2b900)
        return;
    image = static_cast<unsigned char *>(base);
    // Both helpers belong to the pinned Godzilla 01.01 image. Verify all
    // signatures before installing; no native script alone enables routing.
    auto signature = [&](size_t offset) {
        return GzNative::HookSignature({reinterpret_cast<const std::byte *>(image + offset), 96});
    };
    if (signature(0x15340) != 0xfdfacbb90c0ac62aull ||
        signature(0x406b0) != 0x87b1d774dccccb3aull) {
        gz_log("PC pause: unsupported native profile; extension disabled");
        return;
    }
    if (hook.Install({image + 0x15340, size - 0x15340, 96, 6, 0xfdfacbb90c0ac62aull},
                     reinterpret_cast<void *>(update)) != GzNative::HookResult::Installed) {
        gz_log("PC pause: extension hook unavailable");
        return;
    }
    original = hook.Original<Update>();
    script = reinterpret_cast<Script>(image + 0x406b0);
    gz_log("PC pause: experimental photo row enabled; requires matching BP script");
}
} // namespace GzPauseMenu
