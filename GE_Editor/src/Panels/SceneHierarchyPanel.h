#pragma once

#include "GE/Core/Base.h"
#include "GE/Core/Log.h"
#include "GE/Scene/Scene.h"
#include "GE/Scene/Entity.h"

#include "GE/Debug/Assert.h"

namespace GE {

class SceneHierarchyPanel {
public:
    SceneHierarchyPanel() = default;

    explicit SceneHierarchyPanel(const Ref<Scene> &scene);

    void SetContext(const Ref<Scene> &scene);

    void SetSelectedEntity(Entity entity);

    void OnImGuiRender();

    [[nodiscard]] Entity GetSelectedEntity() const { return m_SelectionContext; }

private:
    void DrawEntityNode(Entity entity);

    void DrawComponents(Entity entity);

private:
    Ref<Scene> m_Context;
    Entity m_SelectionContext;
};

}