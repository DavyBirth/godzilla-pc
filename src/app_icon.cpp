// Load the player's original icon at runtime; never embed game artwork.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include "app_icon.h"

extern "C" int gz_set_game_icon(HWND window, const wchar_t *png_path) {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
        return 0;
    HICON icon = nullptr;
    {
        Gdiplus::Bitmap bitmap(png_path);
        if (bitmap.GetLastStatus() == Gdiplus::Ok)
            bitmap.GetHICON(&icon);
    }
    Gdiplus::GdiplusShutdown(token);
    if (!icon)
        return 0;
    if (!SetPropW(window, L"GodzillaOwnedIcon", icon)) {
        DestroyIcon(icon);
        return 0;
    }
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    return 1;
}
extern "C" void gz_release_game_icon(HWND window) {
    if (auto icon = static_cast<HICON>(RemovePropW(window, L"GodzillaOwnedIcon"))) {
        SendMessageW(window, WM_SETICON, ICON_BIG, 0);
        SendMessageW(window, WM_SETICON, ICON_SMALL, 0);
        DestroyIcon(icon);
    }
}
