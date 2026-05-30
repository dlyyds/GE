#pragma once

#include "GE.h"
#include "Panels/ContentBrowserPanel.h"

#include "Panels/SceneHierarchyPanel.h"
#include "Renderer/EditorCamrea.h"

namespace GE {

class EditorLayer : public Layer {
public:
    EditorLayer();

    ~EditorLayer() override = default;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnImGuiRender() override;

    void OnEvent(Event &e) override;

private:
    OrthographicCameraController m_CameraController;

    bool OnKeyPressed(KeyPressedEvent &e);

    bool OnMouseButtonPressed(MouseButtonPressedEvent &e);

    void NewScene();

    void OpenScene();

    void OpenScene(const std::filesystem::path &path);

    void SaveSceneAs();

    // Temp
    Ref<VertexArray> m_SquareVA;
    Ref<Shader> m_FlatColorShader;
    Ref<Framebuffer> m_Framebuffer;


    Ref<Scene> m_ActiveScene;

    EditorCamera m_EditorCamera;

    Entity m_HoveredEntity;

    glm::vec2 m_ViewportSize = {1.0f, 1.0f};
    glm::vec2 m_ViewportBounds[2];

    bool m_ViewportFocused = false, m_ViewportHovered = false;

    Ref<Texture2D> m_CheckerboardTexture;

    glm::vec4 m_SquareColor = {0.2f, 0.3f, 0.8f, 1.0f};

    SceneHierarchyPanel m_SceneHierarchyPanel;
    ContentBrowserPanel m_ContentBrowserPanel;

    int m_GizmoType = -1;
};

}