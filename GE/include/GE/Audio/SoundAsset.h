#pragma once

#include <cstdint>
#include <string>

namespace GE {
namespace Audio {

/// 声音资源：只保存文件路径与解码元数据，不持有 GPU/后端资源。
/// 实际发声由 AudioContext 在每次 Play 时按路径创建 Voice。
class SoundAsset {
public:
    SoundAsset() = default;
    ~SoundAsset() = default;

    [[nodiscard]] const std::string &GetFilePath() const { return m_FilePath; }
    [[nodiscard]] uint64_t GetSampleRate() const { return m_SampleRate; }
    [[nodiscard]] uint64_t GetFrameCount() const { return m_FrameCount; }

private:
    friend class SoundManager;

    /// 读取文件元数据（采样率 / PCM 帧数）。失败返回 false，字段保持 0。
    static bool ProbeMetadata(const std::string &filepath,
                              uint64_t &outSampleRate,
                              uint64_t &outFrameCount);

    std::string m_FilePath;
    uint64_t    m_SampleRate = 0;
    uint64_t    m_FrameCount = 0;
};

} // namespace Audio
} // namespace GE
