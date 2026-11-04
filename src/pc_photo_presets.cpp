// Atomic local preset storage, separate from the game's savedata.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_photo_presets.h"
extern "C" {
#include "gz.h"
}
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
namespace GzPhoto {
namespace {
std::mutex preset_mutex;
std::atomic<unsigned> sequence{};
auto path(unsigned slot) {
    return std::filesystem::u8path(gz_user_dir()) / "photos" / "presets" /
           ("slot-" + std::to_string(slot) + ".txt");
}
struct Temporary {
    std::filesystem::path path;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool owned{};
    void Close() {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
    }
    ~Temporary() {
        Close();
        if (owned) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};
} // namespace
bool SavePreset(unsigned slot, const Preset &preset) {
    if (slot < 1 || slot > 8)
        return false;
    const auto text = EncodePreset(preset);
    if (!text)
        return false;
    std::lock_guard lock{preset_mutex};
    try {
        const auto target = path(slot);
        std::filesystem::create_directories(target.parent_path());
        Temporary temporary;
        temporary.path = target.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                         std::to_wstring(++sequence) + L".tmp";
        temporary.file = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary.file == INVALID_HANDLE_VALUE)
            return false;
        temporary.owned = true;
        DWORD written{};
        if (!WriteFile(temporary.file, text->data(), DWORD(text->size()), &written, nullptr) ||
            written != text->size() || !FlushFileBuffers(temporary.file))
            return false;
        temporary.Close();
        return MoveFileExW(temporary.path.c_str(), target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    } catch (const std::exception &error) {
        gz_log("Photo preset save failed: %s", error.what());
        return false;
    }
}
std::optional<Preset> LoadPreset(unsigned slot) {
    if (slot < 1 || slot > 8)
        return {};
    std::lock_guard lock{preset_mutex};
    try {
        std::ifstream input(path(slot), std::ios::binary);
        if (!input)
            return {};
        char text[4097];
        input.read(text, sizeof(text));
        const auto bytes = input.gcount();
        if (input.bad() || bytes > 4096)
            return {};
        return DecodePreset(std::string_view(text, size_t(bytes)));
    } catch (const std::exception &error) {
        gz_log("Photo preset load failed: %s", error.what());
        return {};
    }
}
} // namespace GzPhoto
