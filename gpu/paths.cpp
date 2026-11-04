// Keep all GPU cache files inside the runtime's isolated user folder.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <map>
#include <mutex>
#include "common/path_util.h"
extern "C" {
#include "gz.h"
}
namespace Common::FS {
static std::map<PathType, std::filesystem::path> paths;
static std::mutex path_mutex;
const std::filesystem::path &GetUserPath(PathType type) {
    std::lock_guard lock{path_mutex};
    auto it = paths.find(type);
    if (it != paths.end())
        return it->second;
    const char *sub = type == PathType::CacheDir    ? "cache"
                      : type == PathType::ShaderDir ? "shader"
                      : type == PathType::LogDir    ? "gpu_logs"
                                                    : "gpu";
    auto path = std::filesystem::u8path(gz_user_dir()) / sub;
    std::filesystem::create_directories(path);
    return paths.emplace(type, std::move(path)).first->second;
}
std::string PathToUTF8String(const std::filesystem::path &path) {
    auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
std::string GetUserPathString(PathType type) {
    return PathToUTF8String(GetUserPath(type));
}
void SetUserPath(PathType type, const std::filesystem::path &path) {
    std::lock_guard lock{path_mutex};
    paths[type] = path;
}
bool ValidatePath(const std::filesystem::path &path) {
    return !path.empty();
}
} // namespace Common::FS
