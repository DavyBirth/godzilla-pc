// Native keyboard/mouse bindings, separate from PS4 command masks.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
namespace GzPcControls {
inline constexpr unsigned OptionsMask = 0x8;
inline constexpr unsigned TouchpadMask = 0x100000;
inline constexpr unsigned StartMask(bool movie, bool mission_scene = false, bool modal = false) {
    return !modal && (movie || mission_scene) ? TouchpadMask : OptionsMask;
}
inline constexpr unsigned SystemButtonMask(SDL_GamepadButton button) {
    return button == SDL_GAMEPAD_BUTTON_START      ? OptionsMask
           : button == SDL_GAMEPAD_BUTTON_TOUCHPAD ? TouchpadMask
                                                   : 0;
}
struct MenuWheel {
    int steps{};
    unsigned button{};
    uint64_t deadline{}, next{};
    void Add(float amount) {
        if (std::isfinite(amount) && amount)
            steps = std::clamp(steps + (amount > 0 ? 1 : -1), -8, 8);
    }
    void Clear() {
        steps = 0;
        button = 0;
        deadline = next = 0;
    }
    unsigned Sample(bool gameplay, uint64_t now) {
        if (gameplay) {
            Clear();
            return 0;
        }
        if (button && now >= deadline) {
            button = 0;
            next = now + 40;
        }
        if (!button && steps && now >= next) {
            button = steps > 0 ? 0x10 : 0x40;
            steps += steps > 0 ? -1 : 1;
            deadline = now + 60;
        }
        return button;
    }
};
// A press keeps its action until release; changing context must never create
// another press (e.g. skip a movie and immediately pause the newly entered menu).
struct ContextKey {
    unsigned mask{};
    bool held{}, blocked{};
    void Press(unsigned value) {
        if (!held) {
            mask = value;
            held = true;
            blocked = false;
        }
    }
    void Release() {
        mask = 0;
        held = blocked = false;
    }
    unsigned Sample(unsigned expected) {
        if (!held)
            return 0;
        if (mask != expected)
            blocked = true;
        return blocked ? 0 : mask;
    }
};
inline unsigned KeyMask(SDL_Scancode key, bool gameplay = true, bool movie = false,
                        bool modal = false) {
    switch (key) {
    case SDL_SCANCODE_UP:
        return 0x10;
    case SDL_SCANCODE_RIGHT:
        return 0x20;
    case SDL_SCANCODE_DOWN:
        return 0x40;
    case SDL_SCANCODE_LEFT:
        return 0x80;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        return 0x4000;
    case SDL_SCANCODE_SPACE:
        return gameplay ? 0x200 : 0x4000;
    case SDL_SCANCODE_BACKSPACE:
    case SDL_SCANCODE_C:
        return 0x2000;
    case SDL_SCANCODE_Z:
        return 0x8000;
    case SDL_SCANCODE_X:
        return 0x1000;
    case SDL_SCANCODE_A:
        return gameplay ? 0x400 : 0x80;
    case SDL_SCANCODE_D:
        return gameplay ? 0x800 : 0x20;
    case SDL_SCANCODE_W:
        return gameplay ? 0 : 0x10;
    case SDL_SCANCODE_S:
        return gameplay ? 0 : 0x40;
    case SDL_SCANCODE_Q:
        return 0x400;
    case SDL_SCANCODE_E:
        return 0x800;
    case SDL_SCANCODE_R:
        return 0xc00;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
        return gameplay ? 0x4000 : 0;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
        return 0x100;
    case SDL_SCANCODE_G:
        return 2;
    case SDL_SCANCODE_F:
        return 4;
    case SDL_SCANCODE_ESCAPE:
        return modal ? 0x2000 : (movie || gameplay) ? TouchpadMask : 0x2000;
    case SDL_SCANCODE_TAB:
        return movie ? 0 : TouchpadMask;
    default:
        return 0;
    }
}
inline unsigned MouseMask(unsigned button, bool gameplay) {
    if (!gameplay)
        return 0;
    if (button == SDL_BUTTON_LEFT)
        return 0x8000;
    if (button == SDL_BUTTON_RIGHT)
        return 0x1000;
    if (button == SDL_BUTTON_MIDDLE)
        return 0x2000;
    return 0;
}
inline float Sensitivity() {
    const char *s = std::getenv("GZ_MOUSE_SENSITIVITY");
    if (!s)
        return 1.8f;
    char *end{};
    const float value = std::strtof(s, &end);
    return end && !*end && std::isfinite(value) ? std::clamp(value, .2f, 5.f) : 1.8f;
}
inline unsigned char MouseAxis(float distance, double seconds, unsigned char fallback) {
    if (!std::isfinite(distance) || !std::isfinite(seconds) || distance == 0 || seconds <= 0 ||
        seconds > .1)
        return fallback;
    const float value =
        static_cast<float>(distance / std::max(.001, seconds) * .09) * Sensitivity();
    const float magnitude = std::min(127.f, 24.f + std::abs(value));
    return static_cast<unsigned char>(
        std::clamp(128 + static_cast<int>(std::copysign(magnitude, value)), 0, 255));
}
} // namespace GzPcControls
