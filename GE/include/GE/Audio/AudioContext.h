#pragma once

#include "Audio/AudioTypes.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>

namespace GE {
namespace Audio {

class SoundAsset;

/// 音频后端上下文（miniaudio）：全局一份，由 Application 持有。
/// 封装 Device/Engine 初始化、Master Volume 与底层发声/停止/查询。
class AudioContext {
public:
    AudioContext();
    ~AudioContext();

    AudioContext(const AudioContext &) = delete;
    AudioContext &operator=(const AudioContext &) = delete;

    /// 初始化 miniaudio 引擎 / 默认输出设备。可重复调用（幂等）。
    bool Init();

    /// 释放后端资源（停掉全部 Voice）。
    void Shutdown();

    void SetMasterVolume(float v);
    [[nodiscard]] float GetMasterVolume() const;

    /// 设置 Listener 0 的世界位置 / 朝向 / 上向量。
    void SetListener(uint32_t index, const glm::vec3 &pos,
                     const glm::vec3 &forward, const glm::vec3 &up);

    void SetListenerVelocity(uint32_t index, const glm::vec3 &v);

    /// 播放一个声音资源并返回句柄（失败返回 kInvalidVoice）。
    VoiceHandle Play(SoundAsset *sound, bool loop, float volume, float pitch,
                     bool spatial, const glm::vec3 &pos, float minDist,
                     float maxDist, float rolloff,
                     AttenuationModel attenuation = AttenuationModel::Inverse);

    void Stop(VoiceHandle h);
    void StopAll();
    void SetVolume(VoiceHandle h, float v);
    void SetPitch(VoiceHandle h, float p);
    void SetLoop(VoiceHandle h, bool loop);
    void SetPosition(VoiceHandle h, const glm::vec3 &pos);
    [[nodiscard]] bool IsPlaying(VoiceHandle h) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl; ///< 持 ma_engine / 后端资源，头文件隔离 miniaudio
};

} // namespace Audio
} // namespace GE
