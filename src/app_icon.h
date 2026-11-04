// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
int gz_set_game_icon(HWND window, const wchar_t *png_path);
void gz_release_game_icon(HWND window);
#ifdef __cplusplus
}
#endif
