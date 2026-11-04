// Photo controls edit the shared channel; no guest calls on the UI thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_channel.h"
#include "pc_photo_native_scene.h"
#include "pc_photo_presets.h"
#include "imgui.h"
namespace GzPcOverlay {
void RequestCapture();
inline void PhotoNavigation(ImGuiIO &io, unsigned buttons, bool enabled) {
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    if (enabled)
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    const unsigned nav = enabled ? buttons : 0;
    for (auto [key, mask] : {std::pair{ImGuiKey_GamepadDpadUp, 0x10u},
                             {ImGuiKey_GamepadDpadRight, 0x20u},
                             {ImGuiKey_GamepadDpadDown, 0x40u},
                             {ImGuiKey_GamepadDpadLeft, 0x80u},
                             {ImGuiKey_GamepadFaceDown, 0x4000u},
                             {ImGuiKey_GamepadFaceRight, 0x2000u},
                             {ImGuiKey_GamepadL1, 0x400u},
                             {ImGuiKey_GamepadR1, 0x800u}})
        io.AddKeyEvent(key, (nav & mask) != 0);
}
inline const char *SavePhotoPreset(const GzPhoto::PhotoFrame &frame, unsigned slot) {
    const auto current = GzPhoto::photo_channel.Read();
    if (!current.active || current.ticket != frame.ticket)
        return "A sessão de foto terminou.";
    const auto pose = GzPhotoNative::CameraPose(frame.ticket);
    if (!pose)
        return "A câmera não está disponível para salvar.";
    return GzPhoto::SavePreset(slot, GzPhoto::CapturePreset(current.settings, pose->fov))
               ? "Ajustes salvos."
               : "Não foi possível salvar os ajustes.";
}
inline const char *LoadPhotoPreset(const GzPhoto::PhotoFrame &frame, unsigned slot) {
    const auto current = GzPhoto::photo_channel.Read();
    if (!current.active || current.ticket != frame.ticket)
        return "A sessão de foto terminou.";
    const auto preset = GzPhoto::LoadPreset(slot);
    if (!preset)
        return "Nada salvo nesta posição ainda.";
    if (!GzPhotoNative::QueuePreset(frame.ticket, *preset))
        return "Não foi possível carregar os ajustes nesta sessão.";
    bool limited = (!frame.capabilities.hud && preset->hide_hud) ||
                   (!frame.capabilities.dof && preset->dof.mode != GzPhoto::DofMode::Original);
    for (const auto &light : preset->lights)
        limited |= light.enabled &&
                   (!frame.capabilities.lights ||
                    (light.type == GzPhoto::LightType::Spot && !frame.capabilities.spot_lights));
    return limited
               ? "Carregamento solicitado. Recursos indisponíveis nesta cena ficarão desligados."
               : "Carregamento solicitado.";
}
inline void NativePhotoStyle() {
    auto &style = ImGui::GetStyle();
    style.WindowRounding = 6;
    style.FrameRounding = 3;
    style.PopupRounding = 3;
    style.WindowBorderSize = 1;
    style.FrameBorderSize = 1;
    auto &c = style.Colors;
    c[ImGuiCol_Text] = {.86f, .83f, .77f, 1};
    c[ImGuiCol_TextDisabled] = {.48f, .46f, .43f, 1};
    c[ImGuiCol_WindowBg] = {.13f, .14f, .16f, .97f};
    c[ImGuiCol_PopupBg] = c[ImGuiCol_WindowBg];
    c[ImGuiCol_Border] = {.49f, .37f, .22f, .9f};
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    c[ImGuiCol_TitleBg] = {.14f, .13f, .14f, 1};
    c[ImGuiCol_TitleBgActive] = {.21f, .16f, .13f, 1};
    c[ImGuiCol_FrameBg] = {.21f, .20f, .21f, 1};
    c[ImGuiCol_FrameBgHovered] = {.34f, .25f, .18f, 1};
    c[ImGuiCol_FrameBgActive] = {.43f, .22f, .18f, 1};
    c[ImGuiCol_Button] = {.25f, .22f, .20f, 1};
    c[ImGuiCol_ButtonHovered] = {.44f, .29f, .19f, 1};
    c[ImGuiCol_ButtonActive] = {.65f, .28f, .23f, 1};
    c[ImGuiCol_Header] = {.28f, .23f, .20f, 1};
    c[ImGuiCol_HeaderHovered] = {.43f, .27f, .18f, 1};
    c[ImGuiCol_HeaderActive] = {.65f, .28f, .23f, 1};
    c[ImGuiCol_CheckMark] = {.84f, .65f, .38f, 1};
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_CheckMark];
    c[ImGuiCol_SliderGrabActive] = {.93f, .74f, .46f, 1};
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_CheckMark];
    c[ImGuiCol_SeparatorActive] = c[ImGuiCol_SliderGrabActive];
}
} // namespace GzPcOverlay
