#pragma once

#include "Audio/AudioTypes.h"
#include "Audio/SoundAsset.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace GE {
namespace Audio {

/// 声音资源缓存管理器：按路径去重，生命周期与 Renderer/AssetManager 一致。
class SoundManager {
public:
    SoundManager() = default;
    ~SoundManager() = default;

    SoundManager(const SoundManager &) = delete;
    SoundManager &operator=(const SoundManager &) = delete;

    /// 加载并缓存声音（路径已解析）。失败返回 nullptr。
    SoundAsset *Load(const std::string &filepath);

    /// 查询已缓存的声音（不触发加载）。
    [[nodiscard]] SoundAsset *Get(const std::string &filepath) const;

    [[nodiscard]] bool Has(const std::string &filepath) const;

    /// 直接播放一次（编辑器试听 / 一次性 SFX）。不持有句柄。
    bool PlayOneShot(const std::string &filepath, float volume = 1.0f);

    /// 清空缓存（AudioContext Shutdown 前应由外部先停掉全部声音）。
    void Clear();

private:
    std::unordered_map<std::string, std::shared_ptr<SoundAsset>> m_Sounds;
};

} // namespace Audio
} // namespace GE
