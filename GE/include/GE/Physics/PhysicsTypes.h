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

} // namespace GE::Physics