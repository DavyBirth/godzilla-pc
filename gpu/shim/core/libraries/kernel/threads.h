// Native host threads that can call guest allocator/file callbacks (AvPlayer).
// Fully commit their stacks, matching the runtime's guest-thread policy.
// Surface follows bbport's host thread adapter; Windows implementation is local.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include "core/libraries/kernel/time.h"
extern "C" {
void gz_thread_attach_host(const char *);
void gz_thread_detach_host();
[[noreturn]] void gz_stop(int, const char *, ...);
}
namespace Libraries::Kernel {
class Thread {
    struct State {
        std::function<void(std::stop_token)> function;
        std::stop_source stop;
        std::mutex mutex;
        HANDLE handle{};
        DWORD id{};
        ~State() {
            if (handle)
                CloseHandle(handle);
        }
    };
    std::shared_ptr<State> state;
    static DWORD WINAPI Start(void *arg) {
        std::unique_ptr<std::shared_ptr<State>> holder{static_cast<std::shared_ptr<State> *>(arg)};
        auto state = *holder;
        gz_thread_attach_host("AvPlayer");
        try {
            state->function(state->stop.get_token());
        } catch (const std::exception &e) {
            gz_stop(23, "STOP: AvPlayer worker: %s", e.what());
        }
        gz_thread_detach_host();
        return 0;
    }

  public:
    Thread() = default;
    ~Thread() {
        Stop();
    }
    void Run(std::function<void(std::stop_token)> &&fn) {
        Stop();
        state = std::make_shared<State>();
        state->function = std::move(fn);
        auto *arg = new std::shared_ptr<State>{state};
        state->handle = CreateThread(nullptr, 16u << 20, Start, arg, CREATE_SUSPENDED, &state->id);
        if (!state->handle) {
            delete arg;
            gz_stop(23, "STOP: cannot create AvPlayer worker");
        }
        ResumeThread(state->handle);
    }
    void Join() {
        if (!state)
            return;
        HANDLE wait{};
        {
            std::lock_guard lock{state->mutex};
            if (!state->handle)
                return;
            if (state->id == GetCurrentThreadId()) {
                CloseHandle(state->handle);
                state->handle = nullptr;
                return;
            }
            if (!DuplicateHandle(GetCurrentProcess(), state->handle, GetCurrentProcess(), &wait,
                                 SYNCHRONIZE, FALSE, 0))
                gz_stop(23, "STOP: cannot join AvPlayer worker");
        }
        WaitForSingleObject(wait, INFINITE);
        CloseHandle(wait);
        std::lock_guard lock{state->mutex};
        if (state->handle) {
            CloseHandle(state->handle);
            state->handle = nullptr;
        }
    }
    bool Joinable() const {
        if (!state)
            return false;
        std::lock_guard lock{state->mutex};
        return state->handle != nullptr;
    }
    void Stop() {
        if (state) {
            state->stop.request_stop();
            Join();
        }
    }
};
} // namespace Libraries::Kernel
