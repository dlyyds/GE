#pragma once

#include "entt.hpp"


#include "Core/Timestep.h"
#include <glm/glm.hpp>

namespace GE {

class Entity;

class Scene {
public:
    Scene();

    ~Scene();

    Entity CreateEntity(const std::string &name = "Entity");

    void DestroyEntity(Entity entity);

    // TEMP
    entt::registry &Reg() { return m_Registry; }

    /**
     * @brief 更新并渲染场景。
     * @param ts             时间步长
     * @param viewProjection 视口投影矩阵（用于 2D 精灵渲染）
     * @param clearColor     场景背景色（仅当需要清屏时使用，此处传给 Renderer2D::BeginScene）
     */
    void OnUpdate(Timestep ts,
                  const glm::mat4 &viewProjection,
                  const glm::vec4 &clearColor = {0.1f, 0.1f, 0.1f, 1.0f});

    void OnViewportResize(uint32_t width, uint32_t height);

private:
    entt::registry m_Registry;
    uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

    template <typename T>
    void OnComponentAdded(Entity entity, T &component);

    friend class Entity;

};

}