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
#include <unordered_map>
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
    class CharacterVirtual;

    template <class T> class RefConst;
    class Shape;
    using ShapeRefC = RefConst<Shape>;
}

namespace GE {

class Scene;
struct TransformComponent;

namespace Physics {

/**
 * @brief 接触事件缓冲（阶段 B）：环形缓冲 + 引擎 ContactListener + Stay 开关。
 * 定义放 PhysicsWorld.cpp（持 Jolt 类型），头文件仅前向声明隔离 Jolt。
 */
class ContactEventBuffer;

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

    /**
     * @brief 同步更新已创建刚体的属性（质量、摩擦、弹性、阻尼、传感器标记）。
     *
     * 用于编辑器面板修改属性后同步到 Jolt 物理世界。
     * 不涉及运动类型或碰撞形状的改变。
     *
     * @param entity 实体句柄
     */
    void UpdateRigidBodyProperties(entt::entity entity);

    /**
     * @brief 销毁并重建 Jolt 刚体（用于运动类型改变或碰撞形状改变时）。
     *
     * 先销毁当前 body，再将 entity 加入待创建列表，
     * 下一次 Step() 时会根据最新组件数据重新创建。
     *
     * @param entity 实体句柄
     */
    void RebuildRigidBody(entt::entity entity);

    // ========================================================================
    // 角色控制器（CharacterVirtual，由 Scene 的 OnComponentAdded 回调触发）
    // ========================================================================

    /**
     * @brief 请求创建 Jolt CharacterVirtual（延迟创建）。
     *
     * 与刚体同样走「先入 pending，下次 Step 再创建」的策略，取当前 Transform
     * 作初始位置/旋转。角色实体不挂 RigidBodyComponent，两条刚体同步循环不受影响。
     *
     * @param entity 实体句柄（需带 TransformComponent + CharacterControllerComponent）
     */
    void RequestCreateCharacter(entt::entity entity);

    /**
     * @brief 销毁 Jolt CharacterVirtual（实体销毁或移除角色组件时调用）。
     *
     * CharacterVirtual 析构会自动 RemoveBody + DestroyBody 清理 inner body。
     * @param entity 实体句柄
     */
    void DestroyCharacter(entt::entity entity);

    /**
     * @brief 销毁并重建角色（编辑器面板改 Radius/Height/MaxSlopeAngle 时用）。
     * @param entity 实体句柄
     */
    void RebuildCharacter(entt::entity entity);

    // ========================================================================
    // 重力
    // ========================================================================

    /** @brief 设置重力加速度（m/s²），默认 (0, -9.81, 0) */
    void SetGravity(const glm::vec3 &gravity);

    /** @brief 获取当前重力加速度 */
    [[nodiscard]] glm::vec3 GetGravity() const { return m_Gravity; }

    // ========================================================================
    // 碰撞事件（阶段 B）
    // ========================================================================

    /**
     * @brief 开关 Stay 高频通道（Step 前由 Scene 设）。
     * 任何脚本订阅了 OnCollisionStay/OnTriggerStay 时置 true，Persisted 回调才写缓冲；
     * 否则 Stay 零开销（计划书 B4）。
     */
    void SetStayEnabled(bool on);

    /**
     * @brief 取本帧碰撞事件列表（Step 后消费，紧邻调用）。
     *
     * 返回的是 Step 内部把环形缓冲翻译成的实体级事件；本帧有效，
     * 下次 Step 会整体重建，调用方须在当帧派发完（阶段 B 约定：到达即派发即弃）。
     */
    [[nodiscard]] const std::vector<CollisionEvent> &TakeCollisionEvents() const;

    // ========================================================================
    // 运行态生命周期（阶段 C：Edit/Play 分离）
    // ========================================================================

    /**
     * @brief 清空待创建刚体列表（Stop 时调用）。
     *
     * 销毁动作只收已初始化的 body；pending 里"编辑态加未建 / Play 后又加"的请求
     * 用此接口一并清空，避免残留句柄在下个 Play 周期建出错位 body。
     */
    void ClearPendingBodies();

    /**
     * @brief 清空待创建角色列表（Stop 时调用，与 ClearPendingBodies 同语义）。
     */
    void ClearPendingCharacters();

    /**
     * @brief 时间累加器归零（Play/Stop 时调用）。
     *
     * 编辑态不步进、累加器本应恒 0；上轮 Play 残留的累积时间若不清理，
     * 下次 Play 首帧会连跑多个子步、画面"抖一下"。
     */
    void ResetAccumulator();

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

    /** @brief Step 步进后：把环形缓冲（BodyID 对）翻译成本帧实体级碰撞事件（主线程可碰 ECS） */
    void CollectCollisionEvents();

    /** @brief 处理待创建角色列表（角色延迟创建，参考 ProcessPendingBodies） */
    void ProcessPendingCharacters();

    /** @brief 每个物理子步推进所有角色：合成速度（重力 + 水平期望）→ ExtendedUpdate */
    void UpdateCharacters(float dt);

    /** @brief 步进后将 CharacterVirtual 位置/旋转写回 TransformComponent */
    void SyncCharacterTransformsToComponents();

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

    // 角色控制器：<实体, CharacterVirtual>；析构顺序先于 m_PhysicsSystem（见析构函数）
    std::unordered_map<entt::entity, std::unique_ptr<JPH::CharacterVirtual>> m_Characters;
    // 待创建角色列表（延迟创建机制）
    std::vector<entt::entity> m_PendingCharacters;

    // 接触事件（阶段 B）：环形缓冲（Listener 写）+ 本帧实体级事件（Step 后填）
    std::unique_ptr<ContactEventBuffer> m_ContactBuffer;
    std::vector<CollisionEvent>         m_CollisionEvents;
};

} // namespace Physics
} // namespace GE