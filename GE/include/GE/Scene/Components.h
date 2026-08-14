#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>
#include <functional>


#define GLM_ENABLE_EXPERIMENTAL
#include <string>
#include <unordered_map>
#include <glm/gtx/quaternion.hpp>

#include "Render/Camera.h"
#include "Core/KeyCodes.h"
#include "Core/MouseCodes.h"
#include "Physics/PhysicsTypes.h"

namespace GE {

class Texture;  // 前向声明，避免引入整个 Texture 头文件
class Mesh;     // 前向声明，避免引入整个 Mesh 头文件
class Material; // 前向声明，避免引入整个 Material 头文件
class Entity;   // 前向声明，供 ScriptComponent 回调签名使用
class Timestep; // 前向声明，供 ScriptComponent 回调签名使用


struct TagComponent {
    std::string Tag;

    TagComponent() = default;

    TagComponent(const TagComponent &) = default;

    explicit TagComponent(std::string tag) : Tag(std::move(tag)) {
    }
};

struct TransformComponent {
    glm::vec3 Translation = {0.0f, 0.0f, 0.0f};
    glm::vec3 Rotation = {0.0f, 0.0f, 0.0f};
    glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

    TransformComponent() = default;

    TransformComponent(const TransformComponent &) = default;

    explicit TransformComponent(const glm::vec3 &translation)
        : Translation(translation) {
    }

    [[nodiscard]] glm::mat4 GetTransform() const {

        const glm::mat4 rotation = glm::toMat4(glm::quat(Rotation));

        return glm::translate(glm::mat4(1.0f), Translation)
               * rotation
               * glm::scale(glm::mat4(1.0f), Scale);
    }
};


/**
 * @brief 精灵渲染组件 —— 描述一个 2D 精灵的渲染属性。
 *
 * 与 TransformComponent 配合使用：Transform 决定位置/旋转/缩放，
 * SpriteRendererComponent 决定显示什么纹理、什么颜色、是否为 UI。
 *
 * 纹理使用裸指针引用，不拥有资源。纹理资源由外部资源管理器（如 VulkanResourceCache）管理。
 * Color 为 RGBA 分量，白色 (1,1,1,1) 表示原样显示纹理。
 *
 * IsUI 为 true 时：精灵在屏幕空间叠加渲染，无深度测试，永远显示在最上层（适用于 HUD/UI）。
 * IsUI 为 false 时：精灵在 3D 世界空间中渲染，参与深度测试，会被 3D 物体遮挡。
 */
struct SpriteRendererComponent {
    glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 叠加颜色（默认白色，即不染色）
    Texture  *SpriteTexture = nullptr;        ///< 精灵纹理（可选，为 null 时绘制纯色矩形）
    bool      IsUI = false;                   ///< 是否为 UI 精灵（true=屏幕空间无深度，false=世界空间有深度）

    SpriteRendererComponent() = default;

    SpriteRendererComponent(const SpriteRendererComponent &) = default;

    /**
     * @brief 仅指定颜色的构造函数（纯色矩形，无纹理）。
     */
    explicit SpriteRendererComponent(const glm::vec4 &color)
        : Color(color) {
    }

    /**
     * @brief 指定纹理的构造函数（颜色默认白色）。
     */
    explicit SpriteRendererComponent(Texture *texture)
        : SpriteTexture(texture) {
    }

    /**
     * @brief 同时指定纹理和颜色的构造函数。
     */
    SpriteRendererComponent(Texture *texture, const glm::vec4 &color)
        : Color(color), SpriteTexture(texture) {
    }
};


/**
 * @brief 脚本组件 —— 挂载到实体上的行为回调。
 *
 * 轻量级脚本系统：通过 std::function 绑定每帧更新和各类事件回调，
 * 在 Scene::OnUpdate 中调用 OnUpdate，在 Scene::OnEvent 中调用对应事件回调，
 * 用于实现实体的行为逻辑。
 *
 * 按键/鼠标按键类回调返回 bool：true 表示消费该事件，阻止后续脚本接收。
 * 移动/滚动类回调返回 void，一般不消费事件。
 */
struct ScriptComponent {
    using UpdateCallback       = std::function<void(Timestep, Entity)>;
    using KeyCallback          = std::function<bool(Entity, KeyCode, int repeatCount)>;
    using MouseButtonCallback  = std::function<bool(Entity, MouseCode)>;
    using MouseMoveCallback    = std::function<void(Entity, float x, float y)>;
    using MouseScrollCallback  = std::function<void(Entity, float xOffset, float yOffset)>;

    UpdateCallback      OnUpdate;              ///< 每帧更新回调
    KeyCallback         OnKeyPressed;          ///< 按键按下，返回 true = 消费事件
    KeyCallback         OnKeyReleased;         ///< 按键释放，返回 true = 消费事件
    MouseButtonCallback OnMouseButtonPressed;  ///< 鼠标按下，返回 true = 消费事件
    MouseButtonCallback OnMouseButtonReleased; ///< 鼠标释放，返回 true = 消费事件
    MouseMoveCallback   OnMouseMoved;          ///< 鼠标移动
    MouseScrollCallback OnMouseScrolled;       ///< 鼠标滚轮

    bool Enabled = true; ///< 脚本是否启用（false 时跳过所有回调）

    ScriptComponent() = default;

    ScriptComponent(const ScriptComponent &) = default;

    explicit ScriptComponent(UpdateCallback callback)
        : OnUpdate(std::move(callback)) {
    }
};


/**
 * @brief 网格渲染组件 —— 描述一个 3D 网格资源及其材质覆写。
 *
 * 与 TransformComponent 配合使用：Transform 决定位置/旋转/缩放，
 * MeshRendererComponent 决定绘制什么网格。
 * 材质默认来自子网格的 defaultMaterial（随模型加载），实体可通过
 * materialOverrides 按子网格索引覆写材质（每实体独立，可序列化）。
 *
 * Mesh / Material 均使用裸指针引用，不拥有资源。资源由外部资源管理器管理。
 * Color 为 RGBA 分量，白色 (1,1,1,1) 表示原样显示材质颜色。
 *
 * 由 Renderer3D 在 Scene::OnUpdate3D() 中遍历并绘制，
 * 支持深度测试、背面剔除和 Blinn-Phong 光照。
 */
struct MeshRendererComponent {
    glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 叠加颜色（默认白色，即不染色）
    Mesh     *MeshPtr = nullptr;              ///< 网格资源指针（可选，为 null 时不绘制）

    /// 子网格材质覆写表：<子网格索引, 自定义材质>（每实体独立，借用 MaterialManager）
    std::unordered_map<uint32_t, Material *> materialOverrides;

    MeshRendererComponent() = default;

    MeshRendererComponent(const MeshRendererComponent &) = default;

    /**
     * @brief 仅指定颜色的构造函数（纯色网格，无网格资源）。
     */
    explicit MeshRendererComponent(const glm::vec4 &color)
        : Color(color) {
    }

    /**
     * @brief 指定网格的构造函数（颜色默认白色）。
     */
    explicit MeshRendererComponent(Mesh *mesh)
        : MeshPtr(mesh) {
    }

    /**
     * @brief 同时指定网格和颜色的构造函数。
     */
    MeshRendererComponent(Mesh *mesh, const glm::vec4 &color)
        : Color(color), MeshPtr(mesh) {
    }
};


/**
 * @brief 相机组件 —— 挂载到实体上的相机，用于 3D 场景渲染。
 *
 * 包含一个完整的 Camera 实例，支持 Orbit（轨道）和 FPS（第一人称）两种模式。
 * Primary 标志用于标记场景中的主相机，渲染器会使用主相机的视图投影矩阵进行渲染。
 * FixedAspectRatio 控制是否随窗口大小自动调整宽高比。
 *
 * 与 TransformComponent 的关系：
 * - 若实体挂载了 TransformComponent，可在逻辑层将 Transform 的 Translation/Rotation 同步到 Camera
 * - 也可直接使用 Camera 内置的交互（OnEvent、MoveForward 等）独立控制
 */
struct CameraComponent {
    Camera CameraInstance;           ///< 相机实例（包含投影、视图、交互等完整功能）
    bool   Primary = true;           ///< 是否为主相机（场景中第一个主相机会被渲染器使用）
    bool   FixedAspectRatio = false; ///< 是否固定宽高比（false 时随窗口大小自动调整）

    CameraComponent() = default;

    CameraComponent(const CameraComponent &) = default;

    /**
     * @brief 指定是否为主相机的构造函数。
     */
    explicit CameraComponent(bool primary)
        : Primary(primary) {
    }

    /**
     * @brief 直接使用 Camera 实例构造。
     */
    explicit CameraComponent(const Camera &camera, bool primary = true)
        : CameraInstance(camera), Primary(primary) {
    }

    /**
     * @brief 获取视图投影矩阵（便捷方法）。
     */
    [[nodiscard]] glm::mat4 GetViewProj() const {
        return CameraInstance.GetViewProj();
    }
};


/**
 * @brief 点光源组件 —— 挂载到实体上的点光源。
 *
 * 与 TransformComponent 配合使用：Transform 的 Translation 即为点光源的世界坐标位置。
 * Color 的 rgb 分量表示光源颜色，a 分量表示光源强度。
 * RadiusInv 是光源影响半径的倒数，用于距离衰减计算：attenuation = 1 / (1 + d^2 * radiusInv^2)。
 *
 * 由 Scene 在渲染前收集所有点光源组件，传递给 Renderer3D 的 LightParams。
 * 点光源数量无编译期上限（存入 SSBO 动态数组，按实际数量上传）。
 */
struct PointLightComponent {
    glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 光源颜色(rgb) + 强度(a)
    float     RadiusInv = 0.5f;               ///< 光源影响半径倒数（衰减系数，越大衰减越快）

    PointLightComponent() = default;

    PointLightComponent(const PointLightComponent &) = default;

    /**
     * @brief 指定颜色和强度的构造函数。
     */
    explicit PointLightComponent(const glm::vec4 &color)
        : Color(color) {
    }

    /**
     * @brief 同时指定颜色、强度和半径倒数的构造函数。
     */
    PointLightComponent(const glm::vec4 &color, float radiusInv)
        : Color(color), RadiusInv(radiusInv) {
    }
};


/**
 * @brief 方向光组件 —— 挂载到实体上的方向光。
 *
 * 与 TransformComponent 配合使用：Transform 的 Rotation 决定方向光的照射方向。
 * 光线方向取 Transform 前向向量（即 -Z 轴经过旋转后的方向），从光源指向被照物体。
 * Color 的 rgb 分量表示光源颜色，a 分量表示光源强度。
 *
 * 由 Scene 在渲染前收集方向光组件，取场景中第一个方向光作为主方向光，
 * 传递给 Renderer3D 的 LightParams。场景中存在多个方向光时，仅第一个生效。
 */
struct DirectionalLightComponent {
    glm::vec4 Color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 光源颜色(rgb) + 强度(a)

    DirectionalLightComponent() = default;

    DirectionalLightComponent(const DirectionalLightComponent &) = default;

    /**
     * @brief 指定颜色和强度的构造函数。
     */
    explicit DirectionalLightComponent(const glm::vec4 &color)
        : Color(color) {
    }
};


/**
 * @brief 环境光组件 —— 场景全局环境光。
 *
 * 环境光不依赖实体的 Transform，是场景级别的全局光照。
 * Color 的 rgb 分量表示环境光颜色，a 分量表示环境光强度系数。
 *
 * 由 Scene 在渲染前收集环境光组件，取场景中第一个环境光作为全局环境光，
 * 传递给 Renderer3D 的 LightParams。若场景中无环境光组件，使用默认值。
 */
struct AmbientLightComponent {
    glm::vec4 Color{0.3f, 0.3f, 0.3f, 1.0f}; ///< 环境光颜色(rgb) + 强度(a)

    AmbientLightComponent() = default;

    AmbientLightComponent(const AmbientLightComponent &) = default;

    /**
     * @brief 指定颜色和强度的构造函数。
     */
    explicit AmbientLightComponent(const glm::vec4 &color)
        : Color(color) {
    }
};


// ============================================================
// 物理相关组件
// ============================================================

/**
 * @brief 刚体组件 —— 描述实体的物理运动属性。
 *
 * 与 TransformComponent 配合使用：创建时从 Transform 初始化位置和旋转。
 * 动态体每帧由物理模拟更新 TransformComponent，
 * 运动学体由用户修改 Transform，通过 PhysicsWorld 同步到物理世界。
 *
 * RuntimeBodyID 和 IsInitialized 是运行时数据，不参与序列化。
 */
struct RigidBodyComponent {
    Physics::RigidBodyType Type = Physics::RigidBodyType::Static; ///< 刚体类型
    float Mass = 1.0f;        ///< 质量（kg，静态体/运动学体忽略）
    float Friction = 0.6f;    ///< 摩擦系数（0~1）
    float Restitution = 0.0f; ///< 弹性系数（0~1）
    float LinearDamping = 0.05f;  ///< 线性阻尼
    float AngularDamping = 0.05f; ///< 角阻尼
    bool  IsSensor = false;   ///< 是否为触发器（不产生物理响应，只触发事件）

    // 运行时数据（不参与序列化）
    Physics::BodyID RuntimeBodyID{}; ///< Jolt Body 句柄（由 PhysicsWorld 设置）
    bool IsInitialized = false;      ///< 是否已加入物理世界

    RigidBodyComponent() = default;
    RigidBodyComponent(const RigidBodyComponent &) = default;
    explicit RigidBodyComponent(Physics::RigidBodyType type) : Type(type) {}
};

/**
 * @brief 盒子碰撞体组件。
 *
 * HalfExtents 是半尺寸，即从中心到各面的距离。
 * 一个 2x2x2 的立方体对应 HalfExtents = (1,1,1)。
 * Offset 是碰撞体相对于刚体中心的偏移。
 *
 * 实体上可挂载多个碰撞体组件，PhysicsWorld 会合并为复合形状。
 */
struct BoxColliderComponent {
    glm::vec3 HalfExtents = {0.5f, 0.5f, 0.5f}; ///< 半尺寸
    glm::vec3 Offset = {0.0f, 0.0f, 0.0f};      ///< 相对于刚体中心的偏移

    BoxColliderComponent() = default;
    BoxColliderComponent(const BoxColliderComponent &) = default;
    explicit BoxColliderComponent(const glm::vec3 &halfExtents) : HalfExtents(halfExtents) {}
};

/**
 * @brief 球体碰撞体组件。
 *
 * Radius 为球体半径，Offset 为碰撞体相对于刚体中心的偏移。
 * 实体上可挂载多个碰撞体组件，PhysicsWorld 会合并为复合形状。
 */
struct SphereColliderComponent {
    float Radius = 0.5f;                       ///< 半径
    glm::vec3 Offset = {0.0f, 0.0f, 0.0f};     ///< 偏移

    SphereColliderComponent() = default;
    SphereColliderComponent(const SphereColliderComponent &) = default;
    explicit SphereColliderComponent(float radius) : Radius(radius) {}
};

}