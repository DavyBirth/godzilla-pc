// Native Windows host adapter, following bbport's bbgpu.cpp architecture.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include "common/elf_info.h"
#include "common/rdtsc.h"
#include "core/libraries/libs.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/time.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderdoc.h"
#include <vk_mem_alloc.h>
#include "gpu.h"
#include "pc_overlay.h"
extern "C" {
#include "gz.h"
}

Frontend::WindowSDL *g_window{};
extern "C" void *gz_gpu_window_handle() {
    return g_window ? g_window->GetWindowInfo().render_surface : nullptr;
}
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;
namespace Libraries::GnmDriver {
void RegisterLib(Core::Loader::SymbolsResolver *);
}
namespace Libraries::VideoOut {
void RegisterLib(Core::Loader::SymbolsResolver *);
}
namespace Libraries::AvPlayer {
void RegisterLib(Core::Loader::SymbolsResolver *);
}
namespace Libraries::Ajm {
void RegisterLib(Core::Loader::SymbolsResolver *);
}
namespace Libraries::Pad {
void RegisterNative(Core::Loader::SymbolsResolver *);
}
namespace Libraries::AudioOut {
void RegisterLib(Core::Loader::SymbolsResolver *);
}
namespace Libraries::Kernel {
void RegisterEventQueue(Core::Loader::SymbolsResolver *);
}
namespace {
struct Symbol {
    std::string library, name;
    uintptr_t address;
};
std::vector<Symbol> symbols;
u32 sdk_version;
} // namespace
namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char *, const char *library, const char *, SymbolType,
                                u64 address, const char *name) {
    symbols.push_back({library, name, address});
}
} // namespace Core::Loader
namespace Core {
class Emulator {
  public:
    static void FillElfInfo(u32 sdk) {
        auto &info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = "CUSA02046";
        info.title = "Godzilla";
        info.app_ver = "01.01";
        info.sdk_ver = sdk;
        info.game_folder = std::filesystem::u8path(gz_app0());
    }
};
void MemoryManager::SetRasterizer(Vulkan::Rasterizer *value) {
    rasterizer = value;
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    gz_gpu_invalidate(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr address, u64 size) {
    return gz_memory_clamp(address, size);
}
void MemoryManager::CopySparseMemory(VAddr source, u8 *dest, u64 size) {
    if (!gz_memory_copy(source, dest, size, 0))
        std::memset(dest, 0, size);
}
bool MemoryManager::TryWriteBacking(void *dest, const void *source, u64 size) {
    return gz_memory_copy(reinterpret_cast<uintptr_t>(dest), const_cast<void *>(source), size, 1) !=
           0;
}
void MemoryManager::WaitForGpuIdle() {
    if (!liverpool || !rasterizer)
        return;
    liverpool->WaitGpuIdle();
    liverpool->SendCommand<true>([this] { rasterizer->Finish(); });
}
void AddressSpace::Protect(VAddr address, u64 size, MemoryPermission perms) {
    gz_memory_gpu_protect(address, size, True(perms & MemoryPermission::Read),
                          True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> result;
    result += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return result;
}
} // namespace Core
namespace Libraries::Kernel {
boost::asio::io_context io_context;
void KernelSignalRequest() {}
int *PS4_SYSV_ABI __Error() {
    return gz_errno();
}
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() {
    using Fn = u64(PS4_SYSV_ABI *)();
    return reinterpret_cast<Fn>(gz_kernel_resolve("libkernel", "sceKernelGetTscFrequency"))();
}
u64 PS4_SYSV_ABI sceKernelReadTsc() {
    return Common::FencedRDTSC();
}
u64 PS4_SYSV_ABI sceKernelGetProcessTime() {
    using Fn = u64(PS4_SYSV_ABI *)();
    return reinterpret_cast<Fn>(gz_kernel_resolve("libkernel", "sceKernelGetProcessTime"))();
}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() {
    using Fn = u64(PS4_SYSV_ABI *)();
    return reinterpret_cast<Fn>(gz_kernel_resolve("libkernel", "sceKernelGetProcessTimeCounter"))();
}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() {
    return 1'000'000'000;
}
s32 PS4_SYSV_ABI sceKernelIsNeoMode() {
    return 0;
}
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32 *out) {
    if (!out)
        return ORBIS_KERNEL_ERROR_EINVAL;
    *out = static_cast<s32>(sdk_version);
    return 0;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 usec) {
    using Fn = s32(PS4_SYSV_ABI *)(u32);
    return reinterpret_cast<Fn>(gz_thread_resolve("libkernel", "sceKernelUsleep"))(usec);
}
} // namespace Libraries::Kernel
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
bool IsRenderDocLoaded() {
    return false;
}
} // namespace VideoCore
extern "C" void gz_gpu_start(uint32_t sdk, int headless) {
    sdk_version = sdk;
    Core::Emulator::FillElfInfo(sdk);
    if (!headless) {
        auto guard = boost::asio::make_work_guard(Libraries::Kernel::io_context);
        std::thread([guard = std::move(guard)] { Libraries::Kernel::io_context.run(); }).detach();
    }
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    if (!headless) {
        struct WindowReady {
            std::mutex mutex;
            std::condition_variable cv;
            bool ready = false;
        };
        auto state = std::make_shared<WindowReady>();
        std::thread([state] {
            auto *window = new Frontend::WindowSDL(1280, 720, "Godzilla");
            {
                std::lock_guard lock{state->mutex};
                g_window = window;
                state->ready = true;
            }
            state->cv.notify_one();
            while (window->PollEvents())
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            GzPcOverlay::FinishSaves();
            gz_stop(0, "GPU: window closed by user");
        }).detach();
        {
            std::unique_lock lock{state->mutex};
            state->cv.wait(lock, [&] { return state->ready; });
        }
    }
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
    Libraries::Ajm::RegisterLib(&resolver);
    Libraries::Pad::RegisterNative(&resolver);
    Libraries::AudioOut::RegisterLib(&resolver);
    gz_log("GPU: %s, %zu registered symbols, SDK 0x%x",
           headless ? "symbols registered without window" : "Vulkan backend ready", symbols.size(),
           sdk);
    gz_memory_replay_gpu();
}
extern "C" uintptr_t gz_gpu_resolve(const char *library, const char *symbol) {
    for (const auto &entry : symbols)
        if (entry.library == library && entry.name == symbol)
            return entry.address;
    return 0;
}
extern "C" int gz_gpu_handle_fault(void *exception, void *address) {
    // Dispatch to the runtime's current rasterizer, without a second global owner
    // in the emulator signal layer. Match shadPS4 PageManager's read/write paths.
    auto *r = Core::Memory::Instance()->GetRasterizer();
    const auto at = reinterpret_cast<uintptr_t>(address);
    const auto bytes = gz_memory_clamp(at, 8);
    if (!r || !bytes || !r->IsMapped(at, bytes))
        return 0;
    const auto *e = static_cast<EXCEPTION_POINTERS *>(exception);
    return e->ExceptionRecord->ExceptionInformation[0] == 1 ? r->InvalidateMemory(at, bytes)
                                                            : r->ReadMemory(at, bytes);
}
extern "C" void gz_gpu_map(uintptr_t address, uint64_t size) {
    if (auto *r = Core::Memory::Instance()->GetRasterizer()) {
        r->MapMemory(address, size);
        gz_log("GPU memory mapped: 0x%llx size=0x%llx rasterizer=%p", (unsigned long long)address,
               (unsigned long long)size, r);
    }
}
extern "C" void gz_gpu_unmap(uintptr_t address, uint64_t size) {
    if (auto *r = Core::Memory::Instance()->GetRasterizer()) {
        gz_log("GPU memory unmapped: 0x%llx size=0x%llx rasterizer=%p", (unsigned long long)address,
               (unsigned long long)size, r);
        r->UnmapMemory(address, size);
    }
}
extern "C" void gz_gpu_invalidate(uintptr_t address, uint64_t size) {
    if (auto *r = Core::Memory::Instance()->GetRasterizer())
        r->InvalidateMemory(address, size);
}
extern "C" void gz_gpu_prepare_write(void *address, uint64_t size) {
    if (auto *r = Core::Memory::Instance()->GetRasterizer()) {
        const auto start = reinterpret_cast<uintptr_t>(address);
        // Invalidate each page before ReadFile writes in kernel mode (no user VEH fault).
        for (u64 offset = 0; offset < size;) {
            r->InvalidateMemory(start + offset, 1);
            offset += std::min<u64>(size - offset, 4096 - ((start + offset) & 4095));
        }
    }
}
extern "C" void gz_gpu_prepare_read(const void *address, uint64_t size) {
    auto *raster = Core::Memory::Instance()->GetRasterizer();
    const auto start = reinterpret_cast<uintptr_t>(address);
    if (!raster || !liverpool || !size || !raster->IsMapped(start, size))
        return;
    // WriteFile reads in kernel mode, bypassing the user-mode page-fault
    // handler. Flush GPU-produced backing at the guest file-I/O boundary.
    // Runtime file writes originate on guest CPU threads, not the GPU worker.
    liverpool->WaitGpuIdle();
    liverpool->SendCommand<true>([raster, start, size] {
        raster->ProcessDownloadImages();
        raster->ReadMemory(start, size);
        raster->Finish();
    });
}
extern "C" int gz_gpu_read_image_rgba(const void *source, uint32_t width, uint32_t height,
                                      void *destination, uint64_t capacity) {
    auto *raster = Core::Memory::Instance()->GetRasterizer();
    const uint64_t bytes = uint64_t(width) * height * 4;
    if (!raster || !liverpool || !source || !destination || !width || !height || width > 16384 ||
        height > 16384 || capacity < bytes)
        return 0;
    int copied = 0;
    liverpool->WaitGpuIdle();
    liverpool->SendCommand<true>([&] {
        auto &cache = raster->GetTextureCache();
        const auto id = cache.FindImageFromRange(reinterpret_cast<uintptr_t>(source), bytes, false);
        if (!id) {
            gz_log("Camera GPU: no cached image at %p", source);
            return;
        }
        auto &image = cache.GetImage(id);
        const auto &info = image.info;
        gz_log("Camera GPU: source=%p size=%ux%u pitch=%u tiled=%d format=%u modified=%d", source,
               info.size.width, info.size.height, info.pitch, int(info.props.is_tiled),
               unsigned(info.pixel_format),
               int(True(image.flags & VideoCore::ImageFlagBits::GpuModified)));
        const bool rgba = info.pixel_format == vk::Format::eR8G8B8A8Unorm ||
                          info.pixel_format == vk::Format::eR8G8B8A8Srgb;
        const bool bgra = info.pixel_format == vk::Format::eB8G8R8A8Unorm ||
                          info.pixel_format == vk::Format::eB8G8R8A8Srgb;
        if ((!rgba && !bgra) || info.size.width != width || info.size.height != height ||
            info.size.depth != 1 || info.resources.layers != 1 || info.num_samples != 1 ||
            info.props.is_depth || !True(image.flags & VideoCore::ImageFlagBits::GpuModified))
            return;
        auto &download =
            raster->GetBufferCache().GetUtilityBuffer(VideoCore::MemoryUsage::Download);
        const auto [pixels, offset] = download.Map(bytes);
        if (!pixels)
            return;
        download.Commit();
        auto &scheduler = raster->GetScheduler();
        scheduler.EndRendering();
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
        const vk::BufferImageCopy copy{
            .bufferOffset = offset,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {width, height, 1}};
        scheduler.CommandBuffer().copyImageToBuffer(
            image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, download.Handle(), copy);
        scheduler.Finish();
        if (vmaInvalidateAllocation(download.buffer.allocator, download.buffer.allocation, offset,
                                    bytes) != VK_SUCCESS)
            return;
        // Copy to owned host memory before touching protected guest memory.
        std::vector<u8> output(pixels, pixels + bytes);
        if (bgra)
            for (size_t i = 0; i < output.size(); i += 4)
                std::swap(output[i], output[i + 2]);
        const auto at = reinterpret_cast<uintptr_t>(destination);
        const bool mapped = raster->IsMapped(at, bytes);
        if (mapped && gz_memory_clamp(at, bytes) == bytes) {
            raster->InvalidateMemory(at, bytes);
            copied = gz_memory_copy(at, output.data(), bytes, 1) != 0;
        } else if (!mapped) {
            // The native camera stores its converted pixels in the image's
            // writable data segment, outside the direct/flexible memory pool.
            // Host-owned buffers are also valid CPU capture destinations.
            MEMORY_BASIC_INFORMATION region{};
            SIZE_T written{};
            if (VirtualQuery(destination, &region, sizeof(region)) && region.State == MEM_COMMIT &&
                !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                ((region.Protect & 0xff) == PAGE_READWRITE ||
                 (region.Protect & 0xff) == PAGE_WRITECOPY) &&
                bytes <= region.RegionSize - (at - reinterpret_cast<uintptr_t>(region.BaseAddress)))
                copied = WriteProcessMemory(GetCurrentProcess(), destination, output.data(), bytes,
                                            &written) &&
                         written == bytes;
        }
        gz_log("Camera GPU: destination=%p mapped=%d copied=%d", destination, int(mapped), copied);
    });
    return copied;
}
