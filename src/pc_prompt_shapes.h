/* Shared vector strokes for GDI dialogs and the ImGui photo menu. */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
typedef struct {
    float x1, y1, x2, y2;
} GzPromptStroke;
enum GzPromptShape { GZ_PROMPT_CROSS, GZ_PROMPT_RETURN };
static inline const GzPromptStroke *gz_prompt_strokes(enum GzPromptShape shape, size_t *count) {
    static const GzPromptStroke cross[] = {{.3f, .3f, .7f, .7f}, {.7f, .3f, .3f, .7f}};
    static const GzPromptStroke enter[] = {{.78f, .24f, .78f, .58f},
                                           {.78f, .58f, .23f, .58f},
                                           {.23f, .58f, .40f, .40f},
                                           {.23f, .58f, .40f, .76f}};
    if (shape == GZ_PROMPT_RETURN) {
        *count = 4;
        return enter;
    }
    *count = 2;
    return cross;
}
