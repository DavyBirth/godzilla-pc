// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production AAC decoder without the game, Vulkan or real saves.
#include "ajm_aac.h"
#include "aac_test_data.h"
#include "core/libraries/ajm/ajm_result.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace Common::Log {
bool ShouldLog(Level) {
    return false;
}
void Write(std::string_view, Level, const char *, int, const char *, std::string &&) {}
} // namespace Common::Log
namespace Libraries::Ajm {
u32 GetChannelMask(u32 channels) {
    switch (channels) {
    case 1:
        return 4;
    case 2:
        return 3;
    case 4:
        return 0x33;
    case 6:
        return 0x60f;
    case 8:
        return 0x63f;
    default:
        throw std::runtime_error("unexpected channel count");
    }
}
} // namespace Libraries::Ajm
using namespace Libraries::Ajm;

void Check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}

std::vector<u8> Frame(std::span<const u8> data, size_t index = 0, bool raw = false) {
    while (!data.empty()) {
        const size_t length = ((data[3] & 3) << 11) | (data[4] << 3) | (data[5] >> 5);
        Check(length >= 7 && length <= data.size(), "invalid generated ADTS fixture");
        if (index-- == 0) {
            auto frame = data.first(length).subspan(raw ? 7 : 0);
            return {frame.begin(), frame.end()};
        }
        data = data.subspan(length);
    }
    throw std::runtime_error("fixture frame missing");
}

struct Output {
    std::vector<u8> data;
    std::array<std::span<u8>, 3> chunks;
    SparseOutputBuffer buffer;
    explicit Output(size_t bytes = 65536)
        : data(bytes, 0x7b), chunks{std::span(data).first(std::min<size_t>(3, bytes)),
                                    std::span(data).subspan(std::min<size_t>(3, bytes)),
                                    {}},
          buffer(chunks) {}
};

void DecodeFormats() {
    for (auto transport : {ADTS, RAW}) {
        for (auto format :
             {AjmFormatEncoding::S16, AjmFormatEncoding::S32, AjmFormatEncoding::Float}) {
            AjmAacDecoder decoder(format, EnableNondelayOutput, 2);
            const std::array<u32, 2> config{transport, 4};
            decoder.Initialize(config.data(), sizeof(config));
            AjmInstanceGapless gapless{};
            size_t total = 0;
            for (size_t n = 0; n < 4; ++n) {
                auto encoded = Frame(AacTestData::Tone, n, transport == RAW);
                std::span<u8> input(encoded);
                Output out;
                const auto result = decoder.ProcessData(input, out.buffer, gapless);
                Check(result.result == 0 && result.frames_decoded == 1 &&
                          result.samples_written == 1024 && input.empty(),
                      "frame decode/count failed");
                const auto info = decoder.GetFormat();
                Check(info.num_channels == 2 && info.sampl_freq == 44100 &&
                          info.sample_encoding == format,
                      "decoded format mismatch");
                const size_t bytes = 1024 * 2 * (format == AjmFormatEncoding::S16 ? 2 : 4);
                Check(out.data[bytes] == 0x7b, "wrote beyond decoded output");
                if (format == AjmFormatEncoding::Float) {
                    for (size_t i = 0; i < 2048; ++i) {
                        float sample;
                        std::memcpy(&sample, out.data.data() + i * 4, 4);
                        Check(std::isfinite(sample) && std::abs(sample) < 1, "invalid float PCM");
                        if (std::abs(sample) > .01f)
                            ++total;
                    }
                }
            }
            if (format == AjmFormatEncoding::Float)
                Check(total > 1000, "tone decoded as silence");
        }
    }
}

void RetryAndReset() {
    AjmAacDecoder decoder(AjmFormatEncoding::Float, EnableNondelayOutput, 2);
    auto encoded = Frame(AacTestData::Tone);
    std::span<u8> input(encoded);
    AjmInstanceGapless gapless{};
    Output small(100);
    auto result = decoder.ProcessData(input, small.buffer, gapless);
    Check(result.result == ORBIS_AJM_RESULT_NOT_ENOUGH_ROOM && input.size() == encoded.size() &&
              small.buffer.Size() == 100,
          "small output consumed input or wrote partial PCM");
    Output complete;
    result = decoder.ProcessData(input, complete.buffer, gapless);
    Check(result.result == 0 && input.empty() && result.samples_written == 1024,
          "retry lost decoded frame");
    decoder.Reset();
    input = encoded;
    Output repeated;
    result = decoder.ProcessData(input, repeated.buffer, gapless);
    Check(result.result == 0 && repeated.data == complete.data,
          "reset did not reproduce first frame");
    decoder.Reset();
    for (size_t size = 0; size < encoded.size(); ++size) {
        input = std::span(encoded).first(size);
        Output out;
        result = decoder.ProcessData(input, out.buffer, gapless);
        Check(result.result == ORBIS_AJM_RESULT_PARTIAL_INPUT && input.size() == size,
              "truncated ADTS was consumed");
    }
    input = encoded;
    Output after_partial;
    Check(decoder.ProcessData(input, after_partial.buffer, gapless).result == 0 && input.empty(),
          "partial input corrupted subsequent complete decode");
}

void GaplessAndDelay() {
    AjmAacDecoder decoder(AjmFormatEncoding::S16, EnableNondelayOutput, 1);
    AjmInstanceGapless gapless{};
    gapless.init = {.total_samples = 700, .skip_samples = 100};
    gapless.Reset();
    auto encoded = Frame(AacTestData::Mono);
    std::span<u8> input(encoded);
    Output out;
    auto result = decoder.ProcessData(input, out.buffer, gapless);
    Check(result.result == 0 && result.samples_written == 700 &&
              gapless.current.total_samples == 0 && gapless.current.skip_samples == 0 &&
              gapless.current.skipped_samples == 324,
          "gapless bounds/count mismatch");
    Check(decoder.GetFormat().num_channels == 1 && decoder.GetFormat().sampl_freq == 48000,
          "mono/rate change not reported");
    Check(std::all_of(out.data.begin(), out.data.begin() + 1400, [](u8 b) { return b == 0; }),
          "silence decoded as nonzero PCM");

    AjmAacDecoder delayed(AjmFormatEncoding::S16, AjmAacCodecFlags(0), 2);
    gapless = {};
    for (size_t n = 0; n < 3; ++n) {
        encoded = Frame(AacTestData::Tone, n);
        input = encoded;
        Output output;
        result = delayed.ProcessData(input, output.buffer, gapless);
        Check(result.result == 0 && result.frames_decoded == 1 && input.empty() &&
                  result.samples_written == (n < 2 ? 0 : 1024),
              "two-frame delay changed");
    }
}

void BadInput() {
    AjmAacDecoder decoder(AjmFormatEncoding::S16, EnableNondelayOutput, 2);
    AjmInstanceGapless gapless{};
    auto encoded = Frame(AacTestData::Tone);
    encoded[0] = 0;
    std::span<u8> input(encoded);
    Output out;
    Check(decoder.ProcessData(input, out.buffer, gapless).result == ORBIS_AJM_RESULT_INVALID_DATA &&
              input.size() == encoded.size(),
          "bad sync accepted");
    for (auto config : {std::array<u32, 2>{RAW, 12}, {99, 4}}) {
        decoder.Initialize(config.data(), sizeof(config));
        Check(decoder.ProcessData(input, out.buffer, gapless).result != 0,
              "invalid initialization accepted");
    }
    decoder.Initialize(nullptr, 8);
    Check(decoder.ProcessData(input, out.buffer, gapless).result != 0,
          "null initialization accepted");
}

int main() {
    try {
        const std::string config = avcodec_configuration();
        Check(config.find("--enable-nonfree") == std::string::npos &&
                  config.find("--enable-libfdk-aac") == std::string::npos,
              "FFmpeg contains a nonfree codec configuration");
        std::printf("FFmpeg: %s\n", avcodec_license());
        DecodeFormats();
        RetryAndReset();
        GaplessAndDelay();
        BadInput();
        std::puts("PASS: native FFmpeg AAC, ADTS/RAW, S16/S32/float, stereo/mono, sample rates, "
                  "nonzero tone, silence, odd output splits, gapless, delay, truncated input, "
                  "small-output retry, deterministic reset and invalid initialization.");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
