/* Native input symbols; no dependency on Unicode icon coverage. */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <windows.h>
#include "pc_prompt_shapes.h"
static inline void gz_prompt_gdi_strokes(HDC dc, enum GzPromptShape shape, int x, int y, int width,
                                         int height) {
    size_t count;
    const GzPromptStroke *lines = gz_prompt_strokes(shape, &count);
    for (size_t i = 0; i < count; ++i) {
        MoveToEx(dc, x + (int)(lines[i].x1 * width + .5f), y + (int)(lines[i].y1 * height + .5f),
                 NULL);
        LineTo(dc, x + (int)(lines[i].x2 * width + .5f), y + (int)(lines[i].y2 * height + .5f));
    }
}
static inline int gz_prompt_gdi_button(HDC dc, int x, int y, int controller, int style, int back) {
    int width = controller ? 24 : back ? 36 : 30;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(215, 210, 200));
    HBRUSH brush = CreateSolidBrush(RGB(30, 28, 30));
    HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, brush);
    if (controller)
        Ellipse(dc, x, y, x + 24, y + 24);
    else
        RoundRect(dc, x, y, x + width, y + 24, 5, 5);
    if (controller && style == 1) {
        HPEN face = CreatePen(PS_SOLID, 2, back ? RGB(230, 80, 80) : RGB(110, 150, 230));
        SelectObject(dc, face);
        if (back) {
            SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Ellipse(dc, x + 6, y + 6, x + 18, y + 18);
        } else
            gz_prompt_gdi_strokes(dc, GZ_PROMPT_CROSS, x, y, 24, 24);
        SelectObject(dc, pen);
        DeleteObject(face);
    } else if (controller || back) {
        RECT label = {x, y, x + width, y + 24};
        const wchar_t *text =
            controller ? (style == 3 ? (back ? L"A" : L"B") : (back ? L"B" : L"A")) : L"Esc";
        SetTextColor(dc, RGB(230, 225, 215));
        DrawTextW(dc, text, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else
        gz_prompt_gdi_strokes(dc, GZ_PROMPT_RETURN, x, y, width, 24);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    return width;
}
static inline void gz_prompt_gdi_hint(HDC dc, int width, int y, int controller, int style) {
    SIZE confirm, back;
    GetTextExtentPoint32W(dc, L"CONFIRMAR", 9, &confirm);
    GetTextExtentPoint32W(dc, L"VOLTAR", 6, &back);
    int button = controller ? 24 : 30, back_button = controller ? 24 : 36;
    int x = (width - (button + 8 + confirm.cx + 28 + back_button + 8 + back.cx)) / 2;
    x += gz_prompt_gdi_button(dc, x, y, controller, style, 0) + 8;
    SetTextColor(dc, RGB(200, 190, 165));
    TextOutW(dc, x, y + 3, L"CONFIRMAR", 9);
    x += confirm.cx + 28;
    x += gz_prompt_gdi_button(dc, x, y, controller, style, 1) + 8;
    SetTextColor(dc, RGB(200, 190, 165));
    TextOutW(dc, x, y + 3, L"VOLTAR", 6);
}
