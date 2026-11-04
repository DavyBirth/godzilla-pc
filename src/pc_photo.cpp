// Bounded asynchronous PNG writer. Publish only complete files; drain on exit.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_photo.h"
extern "C" {
#include "gz.h"
}
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <algorithm>
#include <utility>
namespace GzPcPhotos {
namespace {
std::mutex mutex, notice_mutex;
std::condition_variable done;
unsigned pending{};
bool closing{};
std::string notice;
void report(const char *text) {
    std::lock_guard lock{notice_mutex};
    notice = text;
}
void complete() {
    std::lock_guard lock{mutex};
    --pending;
    done.notify_all();
}
void write(std::vector<unsigned char> rgba, unsigned width, unsigned height, GzPhoto::Grade grade) {
    struct Complete {
        ~Complete() {
            complete();
        }
    } completion;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token{};
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        report("Falha ao iniciar gravacao PNG.");
        return;
    }
    std::filesystem::path temporary;
    try {
        const auto directory = std::filesystem::u8path(gz_user_dir()) / "photos";
        std::filesystem::create_directories(directory);
        static std::atomic<unsigned> sequence{};
        const auto path = directory / ("Godzilla-" + std::to_string(GetTickCount64()) + "-" +
                                       std::to_string(++sequence) + ".png");
        temporary = std::filesystem::path(path.wstring() + L".tmp");
        GzPhoto::GradePixels(rgba.data(), rgba.size(), false, grade, width);
        for (size_t i = 0; i < rgba.size(); i += 4)
            std::swap(rgba[i], rgba[i + 2]);
        Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppARGB, rgba.data());
        UINT count{}, bytes{};
        Gdiplus::GetImageEncodersSize(&count, &bytes);
        std::vector<unsigned char> memory(bytes);
        auto *codecs = reinterpret_cast<Gdiplus::ImageCodecInfo *>(memory.data());
        bool saved = false;
        if (bytes && Gdiplus::GetImageEncoders(count, bytes, codecs) == Gdiplus::Ok)
            for (unsigned i = 0; i < count; ++i)
                if (!std::wcscmp(codecs[i].MimeType, L"image/png")) {
                    saved =
                        bitmap.Save(temporary.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
                    break;
                }
        if (saved) {
            std::filesystem::rename(temporary, path);
            report("Foto PNG salva em userdata/photos.");
            gz_log("Photo: saved %s (%ux%u)", path.string().c_str(), width, height);
        } else
            report("Falha ao salvar a foto PNG.");
    } catch (const std::exception &error) {
        report("Falha ao gravar a foto.");
        gz_log("Photo: %s", error.what());
    }
    if (!temporary.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    Gdiplus::GdiplusShutdown(token);
}
} // namespace
void Save(std::vector<unsigned char> rgba, unsigned width, unsigned height, GzPhoto::Grade grade) {
    if (!width || !height || width > 16384 || height > 16384 ||
        rgba.size() != uint64_t(width) * height * 4) {
        report("Foto: imagem indisponivel.");
        return;
    }
    if (!GzPhoto::Valid(grade)) {
        report("Foto: ajustes invalidos.");
        return;
    }
    {
        std::lock_guard lock{mutex};
        if (closing)
            return;
        if (pending >= 2) {
            report("Foto: aguarde a gravacao anterior.");
            return;
        }
        ++pending;
    }
    try {
        std::thread(write, std::move(rgba), width, height, grade).detach();
    } catch (const std::exception &) {
        complete();
        report("Falha ao iniciar gravacao da foto.");
    }
}
void Finish() {
    std::unique_lock lock{mutex};
    closing = true;
    done.wait(lock, [] { return pending == 0; });
}
std::string TakeNotice() {
    std::lock_guard lock{notice_mutex};
    return std::exchange(notice, {});
}
} // namespace GzPcPhotos
