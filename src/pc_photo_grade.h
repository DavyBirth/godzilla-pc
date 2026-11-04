// Host photographic math adapted from Kyoei photo settings, 23 October 2026.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
namespace GzPhoto {
enum class Look {
    Original,
    Mono,
    Sepia,
    Cool,
    Warm,
    Kaiju1954,
    Tokusatsu,
    KaijuNight,
    AnimeKaiju,
    Count
};
struct Grade {
    float exposure = 0, contrast = 1, saturation = 1;
    int filter = 0;
    float grain = 0, vignette = 0;
    int frame = 0;
    float strength = 1;
};
inline bool Valid(Grade g) {
    return std::isfinite(g.exposure) && std::isfinite(g.contrast) && std::isfinite(g.saturation) &&
           std::isfinite(g.grain) && std::isfinite(g.vignette) && std::isfinite(g.strength) &&
           g.exposure >= -3 && g.exposure <= 3 && g.contrast >= 0 && g.contrast <= 2 &&
           g.saturation >= 0 && g.saturation <= 2 && g.grain >= 0 && g.grain <= 1 &&
           g.vignette >= 0 && g.vignette <= 1 && g.strength >= 0 && g.strength <= 1 &&
           g.filter >= 0 && g.filter < int(Look::Count) && g.frame >= 0 && g.frame <= 5;
}
inline bool Neutral(Grade g) {
    return g.exposure == 0 && g.contrast == 1 && g.saturation == 1 &&
           (g.filter == 0 || g.strength == 0) && g.grain == 0 && g.vignette == 0 && g.frame == 0;
}
inline float FrameRatio(int value) {
    constexpr float ratios[] = {0, 2.39f, 21.f / 9.f, 1, .8f, 4.f / 3.f};
    return ratios[std::clamp(value, 0, 5)];
}
// Normalized visible rectangle; image pixels outside become black bars.
struct FrameRect {
    float x = 0, y = 0, w = 1, h = 1;
};
inline FrameRect VisibleFrame(unsigned width, unsigned height, int value) {
    FrameRect r;
    const float ratio = FrameRatio(value);
    if (!ratio || !width || !height)
        return r;
    const float aspect = float(width) / height;
    if (aspect > ratio) {
        r.w = ratio / aspect;
        r.x = (1 - r.w) * .5f;
    } else {
        r.h = aspect / ratio;
        r.y = (1 - r.h) * .5f;
    }
    return r;
}
inline float FocalLength(float vertical_fov) {
    return 17.5f / std::tan(vertical_fov * .00872664625997f);
}
inline float FovFromFocal(float mm) {
    return 114.591559026f * std::atan(17.5f / mm);
}
inline float Decode(float x) {
    return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}
inline float Encode(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x <= .0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.f / 2.4f) - .055f;
}
inline void GradeLinear(float &r, float &g, float &b, Grade grade) {
    const float gain = std::exp2(grade.exposure);
    const auto curve = [&](float x) {
        return grade.contrast == 0
                   ? .18f
                   : .18f * std::pow(std::max(x * gain, 0.f) / .18f, grade.contrast);
    };
    r = curve(r);
    g = curve(g);
    b = curve(b);
    const float l = r * .2126f + g * .7152f + b * .0722f;
    r = l + (r - l) * grade.saturation;
    g = l + (g - l) * grade.saturation;
    b = l + (b - l) * grade.saturation;
    const float base_r = r, base_g = g, base_b = b;
    if (grade.filter == 1)
        r = g = b = l;
    else if (grade.filter == 2) {
        const float a = r, c = g, d = b;
        r = .393f * a + .769f * c + .189f * d;
        g = .349f * a + .686f * c + .168f * d;
        b = .272f * a + .534f * c + .131f * d;
    } else if (grade.filter == 3) {
        r *= .90f;
        b *= 1.10f;
    } else if (grade.filter == 4) {
        r *= 1.10f;
        b *= .90f;
    } else if (grade.filter == int(Look::Kaiju1954))
        r = g = b = .006f + .18f * std::pow(std::max(l, 0.f) / .18f, 1.18f);
    else if (grade.filter == int(Look::Tokusatsu)) {
        r = .015f + (l + (r - l) * 1.12f) * 1.08f;
        g = .010f + (l + (g - l) * 1.12f) * 1.02f;
        b = .015f + (l + (b - l) * 1.12f) * .83f;
    } else if (grade.filter == int(Look::KaijuNight)) {
        const float shadow = 1 - std::clamp(l * 2.f, 0.f, 1.f);
        r *= 1.03f - .25f * shadow;
        g *= .95f + .15f * shadow;
        b *= 1.15f + .15f * shadow;
    } else if (grade.filter == int(Look::AnimeKaiju)) {
        const float shadow = 1 - std::clamp(l * 2.f, 0.f, 1.f);
        r = (l + (r - l) * 1.16f) * (1.06f - .10f * shadow);
        g = (l + (g - l) * 1.16f) * (1.02f + .02f * shadow);
        b = (l + (b - l) * 1.16f) * (.97f + .13f * shadow);
    }
    r = base_r + (r - base_r) * grade.strength;
    g = base_g + (g - base_g) * grade.strength;
    b = base_b + (b - base_b) * grade.strength;
}
inline uint32_t FilmHash(uint32_t x, uint32_t y) {
    uint32_t h = x * UINT32_C(0x9e3779b9) ^ y * UINT32_C(0x85ebca6b) ^ UINT32_C(0x1954);
    h ^= h >> 16;
    h *= UINT32_C(0x7feb352d);
    h ^= h >> 15;
    h *= UINT32_C(0x846ca68b);
    return h ^ (h >> 16);
}
inline void FilmTexture(float &r, float &g, float &b, unsigned x, unsigned y, unsigned width,
                        unsigned height, Grade grade) {
    if (grade.grain == 0 && grade.vignette == 0)
        return;
    float noise = (float(FilmHash(x, y) & 65535) / 65535.f - .5f) * grade.grain * .16f;
    if (grade.filter == int(Look::Kaiju1954) && grade.grain > 0) {
        if (FilmHash(x / 2, 37) % 503 == 0 && (FilmHash(x / 2, y / 12) & 3) != 0)
            noise += grade.grain * grade.strength * .12f;
        if (FilmHash(x / 3, y / 3) % 4093 == 0)
            noise -= grade.grain * grade.strength * .25f;
    }
    const float u = 2 * (x + .5f) / width - 1, v = 2 * (y + .5f) / height - 1;
    const float radius = std::min(1.f, (u * u + v * v) * .5f),
                shade = 1 - grade.vignette * .65f * radius * radius;
    r = std::clamp(r * shade + noise, 0.f, 1.f);
    g = std::clamp(g * shade + noise, 0.f, 1.f);
    b = std::clamp(b * shade + noise, 0.f, 1.f);
}
inline void GradePixels(uint8_t *pixels, size_t bytes, bool bgra, Grade grade, unsigned width = 0) {
    if (!pixels || !Valid(grade) || Neutral(grade) || bytes % 4)
        return;
    if (!width)
        width = unsigned(bytes / 4);
    if (!width || bytes % (size_t(width) * 4))
        return;
    const unsigned height = unsigned(bytes / (size_t(width) * 4));
    const auto rect = VisibleFrame(width, height, grade.frame);
    for (size_t i = 0; i + 3 < bytes; i += 4) {
        auto *p = pixels + i;
        const int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
        const float x = (float(i / 4 % width) + .5f) / width,
                    y = (float(i / 4 / width) + .5f) / height;
        if (x < rect.x || x > rect.x + rect.w || y < rect.y || y > rect.y + rect.h) {
            p[0] = p[1] = p[2] = 0;
            continue;
        }
        float r = Decode(p[ri] / 255.f), g = Decode(p[1] / 255.f), b = Decode(p[bi] / 255.f);
        GradeLinear(r, g, b, grade);
        r = Encode(r);
        g = Encode(g);
        b = Encode(b);
        FilmTexture(r, g, b, unsigned(i / 4 % width), unsigned(i / 4 / width), width, height,
                    grade);
        p[ri] = uint8_t(std::lround(r * 255));
        p[1] = uint8_t(std::lround(g * 255));
        p[bi] = uint8_t(std::lround(b * 255));
    }
}
} // namespace GzPhoto
