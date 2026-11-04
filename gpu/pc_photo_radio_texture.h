// GPU-only visibility of the radio portrait atlas. Guest memory is read-only.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_prompts.h"
#include "pc_photo_channel.h"
#include <span>
namespace GzPhotoRadioTexture {
struct Profile {
    unsigned width, height;
    uint64_t hash;
    const char *label = "fixture";
};
inline constexpr Profile Profiles[]{
    {512, 1024, 0x8cb675512d7fc0e7ull, "operator"}, {512, 1024, 0xd74af1add03d9a41ull, "prime-1"},
    {512, 1024, 0xb4a8cd2f1fd916b6ull, "prime-2"},  {512, 1024, 0x10b8f7870078f728ull, "prime-3"},
    {1024, 512, 0x5957a9d6aaefd285ull, "hud-bp"},   {1024, 512, 0x5a811f4050357abfull, "hud-en"},
    {1024, 512, 0xbca11f70f0530c89ull, "hud-fr"},   {1024, 512, 0x35a5aa9d36c6e1d1ull, "hud-ns"}};
struct State {
    GzPcPrompts::Pair pair;
    bool matched{};
    unsigned mode = ~0u;
};
inline std::unordered_map<uint64_t, State> states;
inline uint64_t Hash(const std::vector<unsigned char> &data) {
    uint64_t hash = 14695981039346656037ull;
    for (auto byte : data)
        hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}
template <class Image>
const GzPcPrompts::Pair *SelectProfiles(Image &image, bool dirty, bool hidden, bool &changed,
                                        unsigned &mode, std::span<const Profile> profiles) {
    changed = false;
    const auto &info = image.info;
    if (!info.resources.levels || info.resources.levels > info.mips_layout.size() ||
        info.resources.layers != 1 || info.size.depth != 1 || info.num_samples != 1 ||
        !info.props.is_block || info.num_bits != 128 || info.guest_size > 8 * 1024 * 1024)
        return nullptr;
    const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](auto p) {
        return p.width == info.size.width && p.height == info.size.height;
    });
    if (profile == profiles.end())
        return nullptr;
    for (unsigned level = 0; level < info.resources.levels; ++level) {
        const auto [size, pitch, height, offset] = info.mips_layout[level];
        if (offset > info.guest_size || size > info.guest_size - offset)
            return nullptr;
    }
    auto [it, fresh] = states.try_emplace(image.image_uid);
    auto &state = it->second;
    if (fresh || dirty) {
        state.matched = false;
        GzPcPrompts::Pair pair{
            profile->width, profile->height, profile->width * profile->height, {}, 16};
        std::vector<unsigned char> guest(info.guest_size);
        pair.variant[0].resize(pair.bytes);
        if (!gz_memory_copy(info.guest_address, guest.data(), guest.size(), 0))
            return nullptr;
        auto base_info = info;
        base_info.resources.levels = 1;
        const bool copied = GzPcPrompts::Rows(
            pair, base_info,
            [&](size_t at, size_t to, size_t bytes) {
                std::memcpy(pair.variant[0].data() + to, guest.data() + at, bytes);
                return true;
            },
            true);
        if (!copied)
            return nullptr;
        const auto hash = Hash(pair.variant[0]);
        const auto matched = std::find_if(profiles.begin(), profiles.end(), [&](auto p) {
            return p.width == pair.width && p.height == pair.height && p.hash == hash;
        });
        if (matched == profiles.end())
            return nullptr;
        // The upload adapter clears the complete staging range, including all
        // mip levels. On exit the normal guest upload path restores every mip.
        pair.variant[0].clear();
        state.pair = std::move(pair);
        state.matched = true;
        gz_log("Photo radio texture: matched %s atlas uid=%llu", matched->label,
               (unsigned long long)image.image_uid);
    }
    if (!state.matched)
        return nullptr;
    mode = hidden ? 1 : 0;
    changed = state.mode != mode;
    state.mode = mode;
    if (changed)
        gz_log("Photo radio texture: uid=%llu hidden=%u", (unsigned long long)image.image_uid,
               mode);
    return hidden ? &state.pair : nullptr;
}
template <class Image>
const GzPcPrompts::Pair *SelectForProfile(Image &image, bool dirty, bool hidden, bool &changed,
                                          unsigned &mode, Profile profile) {
    return SelectProfiles(image, dirty, hidden, changed, mode, std::span{&profile, 1});
}
template <class Image>
const GzPcPrompts::Pair *Select(Image &image, bool dirty, bool &changed, unsigned &mode) {
    static const bool enabled = [] {
        const char *option = std::getenv("GZ_PHOTO_HIDE_RADIO_TEXTURE");
        return !option || std::strcmp(option, "0") != 0;
    }();
    if (!enabled)
        return nullptr;
    const auto &info = image.info;
    if (!info.props.is_block || info.num_bits != 128 ||
        !((info.size.width == 512 && info.size.height == 1024) ||
          (info.size.width == 1024 && info.size.height == 512)))
        return nullptr;
    const auto frame = GzPhoto::photo_channel.Read();
    return SelectProfiles(image, dirty,
                          frame.active && frame.capabilities.hud && frame.settings.hide_hud,
                          changed, mode, Profiles);
}
} // namespace GzPhotoRadioTexture
