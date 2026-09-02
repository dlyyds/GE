#pragma once

#include "GE/GE.h"

#include "EditorContext.h"

#include "imgui.h" // ImGuiID / DockBuilder 系列需要 imgui.h（与 GizmoController 同款 include 模式）
#include "imgui_node_editor.h" // ax::NodeEditor：节点图画布（阶段 B 起）
#include "NodeEditorUtils/widgets.h" // ax::Widgets::Icon：引脚图标（阶段 4）

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

    /// 一次性配置节点图编辑器全局样式（深色主题 / 网格 / 节点描边 / 连线方向）
    void SetupStyle();

    /// 样式已应用标记：SetupStyle 需在 SetCurrentEditor 之后调用（GetStyle 依赖当前编辑器），
    /// 首帧在 DrawASMGraph 内 set current 后应用一次，之后不再重置
    bool m_StyleApplied = false;

    /// 画单个状态节点（Builder：Header 色条 + 左输入/右输出引脚 Icon）
    void DrawStateNode(AnimStateMachineComponent &asmc, size_t stateIndex,
                       const ax::NodeEditor::NodeId &nodeId, const ax::NodeEditor::PinId &inPin,
                       const ax::NodeEditor::PinId &outPin);

    void DrawASMGraph(AnimStateMachineComponent &asmc);

    // ---- 阶段 C：交互编辑（拖拽建转换 / Del 删除 / 点选改属性）----

    /// 画布内拖拽建转换（C1）：BeginCreate 循环里 QueryNewLink，输出 pin → 输入 pin 校验后
    /// push AnimTransitionDef{from, to, 0.25f, {}}；重复 (from,to)/同实体不匹配/同向引脚拒绝。
    void HandleCreateTransition(AnimStateMachineComponent &asmc);

    /// 画布内响应 Del 删除（C2）：收集被删状态集合 A（NodeId 解码）与被删连线集合 B
    /// （LinkId==转换下标），过滤后一次性 ApplyRemovals 重写。ANY 虚拟节点不可删，RejectDeletedItem。
    void HandleDeleteSelection(AnimStateMachineComponent &asmc);

    /// 批量删除并维护引用一致：states 剔除 A、transitions 剔除 from/to 引用 A 状态者
    /// 及下标在 B 者，再统一重编号。语义与 SceneHierarchyPanel 列表面板删状态一致
    /// （计划书 C2 / §2.4）；A/B 必须已升序去重。ANY（from==SIZE_MAX）永不为下标。
    void ApplyRemovals(AnimStateMachineComponent &asmc,
                       const std::vector<size_t> &statesToErase,
                       const std::vector<size_t> &linksToErase);

    /// 底部属性编辑区（C3）：按 m_SelKind 渲染选中状态（name/clip/loop/speed/设初始）
    /// 或转换（blendSec + 条件列表）的属性编辑；无选中/多选显示提示。
    void DrawProperties(AnimStateMachineComponent &asmc, const AnimationComponent *ac);

    /// 画布内查询当前选中（Begin 后调用，属性编辑区在 ed::End 之后据此渲染）。
    /// m_SelKind/m_SelState/m_SelLink 是属性区「展示目标」：仅当画布出现明确的单选
    /// 状态/连线时才切换；点空白 / 点 ANY / 选到已删对象保留上一帧展示目标（属性窗不关），
    /// 框选多选置 Mixed 并清掉旧单选展示。
    void QuerySelection(AnimStateMachineComponent &asmc);

    /// 清空属性区展示目标（实体切换时调用：旧实体的状态/连线下标不再有意义）
    void ResetSelection();

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

    /// 重新布局请求：按钮置位，由下帧 DrawASMGraph 画布内（SetCurrentEditor 之后）统一
    /// 重摆所有状态节点 + ANY 节点，随后置 m_RequestNavigateContent 让再下一帧导航聚焦。
    /// 不能在按钮处直接做 —— ed::SetNodePosition 依赖库静态 s_Editor（当前编辑器），
    /// 而按钮在 ed::Begin/End 之外执行时 s_Editor 为 nullptr，同步调用会空指针崩溃
    ///（vector::data this==null）。
    bool m_RequestRelayout = false;

    /// 重新布局完成后导航到内容（置位后由下帧画布内触发 NavigateToContent）
    bool m_RequestNavigateContent = false;

    /// 网格列数（按状态数自适应，≥1）
    static constexpr int kGridColumns = 4;

    /// 属性区展示目标类型（QuerySelection 每帧按画布选中更新；点空白/ANY 保留旧值）
    enum class SelKind { None, State, Link, Mixed };
    SelKind m_SelKind = SelKind::None;   ///< 当前展示目标（State/Link 时属性区编辑之）
    size_t m_SelState = SIZE_MAX;        ///< 展示状态下标（SelKind==State 时有效）
    size_t m_SelLink = SIZE_MAX;         ///< 展示连线下标（SelKind==Link 时有效）

    /// 画布/属性区可拖拽分割条厚度（像素）
    static constexpr float kSplitterH = 4.0f;
    /// 底部属性区高度（像素；无选中为 0=画布撑满）。点选展开后从初始高度（210）起步，
    /// 用户拖拽分割条实时改此值并跨选中记忆。
    float m_PropHeight = 210.0f;
    /// 分割条拖拽基准：按下瞬间的属性区高度（高度 = 基准 + 鼠标位移，避免拖动抖动）
    float m_PropDragStartH = 0.0f;
    /// 分割条拖拽基准：按下瞬间的鼠标屏幕 y
    float m_PropDragStartY = 0.0f;
    /// 分割条是否正在被拖动（按下后即使鼠标移出条也继续，直到松开）
    bool m_PropDragging = false;
    /// 底部属性区高度下限（像素，避免拖没）
    static constexpr float kPropMinHeight = 80.0f;
    /// 画布区最小高度（像素）：拖拽分割条时始终给画布留至少这么高
    static constexpr float kCanvasMinH = 60.0f;
};

} // namespace GE
