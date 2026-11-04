// Read-only presentation fallback for legacy empty camera images. GPL-2.0-or-later
#pragma once
#include <windows.h>
const unsigned char *gz_snapshot_view(HANDLE file, const char *path, const char *user_root);
