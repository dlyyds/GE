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
     * 先绘制所有 MeshComponent 的 3D 网格（带深度测试和光照），
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

    void OnViewportResize(uint32_t width, uint32_t height);

private:
    /// 将输入事件分发给所有 ScriptComponent
    void DispatchInputEventToScripts(Event &e);
    entt::registry m_Registry;
    uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

    /// 物理世界（每个 Scene 一个实例）
    std::unique_ptr<Physics::PhysicsWorld> m_PhysicsWorld;

    template <typename T>
    void OnComponentAdded(Entity entity, T &component);

    /// 实体销毁回调（用于清理物理 body）
    void OnRigidBodyDestroyed(entt::registry &registry, entt::entity entity);

    friend class Entity;
    friend class Physics::PhysicsWorld;

};

}