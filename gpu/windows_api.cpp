// Native Windows API backing the reference's file cache helper.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/ntapi.h"
NtSetInformationFile_t NtSetInformationFile = reinterpret_cast<NtSetInformationFile_t>(
    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
