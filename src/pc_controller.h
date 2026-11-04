// One template preference shared by the launcher, in-game UI and Pad bridge.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// 0 automatic, 1 PlayStation, 2 Xbox, 3 Nintendo.
int gz_pc_controller_preference(void);
int gz_pc_controller_template(void);
int gz_pc_set_controller_preference(int value);
void gz_pc_controller_detected(int value);
#ifdef __cplusplus
}
namespace GzPcController {
inline unsigned SouthMask(int type) {
    return type == 3 ? 0x2000u : 0x4000u;
}
inline unsigned EastMask(int type) {
    return type == 3 ? 0x4000u : 0x2000u;
}
struct ReleaseGate {
    int previous{};
    bool blocked{};
    unsigned Apply(int type, unsigned buttons) {
        if (previous && previous != type)
            blocked = true;
        previous = type;
        if (blocked) {
            if (!(buttons & 0x6000u))
                blocked = false;
            return buttons & ~0x6000u;
        }
        return buttons;
    }
};
} // namespace GzPcController
#endif
