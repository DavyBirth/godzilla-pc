// End-to-end PS4 API test: real command processing, protected memory, event, pixels.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <algorithm>
#include <SDL3/SDL.h>
#include <atomic>
#include <cstring>
#include <malloc.h>
#include <thread>
#include <windows.h>
#include "sdl_window.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "core/libraries/avplayer/avplayer_impl.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "gpu.h"
#include "pad_host.h"
extern "C" {
#include "gz.h"
#include "dialog_ui.h"
}
extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;
extern Frontend::WindowSDL *g_window;
void gz_ajm_self_test();
namespace Libraries::VideoOut {
s32 PS4_SYSV_ABI sceVideoOutDeleteFlipEvent(Libraries::Kernel::OrbisKernelEqueue, s32);
}
namespace Libraries::Kernel {
int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue *, const char *);
int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue);
int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue, OrbisKernelEvent *, int, int *,
                                     OrbisKernelUseconds *);
} // namespace Libraries::Kernel

static void require(bool condition, const char *message) {
    if (!condition)
        gz_stop(24, "GPU TEST: FAIL: %s", message);
}
static void *PS4_SYSV_ABI video_test_alloc(void *, u32 alignment, u32 size) {
    return _aligned_malloc(size, std::max<u32>(alignment, sizeof(void *)));
}
static void PS4_SYSV_ABI video_test_free(void *, void *memory) {
    _aligned_free(memory);
}
static void PS4_SYSV_ABI video_test_event(void *object, Libraries::AvPlayer::AvPlayerEvents event,
                                          s32, void *) {
    if (event == Libraries::AvPlayer::AvPlayerEvents::StateReady)
        static_cast<std::atomic<bool> *>(object)->store(true);
}
static void video_frame_lifetime_test() {
    using namespace Libraries::AvPlayer;
    std::atomic<bool> ready{};
    AvPlayerInitData init{};
    init.memory_replacement = {nullptr, video_test_alloc, video_test_free, video_test_alloc,
                               video_test_free};
    init.event_replacement = {&ready, video_test_event};
    init.num_output_video_framebuffers = 2;
    init.default_language = "eng";
    AvPlayer player(init);
    require(player.AddSource("/app0/01_op_en.mp4") == 0, "video test source");
    for (int i = 0; i < 500 && !ready.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(ready.load(), "video source ready");
    bool video_stream = false;
    for (s32 i = 0, count = player.GetStreamCount(); i < count; ++i) {
        AvPlayerStreamInfo stream{};
        require(player.GetStreamInfo(i, stream) == 0, "video stream info");
        if (stream.type == AvPlayerStreamType::Video) {
            require(player.EnableStream(i) == 0, "enable test video");
            video_stream = true;
        }
    }
    require(video_stream && player.Start() == 0, "start test video");
    AvPlayerFrameInfo held{};
    std::vector<u8> snapshot;
    int frames = 0, waiting_checks = 0;
    u64 previous_timestamp = 0;
    for (int i = 0; i < 500 && frames < 20; ++i) {
        AvPlayerFrameInfo frame{};
        if (player.GetVideoData(frame)) {
            require(!frames || frame.timestamp > previous_timestamp, "video timestamps advance");
            previous_timestamp = frame.timestamp;
            held = frame;
            ++frames;
            const size_t bytes =
                size_t(frame.details.video.width) * frame.details.video.height * 3 / 2;
            snapshot.assign(frame.p_data, frame.p_data + bytes);
        } else if (!snapshot.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            require(std::memcmp(held.p_data, snapshot.data(), snapshot.size()) == 0,
                    "false video poll preserves displayed frame while decoder runs");
            ++waiting_checks;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(frames == 20 && waiting_checks > 10,
            "video progresses with two buffers and pending polls");
    require(player.Stop() == 0, "stop test video");
    gz_log("VIDEO TEST: 20 advancing frames; %d pending polls retained complete displayed frame",
           waiting_checks);
}
static void camera_readback_test() {
    using namespace Libraries::VideoOut;
    using Alloc = s32(ABI *)(s64, s64, u64, u64, s32, s64 *);
    using Map = s32(ABI *)(void **, u64, s32, s32, s64, u64);
    using Open = s32(ABI *)(const char *, s32, u16);
    using Pread = s64(ABI *)(s32, void *, u64, s64);
    using Close = s32(ABI *)(s32);
    auto alloc =
        reinterpret_cast<Alloc>(gz_memory_resolve("libkernel", "sceKernelAllocateDirectMemory"));
    auto map = reinterpret_cast<Map>(gz_memory_resolve("libkernel", "sceKernelMapDirectMemory"));
    auto open = reinterpret_cast<Open>(gz_file_resolve("libkernel", "sceKernelOpen"));
    auto pread = reinterpret_cast<Pread>(gz_file_resolve("libkernel", "sceKernelPread"));
    auto close = reinterpret_cast<Close>(gz_file_resolve("libkernel", "sceKernelClose"));
    FILE *file{};
    // A camera scan is a GPU-produced linear 480x270 image, then a CPU file write.
    // Presentation tests alone cannot detect an all-zero host backing image.
    s64 camera_offset{};
    constexpr u64 camera_bytes = 480 * 270 * 4, camera_allocation = 0x100000;
    void *camera_memory{};
    require(alloc(0, 5056ULL << 20, camera_allocation, 0x4000, 0, &camera_offset) == 0,
            "camera readback allocation");
    require(map(&camera_memory, camera_allocation, 0x33, 0, camera_offset, 0x4000) == 0,
            "camera readback map");
    liverpool->WaitGpuIdle();
    liverpool->SendCommand<true>([&] {
        auto *raster = Core::Memory::Instance()->GetRasterizer();
        auto &cache = raster->GetTextureCache();
        BufferAttributeGroup group{};
        sceVideoOutSetBufferAttribute(&group.attrib, PixelFormat::A8B8G8R8Srgb, 1, 0, 480, 270,
                                      480);
        VideoCore::TextureCache::ImageDesc desc(group, reinterpret_cast<VAddr>(camera_memory));
        desc.type = VideoCore::TextureCache::BindingType::RenderTarget;
        desc.view_info.format = desc.info.pixel_format;
        auto id = cache.FindImage(desc);
        (void)cache.FindRenderTarget(id, desc);
        auto &image = cache.GetImage(id);
        auto &scheduler = raster->GetScheduler();
        scheduler.EndRendering();
        image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                      {});
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        scheduler.CommandBuffer().clearColorImage(
            image.GetImage(), vk::ImageLayout::eTransferDstOptimal,
            vk::ClearColorValue{std::array<float, 4>{1.f, 1.f, 0.f, 1.f}}, range);
    });
    std::vector<u32> camera_pixels(camera_bytes / 4);
    auto *converted = static_cast<u8 *>(camera_memory) + 0x80000;
    require(!gz_gpu_read_image_rgba(camera_memory, 480, 270, converted, camera_bytes - 1),
            "camera image capture rejects short destination");
    require(!gz_gpu_read_image_rgba(camera_memory, 479, 270, converted, camera_bytes),
            "camera image capture rejects mismatched dimensions");
    require(gz_gpu_read_image_rgba(camera_memory, 480, 270, converted, camera_bytes),
            "explicit GPU image capture before CPU conversion");
    require(gz_memory_copy(reinterpret_cast<uintptr_t>(converted), camera_pixels.data(),
                           camera_bytes, 0) &&
                std::all_of(camera_pixels.begin(), camera_pixels.end(),
                            [](u32 p) { return p == 0xff00ffff; }),
            "explicit camera capture preserves RGBA and alpha");
    std::fill(camera_pixels.begin(), camera_pixels.end(), 0);
    require(gz_gpu_read_image_rgba(camera_memory, 480, 270, camera_pixels.data(), camera_bytes) &&
                std::all_of(camera_pixels.begin(), camera_pixels.end(),
                            [](u32 p) { return p == 0xff00ffff; }),
            "camera capture supports writable CPU memory outside guest pools");
    void *protected_pixels =
        VirtualAlloc(nullptr, camera_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
    require(protected_pixels &&
                !gz_gpu_read_image_rgba(camera_memory, 480, 270, protected_pixels, camera_bytes),
            "camera capture refuses read-only CPU memory without changing protection");
    require(VirtualFree(protected_pixels, 0, MEM_RELEASE), "read-only capture fixture cleanup");
    // No manual download/Finish or CPU read before WriteFile: the file API
    // must synchronize the GPU source itself, as a real game save requires.
    using Write = s64(ABI *)(s32, const void *, u64);
    const auto write = reinterpret_cast<Write>(gz_file_resolve("libkernel", "sceKernelWrite"));
    const s32 camera_fd = open("/data/camera_readback_test.dat", 0x601, 0600);
    require(camera_fd >= 0 && write(camera_fd, camera_memory, camera_bytes) == camera_bytes &&
                close(camera_fd) == 0,
            "camera image saved through guest file API");
    const auto camera_path =
        std::filesystem::u8path(gz_user_dir()) / "data" / "camera_readback_test.dat";
    file = _wfopen(camera_path.c_str(), L"rb");
    require(file && std::fread(camera_pixels.data(), 1, camera_bytes, file) == camera_bytes &&
                std::fclose(file) == 0,
            "camera image file readback");
    require(std::all_of(camera_pixels.begin(), camera_pixels.end(),
                        [](u32 pixel) { return pixel == 0xff00ffff; }),
            "saved camera file contains GPU colors and opaque alpha");
    std::filesystem::remove(camera_path);
    // Legacy all-zero screenshots receive a read-only unavailable marker.
    // The saved bytes and valid camera images must remain unchanged.
    const auto legacy_folder =
        std::filesystem::u8path(gz_user_dir()) / "savedata" / "gpu_snapshot_view_test";
    std::filesystem::create_directories(legacy_folder);
    const auto legacy_host = legacy_folder / "area99_00.dat";
    const auto legacy_utf8 = legacy_folder.u8string();
    require(gz_file_mount("/gpu_snapshot_view",
                          reinterpret_cast<const char *>(legacy_utf8.c_str())) == 0,
            "legacy snapshot test mount");
    std::vector<u32> zero_camera(camera_pixels.size(), 0);
    s32 legacy_fd = open("/gpu_snapshot_view/area99_00.dat", 0x601, 0600);
    require(legacy_fd >= 0 && write(legacy_fd, zero_camera.data(), camera_bytes) == camera_bytes &&
                close(legacy_fd) == 0,
            "legacy empty image fixture");
    legacy_fd = open("/gpu_snapshot_view/area99_00.dat", 0, 0);
    require(legacy_fd >= 0 &&
                pread(legacy_fd, camera_pixels.data(), camera_bytes, 0) == camera_bytes &&
                close(legacy_fd) == 0,
            "legacy image display read");
    require(std::all_of(camera_pixels.begin(), camera_pixels.end(),
                        [](u32 pixel) { return (pixel >> 24) == 255; }),
            "legacy unavailable marker has opaque pixels");
    file = _wfopen(legacy_host.c_str(), L"rb");
    require(file && std::fread(zero_camera.data(), 1, camera_bytes, file) == camera_bytes &&
                std::fclose(file) == 0 &&
                std::all_of(zero_camera.begin(), zero_camera.end(),
                            [](u32 pixel) { return pixel == 0; }),
            "legacy marker must not modify saved file");
    legacy_fd = open("/gpu_snapshot_view/area99_00.dat", 0x601, 0600);
    std::fill(camera_pixels.begin(), camera_pixels.end(), 0xff00ffff);
    require(legacy_fd >= 0 &&
                write(legacy_fd, camera_pixels.data(), camera_bytes) == camera_bytes &&
                close(legacy_fd) == 0,
            "new capture replaces empty fixture normally");
    legacy_fd = open("/gpu_snapshot_view/area99_00.dat", 0, 0);
    require(legacy_fd >= 0 &&
                pread(legacy_fd, zero_camera.data(), camera_bytes, 0) == camera_bytes &&
                close(legacy_fd) == 0 && zero_camera == camera_pixels,
            "valid capture bypasses legacy marker");
    std::filesystem::remove(legacy_host);
    std::filesystem::remove(legacy_folder);
    gz_file_unmount("/gpu_snapshot_view");
    gz_log("GPU TEST: legacy empty image marker preserves file; valid images unchanged");
    require(reinterpret_cast<s32(ABI *)(void *, u64)>(gz_memory_resolve(
                "libkernel", "sceKernelMunmap"))(camera_memory, camera_allocation) == 0 &&
                reinterpret_cast<s32(ABI *)(s64, u64)>(
                    gz_memory_resolve("libkernel", "sceKernelReleaseDirectMemory"))(
                    camera_offset, camera_allocation) == 0,
            "camera readback cleanup");
    gz_log("GPU TEST: 480x270 GPU camera image -> host memory -> guest save file passed");
}

extern "C" void gz_gpu_self_test() {
    if (std::getenv("GZ_GPU_CAMERA_TEST_ONLY")) {
        camera_readback_test();
        return;
    }
    gz_pad_self_test();
    gz_ajm_self_test();
    // Exercise the real child panel while the Vulkan presentation thread runs.
    // SDL keyboard events take the same routing path as physical Pad buttons.
    for (const auto type : {0u, 7u, 8u}) {
        int answer = -1;
        std::thread dialog(
            [&] { answer = gz_dialog_show(L"Teste de mensagem dentro do jogo", type); });
        for (int n = 0; n < 200 && !gz_dialog_active(); ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(gz_dialog_active(), "embedded dialog opens");
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.scancode = SDL_SCANCODE_SPACE;
        require(SDL_PushEvent(&key), "dialog confirm event");
        for (int n = 0; n < 200 && gz_dialog_active(); ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(!gz_dialog_active(), "embedded dialog handles confirmation");
        key.type = SDL_EVENT_KEY_UP;
        require(SDL_PushEvent(&key), "dialog release event");
        dialog.join();
        require(answer == (type == 7   ? IDNO
                           : type == 8 ? IDCANCEL
                                       : IDOK),
                "dialog default choice result");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    gz_log("UI TEST: embedded OK, default No and default Cancel passed via SDL input");
    {
        using Call = s32(ABI *)();
        struct ErrorParam {
            s32 size, code, user, reserved;
        };
        using OpenError = s32(ABI *)(const ErrorParam *);
        auto init = reinterpret_cast<Call>(
            gz_services_resolve("libSceErrorDialog", "sceErrorDialogInitialize"));
        auto status = reinterpret_cast<Call>(
            gz_services_resolve("libSceErrorDialog", "sceErrorDialogUpdateStatus"));
        auto term = reinterpret_cast<Call>(
            gz_services_resolve("libSceErrorDialog", "sceErrorDialogTerminate"));
        auto open = reinterpret_cast<OpenError>(
            gz_services_resolve("libSceErrorDialog", "sceErrorDialogOpen"));
        require(init && status && term && open, "ErrorDialog API resolved");
        require(init() == 0 && status() == 1, "ErrorDialog initializes");
        ErrorParam param{16, s32(0x80000001), 1, 0};
        require(open(&param) == 0 && status() == 2, "ErrorDialog waits instead of auto completing");
        for (int n = 0; n < 200 && !gz_dialog_active(); ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(gz_dialog_active(), "ErrorDialog visible");
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.scancode = SDL_SCANCODE_SPACE;
        require(SDL_PushEvent(&key), "ErrorDialog confirmation event");
        for (int n = 0; n < 200 && status() == 2; ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(status() == 3 && term() == 0, "ErrorDialog confirmation completes API lifecycle");
        key.type = SDL_EVENT_KEY_UP;
        require(SDL_PushEvent(&key), "ErrorDialog release event");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        gz_log("UI TEST: ErrorDialog PS4 ABI lifecycle and confirmation passed");
    }
    {
        using Call = s32(ABI *)();
        struct Base {
            u64 size;
            char reserved[36];
            u32 magic;
        };
        struct User {
            u32 buttons, kind;
            const char *text;
            char reserved[32];
        };
        struct Param {
            Base base;
            s32 size;
            u32 mode, display, pad;
            const void *animation, *items;
            const User *user;
            const void *system;
            const u32 *error;
            const void *progress;
            void *user_data;
            const void *option;
            char reserved[24];
        };
        struct Result {
            u32 mode, result, button, pad;
            void *directory, *param, *user_data;
            char reserved[32];
        };
        static_assert(sizeof(Param) == 152 && sizeof(Result) == 72);
        using Open = s32(ABI *)(const Param *);
        using Get = s32(ABI *)(Result *);
        auto init = reinterpret_cast<Call>(
            gz_services_resolve("libSceSaveDataDialog", "sceSaveDataDialogInitialize"));
        auto status = reinterpret_cast<Call>(
            gz_services_resolve("libSceSaveDataDialog", "sceSaveDataDialogUpdateStatus"));
        auto term = reinterpret_cast<Call>(
            gz_services_resolve("libSceSaveDataDialog", "sceSaveDataDialogTerminate"));
        auto open = reinterpret_cast<Open>(
            gz_services_resolve("libSceSaveDataDialog", "sceSaveDataDialogOpen"));
        auto get = reinterpret_cast<Get>(
            gz_services_resolve("libSceSaveDataDialog", "sceSaveDataDialogGetResult"));
        require(init && status && term && open && get, "SaveDataDialog APIs resolved");
        require(init() == 0, "SaveDataDialog initialize");
        User user{1, 0, "Confirmar dados salvos?", {}};
        u32 error = 0x809f000f;
        int caller_data{}, directory{}, saved_param{};
        Param param{};
        param.base.size = sizeof(Base);
        param.size = sizeof(Param);
        param.display = 1;
        param.user = &user;
        param.error = &error;
        param.user_data = &caller_data;
        param.mode = 1;
        require(open(&param) == s32(0x80b8000f) && status() == 1,
                "unsupported list does not auto approve");
        for (u32 mode : {2u, 4u}) {
            param.mode = mode;
            require(open(&param) == 0 && status() == 2, "SaveDataDialog waits for input");
            Result result{};
            result.directory = &directory;
            result.param = &saved_param;
            require(get(&result) == s32(0x80b80005), "unfinished result rejected");
            for (int n = 0; n < 200 && !gz_dialog_active(); ++n)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            require(gz_dialog_active(), "SaveDataDialog visible");
            SDL_Event key{};
            key.type = SDL_EVENT_KEY_DOWN;
            key.key.scancode = mode == 2 ? SDL_SCANCODE_X : SDL_SCANCODE_SPACE;
            require(SDL_PushEvent(&key), "SaveDataDialog input");
            for (int n = 0; n < 200 && status() == 2; ++n)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            require(status() == 3 && get(&result) == 0, "SaveDataDialog completes");
            require(result.mode == mode && result.result == 0 &&
                        result.button == (mode == 2 ? 2u : 1u),
                    "SaveDataDialog chosen No or OK");
            require(result.directory == &directory && result.param == &saved_param &&
                        result.user_data == &caller_data,
                    "SaveDataDialog preserves caller pointers");
            key.type = SDL_EVENT_KEY_UP;
            require(SDL_PushEvent(&key), "SaveDataDialog release");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        require(term() == 0, "SaveDataDialog terminate");
        gz_log("UI TEST: SaveDataDialog user No, error OK and caller pointers passed via PS4 ABI");
        struct Progress {
            u32 type, pad;
            const char *text;
            u32 system;
            char reserved[28];
        } bar{};
        using SetProgress = s32(ABI *)(s32, u32);
        auto set = reinterpret_cast<SetProgress>(
            gz_services_resolve("libSceSaveDataDialog", "sceSaveDataDialogProgressBarSetValue"));
        auto inc = reinterpret_cast<SetProgress>(
            gz_services_resolve("libSceSaveDataDialog", "sceSaveDataDialogProgressBarInc"));
        auto close = reinterpret_cast<Call>(
            gz_services_resolve("libSceSaveDataDialog", "sceSaveDataDialogClose"));
        require(init() == 0 && set && inc && close, "SaveDataDialog progress API");
        param.mode = 5;
        param.progress = &bar;
        bar.system = 1;
        require(open(&param) == 0 && set(0, 45) == 0 && inc(0, 25) == 0 &&
                    set(1, 20) == s32(0x80b8000a),
                "SaveDataDialog progress updates and target validation");
        for (int n = 0; n < 200 && !gz_dialog_active(); ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(gz_dialog_active(), "progress panel visible");
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.scancode = SDL_SCANCODE_SPACE;
        require(SDL_PushEvent(&key), "progress confirm input");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(status() == 2 && gz_dialog_active(), "progress must not finish on X");
        key.type = SDL_EVENT_KEY_UP;
        require(SDL_PushEvent(&key), "progress release");
        require(set(0, 100) == 0 && close() == 0, "game closes progress panel");
        for (int n = 0; n < 200 && status() == 2; ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Result result{};
        require(status() == 3 && get(&result) == 0 && result.result == 0 && result.button == 0 &&
                    term() == 0,
                "progress close returns neutral OK result");
        gz_log("UI TEST: SaveDataDialog progress updates, ignores X and closes through API passed");
    }
    video_frame_lifetime_test();
    using Alloc = s32(ABI *)(s64, s64, u64, u64, s32, s64 *);
    using Map = s32(ABI *)(void **, u64, s32, s32, s64, u64);
    auto alloc =
        reinterpret_cast<Alloc>(gz_memory_resolve("libkernel", "sceKernelAllocateDirectMemory"));
    auto map = reinterpret_cast<Map>(gz_memory_resolve("libkernel", "sceKernelMapDirectMemory"));
    s64 offset;
    constexpr u64 size = 0x20000;
    require(alloc(0, 5056ULL << 20, size, 0x4000, 0, &offset) == 0, "direct allocation");
    void *memory{};
    require(map(&memory, size, 0x33, 0, offset, 0x4000) == 0, "direct map");
    const auto address = reinterpret_cast<uintptr_t>(memory);
    auto *pixels = static_cast<u32 *>(memory);
    // Four quadrants: red, green, blue, white, in the PS4's A8B8G8R8 format.
    for (u32 y = 0; y < 128; ++y)
        for (u32 x = 0; x < 128; ++x)
            pixels[y * 128 + x] =
                y < 64 ? (x < 64 ? 0xff0000ff : 0xff00ff00) : (x < 64 ? 0xffff0000 : 0xffffffff);
    u32 check{};
    require(gz_memory_copy(address, &check, 4, 0) && check == 0xff0000ff, "backing alias read");
    auto *sentinel = reinterpret_cast<u32 *>(address + 0x10000);
    auto *commands = reinterpret_cast<u32 *>(address + 0x14000);
    const uintptr_t target = reinterpret_cast<uintptr_t>(sentinel);
    commands[0] = 0xc0033700; // WRITE_DATA, total 5 words
    commands[1] = 5u << 8;    // destination: memory
    commands[2] = static_cast<u32>(target);
    commands[3] = static_cast<u32>(target >> 32);
    commands[4] = 0xc0def00d;
    const u32 *dcb[] = {commands};
    u32 dcb_size[] = {20};
    require(Libraries::GnmDriver::sceGnmSubmitCommandBuffers(1, dcb, dcb_size, nullptr, nullptr) ==
                0,
            "GnmSubmitCommandBuffers");
    Core::Memory::Instance()->WaitForGpuIdle();
    require(gz_memory_copy(target, &check, 4, 0) && check == 0xc0def00d,
            "PM4 WRITE_DATA execution");
    gz_log("GPU TEST: native memory alias and PM4 command passed");

    using namespace Libraries::VideoOut;
    using namespace Libraries::Kernel;
    const s32 handle = sceVideoOutOpen(1, 0, 0, nullptr);
    require(handle > 0, "VideoOutOpen");
    BufferAttribute attribute;
    sceVideoOutSetBufferAttribute(&attribute, PixelFormat::A8B8G8R8Srgb, 1, 0, 128, 128, 128);
    require(sceVideoOutRegisterBuffers(handle, 0, &memory, 1, &attribute) >= 0,
            "VideoOutRegisterBuffers");
    OrbisKernelEqueue queue;
    require(sceKernelCreateEqueue(&queue, "gpu_test") == 0, "CreateEqueue");
    require(sceVideoOutAddFlipEvent(queue, handle, reinterpret_cast<void *>(0x1234)) == 0,
            "AddFlipEvent");
    require(sceVideoOutSubmitFlip(handle, 0, 1, 77) == 0, "SubmitFlip");
    OrbisKernelEvent event;
    int count{};
    OrbisKernelUseconds timeout = 10'000'000;
    require(sceKernelWaitEqueue(queue, &event, 1, &count, &timeout) == 0 && count == 1,
            "flip event delivery");
    require(event.udata == reinterpret_cast<void *>(0x1234), "flip event user data");
    std::vector<u8> readback;
    u32 width{}, height{};
    require(presenter->CaptureLastFrame(readback, width, height), "GPU readback");
    const u32 expected[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    for (u32 q = 0; q < 4; ++q) {
        const u32 x = (q & 1) ? width * 3 / 4 : width / 4;
        const u32 y = (q & 2) ? height * 3 / 4 : height / 4;
        const u64 at = (u64(y) * width + x) * 4;
        for (u32 c = 0; c < 3; ++c)
            require(std::abs(int(readback[at + c]) - int(expected[q][c])) <= 3,
                    "rendered quadrant color");
    }
    const auto path = std::filesystem::u8path(gz_user_dir()) / "gpu_test.ppm";
    auto *file = _wfopen(path.c_str(), L"wb");
    require(file != nullptr, "capture file open");
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (u64 i = 0; i < readback.size(); i += 4)
        std::fwrite(readback.data() + i, 1, 3, file);
    require(std::fclose(file) == 0, "capture file write");
    gz_log("GPU TEST: VideoOut flip, event and four GPU pixel samples passed (%ux%u)", width,
           height);
    gz_log("GPU TEST: capture saved under user/gpu_test.ppm");
    const auto hwnd = static_cast<HWND>(g_window->GetWindowInfo().render_surface);
    ShowWindow(hwnd, SW_MINIMIZE);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(g_window->IsMinimized(), "window minimize state");
    // CPU writes to the surface now exercise the real GPU page-watch fault handler.
    for (u32 i = 0; i < 128 * 128; ++i)
        pixels[i] = 0xff00ffff;
    require(sceVideoOutSubmitFlip(handle, 0, 1, 78) == 0, "second flip after CPU writes");
    timeout = 10'000'000;
    require(sceKernelWaitEqueue(queue, &event, 1, &count, &timeout) == 0 && count == 1,
            "second flip event");
    require(presenter->CaptureLastFrame(readback, width, height), "second GPU readback");
    u64 center = (u64(height / 2) * width + width / 2) * 4;
    require(readback[center] >= 252 && readback[center + 1] >= 252 && readback[center + 2] <= 3,
            "CPU write invalidation updates GPU image");
    ShowWindow(hwnd, SW_RESTORE);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(!g_window->IsMinimized(), "window restore state");

    // ReadFile writes from kernel mode: our pre-write hook must release watched pages.
    const auto data_path = std::filesystem::u8path(gz_user_dir()) / "data" / "gpu_test_input.tmp";
    file = _wfopen(data_path.c_str(), L"wb");
    require(file != nullptr, "temporary read test file");
    std::vector<u32> green(128 * 128, 0xff00ff00);
    require(std::fwrite(green.data(), 4, green.size(), file) == green.size(), "test file write");
    require(std::fclose(file) == 0, "test file close");
    using Open = s32(ABI *)(const char *, s32, u16);
    using Pread = s64(ABI *)(s32, void *, u64, s64);
    using Close = s32(ABI *)(s32);
    const auto open = reinterpret_cast<Open>(gz_file_resolve("libkernel", "sceKernelOpen"));
    const auto pread = reinterpret_cast<Pread>(gz_file_resolve("libkernel", "sceKernelPread"));
    const auto close = reinterpret_cast<Close>(gz_file_resolve("libkernel", "sceKernelClose"));
    const s32 fd = open("/data/gpu_test_input.tmp", 0, 0);
    require(fd >= 0, "guest test file open");
    require(pread(fd, memory, 128 * 128 * 4, 0) == 128 * 128 * 4, "ReadFile to GPU watched pages");
    require(close(fd) == 0, "guest test file close");
    std::filesystem::remove(data_path);
    require(sceVideoOutSubmitFlip(handle, 0, 1, 79) == 0, "third flip after file read");
    timeout = 10'000'000;
    require(sceKernelWaitEqueue(queue, &event, 1, &count, &timeout) == 0 && count == 1,
            "third flip event");
    require(presenter->CaptureLastFrame(readback, width, height), "third GPU readback");
    center = (u64(height / 2) * width + width / 2) * 4;
    require(readback[center] <= 3 && readback[center + 1] >= 252 && readback[center + 2] <= 3,
            "file read invalidation updates GPU image");
    gz_log("GPU TEST: protected CPU writes and file reads both updated rendered pixels");
    gz_log("GPU TEST: flip while minimized and restored presentation passed");
    // AvPlayer publishes a completed frame through the backing alias and then
    // invalidates once. Verify that this path updates an already cached image.
    std::vector<u32> blue(128 * 128, 0xffff0000);
    require(Core::Memory::Instance()->TryWriteBacking(memory, blue.data(), blue.size() * 4),
            "staged frame backing write");
    Core::Memory::Instance()->InvalidateMemory(address, blue.size() * 4);
    require(sceVideoOutSubmitFlip(handle, 0, 1, 80) == 0, "staged frame flip");
    timeout = 10'000'000;
    require(sceKernelWaitEqueue(queue, &event, 1, &count, &timeout) == 0 && count == 1,
            "staged frame flip event");
    require(presenter->CaptureLastFrame(readback, width, height), "staged frame readback");
    center = (u64(height / 2) * width + width / 2) * 4;
    require(readback[center] <= 3 && readback[center + 1] <= 3 && readback[center + 2] >= 252,
            "completed backing write updates cached GPU image");
    gz_log("GPU TEST: staged frame publication through backing alias passed");
    require(sceVideoOutDeleteFlipEvent(queue, handle) == 0, "DeleteFlipEvent");
    require(sceKernelDeleteEqueue(queue) == 0, "DeleteEqueue");
    require(sceVideoOutClose(handle) == 0, "VideoOutClose");
    camera_readback_test();
    using Unmap = s32(ABI *)(void *, u64);
    using Release = s32(ABI *)(s64, u64);
    require(reinterpret_cast<Unmap>(gz_memory_resolve("libkernel", "sceKernelMunmap"))(memory,
                                                                                       size) == 0,
            "GPU mapped range unmap");
    require(gz_memory_clamp(address, 4) == 0, "unmapped range query");
    require(reinterpret_cast<Release>(
                gz_memory_resolve("libkernel", "sceKernelReleaseDirectMemory"))(offset, size) == 0,
            "direct allocation release");
}
