/* Host dialog surface embedded in the game window; independent of guest ABI. */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdint.h>
#include "dialog_ui.h"
#include "pc_features.h"
#include "pc_controller.h"
#include "pc_prompt_gdi.h"
#ifdef GZ_GPU
#include "gpu.h"
#endif
static HWND panel;
static volatile LONG active;
static volatile LONG canceled;
static volatile LONG progress;
static uint32_t previous_buttons;
static int release_gate;
typedef struct {
    const wchar_t *text;
    unsigned type;
    int selected, choices, result, done, prompt_device, prompt_template;
} Dialog;
#define INPUT_MESSAGE (WM_APP + 10)
static void choose(HWND window, Dialog *d, int result) {
    d->result = result;
    d->done = 1;
    DestroyWindow(window);
}
static void navigate(HWND window, Dialog *d, uint32_t pressed) {
    if (!d->choices)
        return;
    if (pressed & (0x10 | 0x20 | 0x40 | 0x80)) {
        d->selected = (d->selected + 1) % d->choices;
        InvalidateRect(window, NULL, FALSE);
    }
    if (pressed & (0x4000 | 8))
        choose(window, d, d->selected + 1);
    else if (pressed & 0x2000)
        choose(window, d, d->choices == 2 ? 2 : 1);
}
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    Dialog *d = (Dialog *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        d = ((CREATESTRUCTW *)l)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)d);
    }
    if (!d)
        return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_DESTROY:
        d->done = 1;
        return 0;
    case WM_TIMER: {
        if (d->type == 256)
            InvalidateRect(window, NULL, FALSE);
        int device = gz_pc_controller(), style = gz_pc_controller_template();
        if (device != d->prompt_device || style != d->prompt_template) {
            d->prompt_device = device;
            d->prompt_template = style;
            InvalidateRect(window, NULL, FALSE);
        }
        RECT parent, current;
        GetClientRect(GetParent(window), &parent);
        GetClientRect(window, &current);
        int width = parent.right > 720 ? 680 : parent.right - 32;
        int height = parent.bottom > 440 ? 340 : parent.bottom - 32;
        if (width > 0 && height > 0 && (width != current.right || height != current.bottom))
            SetWindowPos(window, HWND_TOP, (parent.right - width) / 2, (parent.bottom - height) / 2,
                         width, height, SWP_NOACTIVATE);
        return 0;
    }
    case INPUT_MESSAGE:
        navigate(window, d, (uint32_t)w);
        return 0;
    case WM_KEYDOWN:
        if (!(l & (1L << 30))) {
            uint32_t key = w == VK_RETURN || w == VK_SPACE                               ? 0x4000
                           : w == VK_ESCAPE                                              ? 0x2000
                           : w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN ? 0x10
                                                                                         : 0;
            navigate(window, d, key);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!d->choices)
            return 0;
        RECT r;
        GetClientRect(window, &r);
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (y > r.bottom - 112 && y < r.bottom - 36) {
            (void)x;
            d->selected = d->choices == 2 && y > r.bottom - 74 ? 1 : 0;
            choose(window, d, d->selected + 1);
        }
        return 0;
    }
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        /* Same look as the game's PAUSA panel: dark box, gold rule and frame,
           italic gold header, cream body, orange selection bar. */
        PAINTSTRUCT p;
        HDC dc = message == WM_PRINTCLIENT ? (HDC)w : BeginPaint(window, &p);
        RECT r;
        GetClientRect(window, &r);
        HBRUSH background = CreateSolidBrush(RGB(18, 18, 21));
        FillRect(dc, &r, background);
        DeleteObject(background);
        HBRUSH frame = CreateSolidBrush(RGB(150, 112, 58));
        FrameRect(dc, &r, frame);
        RECT inner = {3, 3, r.right - 3, r.bottom - 3};
        FrameRect(dc, &inner, frame);
        DeleteObject(frame);
        SetBkMode(dc, TRANSPARENT);
        HFONT header = CreateFontW(-22, 0, 0, 0, FW_BOLD, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0,
                                   CLEARTYPE_QUALITY, 0, L"Bahnschrift");
        HFONT body = CreateFontW(-21, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT item = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, L"Bahnschrift");
        HFONT hintf = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, 0, L"Bahnschrift");
        HGDIOBJ old = SelectObject(dc, header);
        RECT square = {22, 24, 34, 36};
        HBRUSH blue = CreateSolidBrush(RGB(90, 140, 200));
        FillRect(dc, &square, blue);
        DeleteObject(blue);
        SetTextColor(dc, RGB(214, 190, 138));
        RECT title = {42, 16, r.right - 20, 46};
        DrawTextW(dc, d->type == 256 ? L"A G U A R D E" : L"A V I S O", -1, &title,
                  DT_SINGLELINE | DT_VCENTER);
        RECT rule = {16, 52, r.right - 16, 54};
        HBRUSH gold = CreateSolidBrush(RGB(200, 150, 70));
        FillRect(dc, &rule, gold);
        DeleteObject(gold);
        SelectObject(dc, body);
        SetTextColor(dc, RGB(235, 225, 200));
        RECT text = {32, 72, r.right - 32, r.bottom - 120};
        RECT measure = text;
        DrawTextW(dc, d->text, -1, &measure, DT_WORDBREAK | DT_NOPREFIX | DT_CENTER | DT_CALCRECT);
        int shift = ((text.bottom - text.top) - (measure.bottom - measure.top)) / 2;
        if (shift > 0)
            text.top += shift;
        DrawTextW(dc, d->text, -1, &text, DT_WORDBREAK | DT_NOPREFIX | DT_CENTER);
        if (d->type == 256) {
            RECT track = {48, r.bottom - 92, r.right - 48, r.bottom - 82};
            HBRUSH track_brush = CreateSolidBrush(RGB(55, 50, 45));
            FillRect(dc, &track, track_brush);
            DeleteObject(track_brush);
            unsigned percent = (unsigned)InterlockedCompareExchange(&progress, 0, 0);
            RECT fill = track;
            fill.right = fill.left + (track.right - track.left) * percent / 100;
            HBRUSH fill_brush = CreateSolidBrush(RGB(240, 110, 20));
            FillRect(dc, &fill, fill_brush);
            DeleteObject(fill_brush);
        }
        SelectObject(dc, item);
        for (int i = 0; i < d->choices; ++i) {
            int y = r.bottom - 112 + i * 38 - (d->choices == 1 ? -19 : 0);
            RECT row = {40, y, r.right - 40, y + 34};
            if (i == d->selected) {
                /* Orange bar fading at both ends, as the game's selected row. */
                int w = row.right - row.left;
                for (int x = 0; x < w; x += 4) {
                    double edge = x < w / 2 ? x : w - x;
                    double a = edge > 90 ? 1 : edge / 90;
                    RECT slice = {row.left + x, row.top, row.left + x + 4, row.bottom};
                    HBRUSH b =
                        CreateSolidBrush(RGB((int)(18 + (240 - 18) * a), (int)(18 + (110 - 18) * a),
                                             (int)(21 + (20 - 21) * a)));
                    FillRect(dc, &slice, b);
                    DeleteObject(b);
                }
            }
            SetTextColor(dc, i == d->selected ? RGB(255, 245, 225) : RGB(230, 215, 180));
            const wchar_t *label = d->choices == 1                ? L"OK"
                                   : d->type == 1 || d->type == 7 ? (i ? L"Não" : L"Sim")
                                                                  : (i ? L"Cancelar" : L"OK");
            DrawTextW(dc, label, -1, &row, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, hintf);
        SetTextColor(dc, RGB(200, 190, 165));
        if (d->type != 256)
            gz_prompt_gdi_hint(dc, r.right, r.bottom - 32, gz_pc_controller(),
                               gz_pc_controller_template());
        SelectObject(dc, old);
        DeleteObject(header);
        DeleteObject(body);
        DeleteObject(item);
        DeleteObject(hintf);
        if (message == WM_PAINT)
            EndPaint(window, &p);
        else
            GdiFlush();
        return 0;
    }
    case WM_CLOSE:
        choose(window, d, 0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
int gz_dialog_active(void) {
    return InterlockedCompareExchange(&active, 0, 0) != 0;
}
int gz_dialog_input(uint32_t buttons) {
    int running = gz_dialog_active();
    uint32_t pressed = buttons & ~previous_buttons;
    previous_buttons = buttons;
    if (running) {
        release_gate = 1;
        HWND window = (HWND)InterlockedCompareExchangePointer((void *volatile *)&panel, NULL, NULL);
        if (window && pressed)
            PostMessageW(window, INPUT_MESSAGE, pressed, 0);
        return 1;
    }
    if (release_gate) {
        if (!buttons)
            release_gate = 0;
        return 1;
    }
    return 0;
}
void gz_dialog_cancel(void) {
    InterlockedExchange(&canceled, 1);
    HWND window = (HWND)InterlockedCompareExchangePointer((void *volatile *)&panel, NULL, NULL);
    if (window)
        PostMessageW(window, WM_CLOSE, 0, 0);
}
void gz_dialog_prepare(void) {
    InterlockedExchange(&canceled, 0);
    InterlockedExchange(&progress, 0);
}
void gz_dialog_progress(unsigned percent) {
    InterlockedExchange(&progress, percent > 100 ? 100 : percent);
}
int gz_dialog_show(const wchar_t *text, unsigned type) {
    if (InterlockedCompareExchange(&canceled, 0, 0))
        return 0;
    HWND parent = NULL;
#ifdef GZ_GPU
    parent = (HWND)gz_gpu_window_handle();
#endif
    if (!parent)
        return 0;
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = procedure;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"GodzillaEmbeddedDialog";
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    RegisterClassW(&cls);
    RECT r;
    GetClientRect(parent, &r);
    int width = r.right > 720 ? 680 : r.right - 32;
    int height = r.bottom > 440 ? 340 : r.bottom - 32;
    if (width <= 0 || height <= 0)
        return 0;
    Dialog d = {text, type, type == 7 || type == 8, type == 256 ? 0 : type == 0 ? 1 : 2,
                0,    0,    gz_pc_controller(),     gz_pc_controller_template()};
    SetWindowLongPtrW(parent, GWL_STYLE, GetWindowLongPtrW(parent, GWL_STYLE) | WS_CLIPCHILDREN);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  (r.right - width) / 2, (r.bottom - height) / 2, width, height,
                                  parent, NULL, cls.hInstance, &d);
    if (!window)
        return 0;
    if (InterlockedCompareExchange(&canceled, 0, 0)) {
        DestroyWindow(window);
        return 0;
    }
    InterlockedExchangePointer((void *volatile *)&panel, window);
    InterlockedExchange(&active, 1);
    SetTimer(window, 1, 100, NULL);
    SetFocus(window);
    MSG message;
    while (!d.done && GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    InterlockedExchange(&active, 0);
    InterlockedExchangePointer((void *volatile *)&panel, NULL);
    if (IsWindow(window))
        DestroyWindow(window);
    return d.result == 2 ? type == 1 || type == 7 ? IDNO : IDCANCEL : d.result == 1 ? IDOK : 0;
}
