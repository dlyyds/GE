#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "Audio/AudioContext.h"
#include "Audio/SoundAsset.h"

#include "Core/Log.h"

#include <glm/glm.hpp>
#include <cmath>
#include <string>
#include <unordered_map>
#include <memory>
#include <utility>

namespace GE {
namespace Audio {

namespace {

ma_attenuation_model ToMaAttenuation(AttenuationModel model) {
    switch (model) {
    case AttenuationModel::Linear:      return ma_attenuation_model_linear;
    case AttenuationModel::Exponential: return ma_attenuation_model_exponential;
    case AttenuationModel::None:        return ma_attenuation_model_none;
    default:                            return ma_attenuation_model_inverse;
    }
}

} // namespace

struct AudioContext::Impl {
    ma_engine engine{};
    bool initialized = false;
    uint64_t nextHandle = 1;
    float masterVolume = 1.0f;
    std::unordered_map<VoiceHandle, std::unique_ptr<ma_sound>> voices;
};

AudioContext::AudioContext()
    : m_Impl(std::make_unique<Impl>()) {
}

AudioContext::~AudioContext() {
    Shutdown();
}

bool AudioContext::Init() {
    if (m_Impl->initialized)
        return true;

    ma_engine_config config = ma_engine_config_init();
    if (ma_engine_init(&config, &m_Impl->engine) != MA_SUCCESS) {
        GE_CORE_ERROR("AudioContext: failed to initialize miniaudio engine");
        return false;
    }

    m_Impl->initialized = true;
    SetMasterVolume(m_Impl->masterVolume);

    GE_CORE_INFO("AudioContext initialized (sample rate: {0} Hz)",
                 ma_engine_get_sample_rate(&m_Impl->engine));
    return true;
}

void AudioContext::Shutdown() {
    if (!m_Impl || !m_Impl->initialized)
        return;

    StopAll();
    ma_engine_uninit(&m_Impl->engine);
    m_Impl->initialized = false;
    GE_CORE_INFO("AudioContext shutdown");
}

void AudioContext::SetMasterVolume(float v) {
    if (!m_Impl)
        return;
    m_Impl->masterVolume = v;
    if (m_Impl->initialized)
        ma_engine_set_volume(&m_Impl->engine, v);
}

float AudioContext::GetMasterVolume() const {
    return m_Impl ? m_Impl->masterVolume : 1.0f;
}

void AudioContext::SetListener(uint32_t index, const glm::vec3 &pos,
                               const glm::vec3 &forward, const glm::vec3 &up) {
    if (!m_Impl || !m_Impl->initialized)
        return;

    ma_engine_listener_set_position(&m_Impl->engine, index, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(&m_Impl->engine, index, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&m_Impl->engine, index, up.x, up.y, up.z);
}

void AudioContext::SetListenerVelocity(uint32_t index, const glm::vec3 &v) {
    if (!m_Impl || !m_Impl->initialized)
        return;

    ma_engine_listener_set_velocity(&m_Impl->engine, index, v.x, v.y, v.z);
}

VoiceHandle AudioContext::Play(SoundAsset *sound, bool loop, float volume, float pitch,
                               bool spatial, const glm::vec3 &pos, float minDist,
                               float maxDist, float rolloff,
                               AttenuationModel attenuation) {
    if (!m_Impl || !m_Impl->initialized || !sound)
        return kInvalidVoice;

    const std::string &filepath = sound->GetFilePath();
    if (filepath.empty())
        return kInvalidVoice;

    ma_uint32 flags = 0;
    if (!spatial)
        flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    std::unique_ptr<ma_sound> snd = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(&m_Impl->engine, filepath.c_str(), flags,
                                nullptr, nullptr, snd.get()) != MA_SUCCESS) {
        GE_CORE_WARN("AudioContext: failed to init sound \"{0}\"", filepath);
        return kInvalidVoice;
    }

    const VoiceHandle handle = m_Impl->nextHandle++;
    ma_sound *voice = snd.get();
    m_Impl->voices.emplace(handle, std::move(snd));

    ma_sound_set_looping(voice, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(voice, volume);
    ma_sound_set_pitch(voice, pitch);

    if (spatial) {
        ma_sound_set_positioning(voice, ma_positioning_absolute);
        ma_sound_set_position(voice, pos.x, pos.y, pos.z);
        ma_sound_set_attenuation_model(voice, ToMaAttenuation(attenuation));
        ma_sound_set_min_distance(voice, minDist);
        ma_sound_set_max_distance(voice, maxDist);
        ma_sound_set_rolloff(voice, rolloff);
    } else {
        ma_sound_set_positioning(voice, ma_positioning_relative);
    }

    if (ma_sound_start(voice) != MA_SUCCESS) {
        Stop(handle);
        return kInvalidVoice;
    }

    return handle;
}

void AudioContext::Stop(VoiceHandle h) {
    if (!m_Impl || !m_Impl->initialized || h == kInvalidVoice)
        return;

    auto it = m_Impl->voices.find(h);
    if (it == m_Impl->voices.end())
        return;

    ma_sound_stop(it->second.get());
    ma_sound_uninit(it->second.get());
    m_Impl->voices.erase(it);
}

void AudioContext::StopAll() {
    if (!m_Impl || !m_Impl->initialized)
        return;

    for (auto &[handle, sound] : m_Impl->voices) {
        (void)handle;
        ma_sound_stop(sound.get());
        ma_sound_uninit(sound.get());
    }
    m_Impl->voices.clear();
}

void AudioContext::SetVolume(VoiceHandle h, float v) {
    if (!m_Impl || !m_Impl->initialized || h == kInvalidVoice)
        return;

    auto it = m_Impl->voices.find(h);
    if (it != m_Impl->voices.end())
        ma_sound_set_volume(it->second.get(), v);
}

void AudioContext::SetPitch(VoiceHandle h, float p) {
    if (!m_Impl || !m_Impl->initialized || h == kInvalidVoice)
        return;

    auto it = m_Impl->voices.find(h);
    if (it != m_Impl->voices.end())
        ma_sound_set_pitch(it->second.get(), p);
}

void AudioContext::SetLoop(VoiceHandle h, bool loop) {
    if (!m_Impl || !m_Impl->initialized || h == kInvalidVoice)
        return;

    auto it = m_Impl->voices.find(h);
    if (it != m_Impl->voices.end())
        ma_sound_set_looping(it->second.get(), loop ? MA_TRUE : MA_FALSE);
}

void AudioContext::SetPosition(VoiceHandle h, const glm::vec3 &pos) {
    if (!m_Impl || !m_Impl->initialized || h == kInvalidVoice)
        return;

    auto it = m_Impl->voices.find(h);
    if (it != m_Impl->voices.end())
        ma_sound_set_position(it->second.get(), pos.x, pos.y, pos.z);
}

bool AudioContext::IsPlaying(VoiceHandle h) const {
    if (!m_Impl || !m_Impl->initialized || h == kInvalidVoice)
        return false;

    auto it = m_Impl->voices.find(h);
    return it != m_Impl->voices.end() && ma_sound_is_playing(it->second.get()) == MA_TRUE;
}

} // namespace Audio
} // namespace GE
