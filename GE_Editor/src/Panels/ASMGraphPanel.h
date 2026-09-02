#pragma once

#include "GE/GE.h"

#include "EditorContext.h"

#include "imgui.h" // ImGuiID / DockBuilder 系列需要 imgui.h（与 GizmoController 同款 include 模式）
#include "imgui_node_editor.h" // ax::NodeEditor：节点图画布（阶段 B 起）

#include <memory>
#include <vector>

namespace GE {

class HierarchyLayer; // 前向声明，避免循环包含（ASMGraphLayer 需 HierarchyLayer 的选中实体）

/// 动画状态机节点图面板 —— 用 imgui-node-editor 把 AnimStateMachineComponent 画成节点图。
///
/// 阶段 A：仅占位窗口 —— 打开「动画状态机图」停靠窗口，无选中实体 / 选中实体无
/// ASM 组件时显示占位提示。
/// 阶段 B：只读图渲染 —— 把 states 画成节点（含虚拟 ANY 节点）、transitions 画成连线，
/// 当前状态节点实时高亮 + 显示 stateTime。交互编辑（拖拽建转换/删除/选中改属性）在阶段 C。
///
/// 与 GizmoController 同款接线：持有 EditorContext（共享场景）+ HierarchyLayer*（每帧
/// 轮询当前选中实体）。面板不拥有场景，仅读写选中实体上的 AnimStateMachineComponent。
///
/// 图↔数据映射（详见 docs/动画状态机节点图编辑器计划书.md §2）：
///   - 节点 = states[i]；虚拟 ANY 节点 = transitions[].from==SIZE_MAX 的公共源（不进 states）
///   - 连线 = transitions[j]（from==SIZE_MAX 的连线从 ANY 的输出引脚出发）
///   - 多实体共享同一 ed::EditorContext：NodeId = (实体id << 32) | (命名空间 << 16) |
///     状态下标；PinId 用独立命名空间区分方向（输出 / 输入），与 NodeId 区间不重叠；
///     ANY 节点单独一个命名空间。LinkId = 转换下标
///   - 节点位置由库 SettingsFile 持久化（"asm_graph.json"），跨启动保持
class ASMGraphPanel {
public:
    ASMGraphPanel() = default;

    ASMGraphPanel(std::shared_ptr<EditorContext> context, HierarchyLayer *hierarchy);

    ~ASMGraphPanel();

    /// 每帧 ImGui 渲染
    void OnImGuiRender();

private:
    /// 节点/引脚 ID 编码（详见计划书 §2.2）
    static uintptr_t EncodeNodeId(size_t entityId, size_t stateIndex);
    static uintptr_t EncodePinId(size_t entityId, size_t stateIndex, bool output);
    /// ANY 虚拟节点专用编码（独立命名空间，与状态节点/引脚区间不重叠）
    static uintptr_t EncodeAnyNodeId(size_t entityId);
    /// 由编码反解状态下标（LinkId 解码 / 节点选中反查用）
    static size_t DecodeStateIndex(uintptr_t encoded);

    /// 按声明序网格布局一个节点（首次见到某状态 / 手动重新布局时用）
    void LayoutNode(size_t stateIndex);

    void DrawASMGraph(AnimStateMachineComponent &asmc);

    std::shared_ptr<EditorContext> m_Context; ///< 共享场景上下文（非拥有）
    HierarchyLayer *m_Hierarchy = nullptr;    ///< 选中实体来源（每帧轮询，非拥有）

    /// 停靠目标 DockSpace ID（根上下文取 "MainDockspace"，首帧初始化一次）
    ImGuiID m_DockSpaceID = 0;

    /// 节点图编辑器上下文（CreateEditor 创建，DestroyEditor 销毁）
    ax::NodeEditor::EditorContext *m_EditorCtx = nullptr;

    /// 当前实体 id（NodeId 高位编码；首帧 / 实体切换时刷新）
    uintptr_t m_EntityId = 0;

    /// 上一帧所属实体 id + 状态数（实体切换 / 状态增删时复位「首次布局」标记）
    uintptr_t m_LastEntityId = static_cast<uintptr_t>(-1);
    size_t m_LastStateCount = static_cast<size_t>(-1);

    /// 本帧需补一次首次布局（实体切换 / 状态数变化后置位，DrawASMGraph 末尾复位）
    bool m_NeedsInitialLayout = false;

    /// 重新布局按钮请求导航（置位后由下帧画布内触发 NavigateToContent，按钮在画布外）
    bool m_RequestNavigateContent = false;

    /// 网格列数（按状态数自适应，≥1）
    static constexpr int kGridColumns = 4;
};

} // namespace GE
