#pragma once

#include "Audio/AudioTypes.h"

#include <string>
#include <vector>

namespace GE {

/// 一条声音槽：一个文件 + 该声音自己的播放参数（可序列化）。
/// 运行时 Voice 句柄不写回这里，由 AudioWorld 内部维护。
struct AudioSound {
    std::string SoundPath;                   ///< audio/ 下相对路径（含扩展名），空 = 未绑定
    std::string Name;                        ///< 槽位名，编辑器/脚本用；空 = 用索引
    bool PlayOnAwake = false;                ///< Scene::Play 后自动开始
    bool Loop = false;
    float Volume = 1.0f;
    float Pitch = 1.0f;
    float MinDistance = 1.0f;                ///< 衰减近距（仅 Spatial）
    float MaxDistance = 100.0f;              ///< 衰减远距 / 截断（仅 Spatial）
    float Rolloff = 1.0f;
    Audio::AttenuationModel Attenuation = Audio::AttenuationModel::Inverse;

    AudioSound() = default;
    explicit AudioSound(std::string path) : SoundPath(std::move(path)) {}
};

/// 多声音槽位容器：一个实体可以同时发出多个声音。
struct AudioSourceComponent {
    bool Enabled = true;
    bool Spatial = true;                     ///< 整个源的空间性；false = 2D 全向
    std::vector<AudioSound> Sounds;

    /// 编辑器 UI 暂存（不序列化）：面板“试听”的槽位索引；-1 = 无请求
    int uiPreviewSlot = -1;

    AudioSourceComponent() = default;
};

/// 音频监听器：运行时位置来自实体 Transform。
/// 无 Listener 实体时退化到主相机实体。
struct AudioListenerComponent {
    bool Enabled = true;

    AudioListenerComponent() = default;
};

} // namespace GE
