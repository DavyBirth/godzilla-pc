#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int gz_dialog_show(const wchar_t *text, unsigned type);
int gz_dialog_active(void);
int gz_dialog_input(uint32_t buttons);
void gz_dialog_cancel(void);
void gz_dialog_prepare(void);
/* type 256 displays progress and ignores confirmation; caller closes it. */
void gz_dialog_progress(unsigned percent);
#ifdef __cplusplus
}
#endif
