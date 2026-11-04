/* Stable host interface, usable by future reimplemented game code as well.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GZ_GPU_H
#define GZ_GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void gz_gpu_start(uint32_t sdk_version, int headless);
uintptr_t gz_gpu_resolve(const char *library, const char *symbol);
int gz_gpu_handle_fault(void *exception, void *address);
void gz_gpu_map(uintptr_t address, uint64_t size);
void gz_gpu_unmap(uintptr_t address, uint64_t size);
void gz_gpu_invalidate(uintptr_t address, uint64_t size);
void gz_gpu_prepare_write(void *address, uint64_t size);
void gz_gpu_prepare_read(const void *address, uint64_t size);
/* Explicit image capture for CPU consumers; no game-specific addresses.
 * Call on a CPU thread, never the GPU worker. Destination is writable CPU memory. */
int gz_gpu_read_image_rgba(const void *source, uint32_t width, uint32_t height, void *destination,
                           uint64_t capacity);
void gz_gpu_self_test(void);
void *gz_gpu_window_handle(void);
#ifdef __cplusplus
}
#endif
#endif
