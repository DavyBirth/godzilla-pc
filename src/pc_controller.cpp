// Template selection contains no guest addresses or controller-library details.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_controller.h"
extern "C" {
#include "gz.h"
}
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
namespace {
std::atomic<int> preference{}, detected{1};
std::once_flag initialized;
std::mutex save_mutex;
auto path() {
    return std::filesystem::u8path(gz_user_dir()) / "pc-controller.txt";
}
void load() {
    try {
        int value = -1;
        std::ifstream source(path());
        if (source >> value && value >= 0 && value <= 3)
            preference = value;
    } catch (...) {
    }
}
} // namespace
extern "C" int gz_pc_controller_preference() {
    std::call_once(initialized, load);
    return preference.load();
}
extern "C" int gz_pc_controller_template() {
    const int value = gz_pc_controller_preference();
    return value ? value : detected.load();
}
extern "C" void gz_pc_controller_detected(int value) {
    if (value >= 1 && value <= 3)
        detected = value;
}
extern "C" int gz_pc_set_controller_preference(int value) {
    if (value < 0 || value > 3)
        return 0;
    std::call_once(initialized, load);
    std::lock_guard lock{save_mutex};
    try {
        const auto file = path();
        std::filesystem::create_directories(file.parent_path());
        const auto temporary = std::filesystem::path(file.wstring() + L".tmp");
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << value << '\n';
            output.close();
            if (!output)
                return 0;
        }
        if (!MoveFileExW(temporary.c_str(), file.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return 0;
        preference = value;
        gz_log("PC controller template: preference=%d effective=%d", value,
               gz_pc_controller_template());
        return 1;
    } catch (const std::exception &error) {
        gz_log("PC controller template: save failed: %s", error.what());
        return 0;
    }
}
