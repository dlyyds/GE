/**
 * @file PhysicsWorld.cpp
 * @brief 物理世界实现 —— Jolt Physics 封装。
 */

#include "pch.h"
#include "Physics/PhysicsWorld.h"

#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Core/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>
#include <vector>

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
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
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

/// 判断两个 ObjectLayer 之间是否可以碰撞。
/// 当前所有刚体统一放 Default 层；Trigger 语义由传感器标记（mIsSensor）承担——
/// 传感器照常产生接触事件但不参与物理求解，故这里放行所有层对（按 Jolt 示例默认）。
class EngineObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override {
        return true;
    }
};

/// 判断 ObjectLayer 与 BroadPhaseLayer 是否可以碰撞。
/// 同 ObjectLayerPairFilter：默认全放行，后续需要按层剔除时再在此补充。
class EngineObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override {
        return true;
    }
};

} // anonymous namespace

// ============================================================
// 碰撞事件（阶段 B）：Jolt ContactListener → 环形缓冲
// 定义在 GE::Physics 作用域（头文件已 fwd 声明 ContactEventBuffer）。
// 监听器与缓冲互相依赖：先声明监听器，缓冲定义后实现监听器（两位一体）。
// ============================================================

/// 回调内只写这段 POD：BodyID 对 + 纯数值，无 ECS 引用。
/// 回调可能在 Jolt 工作线程上触发，禁止碰 ECS / 分配内存（计划书 §0 硬约束）。
struct RawContactEvent {
    JPH::BodyID body1;
    JPH::BodyID body2;
    float nx = 0.0f, ny = 0.0f, nz = 0.0f; ///< 世界空间法线（向 body1；Stay/Exit 无效）
    float impulse = 0.0f;                  ///< 法向冲量近似（仅 Enter 有效，kg·m/s）
    CollisionPhase phase = CollisionPhase::Enter;
};

/// Jolt 接触监听器：Added→Enter / Persisted→Stay（按需）/ Removed→Exit。
/// 三态天然对应 Jolt 回调；只写环形缓冲，Step 返回后主线程再消费。
/// 成员函数实现放本文件后部（ContactEventBuffer 完整后）。
class EngineContactListener final : public JPH::ContactListener {
public:
    explicit EngineContactListener(ContactEventBuffer &buffer);

    void OnContactAdded(const JPH::Body &inBody1, const JPH::Body &inBody2,
                        const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings) override;
    void OnContactPersisted(const JPH::Body &inBody1, const JPH::Body &inBody2,
                            const JPH::ContactManifold &inManifold, JPH::ContactSettings &ioSettings) override;
    void OnContactRemoved(const JPH::SubShapeIDPair &inPair) override;

private:
    ContactEventBuffer &m_Buffer;
};

/// 固定容量、无堆分配：Listener 用原子计数追加（各线程写各槽），Step 返回后主线程只读消费。
class ContactEventBuffer {
public:
    ContactEventBuffer();

    /// 步进前调用（主线程）：本帧计数归零（Update 同步完成后无残余写入，天然无竞态）
    void Reset() { m_Count.store(0u, std::memory_order_relaxed); }

    void SetStayEnabled(bool on) { m_StayEnabled.store(on, std::memory_order_relaxed); }
    bool StayEnabled() const { return m_StayEnabled.load(std::memory_order_relaxed); }

    /// 回调追加：原子取槽，各线程写不同槽；数量封顶，溢出丢弃（防冲刷，不扩容）
    void Append(const RawContactEvent &e) {
        const uint32_t idx = m_Count.fetch_add(1u, std::memory_order_relaxed);
        if (idx < kCapacity)
            m_Events[idx] = e;
    }

    /// 已写条数（封顶到容量；溢出部分在 Append 里已被丢弃）
    uint32_t Count() const {
        return std::min<uint32_t>(m_Count.load(std::memory_order_relaxed), kCapacity);
    }

    RawContactEvent &operator[](uint32_t idx) { return m_Events[idx]; }

    JPH::ContactListener *Listener() const { return m_Listener.get(); }

    static constexpr uint32_t kCapacity = 256;

private:
    std::array<RawContactEvent, kCapacity> m_Events;
    std::atomic<uint32_t> m_Count{0u};
    std::atomic<bool> m_StayEnabled{false};
    std::unique_ptr<EngineContactListener> m_Listener;
};

// ---- 监听器与缓冲实现（双方完整后才可写）----

ContactEventBuffer::ContactEventBuffer() {
    m_Listener = std::make_unique<EngineContactListener>(*this);
}

EngineContactListener::EngineContactListener(ContactEventBuffer &buffer)
    : m_Buffer(buffer) {
}

void EngineContactListener::OnContactAdded(const JPH::Body &inBody1, const JPH::Body &inBody2,
                                           const JPH::ContactManifold &inManifold,
                                           JPH::ContactSettings &ioSettings) {
    // 求解前冲量未知，用 Jolt 官方估算（读两侧线/角速度 + 流形，纯计算无分配）
    float impulse = 0.0f;
    JPH::CollisionEstimationResult est;
    JPH::EstimateCollisionResponse(inBody1, inBody2, inManifold, est,
                                   ioSettings.mCombinedFriction, ioSettings.mCombinedRestitution);
    for (float contactImpulse : est.mContactImpulse)
        impulse += contactImpulse;
    const JPH::Vec3 &n = inManifold.mWorldSpaceNormal;
    m_Buffer.Append({ inBody1.GetID(), inBody2.GetID(),
                      n.GetX(), n.GetY(), n.GetZ(), impulse, CollisionPhase::Enter });
}

void EngineContactListener::OnContactPersisted(const JPH::Body &inBody1, const JPH::Body &inBody2,
                                               const JPH::ContactManifold &,
                                               JPH::ContactSettings &) {
    if (!m_Buffer.StayEnabled())
        return; // 无脚本订阅 Stay → 不收集（计划书 B4 零开销）
    m_Buffer.Append({ inBody1.GetID(), inBody2.GetID(),
                      0.0f, 0.0f, 0.0f, 0.0f, CollisionPhase::Stay });
}

void EngineContactListener::OnContactRemoved(const JPH::SubShapeIDPair &inPair) {
    // Removed 回调禁止访问 body（ContactListener.h:127），只能取 ID
    m_Buffer.Append({ inPair.GetBody1ID(), inPair.GetBody2ID(),
                      0.0f, 0.0f, 0.0f, 0.0f, CollisionPhase::Exit });
}

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
    // 角色必须先于物理系统销毁：CharacterVirtual 析构要经 BodyInterface 移除 inner body，
    // 若等成员析构（此时 m_PhysicsSystem 已释放）再析构 unique_ptr 会访问悬垂指针。
    m_Characters.clear();
    // 先销毁 Jolt 物理系统，再销毁依赖对象
    m_PhysicsSystem.reset();
    m_JobSystem.reset();
    m_TempAllocator.reset();
    m_BroadPhaseLayerInterface.reset();
    m_ObjectVsBroadPhaseFilter.reset();
    m_ObjectLayerPairFilter.reset();
    // 接触监听器持有 physics system 的裸指针，必须在其析构之后再销毁
    m_ContactBuffer.reset();
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

    // 接触事件监听器（阶段 B）：环形缓冲承载 Jolt contact 回调，Step 返回后主线程消费
    m_ContactBuffer = std::make_unique<ContactEventBuffer>();
    m_PhysicsSystem->SetContactListener(m_ContactBuffer->Listener());

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

    // Step 1.5: 处理待创建角色列表（角色延迟创建，取当前 Transform 作初始位置）
    ProcessPendingCharacters();

    // Step 2: 同步运动学体 Transform → Jolt
    SyncKinematicTransformsToBodies();

    // Step 3: 固定步长累加器 + 物理步进
    const float dt = ts.GetSeconds();
    m_Accumulator += dt;

    // 步进前清零接触缓冲：本帧的 Jolt 回调从 0 开始累加（Update 同步阻塞，无跨帧残留竞态）
    m_ContactBuffer->Reset();

    // 防止"死亡螺旋"：长时间卡顿后限制最大步进次数
    int subSteps = 0;
    while (m_Accumulator >= FIXED_TIMESTEP && subSteps < MAX_SUBSTEPS) {
        m_PhysicsSystem->Update(
            FIXED_TIMESTEP,
            1, // collisionSteps
            m_TempAllocator.get(),
            m_JobSystem.get()
            );
        // 角色在物理 Update 之后查询世界（看到动态体最新位置），在下一子步前推进
        UpdateCharacters(FIXED_TIMESTEP);
        m_Accumulator -= FIXED_TIMESTEP;
        subSteps++;
    }

    // 如果累积器溢出，重置为 0
    if (subSteps >= MAX_SUBSTEPS) {
        m_Accumulator = 0.0f;
    }

    // 步进后：把接触缓冲（BodyID 对）翻译成本帧实体级事件（主线程可碰 ECS）
    CollectCollisionEvents();

    // Step 4: 同步动态体 Jolt → Transform
    SyncBodiesToTransforms();

    // Step 5: 同步角色 CharacterVirtual 位置 → TransformComponent（渲染读它）
    SyncCharacterTransformsToComponents();
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

// ============================================================
// 角色控制器（Jolt CharacterVirtual，计划书 §3）
// ============================================================

void PhysicsWorld::RequestCreateCharacter(entt::entity entity) {
    // 去重：待创建列表已有则跳过（同 RequestCreateRigidBody）
    for (auto pending : m_PendingCharacters) {
        if (pending == entity)
            return;
    }
    m_PendingCharacters.push_back(entity);
}

void PhysicsWorld::DestroyCharacter(entt::entity entity) {
    // 尚未创建的角色停在 pending 列表，先移除再擦除映射
    m_PendingCharacters.erase(
        std::remove(m_PendingCharacters.begin(), m_PendingCharacters.end(), entity),
        m_PendingCharacters.end());
    m_Characters.erase(entity); // unique_ptr 析构 → CharacterVirtual 自动清 inner body

    // 复位初始化标记：RebuildCharacter 依赖它被清掉才能在下次 Step 重新创建。
    // 组件正在被移除/实体销毁时 try_get 返回 nullptr，自动跳过（该路径无需重建）。
    if (m_Scene) {
        if (auto *cc = m_Scene->Reg().try_get<CharacterControllerComponent>(entity)) {
            cc->IsInitialized = false;
        }
    }
}

void PhysicsWorld::RebuildCharacter(entt::entity entity) {
    DestroyCharacter(entity);
    RequestCreateCharacter(entity);
}

void PhysicsWorld::ProcessPendingCharacters() {
    if (m_PendingCharacters.empty() || !m_Scene || !m_PhysicsSystem)
        return;

    auto &reg = m_Scene->Reg();
    std::vector<entt::entity> completed;
    std::vector<entt::entity> stillPending;

    for (auto entity : m_PendingCharacters) {
        if (!reg.valid(entity)) {
            completed.push_back(entity); // 无效实体直接丢弃
            continue;
        }

        auto *cc = reg.try_get<CharacterControllerComponent>(entity);
        if (!cc || cc->IsInitialized) {
            completed.push_back(entity); // 组件已移除或已初始化
            continue;
        }

        auto *tc = reg.try_get<TransformComponent>(entity);
        if (!tc) {
            stillPending.push_back(entity); // 缺 Transform：等待下次 Step
            continue;
        }

        // 胶囊形状：柱身半高 = H/2 - R，总高 = 2·(半高 + R) = H。半高归零退化为球
        //（与 CapsuleCollider 的退化分支一致，CapsuleShape 半高 0 会触发 JPH_ASSERT）
        const float cylHalf = std::max(cc->Height * 0.5f - cc->Radius, 0.0f);
        JPH::ShapeRefC capsule;
        if (cylHalf > 0.0f)
            capsule = new JPH::CapsuleShape(cylHalf, cc->Radius);
        else
            capsule = new JPH::SphereShape(cc->Radius);

        // 轴向烘焙（与 CapsuleCollider 一致：X=绕局部 Z -90°使 Y→X，Z=绕局部 X +90°使 Y→Z），
        // 并沿所选轴把胶囊底部抬到局部原点 —— CharacterBaseSettings 硬约束 shape 底部在 (0,0,0)，
        // 使 Transform.Translation 仍是「脚底」。选轴应保证该轴经实体旋转后指向世界 up
        //（角色仍沿世界 Y 行走/贴地，只旋转胶囊几何）。
        glm::quat axisRot(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 shift(0.0f);
        constexpr float kSqrtHalf = 0.707106781f;
        switch (cc->Axis) {
        case CapsuleAxis::X:
            axisRot = glm::quat(kSqrtHalf, 0.0f, 0.0f, -kSqrtHalf);
            shift = {cc->Height * 0.5f, 0.0f, 0.0f};
            break;
        case CapsuleAxis::Z:
            axisRot = glm::quat(kSqrtHalf, kSqrtHalf, 0.0f, 0.0f);
            shift = {0.0f, 0.0f, cc->Height * 0.5f};
            break;
        default: // Y（默认）：胶囊主轴沿局部 Y，沿 Y 抬升 H/2
            shift = {0.0f, cc->Height * 0.5f, 0.0f};
            break;
        }
        // 轴向用 RotatedTranslatedShape 的位置（外局部帧即角色局部帧）叠加用户 Offset，
        // 使胶囊可相对脚底偏移（与 CapsuleCollider 的 Offset 语义一致）
        JPH::RotatedTranslatedShapeSettings shifted(
            ToJoltVec3(shift) + ToJoltVec3(cc->Offset), ToJoltQuat(axisRot), capsule);
        auto shiftedResult = shifted.Create();
        if (!shiftedResult.IsValid()) {
            stillPending.push_back(entity);
            continue;
        }

        JPH::CharacterVirtualSettings settings;
        settings.mShape = shiftedResult.Get();
        settings.mInnerBodyShape = settings.mShape; // 启用 inner body：角色在世界中有存在感
        settings.mInnerBodyLayer = static_cast<JPH::ObjectLayer>(CollisionLayer::Player);
        settings.mUp = JPH::Vec3::sAxisY();
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(cc->MaxSlopeAngle);
        settings.mPredictiveContactDistance = 0.1f;
        settings.mCharacterPadding = 0.02f;

        // inner body 创建时 settings.mUserData = inUserData（CharacterVirtual.cpp:147），
        // 传实体句柄即让既有 CollectCollisionEvents 免费把接触映射回实体。
        std::unique_ptr<JPH::CharacterVirtual> cv(
            new JPH::CharacterVirtual(&settings,
                                      ToJoltVec3(tc->Translation),
                                      ToJoltQuat(tc->Rotation),
                                      static_cast<JPH::uint64>(static_cast<entt::id_type>(entity)),
                                      m_PhysicsSystem.get()));

        cc->IsInitialized = true;
        cc->IsGrounded = false;
        cc->JumpRequested = false;
        cc->WishVelocity = {0.0f, 0.0f, 0.0f};
        // 首次创建时捕获面朝基准姿态（含转正倾斜）；重建时 FacingInit 挡住，不再重捕获
        if (!cc->FacingInit) {
            cc->BaseRotation = tc->Rotation;
            cc->FacingYaw = 0.0f;
            cc->FacingInit = true;
        }
        m_Characters[entity] = std::move(cv);
        completed.push_back(entity);
    }

    // 只保留仍需等待的实体
    m_PendingCharacters = std::move(stillPending);
}

void PhysicsWorld::UpdateCharacters(float dt) {
    if (m_Characters.empty() || !m_Scene || !m_PhysicsSystem)
        return;

    auto &reg = m_Scene->Reg();
    // ExtendedUpdate = Update + StickToFloor + WalkStairs 一站式（默认上楼 0.4m、贴地下探 0.5m）
    const JPH::CharacterVirtual::ExtendedUpdateSettings extSettings;
    // 对象层过滤：以 Player 层身份撞向全层（复用现有 pair filter 全放行语义）；
    // BroadPhase 过滤：DefaultBroadPhaseLayerFilter 把现有双参 ObjectVsBroadPhaseLayerFilter
    // 以固定对象层（Player）适配成 ExtendedUpdate 需要的单参形式，语义与全放行一致；
    // Body/Shape 过滤默认实现即全放行（Jolt 无 sAllHit 常量，默认构造等价）
    const JPH::DefaultObjectLayerFilter objFilter(
        *m_ObjectLayerPairFilter, static_cast<JPH::ObjectLayer>(CollisionLayer::Player));
    const JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(
        *m_ObjectVsBroadPhaseFilter, static_cast<JPH::ObjectLayer>(CollisionLayer::Player));
    const JPH::BodyFilter bodyFilter;
    const JPH::ShapeFilter shapeFilter;

    for (auto &[entity, cv] : m_Characters) {
        auto *cc = reg.try_get<CharacterControllerComponent>(entity);
        if (!cc || !cv)
            continue;

        // 速度合成：重力由引擎积分（Jolt 原文"自己负责给角色速度施加重力"）。
        // 贴地 → 跟随地面速度（站移动平台不掉落）；悬空 → 保留当前垂直速度。
        // 再叠加重力增量与脚本水平期望速度。
        const JPH::Vec3 up(0.0f, 1.0f, 0.0f);
        const JPH::Vec3 vert(0.0f, cv->GetLinearVelocity().GetY(), 0.0f);
        const JPH::Vec3 groundVel = cv->GetGroundVelocity();
        JPH::Vec3 vel;
        if (cv->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround
            && (vert - groundVel).Dot(up) < 0.1f) {
            vel = groundVel;
            if (cc->JumpRequested) {
                vel += up * cc->MaxJumpSpeed;
                cc->JumpRequested = false;
            }
        } else {
            vel = vert;
        }
        vel += ToJoltVec3(m_Gravity) * dt;   // 重力（每子步）
        vel += ToJoltVec3(cc->WishVelocity); // 脚本水平输入
        cv->SetLinearVelocity(vel);
        cv->ExtendedUpdate(dt, ToJoltVec3(m_Gravity), extSettings,
                           broadPhaseFilter, objFilter, bodyFilter, shapeFilter,
                           *m_TempAllocator);

        // 组件回写：贴地态 + 真实速度 + 地面法线（character.* 查询 API 读这些字段）
        cc->IsGrounded = (cv->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround);
        cc->Velocity = ToGlmVec3(cv->GetLinearVelocity());
        cc->GroundNormalY = cv->GetGroundNormal().GetY();
        cc->JumpRequested = false; // 未贴地也清：空中按压不缓冲，防落地自动跳

        // 面朝水平移动方向（可选）：仅在脚本有水平输入时绕世界 up 缓转朝向。
        // 用「基准姿态 × 累计偏航」重建完整旋转：基准姿态（含转正倾斜）在创建时捕获、
        // 不随之累加，因此模型不会因转向而躺倒；累计偏航每子步朝移动方向角逼近。
        // 目标取 WishVelocity 水平向而非合成速度（避免站移动平台被回带/贴墙朝向归零）。
        if (cc->FaceMovement) {
            constexpr float kPi = 3.14159265358979f;
            const float wx = cc->WishVelocity.x, wz = cc->WishVelocity.z;
            if (std::sqrt(wx * wx + wz * wz) > 0.1f) {
                // 模型前向基准：按 FrontAxis 取局部轴经基准旋转的水平投影；该轴退化
                //（正好指向世界竖直方向，如 armatur 的 +Z 被 +90°X 转朝下）时回退 Y、再 X，
                // 保证带转正倾斜的模型仍能转向。home = 归一化的水平前向
                glm::vec3 home(0.0f);
                glm::vec3 configured(0.0f, 0.0f, 1.0f);
                switch (cc->FrontAxis) {
                case CapsuleAxis::X: configured = {1.0f, 0.0f, 0.0f}; break;
                case CapsuleAxis::Y: configured = {0.0f, 1.0f, 0.0f}; break;
                default: configured = {0.0f, 0.0f, 1.0f}; break;
                }
                const glm::vec3 tryAxes[3] = {configured, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
                for (const auto &ax : tryAxes) {
                    const glm::vec3 wv = cc->BaseRotation * ax;
                    const float hl = std::sqrt(wv.x * wv.x + wv.z * wv.z);
                    if (hl > 1e-3f) {
                        home = {wv.x / hl, 0.0f, wv.z / hl};
                        break;
                    }
                }
                if (home.x != 0.0f || home.z != 0.0f) {
                    // 目标累计偏航 = 移动方向水平角 − 模型前向固有朝向角
                    const float globalFrontYaw = std::atan2(home.x, home.z);
                    float target = std::atan2(wx, wz) - globalFrontYaw;
                    // 累计偏航之差收进 [-π, π]，按 TurnSpeed 限速逼近（不瞬转）
                    float diff = target - cc->FacingYaw;
                    while (diff > kPi) diff -= 2.0f * kPi;
                    while (diff < -kPi) diff += 2.0f * kPi;
                    const float maxStep = JPH::DegreesToRadians(cc->TurnSpeed) * dt;
                    cc->FacingYaw += std::clamp(diff, -maxStep, maxStep);
                    cv->SetRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(), cc->FacingYaw)
                                    * ToJoltQuat(cc->BaseRotation));
                }
            }
        }
    }
}

void PhysicsWorld::SyncCharacterTransformsToComponents() {
    if (m_Characters.empty() || !m_Scene)
        return;

    auto &reg = m_Scene->Reg();
    for (auto &[entity, cv] : m_Characters) {
        auto *tc = reg.try_get<TransformComponent>(entity);
        if (!tc || !cv)
            continue;
        // 渲染读 TransformComponent，取最末子步位置（与 SyncBodiesToTransforms 同风格）
        tc->Translation = ToGlmVec3(cv->GetPosition());
        tc->Rotation = ToGlmQuat(cv->GetRotation());
    }
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
        // 阶段 B：写入实体句柄，Step 返回后经 userdata 反向查实体（碰撞事件/查询接口都靠它）
        bodySettings.mUserData = static_cast<JPH::uint64>(static_cast<entt::id_type>(entity));

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
    return reg.any_of<BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent>(entity);
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

    // 收集 CapsuleCollider 组件（胶囊默认沿局部 Y；可选 X/Z，半高缩放跟随所选轴）
    auto *capsuleColliders = reg.try_get<CapsuleColliderComponent>(entity);
    if (capsuleColliders) {
        // 半高缩放：跟随胶囊主轴对应的轴分量；半径取三轴最大值（与球体一致）
        float heightScale = tc->Scale.y;
        if (capsuleColliders->Axis == CapsuleAxis::X)
            heightScale = tc->Scale.x;
        else if (capsuleColliders->Axis == CapsuleAxis::Z)
            heightScale = tc->Scale.z;
        float scaledHalfHeight = capsuleColliders->HalfHeight * heightScale;
        float scaledRadius = capsuleColliders->Radius * std::max({tc->Scale.x, tc->Scale.y, tc->Scale.z});

        // 半高归零时 CapsuleShape 直接构造会触发 JPH_ASSERT(> 0)，退化为球体（与 Jolt Settings::IsSphere 语义一致）
        JPH::ShapeRefC capsuleShape;
        if (scaledHalfHeight > 0.0f) {
            capsuleShape = new JPH::CapsuleShape(scaledHalfHeight, scaledRadius);
        } else {
            capsuleShape = new JPH::SphereShape(scaledRadius);
        }

        // 轴向烘焙：Jolt 胶囊原生沿局部 Y，选 X/Z 时把形状绕对应轴转过去
        // （X: 绕局部 Z -90° 使 Y→X；Z: 绕局部 X +90° 使 Y→Z。glm::quat(w,x,y,z)）
        glm::quat axisRot(1.0f, 0.0f, 0.0f, 0.0f);
        constexpr float kSqrtHalf = 0.707106781f;
        switch (capsuleColliders->Axis) {
        case CapsuleAxis::X: axisRot = glm::quat(kSqrtHalf, 0.0f, 0.0f, -kSqrtHalf); break;
        case CapsuleAxis::Z: axisRot = glm::quat(kSqrtHalf, kSqrtHalf, 0.0f, 0.0f); break;
        default: break; // Y 轴无需旋转
        }

        // 仅在带偏移或非默认轴向时才用 RotatedTranslatedShape 烘焙（零偏移零旋转时直接用裸形状）
        if (capsuleColliders->Offset != glm::vec3(0.0f) || capsuleColliders->Axis != CapsuleAxis::Y) {
            JPH::RotatedTranslatedShapeSettings offsetSettings(
                ToJoltVec3(capsuleColliders->Offset),
                ToJoltQuat(axisRot),
                capsuleShape
                );
            auto result = offsetSettings.Create();
            if (result.IsValid()) {
                subShapes.push_back(result.Get());
            }
        } else {
            subShapes.push_back(capsuleShape);
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

// ============================================================
// 碰撞事件（阶段 B）：把环形缓冲翻译成实体级事件并供场景分发
// ============================================================

void PhysicsWorld::SetStayEnabled(bool on) {
    if (m_ContactBuffer)
        m_ContactBuffer->SetStayEnabled(on);
}

const std::vector<CollisionEvent> &PhysicsWorld::TakeCollisionEvents() const {
    return m_CollisionEvents;
}

void PhysicsWorld::CollectCollisionEvents() {
    m_CollisionEvents.clear();
    if (!m_ContactBuffer || !m_PhysicsSystem)
        return;
    const uint32_t n = m_ContactBuffer->Count();
    if (n == 0)
        return;

    // 主线程在 Update 返回后读 body，此时无并发模拟但不持有任何 body 锁：
    // 用 NoLock 接口做裸读（不取 PerBody 锁，避开 Jolt 锁层级断言——PhysicsLock 要求
    // 本线程掩码 < PerBody 才能取 BodyLockRead；Update 内部已对全体 body 加过锁，返回前释放）。
    // BodyLockRead::Succeeded() 仍借 TryGetBody 做"body 是否有效"校验。
    auto &lockInterface = m_PhysicsSystem->GetBodyLockInterfaceNoLock();
    std::vector<std::pair<entt::entity, entt::entity>> staySeen; // Stay 按实体对去重
    for (uint32_t i = 0; i < n; ++i) {
        const RawContactEvent &raw = (*m_ContactBuffer)[i];
        JPH::BodyLockRead lockA(lockInterface, raw.body1);
        JPH::BodyLockRead lockB(lockInterface, raw.body2);
        if (!lockA.Succeeded() || !lockB.Succeeded())
            continue; // body 不存在/本帧已销毁 → 事件无效，跳过

        const auto ud1 = lockA.GetBody().GetUserData();
        const auto ud2 = lockB.GetBody().GetUserData();
        const entt::entity a = static_cast<entt::entity>(static_cast<entt::id_type>(ud1));
        const entt::entity b = static_cast<entt::entity>(static_cast<entt::id_type>(ud2));
        if (a == entt::null || b == entt::null)
            continue;

        if (raw.phase == CollisionPhase::Stay) {
            // 同一对每帧最多一条 Stay（复合形状多子形状时 Jolt 会对同对多发 Persisted）
            const auto key = std::make_pair(a, b);
            const auto rev = std::make_pair(b, a);
            if (std::find(staySeen.begin(), staySeen.end(), key) != staySeen.end() ||
                std::find(staySeen.begin(), staySeen.end(), rev) != staySeen.end())
                continue;
            staySeen.push_back(key);
        }

        // 传感器标志：主线程从组件读（回调里读 body 受限；RigidBodyComponent.IsSensor 是权威）
        const auto isSensor = [this](entt::entity e) {
            const auto *rc = (m_Scene ? m_Scene->Reg().try_get<RigidBodyComponent>(e) : nullptr);
            return rc && rc->IsSensor;
        };

        CollisionEvent evt;
        evt.A = a;
        evt.B = b;
        evt.phase = raw.phase;
        evt.isTrigger = isSensor(a) || isSensor(b);
        evt.normal = {raw.nx, raw.ny, raw.nz};
        evt.impulse = raw.impulse;
        m_CollisionEvents.push_back(evt);
    }
}

} // namespace GE::Physics