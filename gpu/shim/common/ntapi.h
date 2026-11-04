// shadPS4's internal enum predates the identically named Windows SDK enum.
// Keep its private definitions separate without changing the reference source.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#define _QUEUE_USER_APC_FLAGS GzInternalQueueUserApcFlags
#define QUEUE_USER_APC_FLAGS GzInternalQueueUserApcFlagsType
#include "../../../third_party/shadPS4/src/common/ntapi.h"
#undef QUEUE_USER_APC_FLAGS
#undef _QUEUE_USER_APC_FLAGS
