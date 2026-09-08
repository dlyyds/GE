#pragma once

#include "Core/Timestep.h"

#include <entt.hpp>
#include <memory>

namespace GE {
class Scene;
struct AudioSourceComponent;

namespace Audio {
class AudioContext;

/// 每 Scene 一个实例：把 ECS 音频组件翻译成 AudioContext 的 Voice。
/// 纯数据组件 + 延迟命令队列，AudioWorld 内部维护运行时 Voice 映射。
class AudioWorld {
public:
    explicit AudioWorld(Scene *scene);
    ~AudioWorld();

    AudioWorld(const AudioWorld &) = delete;
    AudioWorld &operator=(const AudioWorld &) = delete;

    void OnComponentAdded(entt::entity entity);
    void OnComponentDestroyed(entt::entity entity);

    // —— 主线程命令（脚本 / 编辑器调用，Update 时统一转发到 AudioContext）——
    void RequestPlay(entt::entity entity, int slot, bool restart);
    void RequestStop(entt::entity entity, int slot);
    void StopAll(entt::entity entity);
    void SetVolume(entt::entity entity, int slot, float v);
    void SetPitch(entt::entity entity, int slot, float p);
    void SetLoop(entt::entity entity, int slot, bool on);

    /// Query whether an audio slot is currently playing (-1 = any/all slots).
    bool IsPlaying(entt::entity entity, int slot = -1) const;

    /// Play 态每帧调用（Scene::UpdateAudio）。
    void Update(Timestep ts);

    /// Scene::Play：触发 PlayOnAwake=true 的槽位。
    void OnPlay();

    /// Scene::Stop：停止本场景全部 Voice。
    void OnStop();

    /// Scene 析构 / 清空时调用。
    void Shutdown();

private:
    void SyncListener(float dtSeconds);
    void SyncSource(entt::entity entity, AudioSourceComponent &src);
    void ProcessCommandQueue();
    void PlaySlot(entt::entity entity, int slot, bool restart, AudioContext *ctx);
    void StopSlot(entt::entity entity, int slot, AudioContext *ctx);
    void StopAllVoices();

    Scene *m_Scene = nullptr;
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Audio
} // namespace GE


