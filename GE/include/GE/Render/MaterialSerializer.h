/**
 * @file MaterialSerializer.h
 * @brief 材质 ↔ YAML 的**唯一**形状实现 —— `.gemat` 资产与场景内联材质节点共用。
 *
 * 抽出来的理由：材质的 YAML 形状原先只散落在 SceneSerializer.cpp 的匿名命名空间里
 * （场景覆写的存/取两个消费者）。加入独立 `.gemat` 资产格式后消费者变四处，
 * 两份实现迟早漂移，故收敛成一份。
 *
 * 调用约定：
 * - 写：先 WriteSamplerNode 填贴图槽的采样器，再 WriteMaterialNode 填主体。
 * - 读：ApplyMaterialNode 按节点内容**整体重写**材质（不是打补丁），
 *   保证「拿到的材质 == 文件里写的材质」。
 *
 * 形状（`.gemat` 文件与场景内联覆写节点完全同构，仅后者可换成 { Material: <路径> } 引用）：
 * @code
 *   Version: 1                 # 可选，缺省视为 1
 *   Type: PBR                  # PBR | BlinnPhong
 *   Name: 棋盘
 *   BaseColor: [1, 1, 1, 1]    # 无贴图时的固有色（纯色 albedo 的源头）
 *   AlphaMode: Opaque          # Opaque | Mask | Blend
 *   AlphaCutoff: 0.5           # 仅 Mask 时写
 *   DoubleSided: false
 *   AlbedoTexture: textures/Checkerboard.png
 *   AlbedoTextureSampler: { MagFilter: Nearest, ... }
 *   EmissiveFactor: [0, 0, 0]
 *   FloatParams: { metallic: 0, roughness: 0.5, uvTiling: 1 }
 * @endcode
 */

#pragma once

#include "Render/Material.h"

#include <yaml-cpp/yaml.h>

#include <string>

namespace GE {

class Texture;

namespace MaterialSerializer {

/// 材质节点格式版本（`.gemat` 文件头）。节点缺 Version 键时视为 1。
inline constexpr int kFormatVersion = 1;

/// 材质纹理槽位名（与 Material::TextureSlot 顺序一一对应）
inline constexpr const char *kTextureSlotNames[] = {
    "Albedo", "Normal", "Emissive", "MetallicRoughness"};

// ============================================================================
// 路径归一
// ============================================================================

/**
 * @brief 把资产引用归一为规范形（相对资源根、正斜杠）后写入材质节点。
 *
 * 归一失败（根外绝对路径 / 越界）时原样返回并告警——保存不因此失败，
 * 具体问题留给打包校验显式报错。
 *
 * @param raw       原始引用（"solid:*" 伪键 / 相对资源根 / 绝对路径）
 * @param assetRoot 资源根
 * @return 应写入 YAML 的路径字符串
 */
std::string CanonicalAssetRef(const std::string &raw, const std::string &assetRoot);

// ============================================================================
// 纹理槽位
// ============================================================================

/// 纹理槽位名 → 槽位枚举（未知名回退 Albedo）
Material::TextureSlot TextureSlotFromName(const std::string &name);

// ============================================================================
// 写入
// ============================================================================

/**
 * @brief 写采样器参数（供材质贴图槽 / 精灵纹理共用同一形状）。
 *
 * 纹理指针为空时为空操作。
 */
void WriteSamplerNode(YAML::Node &out, const Texture *tex);

/**
 * @brief 把材质内容写入节点（纹理槽 + 标量参数 + 渲染状态 + 固有色）。
 *
 * 不写 FormatVersion（由 `.gemat` 读写层负责）与源文件路径（路径即注册身份，
 * 不落进内容）。只写有文件路径的贴图槽；无路径的纯色 albedo 由 BaseColor 承载。
 *
 * @param out       输出节点（就地填充）
 * @param mat       材质
 * @param assetRoot 资源根（贴图路径归一用）
 */
void WriteMaterialNode(YAML::Node &out, const Material &mat, const std::string &assetRoot);

// ============================================================================
// 读取
// ============================================================================

/**
 * @brief 按节点内容整体重写材质（类型 / 纹理槽 / 标量参数 / 自发光 / 渲染状态 / 固有色）。
 *
 * 与「新建材质」路径共用：先建空材质再调用本函数，即可得到与节点一致的内容。
 * 节点缺某字段时退回该字段的默认语义（旧场景无新字段也能读）。
 *
 * @param mat  目标材质（内容整体按节点重写）
 * @param node 材质节点
 */
void ApplyMaterialNode(Material &mat, const YAML::Node &node);

/// 按节点内容重建采样器参数（纹理为空或节点缺失时为空操作）。
void ApplySamplerNode(Texture *tex, const YAML::Node &node);

// ============================================================================
// 内容签名（去重 / 变更检测）
// ============================================================================

/**
 * @brief YAML 节点的稳定文本签名（Flow 风格，单行）。
 *
 * 同一节点内容恒得同一签名，可用于「按内容去重」与「这一帧内容变了没有」。
 */
std::string NodeSignature(const YAML::Node &node);

/**
 * @brief 材质内容的稳定签名（= 把材质写成节点后取 NodeSignature）。
 *
 * 用于按内容去重、以及编辑器判断某材质的可序列化内容在本帧是否被改动过。
 * 用签名而不是手写字段清单比较，形状改了签名自动跟着改——手写清单每加一个
 * 可序列化字段就得记得同步，漏一个就会出现「改了却判断为没改」。
 */
std::string ContentSignature(const Material &mat, const std::string &assetRoot);

// ============================================================================
// 枚举 ↔ 字符串
// ============================================================================

/// Material::AlphaMode → 字符串（Opaque / Mask / Blend）
const char *AlphaModeToString(Material::AlphaMode mode);

/// 字符串 → Material::AlphaMode（未知值回退 Opaque）
Material::AlphaMode AlphaModeFromString(const std::string &s);

} // namespace MaterialSerializer

} // namespace GE
