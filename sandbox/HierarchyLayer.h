#pragma once

#include "GE/GE.h"

#include "EditorContext.h"
#include "Panels/SceneHierarchyPanel.h"

#include <memory>

namespace GE {

/// 场景层级面板层 —— 独立承载 SceneHierarchyPanel（"Scene Hierarchy" + "Properties" 窗口）。
///
/// 与场景层共享 EditorContext。场景对象在新建/加载时会被替换，本层在每帧
/// OnImGuiRender 前对比场景指针，变了就重新绑定面板并清空选中，避免悬空。
class HierarchyLayer : public Layer {
public:
    explicit HierarchyLayer(std::shared_ptr<EditorContext> context);

    ~HierarchyLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override;

private:
    std::shared_ptr<EditorContext> m_Context;  ///< 共享场景上下文
    SceneHierarchyPanel m_Panel;               ///< 场景层级面板（ImGui）
    Scene *m_LastScene = nullptr;              ///< 上次绑定的场景对象（用于感知替换）
};

} // namespace GE