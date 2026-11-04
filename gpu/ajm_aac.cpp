// SPDX-License-Identifier: GPL-2.0-or-later
// Native FFmpeg AAC behind the existing AJM interface. No FDK AAC code is used.
#include "ajm_aac.h"
#include "core/libraries/ajm/ajm_result.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

namespace Libraries::Ajm {
namespace {
constexpr std::array<u32, 12> SampleRates = {96000, 88200, 64000, 48000, 44100, 32000,
                                             24000, 22050, 16000, 12000, 11025, 8000};

AVSampleFormat SampleFormat(AjmFormatEncoding format) {
    switch (format) {
    case AjmFormatEncoding::S16:
        return AV_SAMPLE_FMT_S16;
    case AjmFormatEncoding::S32:
        return AV_SAMPLE_FMT_S32;
    case AjmFormatEncoding::Float:
        return AV_SAMPLE_FMT_FLT;
    default:
        return AV_SAMPLE_FMT_NONE;
    }
}
} // namespace

AjmAacDecoder::AjmAacDecoder(AjmFormatEncoding format, AjmAacCodecFlags flags, u32 channels)
    : m_format(format), m_flags(flags), m_channels(channels) {
    Reset();
}

AjmAacDecoder::~AjmAacDecoder() {
    Close();
}

void AjmAacDecoder::Close() {
    swr_free(&m_converter);
    av_frame_free(&m_frame);
    avcodec_free_context(&m_decoder);
}

void AjmAacDecoder::Reset() {
    Close();
    m_pending = false;
    m_packet.clear();
    m_pcm.clear();
    m_frame_samples = True(m_flags & EnableSbrDecode) ? 2048 : 1024;
    m_skip_frames = True(m_flags & EnableNondelayOutput) ? 0 : 2;
    m_info = {.num_channels = m_channels ? m_channels : 2,
              .sampl_freq =
                  m_frequency_index < SampleRates.size() ? SampleRates[m_frequency_index] : 0,
              .sample_encoding = m_format};
    m_error = 0;
    if ((m_transport != ADTS && m_transport != RAW) || m_frequency_index >= SampleRates.size() ||
        m_channels > 8 || SampleFormat(m_format) == AV_SAMPLE_FMT_NONE) {
        m_error = AVERROR(EINVAL);
        return;
    }
    const AVCodec *codec = avcodec_find_decoder_by_name("aac");
    if (!codec) {
        m_error = AVERROR_DECODER_NOT_FOUND;
        return;
    }
    m_decoder = avcodec_alloc_context3(codec);
    m_frame = av_frame_alloc();
    if (!m_decoder || !m_frame) {
        m_error = AVERROR(ENOMEM);
        return;
    }
    if (m_transport == RAW) {
        // ISO AudioSpecificConfig: AAC-LC, sampling-frequency index, channel configuration.
        const u32 channels = m_channels == 0   ? 2
                             : m_channels == 7 ? 11
                             : m_channels == 8 ? 12
                                               : m_channels;
        const u16 config = (2 << 11) | (m_frequency_index << 7) | (channels << 3);
        m_decoder->extradata = static_cast<u8 *>(av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_decoder->extradata) {
            m_error = AVERROR(ENOMEM);
            return;
        }
        m_decoder->extradata_size = 2;
        m_decoder->extradata[0] = config >> 8;
        m_decoder->extradata[1] = config & 255;
    }
    m_error = avcodec_open2(m_decoder, codec, nullptr);
}

void AjmAacDecoder::Initialize(const void *buffer, u32 size) {
    std::array<u32, 2> config{};
    if (!buffer || size != sizeof(config)) {
        Close();
        m_pending = false;
        m_error = AVERROR(EINVAL);
        return;
    }
    std::memcpy(config.data(), buffer, sizeof(config));
    m_transport = static_cast<ConfigType>(config[0]);
    m_frequency_index = config[1];
    Reset();
}

void AjmAacDecoder::GetInfo(void *out) const {
    if (out) {
        *static_cast<AjmSidebandDecM4aacCodecInfo *>(out) = {.heaac =
                                                                 True(m_flags & EnableSbrDecode)};
    }
}

AjmSidebandFormat AjmAacDecoder::GetFormat() const {
    return m_info;
}

u32 AjmAacDecoder::GetMinimumInputSize() const {
    return m_transport == ADTS ? 7 : 1;
}

u32 AjmAacDecoder::OutputSamples(const AjmInstanceGapless &gapless) const {
    if (m_skip_frames)
        return 0;
    const u32 skip = std::min<u32>(m_frame_samples, gapless.current.skip_samples);
    const u32 remaining = m_frame_samples - skip;
    return gapless.init.total_samples ? std::min(remaining, gapless.current.total_samples)
                                      : remaining;
}

u32 AjmAacDecoder::GetNextFrameSize(const AjmInstanceGapless &gapless) const {
    return OutputSamples(gapless) * m_info.num_channels *
           av_get_bytes_per_sample(SampleFormat(m_format));
}

int AjmAacDecoder::Decode(std::span<const u8> input) {
    AVPacket *packet = av_packet_alloc();
    if (!packet)
        return AVERROR(ENOMEM);
    int status = av_new_packet(packet, static_cast<int>(input.size()));
    if (status >= 0) {
        // FFmpeg bit readers require zero padding beyond the end of compressed data.
        std::memcpy(packet->data, input.data(), input.size());
        status = avcodec_send_packet(m_decoder, packet);
    }
    av_packet_free(&packet);
    if (status < 0)
        return status;
    av_frame_unref(m_frame);
    status = avcodec_receive_frame(m_decoder, m_frame);
    if (status < 0)
        return status;
    if (m_frame->nb_samples <= 0 || m_frame->nb_samples > 2048 ||
        m_frame->ch_layout.nb_channels < 1 || m_frame->ch_layout.nb_channels > 8 ||
        m_frame->sample_rate <= 0)
        return AVERROR_INVALIDDATA;

    swr_free(&m_converter);
    AVChannelLayout layout{};
    status = av_channel_layout_copy(&layout, &m_frame->ch_layout);
    if (status < 0)
        return status;
    status = swr_alloc_set_opts2(
        &m_converter, &layout, SampleFormat(m_format), m_frame->sample_rate, &m_frame->ch_layout,
        static_cast<AVSampleFormat>(m_frame->format), m_frame->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&layout);
    if (status < 0)
        return status;
    status = swr_init(m_converter);
    if (status < 0)
        return status;
    const int channels = m_frame->ch_layout.nb_channels;
    m_pcm.resize(m_frame->nb_samples * channels * av_get_bytes_per_sample(SampleFormat(m_format)));
    u8 *output[] = {m_pcm.data()};
    status = swr_convert(m_converter, output, m_frame->nb_samples,
                         const_cast<const u8 **>(m_frame->extended_data), m_frame->nb_samples);
    if (status != m_frame->nb_samples)
        return status < 0 ? status : AVERROR_INVALIDDATA;
    m_frame_samples = m_frame->nb_samples;
    m_info = {.num_channels = static_cast<u32>(channels),
              .channel_mask =
                  channels == 1 || channels == 2 || channels == 4 || channels == 6 || channels == 8
                      ? GetChannelMask(channels)
                  : m_frame->ch_layout.order == AV_CHANNEL_ORDER_NATIVE
                      ? static_cast<u32>(m_frame->ch_layout.u.mask)
                      : 0,
              .sampl_freq = static_cast<u32>(m_frame->sample_rate),
              .sample_encoding = m_format,
              .bitrate = static_cast<u32>(std::clamp<int64_t>(m_decoder->bit_rate, 0, UINT32_MAX))};
    m_packet.assign(input.begin(), input.end());
    m_pending = true;
    return 0;
}

DecoderResult AjmAacDecoder::ProcessData(std::span<u8> &input, SparseOutputBuffer &output,
                                         AjmInstanceGapless &gapless) {
    DecoderResult result{};
    if (m_error < 0) {
        result.result = ORBIS_AJM_RESULT_CODEC_ERROR | ORBIS_AJM_RESULT_FATAL;
        result.internal_result = m_error;
        return result;
    }
    if (!m_pending) {
        size_t length = input.size();
        if (length < GetMinimumInputSize()) {
            result.result = ORBIS_AJM_RESULT_PARTIAL_INPUT;
            return result;
        }
        if (m_transport == ADTS) {
            // Validate exactly one complete ADTS frame; leave partial input for the caller.
            if (input[0] != 0xff || (input[1] & 0xf6) != 0xf0 || (input[2] & 0x3c) == 0x3c) {
                result.result = ORBIS_AJM_RESULT_INVALID_DATA;
                return result;
            }
            length = ((input[3] & 3) << 11) | (input[4] << 3) | (input[5] >> 5);
            const size_t header = (input[1] & 1) ? 7 : 9;
            if (length < header || (input[6] & 3) != 0) {
                result.result = ORBIS_AJM_RESULT_INVALID_DATA;
                return result;
            }
            if (input.size() < length) {
                result.result = ORBIS_AJM_RESULT_PARTIAL_INPUT;
                return result;
            }
        }
        if (length > std::numeric_limits<int>::max()) {
            result.result = ORBIS_AJM_RESULT_INVALID_PARAMETER;
            return result;
        }
        const int status = Decode(input.first(length));
        if (status < 0) {
            // A malformed complete packet is an error, not silent successful consumption.
            result.result = ORBIS_AJM_RESULT_CODEC_ERROR;
            result.internal_result = status;
            return result;
        }
    }
    if (input.size() < m_packet.size() ||
        !std::equal(m_packet.begin(), m_packet.end(), input.begin())) {
        result.result = ORBIS_AJM_RESULT_INVALID_PARAMETER;
        return result;
    }
    const u32 samples = OutputSamples(gapless);
    const u32 sample_bytes = m_info.num_channels * av_get_bytes_per_sample(SampleFormat(m_format));
    const size_t bytes = samples * sample_bytes;
    if (output.Size() < bytes) {
        // Retain the decoded frame and leave input untouched so a larger retry is lossless.
        result.result = ORBIS_AJM_RESULT_NOT_ENOUGH_ROOM;
        return result;
    }
    input = input.subspan(m_packet.size());
    m_pending = false;
    result.frames_decoded = 1;
    if (m_skip_frames) {
        --m_skip_frames;
        return result;
    }
    const u32 skip = std::min<u32>(m_frame_samples, gapless.current.skip_samples);
    const auto pcm = std::span<const u8>(m_pcm).subspan(skip * sample_bytes, bytes);
    output.Write(pcm);
    gapless.current.skip_samples -= skip;
    result.samples_written = samples;
    gapless.current.skipped_samples += m_frame_samples - samples;
    if (gapless.init.total_samples)
        gapless.current.total_samples -= samples;
    return result;
}
} // namespace Libraries::Ajm
