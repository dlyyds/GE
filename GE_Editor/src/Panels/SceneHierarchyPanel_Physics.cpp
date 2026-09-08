//
// 场景层级面板 —— 物理/碰撞类组件属性绘制。
//
// 本文件为 SceneHierarchyPanel 拆分的一部分，包含：RigidBody（刚体）、
// CharacterController（角色控制器）、Box/Sphere/Capsule Collider、
// BoundingBox（实体级粗剔除盒，含 AnimLocalMatrix / AutoFitBoundingBoxToJoints）。
// 其余域见 SceneHierarchyPanel.cpp 顶部注释。
//

#include "SceneHierarchyPanel.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>

#include "GE/Scene/Components.h"
#include "GE/Animation/AnimationSystem.h"
#include "GE/Scene/Scene.h"
#include "GE/Physics/PhysicsWorld.h"
#include "GE/Render/Camera.h"
#include "GE/Render/Material.h"
#include "GE/Render/MaterialManager.h"
#include "GE/Render/TextureManager.h"
#include "GE/Render/AssetManager.h"
#include "GE/Render/Renderer.h"
#include "GE/Render/Renderer3D.h"
#include "GE/Render/Mesh.h"
#include "GE/Render/MeshManager.h"
#include "GE/Utils/PlatformUtils.h"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <functional>
#include <unordered_set>

#include "SceneHierarchyPanelInternal.h"

namespace GE {
// ============================================================
// Rigid Body 组件
// ============================================================
void SceneHierarchyPanel::DrawRigidBodyComponent(Entity entity, RigidBodyComponent &component) {
    Physics::PhysicsWorld *physicsWorld = m_Context->GetPhysicsWorld();
    const auto entityHandle = (entt::entity)entity;

    // 刚体类型下拉框（类型改变需要重建 body）
    {
        const char *typeStrings[] = {"静态", "运动学", "动态"};
        int currentType = static_cast<int>(component.Type);
        if (ImGui::BeginCombo("类型", typeStrings[currentType])) {
            for (int i = 0; i < 3; i++) {
                const bool isSelected = currentType == i;
                if (ImGui::Selectable(typeStrings[i], isSelected)) {
                    component.Type = static_cast<Physics::RigidBodyType>(i);
                    // 运动类型改变 → 销毁并重建刚体
                    if (physicsWorld)
                        physicsWorld->RebuildRigidBody(entityHandle);
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // 质量（仅动态体有效，改变后需要更新属性）
    bool massDisabled = (component.Type != Physics::RigidBodyType::Dynamic);
    if (massDisabled) ImGui::BeginDisabled();
    if (ImGui::DragFloat("质量（kg）", &component.Mass, 0.1f, 0.01f, 10000.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }
    if (massDisabled) ImGui::EndDisabled();

    // 摩擦系数
    if (ImGui::DragFloat("摩擦力", &component.Friction, 0.01f, 0.0f, 1.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    // 弹性系数
    if (ImGui::DragFloat("弹性恢复", &component.Restitution, 0.01f, 0.0f, 1.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    // 线性阻尼
    if (ImGui::DragFloat("线性阻尼", &component.LinearDamping, 0.01f, 0.0f, 10.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    // 角阻尼
    if (ImGui::DragFloat("角阻尼", &component.AngularDamping, 0.01f, 0.0f, 10.0f)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    // 传感器标记（改变后需要更新属性）
    if (ImGui::Checkbox("传感器（触发器）", &component.IsSensor)) {
        if (physicsWorld)
            physicsWorld->UpdateRigidBodyProperties(entityHandle);
    }

    ImGui::Separator();
    ImGui::TextDisabled("运行时刚体 ID: %s", component.IsInitialized ? "有效" : "未初始化");
}

// ============================================================
// Character Controller（角色控制器）组件
// ============================================================
void SceneHierarchyPanel::DrawCharacterControllerComponent(
    Entity entity, CharacterControllerComponent &component) {
    Physics::PhysicsWorld *physicsWorld = m_Context->GetPhysicsWorld();
    const auto entityHandle = (entt::entity)entity;

    ImGui::Separator();
    ImGui::TextDisabled("运行时: %s%s",
                        component.IsInitialized ? "运行中" : "未初始化",
                        component.IsInitialized
                            ? (component.IsGrounded ? " / 在地面" : " / 空中")
                            : "");

    // 胶囊轴向（模型实际"上"不在局部 Y 时选 X/Z；改变胶囊朝向 → 重建角色）
    {
        const char *axisStrings[] = {"Y", "X", "Z"};
        const int currentAxis = static_cast<int>(component.Axis);
        if (ImGui::BeginCombo("胶囊轴向", axisStrings[currentAxis])) {
            for (int i = 0; i < 3; i++) {
                const bool isSelected = currentAxis == i;
                if (ImGui::Selectable(axisStrings[i], isSelected)) {
                    component.Axis = static_cast<CapsuleAxis>(i);
                    if (physicsWorld)
                        physicsWorld->RebuildCharacter(entityHandle);
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // 模型前向基准轴（面朝方向对齐哪个局部轴；退化时引擎自动回退 Y/X，实时生效无需重建）
    {
        const char *frontStrings[] = {"Y", "X", "Z"};
        const int currentFront = static_cast<int>(component.FrontAxis);
        if (ImGui::BeginCombo("前向基准轴", frontStrings[currentFront])) {
            for (int i = 0; i < 3; i++) {
                const bool isSelected = currentFront == i;
                if (ImGui::Selectable(frontStrings[i], isSelected))
                    component.FrontAxis = static_cast<CapsuleAxis>(i);
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    // 前向轴取反：模型"脸"在 FrontAxis 反方向时勾选（如角色初始旋转后正面朝 -Z）
    ImGui::Checkbox("反转前向", &component.InvertFront);

    // 半径/总高/最大坡度改变胶囊形状 → 销毁重建角色（取当前 Transform 作初始位置）
    if (ImGui::DragFloat("半径", &component.Radius, 0.05f, 0.001f, 100.0f)) {
        if (physicsWorld)
            physicsWorld->RebuildCharacter(entityHandle);
    }
    if (ImGui::DragFloat("高度", &component.Height, 0.05f, 0.01f, 100.0f)) {
        if (physicsWorld)
            physicsWorld->RebuildCharacter(entityHandle);
    }
    if (DrawVec3Control("偏移", component.Offset, 0.0f, 120)) {
        if (physicsWorld)
            physicsWorld->RebuildCharacter(entityHandle);
    }
    if (ImGui::DragFloat("最大坡度（度）", &component.MaxSlopeAngle, 0.5f, 0.0f, 89.0f)) {
        if (physicsWorld)
            physicsWorld->RebuildCharacter(entityHandle);
    }

    // 跳跃初速 / 重力缩放不改形状：无需重建（引擎每子步实时读组件值）
    ImGui::DragFloat("最大跳跃初速", &component.MaxJumpSpeed, 0.1f, 0.0f, 100.0f);
    ImGui::DragFloat("重力缩放", &component.GravityScale, 0.05f, 0.0f, 20.0f);

    // 朝向移动：每子步实时读组件值，无需重建
    ImGui::Checkbox("面向移动方向", &component.FaceMovement);
    if (component.FaceMovement) {
        ImGui::DragFloat("转向速度（度/秒）", &component.TurnSpeed, 10.0f, 1.0f, 1080.0f);
    }

    // ---- 根位移（纯配置，影响 Scene::UpdateRootMotion 功能开关，不触发 RebuildCharacter）----
    ImGui::Separator();
    ImGui::TextDisabled("根位移");
    ImGui::Checkbox("启用根位移", &component.UseRootMotion);
    if (component.UseRootMotion) {
        ImGui::InputInt("根骨骼节点索引", &component.RootBoneNodeIndex, 1, 1);
        ImGui::TextDisabled("填模型根骨骼的 glTF nodeIndex（-1 = 未配置）");

        // 方便填写：从角色实体的 active clip 里列出带 Translation 通道的骨骼。
        // 根位移通常就在这些骨骼当中；选中后直接写回 RootBoneNodeIndex，不再需要手动查 nodeIndex。
        if (entity.HasComponent<AnimationComponent>()) {
            auto &ac = entity.GetComponent<AnimationComponent>();
            struct BoneCandidate { int nodeIndex = -1; std::string label; };
            std::vector<BoneCandidate> candidates;
            std::unordered_set<int> seen;
            if (!ac.clips.empty() && ac.active < ac.clips.size()) {
                const auto &inst = ac.clips[ac.active];
                const auto *clip = inst.clip.get();
                if (clip) {
                    auto &reg = m_Context->Reg();
                    for (size_t ci = 0; ci < clip->channels.size(); ++ci) {
                        const auto &ch = clip->channels[ci];
                        if (ch.nodeIndex < 0 || ch.path != AnimationChannel::Path::Translation)
                            continue;
                        if (!seen.insert(ch.nodeIndex).second)
                            continue;
                        std::string boneName;
                        if (ci < inst.channelTargets.size()) {
                            if (auto *tag = reg.try_get<TagComponent>(inst.channelTargets[ci]))
                                boneName = tag->Tag;
                        }
                        candidates.push_back({ch.nodeIndex,
                                              std::to_string(ch.nodeIndex) + " - " + boneName});
                    }
                }
            }

            if (!candidates.empty()) {
                const int current = component.RootBoneNodeIndex;
                auto curIt = std::find_if(candidates.begin(), candidates.end(),
                                          [current](const BoneCandidate &c) { return c.nodeIndex == current; });
                const char *preview = (curIt != candidates.end()) ? curIt->label.c_str() : "选择根骨骼...（或手填上方数字）";
                if (ImGui::BeginCombo("根骨骼（从动画选择）", preview)) {
                    for (const auto &c : candidates) {
                        const bool selected = (c.nodeIndex == current);
                        if (ImGui::Selectable(c.label.c_str(), selected))
                            component.RootBoneNodeIndex = c.nodeIndex;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("角色实体没有可选的根骨骼 Translation 通道，请手填 glTF nodeIndex");
            }
        }
        ImGui::Checkbox("根骨骼局部归零", &component.ZeroRootBoneLocal);
        ImGui::TextDisabled("就地化：把根骨骼局部 Translation 归 base，烘焙动画防双倍移动");
        ImGui::TextDisabled("根骨骼局部每帧归 base；角色位移仍由原来的键盘输入驱动（不再提取动画根位移）");
    }
}

// ============================================================
// Box Collider 组件
// ============================================================
void SceneHierarchyPanel::DrawBoxColliderComponent(Entity entity, BoxColliderComponent &component) {
    Physics::PhysicsWorld *physicsWorld = m_Context->GetPhysicsWorld();
    const auto entityHandle = (entt::entity)entity;

    // 碰撞体调试线框单独开关（仅影响视口叠加显示，不涉及物理）
    ImGui::Checkbox("显示调试线框", &component.DrawDebug);
    ImGui::Separator();

    // 形状改变 → 重建刚体
    if (DrawVec3Control("半尺寸", component.HalfExtents, 0.5f, 120)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
    if (DrawVec3Control("偏移", component.Offset, 0.0f, 120)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
}

// ============================================================
// Sphere Collider 组件
// ============================================================
void SceneHierarchyPanel::DrawSphereColliderComponent(Entity entity, SphereColliderComponent &component) {
    Physics::PhysicsWorld *physicsWorld = m_Context->GetPhysicsWorld();
    const auto entityHandle = (entt::entity)entity;

    // 碰撞体调试线框单独开关（仅影响视口叠加显示，不涉及物理）
    ImGui::Checkbox("显示调试线框", &component.DrawDebug);
    ImGui::Separator();

    // 形状改变 → 重建刚体
    if (ImGui::DragFloat("半径", &component.Radius, 0.05f, 0.001f, 1000.0f)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
    if (DrawVec3Control("偏移", component.Offset, 0.0f, 120)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
}

// ============================================================
// Capsule Collider 组件
// ============================================================
void SceneHierarchyPanel::DrawCapsuleColliderComponent(Entity entity, CapsuleColliderComponent &component) {
    Physics::PhysicsWorld *physicsWorld = m_Context->GetPhysicsWorld();
    const auto entityHandle = (entt::entity)entity;

    // 碰撞体调试线框单独开关（仅影响视口叠加显示，不涉及物理）
    ImGui::Checkbox("显示调试线框", &component.DrawDebug);
    ImGui::Separator();

    // 胶囊轴向（实体带旋转时让胶囊贴住模型实际朝向；改变需要重建刚体）
    {
        const char *axisStrings[] = {"Y", "X", "Z"};
        const int currentAxis = static_cast<int>(component.Axis);
        if (ImGui::BeginCombo("胶囊轴向", axisStrings[currentAxis])) {
            for (int i = 0; i < 3; i++) {
                const bool isSelected = currentAxis == i;
                if (ImGui::Selectable(axisStrings[i], isSelected)) {
                    component.Axis = static_cast<CapsuleAxis>(i);
                    if (physicsWorld)
                        physicsWorld->RebuildRigidBody(entityHandle);
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // 形状改变 → 重建刚体
    if (ImGui::DragFloat("半径", &component.Radius, 0.05f, 0.001f, 1000.0f)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
    if (ImGui::DragFloat("半高", &component.HalfHeight, 0.05f, 0.0f, 1000.0f)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
    if (DrawVec3Control("偏移", component.Offset, 0.0f, 120)) {
        if (physicsWorld)
            physicsWorld->RebuildRigidBody(entityHandle);
    }
}

// ============================================================
// Bounding Box 组件（实体级粗剔除盒，gizmo 手动摆放）
// ============================================================

/// 动画评估用局部 TRS（只拷作者字段，不碰缓存 worldMatrix）
struct AnimLocalTrs {
    glm::vec3 Translation;
    glm::quat Rotation;
    glm::vec3 Scale;
};

static glm::mat4 AnimLocalMatrix(const AnimLocalTrs &trs) {
    return glm::translate(glm::mat4(1.0f), trs.Translation)
           * glm::toMat4(trs.Rotation)
           * glm::scale(glm::mat4(1.0f), trs.Scale);
}

void SceneHierarchyPanel::DrawBoundingBoxComponent(Entity entity, BoundingBoxComponent &component, Scene *scene) {
    // Center/Size 为模型局部空间；Size 任一轴重置为 0 会令盒失效（不参与剔除）
    DrawVec3Control("中心", component.Center, 0.0f, 120);
    DrawVec3Control("尺寸", component.Size, 1.0f, 120);
    if (!component.IsValid()) {
        ImGui::TextDisabled("盒未摆放（Size 需全部 > 0）时不参与剔除");
    }
    if (ImGui::Button("从关节自动适配")) {
        if (!AutoFitBoundingBoxToJoints(scene, entity, component)) {
            GE_CORE_WARN("该实体子树内没有蒙皮关节，无法自动适配包围盒");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("按子树内全部蒙皮关节计算并写回；检测到动画时逐帧播放取最大范围");
}

bool SceneHierarchyPanel::AutoFitBoundingBoxToJoints(Scene *scene, Entity entity, BoundingBoxComponent &bb) {
    if (!scene || !entity) {
        return false;
    }
    entt::registry &reg = scene->Reg();
    const auto *tc = reg.try_get<TransformComponent>(static_cast<entt::entity>(entity));
    if (!tc) {
        return false;
    }
    // 关节是世界实体，先经逆世界矩阵变换到本实体局部空间，再取本地 AABB。
    // 这样写回的 Center/Size 经世界矩阵还原（视锥剔除用的 world 盒）后必能罩住全部关节。
    const glm::mat4 invWorld = glm::inverse(tc->GetWorldMatrix());

    // ── 1) DFS 子树：收集蒙皮关节 + 动画组件 ──
    std::vector<entt::entity> joints;
    std::vector<AnimationComponent *> anims;
    std::vector<Entity> stack;
    stack.push_back(entity);
    while (!stack.empty()) {
        const Entity n = stack.back();
        stack.pop_back();
        const entt::entity h = static_cast<entt::entity>(n);
        if (const auto *sc = reg.try_get<SkinComponent>(h)) {
            joints.insert(joints.end(), sc->joints().begin(), sc->joints().end());
        }
        if (auto *ac = reg.try_get<AnimationComponent>(h)) {
            anims.push_back(ac); // 有动画则后续按帧扫关节极值
        }
        for (const auto &child : scene->GetChildren(n)) {
            stack.push_back(child); // 继续下钻同棵子树
        }
    }
    if (joints.empty()) {
        return false; // 子树内无蒙皮关节，无从适配
    }

    glm::vec3 localMin(0.0f), localMax(0.0f);
    bool found = false;
    // invW 逐帧变化：当前姿态用 invWorld，动画扫描用「该帧盒子自身世界矩阵的逆」，
    // 这样即使盒子随动画移动/旋转，度量尺度始终与盒子本地空间一致。
    const auto expandLocal = [&](const glm::mat4 &invW, const glm::vec3 &wpos) {
        const glm::vec3 jp = glm::vec3(invW * glm::vec4(wpos, 1.0f));
        if (!found) {
            localMin = localMax = jp;
            found = true;
        } else {
            localMin = glm::min(localMin, jp);
            localMax = glm::max(localMax, jp);
        }
    };

    // ── 2) 当前姿态打底（无动画时仅此一步） ──
    for (const entt::entity jh : joints) {
        const auto *jtc = reg.try_get<TransformComponent>(jh);
        if (jtc) {
            expandLocal(invWorld, glm::vec3(jtc->GetWorldMatrix()[3]));
        }
    }
    if (!found) {
        return false;
    }

    // ── 3) 动画扫描：把子树内的每个 clip 从头到尾播一遍，各帧关节极值并入最大盒 ──
    // 关节世界位置只受「关节到根祖先链」上实体局部 TRS 影响，先收集整条链
    std::unordered_set<entt::entity> chainSet;
    for (const entt::entity jh : joints) {
        entt::entity cur = jh;
        while (cur != entt::null) {
            chainSet.insert(cur);
            const auto *curTc = reg.try_get<TransformComponent>(cur);
            cur = curTc ? curTc->parent : entt::null;
        }
    }

    for (const AnimationComponent *ac : anims) {
        for (const ClipInstance &inst : ac->clips) {
            const auto &clip = inst.clip;
            if (!clip || clip->channels.empty()) {
                continue;
            }
            const auto &targets = inst.channelTargets;

            // 采样时间轴单调递增 → 每 channel 独立缓存键帧下界（阶段 A：扫帧整体受益最大）。
            // 每个 clip 从头独立扫，故用本地数组、不复用 ClipInstance 的运行时缓存。
            std::vector<uint32_t> keyHints(clip->channels.size(), 0u);

            // 采样时刻：均匀网格（限流，含首尾）+ 全部键帧时刻
            // （键帧时刻覆盖 STEP 跳变与旋转弧极值不在网格上的情形）
            std::vector<float> times;
            const float duration = clip->duration;
            if (duration > 0.0f) {
                const float step = std::max(1.0f / 30.0f, duration / 1200.0f);
                for (float t = 0.0f; t < duration; t += step) {
                    times.push_back(t);
                }
            }
            times.push_back(duration);
            for (const auto &ch : clip->channels) {
                times.insert(times.end(), ch.times.begin(), ch.times.end());
            }
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());

            for (const float t : times) {
                // 局部 TRS：默认取场景当前值，被本 clip 指定 path 驱动的分量用采样覆盖
                // （同一目标只改写驱动分量，其余分量保持绑定姿态）
                std::unordered_map<entt::entity, AnimLocalTrs> localTrs;
                for (const entt::entity c : chainSet) {
                    const auto *cTc = reg.try_get<TransformComponent>(c);
                    if (cTc) {
                        localTrs[c] = AnimLocalTrs{cTc->Translation, cTc->Rotation, cTc->Scale};
                    }
                }
                for (size_t ci = 0; ci < clip->channels.size(); ++ci) {
                    const AnimationChannel &ch = clip->channels[ci];
                    const entt::entity target = (ci < targets.size()) ? targets[ci] : entt::null;
                    if (target == entt::null || !chainSet.count(target)) {
                        continue; // 空洞目标、或不在本骨架链上（不影响这些关节），跳过
                    }
                    auto &lt = localTrs[target];
                    switch (ch.path) {
                    case AnimationChannel::Path::Translation:
                        lt.Translation = AnimationSystem::SampleVec3Channel(ch, t, keyHints[ci]);
                        break;
                    case AnimationChannel::Path::Rotation:
                        lt.Rotation = AnimationSystem::SampleQuatChannel(ch, t, keyHints[ci]);
                        break;
                    case AnimationChannel::Path::Scale:
                        lt.Scale = AnimationSystem::SampleVec3Channel(ch, t, keyHints[ci]);
                        break;
                    }
                }

                // 自顶向下累乘世界矩阵（链已含根，父先于子），滚到每个关节取世界位置。
                // 度量尺度用该帧盒子自身的逆世界矩阵：盒子随动画位移/旋转时仍得本地空间极值。
                std::unordered_map<entt::entity, glm::mat4> worldMat;
                std::function<glm::mat4(entt::entity)> calcWorld = [&](entt::entity e) -> glm::mat4 {
                    auto it = worldMat.find(e);
                    if (it != worldMat.end()) {
                        return it->second;
                    }
                    const auto ltIt = localTrs.find(e);
                    glm::mat4 m = (ltIt != localTrs.end()) ? AnimLocalMatrix(ltIt->second) : glm::mat4(1.0f);
                    const auto *eTc = reg.try_get<TransformComponent>(e);
                    if (eTc && eTc->parent != entt::null) {
                        m = calcWorld(eTc->parent) * m;
                    }
                    worldMat[e] = m;
                    return m;
                };
                const entt::entity boxH = static_cast<entt::entity>(entity);
                const glm::mat4 invWorldT = glm::inverse(calcWorld(boxH));
                for (const entt::entity jh : joints) {
                    expandLocal(invWorldT, glm::vec3(calcWorld(jh)[3]));
                }
            }
        }
    }

    bb.Center = (localMin + localMax) * 0.5f;
    bb.Size = localMax - localMin;
    return true;
}

} // namespace GE
