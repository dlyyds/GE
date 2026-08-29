/**
 * @file PhysicsTypes.h
 * @brief 物理系统类型定义 —— 枚举、类型别名和前向声明。
 *
 * 本文件设计为轻量级头文件，不引入 Jolt 重型头文件，
 * 可被 Components.h 等 ECS 头文件安全包含。
 */

#pragma once

#include "Core/Base.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <entt.hpp>
#include <glm/glm.hpp>

namespace GE::Physics {

/**
 * @brief 刚体运动类型。
 *
 * 直接映射 Jolt 的 EMotionType：
 * - Static：静态刚体，永不移动，用于场景中的固定碰撞体（地板、墙壁等）
 * - Kinematic：运动学刚体，由用户代码驱动位置，不受物理力影响但可碰撞动态体
 * - Dynamic：动态刚体，完全由物理模拟驱动（重力、碰撞、力）
 */
enum class RigidBodyType : uint8_t {
    Static    = 0, ///< 静态刚体（永不移动）
    Kinematic = 1, ///< 运动学刚体（由用户驱动）
    Dynamic   = 2  ///< 动态刚体（完全模拟）
};

/**
 * @brief 碰撞层 —— 用于控制哪些物体之间可以发生碰撞。
 *
 * 基于 Jolt 的 BroadPhaseLayer 机制，每个刚体属于一个碰撞层，
 * ObjectLayerPairFilter 决定两个层之间是否碰撞。
 *
 * Count 用于在编译期推导层数，不要作为实际层使用。
 */
enum class CollisionLayer : uint8_t {
    Default = 0, ///< 默认碰撞层
    Player  = 1, ///< 玩家
    Enemy   = 2, ///< 敌人
    Trigger = 3, ///< 触发器（不产生物理碰撞，只触发事件）
    Count         ///< 层总数（用于编译期推导）
};

/** @brief Jolt BodyID 别名，用作 ECS 组件与 Jolt 物理世界之间的句柄 */
using BodyID = JPH::BodyID;

/** @brief 无效的 BodyID，表示刚体尚未创建或已销毁 */
inline BodyID InvalidBodyID() { return JPH::BodyID(); }

/**
 * @brief 碰撞阶段 —— 与 Jolt ContactListener 三态一一映射。
 *
 * Enter = 新增接触 / Stay = 持续接触 / Exit = 接触消失。
 */
enum class CollisionPhase : uint8_t {
    Enter = 0,
    Stay  = 1,
    Exit  = 2
};

/**
 * @brief 实体级碰撞事件（阶段 B 桥接产物）。
 *
 * 由 Jolt 回调内环形缓冲（BodyID 对）在 Step 返回后、主线程上翻译成 ECS 实体对，
 * 再由 Scene::StepPhysics 尾部按实体双侧派发到脚本（OnCollision 系 / OnTrigger 系钩子）。
 * 事件到达即派发即弃，不留过帧状态。
 */
struct CollisionEvent {
    entt::entity A = entt::null;                     ///< 接触一侧实体
    entt::entity B = entt::null;                     ///< 接触另一侧实体
    CollisionPhase phase = CollisionPhase::Enter;
    bool isTrigger = false;                          ///< 是否涉及传感器（主线程由 RigidBodyComponent.IsSensor 推导）
    glm::vec3 normal = {0.0f, 0.0f, 0.0f};           ///< 世界空间法线（近似指向 A；Exit 无效）
    float impulse = 0.0f;                            ///< 法向冲量近似（仅 Enter 有效，kg·m/s）
};

} // namespace GE::Physics