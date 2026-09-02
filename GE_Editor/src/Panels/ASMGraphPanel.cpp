//
// 动画状态机节点图面板实现。
// 阶段 A：占位窗口 —— 停靠「动画状态机图」窗口，无选中/无 ASM 时占位提示。
// 阶段 B：只读图渲染 —— 把 AnimStateMachineComponent 的 states 画成节点（含虚拟 ANY
//         节点）、transitions 画成连线；当前状态节点实时高亮 + 显示 stateTime。
// 阶段 C：交互编辑 —— 拖拽两状态引脚建转换（ANY 输出 → 目标输入）、Del 删连线/删状态、
//         点选节点/连线后在窗口底部属性区编辑（状态 name/clip/loop/speed/设初始，
//         转换 blendSec + 条件列表）。
// 阶段 4：节点改用 BlueprintNodeBuilder（Header 色条 + 左右列）+ ax::Widgets::Icon
//         引脚图标；删除手绘 DrawStateIcon/DrawPinIcon。
//
// 交互 API 用法遵循 imgui-node-editor blueprints-example 的模式：
//   建转换：ed::BeginCreate → QueryNewLink → 校验输出→输入 → AcceptNewItem/RejectNewItem
//   删除：  ed::BeginDelete → QueryDeletedLink（连线）/ QueryDeletedNode（状态）→
//           AcceptDeletedItem/RejectDeletedItem
// 删除的坑：库删节点会自动把它的关联连线排进同一批删除候选（DeleteDeadLinks），而我们的
// LinkId == transitions 下标；删状态会重排下标导致旧 LinkId 失效，故 HandleDeleteSelection
// 先把所有候选收集成「删状态集合 A + 删连线集合 B」，再统一 ApplyRemovals 重写，避免下标漂移。
//

#include "Panels/ASMGraphPanel.h"

#include "HierarchyLayer.h"
#include "GE/Scene/AnimationComponents.h"
#include "GE/Scene/Scene.h" // Scene::Reg：读同实体 AnimationComponent 的 clips（clip 下拉数据源）
#include "NodeEditorUtils/builders.h" // ax::NodeEditor::Utilities::BlueprintNodeBuilder（阶段 4 节点骨架）

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio> // snprintf：连线标签格式化
#include <cstring> // strncpy_s：条件/状态名输入缓冲
#include <vector>

namespace GE {
namespace ed = ax::NodeEditor;
namespace util = ax::NodeEditor::Utilities;

/// 条件编辑器下拉项（照 SceneHierarchyPanel::DrawAnimStateMachine 同款枚举串）
static const char *kCondTypeItems = "FloatCmp\0Bool\0StateTime\0StateEnded\0";
static const char *kCmpItems = "Greater\0GreaterEq\0Less\0LessEq\0NearEq\0Not\0";

// ---- 节点语义配色（阶段 4 视觉：Header 色条）----
namespace {
/// 把颜色提亮（乘以系数并封顶），用于悬停/选中态叠加
ImVec4 Brighten(const ImVec4 &c, float k = 1.25f) {
    return ImVec4(std::min(c.x * k, 1.0f), std::min(c.y * k, 1.0f),
                  std::min(c.z * k, 1.0f), 1.0f);
}

/// 画一个 18px 状态语义图标（当前=绿实心 / 初始=橙点 / 其它=中灰空环）
void DrawStatusGlyph(ImU32 color, bool filled) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float size = 18.0f;
    const ImVec2 c(a.x + size * 0.5f, a.y + size * 0.5f);
    const float r = size * 0.42f;
    if (filled) {
        dl->AddCircleFilled(c, r, color);
    } else {
        dl->AddCircle(c, r, color, 24, 2.0f);
    }
    ImGui::Dummy(ImVec2(size, size));
}

/// 画状态节点引脚图标（与 ANY 输出同款：18px 空心浅蓝 Flow ▶）。
/// 注意：不能走 ax::Widgets::Icon —— 它在内部先 IsRectVisible(尺寸) 判定，节点处在画布
/// 边缘/被 Canvas 局部裁剪时该判定会误报「不可见」而跳过绘制（节点主体由 node-editor 用
/// 整体包围盒强制绘制仍可见），导致边缘节点的引脚图标消失。这里直接画图标、仅保留占位。
void DrawPinGlyph() {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 size(18.0f, 18.0f);
    ax::Drawing::DrawIcon(ImGui::GetWindowDrawList(), a,
                          ImVec2(a.x + size.x, a.y + size.y),
                          ax::Widgets::IconType::Flow, false,
                          static_cast<ImU32>(ImColor(120, 190, 255)),
                          static_cast<ImU32>(ImColor(120, 190, 255)));
    ImGui::Dummy(size);
}
} // namespace

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

/// 状态默认网格间距（像素）—— 阶段 4 起节点含 Header 色条更高，纵向间距加大避免连线穿节点
constexpr float kGridSpacingX = 320.0f;
constexpr float kGridSpacingY = 260.0f;

/// 解码统一辅助：任意 NodeId/PinId 编码都形如
/// (实体id << 32) | (命名空间 << 16) | 下标，这里只取「命名空间」与「下标」两段
///（实体 id 高位在解码中不参与 —— 删除/选中都发生在同一选中实体上）。

/// 取 16-31 位命名空间（区分 ANY/状态节点/输出/输入引脚）
uintptr_t IdNamespace(uintptr_t encoded) {
    return (encoded >> kNamespaceShift) & 0xFFFFu;
}

/// 取低 16 位下标（状态下标；输出引脚恒为 kNamespacePinOut=2 / 输入 =3，下标段才是状态引用；
/// ANY 输出引脚的下标段存 kStateIndexMask 哨兵）
size_t IdIndex(uintptr_t encoded) {
    return static_cast<size_t>(encoded & kStateIndexMask);
}
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
    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("动画状态机图", nullptr, windowFlags);

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

    // ---- 画布与底部属性区垂直拆分（属性区可拖拽高度）----
    // 属性区用「上一帧选中态」决定是否展开，与 DrawProperties 渲染的 m_SelKind 保持一致 →
    // 同帧内布局不跳变；首次点选会晚一帧展开属性区（选中本身在画布内即帧更新，属性区慢一帧无碍）。
    const bool hasPropArea = (m_SelKind == SelKind::State || m_SelKind == SelKind::Link);

    {
        ImGui::BeginChild("##asmBody", ImVec2(0, 40), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        //topBar
        ImGui::TextUnformatted(selected.GetComponent<TagComponent>().Tag.c_str());
        ImGui::SameLine();
        ImGui::Checkbox("状态机", &asmc.enabled);
        ImGui::SameLine();
        if (ImGui::Button("重新布局")) {
            // 不能在此同步重摆：LayoutNode → ed::SetNodePosition 依赖库静态 s_Editor
            //（当前编辑器），而按钮在 ed::Begin/End 之外执行时 s_Editor 为 nullptr，
            // 会空指针崩溃。改为置位，由下帧 DrawASMGraph 画布内统一执行（见
            // m_RequestRelayout 处理）。仅复位一次：节点坐标全部按声明序网格重摆。
            m_RequestRelayout = true;
        }
        ImGui::EndChild();
    }

    // 画布与属性区分割：先算父剩余可用高。画布高度 = 总高 − 分割条 − 属性区
    //（无属性区则画布吃满）。属性区高度帧初钳到 [min, 可用高−画布最小高−分割条]，
    // 与拖拽上限一致，避免拖到超限后下一帧跳回。
    const float availH = ImGui::GetContentRegionAvail().y;
    const float maxPropH = ImMax(kPropMinHeight, availH - kSplitterH - kCanvasMinH);
    const float propH = hasPropArea ? ImClamp(m_PropHeight, kPropMinHeight, maxPropH) : 0.0f;
    const float canvasSize = hasPropArea
                                 ? ImMax(kCanvasMinH, availH - kSplitterH - propH)
                                 : availH;

    ImGui::BeginChild("##asmCanvas", ImVec2(0, canvasSize), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Button("神秘", ImVec2(0.1, 0.1));
    DrawASMGraph(asmc);
    ImGui::EndChild();

    // 底部属性区 + 其上可拖拽分割条：点选状态/连线时展开（可编辑）。无选中不建，画布独占全高。
    if (hasPropArea) {
        // ---- 分割条：画布 EndChild 后游标已在画布底，此处即条顶（占高 kSplitterH）----
        const ImVec2 splitMin = ImGui::GetCursorScreenPos();
        const float splitWidth = ImGui::GetContentRegionAvail().x;
        const ImVec2 splitMax(splitMin.x + splitWidth, splitMin.y + kSplitterH);

        // 命中/拖动处理（手动实现）：分割条位于属性区顶边，往上拖 = 属性区变高，
        // 故用「基准 − 鼠标位移」（上移 y 减小 → 高度增大）。拖拽基准在按下瞬间记录
        //（m_PropDragStartH/Y），拖动中鼠标移出条带也持续跟随（m_PropDragging），
        // 直到松开。上限给画布留 kCanvasMinH。
        const ImVec2 &mp = ImGui::GetIO().MousePos;
        const bool hover = mp.x >= splitMin.x && mp.x <= splitMax.x &&
                           mp.y >= splitMin.y - 2.0f && mp.y <= splitMax.y + 2.0f;
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_PropDragging) {
            m_PropDragging = true;
            m_PropDragStartH = m_PropHeight;
            m_PropDragStartY = mp.y;
        }
        if (m_PropDragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                m_PropHeight = ImClamp(m_PropDragStartH - (mp.y - m_PropDragStartY),
                                       kPropMinHeight, maxPropH);
            } else {
                m_PropDragging = false;
            }
        }
        if (m_PropDragging || hover)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        // 自绘分割条：按住亮 / 悬停亮 / 平时暗（与 dock 分割条观感一致）
        ImU32 splitCol = m_PropDragging
                             ? ImGui::GetColorU32(ImGuiCol_SeparatorActive)
                             : hover
                             ? ImGui::GetColorU32(ImGuiCol_SeparatorHovered)
                             : ImGui::GetColorU32(ImGuiCol_Separator);
        ImGui::GetWindowDrawList()->AddRectFilled(splitMin, splitMax, splitCol);

        // 占位推进：本行高 kSplitterH（手动控件不自动占位，用 ItemSize 让出）
        ImGui::ItemSize(ImVec2(0.0f, kSplitterH));

        ImGui::BeginChild("##asmProp", ImVec2(0, m_PropHeight), false);
        ImGui::Button("神秘", ImVec2(0.1, 0.1));

        // 同实体 AnimationComponent 的 clip 名列表：状态「片段」下拉数据源。
        // Entity 关联的 Scene 必然非空（能取到组件就说明注册表可用），比 EditorContext::Scene 更可靠。
        Scene *scene = selected.GetScene();
        const AnimationComponent *ac = scene
                                           ? scene->Reg().try_get<AnimationComponent>(
                                               static_cast<entt::entity>(selected))
                                           : nullptr;
        DrawProperties(asmc, ac);
        ImGui::EndChild();
    }

    ImGui::End();

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
    // 节点内容有 Header 色条铺底（纯色），主体底色压到更暗让色条更突出
    style.Colors[ed::StyleColor_NodeBg] = ImColor(36, 36, 44, 235);
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
        // 实体切换：旧实体的状态/连线下标不再有意义，清空属性区展示目标
        //（否则新实体属性区会残留上个实体的旧选中，越界保护虽能兜底但显式清更干净）。
        // 注意只清选中，不动布局：位置由 SettingsFile 按 NodeId 恢复。
        if (firstSeenEntity)
            ResetSelection();
    }

    // 重新布局按钮的导航请求：在 Begin/End 内部触发（需 current editor 已设置）。
    // 注意此块必须位于重摆块之前：重摆块会置位本标志，本块若在置位之后才检查，
    // 会与重摆同帧导航（此时新摆节点尺寸未定，包围盒不准）。隔一帧导航更稳。
    if (m_RequestNavigateContent) {
        m_RequestNavigateContent = false;
        ed::NavigateToContent();
    }

    // 画布内统一处理「重新布局」请求：按钮置位、此处执行（s_Editor 已由 ed::Begin 设好，
    // m_EntityId 已刷新为当前实体）。强制按声明序网格重摆所有状态节点，并把 ANY 节点
    // 摆到固定角落。重摆后置位导航请求 → 下帧（节点尺寸已定）才导航聚焦。
    if (m_RequestRelayout) {
        m_RequestRelayout = false;
        for (size_t i = 0; i < states.size(); ++i) {
            LayoutNode(i);
        }
        ed::SetNodePosition(EncodeAnyNodeId(entityId), ImVec2(20.0f, -60.0f));
        m_RequestNavigateContent = true;
    }

    // ---- 虚拟 ANY 节点：固定画布左上（首次布局 / 重新布局时重置到角落），from==SIZE_MAX 的公共源 ----
    const ed::NodeId anyNodeId(EncodeAnyNodeId(entityId));
    if (firstSeenEntity || m_NeedsInitialLayout) {
        ed::SetNodePosition(anyNodeId, ImVec2(20.0f, -60.0f));
    }
    {
        const bool isHovered = ed::GetHoveredNode() == anyNodeId;
        const bool isSelected = ed::IsNodeSelected(anyNodeId);

        // ANY 节点用「全局」蓝色：Header 蓝、描边蓝；悬停/选中提亮加粗
        ImVec4 anyBorder = ImColor(90, 160, 230, 255);
        float anyBorderWidth = style.NodeBorderWidth;
        if (isHovered || isSelected) {
            anyBorder = Brighten(anyBorder, 1.4f);
            anyBorderWidth = (isHovered ? style.HoveredNodeBorderWidth : style.SelectedNodeBorderWidth);
        }
        ed::PushStyleColor(ed::StyleColor_NodeBorder, anyBorder);
        ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, anyBorderWidth);

        // Header 底色用「全局」蓝，悬停/选中提亮
        ImVec4 anyHeader = ImColor(52, 110, 190, 255);
        if (isHovered || isSelected) {
            anyHeader = Brighten(anyHeader, 1.25f);
        }
        util::BlueprintNodeBuilder builder;
        builder.Begin(anyNodeId);
        {
            builder.Header(anyHeader);
            {
                ImGui::Spring(1);
                DrawStatusGlyph(ImColor(150, 205, 255), true);
                ImGui::Spring(1);
                ImGui::TextUnformatted("ANY");
                ImGui::Spring(1);
                ImGui::TextDisabled("全局");
                ImGui::Spring(1);
            }
            builder.EndHeader();

            builder.Output(ed::PinId(EncodePinId(entityId, kStateIndexMask, true)));
            {
                ImGui::Spring(0);
                ImGui::TextUnformatted("全局");
                ImGui::Spring(0);
                DrawPinGlyph();
                ImGui::Spring(0);
            }
            builder.EndOutput();
        }
        builder.End();

        ed::PopStyleVar();
        ed::PopStyleColor();
    }

    // ---- 状态节点：Builder Header 色条（当前绿/初始橙/普通深灰）+ 输入/输出引脚 ----
    for (size_t i = 0; i < states.size(); ++i) {
        const ed::NodeId nodeId(EncodeNodeId(entityId, i));
        const ed::PinId outPin(EncodePinId(entityId, i, true));
        const ed::PinId inPin(EncodePinId(entityId, i, false));

        // 首次见到该状态（位置在原点）→ 按声明序网格摆放，避免叠在原点。
        // 注意：只有「原点」才触发布局，实体切换后已有坐标的状态保持原位置（计划书 D1）。
        const ImVec2 pos = ed::GetNodePosition(nodeId);
        if (pos.x == 0.0f && pos.y == 0.0f) {
            LayoutNode(i);
        }

        DrawStateNode(asmc, i, nodeId, inPin, outPin);
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
        // 色值沿用旧手绘时代语义，保证既有场景观感一致
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
            // 无 GetPinPosition 公开 API，用节点坐标近似连线中点：源节点右侧中点 ↔ 目标节点左侧中点。
            // 节点含 Header 色条更高，Y 偏移按各节点实际高度取（GetNodeSize 需节点已绘，标签在节点之后画）。
            const ed::NodeId srcNode = (tr.from == SIZE_MAX) ? anyNodeId : ed::NodeId(EncodeNodeId(entityId, tr.from));
            const ed::NodeId dstNode = ed::NodeId(EncodeNodeId(entityId, tr.to));
            const ImVec2 srcPos = ed::GetNodePosition(srcNode);
            const ImVec2 dstPos = ed::GetNodePosition(dstNode);
            const ImVec2 srcSize = ed::GetNodeSize(srcNode);
            const ImVec2 dstSize = ed::GetNodeSize(dstNode);
            const float srcY = (srcSize.y > 0.0f) ? srcPos.y + srcSize.y * 0.5f : srcPos.y + 40.0f;
            const float dstY = (dstSize.y > 0.0f) ? dstPos.y + dstSize.y * 0.5f : dstPos.y + 40.0f;
            const ImVec2 srcMid = ed::CanvasToScreen(ImVec2(srcPos.x + srcSize.x, srcY));
            const ImVec2 dstMid = ed::CanvasToScreen(ImVec2(dstPos.x, dstY));
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

    // ---- 阶段 C 交互：拖拽建转换 / Del 删除 / 选中查询（都需在节点/连线绘制之后的画布内）----
    // 顺序不能错：
    //   1) HandleCreateTransition —— BeginCreate 循环在用户松键拖拽完时给出新连线候选；
    //   2) HandleDeleteSelection —— BeginDelete 处理 Del 键删除（库的删除候选是上帧/本帧选中）。
    //      必须先删（改动 transitions/states）再查询选中，否则删完再查会拿到刚删的悬空选中；
    //   3) QuerySelection —— 读当前选中节点/连线，缓存到成员供 End 之后属性区渲染。
    HandleCreateTransition(asmc);
    HandleDeleteSelection(asmc);
    QuerySelection(asmc);

    // 复位首次布局标记（ANY 节点首帧已摆到角落）
    m_NeedsInitialLayout = false;

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

/// 画单个状态节点（BlueprintNodeBuilder：Header 色条 + 左输入/右输出引脚 Icon）。
/// 当前状态=绿、初始状态=橙、普通=中性深灰 Header；悬停/选中提亮加粗描边。
/// 引脚：输入(被进入)=灰空心圆环在左、输出(离开)=蓝实心圆点在右。
void ASMGraphPanel::DrawStateNode(AnimStateMachineComponent &asmc, size_t stateIndex,
                                  const ed::NodeId &nodeId, const ed::PinId &inPin,
                                  const ed::PinId &outPin) {
    const AnimStateDef &st = asmc.states[stateIndex];
    const bool isInitial = (st.name == asmc.initialState);
    const bool isCurrent = (asmc.current == stateIndex);
    const bool isHovered = ed::GetHoveredNode() == nodeId;
    const bool isSelected = ed::IsNodeSelected(nodeId);
    const bool isLit = isHovered || isSelected;

    // ---- Header 语义配色：当前=绿 / 初始=橙 / 普通=中性深灰；悬停/选中提亮 ----
    ImVec4 headerCol;
    if (isCurrent)
        headerCol = ImColor(50, 175, 100, 255);
    else if (isInitial)
        headerCol = ImColor(215, 150, 50, 255);
    else
        headerCol = ImColor(72, 76, 92, 255);
    if (isLit)
        headerCol = Brighten(headerCol, 1.25f);

    // ---- 节点描边：跟随 Header 色系（普通态半透明、语义态加实）----
    const auto &style = ed::GetStyle();
    ImVec4 borderCol = headerCol;
    borderCol.w = (isCurrent || isInitial) ? 0.95f : 0.75f;
    if (isLit)
        borderCol = Brighten(borderCol, 1.2f);
    const float borderWidth = isHovered
                                  ? style.HoveredNodeBorderWidth
                                  : isSelected
                                  ? style.SelectedNodeBorderWidth
                                  : style.NodeBorderWidth;

    ed::PushStyleColor(ed::StyleColor_NodeBorder, borderCol);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, borderWidth);

    util::BlueprintNodeBuilder builder;
    builder.Begin(nodeId);
    {
        // ---- Header 行：状态图标 + ▶/★ 标记 + 名称 +（当前）stateTime ----
        builder.Header(headerCol);
        {
            ImGui::Spring(1);
            if (isCurrent)
                DrawStatusGlyph(ImColor(235, 255, 242, 255), true);
            else if (isInitial)
                DrawStatusGlyph(ImColor(255, 225, 160, 255), false);
            else
                DrawStatusGlyph(ImColor(255, 255, 255, 110), false);
            ImGui::Spring(1);

            ImGui::PushStyleColor(ImGuiCol_Text, ImU32(ImColor(255, 255, 255, 235)));
            if (isCurrent) {
                ImGui::Text("▶ ");
                ImGui::Spring(0);
            }
            ImGui::TextUnformatted(st.name.empty() ? "(未命名)" : st.name.c_str());
            if (isInitial) {
                ImGui::Spring(0);
                ImGui::Text("★");
            }
            if (isCurrent) {
                ImGui::Spring(1);
                ImGui::Text("stateTime %.2fs", asmc.stateTime);
            }
            ImGui::PopStyleColor();
            ImGui::Spring(1);
        }
        builder.EndHeader();

        // ---- 内容行：左输入引脚（目标）/ 右输出引脚（源）----
        builder.Input(inPin);
        {
            ImGui::Spring(0);
            DrawPinGlyph();
            ImGui::Spring(0);
            ImGui::TextDisabled("输入");
        }
        builder.EndInput();

        builder.Output(outPin);
        {
            ImGui::TextDisabled("输出");
            ImGui::Spring(0);
            DrawPinGlyph();
            ImGui::Spring(0);
        }
        builder.EndOutput();
    }
    builder.End();

    ed::PopStyleVar();
    ed::PopStyleColor();
}

// ============================================================
// 阶段 C：交互编辑
// ============================================================

/// 拖拽建转换（C1）：BeginCreate → QueryNewLink → 校验 → AcceptNewItem。
/// 在画布内、所有节点/连线绘制之后调用（库的拖拽交互发生在绘制期）。
void ASMGraphPanel::HandleCreateTransition(AnimStateMachineComponent &asmc) {
    if (!ed::BeginCreate(ImColor(255, 255, 255), 2.0f)) {
        ed::EndCreate();
        return;
    }

    ed::PinId startPinId = 0, endPinId = 0;
    if (ed::QueryNewLink(&startPinId, &endPinId)) {
        // 命名空间解码：输出引脚 ns=2、输入引脚 ns=3；拖拽方向不保证 start=输出，
        // 交换使 start=输出引脚（源）、end=输入引脚（目标），与官方示例语义一致。
        const uintptr_t startVal = startPinId.Get();
        const uintptr_t endVal = endPinId.Get();
        uintptr_t outVal = startVal, inVal = endVal;
        if (IdNamespace(startVal) == kNamespacePinIn && IdNamespace(endVal) == kNamespacePinOut)
            std::swap(outVal, inVal);

        // 各校验不通过就 Reject（拖拽仍可继续到别的目标引脚；库在 End 前都保持候选）
        bool ok = true;
        size_t from = SIZE_MAX; // 解析出的源状态；ANY 输出引脚的下标段存 kStateIndexMask 哨兵
        size_t to = SIZE_MAX;

        const bool startIsOut = (IdNamespace(outVal) == kNamespacePinOut);
        const bool endIsIn = (IdNamespace(inVal) == kNamespacePinIn);
        if (!startIsOut || !endIsIn) {
            ok = false; // 同向引脚（输出→输出 / 输入→输入）
        } else {
            const size_t outIdx = IdIndex(outVal);
            if (outIdx != kStateIndexMask) {
                // 非 ANY 源 → 必须是界内状态输出
                if (outIdx >= asmc.states.size())
                    ok = false; // 越界源引脚（旧场景残留）
                else
                    from = outIdx;
            }

            to = IdIndex(inVal);
            if (to >= asmc.states.size())
                ok = false; // 越界目标

            if (ok && from != SIZE_MAX && from == to)
                ok = false; // 自环（源==目标，不含 ANY）无意义

            // 已存在同 (from,to) 转换 → 拒绝，避免图上重叠连线
            if (ok) {
                const bool dup = std::any_of(asmc.transitions.begin(), asmc.transitions.end(),
                                             [from, to](const AnimTransitionDef &tr) {
                                                 return tr.from == from && tr.to == to;
                                             });
                if (dup)
                    ok = false;
            }
        }

        if (!ok) {
            ed::RejectNewItem(ImColor(255, 0, 0), 2.0f);
        } else if (ed::AcceptNewItem(ImColor(128, 255, 128), 4.0f)) {
            AnimTransitionDef t;
            t.from = from;
            t.to = to;
            t.blendSec = 0.25f; // 默认过渡时长，与列表面板「添加转换」默认一致
            asmc.transitions.push_back(t);
        }
    }

    ed::EndCreate();
}

/// Del 删除（C2）：收集被删状态集合 A 与被删连线集合 B，统一 ApplyRemovals 重写。
/// 库的删除候选分两阶段给出：QueryDeletedNode 先逐个收状态节点，Accept 一个状态节点后
/// 库自动把它的关联连线追加进候选（DeleteDeadLinks），随后 QueryDeletedLink 才轮到连线。
/// 故先收完 A、再收完 B，两阶段内都不改数组，最后统一重写 —— 避免删连线（B 用删除前下标）
/// 与删状态（A 会重排下标）交错时 LinkId 失效。
void ASMGraphPanel::HandleDeleteSelection(AnimStateMachineComponent &asmc) {
    if (!ed::BeginDelete())
        return;

    std::vector<size_t> statesToErase;
    std::vector<size_t> linksToErase;

    // 阶段 1：状态节点。ANY 虚拟节点不可删（无对应 states 数据），Reject。
    {
        ed::NodeId nodeId = 0;
        while (ed::QueryDeletedNode(&nodeId)) {
            const uintptr_t val = nodeId.Get();
            if (IdNamespace(val) == kNamespaceAny) {
                ed::RejectDeletedItem();
                continue;
            }
            const size_t idx = IdIndex(val);
            if (idx >= asmc.states.size()) {
                // 越界（不该发生）→ 拒绝保平安
                ed::RejectDeletedItem();
                continue;
            }
            statesToErase.push_back(idx);
            ed::AcceptDeletedItem();
        }
    }

    // 阶段 2：连线。此时状态尚未删除、下标未重排，B 用的是删除前下标，可靠。
    {
        ed::LinkId linkId = 0;
        while (ed::QueryDeletedLink(&linkId)) {
            const size_t idx = IdIndex(linkId.Get());
            if (idx < asmc.transitions.size()) // 仅收集仍在界的；越界/悬空忽略（已随节点删除而删）
                linksToErase.push_back(idx);
            ed::AcceptDeletedItem(); // 库内部清理照做（勿 Reject 否则残留内部状态）
        }
    }

    ed::EndDelete();

    // 统一去重升序（Query 顺序无保证），再一次性重写
    std::sort(statesToErase.begin(), statesToErase.end());
    statesToErase.erase(std::unique(statesToErase.begin(), statesToErase.end()),
                        statesToErase.end());
    std::sort(linksToErase.begin(), linksToErase.end());
    linksToErase.erase(std::unique(linksToErase.begin(), linksToErase.end()),
                       linksToErase.end());

    if (!statesToErase.empty() || !linksToErase.empty())
        ApplyRemovals(asmc, statesToErase, linksToErase);
}

/// 批量删除状态 + 连线并重编号（见头文件注释；A/B 须已升序去重）。
void ASMGraphPanel::ApplyRemovals(AnimStateMachineComponent &asmc,
                                  const std::vector<size_t> &statesToErase,
                                  const std::vector<size_t> &linksToErase) {
    const auto stateErased = [&statesToErase](size_t idx) {
        return std::binary_search(statesToErase.begin(), statesToErase.end(), idx);
    };
    const auto linkErased = [&linksToErase](size_t idx) {
        return std::binary_search(linksToErase.begin(), linksToErase.end(), idx);
    };
    // 状态下标重映射：old → new（被删 → SIZE_MAX）
    auto remapState = [&statesToErase](size_t oldIdx) -> size_t {
        size_t newIdx = oldIdx;
        for (size_t removed : statesToErase) {
            if (removed < oldIdx)
                --newIdx;
            else if (removed == oldIdx)
                return SIZE_MAX; // 该状态被删
        }
        return newIdx;
    };

    // 1) 过滤 states
    std::vector<AnimStateDef> newStates;
    newStates.reserve(asmc.states.size() - statesToErase.size());
    for (size_t i = 0; i < asmc.states.size(); ++i) {
        if (!stateErased(i))
            newStates.push_back(std::move(asmc.states[i]));
    }

    // 2) current 修正（列表面板语义：删了当前 → 复位；被删之前的 current 左移）
    if (asmc.current != SIZE_MAX) {
        if (stateErased(asmc.current)) {
            asmc.current = SIZE_MAX; // 下帧求值回初始状态
        } else {
            asmc.current = remapState(asmc.current);
        }
    }

    // 3) 过滤 + 重编号 transitions：删掉下标在 B 的，以及 from/to 引用 A 状态的
    std::vector<AnimTransitionDef> newTransitions;
    newTransitions.reserve(asmc.transitions.size());
    for (size_t j = 0; j < asmc.transitions.size(); ++j) {
        const AnimTransitionDef &tr = asmc.transitions[j];
        if (linkErased(j))
            continue;
        if (stateErased(tr.to))
            continue; // 目标状态被删
        if (tr.from != SIZE_MAX && stateErased(tr.from))
            continue; // 源状态被删（ANY 源永不删）

        AnimTransitionDef nt = tr;
        if (nt.from != SIZE_MAX)
            nt.from = remapState(nt.from);
        nt.to = remapState(nt.to);
        newTransitions.push_back(std::move(nt));
    }

    asmc.states = std::move(newStates);
    asmc.transitions = std::move(newTransitions);
    // states.size() 已变 → 下帧 DrawASMGraph 开头的 m_LastStateCount 比较自然触发
    // m_NeedsInitialLayout，对新下标补一次布局（阶段 B 既有机制，无需在此重复置位）。
}

/// 画布内查询当前选中（Begin 内调用）。本函数把「画布即时选中」映射为底部属性区的
/// 「展示目标」（m_SelKind/m_SelState/m_SelLink）。仅当出现明确的单选状态 / 单选连线时
/// 才切换展示目标；点空白、点 ANY、框选多选、选中越界对象 → 一律保留上一帧展示目标，
/// 使属性窗口不随这些操作关闭。实体切换由调用方在 firstSeenEntity 时 ResetSelection。
void ASMGraphPanel::QuerySelection(AnimStateMachineComponent &asmc) {
    // 本帧画布真实选中类型（局部；不直接写成员，避免点空白/ANY/多选清掉展示目标）
    SelKind curKind = SelKind::None;
    size_t curState = SIZE_MAX;
    size_t curLink = SIZE_MAX;

    // 缓冲大小取库当前选中对象数（可能同时含节点+连线）
    const int cap = ed::GetSelectedObjectCount();
    if (cap > 0) {
        std::vector<ed::NodeId> nodes(static_cast<size_t>(cap));
        std::vector<ed::LinkId> links(static_cast<size_t>(cap));
        const int nodeCount = ed::GetSelectedNodes(nodes.data(), cap);
        const int linkCount = ed::GetSelectedLinks(links.data(), cap);

        // 记录选中的普通状态节点下标（ANY 节点不可编辑，忽略 → 视为 None 保留旧展示）
        std::vector<size_t> stateSel;
        for (int i = 0; i < nodeCount; ++i) {
            const uintptr_t val = nodes[i].Get();
            if (IdNamespace(val) != kNamespaceState)
                continue;
            const size_t idx = IdIndex(val);
            if (idx < asmc.states.size())
                stateSel.push_back(idx);
        }

        if (stateSel.empty() && linkCount == 0) {
            curKind = SelKind::None; // 无选中 / 只选中 ANY 等不可编辑对象
        } else if (stateSel.size() == 1 && linkCount == 0) {
            curKind = SelKind::State;
            curState = stateSel[0];
        } else if (stateSel.empty() && linkCount == 1) {
            const size_t idx = IdIndex(links[0].Get());
            if (idx < asmc.transitions.size()) {
                curKind = SelKind::Link;
                curLink = idx;
            }
            // 越界连线 → curKind 保持 None
        } else {
            curKind = SelKind::Mixed; // 多选 / 节点连线混选
        }
    }

    // 写入展示目标：
    //   State/Link —— 明确的单选，切换展示目标；
    //   Mixed（框选多选）—— 清掉旧单选展示，属性区显示「多选不可编辑」提示；
    //   None（点空白 / 点 ANY / 选到已删对象）—— 不写成员，保留上一帧展示目标，
    //   使属性窗口不随这些操作关闭。
    if (curKind == SelKind::State) {
        m_SelKind = SelKind::State;
        m_SelState = curState;
    } else if (curKind == SelKind::Link) {
        m_SelKind = SelKind::Link;
        m_SelLink = curLink;
    } else if (curKind == SelKind::Mixed) {
        m_SelKind = SelKind::Mixed;
    }
    // else (None)：保留旧展示目标
}

/// 清空属性区展示目标（实体切换时调用；旧实体的状态下标/连线下标不再有意义）
void ASMGraphPanel::ResetSelection() {
    m_SelKind = SelKind::None;
    m_SelState = SIZE_MAX;
    m_SelLink = SIZE_MAX;
}

/// 底部属性区（C3）：按选中类型渲染状态或转换的属性编辑；未选中/多选只提示。
void ASMGraphPanel::DrawProperties(AnimStateMachineComponent &asmc, const AnimationComponent *ac) {
    ImGui::Separator();
    if (asmc.states.empty())
        return;

    switch (m_SelKind) {
    case SelKind::State: {
        if (m_SelState >= asmc.states.size()) {
            m_SelKind = SelKind::None;
            ImGui::TextDisabled("（选中状态已删除）");
            return;
        }
        AnimStateDef &st = asmc.states[m_SelState];

        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.35f, 1.0f), "状态属性");
        ImGui::SameLine();
        ImGui::TextDisabled("#%zu", m_SelState);

        // 名称
        char nameBuf[128] = {};
        strncpy_s(nameBuf, sizeof(nameBuf), st.name.c_str(), _TRUNCATE);
        if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf))) {
            st.name = nameBuf;
        }
        // 设初始状态（若名空则跳过 —— initialState 需非空才可作引用）
        ImGui::SameLine();
        if (ImGui::Button("设为初始状态") && !st.name.empty()) {
            asmc.initialState = st.name;
        }

        // clip 下拉：读同实体 AnimationComponent::clips（列表面板同款）
        if (ac && !ac->clips.empty()) {
            const std::string clipPreview = st.clipName.empty() ? "(选片段)" : st.clipName;
            if (ImGui::BeginCombo("片段", clipPreview.c_str())) {
                for (const auto &inst : ac->clips) {
                    if (!inst.clip)
                        continue;
                    const bool sel = (inst.clip->name == st.clipName);
                    if (ImGui::Selectable(inst.clip->name.c_str(), sel))
                        st.clipName = inst.clip->name;
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("无动画片段");
        }

        ImGui::Checkbox("循环", &st.loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::DragFloat("速度", &st.speed, 0.05f, 0.0f, 10.0f, "%.2f");
        ImGui::SameLine();
        ImGui::TextDisabled("当前状态: %s", asmc.current == m_SelState ? "是" : "否");

        // 删除该状态（含重编号关联转换）。等价的画布操作是点选节点按 Del。
        if (ImGui::Button("删除该状态")) {
            ApplyRemovals(asmc, {m_SelState}, {});
            m_SelKind = SelKind::None;
            m_SelState = SIZE_MAX;
            return; // st 引用已被 ApplyRemovals 换表 → 悬空，立即返回
        }
    }
    break;

    case SelKind::Link: {
        if (m_SelLink >= asmc.transitions.size()) {
            m_SelKind = SelKind::None;
            ImGui::TextDisabled("（选中连线已删除）");
            return;
        }
        AnimTransitionDef &tr = asmc.transitions[m_SelLink];

        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "转换属性");
        ImGui::SameLine();
        // from → to 只读预览（ANY → 名字）
        const char *fromName = (tr.from == SIZE_MAX)
                                   ? "ANY"
                                   : (tr.from < asmc.states.size() ? asmc.states[tr.from].name.c_str() : "(无效)");
        const char *toName = (tr.to < asmc.states.size()) ? asmc.states[tr.to].name.c_str() : "(无效)";
        ImGui::TextDisabled("%s → %s", fromName, toName);

        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("过渡时长(s)", &tr.blendSec, 0.01f, 0.0f, 10.0f, "%.2f");

        ImGui::SameLine();
        if (ImGui::Button("删除该转换")) {
            ApplyRemovals(asmc, {}, {m_SelLink});
            m_SelKind = SelKind::None;
            m_SelLink = SIZE_MAX;
            return; // tr 引用已被 ApplyRemovals 换表 → 悬空，立即返回
        }

        // ---- 条件列表（AND 全满足才触发；字段照列表面板）----
        ImGui::Text("条件 (AND)");
        ImGui::Separator();
        int removeCond = -1;
        for (int ci = 0; ci < static_cast<int>(tr.conditions.size()); ++ci) {
            AnimCondition &cond = tr.conditions[ci];
            ImGui::PushID(ci);
            int typeIdx = static_cast<int>(cond.type);
            if (ImGui::Combo("类型", &typeIdx, kCondTypeItems)) {
                cond.type = static_cast<AnimCondition::Type>(typeIdx);
            }
            ImGui::SameLine();
            if (ImGui::Button("删除"))
                removeCond = ci;

            switch (cond.type) {
            case AnimCondition::Type::FloatCmp: {
                char pbuf[64] = {};
                strncpy_s(pbuf, sizeof(pbuf), cond.param.c_str(), _TRUNCATE);
                if (ImGui::InputText("参数", pbuf, sizeof(pbuf)))
                    cond.param = pbuf;
                int cmpIdx = static_cast<int>(cond.cmp);
                if (ImGui::Combo("比较", &cmpIdx, kCmpItems))
                    cond.cmp = static_cast<AnimCondition::Cmp>(cmpIdx);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("阈值", &cond.value, 0.01f, -100.0f, 100.0f, "%.2f");
            }
            break;
            case AnimCondition::Type::Bool: {
                char pbuf[64] = {};
                strncpy_s(pbuf, sizeof(pbuf), cond.param.c_str(), _TRUNCATE);
                if (ImGui::InputText("参数", pbuf, sizeof(pbuf)))
                    cond.param = pbuf;
                ImGui::SameLine();
                const char *expectPreview = cond.expect ? "true" : "false";
                if (ImGui::BeginCombo("期望", expectPreview)) {
                    if (ImGui::Selectable("true", cond.expect))
                        cond.expect = true;
                    if (ImGui::Selectable("false", !cond.expect))
                        cond.expect = false;
                    ImGui::EndCombo();
                }
            }
            break;
            case AnimCondition::Type::StateTime: ImGui::SetNextItemWidth(110.0f);
                ImGui::DragFloat("驻留(秒)", &cond.value, 0.01f, 0.0f, 100.0f, "%.2f");
                break;
            case AnimCondition::Type::StateEnded: ImGui::TextDisabled("当前动画播放到末尾后离开（需非循环）");
                break;
            }
            ImGui::PopID();
        }
        if (removeCond >= 0)
            tr.conditions.erase(tr.conditions.begin() + removeCond);
        if (ImGui::Button("+ 添加条件"))
            tr.conditions.push_back(AnimCondition{});
    }
    break;

    case SelKind::Mixed: ImGui::TextDisabled("多选不可编辑，单选节点或连线后此处编辑属性");
        break;

    case SelKind::None:
    default: ImGui::TextDisabled("点选状态节点或转换连线后，在此处编辑属性");
        break;
    }
}

} // namespace GE
