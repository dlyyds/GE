#include "pch.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Entity.h"
#include "Physics/PhysicsWorld.h"
#include "Events/Event.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"
#include "Render/Renderer.h"
#include "Render/Renderer2D.h"
#include "Render/Renderer3D.h"
#include "Render/Material.h"
#include "Render/Frustum.h"
#include "Render/Mesh.h"
#include "Render/EnvironmentMap.h"
#include "Render/VulkanBase/VulkanDevice.h"
#include "Render/VulkanBase/VulkanRenderFrame.h"
#include "Core/Log.h"
#include "Render/AssetManager.h"

#include <algorithm>
#include <unordered_set>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace GE {


Scene::Scene() {
    // 创建物理世界
    m_PhysicsWorld = std::make_unique<Physics::PhysicsWorld>(this);

    // 注册 RigidBodyComponent 销毁回调
    m_Registry.on_destroy<RigidBodyComponent>().connect<&Scene::OnRigidBodyDestroyed>(this);

    // 注册碰撞体销毁回调（移除碰撞体时触发刚体重建）
    m_Registry.on_destroy<BoxColliderComponent>().connect<&Scene::OnColliderDestroyed>(this);
    m_Registry.on_destroy<SphereColliderComponent>().connect<&Scene::OnColliderDestroyed>(this);
    m_Registry.on_destroy<CapsuleColliderComponent>().connect<&Scene::OnColliderDestroyed>(this);

    // 注册角色控制器销毁回调（销毁 Jolt CharacterVirtual）
    m_Registry.on_destroy<CharacterControllerComponent>().connect<&Scene::OnCharacterControllerDestroyed>(this);

    // Lua 脚本引擎：绑定场景 + 注入 API（脚本基准目录 assets/scripts/）
    m_ScriptEngine.Init(this, "assets/scripts");

    // 注册脚本组件销毁回调（清理 Lua 实例 + 调 OnDestroy）
    m_Registry.on_destroy<ScriptComponent>().connect<&Scene::OnScriptComponentDestroyed>(this);
}

Scene::~Scene() {
    // 提前解挂脚本销毁回调并清理，避免注册表析构时 on_destroy 回调碰卸了一半的 Lua/场景
    m_Registry.on_destroy<ScriptComponent>().disconnect<&Scene::OnScriptComponentDestroyed>(this);
    m_ScriptEngine.Shutdown();
}

Entity Scene::CreateEntity(const std::string &name) {
    Entity entity{m_Registry.create(), this};

    entity.AddComponent<TransformComponent>();
    entity.AddComponent<TagComponent>(name);
    entity.AddComponent<IDComponent>(GenerateUUID());

    return entity;
}

void Scene::DestroyEntity(Entity entity) {
    const entt::entity handle = static_cast<entt::entity>(entity);
    if (!m_Registry.valid(handle))
        return;

    // 先从父实体的后代列表中脱离（保留组件排查语义：父仍存在，删除只影响子树）
    if (auto *tc = m_Registry.try_get<TransformComponent>(handle)) {
        if (tc->parent != entt::null) {
            if (auto it = m_ChildrenOf.find(tc->parent); it != m_ChildrenOf.end()) {
                auto &vec = it->second;
                vec.erase(std::remove(vec.begin(), vec.end(), handle), vec.end());
                if (vec.empty())
                    m_ChildrenOf.erase(it);
            }
            tc->parent = entt::null;
        }
    }

    // 级联销毁后代（沿反向索引递归）；子实体的 DestroyEntity 会继续处理其自身子树
    if (auto it = m_ChildrenOf.find(handle); it != m_ChildrenOf.end()) {
        std::vector<entt::entity> children = std::move(it->second);
        m_ChildrenOf.erase(it);
        for (entt::entity child : children) {
            if (m_Registry.valid(child))
                DestroyEntity(Entity(child, this));
        }
    }

    // 实体销毁（触发各组件 on_destroy 清理，如 RigidBodyComponent 的物理 body）
    m_Registry.destroy(handle);
}

bool Scene::SetParent(Entity child, Entity parent) {
    const entt::entity childHandle = static_cast<entt::entity>(child);
    const entt::entity parentHandle = parent ? static_cast<entt::entity>(parent) : entt::null;

    if (!m_Registry.valid(childHandle))
        return false;
    if (parentHandle != entt::null) {
        if (!m_Registry.valid(parentHandle))
            return false;
        // 父实体必须带 Transform：DFS 依赖 parent 链持有 Transform 下钻
        if (!m_Registry.all_of<TransformComponent>(parentHandle))
            return false;
        // 不能设自己为父
        if (parentHandle == childHandle)
            return false;
        // 环检测：若 parent 位于 child 的子树上，则沿 parent 的父链上溯必然能到达 child，
        // 新边 child→parent 会构成环、导致每帧 DFS 无限递归，拒绝。
        for (entt::entity cur = parentHandle; cur != entt::null && m_Registry.valid(cur);) {
            if (cur == childHandle)
                return false;
            auto *curTc = m_Registry.try_get<TransformComponent>(cur);
            cur = curTc ? curTc->parent : entt::null;
        }
    }

    auto *childTc = m_Registry.try_get<TransformComponent>(childHandle);
    if (!childTc)
        return false;

    // 与旧父脱离（同步组件指针 + 反向索引两处）
    if (childTc->parent != entt::null) {
        if (auto it = m_ChildrenOf.find(childTc->parent); it != m_ChildrenOf.end()) {
            auto &vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), childHandle), vec.end());
            if (vec.empty())
                m_ChildrenOf.erase(it);
        }
    }

    // 挂到新父
    childTc->parent = parentHandle;
    if (parentHandle != entt::null)
        m_ChildrenOf[parentHandle].push_back(childHandle);

    return true;
}

Entity Scene::GetParent(Entity entity) {
    const entt::entity handle = static_cast<entt::entity>(entity);
    if (!m_Registry.valid(handle))
        return {};
    auto *tc = m_Registry.try_get<TransformComponent>(handle);
    if (!tc || tc->parent == entt::null)
        return {};
    return Entity(tc->parent, this);
}

std::vector<Entity> Scene::GetChildren(Entity entity) {
    std::vector<Entity> result;
    const entt::entity handle = static_cast<entt::entity>(entity);
    if (auto it = m_ChildrenOf.find(handle); it != m_ChildrenOf.end()) {
        result.reserve(it->second.size());
        for (entt::entity child : it->second)
            result.emplace_back(child, this);
    }
    return result;
}

void Scene::RebuildChildrenIndex() {
    m_ChildrenOf.clear();
    auto view = m_Registry.view<TransformComponent>();
    for (auto entity : view) {
        const auto &tc = view.get<TransformComponent>(entity);
        if (tc.parent != entt::null && m_Registry.valid(tc.parent))
            m_ChildrenOf[tc.parent].push_back(entity);
    }
}

void Scene::ClearAllEntities() {
    // 物理运行态是瞬态：清实体即回 Edit（Deserialize 加载后恒为编辑态，决策 5.5）。
    // 残留 pending / 快照一并清掉，避免下次 Play 用错位/失效句柄建体。
    m_SimulationState = SimulationState::Edit;
    m_PlaySnapshot.clear();
    if (m_PhysicsWorld) {
        m_PhysicsWorld->ClearPendingBodies();
        m_PhysicsWorld->ClearPendingCharacters();
        m_PhysicsWorld->ResetAccumulator();
    }
    m_ChildrenOf.clear();
    m_Registry.clear();
}

Entity Scene::FindEntityByID(const std::string &uuid) {
    if (uuid.empty())
        return {};
    auto view = m_Registry.view<IDComponent>();
    for (auto entity : view) {
        const auto &idc = view.get<IDComponent>(entity);
        if (idc.UUID == uuid)
            return Entity(entity, this);
    }
    return {};
}

Entity Scene::GetPrimaryCameraEntity() {
    // 优先返回标注为主相机（Primary=true）的实体
    auto view = m_Registry.view<CameraComponent>();
    for (auto entityHandle : view) {
        const auto &cc = view.get<CameraComponent>(entityHandle);
        if (cc.Primary) {
            return Entity(entityHandle, this);
        }
    }

    // 没有主相机，则返回第一个带 CameraComponent 的实体
    for (auto entityHandle : view) {
        return Entity(entityHandle, this);
    }

    // 场景中没有相机
    return {};
}

bool Scene::GetCharacterFacingYaw(entt::entity entity, float &outYaw) const {
    const auto *cc = m_Registry.try_get<CharacterControllerComponent>(entity);
    if (!cc) {
        return false;
    }
    outYaw = cc->FacingYaw;
    return true;
}

void Scene::Play() {
    // 已 Playing：重复进入无操作（幂等）
    if (m_SimulationState == SimulationState::Playing)
        return;
    if (!m_PhysicsWorld)
        return;

    // 1. 备份 Transform 快照：带刚体/角色控制器的实体都存（Stop 回滚到此姿态）。
    //    静态体位置不随模拟变，存了无害（决策 5.7）。
    m_PlaySnapshot.clear();
    auto rbView = m_Registry.view<TransformComponent, RigidBodyComponent>();
    for (auto entity : rbView) {
        const auto &tc = rbView.get<TransformComponent>(entity);
        m_PlaySnapshot.push_back({entity, tc.Translation, tc.Rotation});
    }
    auto ccView = m_Registry.view<TransformComponent, CharacterControllerComponent>();
    for (auto entity : ccView) {
        const auto &tc = ccView.get<TransformComponent>(entity);
        m_PlaySnapshot.push_back({entity, tc.Translation, tc.Rotation});
    }

    // 2. flush：补请求"有刚体/角色但未初始化、也从未入队"的实体，首帧 Step 即建体。
    //    RequestCreate* 内部去重：pending 已有者自然无操作。
    for (auto entity : rbView) {
        const auto &rbc = rbView.get<RigidBodyComponent>(entity);
        if (!rbc.IsInitialized)
            m_PhysicsWorld->RequestCreateRigidBody(entity);
    }
    for (auto entity : ccView) {
        const auto &ccc = ccView.get<CharacterControllerComponent>(entity);
        if (!ccc.IsInitialized)
            m_PhysicsWorld->RequestCreateCharacter(entity);
    }

    // 3. 防抖：清空时间累加器，避免上次运行残留的累积时间在首帧连跑多个子步（"按 Play 抖一下"）。
    //    body 首帧由 ProcessPendingBodies 从实体当前 Transform 创建，初始位与摆放天然对齐，
    //    无需再在 UI 线程补同步（SyncBodiesToTransforms 此刻无已初始化体，重复调用无副作用）。
    m_PhysicsWorld->ResetAccumulator();

    // 4. 第一人称相机初始化：切 FPS 姿态模式、用角色当前朝向初始化 yaw/pitch、
    //    位置钉到角色视点，避免相机从默认姿态"跳"到角色朝向。
    auto fpView = m_Registry.view<TransformComponent, CharacterControllerComponent,
                                  FirstPersonCameraComponent>();
    if (fpView.begin() != fpView.end()) {
        Entity fpCamEnt = GetPrimaryCameraEntity();
        if (fpCamEnt && fpCamEnt.HasComponent<CameraComponent>()) {
            const entt::entity player = fpView.front();
            const auto &tc = fpView.get<TransformComponent>(player);
            const auto &cc = fpView.get<CharacterControllerComponent>(player);
            const auto &fp = fpView.get<FirstPersonCameraComponent>(player);
            auto &camComp = fpCamEnt.GetComponent<CameraComponent>();
            Camera &cam = camComp.CameraInstance;

            cam.SetMode(Camera::Mode::FPS);
            cam.MinPitch = fp.MinPitch;
            cam.MaxPitch = fp.MaxPitch;
            // 相机 yaw 初始化为角色朝向：FacingYaw 是相对 BaseRotation 的累计偏航，
            // 反解出相机绝对朝向 = FacingYaw + modelYaw（与 UpdateFirstPersonCamera 的
            // target = 相机yaw − modelYaw 互为逆运算，见计划书 §4）。
            glm::vec3 configured(0.0f, 0.0f, 1.0f);
            switch (cc.FrontAxis) {
            case CapsuleAxis::X: configured = {1.0f, 0.0f, 0.0f};
                break;
            case CapsuleAxis::Y: configured = {0.0f, 1.0f, 0.0f};
                break;
            default: configured = {0.0f, 0.0f, 1.0f};
                break;
            }
            if (cc.InvertFront) {
                configured = -configured; // 与 UpdateFirstPersonCamera 的 modelYaw 保持一致
            }
            const glm::vec3 wv = cc.BaseRotation * configured;
            const float modelYaw = std::atan2(wv.x, wv.z);
            const float initYaw = glm::degrees(cc.FacingYaw + modelYaw);
            cam.SetYawPitch(initYaw, 0.0f);
            cam.SetPosition(tc.Translation + fp.EyeOffset);
        }
    }

    m_SimulationState = SimulationState::Playing;
}

void Scene::Stop() {
    // 非 Playing：调用无操作
    if (m_SimulationState != SimulationState::Playing)
        return;
    if (!m_PhysicsWorld)
        return;

    // 1. 销毁全部刚体（仅 IsInitialized 进 Jolt；pending 未建的由第 3 步清空列表收口）
    auto rbView = m_Registry.view<RigidBodyComponent>();
    for (auto entity : rbView)
        m_PhysicsWorld->DestroyRigidBody(entity);

    // 2. 销毁全部角色控制器，并复位面朝基准：下次 Play 从回滚后的姿态重新捕获，
    //    避免 Edit 期间改了旋转后仍沿用旧基准导致转向漂移。
    auto ccView = m_Registry.view<CharacterControllerComponent>();
    for (auto entity : ccView) {
        m_PhysicsWorld->DestroyCharacter(entity);
        auto &ccc = ccView.get<CharacterControllerComponent>(entity);
        ccc.FacingInit = false;
        ccc.FacingYaw = 0.0f;
        ccc.BaseRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        ccc.WishVelocity = {0.0f, 0.0f, 0.0f};
        ccc.JumpRequested = false;
    }

    // 3. 清空 pending：编辑态新加未建 / Play 后又加的刚体与角色全部清掉，不留待建残留
    m_PhysicsWorld->ClearPendingBodies();
    m_PhysicsWorld->ClearPendingCharacters();

    // 4. 回滚 Transform 快照 → 实体回到播放前摆放姿态（下帧 UpdateWorldTransforms 重建 world）
    for (const auto &s : m_PlaySnapshot) {
        // 播放期间实体可能被脚本销毁：reg.valid 兜底（物理计划风险 3）
        if (!m_Registry.valid(s.entity))
            continue;
        auto *tc = m_Registry.try_get<TransformComponent>(s.entity);
        if (!tc)
            continue;
        tc->Translation = s.translation;
        tc->Rotation = s.rotation;
    }
    m_PlaySnapshot.clear();

    // 5. 累加器归零：Edit 态不再步进，把干净状态留给下次 Play
    m_PhysicsWorld->ResetAccumulator();

    m_SimulationState = SimulationState::Edit;
}

void Scene::UpdateWorldTransforms() {
    auto view = m_Registry.view<TransformComponent>();
    for (auto entity : view) {
        const auto &tc = view.get<TransformComponent>(entity);

        // 有父实体者由父的递归下钻访问，此处跳过（根→子顺序由递归嵌套保证先父后子）。
        if (tc.parent != entt::null && m_Registry.valid(tc.parent)
            && m_Registry.all_of<TransformComponent>(tc.parent)) {
            continue;
        }

        // 父实体已失效（正常流程经 SetParent/DestroyEntity 不会出现）：兜底自动脱离，
        // 否则该实体既不被父递归访问、又因 parent 非空被跳过，世界矩阵将永不更新。
        if (tc.parent != entt::null) {
            auto &mutableTc = view.get<TransformComponent>(entity);
            mutableTc.parent = entt::null;
            if (auto it = m_ChildrenOf.find(tc.parent); it != m_ChildrenOf.end()) {
                auto &vec = it->second;
                vec.erase(std::remove(vec.begin(), vec.end(), entity), vec.end());
                if (vec.empty())
                    m_ChildrenOf.erase(it);
            }
            GE_CORE_WARN("Scene::UpdateWorldTransforms: 父实体已失效，实体 {} 自动脱离为根",
                         static_cast<uint32_t>(entity));
        }

        // 从根递归下钻（世界矩阵缓存重建 + 反向索引保序）
        UpdateWorldTransformsRecursive(entity, glm::mat4(1.0f));
    }
}

void Scene::UpdateWorldTransformsRecursive(entt::entity entity, const glm::mat4 &parentWorld) {
    auto &tc = m_Registry.get<TransformComponent>(entity);
    tc.worldMatrix = parentWorld * tc.GetLocalMatrix();

    if (auto it = m_ChildrenOf.find(entity); it != m_ChildrenOf.end()) {
        for (entt::entity child : it->second) {
            if (m_Registry.valid(child) && m_Registry.all_of<TransformComponent>(child))
                UpdateWorldTransformsRecursive(child, tc.worldMatrix);
        }
    }
}

void Scene::UpdateSkins() {
    // 上一帧皮肤上传记录已随帧池重置失效，本帧从头重算
    m_SkinJointUploads.clear();

    auto skinView = m_Registry.view<SkinComponent>();
    if (skinView.empty()) {
        return;
    }

    auto &frame = Renderer::GetRenderContext().GetActiveFrame();

    // 同一条 glTF skin 被多个 node 引用时，各 SkinComponent 共享同一 SkinDef
    //（shared_ptr）。只对去重后的每个 SkinDef 算一遍、传一块 SSBO——比如整车
    // 107 块蒙皮共享一骨架时，从 107 次计算/上传降到 1 次，多 node 复用同结果。
    std::unordered_set<const void *> handled;
    for (auto entity : skinView) {
        const auto &skin = skinView.get<SkinComponent>(entity);
        if (!skin.skin) {
            continue;
        }
        const void *def = skin.skin.get();
        if (!handled.insert(def).second) {
            continue; // 本帧该皮肤已计算上传过
        }

        const auto &joints = skin.joints();
        const auto &ibm = skin.inverseBindMatrices();
        const size_t jointCount = joints.size();
        // joints 与 IBM 必须一一对应（导入期已校验，此处兜底）
        if (jointCount == 0 || ibm.size() != jointCount) {
            continue;
        }

        // 关节矩阵 = 关节当前世界矩阵 × 逆绑定矩阵。
        // world 由本帧 UpdateWorldTransforms 的 DFS 刚算好（本帧最新）；
        // IBM 是绑定姿态常量，导入后不再变，两者相乘即蒙皮变换矩阵。
        std::vector<glm::mat4> jointMatrices(jointCount);
        bool valid = true;
        for (size_t i = 0; i < jointCount; ++i) {
            auto *tc = m_Registry.try_get<TransformComponent>(joints[i]);
            if (!tc) {
                valid = false;
                break;
            }
            jointMatrices[i] = tc->GetWorldMatrix() * ibm[i];
        }
        if (!valid) {
            // 关节实体已销毁等洞况：跳过本皮肤，画面上退化为静态
            continue;
        }

        // 从本帧 BufferPool 分配一段关节矩阵 SSBO 并上传（帧池每帧重置，下帧覆盖写）
        BufferAllocation alloc = frame.AllocateBuffer(
            vk::BufferUsageFlagBits::eStorageBuffer,
            jointMatrices.size() * sizeof(glm::mat4));
        alloc.update(jointMatrices);

        m_SkinJointUploads.push_back(SkinJointUpload{
            def, static_cast<uint32_t>(jointCount), alloc});
    }
}


void Scene::UpdateAnimations(Timestep ts) {
    // 动画采样/事件/过渡混合 + ASM 求值 已抽取到 AnimationSystem 模块（Scene/AnimationSystem.cpp）
    AnimationSystem::UpdateAnimations(m_Registry, m_ScriptEngine, ts);
}

void Scene::OnUpdate(Timestep ts,
                     const glm::mat4 &viewProjection,
                     const glm::vec4 &clearColor) {
    // ── 输入快照结算 + 脚本更新（2D 场景路径）────────────────────────────
    m_InputState.BeginFrameInput();
    m_ScriptEngine.OnUpdate(ts);

    // ── 世界矩阵缓存重建（每帧一次 DFS：先于渲染，保证本帧矩阵最新） ──
    UpdateWorldTransforms();

    // ── 2D 精灵渲染 ────────────────────────────────────────────────────
    auto &r2d = Renderer::Get2DRenderer();
    r2d.BeginScene(glm::mat4(1.0f), viewProjection, false, clearColor);

    auto view = m_Registry.view<TransformComponent, SpriteRendererComponent>();
    for (auto entity : view) {
        auto &tc = view.get<TransformComponent>(entity);
        auto &sc = view.get<SpriteRendererComponent>(entity);

        r2d.DrawSprite(
            tc.GetWorldMatrix(),
            sc.SpriteTexture,
            sc.Color
            );
    }

    r2d.EndScene();
}

void Scene::OnUpdate3D(Timestep ts,
                       const glm::mat4 &view,
                       const glm::mat4 &projection,
                       const glm::vec3 &viewPos,
                       const glm::vec4 &clearColor) {
    // ── 输入快照结算：本轮事件累积 → 本帧语义，脚本随后在 OnUpdate 查询 ──
    m_InputState.BeginFrameInput();

    // ── 脚本更新 ──
    UpdateScripts(ts);

    // ── 物理步进 ──
    StepPhysics(ts);

    // ── 第一人称跟随相机 ─────────────────────────────────────────────
    // 放在物理步进之后：相机侧写的 FacingYaw 落在物理子步消费【之后】，
    // 下一帧物理才用到；位置取角色最新脚底。世界矩阵在其后重算，渲染即用最新姿态。
    UpdateFirstPersonCamera();

    // ── 动画更新：采样键帧写目标实体的局部 TRS ────────────────────────
    // 放在物理之后、UpdateWorldTransforms 之前：动画写的是局部 TRS，稍后 DFS
    // 重算 world，蒙皮随之拿到最新关节矩阵（时序见计划书 §10）。
    UpdateAnimations(ts);

    // ── 世界矩阵缓存重建（每帧一次 DFS）───────────────────────────────
    // 放在物理步进之后、光源收集/渲染之前：物理刚回写完局部 TRS，
    // 此处重算让本帧渲染即使用最新世界矩阵。
    UpdateWorldTransforms();

    // ── 蒙皮更新：基于本帧刚算好的关节 world，算 jointMatrix = world × IBM ──
    // 并上传关节 SSBO。必须先于渲染、且紧跟 UpdateWorldTransforms。
    UpdateSkins();

    // ── 光源收集 ──
    UpdateLightParams();

    // ── 环境驱动 ──
    UpdateEnvironment();

    // ── 3D 网格渲染 ──
    RenderMeshes3D(view, projection, viewPos, clearColor);

    // ── 2D 精灵渲染 ──
    RenderSprites2D(view, projection);

}

void Scene::UpdateScripts(Timestep ts) {
    // Lua 脚本每帧推进（输入查询由引擎注入的 input.* 在脚本 OnUpdate 内完成）
    m_ScriptEngine.OnUpdate(ts);
}

void Scene::UpdateFirstPersonCamera() {
    // 编辑态不跟随：FPS 相机只服务 Play 模拟（编辑视角由 EditorCamera 独立持有）
    if (m_SimulationState != SimulationState::Playing) {
        return;
    }

    // 1. 取场景主相机（Primary 优先；沿用 GetPrimaryCameraEntity 语义）
    Entity camEnt = GetPrimaryCameraEntity();
    if (!camEnt || !camEnt.HasComponent<CameraComponent>()) {
        return;
    }
    auto &camComp = camEnt.GetComponent<CameraComponent>();
    Camera &cam = camComp.CameraInstance;

    // 2. 找挂 FPS 组件的角色实体（本场景约定至多一台；多台取第一个）
    auto view = m_Registry.view<TransformComponent, CharacterControllerComponent,
                                FirstPersonCameraComponent>();
    if (view.begin() == view.end()) {
        return;
    }
    entt::entity player = view.front();
    auto &tc = view.get<TransformComponent>(player);
    auto &cc = view.get<CharacterControllerComponent>(player);
    auto &fp = view.get<FirstPersonCameraComponent>(player);
    if (!fp.Enabled) {
        return;
    }

    // 3. 相机切到 FPS 姿态模式（若尚未）；俯仰钳位跟随组件字段
    if (cam.GetMode() != Camera::Mode::FPS) {
        cam.SetMode(Camera::Mode::FPS);
    }
    cam.MinPitch = fp.MinPitch;
    cam.MaxPitch = fp.MaxPitch;

    // 4. 鼠标视角 → yaw/pitch（读本帧输入快照的鼠标增量；灵敏度单位 = 度/像素）
    //    GLFW 窗口坐标 y 向下为正，鼠标上移 → mouseDelta.y 为负；减号让上移 → pitch 增大 → 抬头。
    const glm::vec2 mouseDelta = m_InputState.GetMouseDelta();
    const float yawSign = (fp.InvertY ? -1.0f : 1.0f);
    float yaw = cam.GetYaw() - mouseDelta.x * fp.YawSpeed;
    float pitch = cam.GetPitch() - yawSign * mouseDelta.y * fp.PitchSpeed;
    cam.SetYawPitch(yaw, pitch); // SetYawPitch 内部按 MinPitch/MaxPitch clamp

    // 5. 相机朝向 → 角色朝向：让角色脸朝相机看的方向。
    //    引擎只允许 CharacterVirtual 驱动朝向（UpdateCharacters 每子步
    //    cv->SetRotation 写回 Transform，PhysicsWorld.cpp:697）——相机侧若直接
    //    改 Transform.Rotation 会被下个子步覆盖。因此走组件通道：把相机 yaw
    //    换算成"模型前向应指向的水平角" target，复用 FaceMovement 的
    //    FacingYaw 限速逼近机制，让 CharacterVirtual 自己完成旋转。
    //    modelYaw = FrontAxis 局部前向轴（经 BaseRotation 旋转后）的水平朝向角，
    //               与 FaceMovement 的 globalFrontYaw（PhysicsWorld.cpp:689）同语义。
    // {
    //     constexpr float kPi = 3.14159265358979f;
    //     // 单帧最大转向增量（弧度）：相机每秒 yaw 变化即限速上限，
    //     // 保证转身跟手、不因高灵敏度瞬转鬼畜。
    //     constexpr float kTurnMaxStep = glm::radians(720.0f);
    //
    //     glm::vec3 configured(0.0f, 0.0f, 1.0f);
    //     switch (cc.FrontAxis) {
    //     case CapsuleAxis::X: configured = {1.0f, 0.0f, 0.0f}; break;
    //     case CapsuleAxis::Y: configured = {0.0f, 1.0f, 0.0f}; break;
    //     default:             configured = {0.0f, 0.0f, 1.0f}; break;
    //     }
    //     if (cc.InvertFront) {
    //         configured = -configured; // 模型"脸"在 FrontAxis 反方向
    //     }
    //     const glm::vec3 wv = cc.BaseRotation * configured;
    //     const float modelYaw = std::atan2(wv.x, wv.z); // 模型前向固有朝向角
    //     // 让模型前向(水平) = 相机前向(水平)：角色世界前向角 = FacingYaw + modelYaw，
    //     // 令其等于 cam.GetYaw()（Camera::GetForward 的水平朝向角）→ target = yaw − modelYaw。
    //     // 若模型仍与相机朝向相反，说明此角色 FrontAxis 的"前向"与视觉朝向相反
    //     //（模型默认 -Z 是脸、FrontAxis 却配了 +Z），需把 configured 换成反轴。
    //     float target = glm::radians(cam.GetYaw()) - modelYaw;
    //     float diff = target - cc.FacingYaw;
    //     while (diff > kPi)  diff -= 2.0f * kPi;
    //     while (diff < -kPi) diff += 2.0f * kPi;
    //     cc.FacingYaw += std::clamp(diff, -kTurnMaxStep, kTurnMaxStep);
    //     // 不直接调 cv->SetRotation：下个子步 UpdateCharacters 用 FacingYaw 重建旋转。
    // }

    // 6. 角色位置 → 相机位置（角色局部 EyeOffset 随朝向旋转后加到脚底；
    //    yaw 每帧已与相机同步，绕 up 旋转等于绕相机朝向系旋转 → 纯 +Y 不变，
    //    带水平分量自动成"过肩视角"：相机向后拉、随角色转身）
    const float yawRad = glm::radians(cam.GetYaw());
    const glm::vec3 offset(
        fp.EyeOffset.x * std::cos(yawRad) + fp.EyeOffset.z * std::sin(yawRad),
        fp.EyeOffset.y,
        -fp.EyeOffset.x * std::sin(yawRad) + fp.EyeOffset.z * std::cos(yawRad));
    //cam.SetPosition(tc.Translation + offset);
}

void Scene::StepPhysics(Timestep ts) {
    // 阶段 C 门控：编辑态物理整体停摆——不建体、不步进、不派发碰撞事件（决策 5.3）。
    // 动画事件不受此门控（决策 5.6），UpdateAnimations 独立于 StepPhysics。
    if (m_SimulationState != SimulationState::Playing) {
        return;
    }
    if (!m_PhysicsWorld) {
        return;
    }
    // 阶段 B4：Stay 高频通道按需 —— 任一脚本订阅了 Stay 钩子才收集 Persisted，否则零开销
    m_PhysicsWorld->SetStayEnabled(
        m_ScriptEngine.AnyInstanceDefinesHook("OnCollisionStay") ||
        m_ScriptEngine.AnyInstanceDefinesHook("OnTriggerStay"));
    m_PhysicsWorld->Step(ts);
    // 阶段 B3：本帧碰撞事件按实体双侧派发到脚本（到达即派发即弃，不留过帧状态）
    for (const auto &evt : m_PhysicsWorld->TakeCollisionEvents()) {
        DispatchCollisionEvent(evt);
    }
}

void Scene::DispatchCollisionEvent(const Physics::CollisionEvent &evt) {
    // 双侧各投一次：每侧从自己视角收对端 Tag；法线各向自己（B 侧翻转）。
    // reg.valid 兜底：事件缓冲存续期内实体可能已被销毁（物理计划风险 3）。
    if (m_Registry.valid(evt.A)) {
        const auto *tb = m_Registry.try_get<TagComponent>(evt.B);
        m_ScriptEngine.DispatchCollisionEvent(
            evt.A, tb ? tb->Tag : std::string(), evt.isTrigger, evt.phase,
            evt.impulse, evt.normal.x, evt.normal.y, evt.normal.z);
    }
    if (m_Registry.valid(evt.B)) {
        const auto *ta = m_Registry.try_get<TagComponent>(evt.A);
        m_ScriptEngine.DispatchCollisionEvent(
            evt.B, ta ? ta->Tag : std::string(), evt.isTrigger, evt.phase,
            evt.impulse, -evt.normal.x, -evt.normal.y, -evt.normal.z);
    }
}

void Scene::UpdateLightParams() {
    auto &r3d = Renderer::Get3DRenderer();
    auto &lightParams = r3d.GetLightParams();

    // ---- 方向光：取场景中第一个方向光组件 ----
    {
        auto dirLightView = m_Registry.view<TransformComponent, DirectionalLightComponent>();
        if (dirLightView.begin() != dirLightView.end()) {
            auto entity = *dirLightView.begin();
            auto &tc = dirLightView.get<TransformComponent>(entity);
            auto &dlc = dirLightView.get<DirectionalLightComponent>(entity);

            // 由世界矩阵的旋转部分推导出方向光方向（前向向量，-Z 轴旋转后为光线射出方向）。
            // 读世界矩阵而非局部 Rotation：挂在父子层级下时父级旋转一并作用于照射方向。
            // 缩放会乘进 mat3 的三列，先逐列归一化消除后再转四元数（uniform/非 uniform 缩放均安全）。
            // 着色器中 dirLightDirection 表示"指向光源的方向"（即从表面指向光源），
            // 与光线射出方向相反，因此取反
            const glm::mat3 rot3 = glm::mat3(tc.GetWorldMatrix());
            const glm::mat3 normalizedRot(
                glm::normalize(rot3[0]), glm::normalize(rot3[1]), glm::normalize(rot3[2]));
            const glm::quat worldRot = glm::quat_cast(normalizedRot);
            glm::vec3 lightDir = worldRot * glm::vec3(0.0f, 0.0f, -1.0f);
            lightParams.dirLightDirection = glm::normalize(-lightDir);
            lightParams.dirLightColor = dlc.Color;
        } else {
            // 场景中无方向光组件时，使用默认值（斜向下的白色方向光）
            lightParams.dirLightDirection = {0.0f, -1.0f, 0.0f};
            lightParams.dirLightColor = {1.0f, 1.0f, 1.0f, 1.0f};
        }
    }

    // ---- 环境光：取场景中第一个环境光组件 ----
    {
        auto ambientView = m_Registry.view<AmbientLightComponent>();
        if (ambientView.begin() != ambientView.end()) {
            auto entity = *ambientView.begin();
            auto &alc = ambientView.get<AmbientLightComponent>(entity);
            lightParams.ambient = alc.Color;
        } else {
            // 场景中无环境光组件时，使用默认值保证可见性
            lightParams.ambient = {0.3f, 0.3f, 0.3f, 1.0f};
        }
    }

    // ---- 点光源（SSBO 无编译期上限，收集全部点光源）----
    lightParams.pointLights.clear();
    auto pointLightView = m_Registry.view<TransformComponent, PointLightComponent>();
    for (auto entity : pointLightView) {
        auto &tc = pointLightView.get<TransformComponent>(entity);
        auto &plc = pointLightView.get<PointLightComponent>(entity);

        Renderer3D::PointLight pl;
        // 读世界矩阵的平移列：子层级下的点光源位置随父实体整体联动
        pl.position = glm::vec3(tc.GetWorldMatrix()[3]);
        pl.color = plc.Color;
        pl.radiusInv = plc.RadiusInv;
        lightParams.pointLights.push_back(pl);
    }
}

void Scene::UpdateEnvironment() {
    auto &r3d = Renderer::Get3DRenderer();

    // 只把环境名交给渲染器（内部按命名约定加载三张图），这里控制开关：
    // Enabled 总开关（关则天空盒 + IBL 一并关）、SkyboxEnabled 天空盒、IBLEnabled 环境光。
    auto envView = m_Registry.view<EnvironmentComponent>();
    if (envView.begin() != envView.end()) {
        const auto &ec = envView.get<EnvironmentComponent>(*envView.begin());

        if (ec.Enabled) {
            r3d.SetEnvironment(ec.Name);
            r3d.SetSkyboxEnabled(ec.SkyboxEnabled);
            r3d.SetIBLEnabled(ec.IBLEnabled);
            r3d.SetIBLIntensity(ec.IBLIntensity);
        } else {
            // 环境总开关关闭：天空盒 + IBL 一并关闭
            r3d.SetSkyboxEnabled(false);
            r3d.SetIBLEnabled(false);
        }
    } else {
        // 无环境组件：天空盒 + IBL 一并关闭
        r3d.SetSkyboxEnabled(false);
        r3d.SetIBLEnabled(false);
    }
}

void Scene::RenderMeshes3D(const glm::mat4 &view, const glm::mat4 &projection,
                           const glm::vec3 &viewPos, const glm::vec4 &clearColor) {
    auto &r3d = Renderer::Get3DRenderer();
    r3d.BeginScene(view, projection, viewPos, clearColor);

    // 注册本帧各皮肤的关节矩阵缓冲（UpdateSkins 已算好并上传到帧池）。
    // 键是共享 SkinDef 指针：同一条皮肤被多 node 引用时共用一个缓冲。
    // 只有成功上传的皮肤实体才走蒙皮绘制；被跳过（关节缺失/失效）的皮肤
    // 本帧退化为静态网格，且不会触发渲染器缺缓冲告警。
    std::unordered_set<const void *> activeSkins;
    activeSkins.reserve(m_SkinJointUploads.size());
    for (const auto &upload : m_SkinJointUploads) {
        activeSkins.insert(upload.skinDef);
        r3d.SetSkinJointBuffer(upload.skinDef, upload.jointBuffer);
    }

    // 视锥剔除：由 viewProjection 提取 6 平面。
    //   Mesh    模式：逐实体以世界空间整网格 AABB 判外，整实体完全在外才跳过。
    //   SubMesh 模式：先做网格级粗筛（整实体完全在外直接跳过，免逐子网格重复判定），
    //                再基于 SubMesh::aabb 逐子网格判外，仅跳过完全在视锥外的子网格。
    // 两种模式均保守：AABB 无效或与任一平面相交都保留，避免误剔。
    const Frustum frustum = Frustum::FromViewProjection(projection * view);
    const bool cullSubMesh = (m_CullingMode == CullingMode::SubMesh);

    // 实体级粗剔除（可选）：挂在实体上的 BoundingBoxComponent 判外 → 整棵子树连带剔除。
    // 目的见 BoundingBoxComponent 注释：蒙皮实体因绑定盒追不上变形，被下方循环整段跳过
    // 不剔，靠手动摆放的盒补一级「角色级粗筛」——角色在屏幕外时一次跳过整棵深处网格。
    // 实现：预扫描沿 m_ChildrenOf 从每个根（Transform.parent == null）DF S，节点带有效盒
    // 且世界盒完全在视锥外 → 节点与全部后代加入 subtreeRejected。无任何盒组件时跳过
    // （小型/纯静态场景零损耗）。
    std::unordered_set<entt::entity> subtreeRejected;
    auto boundsView = m_Registry.view<BoundingBoxComponent>();
    if (boundsView.begin() != boundsView.end()) {
        // 以 node 为根的整棵子树收进 subtreeRejected
        const auto rejectSubtree = [&](entt::entity node, auto &&self) -> void {
            subtreeRejected.insert(node);
            if (auto it = m_ChildrenOf.find(node); it != m_ChildrenOf.end()) {
                for (auto child : it->second) {
                    self(child, self);
                }
            }
        };
        // 节点盒判外的截止条件（无效盒 = 未摆放，保守不剔除）
        const auto boxRejects = [&](entt::entity node) {
            const auto *bc = m_Registry.try_get<BoundingBoxComponent>(node);
            if (!bc || !bc->IsValid()) {
                return false;
            }
            AABB box;
            box.min = bc->minCorner();
            box.max = bc->maxCorner();
            const auto &ntc = m_Registry.get<TransformComponent>(node);
            return !frustum.IsVisible(box.Transformed(ntc.GetWorldMatrix()));
        };
        // 从所有根发起 DFS：盒判外 → 整棵收割；否则下钻子节点
        for (auto node : m_Registry.view<TransformComponent>()) {
            if (m_Registry.get<TransformComponent>(node).parent != entt::null) {
                continue; // 非根节点：由所属根的 DFS 覆盖
            }
            const auto dfs = [&](entt::entity n, auto &&self) -> void {
                if (boxRejects(n)) {
                    rejectSubtree(n, rejectSubtree);
                    return;
                }
                if (auto it = m_ChildrenOf.find(n); it != m_ChildrenOf.end()) {
                    for (auto child : it->second) {
                        self(child, self);
                    }
                }
            };
            dfs(node, dfs);
        }
    }

    auto meshView = m_Registry.view<TransformComponent, MeshRendererComponent>();
    for (auto entity : meshView) {
        auto &tc = meshView.get<TransformComponent>(entity);
        auto &mc = meshView.get<MeshRendererComponent>(entity);

        // 实体级粗剔除：所在子树已被盒判外的实体直接跳过（连 MeshPtr 判空都省）
        if (subtreeRejected.count(entity)) {
            continue;
        }

        if (!mc.MeshPtr) {
            continue;
        }

        // 蒙皮实体判定提前：本帧有有效关节上传的蒙皮网格，渲染几何随动画变形，
        // 其绑定姿态静态 AABB 无法覆盖变形后的顶点范围——对蒙皮实体保守跳过视锥
        // 剔除，避免角色动画中可见部位被陈旧绑定盒误剔（如肢体摆向相机/屏幕边缘）。
        // 带皮肤节点的多个网格共享同一 SkinDef 指针，共用同一共享关节矩阵。
        const auto *skinC = m_Registry.try_get<SkinComponent>(entity);
        const void *skinDef = (skinC && skinC->skin) ? skinC->skin.get() : nullptr;
        const bool isSkinned = (skinDef != nullptr) && activeSkins.count(skinDef) > 0;

        // 网格级粗筛（两种模式共用）：世界空间包围盒 = 模型空间 AABB × 世界矩阵
        // （含旋转缩放）。蒙皮实体跳过（绑定盒追不上变形）；无效包围盒（如异步
        // 网格尚未注入）保守不剔除，避免瞬态误剔。
        const AABB &meshAabb = mc.MeshPtr->GetAABB();
        const bool meshVisible = isSkinned
                                 || !meshAabb.IsValid()
                                 || frustum.IsVisible(meshAabb.Transformed(tc.GetWorldMatrix()));
        if (!meshVisible) {
            continue;
        }

        // 统一子网格路径：材质 = 实体覆写（materialOverrides）优先，否则子网格
        // 默认材质（defaultMaterial）。material == nullptr 时渲染器以白色兜底。
        const auto &subMeshes = mc.MeshPtr->GetSubMeshes();
        for (size_t i = 0; i < subMeshes.size(); ++i) {
            const SubMesh &sub = subMeshes[i];

            // 子网格级细剔除（仅 SubMesh 模式）：基于 SubMesh::aabb 逐子网格判外。
            // 静态网格按绑定盒剔除；蒙皮实体跳过（理由同网格级粗筛）。无效包围盒
            // （如加载期数据异常）保守保留，保证不误剔。
            if (cullSubMesh && !isSkinned && sub.aabb.IsValid()
                && !frustum.IsVisible(sub.aabb.Transformed(tc.GetWorldMatrix()))) {
                continue;
            }

            Material *mat = nullptr;
            auto it = mc.materialOverrides.find(static_cast<uint32_t>(i));
            if (it != mc.materialOverrides.end()) {
                mat = it->second;
            } else {
                mat = sub.defaultMaterial;
            }
            if (isSkinned) {
                r3d.DrawSkinnedSubMesh(tc.GetWorldMatrix(), mc.MeshPtr, sub, mat, mc.Color, skinDef);
            } else {
                r3d.DrawSubMesh(tc.GetWorldMatrix(), mc.MeshPtr, sub, mat, mc.Color);
            }
        }
    }

    r3d.EndScene();
}

void Scene::RenderSprites2D(const glm::mat4 &view, const glm::mat4 &projection) {
    auto &r2d = Renderer::Get2DRenderer();
    auto spriteView = m_Registry.view<TransformComponent, SpriteRendererComponent>();

    // ---- 第一批：世界空间精灵（IsUI=false），参与深度测试 ----
    bool hasWorldSprites = false;
    for (auto entity : spriteView) {
        if (!spriteView.get<SpriteRendererComponent>(entity).IsUI) {
            hasWorldSprites = true;
            break;
        }
    }
    if (hasWorldSprites) {
        r2d.BeginScene(view, projection, true, glm::vec4(-1.0f));
        for (auto entity : spriteView) {
            auto &tc = spriteView.get<TransformComponent>(entity);
            auto &sc = spriteView.get<SpriteRendererComponent>(entity);
            if (sc.IsUI)
                continue;

            r2d.DrawSprite(
                tc.GetWorldMatrix(),
                sc.SpriteTexture,
                sc.Color
                );
        }
        r2d.EndScene();
    }

    // ---- 第二批：UI 精灵（IsUI=true），像素坐标正交投影，无深度 ----
    // 收集 UI 精灵，按 z 值从小到大排序（z 小的先画，z 大的后画、盖在上面）
    struct UISprite {
        TransformComponent *tc;
        SpriteRendererComponent *sc;
        float z;
    };
    std::vector<UISprite> uiSprites;
    for (auto entity : spriteView) {
        auto &sc = spriteView.get<SpriteRendererComponent>(entity);
        if (!sc.IsUI)
            continue;
        auto &tc = spriteView.get<TransformComponent>(entity);
        uiSprites.push_back({&tc, &sc, tc.Translation.z});
    }
    if (!uiSprites.empty() && m_ViewportWidth > 0 && m_ViewportHeight > 0) {
        // 按 z 从小到大排序（小的先画，大的后画 → 大的盖在上面）
        std::sort(uiSprites.begin(), uiSprites.end(),
                  [](const UISprite &a, const UISprite &b) { return a.z < b.z; });

        // 正交投影：像素坐标，左上角 (0,0)，右下角 (w,h)
        // Vulkan 屏幕原点在左上角，top=0, bottom=h 即 Y 轴向下
        float w = static_cast<float>(m_ViewportWidth);
        float h = static_cast<float>(m_ViewportHeight);
        glm::mat4 uiProjection = glm::ortho(0.0f, w, h, 0.0f, -1.0f, 1.0f);

        r2d.BeginScene(glm::mat4(1.0f), uiProjection, false, glm::vec4(-1.0f));
        for (auto &sp : uiSprites) {
            r2d.DrawSprite(
                sp.tc->GetWorldMatrix(),
                sp.sc->SpriteTexture,
                sp.sc->Color
                );
        }
        r2d.EndScene();
    }
}


void Scene::OnEvent(Event &e) {
    // ── 输入事件：仅记录到快照供脚本查询，未消费的再交给主相机控制视角。──
    //    是否转发输入事件由外层 Layer 依据「视口是否悬停」决定，此处不再判断。

    if (e.IsInCategory(EventCategoryInput)) {
        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent &ev) {
            m_InputState.RecordKeyPressed(ev.GetKeyCode(), ev.GetRepeatCount());
            return false;
        });
        dispatcher.Dispatch<KeyReleasedEvent>([&](KeyReleasedEvent &ev) {
            m_InputState.RecordKeyReleased(ev.GetKeyCode());
            return false;
        });
        dispatcher.Dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent &ev) {
            m_InputState.RecordMouseButtonPressed(ev.GetMouseButton());
            return false;
        });
        dispatcher.Dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent &ev) {
            m_InputState.RecordMouseButtonReleased(ev.GetMouseButton());
            return false;
        });
        dispatcher.Dispatch<MouseMovedEvent>([&](MouseMovedEvent &ev) {
            m_InputState.RecordMouseMoved(ev.GetX(), ev.GetY());
            return false;
        });
        dispatcher.Dispatch<MouseScrolledEvent>([&](MouseScrolledEvent &ev) {
            m_InputState.RecordMouseScrolled(ev.GetXOffset(), ev.GetYOffset());
            return false;
        });

        if (m_ProcessCameraInput && !e.Handled) {
            DispatchInputEventToCamera(e);
        }
    }
}

void Scene::DispatchInputEventToCamera(Event &e) {
    Entity cameraEntity = GetPrimaryCameraEntity();
    if (!cameraEntity || !cameraEntity.HasComponent<CameraComponent>()) {
        return;
    }
    auto &cameraComp = cameraEntity.GetComponent<CameraComponent>();
    cameraComp.CameraInstance.OnEvent(e);
}


void Scene::OnViewportResize(const uint32_t width, const uint32_t height) {
    m_ViewportWidth = width;
    m_ViewportHeight = height;
}


template <typename T>
void Scene::OnComponentAdded(Entity entity, T &component) {
    static_assert(false);
}

template <>
void Scene::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent &component) {
}


template <>
void Scene::OnComponentAdded<TagComponent>(Entity entity, TagComponent &component) {
}

template <>
void Scene::OnComponentAdded<IDComponent>(Entity entity, IDComponent &component) {
}

template <>
void Scene::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent &component) {
}

template <>
void Scene::OnComponentAdded<ScriptComponent>(Entity entity, ScriptComponent &component) {
    // 挂载/预加载 Lua 行为表并建实例（空路径跳过）
    m_ScriptEngine.OnComponentAdded(static_cast<entt::entity>(entity));
}

template <>
void Scene::OnComponentAdded<MeshRendererComponent>(Entity entity, MeshRendererComponent &component) {
}

template <>
void Scene::OnComponentAdded<JointComponent>(Entity entity, JointComponent &component) {
}

template <>
void Scene::OnComponentAdded<SkinComponent>(Entity entity, SkinComponent &component) {
}

template <>
void Scene::OnComponentAdded<AnimationComponent>(Entity entity, AnimationComponent &component) {
}

template <>
void Scene::OnComponentAdded<AnimStateMachineComponent>(Entity entity, AnimStateMachineComponent &component) {
}

template <>
void Scene::OnComponentAdded<BoundingBoxComponent>(Entity entity, BoundingBoxComponent &component) {
}

template <>
void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent &component) {
}

template <>
void Scene::OnComponentAdded<PointLightComponent>(Entity entity, PointLightComponent &component) {
}

template <>
void Scene::OnComponentAdded<DirectionalLightComponent>(Entity entity, DirectionalLightComponent &component) {
}

template <>
void Scene::OnComponentAdded<AmbientLightComponent>(Entity entity, AmbientLightComponent &component) {
}

template <>
void Scene::OnComponentAdded<EnvironmentComponent>(Entity entity, EnvironmentComponent &component) {
}

template <>
void Scene::OnComponentAdded<FirstPersonCameraComponent>(Entity entity, FirstPersonCameraComponent &component) {
}

template <>
void Scene::OnComponentAdded<RigidBodyComponent>(Entity entity, RigidBodyComponent &component) {
    // 延迟创建：将实体加入 PhysicsWorld 的待创建列表
    // 实际创建发生在下一次 Step() 调用时，确保 collider 组件也已添加
    if (m_PhysicsWorld) {
        m_PhysicsWorld->RequestCreateRigidBody(static_cast<entt::entity>(entity));
    }
}

template <>
void Scene::OnComponentAdded<BoxColliderComponent>(Entity entity, BoxColliderComponent &component) {
    // 如果实体已有 RigidBodyComponent，需要重新请求创建 body
    if (entity.HasComponent<RigidBodyComponent>()) {
        auto &rbc = entity.GetComponent<RigidBodyComponent>();
        if (rbc.IsInitialized) {
            // 已初始化，需要重建 body（先销毁再创建）
            m_PhysicsWorld->DestroyRigidBody(static_cast<entt::entity>(entity));
        }
        m_PhysicsWorld->RequestCreateRigidBody(static_cast<entt::entity>(entity));
    }
}

template <>
void Scene::OnComponentAdded<SphereColliderComponent>(Entity entity, SphereColliderComponent &component) {
    // 同 BoxColliderComponent 逻辑
    if (entity.HasComponent<RigidBodyComponent>()) {
        auto &rbc = entity.GetComponent<RigidBodyComponent>();
        if (rbc.IsInitialized) {
            m_PhysicsWorld->DestroyRigidBody(static_cast<entt::entity>(entity));
        }
        m_PhysicsWorld->RequestCreateRigidBody(static_cast<entt::entity>(entity));
    }
}

template <>
void Scene::OnComponentAdded<CapsuleColliderComponent>(Entity entity, CapsuleColliderComponent &component) {
    // 同 BoxColliderComponent 逻辑
    if (entity.HasComponent<RigidBodyComponent>()) {
        auto &rbc = entity.GetComponent<RigidBodyComponent>();
        if (rbc.IsInitialized) {
            m_PhysicsWorld->DestroyRigidBody(static_cast<entt::entity>(entity));
        }
        m_PhysicsWorld->RequestCreateRigidBody(static_cast<entt::entity>(entity));
    }
}

template <>
void Scene::OnComponentAdded<CharacterControllerComponent>(Entity entity, CharacterControllerComponent &component) {
    // 延迟创建：加入 PhysicsWorld 待创建列表，下次 Step 取当前 Transform 创建 CharacterVirtual
    if (m_PhysicsWorld) {
        m_PhysicsWorld->RequestCreateCharacter(static_cast<entt::entity>(entity));
    }
}

void Scene::OnRigidBodyDestroyed(entt::registry &registry, entt::entity entity) {
    // EnTT 的 on_destroy 回调，在实体销毁或组件移除时触发
    if (m_PhysicsWorld) {
        m_PhysicsWorld->DestroyRigidBody(entity);
    }
}

void Scene::OnColliderDestroyed(entt::registry &registry, entt::entity entity) {
    // 碰撞体被移除时，如果实体还有 RigidBodyComponent 且已初始化，
    // 则重建刚体以反映新的碰撞形状
    if (!m_PhysicsWorld)
        return;

    auto *rbc = registry.try_get<RigidBodyComponent>(entity);
    if (rbc && rbc->IsInitialized) {
        m_PhysicsWorld->RebuildRigidBody(entity);
    }
}

void Scene::OnScriptComponentDestroyed(entt::registry &registry, entt::entity entity) {
    // 脚本组件移除/实体销毁：调 Lua OnDestroy 并清除实例
    m_ScriptEngine.OnEntityDestroyed(entity);
}

void Scene::OnCharacterControllerDestroyed(entt::registry &registry, entt::entity entity) {
    // 角色组件移除/实体销毁：销毁 Jolt CharacterVirtual（析构自动清 inner body）
    if (m_PhysicsWorld) {
        m_PhysicsWorld->DestroyCharacter(entity);
    }
}


} //
// Created by Lenovo on 2026/5/9.