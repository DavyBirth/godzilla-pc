// Host-only variants selected during upload; archives and guest bytes stay intact.
// Thin1D address bits adapted from shadPS4 tiling.comp and Kyoei's live prompts.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "pc_features.h"
#include "pc_controller.h"
#include "pc_playback.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <algorithm>
extern "C" int gz_memory_copy(uintptr_t, void *, uint64_t, int);
extern "C" const char *gz_user_dir(void);
extern "C" void gz_log(const char *, ...);
namespace GzPcPrompts {
struct Pair {
    uint32_t width{}, height{}, bytes{};
    std::vector<unsigned char> variant[5];
    uint32_t block_bytes{};
};
struct State {
    int pair = -1;
    unsigned mode = ~0u;
};
inline std::vector<Pair> pairs;
inline std::unordered_map<uint64_t, State> states;
inline void Initialize() {
    static bool initialized{};
    if (initialized)
        return;
    initialized = true;
    const char *configured = std::getenv("GZ_PROMPT_PACK");
    const auto path = configured
                          ? std::filesystem::u8path(configured)
                          : std::filesystem::u8path(gz_user_dir()) / "patches" / "pc-prompts.bin";
    FILE *file = _wfopen(path.c_str(), L"rb");
    if (!file)
        return;
    char magic[8];
    uint32_t count{};
    size_t total{};
    bool ok = std::fread(magic, 1, 8, file) == 8;
    const bool version3 = ok && !std::memcmp(magic, "GZPRMPT3", 8);
    ok = ok && (version3 || !std::memcmp(magic, "GZPRMPT2", 8)) &&
         std::fread(&count, 4, 1, file) == 1 && count <= 256;
    for (unsigned i = 0; ok && i < count; ++i) {
        uint32_t fields[3];
        if (std::fread(fields, 4, 3, file) != 3) {
            ok = false;
            break;
        }
        Pair p{fields[0], fields[1], fields[2], {}};
        if (version3 && std::fread(&p.block_bytes, 4, 1, file) != 1) {
            ok = false;
            break;
        }
        total += size_t(p.bytes) * 5;
        const uint64_t expected =
            p.block_bytes ? uint64_t((p.width + 3) / 4) * ((p.height + 3) / 4) * p.block_bytes
                          : uint64_t(p.width) * p.height * 4;
        if (!p.width || !p.height || p.width > 4096 || p.height > 4096 ||
            (p.block_bytes && p.block_bytes != 8 && p.block_bytes != 16) || p.bytes != expected ||
            total > 256 * 1024 * 1024) {
            ok = false;
            break;
        }
        for (auto &pixels : p.variant) {
            pixels.resize(p.bytes);
            if (std::fread(pixels.data(), 1, p.bytes, file) != p.bytes) {
                ok = false;
                break;
            }
        }
        if (ok)
            pairs.push_back(std::move(p));
    }
    ok = ok && std::fgetc(file) == EOF;
    std::fclose(file);
    if (!ok)
        pairs.clear();
    gz_log("PC prompts: %zu verified texture variants loaded", pairs.size());
}
template <class Info, class Row>
bool Rows(const Pair &p, const Info &info, Row row, bool tiled = false) {
    if (info.resources.levels != 1 || info.size.width != p.width || info.size.height != p.height)
        return false;
    const auto [size, pitch, height, offset] = info.mips_layout[0];
    const unsigned unit = p.block_bytes ? 4 : 1, bytes_per_unit = p.block_bytes ? p.block_bytes : 4;
    const unsigned width = (p.width + unit - 1) / unit, rows = (p.height + unit - 1) / unit;
    const unsigned stride = std::max((unsigned(pitch) + unit - 1) / unit, width);
    for (unsigned y = 0; y < rows; ++y) {
        if (tiled && info.props.is_tiled) {
            if (unsigned(info.array_mode) != 2 ||
                (unsigned(info.tile_mode) != 9 && unsigned(info.tile_mode) != 13) || stride % 8)
                return false;
            if (p.block_bytes && unsigned(info.tile_mode) != 13)
                return false;
            for (unsigned x = 0; x < width; ++x) {
                const unsigned pixel =
                    unsigned(info.tile_mode) == 9
                        ? (x & 3) | ((y & 1) << 2) | ((x & 4) << 1) | ((y & 6) << 3)
                        : (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                              ((x & 4) << 2) | ((y & 4) << 3);
                const size_t at = size_t(offset) +
                                  (((y / 8) * (stride / 8) + x / 8) * 64 + pixel) * bytes_per_unit;
                if (at > info.guest_size || bytes_per_unit > info.guest_size - at ||
                    !row(at, (size_t(y) * width + x) * bytes_per_unit, bytes_per_unit))
                    return false;
            }
        } else {
            const size_t at = size_t(offset) + size_t(y) * stride * bytes_per_unit,
                         bytes = size_t(width) * bytes_per_unit;
            if (at > info.guest_size || bytes > info.guest_size - at ||
                !row(at, size_t(y) * width * bytes_per_unit, bytes))
                return false;
        }
    }
    return true;
}
template <class Image> const Pair *Select(Image &image, bool dirty, bool &changed, unsigned &mode) {
    Initialize();
    changed = false;
    const auto &info = image.info;
    if (pairs.empty() || info.resources.levels != 1 || info.resources.layers != 1 ||
        info.size.depth != 1 || info.num_samples != 1)
        return nullptr;
    auto format_matches = [&](const Pair &p) {
        return p.width == info.size.width && p.height == info.size.height &&
               bool(p.block_bytes) == bool(info.props.is_block) &&
               info.num_bits == (p.block_bytes ? p.block_bytes * 8 : 32);
    };
    bool candidate = false;
    for (const auto &p : pairs)
        if (format_matches(p)) {
            candidate = true;
            break;
        }
    if (!candidate)
        return nullptr;
    auto [it, fresh] = states.try_emplace(image.image_uid);
    auto &state = it->second;
    if (fresh || dirty) {
        state.pair = -1;
        if (info.guest_size > 8 * 1024 * 1024)
            return nullptr;
        std::vector<unsigned char> guest(info.guest_size);
        if (!gz_memory_copy(info.guest_address, guest.data(), guest.size(), 0))
            return nullptr;
        for (size_t i = 0; i < pairs.size(); ++i) {
            const auto &p = pairs[i];
            if (!format_matches(p))
                continue;
            if (Rows(
                    p, info,
                    [&](size_t at, size_t from, size_t bytes) {
                        return !std::memcmp(guest.data() + at, p.variant[0].data() + from, bytes);
                    },
                    true)) {
                state.pair = int(i);
                break;
            }
        }
        if (state.pair >= 0)
            gz_log("PC prompts: matched texture uid=%llu pair=%d",
                   (unsigned long long)image.image_uid, state.pair);
    }
    if (state.pair < 0)
        return nullptr;
    const int type = gz_pc_controller_template();
    mode = gz_pc_controller()       ? (type == 2   ? 3u
                                       : type == 3 ? 4u
                                                   : 0u)
           : gz_pc_action_prompts() ? 2u
                                    : 1u;
    // The touchpad sheet also contains the independent Options label.
    // Escape labels both native pause and movie skip. Keep the independent
    // Options label, with composite word labels removed by the artwork patch.
    if (!gz_pc_controller() && state.pair == 1 && pairs[1].width == 256 && pairs[1].height == 256)
        mode = GzPcPlayback::Active() ? 1u : 2u;
    changed = mode != state.mode;
    state.mode = mode;
    return &pairs[size_t(state.pair)];
}
} // namespace GzPcPrompts
