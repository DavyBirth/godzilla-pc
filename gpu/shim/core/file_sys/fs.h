// Only the path translation surface used by AvPlayer; native runtime owns mounts.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <string_view>
#include "common/singleton.h"
extern "C" int gz_file_translate(const char *, char *, size_t);
namespace Core::FileSys {
class MntPoints {
  public:
    std::filesystem::path GetHostPath(std::string_view guest) {
        char path[1024];
        if (gz_file_translate(std::string{guest}.c_str(), path, sizeof(path)))
            return {};
        return std::filesystem::u8path(path);
    }
};
} // namespace Core::FileSys
