#pragma once

#include "GE/GE.h"

#include "EditorContext.h"
#include "Panels/ASMGraphPanel.h"

#include <memory>

namespace GE {

class HierarchyLayer; // 前向声明，避免在头文件引入实现

/// 动画状态机节点图面板层 —— 独立承载 ASMGraphPanel（"动画状态机图" 窗口）。
///
/// 与 GizmoController 同款接线：持有 EditorContext + HierarchyLayer*（每帧轮询选中实体）。
/// 场景对象在新建/加载时会被替换，面板每帧按需从 context 取最新 Scene，无需重绑。
class ASMGraphLayer : public Layer {
public:
    explicit ASMGraphLayer(std::shared_ptr<EditorContext> context, HierarchyLayer *hierarchy);

    ~ASMGraphLayer() override;

    void OnAttach() override;

    void OnDetach() override;

    void OnUpdate(Timestep &ts) override;

    void OnEvent(Event &event) override;

    void OnImGuiRender() override;

private:
    std::shared_ptr<EditorContext> m_Context; ///< 共享场景上下文
    ASMGraphPanel m_Panel;                    ///< 动画状态机节点图面板（ImGui）
};

} // namespace GE
