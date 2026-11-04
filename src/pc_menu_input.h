// Host row appended to the seven native main-menu rows.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
struct GzNativeExitInput {
    uint32_t previous{};
    bool blocked{};
    bool Confirm(bool main, int cursor, uint32_t buttons, int action_row = 7) {
        const auto pressed = buttons & ~previous;
        previous = buttons;
        if (!(buttons & 0x4000))
            blocked = false;
        const bool confirm = main && cursor == action_row && (pressed & 0x4000);
        if (confirm)
            blocked = true;
        return confirm;
    }
    void Release() {
        previous = 0;
        blocked = false;
    }
};
struct GzPcMenuInput {
    bool selected{}, quit{};
    uint32_t previous_buttons{};
    bool previous_down{}, previous_up{}, release_gate{};
    bool Update(bool main, int cursor, uint32_t buttons, unsigned stick_y) {
        const bool down = (buttons & 0x40) || stick_y >= 200,
                   up = (buttons & 0x10) || stick_y <= 56;
        const auto pressed = buttons & ~previous_buttons;
        const bool down_edge = down && !previous_down, up_edge = up && !previous_up;
        previous_buttons = buttons;
        previous_down = down;
        previous_up = up;
        if (!main) {
            selected = false;
            release_gate = false;
            return false;
        }
        if (release_gate) {
            if (buttons || down || up)
                return true;
            release_gate = false;
        }
        if (!selected) {
            if (cursor == 6 && down_edge) {
                selected = true;
                return true;
            }
            return false;
        }
        if (up_edge) {
            selected = false;
            release_gate = true;
        } else if (pressed & 0x4000)
            quit = true;
        else if (pressed & 0x2000) {
            selected = false;
            release_gate = true;
        }
        return true;
    }
    void Release() {
        previous_buttons = 0;
        previous_down = previous_up = release_gate = false;
    }
};
