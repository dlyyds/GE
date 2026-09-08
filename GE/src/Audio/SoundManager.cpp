#include "Audio/SoundManager.h"

#include "Audio/AudioContext.h"
#include "Audio/SoundAsset.h"
#include "Core/Application.h"
#include "Core/Log.h"

#include <memory>
#include <utility>

namespace GE {
namespace Audio {

SoundAsset *SoundManager::Load(const std::string &filepath) {
    if (filepath.empty())
        return nullptr;

    auto it = m_Sounds.find(filepath);
    if (it != m_Sounds.end())
        return it->second.get();

    auto asset = std::make_shared<SoundAsset>();
    asset->m_FilePath = filepath;
    (void)SoundAsset::ProbeMetadata(filepath, asset->m_SampleRate, asset->m_FrameCount);

    m_Sounds.emplace(filepath, asset);
    GE_CORE_INFO("SoundManager: loaded {0} ({1} Hz, {2} frames)",
                 filepath, asset->m_SampleRate, asset->m_FrameCount);
    return asset.get();
}

SoundAsset *SoundManager::Get(const std::string &filepath) const {
    auto it = m_Sounds.find(filepath);
    return (it != m_Sounds.end()) ? it->second.get() : nullptr;
}

bool SoundManager::Has(const std::string &filepath) const {
    return m_Sounds.find(filepath) != m_Sounds.end();
}

bool SoundManager::PlayOneShot(const std::string &filepath, float volume) {
    AudioContext *ctx = Application::Get().GetAudioContext();
    if (!ctx)
        return false;

    SoundAsset *sound = Load(filepath);
    if (!sound)
        return false;

    const VoiceHandle handle = ctx->Play(sound, false, volume, 1.0f, false,
                                         glm::vec3(0.0f), 1.0f, 100.0f, 1.0f);
    return handle != kInvalidVoice;
}

void SoundManager::Clear() {
    m_Sounds.clear();
}

} // namespace Audio
} // namespace GE

