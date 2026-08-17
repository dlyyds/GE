/**
 * @file PhysicsWorld.cpp
 * @brief 物理世界实现 —— Jolt Physics 封装。
 */

#include "pch.h"
#include "Physics/PhysicsWorld.h"

#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Core/Log.h"

// Jolt 重型头文件仅在 .cpp 中引入
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>

#include <glm/gtc/quaternion.hpp>

namespace GE::Physics {

// ============================================================
// 碰撞层映射实现（Jolt 要求的三个接口）
// ============================================================

namespace {

/// 每个 BroadPhaseLayer 对应一个碰撞层
static constexpr uint32_t NUM_BROAD_PHASE_LAYERS = static_cast<uint32_t>(CollisionLayer::Count);

/// BroadPhaseLayer 到 ObjectLayer 的映射表
class EngineBroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    EngineBroadPhaseLayerInterface() {
        // 创建映射表：ObjectLayer → BroadPhaseLayer
        m_ObjectToBroadPhase[static_cast<JPH::ObjectLayer>(CollisionLayer::Default)] = JPH::BroadPhaseLayer(0);
        m_ObjectToBroadPhase[static_cast<JPH::ObjectLayer>(CollisionLayer::Player)] = JPH::BroadPhaseLayer(1);
        m_ObjectToBroadPhase[static_cast<JPH::ObjectLayer>(CollisionLayer::Enemy)] = JPH::BroadPhaseLayer(2);
        m_ObjectToBroadPhase[static_cast<JPH::ObjectLayer>(CollisionLayer::Trigger)] = JPH::BroadPhaseLayer(3);
    }

    uint32_t GetNumBroadPhaseLayers() const override {
        return NUM_BROAD_PHASE_LAYERS;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        JPH_ASSERT(inLayer < NUM_BROAD_PHASE_LAYERS);
        return m_ObjectToBroadPhase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        switch (static_cast<JPH::BroadPhaseLayer::Type>(inLayer)) {
        case 0: return "Default";
        case 1: return "Player";
        case 2: return "Enemy";
        case 3: return "Trigger";
        default: return "Unknown";
        }
    }
#endif // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED

private:
    JPH::BroadPhaseLayer m_ObjectToBroadPhase[NUM_BROAD_PHASE_LAYERS];
};

/// 判断两个 ObjectLayer 之间是否可以碰撞
class EngineObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override {
        // Trigger 层不与其他层发生物理碰撞（仅触发事件）
        // 但 Trigger 之间也不碰撞
        if (inLayer1 == static_cast<JPH::ObjectLayer>(CollisionLayer::Trigger)
            || inLayer2 == static_cast<JPH::ObjectLayer>(CollisionLayer::Trigger)) {
            return false;
        }
        // 其余层之间都可以碰撞
        return true;
    }
};

/// 判断 ObjectLayer 与 BroadPhaseLayer 是否可以碰撞
class EngineObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        // Trigger 层不参与 broad phase 碰撞
        if (inLayer1 == static_cast<JPH::ObjectLayer>(CollisionLayer::Trigger)) {
            return false;
        }
        // 其余层都参与碰撞
        return true;
    }
};

} // anonymous namespace

// ============================================================
// GLM ↔ Jolt 数学转换辅助函数
// ============================================================

JPH::Vec3 PhysicsWorld::ToJoltVec3(const glm::vec3 &v) {
    return JPH::Vec3(v.x, v.y, v.z);
}

glm::vec3 PhysicsWorld::ToGlmVec3(const JPH::Vec3 &v) {
    return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
}

JPH::Quat PhysicsWorld::ToJoltQuat(const glm::quat &q) {
    // GLM: (w, x, y, z) → Jolt: (x, y, z, w)
    return JPH::Quat(q.x, q.y, q.z, q.w);
}

glm::quat PhysicsWorld::ToGlmQuat(const JPH::Quat &q) {
    // Jolt: (x, y, z, w) → GLM: (w, x, y, z)
    return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

// ============================================================
// 构造 / 析构
// ============================================================

PhysicsWorld::PhysicsWorld(Scene *scene)
    : m_Scene(scene) {
    InitJolt();
}

PhysicsWorld::~PhysicsWorld() {
    // 先销毁 Jolt 物理系统，再销毁依赖对象
    m_PhysicsSystem.reset();
    m_JobSystem.reset();
    m_TempAllocator.reset();
    m_BroadPhaseLayerInterface.reset();
    m_ObjectVsBroadPhaseFilter.reset();
    m_ObjectLayerPairFilter.reset();
}

void PhysicsWorld::InitJolt() {
    // 注册 Jolt 内置类型（必须在任何 Jolt 操作之前调用）
    JPH::RegisterDefaultAllocator();

    // 创建 Jolt 工厂（注册所有内置 shape/constraint 类型）
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    // 创建碰撞层映射
    m_BroadPhaseLayerInterface = std::make_unique<EngineBroadPhaseLayerInterface>();
    m_ObjectVsBroadPhaseFilter = std::make_unique<EngineObjectVsBroadPhaseLayerFilter>();
    m_ObjectLayerPairFilter = std::make_unique<EngineObjectLayerPairFilter>();

    // 创建临时分配器（10MB，足够大多数场景使用）
    m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

    // 创建任务系统（线程池，使用 CPU 核心数 - 1 个线程）
    m_JobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs,
        JPH::cMaxPhysicsBarriers,
        std::max(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1)
        );

    // 物理系统配置
    constexpr uint32_t cMaxBodies = 4096; // 最大刚体数量
    constexpr uint32_t cMaxBodyPairs = 32768; // 最大刚体对
    constexpr uint32_t cMaxContactConstraints = 4096; // 最大接触约束

    // 创建物理系统
    m_PhysicsSystem = std::make_unique<JPH::PhysicsSystem>();
    m_PhysicsSystem->Init(
        cMaxBodies,
        static_cast<uint32_t>(CollisionLayer::Count),
        cMaxBodyPairs,
        cMaxContactConstraints,
        *m_BroadPhaseLayerInterface,
        *m_ObjectVsBroadPhaseFilter,
        *m_ObjectLayerPairFilter
        );

    // 设置重力
    m_PhysicsSystem->SetGravity(ToJoltVec3(m_Gravity));

    GE_CORE_INFO("PhysicsWorld: Jolt 物理系统初始化完成");
}

// ============================================================
// 步进
// ============================================================

void PhysicsWorld::Step(Timestep ts) {
    if (!m_PhysicsSystem)
        return;

    // Step 1: 处理待创建刚体列表
    ProcessPendingBodies();

    // Step 2: 同步运动学体 Transform → Jolt
    SyncKinematicTransformsToBodies();

    // Step 3: 固定步长累加器 + 物理步进
    const float dt = ts.GetSeconds();
    m_Accumulator += dt;

    // 防止"死亡螺旋"：长时间卡顿后限制最大步进次数
    int subSteps = 0;
    while (m_Accumulator >= FIXED_TIMESTEP && subSteps < MAX_SUBSTEPS) {
        m_PhysicsSystem->Update(
            FIXED_TIMESTEP,
            1, // collisionSteps
            m_TempAllocator.get(),
            m_JobSystem.get()
            );
        m_Accumulator -= FIXED_TIMESTEP;
        subSteps++;
    }

    // 如果累积器溢出，重置为 0
    if (subSteps >= MAX_SUBSTEPS) {
        m_Accumulator = 0.0f;
    }

    // Step 4: 同步动态体 Jolt → Transform
    SyncBodiesToTransforms();
}

// ============================================================
// 刚体管理
// ============================================================

void PhysicsWorld::RequestCreateRigidBody(entt::entity entity) {
    // 去重检查
    for (auto pending : m_PendingBodies) {
        if (pending == entity)
            return;
    }
    m_PendingBodies.push_back(entity);
}

void PhysicsWorld::DestroyRigidBody(entt::entity entity) {
    if (!m_Scene)
        return;

    auto &reg = m_Scene->Reg();
    if (!reg.valid(entity))
        return;

    auto *rbc = reg.try_get<RigidBodyComponent>(entity);
    if (!rbc || !rbc->IsInitialized)
        return;

    auto &bodyInterface = m_PhysicsSystem->GetBodyInterface();
    bodyInterface.RemoveBody(rbc->RuntimeBodyID);
    bodyInterface.DestroyBody(rbc->RuntimeBodyID);

    rbc->RuntimeBodyID = InvalidBodyID();
    rbc->IsInitialized = false;
}

void PhysicsWorld::UpdateRigidBodyProperties(entt::entity entity) {
    if (!m_Scene || !m_PhysicsSystem)
        return;

    auto &reg = m_Scene->Reg();
    if (!reg.valid(entity))
        return;

    auto *rbc = reg.try_get<RigidBodyComponent>(entity);
    if (!rbc || !rbc->IsInitialized)
        return;

    auto &bodyInterface = m_PhysicsSystem->GetBodyInterface();
    const JPH::BodyID &bodyID = rbc->RuntimeBodyID;

    // 摩擦和弹性可通过 BodyInterface 直接设置
    bodyInterface.SetFriction(bodyID, rbc->Friction);
    bodyInterface.SetRestitution(bodyID, rbc->Restitution);
    bodyInterface.SetIsSensor(bodyID, rbc->IsSensor);

    // 阻尼、传感器、质量需要通过 BodyLockWrite 访问 Body/MotionProperties
    JPH::BodyLockWrite bodyLock(m_PhysicsSystem->GetBodyLockInterface(), bodyID);
    if (!bodyLock.Succeeded())
        return;

    JPH::Body &body = bodyLock.GetBody();

    // 更新阻尼（仅非静态体有 MotionProperties）
    if (rbc->Type != RigidBodyType::Static) {
        JPH::MotionProperties *mp = body.GetMotionProperties();
        if (mp) {
            mp->SetLinearDamping(rbc->LinearDamping);
            mp->SetAngularDamping(rbc->AngularDamping);

            // 更新质量（仅动态体，设置逆质量）
            if (rbc->Type == RigidBodyType::Dynamic && rbc->Mass > 0.0f) {
                mp->SetInverseMass(1.0f / rbc->Mass);
            }
        }
    }
}

void PhysicsWorld::RebuildRigidBody(entt::entity entity) {
    if (!m_Scene || !m_PhysicsSystem)
        return;

    auto &reg = m_Scene->Reg();
    if (!reg.valid(entity))
        return;

    auto *rbc = reg.try_get<RigidBodyComponent>(entity);
    if (!rbc)
        return;

    // 如果 body 已初始化，先销毁
    if (rbc->IsInitialized) {
        DestroyRigidBody(entity);
    }

    // 加入待创建列表，下一次 Step() 时重建
    RequestCreateRigidBody(entity);
}

void PhysicsWorld::ProcessPendingBodies() {
    if (m_PendingBodies.empty() || !m_Scene)
        return;

    auto &reg = m_Scene->Reg();
    auto &bodyInterface = m_PhysicsSystem->GetBodyInterface();

    // 收集成功创建和仍需等待的 entity
    std::vector<entt::entity> completed;
    std::vector<entt::entity> stillPending;

    for (auto entity : m_PendingBodies) {
        if (!reg.valid(entity)) {
            completed.push_back(entity); // 无效实体直接丢弃
            continue;
        }

        auto *rbc = reg.try_get<RigidBodyComponent>(entity);
        if (!rbc || rbc->IsInitialized) {
            completed.push_back(entity); // 已初始化或组件已移除
            continue;
        }

        // 需要 TransformComponent 和至少一个碰撞体
        auto *tc = reg.try_get<TransformComponent>(entity);
        if (!tc || !HasColliderComponent(entity)) {
            // 条件不满足，保留在 pending 列表中等待下次 Step
            stillPending.push_back(entity);
            continue;
        }

        // 构建碰撞形状
        JPH::ShapeRefC shape = BuildShapeForEntity(entity);
        if (shape == nullptr) {
            stillPending.push_back(entity);
            continue;
        }

        // 确定运动类型
        JPH::EMotionType motionType;
        JPH::ObjectLayer objectLayer;
        switch (rbc->Type) {
        case RigidBodyType::Static: motionType = JPH::EMotionType::Static;
            objectLayer = static_cast<JPH::ObjectLayer>(CollisionLayer::Default);
            break;
        case RigidBodyType::Kinematic: motionType = JPH::EMotionType::Kinematic;
            objectLayer = static_cast<JPH::ObjectLayer>(CollisionLayer::Default);
            break;
        case RigidBodyType::Dynamic: motionType = JPH::EMotionType::Dynamic;
            objectLayer = static_cast<JPH::ObjectLayer>(CollisionLayer::Default);
            break;
        default: motionType = JPH::EMotionType::Static;
            objectLayer = static_cast<JPH::ObjectLayer>(CollisionLayer::Default);
            break;
        }

        // 从 TransformComponent 获取初始位置和旋转
        const glm::quat &glmRot = tc->Rotation;
        JPH::Vec3 position = ToJoltVec3(tc->Translation);
        JPH::Quat rotation = ToJoltQuat(glmRot);

        // 创建 BodyCreationSettings
        JPH::BodyCreationSettings bodySettings(
            shape,
            position,
            rotation,
            motionType,
            objectLayer
            );

        // 设置物理属性
        bodySettings.mFriction = rbc->Friction;
        bodySettings.mRestitution = rbc->Restitution;
        bodySettings.mLinearDamping = rbc->LinearDamping;
        bodySettings.mAngularDamping = rbc->AngularDamping;
        bodySettings.mIsSensor = rbc->IsSensor;

        if (motionType == JPH::EMotionType::Dynamic) {
            bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bodySettings.mMassPropertiesOverride.mMass = rbc->Mass;
        }

        // 创建并添加 body 到物理世界
        // 静态体不需要激活，动态体需要激活才能开始模拟
        JPH::EActivation activation = (motionType == JPH::EMotionType::Static)
                                          ? JPH::EActivation::DontActivate
                                          : JPH::EActivation::Activate;

        JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(bodySettings, activation);

        // 写回组件
        rbc->RuntimeBodyID = bodyID;
        rbc->IsInitialized = true;

        completed.push_back(entity);
    }

    // 只保留仍需等待的 entity
    m_PendingBodies = std::move(stillPending);
}

bool PhysicsWorld::HasColliderComponent(entt::entity entity) const {
    if (!m_Scene)
        return false;
    auto &reg = m_Scene->Reg();
    return reg.any_of<BoxColliderComponent, SphereColliderComponent>(entity);
}

JPH::ShapeRefC PhysicsWorld::BuildShapeForEntity(entt::entity entity) {
    if (!m_Scene)
        return nullptr;

    auto &reg = m_Scene->Reg();
    auto *tc = reg.try_get<TransformComponent>(entity);
    if (!tc)
        return nullptr;

    std::vector<JPH::ShapeRefC> subShapes;

    // 收集 BoxCollider 组件
    auto *boxColliders = reg.try_get<BoxColliderComponent>(entity);
    if (boxColliders) {
        glm::vec3 scaledHalfExtents = boxColliders->HalfExtents * tc->Scale;
        JPH::ShapeRefC boxShape = new JPH::BoxShape(
            ToJoltVec3(scaledHalfExtents),
            0.0f, // convexRadius（0 表示使用默认值）
            nullptr // 材质（nullptr 表示默认）
            );

        if (boxColliders->Offset != glm::vec3(0.0f)) {
            JPH::RotatedTranslatedShapeSettings offsetSettings(
                ToJoltVec3(boxColliders->Offset),
                JPH::Quat::sIdentity(),
                boxShape
                );
            auto result = offsetSettings.Create();
            if (result.IsValid()) {
                subShapes.push_back(result.Get());
            }
        } else {
            subShapes.push_back(boxShape);
        }
    }

    // 收集 SphereCollider 组件
    auto *sphereColliders = reg.try_get<SphereColliderComponent>(entity);
    if (sphereColliders) {
        float scaledRadius = sphereColliders->Radius * std::max({tc->Scale.x, tc->Scale.y, tc->Scale.z});
        JPH::ShapeRefC sphereShape = new JPH::SphereShape(scaledRadius);

        if (sphereColliders->Offset != glm::vec3(0.0f)) {
            JPH::RotatedTranslatedShapeSettings offsetSettings(
                ToJoltVec3(sphereColliders->Offset),
                JPH::Quat::sIdentity(),
                sphereShape
                );
            auto result = offsetSettings.Create();
            if (result.IsValid()) {
                subShapes.push_back(result.Get());
            }
        } else {
            subShapes.push_back(sphereShape);
        }
    }

    if (subShapes.empty()) {
        GE_CORE_WARN("PhysicsWorld: 实体没有碰撞体组件，无法创建 shape");
        return nullptr;
    }

    if (subShapes.size() == 1) {
        return subShapes[0];
    }

    // 多个碰撞体 → 合并为复合形状
    JPH::StaticCompoundShapeSettings compoundSettings;
    for (auto &subShape : subShapes) {
        compoundSettings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), subShape);
    }

    auto result = compoundSettings.Create();
    if (result.IsValid()) {
        return result.Get();
    }

    return nullptr;
}

// ============================================================
// Transform 同步
// ============================================================

void PhysicsWorld::SyncBodiesToTransforms() {
    if (!m_Scene)
        return;

    auto &reg = m_Scene->Reg();
    auto view = reg.view<TransformComponent, RigidBodyComponent>();
    auto &bodyInterface = m_PhysicsSystem->GetBodyInterface();

    for (auto entity : view) {
        auto &rbc = view.get<RigidBodyComponent>(entity);
        if (!rbc.IsInitialized)
            continue;
        if (rbc.Type != RigidBodyType::Dynamic)
            continue;

        JPH::Vec3 pos = bodyInterface.GetPosition(rbc.RuntimeBodyID);
        JPH::Quat rot = bodyInterface.GetRotation(rbc.RuntimeBodyID);

        auto &tc = view.get<TransformComponent>(entity);
        tc.Translation = ToGlmVec3(pos);
        // 直接回写四元数，避免 eulerAngles 往返在近万向锁区域引入抖动/跳变
        tc.Rotation = ToGlmQuat(rot);
    }
}

void PhysicsWorld::SyncKinematicTransformsToBodies() {
    if (!m_Scene)
        return;

    auto &reg = m_Scene->Reg();
    auto view = reg.view<TransformComponent, RigidBodyComponent>();
    auto &bodyInterface = m_PhysicsSystem->GetBodyInterface();

    for (auto entity : view) {
        auto &rbc = view.get<RigidBodyComponent>(entity);
        if (!rbc.IsInitialized)
            continue;
        if (rbc.Type != RigidBodyType::Kinematic)
            continue;

        auto &tc = view.get<TransformComponent>(entity);
        const glm::quat &glmRot = tc.Rotation;

        bodyInterface.MoveKinematic(
            rbc.RuntimeBodyID,
            ToJoltVec3(tc.Translation),
            ToJoltQuat(glmRot),
            FIXED_TIMESTEP
            );
    }
}

// ============================================================
// 重力
// ============================================================

void PhysicsWorld::SetGravity(const glm::vec3 &gravity) {
    m_Gravity = gravity;
    if (m_PhysicsSystem) {
        m_PhysicsSystem->SetGravity(ToJoltVec3(gravity));
    }
}

} // namespace GE::Physics