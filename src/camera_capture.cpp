// Adapter at the native camera's CPU conversion boundary.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera_capture.h"
#include "native_hook.h"
#include "gpu.h"
extern "C" {
#include "gz.h"
}
#include <cstdlib>
#include <cstring>
#include <windows.h>
namespace GzCameraCapture {
namespace {
GzNative::FunctionHook capture_hook, convert_hook;
using Capture = int(ABI *)(int, int, void *, int);
using Convert = int(ABI *)(void *, const void *, const void *);
thread_local const void *camera_source;
int ABI capture(int area, int slot, void *source, int flags) {
    const auto previous = camera_source;
    camera_source = source;
    gz_log("Camera capture: area=%d slot=%d source=%p flags=%d", area, slot, source, flags);
    const auto result = capture_hook.Original<Capture>()(area, slot, source, flags);
    camera_source = previous;
    return result;
}
int ABI convert(void *dest, const void *source, const void *descriptor) {
    // Other texture conversions continue through the unmodified function.
    if (source && source == camera_source && descriptor) {
        unsigned dimensions[3]{};
        SIZE_T got{};
        if (ReadProcessMemory(GetCurrentProcess(), static_cast<const char *>(descriptor) + 8,
                              dimensions, sizeof(dimensions), &got) &&
            got == sizeof(dimensions) && dimensions[0] == 480 && dimensions[1] == 270 &&
            dimensions[2] == 1) {
            if (gz_gpu_read_image_rgba(source, 480, 270, dest, 480ull * 270 * 4)) {
                gz_log("Camera capture: GPU image copied before CPU conversion");
                return 0;
            }
            gz_log("Camera capture: no compatible GPU image; original conversion retained");
        }
    }
    return convert_hook.Original<Convert>()(dest, source, descriptor);
}
} // namespace
void Configure(void *base, uint64_t size) {
    const auto setting = std::getenv("GZ_CAMERA_READBACK");
    if ((setting && !std::strcmp(setting, "0")) || !base || size < 0x359926)
        return;
    auto *image = static_cast<unsigned char *>(base);
    if (GzNative::HookSignature({reinterpret_cast<const std::byte *>(image + 0x16eba0), 64}) !=
            0x165103b0f4d88e6bull ||
        GzNative::HookSignature({reinterpret_cast<const std::byte *>(image + 0x359830), 246}) !=
            0xaf7b7fb88ae647ddull) {
        gz_log("Camera capture: unsupported native profile; disabled");
        return;
    }
    if (convert_hook.Install({image + 0x359830, size - 0x359830, 246, 6, 0xaf7b7fb88ae647ddull},
                             reinterpret_cast<void *>(convert)) != GzNative::HookResult::Installed)
        return;
    if (capture_hook.Install({image + 0x16eba0, size - 0x16eba0, 64, 8, 0x165103b0f4d88e6bull},
                             reinterpret_cast<void *>(capture)) !=
        GzNative::HookResult::Installed) {
        convert_hook.Remove();
        return;
    }
    gz_log("Camera capture: verified GPU readback enabled (GZ_CAMERA_READBACK=0 disables)");
}
} // namespace GzCameraCapture
