// Native PC feature boundary; no guest addresses exposed to the frontend.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void gz_pc_configure(void *image, uint64_t size);
int gz_pc_gameplay(void);
int gz_pc_action_prompts(void); // Visual context, including in-mission explanations.
int gz_pc_mission_scene(void);
int gz_pc_main_menu(int *cursor);
int gz_pc_capture_point(void);
int gz_pc_exit_input(uint32_t buttons, unsigned stick_y);
int gz_pc_exit_selected(void);
int gz_pc_native_exit_row(void);
int gz_pc_take_quit(void);
void gz_pc_activity(int controller);
int gz_pc_controller(void);
int gz_pc_controller_active(void);
void gz_pc_release_input(void);
#ifdef __cplusplus
}
#endif
