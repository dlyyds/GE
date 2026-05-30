#pragma once

#include "entt.hpp"


#include "Core/Timestep.h"

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

    void OnUpdateRuntime(Timestep ts);

    void OnUpdateEditor(Timestep ts, EditorCamera &camera);

    void OnViewportResize(uint32_t width, uint32_t height);

    Entity GetPrimaryCameraEntity();

private:
    entt::registry m_Registry;
    uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

    template <typename T>
    void OnComponentAdded(Entity entity, T &component);

    friend class Entity;
    friend class SceneHierarchyPanel;
    friend class SceneSerializer;
};

}