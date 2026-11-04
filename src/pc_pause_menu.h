// Native pause extension, queued on the scene thread. GPL-2.0-or-later.
#pragma once
#include <cstdint>
namespace GzPauseMenu {
void Configure(void *image, uint64_t size);
void Tick();
} // namespace GzPauseMenu
