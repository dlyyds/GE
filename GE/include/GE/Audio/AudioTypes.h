#pragma once

#include <cstdint>

namespace GE {
namespace Audio {

/// 播放句柄（单调递增，0 保留给“无效”）。
using VoiceHandle = uint64_t;

inline constexpr VoiceHandle kInvalidVoice = 0;

/// 距离衰减模型（与 miniaudio ma_attenuation_model 一一对应）。
enum class AttenuationModel : uint8_t {
    Inverse    = 0, ///< 反比衰减（默认，等价 OpenAL INVERSE_DISTANCE_CLAMPED）
    Linear     = 1,
    Exponential = 2,
    None       = 255 ///< 不衰减 / 关闭 3D 空间化
};

class SoundAsset;

} // namespace Audio
} // namespace GE
