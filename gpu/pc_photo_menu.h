// Photo mode menu drawn in the style of the game's own menus (header, gold rule,
// italic red items, orange selection bar, device prompts). Edits the shared
// channel only; focus and lights share this scrolling menu.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_photo_panel.h"
#include "imgui.h"
#include "pc_prompt_shapes.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
extern "C" int gz_pc_controller(void);
extern "C" int gz_pc_controller_template(void);
namespace GzPcOverlay {
inline ImFont *menu_font{};
inline std::atomic<unsigned> menu_keys{}; // keyboard presses, same masks as the pad
inline std::atomic<unsigned> photo_pad_buttons_for_menu{};
enum MenuKey : unsigned {
    MenuUp = 0x10,
    MenuRight = 0x20,
    MenuDown = 0x40,
    MenuLeft = 0x80,
    MenuOk = 0x4000
};

namespace PhotoMenuDetail {
// Italic text: shear the glyph vertices like the game's oblique menu lettering.
inline float Text(ImDrawList *dl, ImVec2 at, float size, ImU32 color, const char *text,
                  float tracking, float shear = .22f, bool right = false) {
    float width = 0;
    for (const char *c = text; *c;) {
        const char *next = c + 1;
        while ((*next & 0xC0) == 0x80)
            ++next;
        width += menu_font->CalcTextSizeA(size, 1e9f, 0, c, next).x + tracking;
        c = next;
    }
    width -= tracking;
    if (!dl)
        return width;
    if (right)
        at.x -= width;
    for (int pass = 0; pass < 2; ++pass) {
        const ImU32 col = pass ? color : IM_COL32(0, 0, 0, 170);
        const ImVec2 o = pass ? ImVec2(0, 0) : ImVec2(2, 2);
        const int start = dl->VtxBuffer.Size;
        float x = at.x;
        for (const char *c = text; *c;) {
            const char *next = c + 1;
            while ((*next & 0xC0) == 0x80)
                ++next;
            for (int bold = 0; bold < 2; ++bold)
                dl->AddText(menu_font, size, {x + o.x + bold * .8f, at.y + o.y}, col, c, next);
            x += menu_font->CalcTextSizeA(size, 1e9f, 0, c, next).x + tracking;
            c = next;
        }
        const float base = at.y + o.y + size;
        for (int i = start; i < dl->VtxBuffer.Size; ++i)
            dl->VtxBuffer[i].pos.x += (base - dl->VtxBuffer[i].pos.y) * shear;
    }
    return width;
}
inline void Arrow(ImDrawList *dl, ImVec2 at, float s, bool right, ImU32 c) {
    if (right)
        dl->AddTriangleFilled(at, {at.x, at.y + s}, {at.x + s * .7f, at.y + s * .5f}, c);
    else
        dl->AddTriangleFilled({at.x + s * .7f, at.y}, {at.x + s * .7f, at.y + s},
                              {at.x, at.y + s * .5f}, c);
}
// Input symbols drawn with primitives (no font glyphs that may be missing).
struct PromptGlyph {
    enum Kind {
        KeyCap,
        Mouse,
        MouseWheel,
        ArrowKeys,
        StickIcon,
        TriggerIcon,
        DpadIcon,
        FaceIcon
    } kind{};
    const char *text{};
    float wide{1};
    bool flag{};
    int face{}, style{};
    static PromptGlyph Key(const char *t, float w = 1) {
        PromptGlyph g;
        g.kind = KeyCap;
        g.text = t;
        g.wide = w;
        return g;
    }
    static PromptGlyph MouseDrag() {
        PromptGlyph g;
        g.kind = Mouse;
        return g;
    }
    static PromptGlyph Wheel() {
        PromptGlyph g;
        g.kind = MouseWheel;
        return g;
    }
    static PromptGlyph Arrows(bool vertical) {
        PromptGlyph g;
        g.kind = ArrowKeys;
        g.flag = vertical;
        return g;
    }
    static PromptGlyph Stick(const char *t, bool press = false) {
        PromptGlyph g;
        g.kind = StickIcon;
        g.text = t;
        g.flag = press;
        return g;
    }
    static PromptGlyph Trigger(const char *t) {
        PromptGlyph g;
        g.kind = TriggerIcon;
        g.text = t;
        return g;
    }
    static PromptGlyph Dpad(bool vertical) {
        PromptGlyph g;
        g.kind = DpadIcon;
        g.flag = vertical;
        return g;
    }
    static PromptGlyph Face(int style, int face) {
        PromptGlyph g;
        g.kind = FaceIcon;
        g.style = style;
        g.face = face;
        return g;
    }
    float Width() const {
        return kind == KeyCap        ? 30 * wide
               : kind == ArrowKeys   ? (flag ? 22.f : 44.f)
               : kind == TriggerIcon ? 40.f
                                     : 30.f;
    }
    template <class Pt> float Draw(ImDrawList *dl, Pt P, float s, float x, float y) const {
        const ImU32 fill = IM_COL32(30, 28, 30, 255), edge = IM_COL32(200, 200, 210, 255),
                    ink = IM_COL32(225, 225, 230, 255), hot = IM_COL32(240, 110, 20, 255);
        const float w = Width();
        auto label = [&](const char *t, float cx, float cy, float size, ImU32 c) {
            const ImVec2 ts = menu_font->CalcTextSizeA(size * s, 1e9f, 0, t);
            const ImVec2 at = P(cx, cy);
            dl->AddText(menu_font, size * s, {at.x - ts.x * .5f, at.y - ts.y * .5f}, c, t);
        };
        auto tri = [&](float cx, float cy, float r, int dir,
                       ImU32 c) { // 0 up, 1 right, 2 down, 3 left
            const float dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
            const float px = -dy[dir], py = dx[dir];
            dl->AddTriangleFilled(
                P(cx + dx[dir] * r, cy + dy[dir] * r),
                P(cx - dx[dir] * r * .6f + px * r, cy - dy[dir] * r * .6f + py * r),
                P(cx - dx[dir] * r * .6f - px * r, cy - dy[dir] * r * .6f - py * r), c);
        };
        switch (kind) {
        case KeyCap:
            dl->AddRectFilled(P(x, y), P(x + w, y + 30), fill, 5 * s);
            dl->AddRect(P(x, y), P(x + w, y + 30), edge, 5 * s, 0, 2 * s);
            if (!std::strcmp(text, "Space")) {
                dl->AddLine(P(x + 8, y + 16), P(x + 8, y + 21), ink, 2 * s);
                dl->AddLine(P(x + 8, y + 21), P(x + w - 8, y + 21), ink, 2 * s);
                dl->AddLine(P(x + w - 8, y + 21), P(x + w - 8, y + 16), ink, 2 * s);
            } else if (!std::strcmp(text, "Enter")) {
                size_t count;
                const auto *lines = gz_prompt_strokes(GZ_PROMPT_RETURN, &count);
                for (size_t i = 0; i < count; ++i)
                    dl->AddLine(P(x + lines[i].x1 * w, y + lines[i].y1 * 30),
                                P(x + lines[i].x2 * w, y + lines[i].y2 * 30), ink, 2 * s);
            } else
                label(text, x + w * .5f, y + 15, std::strlen(text) > 1 ? 14.f : 17.f, ink);
            break;
        case Mouse:
            dl->AddRectFilled(P(x + 5, y), P(x + 25, y + 30), fill, 9 * s);
            dl->AddRect(P(x + 5, y), P(x + 25, y + 30), edge, 9 * s, 0, 2 * s);
            dl->AddRectFilled(P(x + 16, y + 2), P(x + 24, y + 12), hot, 4 * s);
            dl->AddLine(P(x + 15, y), P(x + 15, y + 12), edge, 1.5f * s);
            break;
        case MouseWheel:
            dl->AddRectFilled(P(x + 5, y), P(x + 25, y + 30), fill, 9 * s);
            dl->AddRect(P(x + 5, y), P(x + 25, y + 30), edge, 9 * s, 0, 2 * s);
            dl->AddRectFilled(P(x + 13, y + 4), P(x + 17, y + 13), hot, 2 * s);
            break;
        case ArrowKeys:
            if (flag) {
                dl->AddRectFilled(P(x, y), P(x + 22, y + 14), fill, 3 * s);
                dl->AddRectFilled(P(x, y + 16), P(x + 22, y + 30), fill, 3 * s);
                tri(x + 11, y + 7, 4, 0, ink);
                tri(x + 11, y + 23, 4, 2, ink);
            } else {
                dl->AddRectFilled(P(x, y + 8), P(x + 21, y + 30), fill, 3 * s);
                dl->AddRectFilled(P(x + 23, y + 8), P(x + 44, y + 30), fill, 3 * s);
                tri(x + 10, y + 19, 5, 3, ink);
                tri(x + 34, y + 19, 5, 1, ink);
            }
            break;
        case StickIcon:
            dl->AddCircleFilled(P(x + 15, y + 15), 15 * s, fill);
            dl->AddCircle(P(x + 15, y + 15), 15 * s, edge, 0, 2 * s);
            dl->AddCircle(P(x + 15, y + 15), 9 * s, IM_COL32(120, 120, 130, 255), 0, 1.5f * s);
            label(text, x + 15, y + 15, std::strlen(text) > 1 ? 12.f : 15.f, ink);
            if (flag)
                dl->AddCircle(P(x + 15, y + 15), 12.5f * s, hot, 0, 1.5f * s);
            break;
        case TriggerIcon:
            dl->AddRectFilled(P(x, y + 2), P(x + w, y + 30), fill, 9 * s,
                              ImDrawFlags_RoundCornersTop);
            dl->AddRect(P(x, y + 2), P(x + w, y + 30), edge, 9 * s, ImDrawFlags_RoundCornersTop,
                        2 * s);
            label(text, x + w * .5f, y + 16, 14.f, ink);
            break;
        case DpadIcon:
            dl->AddRectFilled(P(x + 10, y), P(x + 20, y + 30), fill);
            dl->AddRectFilled(P(x, y + 10), P(x + 30, y + 20), fill);
            dl->AddRect(P(x + 10, y), P(x + 20, y + 30), edge, 0, 0, 1.5f * s);
            dl->AddRect(P(x, y + 10), P(x + 30, y + 20), edge, 0, 0, 1.5f * s);
            if (flag) {
                tri(x + 15, y + 5, 3.5f, 0, hot);
                tri(x + 15, y + 25, 3.5f, 2, hot);
            } else {
                tri(x + 5, y + 15, 3.5f, 3, hot);
                tri(x + 25, y + 15, 3.5f, 1, hot);
            }
            break;
        case FaceIcon: {
            // face: 0 confirm, 1 back, 3 photo (PS square / Xbox X / Nintendo Y).
            dl->AddCircleFilled(P(x + 15, y + 15), 15 * s, fill);
            dl->AddCircle(P(x + 15, y + 15), 15 * s, edge, 0, 2 * s);
            if (style == 1) {
                if (face == 1)
                    dl->AddCircle(P(x + 15, y + 15), 7 * s, IM_COL32(230, 80, 80, 255), 0,
                                  2.5f * s);
                else if (face == 3)
                    dl->AddRect(P(x + 9, y + 9), P(x + 21, y + 21), IM_COL32(230, 120, 170, 255), 0,
                                0, 2.5f * s);
                else {
                    size_t count;
                    const auto *lines = gz_prompt_strokes(GZ_PROMPT_CROSS, &count);
                    for (size_t i = 0; i < count; ++i)
                        dl->AddLine(P(x + lines[i].x1 * 30, y + lines[i].y1 * 30),
                                    P(x + lines[i].x2 * 30, y + lines[i].y2 * 30),
                                    IM_COL32(110, 150, 230, 255), 2.5f * s);
                }
            } else {
                const bool nin = style == 3;
                const char *t = face == 1   ? (nin ? "A" : "B")
                                : face == 3 ? (nin ? "Y" : "X")
                                            : (nin ? "B" : "A");
                const ImU32 c = nin         ? ink
                                : face == 1 ? IM_COL32(225, 60, 55, 255)
                                : face == 3 ? IM_COL32(60, 130, 230, 255)
                                            : IM_COL32(100, 190, 70, 255);
                label(t, x + 15, y + 15, 17.f, c);
            }
            break;
        }
        }
        return w;
    }
};
struct Item {
    std::string name, value;
    int id;
};
inline const char *Filter(int i) {
    static const char *n[] = {"Original",  "Preto e branco", "Sépia",
                              "Frio",      "Quente",         "Godzilla 1954",
                              "Tokusatsu", "Noite kaiju",    "Anime kaiju"};
    return n[std::clamp(i, 0, 8)];
}
inline const char *Frame(int i) {
    static const char *n[] = {"Original", "Cinema 2.39:1", "Ultrawide 21:9",
                              "Quadrado", "Retrato 4:5",   "Clássico 4:3"};
    return n[std::clamp(i, 0, 5)];
}
inline const char *GuideName(int i) {
    static const char *n[] = {"Nenhuma", "Terços", "Proporção áurea", "Centro"};
    return n[std::clamp(i, 0, 3)];
}
inline std::string Number(float v) {
    char b[24];
    std::snprintf(b, sizeof b, v < 10 ? "%.2f" : v < 1000 ? "%.1f" : "%.0f", v);
    return b;
}
inline GzPhoto::Vec3 Palette(int i) {
    static const GzPhoto::Vec3 c[] = {{1, 1, 1},      {1, .78f, .55f}, {.6f, .75f, 1},
                                      {1, .25f, .2f}, {.3f, 1, .4f},   {.3f, .45f, 1}};
    return c[std::clamp(i, 0, 5)];
}
inline int ColorIndex(GzPhoto::Vec3 v) {
    for (int i = 0; i < 6; ++i) {
        auto c = Palette(i);
        if (std::fabs(c.x - v.x) < .01f && std::fabs(c.y - v.y) < .01f &&
            std::fabs(c.z - v.z) < .01f)
            return i;
    }
    return 0;
}
inline const char *ColorName(GzPhoto::Vec3 v) {
    static const char *n[] = {"Branca", "Quente", "Fria", "Vermelha", "Verde", "Azul"};
    return n[ColorIndex(v)];
}
inline std::string Pct(float v) {
    char b[16];
    std::snprintf(b, sizeof b, "%d%%", int(std::lround(v * 100)));
    return b;
}
inline std::string Signed(float v) {
    char b[16];
    std::snprintf(b, sizeof b, "%+.1f", v);
    return std::fabs(v) < .05f ? std::string("0") : std::string(b);
}
inline std::string Degrees(float value) {
    char b[24];
    std::snprintf(b, sizeof b, "%.1f°", std::fabs(value) < .05f ? 0.f : value);
    return b;
}
inline std::string Inclination(const GzPhoto::Pose &pose) {
    using namespace GzPhoto;
    auto forward = Add(pose.target, Scale(pose.eye, -1)), up = pose.up;
    if (!Unit(forward) || !Unit(up))
        return "--";
    auto right = Cross(forward, {0, 1, 0});
    if (!Unit(right))
        return "--";
    const auto level_up = Cross(right, forward);
    return Degrees(std::atan2(Dot(up, right), Dot(up, level_up)) * 57.295779513f);
}
} // namespace PhotoMenuDetail

inline void GamePhotoMenu(const GzPhoto::PhotoFrame &frame, unsigned width, unsigned height) {
    using namespace PhotoMenuDetail;
    if (!menu_font)
        menu_font = ImGui::GetFont();
    auto settings = frame.settings;
    bool changed = false;
    static int selected{}, slot{};
    static uint64_t ticket{};
    static unsigned previous_pad{};
    static const char *notice = "";
    static float notice_time{};
    if (ticket != frame.ticket) {
        ticket = frame.ticket;
        selected = 0;
        notice = "";
    }
    auto &g = settings.grade;
    const auto pose = GzPhotoNative::CameraPose(frame.ticket);
    // The game HUD is always hidden in photo mode (RS/Tab hides this menu itself).
    if (frame.capabilities.hud && !settings.hide_hud) {
        settings.hide_hud = true;
        changed = true;
    }
    enum {
        Style,
        Strength,
        Exposure,
        Contrast,
        Saturation,
        Grain,
        Vignette,
        Format,
        Guides,
        Lens,
        Roll,
        Speed,
        Hud,
        Dof,
        ResetCam,
        ResetImg,
        Save,
        Load,
        Header,
        FocusMode,
        FocusDistance,
        FocusRange,
        FocusBlur,
        LightOn,
        LightType,
        LightPower,
        LightReach,
        LightColor,
        LightCone,
        LightBring
    };
    // One list, split by section titles (Header rows are skipped by the cursor);
    // focus and lights sections exist only when the scene offers them.
    const char *title = "MODO FOTO";
    std::vector<Item> items;
    auto header = [&](const char *name) { items.push_back({name, "", Header}); };
    header("IMAGEM");
    items.insert(items.end(), {{"ESTILO", Filter(g.filter), Style},
                               {"FORÇA DO ESTILO", Pct(g.strength), Strength},
                               {"EXPOSIÇÃO", Signed(g.exposure), Exposure},
                               {"CONTRASTE", Pct(g.contrast), Contrast},
                               {"SATURAÇÃO", Pct(g.saturation), Saturation},
                               {"GRÃO", Pct(g.grain), Grain},
                               {"VINHETA", Pct(g.vignette), Vignette},
                               {"RESTAURAR IMAGEM", "", ResetImg}});
    header("ENQUADRAMENTO");
    items.insert(items.end(), {{"FORMATO", Frame(g.frame), Format},
                               {"GUIAS", GuideName(int(settings.guide)), Guides},
                               {"ABERTURA DA LENTE", pose ? Degrees(pose->fov) : "--", Lens},
                               {"INCLINAÇÃO", pose ? Inclination(*pose) : "--", Roll},
                               {"VELOCIDADE", Number(settings.speed), Speed},
                               {"RESTAURAR CÂMERA", "", ResetCam}});
    if (frame.capabilities.dof) {
        header("FOCO");
        auto &d = settings.dof;
        static const char *m[] = {"Original", "Manual", "Sem desfoque"};
        items.push_back({"DESFOQUE", m[std::clamp(int(d.mode), 0, 2)], FocusMode});
        if (d.mode == GzPhoto::DofMode::Manual) {
            items.push_back({"DISTÂNCIA", Number(d.distance), FocusDistance});
            items.push_back({"FAIXA NÍTIDA", Number(d.range), FocusRange});
            items.push_back({"TRANSIÇÃO", Number(d.blur_range), FocusBlur});
        }
    }
    if (frame.capabilities.lights) {
        header("LUZES");
        for (int i = 0; i < 3; ++i) {
            const auto &l = settings.lights[i];
            const int base = (i + 1) << 8;
            items.push_back({"LUZ " + std::to_string(i + 1), l.enabled ? "Ligada" : "Desligada",
                             base | LightOn});
            if (!l.enabled)
                continue;
            if (frame.capabilities.spot_lights)
                items.push_back({"   TIPO",
                                 l.type == GzPhoto::LightType::Spot ? "Refletor" : "Geral",
                                 base | LightType});
            items.push_back({"   POTÊNCIA", Number(l.intensity), base | LightPower});
            items.push_back({"   ALCANCE", Number(l.outer_range), base | LightReach});
            items.push_back({"   COR", ColorName(l.color), base | LightColor});
            if (l.type == GzPhoto::LightType::Spot)
                items.push_back({"   FACHO", std::to_string(int(std::lround(l.outer_cone))) + "°",
                                 base | LightCone});
            items.push_back({"   TRAZER PARA A CÂMERA", "", base | LightBring});
        }
    }
    header("AJUSTES SALVOS");
    items.push_back({"SALVAR AJUSTES", "Posição " + std::to_string(slot + 1), Save});
    items.push_back({"CARREGAR AJUSTES", "Posição " + std::to_string(slot + 1), Load});
    selected = std::clamp(selected, 0, int(items.size()) - 1);

    const float s = std::min(width / 1920.f, height / 1080.f);
    const ImVec2 o((width - 1920 * s) * .5f, (height - 1080 * s) * .5f);
    auto P = [&](float x, float y) { return ImVec2(o.x + x * s, o.y + y * s); };
    auto *dl = ImGui::GetForegroundDrawList();
    // Left shade, as the menu screens darken their text side.
    for (int i = 0; i < 24; ++i) {
        const float x0 = i * 900.f / 24, x1 = (i + 1) * 900.f / 24,
                    a = 225 * std::pow(std::max(0.f, 1 - x0 / 900), 1.1f);
        dl->AddRectFilled({o.x + x0 * s, 0}, {o.x + x1 * s, float(height)},
                          IM_COL32(8, 8, 10, int(a)));
    }
    const ImU32 gold = IM_COL32(214, 190, 138, 255), red = IM_COL32(205, 75, 50, 255),
                cream = IM_COL32(235, 225, 200, 255);
    dl->AddRectFilled(P(150, 96), P(166, 110), IM_COL32(90, 140, 200, 255));
    Text(dl, P(176, 84), 28 * s, gold, title, 9 * s);
    dl->AddLine(P(140, 122), P(1780, 122), IM_COL32(200, 150, 70, 255), 2 * s);

    // Input: pad edges, keyboard presses, mouse hover/click.
    const unsigned pad = photo_pad_buttons_for_menu.load();
    unsigned pressed = (pad & ~previous_pad) | menu_keys.exchange(0);
    previous_pad = pad;
    const auto &io = ImGui::GetIO();
    constexpr float Row = 34, Visible = 22;
    static int scroll{};
    const float row = Row * s, top = 150 * s + o.y;
    int hover = -1;
    if (io.MousePos.x >= o.x + 140 * s && io.MousePos.x <= o.x + 860 * s) {
        const int r = int(std::floor((io.MousePos.y - top + 3 * s) / row));
        if (r >= 0 && r + scroll < int(items.size()) && r < int(Visible))
            hover = r + scroll;
    }
    if (hover >= 0 && items[hover].id == Header)
        hover = -1;
    if (hover >= 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0))
        selected = hover;
    // Right button is reserved for dragging the camera; left click on the value side steps it.
    if (hover >= 0 && ImGui::IsMouseClicked(0)) {
        selected = hover;
        pressed |= io.MousePos.x < o.x + 640 * s   ? MenuOk
                   : io.MousePos.x < o.x + 740 * s ? MenuLeft
                                                   : MenuRight;
    }
    const int n = int(items.size());
    auto step_to = [&](int dir) {
        do
            selected = (selected + dir + n) % n;
        while (items[selected].id == Header);
    };
    if (items[selected].id == Header)
        step_to(1);
    if (pressed & MenuUp)
        step_to(-1);
    if (pressed & MenuDown)
        step_to(1);
    // Keep the selection (and its section title) inside the visible window.
    if (selected - 1 < scroll)
        scroll = std::max(0, selected - 1);
    if (selected >= scroll + int(Visible))
        scroll = selected - int(Visible) + 1;
    scroll = std::clamp(scroll, 0, std::max(0, n - int(Visible)));
    const int dir = (pressed & MenuRight) ? 1 : (pressed & MenuLeft) ? -1 : 0;
    const bool ok = (pressed & MenuOk) != 0;
    if (dir || ok) {
        const int d = dir ? dir : 1;
        auto step = [&](float &v, float lo, float hi, float st) {
            v = std::clamp(v + st * d, lo, hi);
            changed = true;
        };
        const int light = (items[selected].id >> 8) - 1;
        auto &L = settings.lights[std::clamp(light, 0, 2)];
        auto scale = [&](float &v, float lo, float hi) {
            v = std::clamp(d > 0 ? v * 1.25f : v / 1.25f, lo, hi);
            changed = true;
        };
        switch (items[selected].id & 0xff) {
        case Style:
            g.filter = (g.filter + d + 9) % 9;
            changed = true;
            break;
        case Strength:
            step(g.strength, 0, 1, .05f);
            break;
        case Exposure:
            step(g.exposure, -3, 3, .1f);
            break;
        case Contrast:
            step(g.contrast, 0, 2, .05f);
            break;
        case Saturation:
            step(g.saturation, 0, 2, .05f);
            break;
        case Grain:
            step(g.grain, 0, 1, .05f);
            break;
        case Vignette:
            step(g.vignette, 0, 1, .05f);
            break;
        case Format:
            g.frame = (g.frame + d + 6) % 6;
            changed = true;
            break;
        case Guides:
            settings.guide = GzPhoto::Guide((int(settings.guide) + d + 4) % 4);
            changed = true;
            break;
        case Lens:
            GzPhotoNative::Lens(float(d));
            break;
        case Roll:
            GzPhotoNative::Roll(.05f * d);
            break;
        case Speed:
            settings.speed =
                std::clamp(d > 0 ? settings.speed * 1.25f : settings.speed / 1.25f, 1.f, 1e6f);
            changed = true;
            break;
        case Hud:
            settings.hide_hud = !settings.hide_hud;
            changed = true;
            break;
        case Header:
            break;
        case Dof:
            settings.dof.mode = GzPhoto::DofMode((int(settings.dof.mode) + d + 3) % 3);
            changed = true;
            break;
        case ResetCam:
            if (ok)
                GzPhotoNative::Reset();
            break;
        case ResetImg:
            if (ok) {
                g = {};
                changed = true;
            }
            break;
        case Save:
            if (dir)
                slot = (slot + dir + 8) % 8;
            else {
                notice = SavePhotoPreset(frame, unsigned(slot + 1));
                notice_time = float(ImGui::GetTime());
            }
            break;
        case Load:
            if (dir)
                slot = (slot + dir + 8) % 8;
            else {
                notice = LoadPhotoPreset(frame, unsigned(slot + 1));
                notice_time = float(ImGui::GetTime());
                changed = false;
                const auto latest = GzPhoto::photo_channel.Read();
                if (latest.ticket == frame.ticket)
                    settings = latest.settings;
            }
            break;
        case FocusMode:
            settings.dof.mode = GzPhoto::DofMode((int(settings.dof.mode) + d + 3) % 3);
            changed = true;
            break;
        case FocusDistance:
            scale(settings.dof.distance, .01f, 1e7f);
            break;
        case FocusRange:
            settings.dof.range =
                settings.dof.range < .01f && d > 0
                    ? 1.f
                    : std::clamp(d > 0 ? settings.dof.range * 1.25f : settings.dof.range / 1.25f,
                                 0.f, 1e7f);
            changed = true;
            break;
        case FocusBlur:
            scale(settings.dof.blur_range, .01f, 1e7f);
            break;
        case LightOn:
            L.enabled = !L.enabled;
            changed = true;
            break;
        case LightType:
            L.type = L.type == GzPhoto::LightType::Spot ? GzPhoto::LightType::Point
                                                        : GzPhoto::LightType::Spot;
            changed = true;
            break;
        case LightPower:
            L.intensity =
                L.intensity < .01f && d > 0
                    ? .1f
                    : std::clamp(d > 0 ? L.intensity * 1.25f : L.intensity / 1.25f, 0.f, 10000.f);
            changed = true;
            break;
        case LightReach:
            scale(L.outer_range, L.inner_range + .01f, 1e7f);
            break;
        case LightColor: {
            const int c = (ColorIndex(L.color) + d + 6) % 6;
            L.color = Palette(c);
            changed = true;
            break;
        }
        case LightCone:
            L.outer_cone = std::clamp(L.outer_cone + 5.f * d, 1.f, 175.f);
            L.inner_cone = std::min(L.inner_cone, L.outer_cone);
            changed = true;
            break;
        case LightBring:
            if (ok) {
                if (auto pose = GzPhotoNative::CameraPose(frame.ticket)) {
                    auto direction = GzPhoto::Add(pose->target, GzPhoto::Scale(pose->eye, -1));
                    if (GzPhoto::Unit(direction)) {
                        L.position = pose->eye;
                        L.direction = direction;
                        changed = true;
                    }
                }
            }
            break;
        }
    }

    float y = 150;
    for (int i = scroll; i < std::min(int(items.size()), scroll + int(Visible)); ++i) {
        const bool sel = i == selected;
        if (sel)
            dl->AddRectFilled(P(140, y - 2), P(860, y + 28), IM_COL32(240, 110, 20, 235));
        if (items[i].id == Header) {
            Text(dl, P(150, y + 8), 17 * s, gold, items[i].name.c_str(), 6 * s, .22f);
            dl->AddLine(P(150, y + 29), P(860, y + 29), IM_COL32(200, 150, 70, 90), 1 * s);
            y += Row;
            continue;
        }
        Text(dl, P(160, y), 24 * s, sel ? cream : red, items[i].name.c_str(), 3 * s);
        if (!items[i].value.empty()) {
            const float w = Text(dl, P(sel ? 800 : 820, y), 24 * s, sel ? cream : gold,
                                 items[i].value.c_str(), 1 * s, .12f, true);
            if (sel) {
                Arrow(dl, P(800 - w / s - 30, y + 6), 16 * s, false, cream);
                Arrow(dl, P(814, y + 6), 16 * s, true, cream);
            }
        } else if (sel && (items[i].id == Lens || items[i].id == Roll)) {
            Arrow(dl, P(760, y + 6), 16 * s, false, cream);
            Arrow(dl, P(808, y + 6), 16 * s, true, cream);
        }
        y += Row;
    }
    // Keep the continuation visible even before the player starts navigating.
    // The track also shows how much of this variable-length menu remains.
    if (n > int(Visible)) {
        const float track_top = 150, track_height = Visible * Row - 6;
        dl->AddRectFilled(P(876, track_top), P(880, track_top + track_height),
                          IM_COL32(214, 190, 138, 65));
        const float thumb_height = track_height * Visible / n;
        const float thumb_top =
            track_top + (track_height - thumb_height) * scroll / (n - int(Visible));
        dl->AddRectFilled(P(876, thumb_top), P(880, thumb_top + thumb_height), gold);
        const bool above = scroll > 0, below = scroll + int(Visible) < n;
        if (above)
            dl->AddTriangleFilled(P(500, 134), P(492, 142), P(508, 142), gold);
        if (below) {
            dl->AddTriangleFilled(P(150, 913), P(166, 913), P(158, 921), gold);
            Text(dl, P(180, 900), 20 * s, gold, "MAIS OPÇÕES ABAIXO", 2 * s, .12f);
        }
    }
    if (*notice && ImGui::GetTime() - notice_time < 4)
        Text(dl, P(160, y + 12), 20 * s, gold, notice, 1 * s, .12f);

    // Prompts for the device in use, drawn as symbols (keycaps, sticks, mouse),
    // two rows at the bottom right like the game's "CONFIRMAR" line.
    const bool pad_in_use = gz_pc_controller() != 0;
    const int kind = gz_pc_controller_template();
    using G = PromptGlyph;
    struct Prompt {
        std::vector<G> glyphs;
        const char *label;
    };
    std::vector<Prompt> camera, menu;
    if (!pad_in_use) {
        camera = {{{G::Key("W"), G::Key("A"), G::Key("S"), G::Key("D")}, "MOVER"},
                  {{G::MouseDrag()}, "GIRAR"},
                  {{G::Key("Space", 1.9f), G::Key("Ctrl", 1.3f)}, "SUBIR / DESCER"},
                  {{G::Wheel()}, "ZOOM"},
                  {{G::Key("Q"), G::Key("E")}, "INCLINAR"}};
        menu = {{{G::Arrows(true)}, "ESCOLHER"},
                {{G::Arrows(false)}, "AJUSTAR"},
                {{G::Key("Tab", 1.3f)}, "ESCONDER MENU"},
                {{G::Key("F12", 1.3f)}, "FOTOGRAFAR"},
                {{G::Key("Esc", 1.3f)}, "SAIR"}};
    } else {
        const bool ps = kind == 1, nin = kind == 3;
        camera = {{{G::Stick(ps ? "L" : "LS")}, "MOVER"},
                  {{G::Stick(ps ? "R" : "RS")}, "GIRAR"},
                  {{G::Trigger(ps    ? "L2"
                               : nin ? "ZL"
                                     : "LT"),
                    G::Trigger(ps    ? "R2"
                               : nin ? "ZR"
                                     : "RT")},
                   "DESCER / SUBIR"},
                  {{G::Trigger(ps    ? "L1"
                               : nin ? "L"
                                     : "LB"),
                    G::Trigger(ps    ? "R1"
                               : nin ? "R"
                                     : "RB")},
                   "INCLINAR"}};
        menu = {{{G::Dpad(true)}, "ESCOLHER"},
                {{G::Dpad(false)}, "AJUSTAR"},
                {{G::Stick(ps ? "R3" : "RS", true)}, "ESCONDER MENU"},
                {{G::Face(kind, 3)}, "FOTOGRAFAR"},
                {{G::Face(kind, 1)}, "SAIR"}};
    }
    auto prompt_row = [&](const std::vector<Prompt> &prompts, float yb) {
        float total = 0;
        for (const auto &p : prompts) {
            for (const auto &g : p.glyphs)
                total += g.Width() + 4;
            total += 4 + Text(nullptr, {0, 0}, 20 * s, 0, p.label, 2 * s, .15f) / s + 44;
        }
        float x = 1780 - total + 44;
        for (const auto &p : prompts) {
            for (const auto &g : p.glyphs)
                x += g.Draw(dl, P, s, x, yb) + 4;
            x += 4 + Text(dl, P(x, yb + 3), 20 * s, cream, p.label, 2 * s, .15f) / s + 44;
        }
    };
    prompt_row(camera, 952);
    prompt_row(menu, 1000);
    if (changed && GzPhoto::Valid(settings))
        GzPhoto::photo_channel.Edit(frame.ticket, settings);
}
} // namespace GzPcOverlay
