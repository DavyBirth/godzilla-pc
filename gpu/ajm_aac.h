// AAC interface adapted from shadPS4; decoder implementation uses native FFmpeg AAC.
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/libraries/ajm/ajm_instance.h"
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct SwrContext;

namespace Libraries::Ajm {

enum ConfigType : u32 { ADTS = 1, RAW = 2 };

enum AjmAacCodecFlags : u32 {
    EnableSbrDecode = 1 << 0,
    EnableNondelayOutput = 1 << 1,
    SurroundChannelInterleaveOrderExtlExtrLsRs = 1 << 2,
    SurroundChannelInterleaveOrderLsRsExtlExtr = 1 << 3,
};
DECLARE_ENUM_FLAG_OPERATORS(AjmAacCodecFlags)

struct AjmSidebandDecM4aacCodecInfo {
    u32 heaac;
    u32 reserved;
};

class AjmAacDecoder final : public AjmCodec {
  public:
    AjmAacDecoder(AjmFormatEncoding format, AjmAacCodecFlags flags, u32 channels);
    ~AjmAacDecoder() override;
    AjmAacDecoder(const AjmAacDecoder &) = delete;
    AjmAacDecoder &operator=(const AjmAacDecoder &) = delete;

    void Reset() override;
    void Initialize(const void *buffer, u32 buffer_size) override;
    void GetInfo(void *out_info) const override;
    AjmSidebandFormat GetFormat() const override;
    u32 GetMinimumInputSize() const override;
    u32 GetNextFrameSize(const AjmInstanceGapless &gapless) const override;
    DecoderResult ProcessData(std::span<u8> &input, SparseOutputBuffer &output,
                              AjmInstanceGapless &gapless) override;

  private:
    void Close();
    int Decode(std::span<const u8> packet);
    u32 OutputSamples(const AjmInstanceGapless &gapless) const;

    const AjmFormatEncoding m_format;
    const AjmAacCodecFlags m_flags;
    const u32 m_channels;
    ConfigType m_transport = ADTS;
    u32 m_frequency_index = 4;
    u32 m_skip_frames = 0;
    u32 m_frame_samples = 1024;
    AjmSidebandFormat m_info{};
    int m_error = 0;
    bool m_pending = false;
    AVCodecContext *m_decoder = nullptr;
    AVFrame *m_frame = nullptr;
    SwrContext *m_converter = nullptr;
    std::vector<u8> m_packet;
    std::vector<u8> m_pcm;
};

} // namespace Libraries::Ajm
