#pragma once

#include "entt.hpp"


#include "Core/InputState.h"
#include "Core/Timestep.h"
#include "Render/BufferPool.h"
#include "Scene/ScriptEngine.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace GE {

class Entity;
class Event;

namespace Physics {
class PhysicsWorld;
struct CollisionEvent; // 定义在 PhysicsTypes.h（头文件只前向声明，避免引 Jolt）
}

class Scene {
public:
    Scene();

    ~Scene();

    Entity CreateEntity(const std::string &name = "Entity");

    void DestroyEntity(Entity entity);

    /**
     * @brief 设置父子关系（唯一改父子关系的入口）。
     *
     * 同步更新组件的 parent 指针与 Scene 侧反向索引两处，杜绝双真相漂移。
     * parent 为空实体（默认构造的 Entity）表示脱离父级、成为根节点。
     * 带环检测：若 parent 位于 child 的子树上（新边会构成环）则拒绝并返回 false，
     * 杜绝互设父子导致的 DFS 无限递归。
     *
     * @param child  要挂载/脱离的子实体
     * @param parent 新父实体（空实体 = 脱离为根）
     * @return true 成功；false 被拒（实体无效 / 环检测失败 / parent 无 Transform）
     */
    bool SetParent(Entity child, Entity parent);

    /// 查询父实体（无父返回空 Entity）
    Entity GetParent(Entity entity);

    /// 查询直接子实体列表（保插入序）
    std::vector<Entity> GetChildren(Entity entity);

    /// 全量扫描重建反向 children 索引（恢复现场 / 兜底；唯一真相在组件指针）
    void RebuildChildrenIndex();

    /// 清空场景全部实体与反向索引（触发各组件 on_destroy 清理）
    void ClearAllEntities();

    /// 按 ID 反查实体（无匹配返回空 Entity）
    Entity FindEntityByID(const std::string &uuid);

    // TEMP
    entt::registry &Reg() { return m_Registry; }

    /**
     * @brief 更新并渲染 2D 场景（仅精灵渲染）。
     * @param ts             时间步长
     * @param viewProjection 视口投影矩阵（用于 2D 精灵渲染）
     * @param clearColor     场景背景色（仅当需要清屏时使用，此处传给 Renderer2D::BeginScene）
     */
    void OnUpdate(Timestep ts,
                  const glm::mat4 &viewProjection,
                  const glm::vec4 &clearColor = {0.1f, 0.1f, 0.1f, 1.0f});

    /**
     * @brief 更新并渲染 3D 场景（网格 + 精灵叠加）。
     *
     * 先绘制所有 MeshRendererComponent 的 3D 网格（带深度测试和光照），
     * 再在其上叠加绘制 SpriteRendererComponent 的 2D 精灵（不清屏）。
     *
     * @param ts          时间步长
     * @param view        视图矩阵
     * @param projection  投影矩阵
     * @param viewPos     相机世界位置（用于光照计算）
     * @param clearColor  场景背景色
     */
    void OnUpdate3D(Timestep ts,
                    const glm::mat4 &view,
                    const glm::mat4 &projection,
                    const glm::vec3 &viewPos,
                    const glm::vec4 &clearColor = {0.1f, 0.1f, 0.1f, 1.0f});

    /**
     * @brief 场景事件入口。
     *
     * 处理系统级事件（如窗口大小变化），并将输入事件分发给所有 ScriptComponent。
     * 由外层 Layer 在其 OnEvent 中调用。
     *
     * @param e 事件对象
     */
    void OnEvent(Event &e);

    /// 是否将输入事件路由给主相机（鼠标/键盘控制相机视角）。
    /// 由编辑器依据「视口是否悬停、是否在拖 gizmo」等 UI 状态设置；
    /// 实际的相机输入路由在 Scene 内部完成。
    void SetProcessCameraInput(bool enabled) { m_ProcessCameraInput = enabled; }

    /// 输入快照：脚本查询入口（IsHeld/JustPressed/…），每场景一份
    const InputState &GetInputState() const { return m_InputState; }

    /// 可写输入快照（SceneLayer 在鼠标锁定/解锁瞬间重置增量基准用；脚本仍走 const 查询）。
    InputState &GetMutableInputState() { return m_InputState; }

    /// Lua 脚本引擎（Scene 持有，每场景共享一个 Lua 状态）
    ScriptEngine &GetScriptEngine() { return m_ScriptEngine; }

    /// 取实体 CharacterControllerComponent 的当前累计偏航（FPS 朝向写回用）。
    /// 返回 false 表示该实体无此组件。
    bool GetCharacterFacingYaw(entt::entity entity, float &outYaw) const;

    /**
     * @brief 视锥剔除粒度。
     *
     * Mesh    = 仅整网格级剔除（默认，保留现有行为）：整实体完全在视锥外才跳过。
     * SubMesh = 先做网格级粗筛（整实体完全在外直接跳过），再基于 SubMesh::aabb
     *           做子网格级细剔除，仅跳过完全在视锥外的子网格。
     * 两种模式均保守：AABB 无效或与任一平面相交都保留，避免误剔。
     */
    enum class CullingMode : uint8_t {
        Mesh = 0, ///< 仅整网格级剔除（默认，现有行为）
        SubMesh = 1, ///< 网格级粗筛 + 子网格级细剔除
    };

    /// 设置视锥剔除粒度（Mesh / SubMesh）。
    void SetCullingMode(CullingMode mode) { m_CullingMode = mode; }

    /// 当前视锥剔除粒度。
    CullingMode GetCullingMode() const { return m_CullingMode; }

    void OnViewportResize(uint32_t width, uint32_t height);

    /** @brief 获取物理世界指针（可能为 nullptr，如果物理系统未启用） */
    [[nodiscard]] Physics::PhysicsWorld *GetPhysicsWorld() const { return m_PhysicsWorld.get(); }

    /// 模拟运行态：Edit = 编辑器编辑（物理静止、可自由摆放），Playing = 运行时模拟（物理接管）
    enum class SimulationState : uint8_t { Edit, Playing };

    /// 是否正在运行时模拟（Play 状态；脚本/编辑器 UI 短路查询用）
    bool IsPlaying() const { return m_SimulationState == SimulationState::Playing; }

    /// Edit → Playing：备份 Transform 快照、flush 未初始化刚体/角色，首帧 Step 建体并模拟。
    /// 重复进入 -> 无操作（已 Playing）。
    void Play();

    /// Playing → Edit：销毁全部 body/角色、清空 pending、回滚快照回到摆放姿态。Edit 态调用 -> 无操作。
    void Stop();

    /**
     * @brief 获取主相机实体。
     *
     * 优先返回 CameraComponent.Primary == true 的第一个实体；
     * 若没有主相机，则返回第一个带 CameraComponent 的实体；
     * 场景中没有相机时返回空 Entity。
     */
    Entity GetPrimaryCameraEntity();

private:
    /// 每帧脚本更新（OnUpdate3D 子步骤）
    void UpdateScripts(Timestep ts);

    /// 物理步进（OnUpdate3D 子步骤）
    void StepPhysics(Timestep ts);

    /// 单条碰撞事件按实体双侧投递到脚本（法线各向自己，B 侧翻转；reg.valid 兜底）
    void DispatchCollisionEvent(const Physics::CollisionEvent &evt);

    /// 收集场景光源（方向光 / 环境光 / 点光源）到渲染器（OnUpdate3D 子步骤）。
    /// view/projection 用于按相机视锥计算方向光阴影的光空间 AABB（阴影贴图计划 §6.3）。
    void UpdateLightParams(const glm::mat4 &view, const glm::mat4 &projection);

    /// 依据环境组件驱动天空盒 + IBL（OnUpdate3D 子步骤）
    void UpdateEnvironment();

    /// 提交 3D 网格渲染（BeginScene + 子网格绘制 + EndScene）
    void RenderMeshes3D(const glm::mat4 &view, const glm::mat4 &projection,
                        const glm::vec3 &viewPos, const glm::vec4 &clearColor);

    /// 提交 2D 精灵渲染（世界空间批 + UI 空间批）
    void RenderSprites2D(const glm::mat4 &view, const glm::mat4 &projection);

    /// 每帧渲染前重建世界矩阵缓存（DFS：扫描 + 跳过非根，从根递归下钻）
    void UpdateWorldTransforms();

    /// DFS 递归体：写自己 world = parentWorld × local，再以下钻传递
    void UpdateWorldTransformsRecursive(entt::entity entity, const glm::mat4 &parentWorld);

    /// 每帧蒙皮更新：对每个 SkinComponent 计算 jointMatrix = world × IBM 并上传 SSBO。
    /// 必须在 UpdateWorldTransforms（DFS 算好全部关节 world）之后调用。
    void UpdateSkins();

    /// 每帧动画更新：采样当前 clip 键帧写目标实体的局部 TRS。
    /// 必须在 UpdateWorldTransforms（DFS 据此重算 world）之前调用。
    void UpdateAnimations(Timestep ts);

    /// 每帧第一人称跟随相机同步：鼠标视角 → 相机 yaw/pitch，相机朝向 →
    /// 角色朝向（FacingYaw 通道），角色位置 → 相机位置（EyeOffset）。
    /// 必须在 StepPhysics（物理子步消费 FacingYaw）之后、UpdateWorldTransforms 之前调用。
    void UpdateFirstPersonCamera();


    /// 将输入事件路由给主相机（控制相机视角）
    void DispatchInputEventToCamera(Event &e);

    entt::registry m_Registry;
    uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

    /// 反向索引：parent → 直接子实体（保插入序）。
    /// 派生缓存：唯一真相在组件 TransformComponent::parent，
    /// 层级变更只走 SetParent 单一入口同步两处；可用 RebuildChildrenIndex 全量重建兜底。
    std::unordered_map<entt::entity, std::vector<entt::entity> > m_ChildrenOf;

    /// 一条皮肤每帧上传的关节矩阵记录（SkinDef 去重，同皮肤多 node 共享一条）。
    struct SkinJointUpload {
        const void *skinDef = nullptr; ///< 共享皮肤定义（SkinDef*，去重键）
        uint32_t jointCount = 0; ///< 关节数量
        BufferAllocation jointBuffer; ///< 关节矩阵 SSBO 分配（仅本帧有效，帧结束池重置）
    };

    /// 本帧所有 SkinComponent 的关节矩阵上传记录（UpdateSkins 填充，渲染读用）。
    std::vector<SkinJointUpload> m_SkinJointUploads;

    /// 物理世界（每个 Scene 一个实例）
    std::unique_ptr<Physics::PhysicsWorld> m_PhysicsWorld;

    /// 是否将输入路由给主相机（由编辑器设置）
    bool m_ProcessCameraInput = false;

    /// 每场景一份的输入快照（脚本查询；非全局单例，多视口解耦）
    InputState m_InputState;

    /// Lua 脚本运行时（每场景一个共享 Lua 状态）
    ScriptEngine m_ScriptEngine;

    /// 视锥剔除粒度（默认仅网格级，保留现有行为）
    CullingMode m_CullingMode = CullingMode::Mesh;

    /// 模拟运行态（默认 Edit；瞬态不序列化，加载后恒为 Edit，见计划书决策 5.5）
    SimulationState m_SimulationState = SimulationState::Edit;

    /// Play(): 快照、Stop(): 回滚用的 Transform 备份（仅内存，Play 前摆放姿态）
    struct PlayTransformSnapshot {
        entt::entity entity = entt::null;
        glm::vec3 translation{0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    std::vector<PlayTransformSnapshot> m_PlaySnapshot;

    template <typename T>
    void OnComponentAdded(Entity entity, T &component);

    /// 实体销毁回调（用于清理物理 body）
    void OnRigidBodyDestroyed(entt::registry &registry, entt::entity entity);

    /// 碰撞体销毁回调（移除碰撞体时重建刚体形状）
    void OnColliderDestroyed(entt::registry &registry, entt::entity entity);

    /// 角色控制器销毁回调（销毁 Jolt CharacterVirtual）
    void OnCharacterControllerDestroyed(entt::registry &registry, entt::entity entity);

    /// 脚本组件销毁回调（清理 ScriptEngine 实例并调 OnDestroy）
    void OnScriptComponentDestroyed(entt::registry &registry, entt::entity entity);

    friend class Entity;
    friend class Physics::PhysicsWorld;

};

}