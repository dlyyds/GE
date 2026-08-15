#pragma once

#include "entt.hpp"


#include "Core/Timestep.h"
#include <glm/glm.hpp>
#include <memory>

namespace GE {

class Entity;
class Event;
namespace Physics { class PhysicsWorld; }

class Scene {
public:
    Scene();

    ~Scene();

    Entity CreateEntity(const std::string &name = "Entity");

    void DestroyEntity(Entity entity);

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

    void OnViewportResize(uint32_t width, uint32_t height);

    /** @brief 获取物理世界指针（可能为 nullptr，如果物理系统未启用） */
    [[nodiscard]] Physics::PhysicsWorld *GetPhysicsWorld() const { return m_PhysicsWorld.get(); }

    /**
     * @brief 获取主相机实体。
     *
     * 优先返回 CameraComponent.Primary == true 的第一个实体；
     * 若没有主相机，则返回第一个带 CameraComponent 的实体；
     * 场景中没有相机时返回空 Entity。
     */
    Entity GetPrimaryCameraEntity();

private:
    /// 将输入事件分发给所有 ScriptComponent
    void DispatchInputEventToScripts(Event &e);

    /// 将输入事件路由给主相机（控制相机视角）
    void DispatchInputEventToCamera(Event &e);

    entt::registry m_Registry;
    uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

    /// 物理世界（每个 Scene 一个实例）
    std::unique_ptr<Physics::PhysicsWorld> m_PhysicsWorld;

    /// 当前已加载环境的名称（用于 EnvironmentComponent 切换时重建 IBL）
    std::string m_EnvironmentName;

    /// 是否将输入路由给主相机（由编辑器设置）
    bool m_ProcessCameraInput = false;

    template <typename T>
    void OnComponentAdded(Entity entity, T &component);

    /// 实体销毁回调（用于清理物理 body）
    void OnRigidBodyDestroyed(entt::registry &registry, entt::entity entity);

    /// 碰撞体销毁回调（移除碰撞体时重建刚体形状）
    void OnColliderDestroyed(entt::registry &registry, entt::entity entity);

    friend class Entity;
    friend class Physics::PhysicsWorld;

};

}