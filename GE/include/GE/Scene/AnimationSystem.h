#pragma once

#include "Scene/AnimationComponents.h"
#include "Core/Timestep.h"
#include "entt.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace GE {

class ScriptEngine;

/// 动画子系统 —— 采样 / 推进 / 事件 / 过渡混合 / ASM 求值 的独立模块（从 Scene.cpp 抽出）。
///
/// 运行时入口 Scene::UpdateAnimations 委托到 AnimationSystem::UpdateAnimations
/// （Scene.cpp 只留薄壳）。采样函数同时供编辑器 AutoFitBoundingBoxToJoints 复用，
/// 避免编辑器与引擎各持一份采样实现导致漂移。
/// 详见 docs/骨骼动画实现计划书.md §4 与 docs/动画状态机ASM计划书.md §2.3。
namespace AnimationSystem {

/// 定位时间 t 所在的键帧区间左端 k0（带 hint 缓存，时间单调推进免二分）
size_t FindActiveKey(const std::vector<float> &times, float t, uint32_t &hint);

/// vec3 通道采样：LINEAR 线性插值；STEP 取前一键值；CUBICSPLINE 按 LINEAR 近似
glm::vec3 SampleVec3Channel(const AnimationChannel &ch, float t, uint32_t &hint);

/// quat 通道采样：LINEAR 用 slerp；STEP 取前一键值；CUBICSPLINE 近似 slerp
glm::quat SampleQuatChannel(const AnimationChannel &ch, float t, uint32_t &hint);

/// 推进 clip 播放时间（含 loop 回绕 / 非循环钳制）
void AdvanceClipTime(float &t, float dt, float duration, bool loop);

/// 动画事件区间检测：跨过 e.time（prev < e.time <= cur）触发脚本 OnAnimationEvent
void FireEvents(ScriptEngine &engine, entt::entity entity,
                const std::vector<AnimationEvent> &evts,
                float prev, float cur, float duration, bool loop);

/// 过渡期双路求值：以 (entity, path) 键合按 α 混合，统一写回局部 TRS
void BlendAndApplyTransition(entt::registry &registry, AnimationComponent &ac, float alpha);

/// 每实体动画更新：ASM 求值 + 时间轴推进 + 事件 + 采样应用。
/// Scene::OnUpdate3D 每帧调用（Scene::UpdateAnimations 的实现体，语义见计划书 §2.4）。
void UpdateAnimations(entt::registry &registry, ScriptEngine &scriptEngine, Timestep ts);

/// 重载片段源：重读 entity 动画组件各 clip 的源文件，按其中动画目录整体重载——
/// 已挂 clip 强制重建键帧（跳过缓存与 .geanim 烘焙）并同步场景内所有同源实体，
/// 保持共享不变，保留各 ClipInstance 的通道目标实体与场景级事件表；
/// 源文件里新增的动画作为新 clip 追加到该组件（通道目标按同文件既存 clip 的
/// nodeIndex 映射解析，未出现过的节点洞掉为绑定姿态）。
/// 单文件加载失败时该文件全部 clip 保留旧数据并告警。返回是否至少成功一条。
bool ReloadClipSource(entt::registry &registry, entt::entity entity);

} // namespace AnimationSystem

} // namespace GE