//
// Gizmo 控制器实现 —— 在场景视口上渲染 ImGuizmo 变换 gizmo 并写回 Transform。
//

#include "GizmoController.h"

#include "HierarchyLayer.h"
#include "GE/Scene/Components.h"
#include "GE/Render/Mesh.h"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace GE {

GizmoController::GizmoController(std::shared_ptr<EditorContext> context, HierarchyLayer *hierarchy)
    : m_Context(std::move(context)), m_Hierarchy(hierarchy) {
}

void GizmoController::Render(const Camera &camera, const glm::vec2 &viewportPos, const glm::vec2 &viewportSize) {
    if (!m_Context || !m_Hierarchy) {
        return;
    }

    // 无选中实体、或选中实体没有 Transform 组件时，不绘制 gizmo
    Entity selected = m_Hierarchy->GetSelectedEntity();
    if (!selected || !selected.HasComponent<TransformComponent>()) {
        return;
    }

    ImGuizmo::BeginFrame();
    // 关键：BeginFrame 内部新建的全屏 NoInputs "gizmo" 窗口不会被当成悬停窗口，
    // 若不重设 draw list，则 IsHoveringWindow() 命中失败导致 gizmo 拖不动。
    // 这里把 draw list 绑定到当前 Scene 窗口（本函数正是在其 Begin/End 内被调用），
    // 使 _OwnerName 为 "Scene"，鼠标悬停命中与拖拽正常。
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());

    // ---- 快捷键切换操作模式（W/E/R）----
    // Ctrl 按下时跳过（避免与 Ctrl+R 全局重载脚本冲突）
    const bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_W)) m_Operation = ImGuizmo::TRANSLATE;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_E)) m_Operation = ImGuizmo::ROTATE;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_R)) m_Operation = ImGuizmo::SCALE;

    // 'B' 切换包围盒编辑模式（仅对带 BoundingBoxComponent 的实体有效，见下方分支）
    if (ImGui::IsKeyPressed(ImGuiKey_B)) {
        m_EditingBounds = !m_EditingBounds;
    }

    // ---- 视口左上角工具条：切换模式 + 吸附开关 ----
    ImGui::SetCursorScreenPos(ImVec2(viewportPos.x + 10.0f, viewportPos.y + 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    const ImVec4 activeColor{0.2f, 0.6f, 0.85f, 1.0f};
    const ImVec4 idleColor{0.25f, 0.25f, 0.28f, 1.0f};

    ImGui::PushStyleColor(ImGuiCol_Button, m_Operation == ImGuizmo::TRANSLATE ? activeColor : idleColor);
    if (ImGui::Button("Move")) m_Operation = ImGuizmo::TRANSLATE;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, m_Operation == ImGuizmo::ROTATE ? activeColor : idleColor);
    if (ImGui::Button("Rotate")) m_Operation = ImGuizmo::ROTATE;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, m_Operation == ImGuizmo::SCALE ? activeColor : idleColor);
    if (ImGui::Button("Scale")) m_Operation = ImGuizmo::SCALE;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // 包围盒编辑开关：选中实体无 BoundingBoxComponent 时置灰
    ImGui::PushStyleColor(ImGuiCol_Button, m_EditingBounds ? activeColor : idleColor);
    const bool canEditBounds = selected.HasComponent<BoundingBoxComponent>();
    if (!canEditBounds) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Box")) {
        m_EditingBounds = !m_EditingBounds;
    }
    if (!canEditBounds) {
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::Checkbox("Snap", &m_UseSnap);
    ImGui::PopStyleVar(2);

    // ---- 包围盒编辑模式：进入后隐藏变换 gizmo，改拖盒角/边 ----
    if (m_EditingBounds && selected.HasComponent<BoundingBoxComponent>()) {
        EditBounds(selected, camera, viewportPos, viewportSize);
        return;
    }

    // ---- 取出选中实体的世界矩阵给 ImGuizmo 显示 ----
    // ImGuizmo 按视口世界坐标渲染手柄，层级化后局部矩阵 ≠ 世界矩阵：
    // 若把局部矩阵直接传入，子实体的手柄会与实体实际位置/朝向不重合。
    // 故传入世界矩阵；写回时再乘父世界矩阵的逆，还原为局部 TRS（见下）。
    auto &transformComp = selected.GetComponent<TransformComponent>();
    glm::mat4 transform = transformComp.GetWorldMatrix();

    // 父实体世界矩阵（根实体为恒等）：用于把 gizmo 输出的世界结果换算回局部 TRS
    glm::mat4 parentWorld = glm::mat4(1.0f);
    if (m_Context->Scene) {
        if (Entity parent = m_Context->Scene->GetParent(selected)) {
            if (parent.HasComponent<TransformComponent>())
                parentWorld = parent.GetComponent<TransformComponent>().GetWorldMatrix();
        }
    }

    // ---- 手柄矩阵：默认在实体局部原点；有网格时定位到网格世界空间包围盒中心 ----
    // 模型原点往往不在几何正中间（DCC 导出锚点、组合模型），手柄应出现在物体
    // 正中间而非原点。做法：gizmo 传入「平移=包围盒中心、旋转/缩放=原世界矩阵」
    // 的手柄矩阵，ImGuizmo 绕此中心旋转/缩放（物体原点不变、几何跟随）；
    // 平移操作产生中心位移（delta），该位移叠加回物体原平移，见写回处。
    glm::mat4 gizmoMatrix = transform;
    if (selected.HasComponent<MeshRendererComponent>()) {
        Mesh *mesh = selected.GetComponent<MeshRendererComponent>().MeshPtr;
        if (mesh) {
            const AABB &aabb = mesh->GetAABB();
            if (aabb.IsValid()) {
                const AABB worldAabb = aabb.Transformed(transform);
                const glm::vec3 center = (worldAabb.min + worldAabb.max) * 0.5f;
                gizmoMatrix = glm::translate(glm::mat4(1.0f), center) * glm::mat4(glm::mat3(transform));
            }
        }
    }
    const glm::vec3 gizmoCenterBefore = glm::vec3(gizmoMatrix[3]);

    ImGuizmo::SetRect(viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y);

    // ---- 吸附参数（按操作类型取对应步长）----
    float snapValues[3] = {m_SnapTranslation, m_SnapRotation, m_SnapScale};
    float *snap = m_UseSnap ? snapValues : nullptr;

    // ImGuizmo 按 OpenGL 惯例解析投影（NDC Y 向上，见 ImGuizmo.cpp worldToPos 的
    // trans.y = 1.f - trans.y）。引擎渲染用的 proj 已做 Vulkan Y 翻转（Camera::GetProj
    // 的 proj[1][1] *= -1），直接传入会让 gizmo 垂直镜像、拖拽跟随错位。
    // 故这里把 [1][1] 再翻转一次，还原成 OpenGL 投影再交给 ImGuizmo，使其与渲染对齐。
    glm::mat4 proj = camera.GetProj();
    proj[1][1] *= -1.0f;

    bool changed = ImGuizmo::Manipulate(
        glm::value_ptr(camera.GetView()),
        glm::value_ptr(proj),
        m_Operation, m_Mode,
        glm::value_ptr(gizmoMatrix),
        nullptr, // deltaMatrix：无需输出增量矩阵
        snap);   // snap：吸附步长（第 7 参）

    if (changed && ImGuizmo::IsUsing()) {
        // gizmo 更新的是「手柄矩阵」：返回矩阵平移位于手柄位置（新中心）。
        // 旋转/缩放绕中心时中心不变（delta=0，物体原点不动、几何绕中心旋转/缩放）；
        // 平移操作才产生中心位移（delta≠0）。把中心位移叠加到物体原平移（原点），
        // 使几何随手柄移动。
        const glm::vec3 centerDelta = glm::vec3(gizmoMatrix[3]) - gizmoCenterBefore;

        // 新世界矩阵 = gizmo 结果的旋转/缩放 + 「原平移 + 中心位移」。
        // 无网格回退时中心即原点，centerDelta 即完整平移增量，结果与原逻辑一致。
        glm::mat4 worldMatrix = glm::mat4(glm::mat3(gizmoMatrix));
        worldMatrix[3] = glm::vec4(glm::vec3(transform[3]) + centerDelta, 1.0f);

        // gizmo 输出的是世界矩阵，先乘父世界矩阵的逆还原为局部矩阵，再拆回 平移/旋转(度)/缩放。
        // （根实体 parentWorld 为恒等，换算无影响；子实体则把父级位移/旋转/缩放一并剥离）
        glm::mat4 localMatrix = glm::inverse(parentWorld) * worldMatrix;

        // 从 gizmo 更新后的局部矩阵拆回 平移/旋转(度)/缩放 写回组件
        float translation[3], rotationDeg[3], scale[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(localMatrix), translation, rotationDeg, scale);
        transformComp.Translation = {translation[0], translation[1], translation[2]};
        // ImGuizmo 拆出欧拉角（角度），在此编辑器边界转成四元数写回
        transformComp.SetRotationEuler(glm::radians(glm::vec3(rotationDeg[0], rotationDeg[1], rotationDeg[2])));
        transformComp.Scale = {scale[0], scale[1], scale[2]};
    }
}

void GizmoController::EditBounds(Entity entity, const Camera &camera,
                                 const glm::vec2 &viewportPos, const glm::vec2 &viewportSize) {
    auto &bb = entity.GetComponent<BoundingBoxComponent>();
    auto &transformComp = entity.GetComponent<TransformComponent>();
    if (!bb.IsValid()) {
        // 未摆放（默认 0 尺寸）：先给一个可拖拽的单位盒，用户拖一下即完成摆放
        bb.Center = glm::vec3(0.0f);
        bb.Size = glm::vec3(1.0f);
    }

    const glm::mat4 transform = transformComp.GetWorldMatrix();

    // 手柄矩阵：平移 = 盒的世界中心，旋转 = 实体世界朝向（列归一化剥离世界缩放）。
    // 盒在模型局部空间必须保持轴对齐，拖拽帧取「模型轴」而非世界轴；localBounds 传
    // 相对盒中心的 ±size/2，拖拽后该帧矩阵的列模长 = 每轴缩放系数、平移 = 新中心。
    glm::mat3 rotN = glm::mat3(transform);
    for (int c = 0; c < 3; ++c) {
        const float l = glm::length(rotN[c]);
        if (l > 1e-6f) {
            rotN[c] /= l;
        }
    }
    const glm::vec3 worldCenter = glm::vec3(transform * glm::vec4(bb.Center, 1.0f));
    glm::mat4 gizmoMatrix = glm::translate(glm::mat4(1.0f), worldCenter) * glm::mat4(rotN);

    const glm::vec3 halfSize = bb.Size * 0.5f;
    const float localBounds[6] = {-halfSize.x, -halfSize.y, -halfSize.z,
                                  halfSize.x,  halfSize.y,  halfSize.z};

    ImGuizmo::SetRect(viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y);
    glm::mat4 proj = camera.GetProj();
    proj[1][1] *= -1.0f; // Vulkan→OpenGL 投影还原（与变换 gizmo 同一套）

    const bool changed = ImGuizmo::Manipulate(
        glm::value_ptr(camera.GetView()),
        glm::value_ptr(proj),
        ImGuizmo::SCALE, ImGuizmo::LOCAL,
        glm::value_ptr(gizmoMatrix),
        nullptr,             // deltaMatrix
        nullptr,             // snap
        localBounds,         // localBounds：盒手柄（角/边拖拽），原帧三列模长均为 1
        nullptr);            // boundsSnap

    if (changed && ImGuizmo::IsUsing()) {
        // 拖拽首帧定格起始尺寸/中心：ImGuizmo 盒手柄比例是相对拖拽起点的绝对值，
        // 若每帧拿最新 bb.Size 去乘会逐帧重复叠加（盒按 比例^帧数 指数膨胀，灵敏度失控）。
        // 之后每帧一律由「起始值 × 阻尼比例」推出，保证静止不漂、拖多远放多大。
        if (!m_BoundsDragActive) {
            m_BoundsDragActive = true;
            m_BoundsStartSize = bb.Size;
            m_BoundsStartCenterWorld = worldCenter; // worldCenter 为本帧拖拽前的盒中心
        }

        // 原始缩放比例：帧矩阵各列模长即每轴缩放系数（相对拖拽起始盒）
        glm::vec3 rawScale;
        const glm::mat3 m3 = glm::mat3(gizmoMatrix);
        for (int c = 0; c < 3; ++c) {
            rawScale[c] = glm::length(m3[c]);
        }

        // 灵敏度阻尼：比例向 1 收缩、中心位移按同系数缩减 —— 两者系数一致才能保证
        // 仍围绕同一支点缩放（中心随动不过冲）；系数越小越不灵敏，0.5 即拖到 2 倍
        // 需要的鼠标位移约为原来的 2 倍。
        constexpr float kBoundsDragDamping = 0.5f;
        const glm::vec3 dampedScale = 1.0f + kBoundsDragDamping * (rawScale - 1.0f);
        const glm::vec3 newCenterWorld = m_BoundsStartCenterWorld
            + kBoundsDragDamping * (glm::vec3(gizmoMatrix[3]) - m_BoundsStartCenterWorld);

        // 新尺寸 = 起始尺寸 × 阻尼比例
        glm::vec3 newSize = m_BoundsStartSize * dampedScale;

        // 新世界中心还原回模型局部中心
        const glm::vec3 newCenterLocal = glm::inverse(rotN) * (newCenterWorld - glm::vec3(transform[3]));

        // 防止缩成退化/反向盒：任一轴过小会让 IsValid()==false、剔除失效，钳制下限
        constexpr float kMinSize = 0.05f;
        newSize = glm::max(newSize, glm::vec3(kMinSize));
        bb.Center = newCenterLocal;
        bb.Size = newSize;
    } else {
        m_BoundsDragActive = false; // 未在拖拽：重置标志，下次拖拽重新定格起始态
    }
}

} // namespace GE