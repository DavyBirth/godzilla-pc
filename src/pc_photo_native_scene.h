// Native integration starts with the runtime; scene checks gate activation.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include "pc_photo_camera.h"
namespace GzPhoto {
struct Preset;
}
namespace GzPhotoNative {
void Configure(void *image, uint64_t size);
bool Available();
bool Active();
bool InputFocused();
const char *UnavailableReason();
const char *TakeNotice();
void Toggle();
void Exit();
void Focus(bool focused);
// Input comes from the window thread; consumed on the native scene thread.
void Motion(float right, float up, float forward, float yaw, float pitch, float seconds,
            float speed = 100);
void Lens(float delta);
void Roll(float delta);
void Reset();
std::optional<GzPhoto::Pose> CameraPose(uint64_t ticket = 0);
// Queued for the scene owner; a stale ticket can never edit a newer session.
bool QueuePreset(uint64_t ticket, const GzPhoto::Preset &preset);
} // namespace GzPhotoNative
