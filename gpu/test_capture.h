// Optional test observations from the GPU, without capturing the desktop.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

inline void gz_test_publish(const std::filesystem::path &from, const std::filesystem::path &to) {
#ifdef _WIN32
    MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    std::error_code error;
    std::filesystem::rename(from, to, error);
#endif
}

template <typename Capture>
void gz_test_observe_frame(Capture capture, unsigned width, unsigned height) {
    static const char *directory = std::getenv("GZ_TEST_CAPTURE_DIR");
    if (!directory || !*directory)
        return;
    using Clock = std::chrono::steady_clock;
    static auto last_poll = Clock::now() - std::chrono::seconds(1);
    static auto last_status = last_poll;
    static unsigned long long frames{};
    ++frames;
    const auto now = Clock::now();
    if (now - last_poll < std::chrono::milliseconds(100))
        return;
    last_poll = now;
    const auto root = std::filesystem::u8path(directory);
    std::error_code error;
    if (now - last_status >= std::chrono::seconds(1)) {
        last_status = now;
        std::ofstream status(root / "heartbeat.tmp");
        status << "{\"frames\":" << frames << ",\"width\":" << width << ",\"height\":" << height
               << "}\n";
        status.close();
        if (status)
            gz_test_publish(root / "heartbeat.tmp", root / "heartbeat.json");
    }
    std::ifstream request(root / "request.txt");
    std::string name;
    if (!std::getline(request, name))
        return;
    request.close();
    std::filesystem::remove(root / "request.txt", error);
    if (name.empty() || name.size() > 64 ||
        name.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos)
        return;
    if (std::filesystem::exists(root / (name + ".ppm"), error))
        return;
    std::vector<unsigned char> pixels;
    unsigned w{}, h{};
    if (!capture(pixels, w, h) || pixels.size() != size_t(w) * h * 4)
        return;
    std::ofstream output(root / (name + ".ppm.tmp"), std::ios::binary);
    output << "P6\n" << w << ' ' << h << "\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4)
        output.write(reinterpret_cast<const char *>(pixels.data() + i), 3);
    output.close();
    if (output)
        std::filesystem::rename(root / (name + ".ppm.tmp"), root / (name + ".ppm"), error);
}
