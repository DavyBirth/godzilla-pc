// Native Vulkan UI, input prompts and PNG capture. Inspired by Kyoei's host
// overlay separation; Dear ImGui retains its MIT license.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_overlay.h"
#include "pc_features.h"
#include "pc_photo.h"
#include "frame_stats.h"
#include "pc_photo_render.h"
#include "pc_photo_native_scene.h"
#include "pc_photo_panel.h"
#include "pc_photo_menu.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <deque>
#include <thread>
#include <algorithm>
#include <cmath>
extern "C" {
#include "gz.h"
}
namespace GzPcOverlay {
namespace {
bool initialized{};
vk::Format surface_format{};
VkFormat color_format{};
unsigned images{};
std::atomic<bool> capture{}, photo_panel{true};
std::atomic<bool> help{}, hints{};
std::atomic<float> pixel_density{1};
std::atomic<int> released_key{};
std::atomic<unsigned> photo_pad_buttons{};
std::atomic<bool> photo_pad_connected{};
std::mutex events_mutex, notice_mutex;
std::deque<SDL_Event> events;
std::string notice;
std::chrono::steady_clock::time_point last_frame, notice_until;
void result(VkResult value) {
    if (value < 0)
        gz_stop(23, "PC overlay: Vulkan error %d", int(value));
}
void report(std::string message) {
    std::lock_guard lock{notice_mutex};
    notice = std::move(message);
    notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}
void keycap(const char *key, const char *action) {
    const float needed = ImGui::CalcTextSize(key).x + ImGui::CalcTextSize(action).x + 42;
    if (ImGui::GetCursorPosX() + needed > ImGui::GetWindowWidth() - 12)
        ImGui::NewLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.25f, .25f, .28f, 1));
    ImGui::SmallButton(key);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(action);
    ImGui::SameLine(0, 18);
}
void draw(unsigned width, unsigned height, bool photo_active) {
    if (auto message = GzPcPhotos::TakeNotice(); !message.empty())
        report(std::move(message));
    GzPhotoNative::TakeNotice(); // Entry/refusal stays in the log; no on-screen popup.
    const float scale = std::min(width / 1920.f, height / 1080.f);
    ImGui::PushFont(nullptr, std::clamp(std::round(32 * scale), 12.f, 48.f));
    const float image_w = 1920 * scale, image_h = 1080 * scale;
    const ImVec2 origin((width - image_w) * .5f, (height - image_h) * .5f);
    const bool main = gz_pc_main_menu(nullptr);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (main && !gz_pc_native_exit_row()) {
        ImGui::SetNextWindowPos(ImVec2(origin.x + image_w * .92f, origin.y + image_h * .333f),
                                ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0);
        ImGui::Begin("##desktop-exit", nullptr, flags | ImGuiWindowFlags_NoInputs);
        ImGui::TextColored(gz_pc_exit_selected() ? ImVec4(1, .55f, .15f, 1)
                                                 : ImVec4(.9f, .85f, .7f, 1),
                           "SAIR PARA A ÁREA DE TRABALHO");
        ImGui::End();
    }
    if (hints && !gz_pc_controller() && !photo_active) {
        ImGui::SetNextWindowPos(ImVec2(origin.x + image_w * .5f, origin.y + image_h - 10),
                                ImGuiCond_Always, ImVec2(.5f, 1));
        ImGui::SetNextWindowBgAlpha(.88f);
        ImGui::SetNextWindowSize(ImVec2(image_w * .96f, 0));
        ImGui::Begin("##pc-prompts", nullptr, flags | ImGuiWindowFlags_NoInputs);
        if (gz_pc_gameplay()) {
            keycap("W / S", "Mover");
            keycap("A / D", "Girar");
            keycap("Mouse", "Camera");
            keycap("Esc", "Pausa");
            if (gz_pc_capture_point() >= 0)
                keycap("F", "Coletar dados");
            keycap("F1", "Controles");
            if (help) {
                keycap("Clique", "Ataque");
                keycap("Clique direito", "Ataque forte");
                keycap("C / Meio", "Sopro");
                keycap("Shift", "Investida");
                keycap("Espaco", "Esquiva");
                keycap("R", "Rugido");
                keycap("F7", "Soltar mouse");
            }
            keycap("F12", "Foto");
        } else {
            keycap("Enter", "Confirmar");
            keycap("Esc", "Voltar");
            keycap("Setas / WASD", "Selecionar");
        }
        ImGui::NewLine();
        ImGui::End();
    }
    if (GzFrameStats::visible) {
        const auto timing = GzFrameStats::Snapshot();
        ImGui::SetNextWindowPos(ImVec2(width - 12, 12), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(.85f);
        ImGui::Begin("##frame-timing", nullptr, flags | ImGuiWindowFlags_NoInputs);
        if (timing.count)
            ImGui::Text("%.1f FPS | %.2f ms | p99 %.2f ms", timing.fps, timing.mean_ms,
                        timing.p99_ms);
        else
            ImGui::TextUnformatted("Medindo quadros apresentados...");
        ImGui::End();
    }
    std::lock_guard lock{notice_mutex};
    if (std::chrono::steady_clock::now() < notice_until) {
        ImGui::SetNextWindowPos(ImVec2(width * .5f, 12), ImGuiCond_Always, ImVec2(.5f, 0));
        ImGui::Begin("##photo-notice", nullptr, flags | ImGuiWindowFlags_NoInputs);
        ImGui::TextUnformatted(notice.c_str());
        ImGui::End();
    }
    ImGui::PopFont();
}
} // namespace
void Initialize(const Vulkan::Instance &instance, vk::Format format, unsigned count) {
    if (initialized && surface_format == format && images == count)
        return;
    if (initialized) {
        {
            std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
            result(VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(instance.GetDevice()));
        }
        Shutdown();
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    const char *font = std::getenv("GZ_SYSTEM_FONT");
    if (font && std::filesystem::exists(std::filesystem::u8path(font)))
        io.Fonts->AddFontFromFileTTF(font, 18);
    else
        io.Fonts->AddFontDefault();
    if (std::filesystem::exists("C:/Windows/Fonts/bahnschrift.ttf"))
        menu_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/bahnschrift.ttf", 24);
    ImGui::StyleColorsDark();
    NativePhotoStyle();
    auto handle = instance.GetInstance();
    if (!ImGui_ImplVulkan_LoadFunctions(
            instance.ApiVersion(),
            [](const char *name, void *user) {
                return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                    *static_cast<vk::Instance *>(user), name);
            },
            &handle))
        gz_stop(23, "PC overlay: Vulkan functions unavailable");
    color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = handle;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 32;
    info.MinImageCount = std::max(2u, count);
    info.ImageCount = std::max(2u, count);
    info.UseDynamicRendering = true;
    info.CheckVkResultFn = result;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format};
    if (!ImGui_ImplVulkan_Init(&info))
        gz_stop(23, "PC overlay: initialization failed");
    surface_format = format;
    images = count;
    initialized = true;
    last_frame = std::chrono::steady_clock::now();
}
void Shutdown() {
    if (!initialized)
        return;
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();
    initialized = false;
}
bool Event(const SDL_Event &event) {
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
        released_key = 0;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_MINIMIZED)
        GzPhotoNative::Focus(false);
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
        GzPhotoNative::Focus(true);
    if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
        event.key.scancode == released_key.load()) {
        if (event.type == SDL_EVENT_KEY_UP)
            released_key = 0;
        return true;
    }
    if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
        event.key.scancode == SDL_SCANCODE_F10) {
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            GzPhotoNative::Toggle();
        }
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL) {
        std::lock_guard lock{events_mutex};
        if (events.size() < 256) {
            auto scaled = event;
            const float density = pixel_density.load();
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                scaled.motion.x *= density;
                scaled.motion.y *= density;
            } else if (event.type != SDL_EVENT_MOUSE_WHEEL) {
                scaled.button.x *= density;
                scaled.button.y *= density;
            }
            events.push_back(scaled);
        }
    }
    if (GzPhotoNative::Active()) {
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    released_key = event.key.scancode;
                    GzPhotoNative::Exit();
                }
                if (event.key.scancode == SDL_SCANCODE_F12)
                    RequestCapture();
                if (event.key.scancode == SDL_SCANCODE_TAB)
                    TogglePhotoPanel();
                if (event.key.scancode == SDL_SCANCODE_R && !photo_panel)
                    GzPhotoNative::Reset();
                if (photo_panel)
                    switch (event.key.scancode) {
                    // WASD/Space/Ctrl always drive the camera; arrows and Enter drive the menu.
                    case SDL_SCANCODE_UP:
                        menu_keys |= MenuUp;
                        break;
                    case SDL_SCANCODE_DOWN:
                        menu_keys |= MenuDown;
                        break;
                    case SDL_SCANCODE_LEFT:
                        menu_keys |= MenuLeft;
                        break;
                    case SDL_SCANCODE_RIGHT:
                        menu_keys |= MenuRight;
                        break;
                    case SDL_SCANCODE_RETURN:
                    case SDL_SCANCODE_KP_ENTER:
                        menu_keys |= MenuOk;
                        break;
                    default:
                        break;
                    }
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            if (!photo_panel)
                GzPhotoNative::Lens(event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                                        ? event.wheel.y
                                        : -event.wheel.y);
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            return true;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        if (event.key.scancode == SDL_SCANCODE_F3) {
            GzFrameStats::visible = !GzFrameStats::visible.load();
            return true;
        }
        if (event.key.scancode == SDL_SCANCODE_F1)
            help = !help.load();
        if (event.key.scancode == SDL_SCANCODE_F9)
            hints = !hints.load();
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F12 &&
        !event.key.repeat)
        RequestCapture();
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        std::lock_guard lock{events_mutex};
        events.clear();
        events.push_back(event);
        return false;
    }
    return false;
}
bool KeyboardUi() {
    return GzPhotoNative::Active();
}
bool MouseUi() {
    return GzPhotoNative::Active() && photo_panel.load();
}
void TogglePhotoPanel() {
    if (GzPhotoNative::Active())
        photo_panel = !photo_panel.load();
}
void PhotoGamepad(unsigned buttons, bool connected) {
    photo_pad_buttons = buttons;
    photo_pad_buttons_for_menu = buttons;
    photo_pad_connected = connected;
}
void PixelDensity(float density) {
    pixel_density = density > 0 ? density : 1;
}
void FinishSaves() {
    GzPcPhotos::Finish();
}
bool Render(vk::CommandBuffer command, vk::ImageView view, vk::Extent2D extent,
            const GzPhoto::PhotoFrame &photo, unsigned source_width, unsigned source_height) {
    if (!initialized)
        return false;
    auto &io = ImGui::GetIO();
    const auto now = std::chrono::steady_clock::now();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = std::clamp(std::chrono::duration<float>(now - last_frame).count(), .001f, .1f);
    last_frame = now;
    {
        std::lock_guard lock{events_mutex};
        for (const auto &e : events) {
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                io.AddFocusEvent(false);
                for (int i = 0; i < 5; ++i)
                    io.AddMouseButtonEvent(i, false);
            }
            if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
                io.AddFocusEvent(true);
            if (e.type == SDL_EVENT_MOUSE_MOTION)
                io.AddMousePosEvent(e.motion.x, e.motion.y);
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                const int b = e.button.button == SDL_BUTTON_LEFT     ? 0
                              : e.button.button == SDL_BUTTON_RIGHT  ? 1
                              : e.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                     : -1;
                if (b >= 0)
                    io.AddMouseButtonEvent(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            }
            if (e.type == SDL_EVENT_MOUSE_WHEEL)
                io.AddMouseWheelEvent(e.wheel.x, e.wheel.y);
        }
        events.clear();
    }
    const bool pad_navigation = photo.active && photo_panel && photo_pad_connected;
    PhotoNavigation(io, photo_pad_buttons, pad_navigation);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (photo.active) {
        const auto viewport =
            GzPhoto::FitPhotoViewport(source_width, source_height, extent.width, extent.height);
        const auto guides = GzPhoto::CompositionGuides(
            source_width, source_height, photo.settings.grade.frame, photo.settings.guide);
        for (unsigned i = 0; i < guides.count; ++i) {
            const auto line = guides.lines[i];
            ImGui::GetBackgroundDrawList()->AddLine(
                {viewport.x + line.x1 * viewport.width, viewport.y + line.y1 * viewport.height},
                {viewport.x + line.x2 * viewport.width, viewport.y + line.y2 * viewport.height},
                IM_COL32(255, 255, 255, 160), 1.f);
        }
    }
    draw(extent.width, extent.height, photo.active);
    static uint64_t panel_ticket{};
    if (photo.active && panel_ticket != photo.ticket) {
        panel_ticket = photo.ticket;
        photo_panel = true;
    }
    if (photo.active && photo_panel)
        GamePhotoMenu(photo, extent.width, extent.height);
    ImGui::Render();
    // No attachment load/store pass or renderer lock when the UI is hidden.
    if (ImGui::GetDrawData()->TotalVtxCount == 0)
        return false;
    const vk::RenderingAttachmentInfo attachment{.imageView = view,
                                                 .imageLayout =
                                                     vk::ImageLayout::eColorAttachmentOptimal,
                                                 .loadOp = vk::AttachmentLoadOp::eLoad,
                                                 .storeOp = vk::AttachmentStoreOp::eStore};
    command.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, extent},
                                             .layerCount = 1,
                                             .colorAttachmentCount = 1,
                                             .pColorAttachments = &attachment});
    {
        std::scoped_lock lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
    }
    command.endRendering();
    return true;
}
void RequestCapture() {
    capture = true;
}
bool TakeCapture() {
    return capture.exchange(false);
}
void SaveCapture(std::vector<unsigned char> pixels, unsigned width, unsigned height,
                 GzPhoto::Grade grade) {
    GzPcPhotos::Save(std::move(pixels), width, height, grade);
}
} // namespace GzPcOverlay
