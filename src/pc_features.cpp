// Godzilla 01.01 context and native menu extension. No gameplay automation.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_features.h"
#include "pc_menu_input.h"
#include "pc_photo_native_scene.h"
#include "pc_pause_menu.h"
#include "camera_capture.h"
#include "dialog_ui.h"
extern "C" {
#include "gz.h"
}
#include <windows.h>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <cstdlib>

namespace {
unsigned char *image{};
uint64_t image_size{};
std::atomic<ULONGLONG> menu_frame{};
std::atomic<int> menu_cursor{-1}, input_controller{}, exit_selected{}, quit{};
std::atomic<bool> native_exit_row{}, native_exit_request{}, native_exit_closing{};
GzPcMenuInput menu_input; // Window input thread only.
GzNativeExitInput native_exit_input;
GzNativeExitInput vs_input;
std::atomic<bool> vs_notice_running{};
DWORD WINAPI vs_notice(void *) {
    gz_dialog_show(L"O modo VS é jogado pela internet, na rede do PlayStation.\n"
                   L"Essa rede não existe na versão de PC, então este modo não está disponível.",
                   0);
    vs_notice_running = false;
    return 0;
}
using MenuUpdate = uintptr_t(ABI *)(void *);
using MenuScript = void(ABI *)(void *, unsigned, unsigned);
MenuUpdate original_menu{};
MenuScript run_menu_script{};
void *closing_menu{};
ULONGLONG closing_started{};
bool read(const void *pointer, void *output, size_t bytes) {
    if (!pointer)
        return false;
    SIZE_T got{};
    return ReadProcessMemory(GetCurrentProcess(), pointer, output, bytes, &got) && got == bytes;
}
template <class T> T at(const void *pointer, size_t offset) {
    T value{};
    if (pointer)
        read(static_cast<const char *>(pointer) + offset, &value, sizeof(value));
    return value;
}
template <class T> void set(void *pointer, size_t offset, T value) {
    std::memcpy(static_cast<char *>(pointer) + offset, &value, sizeof(value));
}
ABI uintptr_t menu_update(void *self) {
    const bool extended = run_menu_script && at<int>(self, 0x670) == 8;
    if (extended) {
        set<unsigned char>(self, 0xd6e47, 1);
        for (const auto offset : {0x3b8, 0x3c8, 0x3d8}) {
            if (auto *child = at<void *>(self, offset))
                set<uint32_t>(child, 0x1e0, at<uint32_t>(child, 0x1e0) | 0x80u);
        }
        if (native_exit_request.exchange(false) && at<short>(self, 0x98) == 2 &&
            at<short>(self, 0x9a) == 1 && at<int>(at<void *>(self, 0x3d8), 0x1d0) == 7) {
            closing_menu = self;
            closing_started = GetTickCount64();
            native_exit_closing = true;
            // Run the same child model/sound scripts as a native selection.
            // Do not enter the native scene-routing path for auxiliary row7.
            run_menu_script(self, 2, 0);
            gz_log("PC menu: native exit animation and sound started");
        }
    }
    if (closing_menu == self) {
        menu_frame = 0;
        // Keep the native animation/event pumps running, but route the state
        // switch to its default branch so it cannot open auxiliary scene7.
        set<short>(self, 0x9a, 0x7ffe);
        const auto result = original_menu(self);
        if (!at<unsigned char>(self, 0x378) || GetTickCount64() - closing_started > 10000) {
            gz_log("PC menu: desktop exit requested after native animation (timeout=%d)",
                   int(at<unsigned char>(self, 0x378) != 0));
            quit = 1;
        }
        return result;
    }
    const auto result = original_menu(self);
    const auto selector = at<void *>(self, 0x3d8);
    const int cursor = at<int>(selector, 0x1d0);
    native_exit_row = extended;
    if (at<short>(self, 0x98) == 2 && at<short>(self, 0x9a) == 1 && selector && cursor >= 0 &&
        cursor < (extended ? 8 : 7) && !at<unsigned char>(selector, 0x378)) {
        static int logged_cursor = -2;
        static bool logged_extended = false;
        if (std::getenv("GZ_MENU_TRACE") &&
            (cursor != logged_cursor || extended != logged_extended)) {
            gz_log("PC menu: cursor=%d rows=%d native=%d object=%p", cursor, at<int>(self, 0x670),
                   int(extended), self);
            logged_cursor = cursor;
            logged_extended = extended;
        }
        menu_cursor = cursor;
        menu_frame = GetTickCount64();
    } else
        menu_frame = 0;
    return result;
}
uint64_t fnv(const unsigned char *p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    while (n--)
        h = (h ^ *p++) * 1099511628211ull;
    return h;
}
} // namespace
extern "C" void gz_pc_configure(void *base, uint64_t size) {
    image = static_cast<unsigned char *>(base);
    image_size = size;
    GzPhotoNative::Configure(base, size);
    GzPauseMenu::Configure(base, size);
    GzCameraCapture::Configure(base, size);
    // A replacement ABI function and relay remain a platform facility, not a
    // dependency of the keyboard/mouse mapping. Verify before touching code.
    if (!base || size < 0xa2b600)
        return;
    const size_t entry = 0x2c640;
    const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    if (std::memcmp(image + entry, prologue, sizeof(prologue)))
        return;
    // Full routine signature is set from the pinned 01.01 linked image.
    constexpr uint64_t expected =
        0x1a56387e579e82d8ull; // Pinned 01.01 linked image, first 96 bytes.
    if (!expected || fnv(image + entry, 96) != expected) {
        gz_log("PC menu: unsupported update signature; native exit row unavailable");
        return;
    }
    if (fnv(image + 0x406b0, 96) == 0x87b1d774dccccb3aull)
        run_menu_script = reinterpret_cast<MenuScript>(image + 0x406b0);
    unsigned char *relay{};
    for (unsigned i = 1; i < 128 && !relay; ++i)
        relay = static_cast<unsigned char *>(
            VirtualAlloc(image + ((size + 0xffff) & ~size_t(0xffff)) + i * 0x10000, 4096,
                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!relay)
        return;
    std::memcpy(relay, prologue, 6);
    relay[6] = 0xff;
    relay[7] = 0x25;
    std::memset(relay + 8, 0, 4);
    const auto resume = reinterpret_cast<uintptr_t>(image + entry + 6);
    std::memcpy(relay + 12, &resume, 8);
    relay[32] = 0xff;
    relay[33] = 0x25;
    std::memset(relay + 34, 0, 4);
    const auto callback = reinterpret_cast<uintptr_t>(menu_update);
    std::memcpy(relay + 38, &callback, 8);
    const int64_t distance = int64_t(reinterpret_cast<uintptr_t>(relay + 32)) -
                             int64_t(reinterpret_cast<uintptr_t>(image + entry + 5));
    DWORD old{};
    if (distance < INT32_MIN || distance > INT32_MAX ||
        !VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return;
    }
    if (!VirtualProtect(image + entry, 6, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return;
    }
    original_menu = reinterpret_cast<MenuUpdate>(relay);
    int32_t relative = static_cast<int32_t>(distance);
    image[entry] = 0xe9;
    std::memcpy(image + entry + 1, &relative, 4);
    image[entry + 5] = 0x90;
    DWORD ignored{};
    if (!VirtualProtect(image + entry, 6, old, &ignored))
        gz_stop(21, "PC menu: cannot restore code protection");
    if (!FlushInstructionCache(GetCurrentProcess(), relay, 46) ||
        !FlushInstructionCache(GetCurrentProcess(), image + entry, 6))
        gz_stop(21, "PC menu: cannot flush code cache");
    gz_log("PC menu: verified native desktop exit row installed");
}
extern "C" int gz_pc_main_menu(int *cursor) {
    const auto frame = menu_frame.load();
    const bool shown = frame && GetTickCount64() - frame < 250;
    if (cursor)
        *cursor = shown ? menu_cursor.load() : -1;
    return shown;
}
extern "C" int gz_pc_gameplay() {
    if (!image || image_size < 0xa2b600)
        return 0;
    const auto mission = at<void *>(image, 0x6c3568);
    return mission && at<uintptr_t>(mission, 0) == reinterpret_cast<uintptr_t>(image + 0x66e480) &&
           at<short>(mission, 0x98) == 2 && at<int>(image, 0x79aa10 + 0x3a8) == 1 &&
           !at<int>(image, 0x79aa10 + 0x3c0);
}
extern "C" int gz_pc_action_prompts() {
    if (gz_pc_gameplay())
        return 1;
    if (!image || image_size < 0xa2b600)
        return 0;
    const auto mission = at<void *>(image, 0x6c3568);
    // Tutorial explanations use mission scene state4, then retain the same
    // sheet during state2. Keep their action glyphs stable across that handoff.
    // Pause3, results5, retry6 and menus retain navigation glyphs. Space and
    // Enter both confirm native tutorial pages; input classification is separate.
    return mission && at<uintptr_t>(mission, 0) == reinterpret_cast<uintptr_t>(image + 0x66e480) &&
           at<short>(mission, 0x98) == 4;
}
// Inside a mission but outside free play: real-time scenes, whose skip is Touchpad.
extern "C" int gz_pc_mission_scene() {
    if (!image || image_size < 0xa2b600 || gz_pc_gameplay())
        return 0;
    const auto mission = at<void *>(image, 0x6c3568);
    return mission && at<uintptr_t>(mission, 0) == reinterpret_cast<uintptr_t>(image + 0x66e480) &&
           at<short>(mission, 0x98) == 4;
}
extern "C" int gz_pc_capture_point() {
    return gz_pc_gameplay() ? at<int>(image, 0x79aa10 + 0x458) : -1;
}
extern "C" int gz_pc_exit_input(uint32_t buttons, unsigned stick_y) {
    int cursor = -1;
    const bool main = gz_pc_main_menu(&cursor);
    // Stop unavailable online routing at the host boundary. Both original VS
    // branches otherwise enter asynchronous connection states with no PSN.
    const bool unavailable =
        vs_input.Confirm(main && !vs_notice_running && !gz_dialog_active(), cursor, buttons, 2);
    if (unavailable) {
        vs_notice_running = true;
        gz_dialog_prepare();
        HANDLE worker = CreateThread(nullptr, 0, vs_notice, nullptr, 0, nullptr);
        if (worker)
            CloseHandle(worker);
        else {
            vs_notice_running = false;
            gz_log("PC menu: VS notice thread unavailable");
        }
        gz_log("PC menu: unavailable online VS intercepted before scene routing");
    }
    // Dialog input must reach gz_dialog_input below this hook; only consume
    // the original opening press and its held/repeated samples here.
    if (vs_input.blocked)
        return 1;
    const bool native = native_exit_row.load();
    const bool confirm = native_exit_input.Confirm(native && main, cursor, buttons);
    if (native) {
        menu_input.Update(false, cursor, buttons, stick_y);
        if (confirm)
            native_exit_request = true;
        exit_selected = main && cursor == 7;
        return native_exit_input.blocked || native_exit_request.load() ||
               native_exit_closing.load();
    }
    const bool consumed = menu_input.Update(main, cursor, buttons, stick_y);
    exit_selected = menu_input.selected;
    if (menu_input.quit) {
        quit = 1;
        menu_input.quit = false;
    }
    return consumed;
}
extern "C" int gz_pc_exit_selected() {
    return gz_pc_main_menu(nullptr) && exit_selected.load();
}
extern "C" int gz_pc_native_exit_row() {
    return native_exit_row.load();
}
extern "C" int gz_pc_take_quit() {
    return quit.exchange(0);
}
extern "C" void gz_pc_activity(int controller) {
    input_controller = controller != 0;
}
extern "C" int gz_pc_controller_active() {
    return input_controller.load();
}
extern "C" int gz_pc_controller() {
    static const char *forced = std::getenv("GZ_PROMPT_MODE");
    if (forced && !std::strcmp(forced, "keyboard"))
        return 0;
    if (forced && !std::strcmp(forced, "controller"))
        return 1;
    return input_controller.load();
}
extern "C" void gz_pc_release_input() {
    menu_input.Release();
    native_exit_input.Release();
    vs_input.Release();
}
