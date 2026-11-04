// Native runtime owns the Windows VEH; GPU tracking registers here.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/singleton.h"
#include "common/types.h"
namespace Core {
class SignalHandler {
  public:
    using Handler = bool (*)(void *, void *);
    void RegisterAccessViolationHandler(Handler callback, u32) {
        handler = callback;
    }
    Handler handler{};
};
using Signals = Common::Singleton<SignalHandler>;
} // namespace Core
