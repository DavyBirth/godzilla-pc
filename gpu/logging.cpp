// fmt log sink adapted from bbport; share the runtime's thread-safe log.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/logging/log.h"
#include <cstdlib>
extern "C" {
#include "gz.h"
}
namespace Common::Log {
bool ShouldLog(Level level) {
    static int threshold = [] {
        const char *value = std::getenv("GZ_GPU_LOG");
        if (value && !std::strcmp(value, "trace"))
            return 0;
        if (value && !std::strcmp(value, "debug"))
            return 1;
        if (value && !std::strcmp(value, "warning"))
            return 3;
        if (value && !std::strcmp(value, "error"))
            return 4;
        return 2;
    }();
    return static_cast<int>(level) >= threshold;
}
void Write(std::string_view category, Level, const char *, int, const char *,
           std::string &&message) {
    gz_log("GPU [%.*s]: %s", static_cast<int>(category.size()), category.data(), message.c_str());
}
} // namespace Common::Log
void assert_fail_impl() {
    gz_stop(23, "STOP: GPU assertion failed (see log)");
}
[[noreturn]] void unreachable_impl() {
    gz_stop(23, "STOP: unreachable GPU code (see log)");
}
