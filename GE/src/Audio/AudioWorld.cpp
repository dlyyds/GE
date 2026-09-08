#include "Audio/AudioWorld.h"

#include "Audio/AudioContext.h"
#include "Audio/SoundAsset.h"
#include "Core/Application.h"
#include "Render/Renderer.h"
#include "Render/AssetManager.h"
#include "Scene/AudioComponents.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Core/Log.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <vector>
#include <unordered_map>

namespace GE {
namespace Audio {

namespace {

glm::vec3 GetEntityWorldPosition(Scene *scene, entt::entity e) {
    const TransformComponent *tc = scene->Reg().try_get<TransformComponent>(e);
    if (!tc)
        return glm::vec3(0.0f);
    return glm::vec3(tc->GetWorldMatrix()[3]);
}

} // namespace

struct AudioWorld::Impl {
    enum class CommandType : uint8_t {
        Play,
        Stop,
        StopAll,
        SetVolume,
        SetPitch,
        SetLoop
    };

    struct Command {
        CommandType type = CommandType::Play;
        entt::entity entity = entt::null;
        int slot = -1;
        bool restart = false;
        float value = 0.0f;
        bool flag = false;
    };

    std::vector<Command> commands;
    std::unordered_map<entt::entity, std::unordered_map<int, VoiceHandle>> voices;

    // Listener 上一帧位置（速度估算用）
    glm::vec3 prevListenerPos{0.0f};
    bool hasPrevListener = false;
};

AudioWorld::AudioWorld(Scene *scene)
    : m_Scene(scene), m_Impl(std::make_unique<Impl>()) {
}

AudioWorld::~AudioWorld() {
    Shutdown();
}

void AudioWorld::OnComponentAdded(entt::entity /*entity*/) {
    // 资源延迟到 Play/RequestPlay 时加载；此处不需要动作。
}

void AudioWorld::OnComponentDestroyed(entt::entity entity) {
    StopAll(entity);

    if (m_Impl) {
        auto it = m_Impl->voices.find(entity);
        if (it != m_Impl->voices.end())
            m_Impl->voices.erase(it);
    }
}

void AudioWorld::RequestPlay(entt::entity entity, int slot, bool restart) {
    if (m_Impl)
        m_Impl->commands.push_back({Impl::CommandType::Play, entity, slot, restart, 0.0f, false});
}

void AudioWorld::RequestStop(entt::entity entity, int slot) {
    if (m_Impl)
        m_Impl->commands.push_back({Impl::CommandType::Stop, entity, slot, false, 0.0f, false});
}

void AudioWorld::StopAll(entt::entity entity) {
    if (!m_Impl)
        return;

    AudioContext *ctx = Application::Get().GetAudioContext();
    auto it = m_Impl->voices.find(entity);
    if (it == m_Impl->voices.end())
        return;

    for (auto &[slot, handle] : it->second) {
        (void)slot;
        if (ctx)
            ctx->Stop(handle);
    }
    m_Impl->voices.erase(it);
}

void AudioWorld::SetVolume(entt::entity entity, int slot, float v) {
    if (m_Impl)
        m_Impl->commands.push_back({Impl::CommandType::SetVolume, entity, slot, false, v, false});
}

void AudioWorld::SetPitch(entt::entity entity, int slot, float p) {
    if (m_Impl)
        m_Impl->commands.push_back({Impl::CommandType::SetPitch, entity, slot, false, p, false});
}


bool AudioWorld::IsPlaying(entt::entity entity, int slot) const {
    if (!m_Impl)
        return false;
    AudioContext *ctx = Application::Get().GetAudioContext();
    if (!ctx)
        return false;

    const auto outer = m_Impl->voices.find(entity);
    if (outer == m_Impl->voices.end())
        return false;

    for (const auto &[s, handle] : outer->second) {
        if (slot != -1 && s != slot)
            continue;
        if (ctx->IsPlaying(handle))
            return true;
    }
    return false;
}

void AudioWorld::SetLoop(entt::entity entity, int slot, bool on) {
    if (m_Impl)
        m_Impl->commands.push_back({Impl::CommandType::SetLoop, entity, slot, false, 0.0f, on});
}

void AudioWorld::Update(Timestep ts) {
    if (!m_Scene)
        return;

    ProcessCommandQueue();
    SyncListener(ts.GetSeconds());

    auto view = m_Scene->Reg().view<AudioSourceComponent>();
    for (auto entity : view)
        SyncSource(entity, view.get<AudioSourceComponent>(entity));
}

void AudioWorld::OnPlay() {
    if (!m_Scene)
        return;

    auto view = m_Scene->Reg().view<AudioSourceComponent>();
    for (auto entity : view) {
        const auto &src = view.get<AudioSourceComponent>(entity);
        if (!src.Enabled)
            continue;
        for (int i = 0; i < static_cast<int>(src.Sounds.size()); ++i) {
            if (src.Sounds[i].PlayOnAwake && !src.Sounds[i].SoundPath.empty())
                RequestPlay(entity, i, false);
        }
    }
}

void AudioWorld::OnStop() {
    if (!m_Impl)
        return;

    // 先清空积压命令，避免 Stop 后 Update 再把 Play 命令发到已停的场景。
    m_Impl->commands.clear();

    // 复制 key 列表后逐个停（StopAll 会 erase，迭代原始 map 有并发风险）。
    std::vector<entt::entity> entities;
    entities.reserve(m_Impl->voices.size());
    for (const auto &[e, slots] : m_Impl->voices)
        entities.push_back(e);

    for (entt::entity e : entities)
        StopAll(e);
}

void AudioWorld::Shutdown() {
    if (!m_Impl)
        return;

    StopAllVoices();
    m_Impl->commands.clear();
    m_Impl->hasPrevListener = false;
}

void AudioWorld::SyncListener(float dtSeconds) {
    if (!m_Scene)
        return;

    AudioContext *ctx = Application::Get().GetAudioContext();
    if (!ctx)
        return;

    entt::entity listener = entt::null;

    auto listenerView = m_Scene->Reg().view<AudioListenerComponent>();
    for (auto entity : listenerView) {
        const auto &lc = listenerView.get<AudioListenerComponent>(entity);
        if (lc.Enabled) {
            listener = entity;
            break;
        }
    }

    if (listener == entt::null) {
        Entity cam = m_Scene->GetPrimaryCameraEntity();
        if (cam)
            listener = static_cast<entt::entity>(cam);
    }

    glm::vec3 pos(0.0f);
    glm::vec3 forward(0.0f, 0.0f, -1.0f);
    glm::vec3 up(0.0f, 1.0f, 0.0f);

    if (listener != entt::null) {
        const TransformComponent *tc = m_Scene->Reg().try_get<TransformComponent>(listener);
        if (tc) {
            const glm::mat4 &world = tc->GetWorldMatrix();
            pos = glm::vec3(world[3]);
            forward = glm::normalize(glm::vec3(world[2]));
            up = glm::normalize(glm::vec3(world[1]));
        }
    }

    ctx->SetListener(0, pos, forward, up);

    // 简单速度估算（帧差 / dt）。
    glm::vec3 velocity(0.0f);
    if (m_Impl->hasPrevListener && dtSeconds > 1e-5f)
        velocity = (pos - m_Impl->prevListenerPos) / dtSeconds;
    ctx->SetListenerVelocity(0, velocity);

    m_Impl->prevListenerPos = pos;
    m_Impl->hasPrevListener = true;
}

void AudioWorld::SyncSource(entt::entity entity, AudioSourceComponent &src) {
    if (!m_Impl)
        return;

    AudioContext *ctx = Application::Get().GetAudioContext();
    if (!ctx)
        return;

    auto outer = m_Impl->voices.find(entity);
    if (outer == m_Impl->voices.end())
        return;

    if (!src.Enabled) {
        StopAll(entity);
        return;
    }

    const glm::vec3 worldPos = GetEntityWorldPosition(m_Scene, entity);

    auto &slotMap = outer->second;
    for (auto it = slotMap.begin(); it != slotMap.end();) {
        const int slotIndex = it->first;
        const VoiceHandle handle = it->second;

        if (slotIndex < 0 || slotIndex >= static_cast<int>(src.Sounds.size())
            || src.Sounds[slotIndex].SoundPath.empty()) {
            // 槽位已不存在或路径被清空 → 停掉。
            ctx->Stop(handle);
            it = slotMap.erase(it);
            continue;
        }

        const AudioSound &s = src.Sounds[slotIndex];

        // 一次性声音播完自动回收句柄。
        if (!ctx->IsPlaying(handle)) {
            ctx->Stop(handle);
            it = slotMap.erase(it);
            continue;
        }

        ctx->SetVolume(handle, s.Volume);
        ctx->SetPitch(handle, s.Pitch);
        ctx->SetLoop(handle, s.Loop);
        if (src.Spatial)
            ctx->SetPosition(handle, worldPos);

        ++it;
    }

    if (slotMap.empty())
        m_Impl->voices.erase(outer);
}

void AudioWorld::ProcessCommandQueue() {
    if (!m_Impl)
        return;

    if (m_Impl->commands.empty())
        return;

    AudioContext *ctx = Application::Get().GetAudioContext();
    if (!ctx) {
        m_Impl->commands.clear();
        return;
    }

    auto &reg = m_Scene->Reg();

    for (const auto &cmd : m_Impl->commands) {
        if (!reg.valid(cmd.entity))
            continue;

        switch (cmd.type) {
        case Impl::CommandType::Play: {
            AudioSourceComponent *src = reg.try_get<AudioSourceComponent>(cmd.entity);
            if (!src)
                break;

            if (cmd.slot == -1) {
                for (int i = 0; i < static_cast<int>(src->Sounds.size()); ++i) {
                    if (!src->Sounds[i].SoundPath.empty())
                        PlaySlot(cmd.entity, i, cmd.restart, ctx);
                }
            } else {
                PlaySlot(cmd.entity, cmd.slot, cmd.restart, ctx);
            }
            break;
        }
        case Impl::CommandType::Stop:
            StopSlot(cmd.entity, cmd.slot, ctx);
            break;
        case Impl::CommandType::StopAll:
            StopSlot(cmd.entity, -1, ctx);
            break;
        case Impl::CommandType::SetVolume:
        case Impl::CommandType::SetPitch:
        case Impl::CommandType::SetLoop: {
            auto outer = m_Impl->voices.find(cmd.entity);
            if (outer != m_Impl->voices.end()) {
                for (auto &[slotIndex, handle] : outer->second) {
                    if (cmd.slot != -1 && slotIndex != cmd.slot)
                        continue;
                    if (cmd.type == Impl::CommandType::SetVolume)
                        ctx->SetVolume(handle, cmd.value);
                    else if (cmd.type == Impl::CommandType::SetPitch)
                        ctx->SetPitch(handle, cmd.value);
                    else
                        ctx->SetLoop(handle, cmd.flag);
                }
            }
            break;
        }
        }
    }

    m_Impl->commands.clear();
}

void AudioWorld::StopAllVoices() {
    if (!m_Impl)
        return;

    AudioContext *ctx = Application::Get().GetAudioContext();
    for (auto &[entity, slotMap] : m_Impl->voices) {
        (void)entity;
        for (auto &[slot, handle] : slotMap) {
            (void)slot;
            if (ctx)
                ctx->Stop(handle);
        }
    }
    m_Impl->voices.clear();
    m_Impl->commands.clear();
}

// 私有辅助：播放指定槽位。
void AudioWorld::PlaySlot(entt::entity entity, int slot, bool restart, AudioContext *ctx) {
    if (!m_Scene || !ctx || slot < 0)
        return;

    AudioSourceComponent *src = m_Scene->Reg().try_get<AudioSourceComponent>(entity);
    if (!src || !src->Enabled)
        return;
    if (slot >= static_cast<int>(src->Sounds.size()))
        return;

    const AudioSound &s = src->Sounds[slot];
    if (s.SoundPath.empty())
        return;

    // 编辑态不发声：编辑器试听走 AudioContext::Play 直接通道，不进本队列。
    if (!m_Scene->IsPlaying())
        return;

    const auto outer = m_Impl->voices.find(entity);
    if (outer != m_Impl->voices.end()) {
        auto inner = outer->second.find(slot);
        if (inner != outer->second.end()) {
            if (ctx->IsPlaying(inner->second) && !restart)
                return;
            ctx->Stop(inner->second);
            outer->second.erase(inner);
        }
    }

    SoundAsset *asset = Renderer::GetAssetManager().LoadSound(s.SoundPath);
    if (!asset) {
        GE_CORE_WARN("AudioWorld: failed to load sound \"{0}\"", s.SoundPath);
        return;
    }

    VoiceHandle handle;
    if (src->Spatial) {
        const glm::vec3 pos = GetEntityWorldPosition(m_Scene, entity);
        handle = ctx->Play(asset, s.Loop, s.Volume, s.Pitch, true, pos,
                           s.MinDistance, s.MaxDistance, s.Rolloff, s.Attenuation);
    } else {
        handle = ctx->Play(asset, s.Loop, s.Volume, s.Pitch, false, glm::vec3(0.0f),
                           s.MinDistance, s.MaxDistance, s.Rolloff, s.Attenuation);
    }

    if (handle != kInvalidVoice)
        m_Impl->voices[entity][slot] = handle;
}

// 私有辅助：停止指定槽位（-1 = 全部槽位）。
void AudioWorld::StopSlot(entt::entity entity, int slot, AudioContext *ctx) {
    if (!m_Impl || !ctx)
        return;

    auto outer = m_Impl->voices.find(entity);
    if (outer == m_Impl->voices.end())
        return;

    auto &slotMap = outer->second;
    auto it = slotMap.begin();
    while (it != slotMap.end()) {
        if (slot == -1 || it->first == slot) {
            ctx->Stop(it->second);
            it = slotMap.erase(it);
        } else {
            ++it;
        }
    }

    if (slotMap.empty())
        m_Impl->voices.erase(outer);
}


} // namespace Audio
} // namespace GE


