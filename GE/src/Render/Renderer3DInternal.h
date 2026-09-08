/**
 * @file Renderer3DInternal.h
 * @brief Renderer3D 多分片实现之间共享的内部常量与辅助函数。
 */
#pragma once

#include <cstdint>

namespace GE {
namespace detail {

/// pipelineId 位布局：
///   bit0 = PBR 材质位（0 Blinn / 1 PBR）
///   bit1 = 蒙皮肤管线位（0 静态 / 1 蒙皮）
constexpr uint8_t kPbrPipelineBit  = 0x01;
constexpr uint8_t kSkinPipelineBit = 0x02;

constexpr uint8_t kPbrPipelineMask  = kPbrPipelineBit;
constexpr uint8_t kSkinPipelineMask = kSkinPipelineBit;

/// 基础管线 id（不含蒙皮位）
constexpr uint8_t kBlinnPipelineId = 0;
constexpr uint8_t kPbrPipelineId   = kPbrPipelineBit;

/// 完整管线 id（材质位 + 蒙皮位）
constexpr uint8_t kBlinnSkinnedPipelineId = kBlinnPipelineId | kSkinPipelineBit; ///< = 2
constexpr uint8_t kPbrSkinnedPipelineId   = kPbrPipelineId   | kSkinPipelineBit; ///< = 3

/// 由基础材质管线 id + 是否蒙皮组合出完整 pipelineId。
constexpr uint8_t ComposePipelineId(uint8_t materialId, bool skinned) {
    return static_cast<uint8_t>(materialId | (skinned ? kSkinPipelineBit : 0u));
}

/// 判断 pipelineId 是否带 PBR 位。
constexpr bool IsPbrPipeline(uint8_t pipelineId) {
    return (pipelineId & kPbrPipelineMask) != 0;
}

/// 判断 pipelineId 是否带蒙皮位。
constexpr bool IsSkinnedPipeline(uint8_t pipelineId) {
    return (pipelineId & kSkinPipelineMask) != 0;
}

} // namespace detail
} // namespace GE