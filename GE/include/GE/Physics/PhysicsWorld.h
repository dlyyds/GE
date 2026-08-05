/**
 * @file PhysicsWorld.h
 * @brief 物理世界 —— 封装 Jolt PhysicsSystem，每个 Scene 一个实例。
 *
 * 职责：
 * - 初始化 / 销毁 Jolt 物理系统
 * - 每帧物理步进（固定步长 + 时间累加器）
 * - 创建 / 销毁刚体（OnComponentAdded / OnComponentRemoved 回调触发）
 * - 同步 Jolt body 与 TransformComponent
 *
 * 与 ECS 的关系：
 * - RigidBodyComponent 存 BodyID，是 Jolt body 的"句柄"
 * - PhysicsWorld 持有 JPH::PhysicsSystem，不持有 ECS 所有权
 * - Scene 在 OnUpdate3D 中调用 PhysicsWorld::Step()
 */

#pragma once

#include "Core/Base.h"
#include "Core/Timestep.h"
#include "Physics/PhysicsTypes.h"

#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <entt.hpp>

// Jolt 前向声明，避免在头文件中引入重型 Jolt 头文件
namespace JPH {
    class PhysicsSystem;
    class BodyInterface;
    class TempAllocator;
    class JobSystemThreadPool;
    class BroadPhaseLayerInterface;
    class ObjectVsBroadPhaseLayerFilter;
    class ObjectLayerPairFilter;

    template <class T> class RefConst;
    class Shape;
    using ShapeRefC = RefConst<Shape>;
}

namespace GE {

class Scene;
struct TransformComponent;

namespace Physics {

/**
 * @brief 物理世界 —— 封装 Jolt 物理系统，每个 Scene 一个实例。
 *
 * 使用固定步长（60Hz）+ 时间累加器模式进行物理步进，
 * 在动态体与 TransformComponent 之间双向同步。
 */
class PhysicsWorld {
public:
    /**
     * @brief 构造并初始化 Jolt 物理系统。
     * @param scene 关联的 Scene 指针（用于访问 ECS registry）
     */
    explicit PhysicsWorld(Scene *scene);

    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld &) = delete;
    PhysicsWorld &operator=(const PhysicsWorld &) = delete;

    // ========================================================================
    // 步进
    // ========================================================================

    /**
     * @brief 执行物理步进。内部使用固定步长 + 时间累加器。
     *
     * 调用顺序：
     * 1. 处理待创建刚体列表（延迟创建机制）
     * 2. 同步运动学体 Transform → Jolt
     * 3. Jolt PhysicsSystem::Update（固定步长子步）
     * 4. 同步动态体 Jolt → Transform
     *
     * @param ts 帧时间（秒）
     */
    void Step(Timestep ts);

    // ========================================================================
    // 刚体管理（由 Scene 的 OnComponentAdded 回调触发）
    // ========================================================================

    /**
     * @brief 请求创建 Jolt 刚体。
     *
     * 采用延迟创建策略：此方法只是将 entity 加入待创建列表，
     * 实际创建发生在下一次 Step() 调用时。
     * 这样允许用户先加 RigidBodyComponent，再加 collider 组件，
     * 避免组件添加顺序依赖。
     *
     * @param entity 实体句柄
     */
    void RequestCreateRigidBody(entt::entity entity);

    /**
     * @brief 销毁 Jolt 刚体（实体销毁或移除 RigidBodyComponent 时调用）。
     * @param entity 实体句柄
     */
    void DestroyRigidBody(entt::entity entity);

    // ========================================================================
    // 重力
    // ========================================================================

    /** @brief 设置重力加速度（m/s²），默认 (0, -9.81, 0) */
    void SetGravity(const glm::vec3 &gravity);

    /** @brief 获取当前重力加速度 */
    [[nodiscard]] glm::vec3 GetGravity() const { return m_Gravity; }

private:
    // ========================================================================
    // 内部辅助
    // ========================================================================

    /** @brief 初始化 Jolt 核心组件（PhysicsSystem、JobSystem 等） */
    void InitJolt();

    /** @brief 根据 ECS 组件构建 JPH::Shape（组合所有 collider 组件） */
    JPH::ShapeRefC BuildShapeForEntity(entt::entity entity);

    /** @brief 将动态体从 Jolt 同步到 TransformComponent（位置 + 旋转） */
    void SyncBodiesToTransforms();

    /** @brief 将运动学体从 TransformComponent 同步到 Jolt */
    void SyncKinematicTransformsToBodies();

    /** @brief 处理待创建刚体列表 */
    void ProcessPendingBodies();

    /** @brief 判断 entity 是否有碰撞体组件 */
    bool HasColliderComponent(entt::entity entity) const;

    // ========================================================================
    // GLM ↔ Jolt 数学转换辅助
    // ========================================================================

    static JPH::Vec3  ToJoltVec3(const glm::vec3 &v);
    static glm::vec3  ToGlmVec3(const JPH::Vec3 &v);
    static JPH::Quat  ToJoltQuat(const glm::quat &q);
    static glm::quat  ToGlmQuat(const JPH::Quat &q);

    // ========================================================================
    // 成员变量
    // ========================================================================

    Scene *m_Scene = nullptr;

    // Jolt 核心对象（用 unique_ptr + 前向声明，避免头文件引入重型 Jolt 头）
    std::unique_ptr<JPH::PhysicsSystem>             m_PhysicsSystem;
    std::unique_ptr<JPH::TempAllocator>             m_TempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool>        m_JobSystem;
    std::unique_ptr<JPH::BroadPhaseLayerInterface>   m_BroadPhaseLayerInterface;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilter> m_ObjectVsBroadPhaseFilter;
    std::unique_ptr<JPH::ObjectLayerPairFilter>      m_ObjectLayerPairFilter;

    // 时间累加器（固定步长）
    float m_Accumulator = 0.0f;
    static constexpr float FIXED_TIMESTEP = 1.0f / 60.0f;
    static constexpr int   MAX_SUBSTEPS = 5;

    // 重力
    glm::vec3 m_Gravity = {0.0f, -9.81f, 0.0f};

    // 待创建刚体列表（延迟创建机制）
    std::vector<entt::entity> m_PendingBodies;
};

} // namespace Physics
} // namespace GE