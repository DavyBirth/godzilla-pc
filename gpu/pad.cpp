// Windows input bridge. PS4 layouts/contracts from shadPS4 3d03375a pad.h/pad.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <SDL3/SDL.h>
#include "pc_photo_native_scene.h"
#include "pc_photo_channel.h"
#include "pc_photo_input_gate.h"
#include <array>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdlib>
#include "pad_host.h"
#include "test_input.h"
#include "pc_controls.h"
#include "pc_playback.h"
#include "pc_overlay.h"
#include "pc_features.h"
#include "pc_controller.h"
#include "core/libraries/libs.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/pad/pad_errors.h"
extern "C" {
#include "gz.h"
#include "dialog_ui.h"
}
using namespace Libraries::Pad;
static_assert(sizeof(OrbisPadData) == 120);
static_assert(GzPcControls::OptionsMask == static_cast<u32>(OrbisPadButtonDataOffset::Options));
static_assert(GzPcControls::TouchpadMask == static_cast<u32>(OrbisPadButtonDataOffset::TouchPad));
static_assert(offsetof(OrbisPadData, timestamp) == 80);
static_assert(sizeof(OrbisPadControllerInformation) == 28);
namespace {
std::mutex mutex;
bool initialized{}, opened{}, motion{};
s32 handle{}, next_handle = 1;
u32 keys{};
u32 mouse_buttons{};
bool focused = true, mouse_captured{}, mouse_released{}, pause_released{}, previous_gameplay{};
float mouse_dx{}, mouse_dy{};
bool photo_drag{};
u64 previous_mouse_ns{};
std::array<bool, SDL_SCANCODE_COUNT> held{};
GzPcControls::ContextKey escape_key;
GzPcControls::ContextKey start_key;
OrbisPadData latest{};
std::deque<OrbisPadData> history;
SDL_Gamepad *device{}; // Accessed exclusively by the window thread.
OrbisPadVibrationParam vibration{};
OrbisPadLightBarParam light{0, 0, 255, 0};
bool vibration_dirty{}, light_dirty{};
u64 last_rumble{};
u64 next_controller_scan{};
int rumble_large = -1, rumble_small = -1;
u32 last_physical_buttons{};
GzPcController::ReleaseGate template_release;
int last_left_direction = -1;
u32 last_read_buttons{}, last_state_buttons{};
bool logged_read{}, logged_state{};
const bool trace_input = std::getenv("GZ_INPUT_TRACE") || std::getenv("GZ_TEST_INPUT_FILE");
std::array<u8, 6> previous_physical_axes{128, 128, 128, 128, 0, 0};
u32 key_mask(SDL_Scancode key, bool gameplay) {
    // Esc skips mission scenes like Tab (Touchpad), as it already skips videos.
    const bool scene = key == SDL_SCANCODE_ESCAPE && !gameplay && gz_pc_mission_scene();
    return GzPcControls::KeyMask(key, gameplay, GzPcPlayback::Active() || scene,
                                 gz_dialog_active());
}
bool valid(s32 h) {
    return opened && h == handle;
}
} // namespace
void gz_pad_event(const SDL_Event &e) {
    const bool keyboard_ui = GzPcOverlay::KeyboardUi();
    const bool consumed = GzPcOverlay::Event(e);
    // Keys/clicks eaten by the photo UI still mean the player switched to keyboard.
    if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        e.type == SDL_EVENT_MOUSE_WHEEL)
        gz_pc_activity(0);
    if (keyboard_ui || GzPcOverlay::KeyboardUi()) {
        std::lock_guard lock{mutex};
        held.fill(false);
    }
    if (consumed && e.type != SDL_EVENT_MOUSE_BUTTON_UP && e.type != SDL_EVENT_KEY_UP)
        return;
    std::lock_guard lock{mutex};
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        gz_pc_activity(0);
        if (e.key.scancode == SDL_SCANCODE_F7)
            mouse_released = !mouse_released;
        if (e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_TAB)
            pause_released = !pause_released;
        if (e.key.scancode == SDL_SCANCODE_ESCAPE)
            escape_key.Press(key_mask(SDL_SCANCODE_ESCAPE, gz_pc_gameplay()));
    }
    if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.scancode > 0 &&
        e.key.scancode < SDL_SCANCODE_COUNT)
        held[e.key.scancode] = e.type == SDL_EVENT_KEY_DOWN;
    if (e.type == SDL_EVENT_KEY_UP && e.key.scancode == SDL_SCANCODE_ESCAPE)
        escape_key.Release();
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        const unsigned bit = 1u << e.button.button;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            mouse_buttons |= bit;
            gz_pc_activity(0);
        } else
            mouse_buttons &= ~bit;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION && !mouse_captured && GzPcOverlay::MouseUi() &&
        (e.motion.state & SDL_BUTTON_RMASK)) {
        // Photo menu open: right-drag turns the camera without hiding the menu.
        mouse_dx = std::clamp(mouse_dx + e.motion.xrel, -4096.f, 4096.f);
        mouse_dy = std::clamp(mouse_dy + e.motion.yrel, -4096.f, 4096.f);
        photo_drag = true;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION && mouse_captured) {
        mouse_dx = std::clamp(mouse_dx + e.motion.xrel, -4096.f, 4096.f);
        mouse_dy = std::clamp(mouse_dy + e.motion.yrel, -4096.f, 4096.f);
        if (e.motion.xrel || e.motion.yrel)
            gz_pc_activity(0);
    }
    // Controller noise and uncaptured cursor motion must not switch devices.
    if (e.type == SDL_EVENT_GAMEPAD_ADDED)
        next_controller_scan = 0;
    if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        focused = false;
        keys = mouse_buttons = 0;
        held.fill(false);
        escape_key.Release();
        mouse_dx = mouse_dy = 0;
        gz_pc_release_input();
    }
    if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
        focused = true;
}
void gz_pad_mouse_update(SDL_Window *window) {
    std::lock_guard lock{mutex};
    const bool gameplay = gz_pc_gameplay();
    if (gameplay && !previous_gameplay)
        pause_released = false;
    previous_gameplay = gameplay;
    const char *option = std::getenv("GZ_MOUSE_CAMERA");
    const bool want = (!option || option[0] != '0') && !std::getenv("GZ_TEST_NO_HARDWARE") &&
                      !std::getenv("GZ_TEST_INPUT_FILE") && gameplay &&
                      !gz_pc_controller_active() && !mouse_released && !pause_released && focused &&
                      !GzPcOverlay::MouseUi() &&
                      !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED);
    if (want != mouse_captured) {
        if (SDL_SetWindowRelativeMouseMode(window, want))
            mouse_captured = want;
        mouse_dx = mouse_dy = 0;
        previous_mouse_ns = SDL_GetTicksNS();
    }
    const bool pointer = !focused || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) ||
                         GzPcOverlay::MouseUi() || (mouse_released && !gz_pc_controller_active());
    if (pointer != SDL_CursorVisible()) {
        if (pointer)
            SDL_ShowCursor();
        else
            SDL_HideCursor();
    }
}
void gz_pad_poll() {
    std::lock_guard lock{mutex};
    static const char *test_input = std::getenv("GZ_TEST_INPUT_FILE");
    const bool no_hardware = test_input || std::getenv("GZ_TEST_NO_HARDWARE");
    if (device && !SDL_GamepadConnected(device)) {
        SDL_CloseGamepad(device);
        device = nullptr;
        start_key.Release();
        gz_log("Pad: controller disconnected; keyboard remains available");
    }
    if (!device && !no_hardware && SDL_GetTicks() >= next_controller_scan) {
        next_controller_scan = SDL_GetTicks() + 1000;
        int count{};
        auto *ids = SDL_GetGamepads(&count);
        if (count) {
            device = SDL_OpenGamepad(ids[0]);
            if (device) {
                gz_log("Pad: SDL controller '%s' connected", SDL_GetGamepadName(device));
                switch (SDL_GetGamepadType(device)) {
                case SDL_GAMEPAD_TYPE_PS3:
                case SDL_GAMEPAD_TYPE_PS4:
                case SDL_GAMEPAD_TYPE_PS5:
                    gz_pc_controller_detected(1);
                    break;
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                    gz_pc_controller_detected(3);
                    break;
                default:
                    gz_pc_controller_detected(2);
                    break;
                }
                rumble_large = rumble_small = -1;
                vibration_dirty = light_dirty = true;
            }
        }
        SDL_free(ids);
    }
    OrbisPadData data{};
    data.leftStick = data.rightStick = {128, 128};
    data.orientation.w = 1;
    data.connected = true;
    data.connectedCount = 1; // Keyboard is a permanent input source.
    // Pad timestamps share the game's process clock. SDL starts its own epoch
    // later; mixing epochs makes fresh button presses appear already held.
    using ProcessClock = u64(PS4_SYSV_ABI *)();
    static const auto process_clock =
        reinterpret_cast<ProcessClock>(gz_kernel_resolve("libkernel", "sceKernelGetProcessTime"));
    data.timestamp = process_clock();
    const bool gameplay = gz_pc_gameplay();
    keys = 0;
    if (!held[SDL_SCANCODE_ESCAPE])
        escape_key.Release();
    for (size_t i = 0; i < held.size(); ++i)
        if (held[i])
            keys |= i == SDL_SCANCODE_ESCAPE
                        ? escape_key.Sample(key_mask(SDL_SCANCODE_ESCAPE, gameplay))
                        : key_mask(static_cast<SDL_Scancode>(i), gameplay);
    u32 buttons = keys;
    for (unsigned b = 1; b <= 5; ++b)
        if (mouse_buttons & (1u << b))
            buttons |= GzPcControls::MouseMask(b, gameplay && !pause_released);
    if (device && !test_input) {
        const int controller_template = gz_pc_controller_template();
        const std::pair<SDL_GamepadButton, u32> mapping[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, GzPcController::SouthMask(controller_template)},
            {SDL_GAMEPAD_BUTTON_EAST, GzPcController::EastMask(controller_template)},
            {SDL_GAMEPAD_BUTTON_WEST, 0x8000},
            {SDL_GAMEPAD_BUTTON_NORTH, 0x1000},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, 2},
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 4},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x400},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x800},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x10},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x20},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x40},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x80},
            {SDL_GAMEPAD_BUTTON_TOUCHPAD,
             GzPcControls::SystemButtonMask(SDL_GAMEPAD_BUTTON_TOUCHPAD)}};
        u32 physical_buttons{};
        for (auto [button, mask] : mapping)
            if (SDL_GetGamepadButton(device, button))
                physical_buttons |= mask;
        const auto start_mask = GzPcControls::StartMask(
            GzPcPlayback::Active(), !gameplay && gz_pc_mission_scene(), gz_dialog_active());
        if (SDL_GetGamepadButton(device, SDL_GAMEPAD_BUTTON_START))
            start_key.Press(start_mask);
        else
            start_key.Release();
        physical_buttons |= start_key.Sample(start_mask);
        physical_buttons = template_release.Apply(controller_template, physical_buttons);
        if (physical_buttons != last_physical_buttons) {
            if ((physical_buttons & ~last_physical_buttons) &
                (GzPcControls::OptionsMask | GzPcControls::TouchpadMask))
                pause_released = !pause_released;
            if (physical_buttons)
                gz_pc_activity(1);
            if (trace_input)
                gz_log("Pad: physical controller buttons=0x%x", physical_buttons);
            last_physical_buttons = physical_buttons;
        }
        buttons |= physical_buttons;
        auto axis = [](SDL_GamepadAxis a) {
            return u8((int(SDL_GetGamepadAxis(device, a)) + 32768) >> 8);
        };
        data.leftStick = {axis(SDL_GAMEPAD_AXIS_LEFTX), axis(SDL_GAMEPAD_AXIS_LEFTY)};
        data.rightStick = {axis(SDL_GAMEPAD_AXIS_RIGHTX), axis(SDL_GAMEPAD_AXIS_RIGHTY)};
        auto region = [](u8 value) { return value < 96 ? 0 : value > 160 ? 2 : 1; };
        const int direction = region(data.leftStick.x) + 3 * region(data.leftStick.y);
        if (direction != last_left_direction) {
            if (trace_input)
                gz_log("Pad: left stick delivered x=%u y=%u", data.leftStick.x, data.leftStick.y);
            last_left_direction = direction;
        }
        data.analogButtons.l2 =
            u8(std::max(0, int(SDL_GetGamepadAxis(device, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) * 255 /
               32767);
        data.analogButtons.r2 =
            u8(std::max(0, int(SDL_GetGamepadAxis(device, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) * 255 /
               32767);
        const std::array<u8, 6> physical_axes{data.leftStick.x,      data.leftStick.y,
                                              data.rightStick.x,     data.rightStick.y,
                                              data.analogButtons.l2, data.analogButtons.r2};
        for (size_t i = 0; i < physical_axes.size(); ++i) {
            const int center = i < 4 ? 128 : 0, threshold = i < 4 ? 40 : 32;
            if (std::abs(int(physical_axes[i]) - center) > threshold &&
                std::abs(int(physical_axes[i]) - previous_physical_axes[i]) > 12)
                gz_pc_activity(1);
        }
        previous_physical_axes = physical_axes;
        const u64 now = SDL_GetTicks();
        if (vibration_dirty ||
            ((vibration.largeMotor || vibration.smallMotor) && now - last_rumble >= 500)) {
            const bool sent = SDL_RumbleGamepad(device, vibration.largeMotor * 257,
                                                vibration.smallMotor * 257, 1000);
            if (rumble_large != vibration.largeMotor || rumble_small != vibration.smallMotor) {
                gz_log("Pad: rumble large=%u small=%u sent=%d%s%s", vibration.largeMotor,
                       vibration.smallMotor, sent,
                       sent ? "" : " error=", sent ? "" : SDL_GetError());
                rumble_large = vibration.largeMotor;
                rumble_small = vibration.smallMotor;
            }
            vibration_dirty = false;
            last_rumble = now;
        }
        if (light_dirty) {
            const bool sent = SDL_SetGamepadLED(device, light.r, light.g, light.b);
            gz_log("Pad: LED rgb=%u,%u,%u sent=%d%s%s", light.r, light.g, light.b, sent,
                   sent ? "" : " error=", sent ? "" : SDL_GetError());
            light_dirty = false;
        }
        for (auto sensor : {SDL_SENSOR_ACCEL, SDL_SENSOR_GYRO}) {
            if (!SDL_GamepadHasSensor(device, sensor))
                continue;
            if (SDL_GamepadSensorEnabled(device, sensor) != motion)
                SDL_SetGamepadSensorEnabled(device, sensor, motion);
            float v[3]{};
            if (motion && SDL_GetGamepadSensorData(device, sensor, v, 3)) {
                if (sensor == SDL_SENSOR_ACCEL)
                    data.acceleration = {v[0] * 0.098f, v[1] * 0.098f, v[2] * 0.098f};
                else
                    data.angularVelocity = {v[0], v[1], v[2]};
            }
        }
    }
    auto keyboard_axis = [](SDL_Scancode negative, SDL_Scancode positive, u8 fallback) {
        if (held[negative] == held[positive])
            return fallback;
        return u8(held[negative] ? 0 : 255);
    };
    // Godzilla turns with L1/R1, not the stick's horizontal axis. Tank-style
    // WASD keeps A/D useful while retaining the physical stick unchanged.
    if (gameplay)
        data.leftStick.y = keyboard_axis(SDL_SCANCODE_W, SDL_SCANCODE_S, data.leftStick.y);
    // Camera is mouse-only on PC; retain the physical controller's right stick.
    const auto now_mouse = SDL_GetTicksNS();
    const double elapsed = previous_mouse_ns ? double(now_mouse - previous_mouse_ns) / 1e9 : .008;
    previous_mouse_ns = now_mouse;
    // Apply finite test-controller samples before photo camera controls, too.
    if (test_input) {
        static GzTestPadLease lease;
        const auto now = SDL_GetTicks();
        lease.Update(test_input, now,
                     [](auto seq, auto b, auto x, auto y, auto z, auto w, auto ttl) {
                         gz_log("TEST INPUT: sequence=%llu buttons=0x%x axes=%u,%u,%u,%u lease=%u",
                                static_cast<unsigned long long>(seq), b, x, y, z, w, ttl);
                     });
        const auto sample = lease.Sample(now);
        buttons = sample.buttons;
        data.leftStick = {sample.axes[0], sample.axes[1]};
        data.rightStick = {sample.axes[2], sample.axes[3]};
        data.analogButtons = {};
    }
    const bool photo_active = GzPhotoNative::Active();
    if (photo_active && GzPhotoNative::InputFocused()) {
        const auto *keyboard = SDL_GetKeyboardState(nullptr);
        const bool camera_mouse = !GzPcOverlay::MouseUi() || photo_drag;
        photo_drag = false;
        const auto axis = [](u8 value) { return GzPhoto::Deadzone((int(value) - 128) / 127.f); };
        const float right = axis(data.leftStick.x) + float(keyboard[SDL_SCANCODE_D]) -
                            float(keyboard[SDL_SCANCODE_A]);
        const float forward = -axis(data.leftStick.y) + float(keyboard[SDL_SCANCODE_W]) -
                              float(keyboard[SDL_SCANCODE_S]);
        const float up = float(keyboard[SDL_SCANCODE_SPACE]) - float(keyboard[SDL_SCANCODE_LCTRL]) +
                         (data.analogButtons.r2 - data.analogButtons.l2) / 255.f;
        const bool boost = keyboard[SDL_SCANCODE_LSHIFT] ||
                           (device && SDL_GetGamepadButton(device, SDL_GAMEPAD_BUTTON_LEFT_STICK));
        const float photo_speed = GzPhoto::photo_channel.Read().settings.speed;
        const float roll =
            float(keyboard[SDL_SCANCODE_E]) - float(keyboard[SDL_SCANCODE_Q]) +
            (device ? float(SDL_GetGamepadButton(device, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) -
                          float(SDL_GetGamepadButton(device, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
                    : 0);
        GzPhotoNative::Motion(right, up, forward,
                              -(camera_mouse ? mouse_dx : 0) * .003f -
                                  axis(data.rightStick.x) * float(elapsed) * 1.8f,
                              -(camera_mouse ? mouse_dy : 0) * .003f -
                                  axis(data.rightStick.y) * float(elapsed) * 1.8f,
                              float(std::clamp(elapsed, 0., .1)),
                              std::min(1e6f, photo_speed * (boost ? 4.f : 1.f)));
        if (camera_mouse)
            GzPhotoNative::Roll(roll * float(std::clamp(elapsed, 0., .1)));
    }
    if (mouse_captured) {
        data.rightStick.x = GzPcControls::MouseAxis(mouse_dx, elapsed, data.rightStick.x);
        data.rightStick.y = GzPcControls::MouseAxis(mouse_dy, elapsed, data.rightStick.y);
    }
    mouse_dx = mouse_dy = 0;
    if (!focused && !test_input) {
        buttons = 0;
        data.leftStick = data.rightStick = {128, 128};
        data.analogButtons = {};
    }
    if (buttons & 0x100)
        data.analogButtons.l2 = 255;
    if (buttons & 0x200)
        data.analogButtons.r2 = 255;
    if (data.analogButtons.l2)
        buttons |= 0x100;
    if (data.analogButtons.r2)
        buttons |= 0x200;
    static GzPhoto::PhotoReleaseGate photo_gate;
    static bool previous_photo{}, previous_share{};
    static u32 previous_photo_buttons{};
    GzPcOverlay::PhotoGamepad(photo_active && GzPhotoNative::InputFocused() ? buttons : 0,
                              device || test_input);
    const auto photo_pressed = buttons & ~previous_photo_buttons;
    previous_photo_buttons = buttons;
    if (photo_active && GzPhotoNative::InputFocused()) {
        if (photo_pressed & 4)
            GzPcOverlay::TogglePhotoPanel();
        if (photo_pressed & 0x8000)
            GzPcOverlay::RequestCapture();
        if (photo_pressed & (0x8 | 0x2000))
            GzPhotoNative::Exit();
    }
    if (device || test_input) {
        // Test input uses the PS4 Share bit (0x1) for the same photo toggle.
        const bool share =
            device ? SDL_GetGamepadButton(device, SDL_GAMEPAD_BUTTON_BACK) : (buttons & 1) != 0;
        buttons &= ~1u;
        if (share && !previous_share)
            GzPhotoNative::Toggle();
        previous_share = share;
    } else {
        previous_share = false;
    }
    if (photo_active != previous_photo) {
        history.clear();
        previous_photo = photo_active;
    }
    const bool neutral = std::abs(int(data.leftStick.x) - 128) < 32 &&
                         std::abs(int(data.leftStick.y) - 128) < 32 &&
                         std::abs(int(data.rightStick.x) - 128) < 32 &&
                         std::abs(int(data.rightStick.y) - 128) < 32 &&
                         data.analogButtons.l2 < 16 && data.analogButtons.r2 < 16;
    if (photo_gate.Block(photo_active, buttons, neutral)) {
        buttons = 0;
        data.leftStick = data.rightStick = {128, 128};
        data.analogButtons = {};
    }
    if (gz_pc_exit_input(buttons, data.leftStick.y)) {
        buttons = 0;
        data.leftStick = data.rightStick = {128, 128};
        data.analogButtons = {};
    }
    if (gz_dialog_input(buttons)) {
        buttons = 0;
        data.leftStick = data.rightStick = {128, 128};
        data.analogButtons = {};
    }
    data.buttons = OrbisPadButtonDataOffset(buttons);
    // shadPS4 also pushes periodic gyro/acceleration samples (SDL timer), even
    // without button changes. Games need fresh states to clear press edges.
    latest = data;
    if (opened) {
        history.push_back(data);
        if (history.size() > 64)
            history.pop_front();
    }
}
namespace Libraries::Pad {
int PS4_SYSV_ABI scePadInit() {
    std::lock_guard lock{mutex};
    initialized = true;
    gz_log("Pad: SDL gamepad and keyboard initialized");
    return 0;
}
int PS4_SYSV_ABI scePadOpen(s32 user, s32 type, s32 index, const OrbisPadOpenParam *) {
    std::lock_guard lock{mutex};
    if (!initialized)
        return ORBIS_PAD_ERROR_NOT_INITIALIZED;
    if (user < 0 || user == 255)
        return ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER;
    if (user != 1)
        return ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN;
    if (type != 0 || index != 0)
        return ORBIS_PAD_ERROR_INVALID_PORT;
    if (opened)
        return ORBIS_PAD_ERROR_ALREADY_OPENED;
    opened = true;
    handle = next_handle++;
    history.clear();
    history.push_back(latest);
    logged_read = logged_state = false;
    gz_log("Pad: user 1 opened controller handle %d", handle);
    return handle;
}
int PS4_SYSV_ABI scePadClose(s32 h) {
    std::lock_guard lock{mutex};
    if (!valid(h))
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    opened = false;
    history.clear();
    vibration = {};
    vibration_dirty = true;
    return 0;
}
int PS4_SYSV_ABI scePadRead(s32 h, OrbisPadData *data, s32 num) {
    std::lock_guard lock{mutex};
    if (!valid(h))
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    if (!data || num < 1 || num > 64)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    int count = std::min(num, int(history.size()));
    for (int i = 0; i < count; ++i) {
        data[i] = history.front();
        history.pop_front();
        const u32 buttons = u32(data[i].buttons);
        if (!logged_read || (trace_input && buttons != last_read_buttons)) {
            gz_log("Pad: scePadRead delivered buttons=0x%x samples=%d requested=%d pending=%llu",
                   buttons, count, num, (unsigned long long)history.size());
            logged_read = true;
            last_read_buttons = buttons;
        }
    }
    return count;
}
int PS4_SYSV_ABI scePadReadState(s32 h, OrbisPadData *data) {
    std::lock_guard lock{mutex};
    if (!valid(h))
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    if (!data)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    *data = latest;
    const u32 buttons = u32(data->buttons);
    if (!logged_state || (trace_input && buttons != last_state_buttons)) {
        gz_log("Pad: scePadReadState delivered buttons=0x%x", buttons);
        logged_state = true;
        last_state_buttons = buttons;
    }
    return 0;
}
int PS4_SYSV_ABI scePadGetControllerInformation(s32 h, OrbisPadControllerInformation *info) {
    std::lock_guard lock{mutex};
    if (!valid(h))
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    if (!info)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    *info = {};
    info->touchPadInfo = {1, {1920, 950}};
    info->stickInfo = {1, 1};
    info->connected = true;
    info->connectedCount = 1;
    info->deviceClass = OrbisPadDeviceClass::Standard;
    return 0;
}
int PS4_SYSV_ABI scePadSetVibration(s32 h, const OrbisPadVibrationParam *p) {
    std::lock_guard lock{mutex};
    if (!valid(h))
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    if (!p)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    vibration = *p;
    vibration_dirty = true;
    return 0;
}
int PS4_SYSV_ABI scePadSetLightBar(s32 h, const OrbisPadLightBarParam *p) {
    std::lock_guard lock{mutex};
    if (!valid(h))
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    if (!p)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    if (!(p->r || p->g || p->b))
        return ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING;
    light = *p;
    light_dirty = true;
    return 0;
}
int PS4_SYSV_ABI scePadResetLightBar(s32 h) {
    OrbisPadLightBarParam p{0, 0, 255, 0};
    return scePadSetLightBar(h, &p);
}
int PS4_SYSV_ABI scePadSetMotionSensorState(s32 h, bool enabled) {
    std::lock_guard lock{mutex};
    if (!valid(h))
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    motion = enabled;
    return 0;
}
// These configure the orientation filter; the current bridge exposes raw sensors only.
int PS4_SYSV_ABI scePadSetTiltCorrectionState(s32 h, bool) {
    std::lock_guard lock{mutex};
    return valid(h) ? 0 : ORBIS_PAD_ERROR_INVALID_HANDLE;
}
int PS4_SYSV_ABI scePadSetAngularVelocityDeadbandState(s32 h, bool) {
    std::lock_guard lock{mutex};
    return valid(h) ? 0 : ORBIS_PAD_ERROR_INVALID_HANDLE;
}
int PS4_SYSV_ABI scePadResetOrientation(s32 h) {
    std::lock_guard lock{mutex};
    if (!valid(h))
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    latest.orientation = {0, 0, 0, 1};
    return 0;
}
void RegisterNative(Core::Loader::SymbolsResolver *sym) {
#define PAD(fn)                                                                                    \
    sym->AddSymbol("", "libScePad", "libScePad", Core::Loader::SymbolType::Function,               \
                   reinterpret_cast<u64>(fn), #fn)
    PAD(scePadInit);
    PAD(scePadOpen);
    PAD(scePadClose);
    PAD(scePadRead);
    PAD(scePadReadState);
    PAD(scePadGetControllerInformation);
    PAD(scePadSetVibration);
    PAD(scePadSetLightBar);
    PAD(scePadResetLightBar);
    PAD(scePadResetOrientation);
    PAD(scePadSetMotionSensorState);
    PAD(scePadSetTiltCorrectionState);
    PAD(scePadSetAngularVelocityDeadbandState);
#undef PAD
}
} // namespace Libraries::Pad
void gz_pad_self_test() {
    auto check = [](bool ok) {
        if (!ok)
            gz_stop(24, "PAD TEST: FAIL");
    };
    check(scePadInit() == 0);
    auto h = scePadOpen(1, 0, 0, nullptr);
    check(h > 0);
    OrbisPadData data{};
    check(scePadRead(h + 1, &data, 1) == ORBIS_PAD_ERROR_INVALID_HANDLE);
    check(scePadRead(h, nullptr, 1) == ORBIS_PAD_ERROR_INVALID_ARG);
    check(scePadRead(h, &data, 65) == ORBIS_PAD_ERROR_INVALID_ARG);
    check(scePadOpen(1, 0, 0, nullptr) == ORBIS_PAD_ERROR_ALREADY_OPENED);
    OrbisPadControllerInformation info{};
    check(scePadGetControllerInformation(h, &info) == 0 && info.connected &&
          info.touchPadInfo.resolution.x == 1920);
    for (bool down : {true, false}) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.scancode = SDL_SCANCODE_SPACE;
        event.key.down = down;
        check(SDL_PushEvent(&event));
        bool seen = false;
        for (int i = 0; i < 200 && !seen; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            check(scePadReadState(h, &data) == 0);
            seen = bool(u32(data.buttons) & 0x4000) == down;
        }
        check(seen && data.connected && data.orientation.w == 1);
        using ProcessClock = u64(PS4_SYSV_ABI *)();
        const auto process_clock = reinterpret_cast<ProcessClock>(
            gz_kernel_resolve("libkernel", "sceKernelGetProcessTime"));
        const auto now = process_clock();
        check(data.timestamp <= now && now - data.timestamp < 100'000);
        // Exercise the history API used by games, not only ReadState. Drain
        // several samples so both press and release must survive consumption.
        bool history_seen = false;
        OrbisPadData samples[64]{};
        const int count = scePadRead(h, samples, 64);
        check(count >= 0);
        for (int i = 0; i < count; ++i)
            if (bool(u32(samples[i].buttons) & 0x4000) == down)
                history_seen = true;
        check(history_seen);
        // A held or released button must continue producing fresh samples;
        // otherwise games can retain a press edge across multiple frames.
        const auto previous_timestamp = samples[count - 1].timestamp;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        const int steady_count = scePadRead(h, samples, 64);
        check(steady_count > 0 && samples[steady_count - 1].timestamp > previous_timestamp &&
              bool(u32(samples[steady_count - 1].buttons) & 0x4000) == down);
    }
    if (const auto *hardware = std::getenv("GZ_PAD_HARDWARE_TEST");
        hardware && hardware[0] == '1') {
        {
            std::lock_guard lock{mutex};
            check(device != nullptr);
        }
        gz_log("PAD HARDWARE TEST: pulses start in 10 seconds");
        std::this_thread::sleep_for(std::chrono::seconds(10));
        for (int motor : {0, 1}) {
            OrbisPadVibrationParam pulse{};
            if (motor == 0)
                pulse.largeMotor = 160;
            else
                pulse.smallMotor = 160;
            OrbisPadLightBarParam color{};
            if (motor == 0)
                color.r = 255;
            else
                color.g = 255;
            check(scePadSetLightBar(h, &color) == 0 && scePadSetVibration(h, &pulse) == 0);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        OrbisPadVibrationParam stop{};
        check(scePadSetVibration(h, &stop) == 0 && scePadResetLightBar(h) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        gz_log("PAD HARDWARE TEST: two motor pulses and red/green/blue LED commands sent; physical "
               "confirmation required");
    }
    check(scePadClose(h) == 0);
    check(scePadRead(h, &data, 1) == ORBIS_PAD_ERROR_INVALID_HANDLE);
    gz_log("PAD TEST: keyboard press/release through SDL and PS4 ABI, invalid handles and bounds "
           "passed");
}
