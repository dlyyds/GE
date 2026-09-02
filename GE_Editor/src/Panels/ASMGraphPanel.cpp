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
#include <cstdio> // snprintf：连线标签格式化

namespace GE {
namespace ed = ax::NodeEditor;

// ---- ID 编码常量（与计划书 §2.2 一致）----
// NodeId / PinId 都是 uintptr_t。为避免多实体共享同一编辑器上下文时撞 ID，统一编码为
// (实体 id << 32) | (命名空间 << 16) | 状态下标；命名空间按类型分开，NodeId / PinId /
// ANY 节点三区间互不相交，绝对不冲突。LinkId 用转换下标（每帧按 transitions 重建）。
// 实体 id 限 32 bit、状态下标限 16 bit，单实体状态数上限 65536，足够。
namespace {
/// ID 位布局（64bit，绝对不冲突）：
///   NodeId = (实体id << 32) | (命名空间 << 16) | 状态下标
///   PinId  = NodeId | 引脚方向标记
/// 命名空间按类型分开（kNsAny=0 / kNsState=1 / kNsPinOutput=2 / kNsPinInput=3），
/// 三种 ID 区间互不相交；状态下标 16 bit，单实体状态数上限 65536。
constexpr uintptr_t kNamespaceShift = 16u;
constexpr uintptr_t kStateIndexMask = 0xFFFFu;
constexpr uintptr_t kEntityIdShift = 32u;
constexpr uintptr_t kEntityIdMask = 0xFFFFFFFFu; // 实体 id 限 32 bit
constexpr uintptr_t kNamespaceAny = 0u; // ANY 虚拟节点
constexpr uintptr_t kNamespaceState = 1u; // 普通状态节点
constexpr uintptr_t kNamespacePinOut = 2u; // 输出引脚
constexpr uintptr_t kNamespacePinIn = 3u; // 输入引脚

/// 状态默认网格间距（像素）
constexpr float kGridSpacingX = 300.0f;
constexpr float kGridSpacingY = 220.0f;
} // namespace

uintptr_t ASMGraphPanel::EncodeNodeId(size_t entityId, size_t stateIndex) {
    // 状态节点命名空间为 1；实体 id 限 32 bit、状态下标限 16 bit（见 k* 常量注释）
    return (static_cast<uintptr_t>(entityId) << kEntityIdShift)
           | (kNamespaceState << kNamespaceShift)
           | static_cast<uintptr_t>(stateIndex & kStateIndexMask);
}

uintptr_t ASMGraphPanel::EncodePinId(size_t entityId, size_t stateIndex, bool output) {
    // 引脚单独占一个命名空间，PinId 与任何 NodeId 都不相等
    const uintptr_t ns = output ? kNamespacePinOut : kNamespacePinIn;
    return (static_cast<uintptr_t>(entityId) << kEntityIdShift)
           | (ns << kNamespaceShift)
           | static_cast<uintptr_t>(stateIndex & kStateIndexMask);
}

/// ANY 虚拟节点（kNamespaceAny）专用编码；不直接占用状态下标
uintptr_t ASMGraphPanel::EncodeAnyNodeId(size_t entityId) {
    return (static_cast<uintptr_t>(entityId) << kEntityIdShift)
           | (kNamespaceAny << kNamespaceShift);
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

    // 懒创建节点图编辑器上下文（带 SettingsFile：节点坐标自动落盘跨启动保持）。
    // 注意：样式不能在这里配 —— GetStyle() 依赖「当前编辑器」s_Editor，需 SetCurrentEditor
    // 之后才能取到，见 DrawASMGraph 首帧的 SetupStyle。
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

/// 画节点标题行的状态图标：实心圆 = 当前状态，空心圆 = 非当前，内点 = 初始状态。
/// 用 ImGui 自带绘图原语手绘（不引 blueprints 的 utilities，避免增加编译依赖）。
void ASMGraphPanel::DrawStateIcon(const ImVec2 &pos, float size, bool current, bool initial) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float r = size * 0.45f;

    if (current) {
        // 当前状态：绿色实心圆
        dl->AddCircleFilled(c, r, ImColor(60, 220, 120, 255));
        if (initial) {
            dl->AddCircleFilled(c, r * 0.45f, ImColor(240, 160, 60, 255));
        }
    } else {
        // 非当前：空心圆环；初始状态中心补一个橙色点
        dl->AddCircle(c, r, ImColor(150, 160, 175, 255), 24, 2.0f);
        if (initial) {
            dl->AddCircleFilled(c, r * 0.4f, ImColor(240, 160, 60, 255));
        }
    }
    ImGui::Dummy(ImVec2(size, size));
}

/// 画引脚类型图标：输入 = 空心圆环，输出 = 实心圆点（与 blueprints 的 Icon 风格一致）。
void ASMGraphPanel::DrawPinIcon(bool input) {
    const float size = 14.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float r = size * 0.38f;

    if (input) {
        dl->AddCircle(c, r, ImColor(170, 180, 195, 255), 24, 2.0f);
    } else {
        dl->AddCircleFilled(c, r, ImColor(70, 180, 250, 255));
    }
    ImGui::Dummy(ImVec2(size, size));
}

/// 一次性配置节点图编辑器全局样式（深色主题 + 更明显的网格 + 节点/连线风格）。
/// 在 CreateEditor 后调用一次；此后 Style 持续到 DestroyEditor（设置文件不含样式）。
void ASMGraphPanel::SetupStyle() {
    auto &style = ed::GetStyle();
    style = ed::Style(); // 先重置回默认，再覆盖想改的项

    // ---- 画布：深色背景 + 更清晰的网格 ----
    style.Colors[ed::StyleColor_Bg] = ImColor(18, 18, 22, 255);
    style.Colors[ed::StyleColor_Grid] = ImColor(70, 70, 90, 60);

    // ---- 节点：圆角卡片 + 更亮描边 ----
    style.Colors[ed::StyleColor_NodeBg] = ImColor(40, 40, 48, 235);
    style.Colors[ed::StyleColor_NodeBorder] = ImColor(120, 130, 150, 160);
    style.Colors[ed::StyleColor_HovNodeBorder] = ImColor(80, 200, 255, 255);
    style.Colors[ed::StyleColor_SelNodeBorder] = ImColor(255, 190, 70, 255);
    style.Colors[ed::StyleColor_NodeSelRect] = ImColor(30, 90, 180, 80);
    style.Colors[ed::StyleColor_NodeSelRectBorder] = ImColor(60, 140, 255, 150);
    style.Colors[ed::StyleColor_PinRect] = ImColor(70, 140, 200, 120);
    style.Colors[ed::StyleColor_PinRectBorder] = ImColor(90, 170, 230, 160);

    style.NodePadding = ImVec4(12.0f, 8.0f, 12.0f, 8.0f);
    style.NodeRounding = 8.0f;
    style.NodeBorderWidth = 1.5f;
    style.HoveredNodeBorderWidth = 3.0f;
    style.SelectedNodeBorderWidth = 3.5f;
    style.PinRounding = 4.0f;
    style.PinBorderWidth = 0.0f;

    // ---- 连线：更强的弯曲，出/进线方向让竖排节点上下走线更顺 ----
    style.LinkStrength = 140.0f;
    style.SourceDirection = ImVec2(1.0f, 0.0f); // 输出引脚 → 向右出线
    style.TargetDirection = ImVec2(-1.0f, 0.0f); // 输入引脚 → 从左侧进线
    style.HighlightConnectedLinks = 0.0f; // 关掉自动高亮，由我们自己画活跃路径

    // ---- 连线点击/悬停配色 ----
    style.Colors[ed::StyleColor_HovLinkBorder] = ImColor(90, 210, 255, 255);
    style.Colors[ed::StyleColor_SelLinkBorder] = ImColor(255, 190, 70, 255);
}

void ASMGraphPanel::DrawASMGraph(AnimStateMachineComponent &asmc) {
    const auto &states = asmc.states;
    const auto &transitions = asmc.transitions;

    // 画布：ed::Begin 之前不插任何控件，GetContentRegionAvail() 即外层 Child 剩余全部区域
    ed::SetCurrentEditor(m_EditorCtx);

    // 样式首帧应用一次：GetStyle() 依赖 s_Editor（当前编辑器），必须在 SetCurrentEditor 之后。
    // 只应用一次，避免每帧 style = ed::Style() 重置用户手改的样式/布局。
    if (!m_StyleApplied) {
        SetupStyle();
        m_StyleApplied = true;
    }
    const auto &style = ed::GetStyle(); // 节点描边基准值（SetupStyle 设过一次）

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
    const ed::NodeId anyNodeId(EncodeAnyNodeId(entityId));
    if (firstSeenEntity || m_NeedsInitialLayout) {
        ed::SetNodePosition(anyNodeId, ImVec2(20.0f, -60.0f));
    }
    {
        const bool isHovered = ed::GetHoveredNode() == anyNodeId;
        const bool isSelected = ed::IsNodeSelected(anyNodeId);

        // ANY 节点用「全局」配色（蓝），悬停/选中同普通节点逻辑
        ImVec4 anyBorder = ImColor(90, 160, 230, 255);
        float anyBorderWidth = style.NodeBorderWidth;
        if (isHovered || isSelected) {
            anyBorder.x = std::min(anyBorder.x * 1.4f, 1.0f);
            anyBorder.y = std::min(anyBorder.y * 1.4f, 1.0f);
            anyBorder.z = std::min(anyBorder.z * 1.4f, 1.0f);
            anyBorder.w = 1.0f;
            anyBorderWidth = (isHovered ? style.HoveredNodeBorderWidth : style.SelectedNodeBorderWidth);
        }

        ed::PushStyleColor(ed::StyleColor_NodeBorder, anyBorder);
        ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, anyBorderWidth);

        ed::BeginNode(anyNodeId);

        ImGui::BeginGroup(); // 标题行：左图标 + 文字
        {
            ImGui::BeginGroup();
            {
                const float iconSize = 18.0f;
                const ImVec2 iconPos = ImGui::GetCursorScreenPos();
                DrawStateIcon(iconPos, iconSize, false, false);
            }
            ImGui::EndGroup();
            ImGui::SameLine();
            ImGui::BeginGroup();
            {
                ImGui::Text("ANY");
                ImGui::TextDisabled("全局");
            }
            ImGui::EndGroup();
        }
        ImGui::EndGroup();

        ImGui::Spacing();

        // 输出引脚（右侧）：圆点图标 + 文本，作为 from==ANY 转换的源
        ed::BeginPin(ed::PinId(EncodePinId(entityId, kStateIndexMask, true)), ed::PinKind::Output);
        DrawPinIcon(false);
        ImGui::SameLine();
        ImGui::Text("输出");
        ed::EndPin();

        ed::EndNode();

        ed::PopStyleVar();
        ed::PopStyleColor();
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

        // 标题行：状态名 + 初始★/当前▶ 标记；当前状态用醒目颜色描边
        const bool isInitial = (st.name == asmc.initialState);
        const bool isCurrent = (asmc.current == i);
        const bool isHovered = ed::GetHoveredNode() == nodeId;
        const bool isSelected = ed::IsNodeSelected(nodeId);

        // ---- 节点描边：默认灰边；当前状态绿色、初始状态橙色、悬停/选中更亮更粗 ----
        // 注意 PushStyleColor 会覆盖 Hovered/Selected 配色，但 GetHoveredNode() 由库维护，
        // 这里手动补一笔"悬停/选中变亮"（只调亮度，不改色相），避免和默认主题打架。
        ImVec4 nodeBorder = ImColor(120, 130, 150, 160);
        float borderWidth = style.NodeBorderWidth;
        if (isCurrent) {
            nodeBorder = ImColor(60, 220, 120, 255);
        } else if (isInitial) {
            nodeBorder = ImColor(240, 160, 60, 255);
        }
        if (isHovered || isSelected) {
            nodeBorder.x = std::min(nodeBorder.x * 1.4f, 1.0f);
            nodeBorder.y = std::min(nodeBorder.y * 1.4f, 1.0f);
            nodeBorder.z = std::min(nodeBorder.z * 1.4f, 1.0f);
            nodeBorder.w = 1.0f;
            borderWidth = (isHovered ? style.HoveredNodeBorderWidth : style.SelectedNodeBorderWidth);
        }

        ed::PushStyleColor(ed::StyleColor_NodeBorder, nodeBorder);
        ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, borderWidth);

        ed::BeginNode(nodeId);

        ImGui::BeginGroup(); // 标题行：左图标 + 状态名 + 标记
        {
            const float iconSize = 18.0f;
            ImGui::BeginGroup();
            {
                // 左边缘对齐图标：标题行图标 + 每行一个
                const ImVec2 iconPos = ImGui::GetCursorScreenPos();
                DrawStateIcon(iconPos, iconSize, isCurrent, isInitial);
            }
            ImGui::EndGroup();
            ImGui::SameLine();

            ImGui::BeginGroup(); // 右列：标题 + 子信息
            {
                // 标题行：状态名 + 初始★/当前▶ 标记；当前状态用醒目颜色文字
                if (isCurrent) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "▶ ");
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(st.name.empty() ? "(未命名)" : st.name.c_str());
                if (isInitial) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("★");
                }
                if (isCurrent) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("stateTime %.2fs", asmc.stateTime);
                }
            }
            ImGui::EndGroup();
        }
        ImGui::EndGroup();

        ImGui::Spacing();

        // 输入引脚（左侧）：圆环图标 + 文本，作为转换目标
        ed::BeginPin(inPin, ed::PinKind::Input);
        DrawPinIcon(true);
        ImGui::SameLine();
        ImGui::Text("输入");
        ed::EndPin();

        // 输出引脚（右侧）：实心圆点图标 + 文本，作为转换源
        ed::BeginPin(outPin, ed::PinKind::Output);
        DrawPinIcon(false);
        ImGui::SameLine();
        ImGui::Text("输出");
        ed::EndPin();

        ed::EndNode();

        ed::PopStyleVar();
        ed::PopStyleColor();
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
                                       ? ed::PinId(EncodePinId(entityId, kStateIndexMask, true)) // ANY 输出引脚
                                       : ed::PinId(EncodePinId(entityId, tr.from, true)); // 源状态输出引脚
        const ed::PinId endPin = ed::PinId(EncodePinId(entityId, tr.to, false)); // 目标状态输入引脚

        // ---- 连线配色/粗细：ANY 连线蓝色、普通连线绿色；当前状态发出的（活跃路径）更亮更粗 ----
        const bool fromAny = (tr.from == SIZE_MAX);
        const bool isActive = (asmc.current != SIZE_MAX) && (tr.from == asmc.current);
        ImVec4 linkColor = fromAny
                               ? ImVec4(0.30f, 0.60f, 1.00f, 1.00f)
                               : ImVec4(0.45f, 0.80f, 0.45f, 1.00f);
        if (isActive) {
            // 活跃路径：提亮 + 加粗（当前状态 → 目标状态的潜在下一跳）
            linkColor = ImVec4(0.35f, 1.00f, 0.55f, 1.00f);
        }

        const ed::LinkId linkId(static_cast<uintptr_t>(j));
        ed::Link(linkId, startPin, endPin, linkColor, isActive ? 3.5f : 2.0f);

        // ---- 连线标签：过渡时长 + 条件数（两节点中点 ≈ 连线中点，用库前景层画底板+文字）----
        if (tr.blendSec > 0.0f || !tr.conditions.empty()) {
            // 无 GetPinPosition 公开 API，用节点坐标近似连线中点：源节点右侧中点 ↔ 目标节点左侧中点
            const ImVec2 srcPos = ed::GetNodePosition(tr.from == SIZE_MAX
                                                          ? anyNodeId
                                                          : ed::NodeId(EncodeNodeId(entityId, tr.from)));
            const ImVec2 dstPos = ed::GetNodePosition(ed::NodeId(EncodeNodeId(entityId, tr.to)));
            const ImVec2 srcMid = ed::CanvasToScreen(ImVec2(srcPos.x + 80.0f, srcPos.y + 32.0f));
            const ImVec2 dstMid = ed::CanvasToScreen(ImVec2(dstPos.x - 80.0f, dstPos.y + 32.0f));
            const ImVec2 mid = ImVec2((srcMid.x + dstMid.x) * 0.5f, (srcMid.y + dstMid.y) * 0.5f);

            char label[64];
            if (tr.conditions.empty()) {
                snprintf(label, sizeof(label), "%.2fs", tr.blendSec);
            } else {
                snprintf(label, sizeof(label), "%.2fs · %zu cond", tr.blendSec, tr.conditions.size());
            }
            const ImVec2 labelSize = ImGui::CalcTextSize(label);
            ImU32 labelBg = ImColor(20, 22, 28, 220);
            ImU32 labelFg = fromAny
                                ? ImColor(130, 190, 255, 255)
                                : ImColor(160, 220, 160, 255);
            // 注意：不能在这里用 ed::GetHintBackground/ForegroundDrawList —— Hint API 只在
            // BeginGroupHint()…EndGroupHint() 之间有效，那是给 group 节点缩小时画"迷你框"用的，
            // 我们这里没有 group 节点，m_CurrentNode 恒为 nullptr 会 IM_ASSERT 崩溃。
            // 标签几何是我们按节点坐标在屏幕空间手算的，直接用 ImGui 窗口 draw list（即画布
            // draw list）画在链接之上即可。
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(mid.x - labelSize.x * 0.5f - 3.0f, mid.y - labelSize.y * 0.5f - 2.0f),
                              ImVec2(mid.x + labelSize.x * 0.5f + 3.0f, mid.y + labelSize.y * 0.5f + 2.0f),
                              labelBg, 3.0f);
            dl->AddText(ImVec2(mid.x - labelSize.x * 0.5f, mid.y - labelSize.y * 0.5f), labelFg, label);
        }
    }

    // 复位首次布局标记（ANY 节点首帧已摆到角落）
    m_NeedsInitialLayout = false;

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

} // namespace GE
