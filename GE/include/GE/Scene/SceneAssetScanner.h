/**
 * @file SceneAssetScanner.h
 * @brief 场景 YAML 的资产引用只读扫描器 —— 打包侧（gepack）的依赖图入口。
 *
 * 为什么不用 SceneSerializer::Deserialize 读场景：它全程依赖 Renderer / AssetManager
 * （LoadMesh / LoadTextureAsync / ApplyMaterialNode / SetEnvironment），而 AssetManager
 * 的构造需要 VulkanDevice，故没有任何「无 GPU 读场景」的路径。打包 CLI 必须在没有显卡、
 * 没有窗口的环境下跑，所以资产引用的知识在这里再实现一份只读版本。
 *
 * 代价是字段名有两处（SceneSerializer 写、这里读）。SceneSerializer 的每个资产字段写出点
 * 都加了注释回指本文件——改那边时请同步这里的扫描分支，否则打包会静静地漏收资产。
 *
 * 扫描器只做「场景文档 → 引用清单」，不负责归一/展开/校验：
 *   - 伪键（builtin: / solid:）原样返回，交由调用方用 AssetPathUtil::IsPseudoKey 跳过
 *   - Script 的路径基准是 assets/scripts（不是资源根），Environment 的 Name 是目录名不是路径
 *   - Clip 是 "path#N" 源键，运行期读的是派生的 .geanim（见 DeriveAnimationBakePath）
 */

#pragma once

#include <string>
#include <vector>

namespace GE {

/// 资产引用种类 —— 决定打包侧如何递归展开与校验
enum class AssetRefKind {
    Mesh,        ///< 网格：MeshRenderer.Mesh / Skin.Mesh（.gemesh，可能带 #N 源键）
    Texture,     ///< 贴图：内联材质四槽 / Water.NormalMap,ColorMap / SpriteRenderer.Texture
    Audio,       ///< 音频：AudioSource.Sounds[].SoundPath
    Script,      ///< Lua 脚本：Script.ScriptPath（相对 assets/scripts）
    Clip,        ///< 动画：Animation.Clips[].Clip（"foo.gltf#N" 源键）
    Material,    ///< 材质资产：MeshRenderer.MaterialOverrides.<i>.Material（.gemat）
    Environment, ///< 环境：Environment.Name（assets/environments/<Name>/ 的目录名）
};

/// 一条场景里的资产引用
struct AssetRef {
    AssetRefKind kind = AssetRefKind::Texture;
    std::string raw;      ///< 场景文件里写的原始串（未归一）
    std::string location; ///< 定位串 "<场景显示名>:<行号>"，报错时直接给用户看
};

struct SceneScanResult {
    std::vector<AssetRef> refs;
    std::string error; ///< 非空 = 场景读不了 / 顶层结构不对（打包侧按前置条件失败处理）

    bool Ok() const { return error.empty(); }
};

/**
 * @brief 扫描场景文件里的全部资产引用。
 *
 * @param scenePath   场景文件的实际路径（用于读盘）
 * @param displayName 报告里显示的场景名（通常是相对资源根的规范形，如 "scenes/2.scene"）；
 *                    为空则用 scenePath
 * @return 引用清单；读盘/YAML 解析失败时 error 非空、refs 为空
 */
SceneScanResult ScanSceneAssetRefs(const std::string &scenePath,
                                   const std::string &displayName = {});

} // namespace GE
