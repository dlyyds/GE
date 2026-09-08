#include "Audio/SoundAsset.h"

#include <string>

// 仅使用 miniaudio 的解码 API（实现由 AudioContext.cpp 单独编译）。
#include "miniaudio.h"

namespace GE {
namespace Audio {

bool SoundAsset::ProbeMetadata(const std::string &filepath,
                               uint64_t &outSampleRate,
                               uint64_t &outFrameCount) {
    outSampleRate = 0;
    outFrameCount = 0;

    if (filepath.empty())
        return false;

    ma_decoder decoder;
    ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
    if (ma_decoder_init_file(filepath.c_str(), &config, &decoder) != MA_SUCCESS)
        return false;

    outSampleRate = decoder.outputSampleRate;
    ma_uint64 totalFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames) == MA_SUCCESS)
        outFrameCount = totalFrames;
    ma_decoder_uninit(&decoder);

    return outSampleRate > 0;
}

} // namespace Audio
} // namespace GE
