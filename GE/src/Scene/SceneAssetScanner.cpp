/**
 * @file SceneAssetScanner.cpp
 * @brief 场景 YAML 的资产引用只读扫描器实现（yaml-cpp，无 GPU 依赖）。
 */

#include "Scene/SceneAssetScanner.h"

#include <yaml-cpp/yaml.h>

namespace GE {

namespace {

/// 一行定位串：yaml-cpp 的 Mark 行号 0 起，报错时给人看要 +1
std::string MakeLocation(const std::string &sceneName, const YAML::Node &node) {
    return sceneName + ":" + std::to_string(node.Mark().line + 1);
}

void AddRef(std::vector<AssetRef> &out, AssetRefKind kind, const YAML::Node &node,
            const std::string &sceneName) {
    if (!node || !node.IsScalar()) {
        return;
    }
    const std::string raw = node.as<std::string>("");
    if (raw.empty()) {
        return;
    }
    out.push_back(AssetRef{kind, raw, MakeLocation(sceneName, node)});
}

/// 内联材质节点的四个贴图槽（引用形态只写一行 Material: x.gemat，走另一个分支）
void ScanInlineMaterial(const YAML::Node &matNode, std::vector<AssetRef> &out,
                        const std::string &sceneName) {
    AddRef(out, AssetRefKind::Texture, matNode["AlbedoTexture"], sceneName);
    AddRef(out, AssetRefKind::Texture, matNode["NormalTexture"], sceneName);
    AddRef(out, AssetRefKind::Texture, matNode["EmissiveTexture"], sceneName);
    AddRef(out, AssetRefKind::Texture, matNode["MetallicRoughnessTexture"], sceneName);
}

void ScanComponentBlocks(const YAML::Node &entity, std::vector<AssetRef> &out,
                         const std::string &sceneName) {
    // ---- MeshRenderer：网格 + 材质覆写（引用形态 → .gemat；内联形态 → 四槽贴图）----
    if (const YAML::Node mr = entity["MeshRenderer"]) {
        AddRef(out, AssetRefKind::Mesh, mr["Mesh"], sceneName);
        if (const YAML::Node overrides = mr["MaterialOverrides"]) {
            for (const auto &kv : overrides) {
                const YAML::Node matNode = kv.second;
                if (!matNode || !matNode.IsMap()) {
                    continue;
                }
                if (matNode["Material"]) {
                    AddRef(out, AssetRefKind::Material, matNode["Material"], sceneName);
                } else {
                    ScanInlineMaterial(matNode, out, sceneName);
                }
            }
        }
    }

    // ---- Skin：蒙皮网格（与 MeshRenderer 常指向同一份 .gemesh）----
    if (const YAML::Node skin = entity["Skin"]) {
        AddRef(out, AssetRefKind::Mesh, skin["Mesh"], sceneName);
    }

    // ---- 精灵贴图 ----
    if (const YAML::Node sprite = entity["SpriteRenderer"]) {
        AddRef(out, AssetRefKind::Texture, sprite["Texture"], sceneName);
    }

    // ---- 水面：法线细节图 / 颜色图（组件可选，仅部分场景有）----
    if (const YAML::Node water = entity["Water"]) {
        AddRef(out, AssetRefKind::Texture, water["NormalMap"], sceneName);
        AddRef(out, AssetRefKind::Texture, water["ColorMap"], sceneName);
    }

    // ---- 音频：一个实体可挂多个音效槽 ----
    if (const YAML::Node audio = entity["AudioSource"]) {
        if (const YAML::Node sounds = audio["Sounds"]) {
            for (const auto &s : sounds) {
                AddRef(out, AssetRefKind::Audio, s["SoundPath"], sceneName);
            }
        }
    }

    // ---- 脚本：路径基准是 assets/scripts，不是资源根 ----
    if (const YAML::Node script = entity["Script"]) {
        AddRef(out, AssetRefKind::Script, script["ScriptPath"], sceneName);
    }

    // ---- 动画：Clips[].Clip 是 "path#N" 源键（注意 AnimStateMachine 里也有 Clip 键，
    //      但那是状态机引用的 clip 名，不是文件路径，故此处不扫）----
    if (const YAML::Node anim = entity["Animation"]) {
        if (const YAML::Node clips = anim["Clips"]) {
            for (const auto &c : clips) {
                AddRef(out, AssetRefKind::Clip, c["Clip"], sceneName);
            }
        }
    }

    // ---- 环境：Name 是 assets/environments/ 下的目录名（IBL/天空盒由场景组件驱动，
    //      game.cfg 里没有 environment 键）。Enabled 为 false 也照收：宁可多带几 MB，
    //      也不要在运行时因开关组合而缺文件。----
    if (const YAML::Node env = entity["Environment"]) {
        AddRef(out, AssetRefKind::Environment, env["Name"], sceneName);
    }
}

} // namespace

SceneScanResult ScanSceneAssetRefs(const std::string &scenePath,
                                   const std::string &displayName) {
    SceneScanResult result;
    const std::string sceneName = displayName.empty() ? scenePath : displayName;

    YAML::Node root;
    try {
        root = YAML::LoadFile(scenePath);
    } catch (const std::exception &e) {
        result.error = "场景文件读取/解析失败: " + sceneName + "（" + e.what() + "）";
        return result;
    }

    const YAML::Node entities = root["Scene"]["Entities"];
    if (!entities) {
        result.error = "场景缺少 Scene.Entities 段: " + sceneName;
        return result;
    }
    if (!entities.IsSequence()) {
        result.error = "Scene.Entities 不是序列: " + sceneName;
        return result;
    }

    for (const auto &entity : entities) {
        if (!entity.IsMap()) {
            continue;
        }
        ScanComponentBlocks(entity, result.refs, sceneName);
    }
    return result;
}

} // namespace GE
