/**
 * @file ShadowCascade.h
 * @brief CSM（级联阴影映射）共享常量—— 轻量头，无任何 Vulkan 依赖。
 *
 * 独立成头文件：`kMaxCascades` 同时被 Renderer3D.h（LightParams 级联数组）与
 * Scene.h（每级阴影视锥 m_ShadowVolume 数组）引用。仿照 Render/AABB.h 的做法
 * 单独放置，Scene.h 不必连带引入 Render/Mesh.h / vulkan.hpp 的重头（见 AABB.h 注释）。
 */

#pragma once

#include <cstdint>

namespace GE {

/// CSM 上限级数：方向光阴影沿相机深度分档的最大档数（CSM 计划书 §1）。
/// 默认生效级数见 LightParams::cascadeCount（阶段 1 保持 1 = 现状单级）。
inline constexpr uint32_t kMaxCascades = 4;

} // namespace GE
