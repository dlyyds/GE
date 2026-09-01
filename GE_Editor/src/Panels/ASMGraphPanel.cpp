//
// 动画状态机节点图面板实现。
// 阶段 A：占位窗口 —— 停靠「动画状态机图」窗口，无选中/无 ASM 时占位提示。
// 阶段 B：只读图渲染 —— 把 AnimStateMachineComponent 的 states 画成节点（含虚拟 ANY
//         节点）、transitions 画成连线；当前状态节点实时高亮 + 显示 stateTime。
//         交互编辑（拖拽建转换 / 删除 / 选中改属性）在阶段 C 落地。
//
// 仅用 ax::NodeEditor 的只读 API：CreateEditor / Begin / BeginNode / BeginPin /
// Link / GetNodePosition / SetNodePosition 等。交互类 API（BeginCreate/BeginDelete
// 等）留给阶段 C，本阶段不触碰选中/删除。
//

#include "Panels/ASMGraphPanel.h"

#include "HierarchyLayer.h"
#include "GE/Scene/AnimationComponents.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace GE {
namespace ed = ax::NodeEditor;

// ---- ID 编码常量（与计划书 §2.2 一致）----
// NodeId / PinId 都是 uintptr_t。为避免多实体共享同一编辑器上下文时撞 ID，统一编码为
// (实体 id << 16) | 状态下标；PinId 再用高位 bit 区分方向（输出 0x8000 / 输入 0x4000）。
// LinkId 用转换下标（每帧按 transitions 重建）。状态下标只占低位 16 bit，单实体状态数
// 上限 65536，足够。
namespace {
constexpr uintptr_t kPinOutputFlag = 0x8000u;
constexpr uintptr_t kPinInputFlag  = 0x4000u;
constexpr uintptr_t kStateIndexMask = 0xFFFFu;

/// 状态默认网格间距（像素）
constexpr float kGridSpacingX = 300.0f;
constexpr float kGridSpacingY = 220.0f;
} // namespace

uintptr_t ASMGraphPanel::EncodeNodeId(size_t entityId, size_t stateIndex) {
    return (static_cast<uintptr_t>(entityId) << 16) | static_cast<uintptr_t>(stateIndex & kStateIndexMask);
}

uintptr_t ASMGraphPanel::EncodePinId(size_t entityId, size_t stateIndex, bool output) {
    return EncodeNodeId(entityId, stateIndex) | (output ? kPinOutputFlag : kPinInputFlag);
}

size_t ASMGraphPanel::DecodeStateIndex(uintptr_t encoded) {
    return static_cast<size_t>(encoded & kStateIndexMask);
}

void ASMGraphPanel::LayoutNode(size_t stateIndex) {
    // 按声明序网格摆放：col = 下标 % 列数，row = 下标 / 列数
    const int col = static_cast<int>(stateIndex % kGridColumns);
    const int row = static_cast<int>(stateIndex / kGridColumns);
    ed::SetNodePosition(ed::NodeId(EncodeNodeId(m_EntityId, stateIndex)),
                        ImVec2(col * kGridSpacingX, row * kGridSpacingY));
}

ASMGraphPanel::ASMGraphPanel(std::shared_ptr<EditorContext> context, HierarchyLayer *hierarchy)
    : m_Context(std::move(context)), m_Hierarchy(hierarchy) {
}

ASMGraphPanel::~ASMGraphPanel() {
    if (m_EditorCtx) {
        ed::DestroyEditor(m_EditorCtx);
        m_EditorCtx = nullptr;
    }
}

void ASMGraphPanel::OnImGuiRender() {
    // 根上下文取一次停靠目标 ID（与 DockSpaceLayer 中 GetID("MainDockspace") 一致）
    if (m_DockSpaceID == 0) {
        m_DockSpaceID = ImGui::GetID("MainDockspace");
    }

    ImGui::SetNextWindowDockID(m_DockSpaceID, ImGuiCond_FirstUseEver);
    ImGui::Begin("动画状态机图");

    // 无场景 / 无选中实体 / 选中实体无 ASM 组件 → 占位提示
    if (!m_Context || !m_Hierarchy) {
        ImGui::TextDisabled("未绑定场景与层级面板");
        ImGui::End();
        return;
    }

    Entity selected = m_Hierarchy->GetSelectedEntity();
    if (!selected || !selected.HasComponent<AnimStateMachineComponent>()) {
        ImGui::TextDisabled("请先在 Scene Hierarchy 中选中带「Anim State Machine」组件的实体");
        ImGui::End();
        return;
    }

    // 懒创建节点图编辑器上下文（带 SettingsFile：节点坐标自动落盘跨启动保持）
    if (!m_EditorCtx) {
        ed::Config cfg;
        cfg.SettingsFile = "asm_graph.json";
        m_EditorCtx = ed::CreateEditor(&cfg);
    }

    AnimStateMachineComponent &asmc = selected.GetComponent<AnimStateMachineComponent>();

    // 整个面板内容放外层 Child（占满窗口）：
    //   - 顶栏一行（实体名 / 状态机开关 / 重新布局）
    //   - ed::Begin 画布在 Child 内、顶栏之后，GetContentRegionAvail() 量到的是
    //     Child 剩余全部区域 → 画布撑满窗口剩余；且 Child 尺寸稳定（= 窗口尺寸），
    //     NavigateAction 的尺寸连续化不漂移，缩放/平移不乱跑。
    // 注意：ed::Begin 必须也在 Child 内。若画布留在 Child 外（EndChild 之后），
    // 主窗口剩余高度只剩一行，画布高度 ≈ 0，节点全部被裁 → 什么都看不见。
    ImGui::BeginChild("##asmBody", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::TextUnformatted(selected.GetComponent<TagComponent>().Tag.c_str());
        ImGui::SameLine();
        ImGui::Checkbox("状态机", &asmc.enabled);
        ImGui::SameLine();
        if (ImGui::Button("重新布局")) {
            // 兜底：中间增删状态导致 NodeId 下标错位时，按声明序重置所有节点坐标
            for (size_t i = 0; i < asmc.states.size(); ++i) {
                LayoutNode(i);
            }
            // DrawASMGraph 的 ed::Begin/End 在设置布局后紧跟执行，此处只置位标志，
            // 由下一帧画布导航聚焦内容（重新布局按钮也支持直接触发 NavigateToContent）
            m_RequestNavigateContent = true;
        }

        // 画布：ed::Begin 之前不插其它控件，GetContentRegionAvail() 即 Child 剩余全部区域
        DrawASMGraph(asmc);
    }
    ImGui::EndChild();

    ImGui::End();
}

void ASMGraphPanel::DrawASMGraph(AnimStateMachineComponent &asmc) {
    const auto &states = asmc.states;
    const auto &transitions = asmc.transitions;

    // 画布：ed::Begin 之前不插任何控件，GetContentRegionAvail() 即外层 Child 剩余全部区域
    ed::SetCurrentEditor(m_EditorCtx);
    ed::Begin("ASM Graph", ImVec2(0.0f, 0.0f));

    // ---- 实体 id（NodeId 高位，跨实体隔离）----
    const entt::entity handle = static_cast<entt::entity>(m_Hierarchy->GetSelectedEntity());
    const uintptr_t entityId = static_cast<uintptr_t>(entt::to_integral(handle));
    m_EntityId = entityId;

    // 首次布局标记：实体切换 / 状态增删时复位，下帧对坐标仍在原点的节点补一次网格布局。
    // 首次见到实体时 ANY 节点也要摆到固定角落（避免与状态0都在 (0,0) 重叠，ANY 被盖住拖不动）。
    const bool firstSeenEntity = (m_LastEntityId != entityId);
    if (firstSeenEntity || m_LastStateCount != states.size()) {
        m_LastEntityId = entityId;
        m_LastStateCount = states.size();
        m_NeedsInitialLayout = true;
    }

    // 重新布局按钮的导航请求：在 Begin/End 内部触发（需 current editor 已设置）
    if (m_RequestNavigateContent) {
        m_RequestNavigateContent = false;
        ed::NavigateToContent();
    }

    // ---- 虚拟 ANY 节点：固定画布左上（首次布局 / 重新布局时重置到角落），from==SIZE_MAX 的公共源 ----
    const ed::NodeId anyNodeId(EncodeNodeId(entityId, kStateIndexMask));
    if (firstSeenEntity || m_NeedsInitialLayout) {
        ed::SetNodePosition(anyNodeId, ImVec2(20.0f, 20.0f));
    }
    {
        ed::BeginNode(anyNodeId);
        ImGui::Text("ANY");
        ImGui::TextDisabled("全局");
        // 输出引脚（右侧）：作为 from==ANY 转换的源
        ed::BeginPin(ed::PinId(EncodePinId(entityId, kStateIndexMask, true)), ed::PinKind::Output);
        ed::PinPivotRect(ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f));
        ed::EndPin();
        ed::EndNode();
    }

    // ---- 状态节点：标题 + 初始★/当前▶ 标记 + 输入/输出引脚 ----
    for (size_t i = 0; i < states.size(); ++i) {
        const AnimStateDef &st = states[i];
        const ed::NodeId nodeId(EncodeNodeId(entityId, i));
        const ed::PinId outPin(EncodePinId(entityId, i, true));
        const ed::PinId inPin(EncodePinId(entityId, i, false));

        // 首次见到该状态（位置在原点）→ 按声明序网格摆放，避免叠在原点。
        // 注意：只有「原点」才触发布局，实体切换后已有坐标的状态保持原位置（计划书 D1）。
        const ImVec2 pos = ed::GetNodePosition(nodeId);
        if (pos.x == 0.0f && pos.y == 0.0f) {
            LayoutNode(i);
        }

        ed::BeginNode(nodeId);

        // 标题行：状态名 + 初始★/当前▶ 标记；当前状态用醒目颜色描边
        const bool isInitial = (st.name == asmc.initialState);
        const bool isCurrent = (asmc.current == i);
        ImGui::BeginGroup();
        {
            if (isCurrent) {
                // 当前状态标题：高亮色文字 + 左侧▶
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "▶ ");
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(st.name.empty() ? "(未命名)" : st.name.c_str());
            if (isInitial) {
                ImGui::SameLine();
                ImGui::TextDisabled("★");
            }
            if (isCurrent) {
                ImGui::TextDisabled("stateTime %.2fs", asmc.stateTime);
            }
        }
        ImGui::EndGroup();

        // 输入引脚（左侧）：作为转换目标
        ed::BeginPin(inPin, ed::PinKind::Input);
        ImGui::Text("输入");
        ed::EndPin();

        // 输出引脚（右侧）：作为转换源
        ed::BeginPin(outPin, ed::PinKind::Output);
        ImGui::Text("输出");
        ed::EndPin();

        ed::EndNode();
    }

    // ---- 连线：每条转换一条 Link（from==SIZE_MAX 从 ANY 输出引脚出发）----
    for (size_t j = 0; j < transitions.size(); ++j) {
        const AnimTransitionDef &tr = transitions[j];
        // 旧场景可能残留越界 from/to：画图前过滤，不崩（计划书 §6）
        if (tr.to >= states.size()) {
            continue;
        }
        if (tr.from != SIZE_MAX && tr.from >= states.size()) {
            continue;
        }

        const ed::PinId startPin = (tr.from == SIZE_MAX)
            ? ed::PinId(EncodePinId(entityId, kStateIndexMask, true))   // ANY 输出引脚
            : ed::PinId(EncodePinId(entityId, tr.from, true));          // 源状态输出引脚
        const ed::PinId endPin = ed::PinId(EncodePinId(entityId, tr.to, false)); // 目标状态输入引脚

        // 连线上标签：过渡时长 + 条件数
        const ImVec4 linkColor = (tr.from == SIZE_MAX)
            ? ImVec4(0.3f, 0.6f, 1.0f, 1.0f)   // ANY 连线：蓝色
            : ImVec4(0.5f, 0.8f, 0.5f, 1.0f);  // 普通连线：绿色

        const ed::LinkId linkId(static_cast<uintptr_t>(j));
        ed::Link(linkId, startPin, endPin, linkColor, 2.0f);
    }

    // 复位首次布局标记（ANY 节点首帧已摆到角落）
    m_NeedsInitialLayout = false;

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

} // namespace GE
