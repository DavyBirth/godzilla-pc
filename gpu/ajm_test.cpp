// SPDX-License-Identifier: GPL-2.0-or-later
// Decode generated MP3 silence through the actual asynchronous PS4 job API.
#include <array>
#include <algorithm>
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_error.h"
#include "gpu.h"
extern "C" {
#include "gz.h"
}
void gz_ajm_self_test() {
    using namespace Libraries::Ajm;
    auto check = [](bool ok, const char *detail) {
        if (!ok)
            gz_stop(24, "AJM TEST: FAIL: %s", detail);
    };
    check(gz_gpu_resolve("libSceAjm", "sceAjmInitialize") != 0, "API registered");
    u32 context{};
    check(sceAjmInitialize(0, &context) == 0 && context != 0, "initialize context");
    AjmInstanceFlags instance_flags{};
    instance_flags.version = 1;
    instance_flags.channels = 2;
    instance_flags.format = 0;
    u32 instance{};
    for (auto codec : {AjmCodecType::At9Dec, AjmCodecType::M4aacDec, AjmCodecType::Mp3Dec}) {
        check(sceAjmModuleRegister(context, codec, 0) == 0, "register codec");
        check(sceAjmInstanceCreate(context, codec, instance_flags, &instance) == 0,
              "create decoder");
        if (codec != AjmCodecType::Mp3Dec)
            check(sceAjmInstanceDestroy(context, instance) == 0, "destroy unused decoder");
    }
    // MPEG-1 Layer III, 128 kbps, 44100 Hz, stereo. Zero side information and
    // zero main data encode silence; these bytes are generated test data.
    std::array<u8, 417 * 3 + 64> compressed{};
    for (int i = 0; i < 3; ++i) {
        compressed[i * 417] = 0xff;
        compressed[i * 417 + 1] = 0xfb;
        compressed[i * 417 + 2] = 0x90;
        compressed[i * 417 + 3] = 0;
    }
    std::array<s16, 1152 * 2 * 3> pcm;
    pcm.fill(1234);
    struct Output {
        AjmSidebandResult result;
        AjmSidebandStream stream;
        AjmSidebandFormat format;
    } output{};
    AjmJobFlags flags{};
    flags.version = 1;
    flags.run_flags = AjmJobRunFlags::MultipleFrames;
    flags.sideband_flags = AjmJobSidebandFlags::Stream | AjmJobSidebandFlags::Format;
    alignas(8) std::array<u8, 1024> batch{};
    auto *end = static_cast<u8 *>(
        sceAjmBatchJobRunBufferRa(batch.data(), instance, flags.raw, compressed.data(), 417 * 3,
                                  pcm.data(), sizeof(pcm), &output, sizeof(output), nullptr));
    check(end > batch.data() && end <= batch.data() + batch.size(), "build decode batch");
    u32 batch_id{};
    AjmBatchError error{};
    check(sceAjmBatchStartBuffer(context, batch.data(), u32(end - batch.data()), 0, &error,
                                 &batch_id) == 0,
          "submit decode batch");
    check(sceAjmBatchWait(context, batch_id, 2000, &error) == 0, "wait for decoder worker");
    check(output.stream.input_consumed > 0 && output.stream.output_written >= 1152 * 2 * 2 &&
              output.stream.output_written <= sizeof(pcm),
          "compressed input produces PCM samples");
    check(output.format.num_channels == 2 && output.format.sampl_freq == 44100, "decoded format");
    check(std::all_of(pcm.begin(), pcm.begin() + output.stream.output_written / 2,
                      [](s16 sample) { return sample == 0; }),
          "decoded silence content");
    check(sceAjmInstanceDestroy(context, instance) == 0, "destroy MP3 decoder");
    gz_log("AJM TEST: codec lifecycles and asynchronous MP3 decode passed (%d PCM bytes, %u Hz)",
           output.stream.output_written, output.format.sampl_freq);
}
