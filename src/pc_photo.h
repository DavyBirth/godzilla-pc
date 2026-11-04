// PNG capture independent of the guest and Vulkan presentation.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vector>
#include <string>
#include "pc_photo_grade.h"
namespace GzPcPhotos {
void Save(std::vector<unsigned char> rgba, unsigned width, unsigned height,
          GzPhoto::Grade grade = {});
void Finish();
std::string TakeNotice();
} // namespace GzPcPhotos
