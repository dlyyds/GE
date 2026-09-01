#pragma once

#include "GE/GE.h"

#include "EditorContext.h"

#include <memory>

namespace GE {

class HierarchyLayer; // 前向声明，避免循环包含（ASMGraphLayer 需 HierarchyLayer 的选中实体）

/// 动画状态机节点图面板 —— 用 imgui-node-editor 把 AnimStateMachineComponent 画成节点图。
///
/// 阶段 A：仅占位窗口 —— 打开「动画状态机图」停靠窗口，无选中实体 / 选中实体无
/// ASM 组件时显示占位提示。节点图渲染与交互在阶段 B/C 落地。
///
/// 与 GizmoController 同款接线：持有 EditorContext（共享场景）+ HierarchyLayer*（每帧
/// 轮询当前选中实体）。面板不拥有场景，仅读写选中实体上的 AnimStateMachineComponent。
class ASMGraphPanel {
public:
    ASMGraphPanel() = default;

    ASMGraphPanel(std::shared_ptr<EditorContext> context, HierarchyLayer *hierarchy);

    /// 每帧 ImGui 渲染
    void OnImGuiRender();

private:
    std::shared_ptr<EditorContext> m_Context; ///< 共享场景上下文（非拥有）
    HierarchyLayer *m_Hierarchy = nullptr;    ///< 选中实体来源（每帧轮询，非拥有）

    /// 停靠目标 DockSpace ID（根上下文取 "MainDockspace"，首帧初始化一次）
    ImGuiID m_DockSpaceID = 0;
};

} // namespace GE
