// SDL video operations stay on one host thread, as in bbport.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdlib>
#include <windows.h>
#include "sdl_window.h"
#include "pad_host.h"
#include "pc_features.h"
#include "pc_overlay.h"
#include "app_icon.h"
extern "C" {
#include "gz.h"
}
namespace Frontend {
WindowSDL::WindowSDL(s32 w, s32 h, const char *title) : width{w}, height{h}, base_title{title} {
    const bool no_hardware =
        std::getenv("GZ_TEST_INPUT_FILE") || std::getenv("GZ_TEST_NO_HARDWARE");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | (no_hardware ? 0 : SDL_INIT_GAMEPAD)))
        gz_stop(23, "STOP: SDL initialization: %s", SDL_GetError());
    auto dimension = [](const char *name, int fallback, int minimum, int maximum) {
        const char *text = std::getenv(name);
        if (!text || !*text)
            return fallback;
        char *end = nullptr;
        const long value = std::strtol(text, &end, 10);
        return end && !*end && value >= minimum && value <= maximum ? int(value) : fallback;
    };
    w = dimension("GZ_WINDOW_WIDTH", w, 640, 8192);
    h = dimension("GZ_WINDOW_HEIGHT", h, 360, 4320);
    width.store(w);
    height.store(h);
    const char *background_option = std::getenv("GZ_TEST_BACKGROUND");
    const bool background_test = background_option != nullptr;
    const bool background_visible = background_option && background_option[0] == '2';
    window = SDL_CreateWindow(
        title, w, h,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
            (background_test ? (background_visible ? SDL_WINDOW_NOT_FOCUSABLE : SDL_WINDOW_HIDDEN)
                             : 0));
    if (!window)
        gz_stop(23, "STOP: SDL window: %s", SDL_GetError());
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!window_info.render_surface)
        gz_stop(23, "STOP: SDL window has no HWND");
    const auto hwnd = static_cast<HWND>(window_info.render_surface);
    if (background_test && !background_visible)
        ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    const auto icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
    wchar_t game_path[32768];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, gz_app0(), -1, game_path, 32768)) {
        std::wstring artwork(game_path);
        artwork += L"\\sce_sys\\icon0.png";
        gz_set_game_icon(hwnd, artwork.c_str());
    }
    if (const char *mode = std::getenv("GZ_FULLSCREEN");
        mode && (mode[0] == '1' || mode[0] == '2')) {
        if (mode[0] == '1') {
            SDL_DisplayMode selected{};
            const auto display = SDL_GetDisplayForWindow(window);
            if (!SDL_GetClosestFullscreenDisplayMode(display, w, h, 0, false, &selected) ||
                selected.w != w || selected.h != h ||
                !SDL_SetWindowFullscreenMode(window, &selected))
                gz_stop(23,
                        "STOP: fullscreen resolution %dx%d is not supported by this monitor: %s", w,
                        h, SDL_GetError());
            gz_log("GPU: exclusive fullscreen selected %dx%d at %.2f Hz", selected.w, selected.h,
                   selected.refresh_rate);
        } else if (!SDL_SetWindowFullscreenMode(window, nullptr)) {
            gz_stop(23, "STOP: cannot select borderless fullscreen: %s", SDL_GetError());
        }
        if (!SDL_SetWindowFullscreen(window, true))
            gz_stop(23, "STOP: fullscreen failed: %s", SDL_GetError());
    }
    gz_log("GPU: SDL window created (%dx%d)", w, h);
}
WindowSDL::~WindowSDL() {
    gz_release_game_icon(static_cast<HWND>(window_info.render_surface));
    SDL_DestroyWindow(window);
}
bool WindowSDL::IsMinimized() const {
    // Use the HWND state directly: SDL restore events can lag behind Windows.
    return IsIconic(static_cast<HWND>(window_info.render_surface)) != FALSE;
}
bool WindowSDL::PollEvents() {
    GzPcOverlay::PixelDensity(SDL_GetWindowPixelDensity(window));
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        gz_pad_event(event);
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            is_open.store(false);
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            if (event.window.data1 > 0 && event.window.data2 > 0) {
                width.store(event.window.data1);
                height.store(event.window.data2);
            }
        }
        if (event.type == SDL_EVENT_WINDOW_MINIMIZED)
            minimized.store(true);
        if (event.type == SDL_EVENT_WINDOW_RESTORED || event.type == SDL_EVENT_WINDOW_MAXIMIZED)
            minimized.store(false);
    }
    if (gz_pc_take_quit())
        is_open.store(false);
    gz_pad_mouse_update(window);
    gz_pad_poll();
    return IsOpen();
}
} // namespace Frontend
